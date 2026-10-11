// The Message Dispatcher.
//
// Five things are worth testing here, in roughly this order of consequence.
//
// **`request:project_context` must not dispatch as a tool.** This is the single
// mistake the component's shape makes available: `request:` is a prefix route whose
// tail is the tool name, and two `request:` types are not tool calls. Match the prefix
// before the exact bindings and a producer asking what they are working with gets an
// unknown-tool error, the Context Builder is never reached, and nothing in the system
// says anything is wrong. So the collision is driven both at the classifier and through
// the whole dispatcher, and the assertion on the second is that the executor was not
// called at all.
//
// **The three unknowns stay three.** A payload whose shape is unknown is refused with
// an error (requirements 4.4, 23.4), an envelope type that is unknown is logged and
// ignored (requirements 5.8, 23.5), and a tool name that is unknown gets an error
// response naming it (requirement 23.6). Three requirements, three different answers,
// and collapsing any two of them is a plausible reading of any one of them. The middle
// one is the counter-intuitive case and is checked hardest: nothing called, nothing
// answered, not a failure.
//
// **The executor's schema answer is carried, not re-derived.** A framework partial
// outcome validates against the partial contract while a `set_item_properties` success
// validates against that tool's own output schema, and from a payload plus a tool name
// the two are indistinguishable — the tool's own schema also carries `actions`. So the
// case puts the two side by side and checks that what reaches the sink is the executor's
// answer and *not* what `EnvelopeCodec::outbound_schema_path_for` would have said.
//
// **Every response echoes the identifier it was asked with, and nothing is remembered.**
// Requirement 5.9 in both halves. The echo is checked per response; "no pending-request
// table" is checked by answering a call that carries no identifier immediately after one
// that did, and asserting the second response is not addressed to the first.
//
// **Each of the seven destinations gets its own envelopes and nobody else's.** One pass
// over a table, asserting the call counts across all seven recorders rather than the one
// under discussion — a route that reaches the right place *and* a second place is still
// wrong.
//
// No REAPER, no CEF, no JSON library. All seven destinations are substituted, which is
// what the seams in message_dispatcher.h exist for.

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/envelope_codec.h>
#include <transport/message_dispatcher.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

using sesh_ai::transport::ConcurrentQueue;
using sesh_ai::transport::ConfirmationRouter;
using sesh_ai::transport::EnvelopeCodec;
using sesh_ai::transport::EnvelopeRoute;
using sesh_ai::transport::MessageDispatcher;
using sesh_ai::transport::PayloadSchemaValidator;
using sesh_ai::transport::ProjectContextRouter;
using sesh_ai::transport::RoutingOutcome;
using sesh_ai::transport::SchemaValidationOutcome;
using sesh_ai::transport::ScriptDownloadRouter;
using sesh_ai::transport::StreamRouter;
using sesh_ai::transport::ToolCallResponse;
using sesh_ai::transport::ToolCallRouter;
using sesh_ai::transport::ToolOutputSchemaBinding;
using sesh_ai::transport::ToolResultResponse;
using sesh_ai::transport::ToolResultSink;
using sesh_ai::transport::TransportCommandRouter;
using sesh_ai::transport::confirmation_approve_envelope_type;
using sesh_ai::transport::confirmation_reject_envelope_type;
using sesh_ai::transport::confirmation_request_envelope_type;
using sesh_ai::transport::confirmation_resolved_envelope_type;
using sesh_ai::transport::describe;
using sesh_ai::transport::invalid_payload_error_code;
using sesh_ai::transport::route_for_envelope_type;
using sesh_ai::transport::script_download_envelope_type;
using sesh_ai::transport::tool_name_from_routed_envelope_type;
using sesh_ai::transport::tool_output_schema_bindings;
using sesh_ai::transport::tool_result_partial_contract_schema_path;
using sesh_ai::transport::tool_result_refusal_schema_path;
using sesh_ai::transport::transport_request_envelope_type;
using sesh_ai::transport::transport_response_envelope_type;
using sesh_ai::transport::validation_error_envelope_type;
using sesh_ai::context::project_context_request_envelope_type;
using sesh_ai::ui::stream_error_envelope_type;

namespace {

	// Stands in for nlohmann::json, in both roles this file needs one: the tool result
	// payload the dispatcher carries, and the document the Envelope Codec parses in the
	// one case that drives the codec.
	//
	// It provides only the operations those two perform on a document — is it an object,
	// does it have a property, that property's value, is it a string, and what string —
	// so what the cases exercise is which branch is taken rather than a reimplementation
	// of a JSON library. parse() resolves text through a table each case registers,
	// because what the cases need to control is whether a message parsed, not how.
	class StubJson {
	public:
		StubJson() = default;

		static StubJson object() { return object_with({}); }

		static StubJson object_with(std::map<std::string, StubJson> properties)
		{
			StubJson document;
			document.kind_ = Kind::object_value;
			document.properties_ = std::move(properties);

			return document;
		}

		static StubJson string_value(std::string value)
		{
			StubJson document;
			document.kind_ = Kind::string_value;
			document.value_ = std::move(value);

			return document;
		}

		// The one operation the codec needs that is not a read. The table is static
		// because parse() is, which is what nlohmann::json's own signature forces.
		static void register_message(std::string message_text, StubJson document)
		{
			parsed_messages()[std::move(message_text)] = std::move(document);
		}

		static void forget_registered_messages() { parsed_messages().clear(); }

		template <typename Iterator>
		static StubJson parse(Iterator first, Iterator last, void* /*callback*/, bool /*allow_exceptions*/)
		{
			const std::string message_text{first, last};
			const auto registered = parsed_messages().find(message_text);

			if (registered == parsed_messages().end()) {
				StubJson document;
				document.kind_ = Kind::discarded;

				return document;
			}

			return registered->second;
		}

		bool is_discarded() const { return kind_ == Kind::discarded; }
		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_string() const { return kind_ == Kind::string_value; }

		bool contains(const std::string& property_name) const
		{
			return properties_.find(property_name) != properties_.end();
		}

		const StubJson& at(const std::string& property_name) const
		{
			return properties_.at(property_name);
		}

		StubJson& operator[](const std::string& property_name)
		{
			kind_ = Kind::object_value;

			return properties_[property_name];
		}

		template <typename ValueType>
		ValueType get() const
		{
			return value_;
		}

		std::string dump() const { return value_; }

		// Not part of either consumer's vocabulary — the cases use it to say which
		// payload a recorder was handed.
		const std::string& label() const { return value_; }

	private:
		enum class Kind { null_value, discarded, object_value, string_value };

		static std::map<std::string, StubJson>& parsed_messages()
		{
			static std::map<std::string, StubJson> messages;

			return messages;
		}

		Kind kind_ = Kind::null_value;
		std::string value_;
		std::map<std::string, StubJson> properties_;
	};

	// Stands in for transport/envelope.h's InboundEnvelope. The real one carries an
	// nlohmann::json payload and that dependency is optional in this suite, which is why
	// the dispatcher is templated on the envelope type at all.
	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubJson payload;
	};

	// Stands in for OutboundEnvelope, for the one case that drives the Envelope Codec.
	// A std::map supports the only payload operation the codec's error path performs.
	struct StubOutboundEnvelope {
		std::string type;
		std::string request_id;
		std::map<std::string, std::string> payload;
	};

	StubInboundEnvelope inbound(std::string type, std::string request_id = "req-1")
	{
		StubInboundEnvelope envelope;

		envelope.type = std::move(type);
		envelope.request_id = std::move(request_id);
		envelope.payload = StubJson::object();

		return envelope;
	}

	// ------------------------------------------------------------------
	// The seven destinations, as recorders
	// ------------------------------------------------------------------

	class RecordingToolCallRouter final : public ToolCallRouter<StubInboundEnvelope, StubJson> {
	public:
		ToolCallResponse<StubJson> execute_tool_call(
			std::string_view tool_name,
			const StubInboundEnvelope& inbound_envelope
		) override
		{
			requested_tool_names.emplace_back(tool_name);
			answered_request_ids.push_back(inbound_envelope.request_id);

			ToolCallResponse<StubJson> response;

			response.payload = StubJson::string_value(next_payload_label);
			response.result_schema_path = next_result_schema_path;
			response.tool_is_known = next_tool_is_known;

			return response;
		}

		std::size_t call_count() const { return requested_tool_names.size(); }

		std::string next_payload_label{"a tool result"};
		std::string next_result_schema_path;
		bool next_tool_is_known = true;

		std::vector<std::string> requested_tool_names;
		std::vector<std::string> answered_request_ids;
	};

	class RecordingToolResultSink final : public ToolResultSink<StubJson> {
	public:
		bool send_tool_result(const ToolResultResponse<StubJson>& response) override
		{
			envelope_types.push_back(response.type());
			request_ids.push_back(response.request_id());
			payload_labels.push_back(response.payload().label());
			result_schema_paths.push_back(response.result_schema_path());

			return !refuse_send;
		}

		std::size_t call_count() const { return envelope_types.size(); }

		bool refuse_send = false;

		std::vector<std::string> envelope_types;
		std::vector<std::string> request_ids;
		std::vector<std::string> payload_labels;
		std::vector<std::string> result_schema_paths;
	};

	class RecordingConfirmationRouter final : public ConfirmationRouter<StubInboundEnvelope> {
	public:
		void handle_confirmation_request(const StubInboundEnvelope& inbound_envelope) override
		{
			presented_request_ids.push_back(inbound_envelope.request_id);
		}

		void handle_confirmation_resolved(const StubInboundEnvelope& inbound_envelope) override
		{
			resolved_request_ids.push_back(inbound_envelope.request_id);
		}

		std::size_t call_count() const
		{
			return presented_request_ids.size() + resolved_request_ids.size();
		}

		std::vector<std::string> presented_request_ids;
		std::vector<std::string> resolved_request_ids;
	};

	class RecordingProjectContextRouter final : public ProjectContextRouter {
	public:
		void answer_project_context_request(std::string_view request_id) override
		{
			answered_request_ids.emplace_back(request_id);
		}

		std::size_t call_count() const { return answered_request_ids.size(); }

		std::vector<std::string> answered_request_ids;
	};

	class RecordingTransportCommandRouter final : public TransportCommandRouter<StubInboundEnvelope> {
	public:
		void execute_transport_command(const StubInboundEnvelope& inbound_envelope) override
		{
			handled_request_ids.push_back(inbound_envelope.request_id);
		}

		std::size_t call_count() const { return handled_request_ids.size(); }

		std::vector<std::string> handled_request_ids;
	};

	class RecordingStreamRouter final : public StreamRouter<StubInboundEnvelope> {
	public:
		void present_stream_envelope(const StubInboundEnvelope& inbound_envelope) override
		{
			presented_envelope_types.push_back(inbound_envelope.type);
		}

		std::size_t call_count() const { return presented_envelope_types.size(); }

		std::vector<std::string> presented_envelope_types;
	};

	class RecordingScriptDownloadRouter final : public ScriptDownloadRouter<StubInboundEnvelope> {
	public:
		void present_script_download(const StubInboundEnvelope& inbound_envelope) override
		{
			presented_request_ids.push_back(inbound_envelope.request_id);
		}

		std::size_t call_count() const { return presented_request_ids.size(); }

		std::vector<std::string> presented_request_ids;
	};

	// The seven wired up, so a case says what it is about rather than repeating the
	// constructor.
	struct DispatcherHarness {
		RecordingToolCallRouter tool_calls;
		RecordingToolResultSink tool_results;
		RecordingConfirmationRouter confirmations;
		RecordingProjectContextRouter project_context;
		RecordingTransportCommandRouter transport_commands;
		RecordingStreamRouter streams;
		RecordingScriptDownloadRouter script_downloads;

		MessageDispatcher<StubInboundEnvelope, StubJson> dispatcher{
			tool_calls,
			tool_results,
			confirmations,
			project_context,
			transport_commands,
			streams,
			script_downloads
		};

		// Every destination's call count, so a case can assert that exactly one of them
		// moved. A route that reaches the right place and also a second place is a bug
		// no single-recorder assertion catches.
		std::size_t total_destination_calls() const
		{
			return tool_calls.call_count()
				+ confirmations.call_count()
				+ project_context.call_count()
				+ transport_commands.call_count()
				+ streams.call_count()
				+ script_downloads.call_count();
		}
	};

	// Refuses whichever schema path it is told to, and records every path it was asked
	// about. Used only by the case that drives the Envelope Codec.
	class ScriptedPayloadSchemaValidator final : public PayloadSchemaValidator<StubJson> {
	public:
		SchemaValidationOutcome validate_against(
			std::string_view schema_path,
			const StubJson& /*payload*/
		) override
		{
			consulted_schema_paths.emplace_back(schema_path);

			SchemaValidationOutcome outcome;
			outcome.schema_path.assign(schema_path);
			outcome.valid = schema_path != schema_path_to_refuse;

			if (!outcome.valid) {
				outcome.errors.push_back("required property 'command' not found");
			}

			return outcome;
		}

		std::string schema_path_to_refuse;
		std::vector<std::string> consulted_schema_paths;
	};

}

// ---------------------------------------------------------------------------
// The collision
// ---------------------------------------------------------------------------

TEST_CASE("the two request types that are not tool calls reach their own handlers", "[dispatch]")
{
	// The prefix-before-exact mistake, at the classifier. `request:project_context`
	// matches the `request:` prefix, and if that is tested first its tail is read as a
	// tool named `project_context`.
	CHECK(route_for_envelope_type(project_context_request_envelope_type) == EnvelopeRoute::context_builder);
	CHECK(route_for_envelope_type(transport_request_envelope_type) == EnvelopeRoute::transport_handler);

	// And neither yields a tool name, because neither is a tool call.
	CHECK(tool_name_from_routed_envelope_type(project_context_request_envelope_type).empty());
	CHECK(tool_name_from_routed_envelope_type(transport_request_envelope_type).empty());
}

TEST_CASE("a project context request never reaches the Tool Executor", "[dispatch]")
{
	DispatcherHarness harness;

	const RoutingOutcome outcome =
		harness.dispatcher.route(inbound(std::string{project_context_request_envelope_type}, "ctx-7"));

	CHECK(outcome.route == EnvelopeRoute::context_builder);
	CHECK(outcome.routed);
	CHECK(outcome.tool_name.empty());

	// The echo is the whole of what this request carries (requirements 5.3, 6.9).
	REQUIRE(harness.project_context.answered_request_ids.size() == 1);
	CHECK(harness.project_context.answered_request_ids.front() == "ctx-7");

	// The assertion the collision is actually about.
	CHECK(harness.tool_calls.call_count() == 0);
	CHECK(harness.tool_results.call_count() == 0);
	CHECK(harness.total_destination_calls() == 1);
}

TEST_CASE("a transport request never reaches the Tool Executor", "[dispatch]")
{
	DispatcherHarness harness;

	const RoutingOutcome outcome =
		harness.dispatcher.route(inbound(std::string{transport_request_envelope_type}, "tr-3"));

	CHECK(outcome.route == EnvelopeRoute::transport_handler);
	REQUIRE(harness.transport_commands.handled_request_ids.size() == 1);
	CHECK(harness.transport_commands.handled_request_ids.front() == "tr-3");
	CHECK(harness.tool_calls.call_count() == 0);
	CHECK(harness.total_destination_calls() == 1);
}

// ---------------------------------------------------------------------------
// The routing table
// ---------------------------------------------------------------------------

TEST_CASE("every envelope type requirement 5 names reaches the component it names", "[dispatch]")
{
	struct RouteExpectation {
		std::string_view envelope_type;
		EnvelopeRoute route;
	};

	const std::array<RouteExpectation, 11> expectations{{
		{"request:create_track", EnvelopeRoute::tool_executor},
		{confirmation_request_envelope_type, EnvelopeRoute::confirmation_request},
		{project_context_request_envelope_type, EnvelopeRoute::context_builder},
		{transport_request_envelope_type, EnvelopeRoute::transport_handler},
		{stream_error_envelope_type, EnvelopeRoute::stream_presenter},
		{"stream:agent_response_delta", EnvelopeRoute::stream_presenter},
		{validation_error_envelope_type, EnvelopeRoute::stream_presenter},
		{"error:tool_relay_timeout", EnvelopeRoute::stream_presenter},
		{confirmation_resolved_envelope_type, EnvelopeRoute::confirmation_resolution},
		{script_download_envelope_type, EnvelopeRoute::script_download},
		{"state:something_this_build_has_never_heard_of", EnvelopeRoute::unknown}
	}};

	for (const RouteExpectation& expectation : expectations) {
		CHECK(route_for_envelope_type(expectation.envelope_type) == expectation.route);
	}
}

TEST_CASE("each destination receives its own envelopes and nobody else's", "[dispatch]")
{
	DispatcherHarness harness;

	harness.dispatcher.route(inbound("request:create_track"));
	harness.dispatcher.route(inbound(std::string{confirmation_request_envelope_type}));
	harness.dispatcher.route(inbound(std::string{confirmation_resolved_envelope_type}));
	harness.dispatcher.route(inbound(std::string{project_context_request_envelope_type}));
	harness.dispatcher.route(inbound(std::string{transport_request_envelope_type}));
	harness.dispatcher.route(inbound(std::string{stream_error_envelope_type}));
	harness.dispatcher.route(inbound(std::string{script_download_envelope_type}));

	CHECK(harness.tool_calls.call_count() == 1);
	CHECK(harness.confirmations.presented_request_ids.size() == 1);
	CHECK(harness.confirmations.resolved_request_ids.size() == 1);
	CHECK(harness.project_context.call_count() == 1);
	CHECK(harness.transport_commands.call_count() == 1);
	CHECK(harness.streams.call_count() == 1);
	CHECK(harness.script_downloads.call_count() == 1);

	// One destination call per envelope and not one more. The Confirmation Coordinator
	// accounts for two of the seven, through its two distinct operations.
	CHECK(harness.total_destination_calls() == 7);
}

TEST_CASE("all 42 constrained tools route to the Tool Executor under their own name", "[dispatch]")
{
	DispatcherHarness harness;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		const std::string envelope_type = "request:" + std::string{binding.tool_name};

		CHECK(route_for_envelope_type(envelope_type) == EnvelopeRoute::tool_executor);
		CHECK(tool_name_from_routed_envelope_type(envelope_type) == binding.tool_name);

		const RoutingOutcome outcome = harness.dispatcher.route(inbound(envelope_type));

		CHECK(outcome.tool_name == binding.tool_name);
		CHECK(outcome.response_sent);
	}

	REQUIRE(harness.tool_calls.requested_tool_names.size() == tool_output_schema_bindings.size());

	for (std::size_t index = 0; index < tool_output_schema_bindings.size(); ++index) {
		CHECK(harness.tool_calls.requested_tool_names[index] == tool_output_schema_bindings[index].tool_name);
	}
}

TEST_CASE("the stream and error namespaces both reach the Stream Presenter", "[dispatch]")
{
	DispatcherHarness harness;

	harness.dispatcher.route(inbound("stream:agent_response_start"));
	harness.dispatcher.route(inbound("stream:a_type_this_build_does_not_assemble"));
	harness.dispatcher.route(inbound("error:tool_relay_failed"));

	// A `stream:` type the presenter does not recognise is still routed. Which of its
	// five types it assembles is the presenter's business — the alternative is the
	// dispatcher holding a second copy of that list and ignoring whatever drifted out
	// of it.
	REQUIRE(harness.streams.presented_envelope_types.size() == 3);
	CHECK(harness.streams.presented_envelope_types[1] == "stream:a_type_this_build_does_not_assemble");
	CHECK(harness.total_destination_calls() == 3);
}

TEST_CASE("an outbound-only envelope type is not mistaken for a route", "[dispatch]")
{
	// `response:` looks like `request:` and is not: the prefix route is `request:`, so
	// an echoed acknowledgement does not dispatch as a tool named `transport`.
	CHECK(route_for_envelope_type(transport_response_envelope_type) == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type("response:create_track") == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type(confirmation_approve_envelope_type) == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type(confirmation_reject_envelope_type) == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type("state:prompt") == EnvelopeRoute::unknown);

	// A bare prefix names nothing to dispatch on. envelope.schema.json's type pattern
	// requires a character after the colon, so this cannot arrive through the codec —
	// it is here because the classifier is a free function anybody can call.
	CHECK(route_for_envelope_type("request:") == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type("stream:") == EnvelopeRoute::unknown);
	CHECK(route_for_envelope_type("") == EnvelopeRoute::unknown);
}

// ---------------------------------------------------------------------------
// Three unknowns, three answers
// ---------------------------------------------------------------------------

TEST_CASE("an unknown envelope type is ignored, and that is not an error", "[dispatch][errors]")
{
	DispatcherHarness harness;

	const RoutingOutcome outcome =
		harness.dispatcher.route(inbound("state:a_feature_the_server_shipped_first", "ahead-1"));

	CHECK(outcome.route == EnvelopeRoute::unknown);
	CHECK(outcome.ignored());
	CHECK_FALSE(outcome.routed);

	// The log line requirement 5.8 asks for: the outcome names what it could not place,
	// and says in words that there was nowhere to put it.
	CHECK(outcome.envelope_type == "state:a_feature_the_server_shipped_first");
	CHECK(outcome.request_id == "ahead-1");
	CHECK(describe(outcome.route) == "no route in this build");

	// Nothing called, and — the part that distinguishes this from the other two
	// unknowns — nothing answered. The server's protocol being ahead of this build is
	// the expected cause, and an error response would be the extension objecting to a
	// version it has no opinion to offer about.
	CHECK(harness.total_destination_calls() == 0);
	CHECK(harness.tool_results.call_count() == 0);
	CHECK_FALSE(outcome.response_sent);
	CHECK_FALSE(outcome.tool_was_unknown);
}

TEST_CASE("an unknown tool name is answered with an error naming the tool", "[dispatch][errors]")
{
	DispatcherHarness harness;

	harness.tool_calls.next_tool_is_known = false;
	harness.tool_calls.next_result_schema_path.clear();
	harness.tool_calls.next_payload_label = "unknown_tool error";

	const RoutingOutcome outcome =
		harness.dispatcher.route(inbound("request:a_tool_this_build_does_not_implement", "req-9"));

	// Routed, unlike an unknown envelope type. The Tool Executor holds the registry, so
	// it is the thing that gets to say the tool does not exist — routing on the 42-name
	// table instead would send this down the ignore path and requirement 23.6's error
	// would never be produced.
	CHECK(outcome.route == EnvelopeRoute::tool_executor);
	CHECK(outcome.routed);
	CHECK(outcome.tool_name == "a_tool_this_build_does_not_implement");
	CHECK(outcome.tool_was_unknown);
	REQUIRE(harness.tool_calls.requested_tool_names.size() == 1);
	CHECK(harness.tool_calls.requested_tool_names.front() == "a_tool_this_build_does_not_implement");

	// Answered, unlike an unknown envelope type, and the envelope type names the tool so
	// a missing implementation is visible rather than silent.
	REQUIRE(harness.tool_results.call_count() == 1);
	CHECK(outcome.response_sent);
	CHECK(harness.tool_results.envelope_types.front() == "response:a_tool_this_build_does_not_implement");
	CHECK(harness.tool_results.request_ids.front() == "req-9");

	// And no schema, because there is no output schema for a tool that does not exist.
	// A derived path would turn requirement 23.6's error into a validator failing to
	// find a file.
	CHECK(harness.tool_results.result_schema_paths.front().empty());
	CHECK(outcome.response_schema_path.empty());
}

TEST_CASE("a payload that fails its schema never reaches a destination", "[dispatch][errors]")
{
	// The third unknown, and the one that is not the dispatcher's to answer: the codec
	// refuses it on the network thread and replies with an error, so the envelope never
	// reaches the inbound queue the dispatcher is downstream of (requirements 4.3, 23.4).
	//
	// Driven through the codec rather than asserted about, because the claim is about
	// the two components' relationship and not about either one alone.
	StubJson::forget_registered_messages();

	StubJson::register_message(
		"a transport command with no command",
		StubJson::object_with({
			{"type", StubJson::string_value(std::string{transport_request_envelope_type})},
			{"payload", StubJson::object()},
			{"requestId", StubJson::string_value("req-bad")}
		})
	);

	ScriptedPayloadSchemaValidator validator;
	validator.schema_path_to_refuse = "messages/transport-command.schema.json";

	EnvelopeCodec<StubJson> codec{validator};

	ConcurrentQueue<StubInboundEnvelope> inbound_queue;
	ConcurrentQueue<StubOutboundEnvelope> outbound_queue;

	codec.accept_inbound_message("a transport command with no command", inbound_queue, outbound_queue);

	// Nothing to route. The wrapper schema passed, the payload schema did not.
	CHECK(inbound_queue.empty());

	DispatcherHarness harness;

	while (const std::optional<StubInboundEnvelope> envelope = inbound_queue.try_pop()) {
		harness.dispatcher.route(*envelope);
	}

	CHECK(harness.total_destination_calls() == 0);
	CHECK(harness.tool_results.call_count() == 0);

	// An error response, which is the half that distinguishes this from an unknown
	// envelope type.
	const std::optional<StubOutboundEnvelope> refusal = outbound_queue.try_pop();

	REQUIRE(refusal.has_value());
	CHECK(refusal->type == validation_error_envelope_type);
	CHECK(refusal->request_id == "req-bad");
	CHECK(refusal->payload.at("code") == invalid_payload_error_code);

	StubJson::forget_registered_messages();
}

// ---------------------------------------------------------------------------
// Requirement 5.9: the echo, and the table that is not kept
// ---------------------------------------------------------------------------

TEST_CASE("a tool result is addressed to the call it answers", "[dispatch]")
{
	DispatcherHarness harness;

	harness.tool_calls.next_result_schema_path = "mcp-tools/outputs/create-track.schema.json";

	const RoutingOutcome outcome = harness.dispatcher.route(inbound("request:create_track", "uuid-4-abc"));

	REQUIRE(harness.tool_results.call_count() == 1);
	CHECK(harness.tool_results.request_ids.front() == "uuid-4-abc");
	CHECK(harness.tool_results.envelope_types.front() == "response:create_track");
	CHECK(outcome.request_id == "uuid-4-abc");

	// The executor was given the identifier too, so a handler's log line can name the
	// call it belongs to.
	REQUIRE(harness.tool_calls.answered_request_ids.size() == 1);
	CHECK(harness.tool_calls.answered_request_ids.front() == "uuid-4-abc");
}

TEST_CASE("nothing is remembered between calls, so no identifier is reused", "[dispatch]")
{
	DispatcherHarness harness;

	harness.dispatcher.route(inbound("request:create_track", "first"));
	harness.dispatcher.route(inbound("request:delete_track", "second"));

	// A call carrying no identifier, immediately after two that did. A pending-request
	// table — or any held copy of the last one — shows up right here as the previous
	// identifier leaking onto this response.
	harness.dispatcher.route(inbound("request:list_tracks", ""));

	REQUIRE(harness.tool_results.request_ids.size() == 3);
	CHECK(harness.tool_results.request_ids[0] == "first");
	CHECK(harness.tool_results.request_ids[1] == "second");
	CHECK(harness.tool_results.request_ids[2].empty());

	CHECK(harness.tool_results.envelope_types[0] == "response:create_track");
	CHECK(harness.tool_results.envelope_types[1] == "response:delete_track");
	CHECK(harness.tool_results.envelope_types[2] == "response:list_tracks");
}

TEST_CASE("a result the sink refused is reported as not sent", "[dispatch][errors]")
{
	DispatcherHarness harness;

	harness.tool_results.refuse_send = true;

	const RoutingOutcome outcome = harness.dispatcher.route(inbound("request:create_track"));

	// Routed and executed — the tool ran, which is why this is not the same thing as an
	// envelope that was never dispatched. Only the reply failed, and requirement 4.1 has
	// that failing locally with a diagnosable error rather than at the server.
	CHECK(outcome.routed);
	CHECK(harness.tool_calls.call_count() == 1);
	CHECK(harness.tool_results.call_count() == 1);
	CHECK_FALSE(outcome.response_sent);
}

// ---------------------------------------------------------------------------
// The carried schema answer
// ---------------------------------------------------------------------------

TEST_CASE("the result schema the executor named is the one that is carried", "[dispatch]")
{
	// The forward risk, in the case where re-deriving gives a different and wrong
	// answer. A framework partial outcome for `set_item_properties` validates against
	// the partial contract; that tool's *own* output schema also carries `actions`, so a
	// derivation from the payload cannot tell the two apart and picks the tool's own —
	// which then refuses the payload on `items`, a field the framework has nowhere to
	// put. The producer sees the tool silently not working.
	DispatcherHarness harness;

	harness.tool_calls.next_result_schema_path = std::string{tool_result_partial_contract_schema_path};

	harness.dispatcher.route(inbound("request:set_item_properties", "partial-1"));

	REQUIRE(harness.tool_results.result_schema_paths.size() == 1);
	CHECK(harness.tool_results.result_schema_paths.front() == tool_result_partial_contract_schema_path);

	// What the payload-plus-name derivation would have said instead. Not an assertion
	// about the codec being wrong — it answers correctly for what it can see — but the
	// pin that stops the dispatcher from quietly adopting that answer.
	const std::optional<std::string_view> derived_schema_path =
		EnvelopeCodec<StubJson>::outbound_schema_path_for(
			harness.tool_results.envelope_types.front(),
			StubJson::object()
		);

	REQUIRE(derived_schema_path.has_value());
	CHECK(*derived_schema_path == "mcp-tools/outputs/set-item-properties.schema.json");
	CHECK(harness.tool_results.result_schema_paths.front() != *derived_schema_path);
}

TEST_CASE("each result shape carries the schema that describes it", "[dispatch]")
{
	struct ShapeExpectation {
		std::string_view envelope_type;
		std::string_view result_schema_path;
	};

	// The three shapes `tool_executor.h` tabulates, plus requirement 23.6's no-shape.
	// What is being checked is that the dispatcher carries whatever it was handed
	// without consulting the tool name — a refusal for `create_track` and a refusal for
	// `render` name the same contract, and neither names a tool output schema.
	const std::array<ShapeExpectation, 4> expectations{{
		{"request:create_track", "mcp-tools/outputs/create-track.schema.json"},
		{"request:create_track", tool_result_refusal_schema_path},
		{"request:change_tempo_map", tool_result_partial_contract_schema_path},
		{"request:a_tool_this_build_does_not_implement", ""}
	}};

	DispatcherHarness harness;

	for (const ShapeExpectation& expectation : expectations) {
		harness.tool_calls.next_result_schema_path.assign(expectation.result_schema_path);
		harness.dispatcher.route(inbound(std::string{expectation.envelope_type}));
	}

	REQUIRE(harness.tool_results.result_schema_paths.size() == expectations.size());

	for (std::size_t index = 0; index < expectations.size(); ++index) {
		CHECK(harness.tool_results.result_schema_paths[index] == expectations[index].result_schema_path);
	}
}

TEST_CASE("the payload reaches the sink untouched", "[dispatch]")
{
	// The dispatcher reads nothing out of a result and writes nothing into one. It is
	// the handler's payload and the framework's shape; this component's only
	// contribution is the envelope around it.
	DispatcherHarness harness;

	harness.tool_calls.next_payload_label = "the handler's own fields";

	harness.dispatcher.route(inbound("request:list_tracks"));

	REQUIRE(harness.tool_results.payload_labels.size() == 1);
	CHECK(harness.tool_results.payload_labels.front() == "the handler's own fields");
}

// ---------------------------------------------------------------------------
// The ReaScript download
// ---------------------------------------------------------------------------

TEST_CASE("a script download is presented and never executed", "[dispatch]")
{
	// ADR 0011 and requirement 26.2: the producer reads and runs the script themselves.
	// There is no operation on the seam that would run it, which is the enforcement —
	// what this case checks is that the envelope reaches the presenting seam and no
	// other, in particular not the Tool Executor, which is the one destination here that
	// does act on a producer's session.
	DispatcherHarness harness;

	const RoutingOutcome outcome =
		harness.dispatcher.route(inbound(std::string{script_download_envelope_type}, "script-2"));

	CHECK(outcome.route == EnvelopeRoute::script_download);
	CHECK(outcome.routed);
	REQUIRE(harness.script_downloads.presented_request_ids.size() == 1);
	CHECK(harness.script_downloads.presented_request_ids.front() == "script-2");
	CHECK(harness.tool_calls.call_count() == 0);
	CHECK(harness.total_destination_calls() == 1);
}
