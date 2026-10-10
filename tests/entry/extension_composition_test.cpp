// The composition root.
//
// Every component in this extension is tested against a recorder, which is what makes
// the suite possible at all and also what leaves one thing untested: whether the real
// objects reach each other. That is this file's subject, and it is not a formality —
// `plugin_entry.cpp` has been shipping a default-constructed `EnvelopeRouter` since
// task 17.1, which `MainThreadDispatcher::route_inbound_envelopes` reads as "nothing
// routes yet" and answers by leaving the inbound queue alone.
//
// Five things are worth testing here, in roughly this order of consequence.
//
// **All 42 tools register.** `tool_handler_registry` rejects a name that is not one of
// the 42, rejects a second handler for one tool, and rejects an empty handler — so a
// composition that is wrong in any of those ways produces a registry with a hole in it
// and requirement 23.6's unknown-tool error for a tool that exists. The report names
// the hole, and the case asserts the report is empty.
//
// **The Tool Executor's schema answer survives both handoffs.** It is read once by the
// router and copied onto `OutboundEnvelope::payload_schema_path_override` by the sink.
// Nothing downstream can re-derive it: a framework `tool_partial_outcome` and the named
// tool's own success are indistinguishable from a payload plus a tool name, and routing
// a partial to the tool's own output schema refuses it on `items` the framework cannot
// supply. So the case puts a refusal and a success side by side and reads the override
// off the queued envelope.
//
// **An envelope routed through `bind_envelope_router` reaches its destination.** That
// is requirement 2.4's slot, filled, and it is the one assertion that covers the gap
// `plugin_entry.cpp`'s header comment described. Each of the seven destinations is
// driven, and the counters on all seven adapters are read on every pass — a route that
// reaches the right place *and* a second place is still wrong.
//
// **Requirement 5.8 still holds through the real graph.** An envelope type this build
// has no route for is logged and ignored: nothing called, nothing answered, not a
// failure. It is the counter-intuitive one of the three unknowns and the one a
// composition is most likely to break, because the obvious way to wire a router is to
// make everything reachable.
//
// **The bridge's inbound half answers locally where it should.** A confirmation
// decision goes to the Confirmation Coordinator rather than straight out, because the
// coordinator holds the pending table and refuses an answer for a prompt that resolved
// or expired while the producer read it.
//
// No REAPER, no CEF, no JSON library. Nine host stubs, one stub document, and a
// recording browser — which is the whole point of the seams every component was built
// against.

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/extension_composition.h>
#include <entry/extension_log.h>
#include <transport/queue_pair.h>

#include "../context/project_context_test_doubles.h"
#include "composition_test_doubles.h"

using sesh_ai::entry::ExtensionLog;
using sesh_ai::entry::LogSeverity;
using sesh_ai::entry::ToolHostReferences;
using sesh_ai::entry::ToolRegistryCompositionReport;
using sesh_ai::entry::bind_envelope_router;
using sesh_ai::transport::tool_result_partial_contract_schema_path;
using sesh_ai::transport::tool_result_refusal_schema_path;

using sesh_ai_tests::ComposedGraph;
using sesh_ai_tests::CompositionInboundEnvelope;
using sesh_ai_tests::StubJson;
using sesh_ai_tests::CompositionOutboundEnvelope;
using sesh_ai_tests::a_confirmation_request;
using sesh_ai_tests::an_envelope;

TEST_CASE("every one of the 42 constrained tools is registered", "[composition][registry]")
{
	ComposedGraph composed;

	const ToolRegistryCompositionReport& report = composed.graph().tool_registry_report();

	SECTION("nothing was refused, and nothing was left out")
	{
		// The two halves are different failures. A refusal is a name this composition
		// offered and the registry would not take — a typo, a duplicate, an empty
		// handler. A name left out is one nothing even offered, which is what an absent
		// codec looks like, because every `register_*_tools` returns early on an
		// incomplete one.
		INFO("refused: " << sesh_ai_tests::describe_failures(report));
		INFO("unregistered: " << sesh_ai_tests::describe_unregistered(report));

		REQUIRE_FALSE(report.anything_was_refused());
		REQUIRE(report.every_constrained_tool_registered());
	}

	SECTION("the count is the 42 the protocol names")
	{
		// Read from `tool_output_schema_bindings` rather than written as 42, so the
		// number cannot drift from the bundle.
		REQUIRE(report.registered_tool_names.size()
			== sesh_ai::transport::tool_output_schema_bindings.size());
	}

	SECTION("a codec with one reader missing leaves exactly its family unregistered")
	{
		// The failure mode this report exists for. `register_marker_region_tools`
		// returns early on an incomplete codec rather than registering a handler that
		// would throw `std::bad_function_call` from inside an undo block — so the
		// symptom is six tools quietly absent, and the only thing that surfaces it at
		// load is this list.
		ComposedGraph without_marker_region_codec{
			sesh_ai_tests::CodecGaps{.marker_region = true}
		};

		const ToolRegistryCompositionReport& gapped =
			without_marker_region_codec.graph().tool_registry_report();

		REQUIRE_FALSE(gapped.every_constrained_tool_registered());

		const std::vector<std::string>& missing = gapped.unregistered_constrained_tool_names;

		REQUIRE(missing.size() == 6);
		REQUIRE(sesh_ai_tests::contains(missing, "create_marker"));
		REQUIRE(sesh_ai_tests::contains(missing, "create_region"));
		REQUIRE(sesh_ai_tests::contains(missing, "update_marker_or_region"));
		REQUIRE(sesh_ai_tests::contains(missing, "delete_marker_or_region"));
		REQUIRE(sesh_ai_tests::contains(missing, "change_tempo_map"));
		REQUIRE(sesh_ai_tests::contains(missing, "set_time_selection"));

		// The two reads over the same host are a different family and are unaffected,
		// which is the distinction that makes the per-family codecs worth having.
		REQUIRE_FALSE(sesh_ai_tests::contains(missing, "list_markers"));
		REQUIRE_FALSE(sesh_ai_tests::contains(missing, "list_regions"));
		REQUIRE_FALSE(sesh_ai_tests::contains(missing, "list_tempo_changes"));
	}
}

TEST_CASE("a tool call routed through the bound router reaches the executor", "[composition][routing]")
{
	ComposedGraph composed;
	ExtensionLog log;
	auto route = bind_envelope_router<CompositionInboundEnvelope>(composed.graph().message_dispatcher(), log);

	SECTION("a known tool is executed and its result queued under the identifier it was asked with")
	{
		route(an_envelope("request:list_tracks", "call-1"));

		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 1);
		REQUIRE(composed.graph().tool_result_sink().queued_result_count() == 1);

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().type == "response:list_tracks");
		REQUIRE(queued.front().request_id == "call-1");
	}

	SECTION("an unknown tool still gets a response, naming the tool")
	{
		// Requirement 23.6. The tool name arrived inside an envelope type that already
		// satisfied the envelope schema's pattern, so the response type satisfies it
		// too and the error is sendable.
		route(an_envelope("request:polish_the_mix", "call-2"));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().type == "response:polish_the_mix");
		REQUIRE(queued.front().request_id == "call-2");

		// Nothing to validate against, which requirement 23.6 makes correct rather than
		// a failure — there is no output schema for a tool that does not exist.
		REQUIRE(queued.front().payload_schema_path_override.empty());

		// And nothing was logged: an unknown *tool* is an error response, not
		// requirement 5.8's ignored envelope type.
		REQUIRE(log.lines_written() == 0);
		REQUIRE(log.lines_dropped() == 0);
	}

	SECTION("the executor's schema answer crosses the queue with the envelope")
	{
		// Task 22.2's field, read through the whole path rather than at the sink. The
		// refusal is the case where the answer cannot be derived from the payload and
		// the tool name: `tool_result_schema_path({}, true)` is the refusal contract
		// whatever declined, and the tool's own output schema would be the wrong answer.
		composed.refuse_every_tool_call();

		route(an_envelope("request:list_tracks", "call-3"));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().payload_schema_path_override == tool_result_refusal_schema_path);
	}

	SECTION("a framework partial is validated against the partial contract, not the tool's schema")
	{
		// The failure the override exists to prevent. `set_item_properties`' own output
		// schema also carries `actions`, so a partial routed to it is refused on
		// `items` — which the framework cannot supply — and the producer sees the tool
		// silently not working.
		//
		// The partial is produced through the real executor rather than substituted:
		// `add_fx`'s handler turns an unreadable input into one failed action, and a
		// handler that returns action outcomes is what the framework reports as a
		// partial.
		route(an_envelope("request:add_fx", "call-4"));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().payload_schema_path_override
			== tool_result_partial_contract_schema_path);
	}

	SECTION("nothing is remembered between calls")
	{
		// Requirement 5.9's second half. A call carrying no identifier immediately
		// after one that did must not be answered under the first one's.
		route(an_envelope("request:list_tracks", "call-5"));
		route(an_envelope("request:list_tracks", ""));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 2);
		REQUIRE(queued[0].request_id == "call-5");
		REQUIRE(queued[1].request_id.empty());
	}
}

TEST_CASE("each of the seven destinations gets its own envelopes", "[composition][routing]")
{
	ComposedGraph composed;
	ExtensionLog log;
	auto route = bind_envelope_router<CompositionInboundEnvelope>(composed.graph().message_dispatcher(), log);

	SECTION("a confirmation request raises a prompt on the panel and nothing else")
	{
		route(a_confirmation_request("confirm-1"));

		REQUIRE(composed.browser_host().count_of("view:confirmation_pending") == 1);
		REQUIRE(composed.graph().confirmation_presenter().tally().published == 1);

		// Nothing queued: a confirmation request is answered by the producer, not by
		// code.
		REQUIRE(composed.drain_outbound().empty());
		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 0);
	}

	SECTION("a confirmation resolution takes the prompt down")
	{
		route(a_confirmation_request("confirm-1"));

		CompositionInboundEnvelope resolved = an_envelope("confirm:resolved", "confirm-1");
		resolved.payload = StubJson::object_with({
			{"requestId", StubJson::string_value("confirm-1")},
			{"decision", StubJson::string_value("expired")}
		});

		route(resolved);

		REQUIRE(composed.browser_host().count_of("view:confirmation_resolved") == 1);
	}

	SECTION("a project context request is answered by the Context Builder, not the executor")
	{
		// The one mistake the dispatcher's shape makes available: `request:` is a
		// prefix route whose tail is the tool name, and `request:project_context` is
		// not a tool call. Matching the prefix first would have the producer's "what am
		// I working with?" fail with a message about a missing implementation.
		route(an_envelope("request:project_context", "context-1"));

		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 0);
		REQUIRE(composed.project_context_source().project_reads == 1);

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().request_id == "context-1");
	}

	SECTION("a transport command runs one REAPER action and acknowledges it")
	{
		CompositionInboundEnvelope transport = an_envelope("request:transport", "transport-1");
		transport.payload = StubJson::object_with({
			{"command", StubJson::string_value("play")}
		});

		route(transport);

		REQUIRE(composed.transport_actions().invoked_command_ids.size() == 1);
		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 0);

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().request_id == "transport-1");
	}

	SECTION("a stream envelope reaches the presenter and publishes on the tick, not on arrival")
	{
		CompositionInboundEnvelope start = an_envelope("stream:agent_response_start", "");
		start.payload = StubJson::object_with({
			{"conversationId", StubJson::string_value("conversation-1")},
			{"turnId", StubJson::string_value("turn-1")},
			{"messageId", StubJson::string_value("message-1")}
		});

		route(start);

		// Requirement 2.3: nothing crossed the bridge yet. A turn's forty deltas are
		// one repaint, which is only true if the presenter accumulates and the tick
		// publishes.
		REQUIRE(composed.browser_host().count_of("view:agent_response") == 0);

		composed.graph().ui_state_publisher()();

		REQUIRE(composed.browser_host().count_of("view:agent_response") == 1);
	}

	SECTION("an error envelope folds into the stream error view")
	{
		// The seven `error:*` types have no bundled payload schema of their own, and
		// requirement 5.5 sends them to the same place as `stream:error`.
		CompositionInboundEnvelope validation_error = an_envelope("error:validation", "call-9");
		validation_error.payload = StubJson::object_with({
			{"code", StubJson::string_value("invalid_payload")},
			{"message", StubJson::string_value("the payload failed its schema")}
		});

		route(validation_error);
		composed.graph().ui_state_publisher()();

		REQUIRE(composed.browser_host().count_of("view:stream_error") == 1);
	}

	SECTION("a script download crosses to JavaScript untouched, and is never executed")
	{
		// ADR 0011 and requirement 26.2. The payload is forwarded verbatim because
		// there is nothing for the C++ side to decide, and there is deliberately no
		// operation on this path that hands anything to REAPER.
		CompositionInboundEnvelope download = an_envelope("script:download", "script-1");
		download.payload = StubJson::object_with({
			{"fileName", StubJson::string_value("sesh_ai_generated.lua")}
		});

		route(download);

		REQUIRE(composed.browser_host().count_of("script:download") == 1);
		REQUIRE(composed.drain_outbound().empty());
		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 0);
	}
}

TEST_CASE("an envelope type this build has no route for is logged and ignored", "[composition][routing]")
{
	// Requirements 5.8 and 23.5, through the real graph. The counter-intuitive one of
	// the three unknowns: the server's protocol may be ahead of this build, and a
	// producer who updated the server first should get the features this build supports
	// and nothing said about the rest.
	ComposedGraph composed;

	std::vector<std::pair<LogSeverity, std::string>> lines;
	ExtensionLog log{[&lines](LogSeverity severity, std::string_view line) {
		lines.emplace_back(severity, std::string{line});
	}};

	auto route = bind_envelope_router<CompositionInboundEnvelope>(composed.graph().message_dispatcher(), log);

	route(an_envelope("state:something_new", "future-1"));

	SECTION("one line, naming the type, phrased as information rather than a fault")
	{
		REQUIRE(lines.size() == 1);
		REQUIRE(lines.front().first == LogSeverity::information);
		REQUIRE(lines.front().second.find("state:something_new") != std::string::npos);
	}

	SECTION("nothing was called and nothing was answered")
	{
		REQUIRE(composed.graph().tool_call_router().executed_call_count() == 0);
		REQUIRE(composed.graph().confirmation_presenter().tally().published == 0);
		REQUIRE(composed.project_context_source().project_reads == 0);
		REQUIRE(composed.transport_actions().invoked_command_ids.empty());
		REQUIRE(composed.browser_host().published.empty());
		REQUIRE(composed.drain_outbound().empty());
	}
}

TEST_CASE("the bridge's inbound half answers where it should", "[composition][bridge]")
{
	ComposedGraph composed;
	ExtensionLog log;
	auto route = bind_envelope_router<CompositionInboundEnvelope>(composed.graph().message_dispatcher(), log);

	SECTION("a prompt is queued for the server exactly as the UI built it")
	{
		// The payload is the UI's, not bare text handed to C++ for framing: the
		// standalone Bedrock Guardrail's `dataPath` resolves to a named field inside
		// it, so reframing it here would put the guarded text somewhere the guardrail
		// does not look.
		REQUIRE(composed.send_from_javascript(
			"state:prompt",
			"{\"promptText\":\"add a compressor to the kick\"}"
		));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().type == "state:prompt");

		// Left empty on purpose: the schema follows from the type here, so the codec
		// derives it. The override is for the one case it cannot.
		REQUIRE(queued.front().payload_schema_path_override.empty());
	}

	SECTION("an approval for a live prompt is sent, and the prompt comes down")
	{
		route(a_confirmation_request("confirm-1"));

		REQUIRE(composed.send_from_javascript(
			"confirm:approve",
			"{\"requestId\":\"confirm-1\",\"decision\":\"approved\"}"
		));

		const std::vector<CompositionOutboundEnvelope> queued = composed.drain_outbound();

		REQUIRE(queued.size() == 1);
		REQUIRE(queued.front().type == "confirm:approve");
		REQUIRE(queued.front().request_id == "confirm-1");
	}

	SECTION("an approval for a prompt that is no longer live is refused, not sent")
	{
		// The producer tapping a prompt the phone answered a moment earlier, or one
		// that expired while they read it. Sending it would have the extension
		// authorising a destructive operation whose window has closed — which is why
		// the decision goes through the coordinator rather than straight out.
		REQUIRE_FALSE(composed.send_from_javascript(
			"confirm:approve",
			"{\"requestId\":\"confirm-never-seen\",\"decision\":\"approved\"}"
		));

		REQUIRE(composed.drain_outbound().empty());
	}

	SECTION("a view model arriving from JavaScript is refused on its declared direction")
	{
		// The bridge pointed the wrong way round. `ui_host.h` declares which direction
		// each of the ten names travels, so this is answered from the contract rather
		// than from a guess about what the name means.
		REQUIRE_FALSE(composed.send_from_javascript("view:connection_state", "{}"));
		REQUIRE(composed.graph().dispatcher_sink().refused_message_count() == 1);
		REQUIRE(composed.drain_outbound().empty());
	}

	SECTION("a name neither side declared is refused")
	{
		REQUIRE_FALSE(composed.send_from_javascript("state:something_the_ui_invented", "{}"));
		REQUIRE(composed.drain_outbound().empty());
	}
}

TEST_CASE("the connection state reaches the panel through the tick", "[composition][bridge]")
{
	// The one path in the graph that crosses a thread boundary. The Transport Client
	// presents from the network thread and requirement 2.3 puts every write to CEF
	// inside the tick, so the presenter holds and the publish step takes.
	ComposedGraph composed;

	sesh_ai::transport::connection_status status;
	status.state = sesh_ai::transport::connection_state::connected;
	status.notice = "Connected to Sesh AI.";

	composed.graph().connection_state_presenter().present_connection_status(status);

	REQUIRE(composed.browser_host().count_of("view:connection_state") == 0);
	REQUIRE(composed.ui_state_publication_requests() == 1);

	composed.graph().ui_state_publisher()();

	REQUIRE(composed.browser_host().count_of("view:connection_state") == 1);
}
