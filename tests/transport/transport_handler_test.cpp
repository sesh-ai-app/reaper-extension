// The Transport Handler.
//
// Four things are worth testing here, and they are worth testing in roughly this
// order of consequence.
//
// The action IDs have to be right. A wrong one is not a crash or a failed call — it
// is a producer tapping Play on their phone and REAPER doing something else in a
// session they have been working in all afternoon. So the seven are pinned to the
// literal IDs REAPER publishes, and checked to be distinct, because a duplicate is
// how a copy-paste in the mapping would show up.
//
// An unrecognised command has to execute nothing. This is the same class of harm
// from the other direction: the Envelope Codec already refuses a payload that fails
// transport-command.schema.json, so reaching the handler with an unknown command
// means a protocol mismatch — and the wrong response to a protocol mismatch is to
// pick the nearest action.
//
// The acknowledgement has to carry the correlation identifier and nothing else,
// because that is the whole of response-transport.schema.json and the extension
// validates its own outbound payloads against it (requirement 4.1).
//
// And the sequence has to hold: nothing invoked before the command is recognised,
// nothing acknowledged before REAPER reported the action ran. An acknowledgement is
// the only thing the phone gets back, so one arriving for a command that did nothing
// is worse than none arriving at all.
//
// No REAPER here. The invoker is substituted, which is what the seam in
// transport_handler.h exists for.

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/queue_pair.h>
#include <transport/transport_handler.h>

using sesh_ai::transport::TransportActionInvoker;
using sesh_ai::transport::TransportCommand;
using sesh_ai::transport::TransportHandler;
using sesh_ai::transport::TransportRequestOutcome;
using sesh_ai::transport::all_transport_commands;
using sesh_ai::transport::reaper_action_command_id;
using sesh_ai::transport::to_schema_string;
using sesh_ai::transport::transport_command_from_schema_string;
using sesh_ai::transport::transport_request_envelope_type;
using sesh_ai::transport::transport_response_envelope_type;

namespace {

	// Stands in for REAPER's action dispatch: records every action ID it was asked to
	// run, in order, and lets a case make the call fail the way an unreachable
	// registration table would.
	class RecordingTransportActionInvoker final : public TransportActionInvoker {
	public:
		bool invoke_main_action(int reaper_action_command_id) override
		{
			invoked_action_command_ids.push_back(reaper_action_command_id);

			return !refuse_invocation;
		}

		bool refuse_invocation = false;
		std::vector<int> invoked_action_command_ids;
	};

	// Stands in for transport/envelope.h's OutboundEnvelope.
	//
	// The real one carries an nlohmann::json payload, and nlohmann/json is an optional
	// dependency of this suite — so the handler is templated on the envelope type and
	// this is what the suite instantiates it with. A std::map supports the one payload
	// operation the handler performs, and the explicit instantiation in
	// transport_handler.cpp is what proves the real envelope does too.
	struct StubOutboundEnvelope {
		std::string type;
		std::string request_id;
		std::map<std::string, std::string> payload;
	};

	using StubOutboundQueue = sesh_ai::transport::ConcurrentQueue<StubOutboundEnvelope>;
	using StubTransportHandler = TransportHandler<StubOutboundEnvelope>;

	// Stands in for the nlohmann::json payload of an inbound envelope, for the same
	// dependency reason as StubOutboundEnvelope above.
	//
	// It provides only what reading the `command` field out of a payload uses — is it
	// an object, does it have the field, is that field a string, and its value — so
	// what the cases below exercise is which branch the handler takes, not a
	// reimplementation of a JSON library. That the real nlohmann::json satisfies the
	// same calls is settled by the explicit instantiation in transport_handler.cpp,
	// which is compiled wherever the dependency is available.
	class StubPayload {
	public:
		static StubPayload object_value(std::map<std::string, StubPayload> fields)
		{
			StubPayload payload;
			payload.kind_ = Kind::object_value;
			payload.fields_ = std::move(fields);

			return payload;
		}

		static StubPayload string_value(std::string value)
		{
			StubPayload payload;
			payload.kind_ = Kind::string_value;
			payload.value_ = std::move(value);

			return payload;
		}

		// A `command` that is present but not a string — what a client sending the
		// enum's index rather than its name would produce.
		static StubPayload number_value()
		{
			StubPayload payload;
			payload.kind_ = Kind::number_value;

			return payload;
		}

		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_string() const { return kind_ == Kind::string_value; }

		bool contains(const std::string& field_name) const
		{
			return fields_.find(field_name) != fields_.end();
		}

		const StubPayload& at(const std::string& field_name) const { return fields_.at(field_name); }

		template <typename ValueType>
		ValueType get() const
		{
			return value_;
		}

	private:
		enum class Kind { object_value, string_value, number_value };

		Kind kind_ = Kind::object_value;
		std::string value_;
		std::map<std::string, StubPayload> fields_;
	};

	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubPayload payload;
	};

	StubInboundEnvelope transport_request(StubPayload payload, std::string request_id)
	{
		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{transport_request_envelope_type};
		inbound_envelope.request_id = std::move(request_id);
		inbound_envelope.payload = std::move(payload);

		return inbound_envelope;
	}

	// The seven command IDs as REAPER publishes them in the Main section of its own
	// action list, paired with REAPER's own name for each.
	//
	// Written out again here rather than read from the header, on purpose. Asserting
	// reaper_action_command_id(play) == reaper_action_transport_play would pass for any
	// value the header happens to hold, which is exactly the mistake worth catching.
	struct ExpectedActionMapping {
		TransportCommand command;
		std::string_view schema_command_name;
		int reaper_action_command_id;
		std::string_view reaper_action_name;
	};

	constexpr std::array<ExpectedActionMapping, 7> expected_action_mappings{
		ExpectedActionMapping{TransportCommand::play, "play", 1007, "Transport: Play"},
		ExpectedActionMapping{TransportCommand::pause, "pause", 1008, "Transport: Pause"},
		ExpectedActionMapping{TransportCommand::stop, "stop", 1016, "Transport: Stop"},
		ExpectedActionMapping{
			TransportCommand::return_to_zero, "return_to_zero", 40042, "Transport: Go to start of project"
		},
		ExpectedActionMapping{
			TransportCommand::previous_marker,
			"previous_marker",
			40172,
			"Markers: Go to previous marker/project start"
		},
		ExpectedActionMapping{
			TransportCommand::next_marker, "next_marker", 40173, "Markers: Go to next marker/project end"
		},
		ExpectedActionMapping{TransportCommand::loop_toggle, "loop_toggle", 1068, "Transport: Toggle repeat"}
	};

}

TEST_CASE("each of the seven commands maps to the REAPER action it names", "[transport]")
{
	// Requirement 14.1. The IDs are REAPER's native ones, which Cockos guarantees are
	// stable across versions and installations — so pinning them as literals is the
	// check, not an approximation of one.
	REQUIRE(expected_action_mappings.size() == all_transport_commands.size());

	for (const auto& expected : expected_action_mappings) {
		REQUIRE(to_schema_string(expected.command) == expected.schema_command_name);
		REQUIRE(reaper_action_command_id(expected.command) == expected.reaper_action_command_id);
	}
}

TEST_CASE("the seven action IDs are distinct and none of them is a no-op", "[transport]")
{
	// A duplicate is how a copy-paste in the mapping presents: two transport buttons
	// doing the same thing, with nothing to see at the call site. Zero is REAPER's "no
	// command", so a command mapped to it would report success having done nothing.
	std::vector<int> action_command_ids;

	for (const TransportCommand command : all_transport_commands) {
		const int action_command_id = reaper_action_command_id(command);

		REQUIRE(action_command_id > 0);

		action_command_ids.push_back(action_command_id);
	}

	std::sort(action_command_ids.begin(), action_command_ids.end());

	REQUIRE(std::adjacent_find(action_command_ids.begin(), action_command_ids.end())
		== action_command_ids.end());
	REQUIRE(action_command_ids.size() == 7);
}

TEST_CASE("the command vocabulary round-trips through the schema spelling", "[transport]")
{
	// transport-command.schema.json's `command` enum is closed, so a spelling drift
	// here is a command the PWA can send and the extension will not recognise.
	for (const TransportCommand command : all_transport_commands) {
		const std::optional<TransportCommand> parsed =
			transport_command_from_schema_string(to_schema_string(command));

		REQUIRE(parsed.has_value());
		REQUIRE(*parsed == command);
	}

	SECTION("and nothing outside it parses")
	{
		// Including the near misses: REAPER's own vocabulary for two of these differs
		// from the schema's, and camelCase is what the rest of the protocol uses for
		// field names.
		REQUIRE_FALSE(transport_command_from_schema_string("").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("PLAY").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("returnToZero").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("toggle_repeat").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("go_to_start_of_project").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("record").has_value());
		REQUIRE_FALSE(transport_command_from_schema_string("play ").has_value());
	}
}

TEST_CASE("the envelope types are the ones the protocol names", "[transport]")
{
	// The Message Dispatcher routes on the request type and the Message Router
	// forwards the response type, and both are matched as strings on the server.
	REQUIRE(transport_request_envelope_type == "request:transport");
	REQUIRE(transport_response_envelope_type == "response:transport");
}

TEST_CASE("every command executes its action and is acknowledged", "[transport]")
{
	// Requirement 14.1 end to end, one command at a time so a failure names the
	// command rather than the loop.
	for (const auto& expected : expected_action_mappings) {
		RecordingTransportActionInvoker action_invoker;
		StubOutboundQueue outbound_queue;
		StubTransportHandler handler{action_invoker, outbound_queue};

		const std::string request_id = std::string{"request-for-"} + std::string{expected.schema_command_name};

		const TransportRequestOutcome outcome =
			handler.handle_transport_command(expected.schema_command_name, request_id);

		REQUIRE(outcome.command_recognised);
		REQUIRE(outcome.command.has_value());
		REQUIRE(*outcome.command == expected.command);
		REQUIRE(outcome.reaper_action_command_id == expected.reaper_action_command_id);
		REQUIRE(outcome.action_invoked);
		REQUIRE(outcome.acknowledgement_queued);
		REQUIRE(outcome.command_name == expected.schema_command_name);

		// Exactly one action, and the right one.
		REQUIRE(action_invoker.invoked_action_command_ids
			== std::vector<int>{expected.reaper_action_command_id});

		// Exactly one acknowledgement, on the outbound queue.
		REQUIRE(outbound_queue.size() == 1);

		const std::optional<StubOutboundEnvelope> acknowledgement = outbound_queue.try_pop();

		REQUIRE(acknowledgement.has_value());
		REQUIRE(acknowledgement->type == "response:transport");
		REQUIRE(acknowledgement->request_id == request_id);

		// The payload is the correlation identifier and nothing else — the whole of
		// response-transport.schema.json, whose `additionalProperties` is false. No
		// playState: REAPER's transport actions do not report one at the moment they
		// are invoked, and the resulting state reaches both clients through the
		// project context snapshot instead.
		REQUIRE(acknowledgement->payload.size() == 1);
		REQUIRE(acknowledgement->payload.at("requestId") == request_id);
	}
}

TEST_CASE("the seven commands reach seven different actions through the handler", "[transport]")
{
	// The mapping is checked above; this checks that the handler uses it rather than
	// collapsing two commands onto one action somewhere between the parse and the
	// invoke.
	RecordingTransportActionInvoker action_invoker;
	StubOutboundQueue outbound_queue;
	StubTransportHandler handler{action_invoker, outbound_queue};

	for (const TransportCommand command : all_transport_commands) {
		handler.handle_transport_command(to_schema_string(command), "correlation-identifier");
	}

	REQUIRE(action_invoker.invoked_action_command_ids.size() == all_transport_commands.size());

	std::vector<int> invoked_in_order = action_invoker.invoked_action_command_ids;
	std::sort(invoked_in_order.begin(), invoked_in_order.end());

	REQUIRE(std::adjacent_find(invoked_in_order.begin(), invoked_in_order.end()) == invoked_in_order.end());
	REQUIRE(outbound_queue.size() == all_transport_commands.size());
}

TEST_CASE("an unrecognised command executes nothing and acknowledges nothing", "[transport]")
{
	// The one that matters. The Envelope Codec refuses a payload failing
	// transport-command.schema.json before it reaches a handler (requirement 4.3), so
	// arriving here means the server's protocol is ahead of this build or the codec was
	// bypassed. Requirement 5.8's answer to that is to report and ignore — and the
	// wrong answer is to guess an action, because every candidate guess moves a
	// producer's transport on the strength of a typo.
	const std::vector<std::string> unrecognised_commands{
		"",
		"record",
		"Play",
		"play;stop",
		"returnToZero",
		"toggle_repeat",
		"next_region",
		"1007"
	};

	for (const std::string& unrecognised_command : unrecognised_commands) {
		RecordingTransportActionInvoker action_invoker;
		StubOutboundQueue outbound_queue;
		StubTransportHandler handler{action_invoker, outbound_queue};

		const TransportRequestOutcome outcome =
			handler.handle_transport_command(unrecognised_command, "correlation-identifier");

		REQUIRE_FALSE(outcome.command_recognised);
		REQUIRE_FALSE(outcome.command.has_value());
		REQUIRE_FALSE(outcome.action_invoked);
		REQUIRE_FALSE(outcome.acknowledgement_queued);

		// No action at all, not merely a harmless one — the invoker was never called.
		REQUIRE(action_invoker.invoked_action_command_ids.empty());
		REQUIRE(outcome.reaper_action_command_id == 0);
		REQUIRE(outbound_queue.empty());

		// And the command that was refused is reported, so the log line says which.
		REQUIRE(outcome.command_name == unrecognised_command);
	}
}

TEST_CASE("a command REAPER could not run is not acknowledged", "[transport][errors]")
{
	// The acknowledgement is the only thing the phone hears back, so it has to mean
	// the action ran. An unreachable registration table — the extension loaded but the
	// API not resolved — is the case that produces this.
	RecordingTransportActionInvoker action_invoker;
	action_invoker.refuse_invocation = true;

	StubOutboundQueue outbound_queue;
	StubTransportHandler handler{action_invoker, outbound_queue};

	const TransportRequestOutcome outcome =
		handler.handle_transport_command("play", "correlation-identifier");

	REQUIRE(outcome.command_recognised);
	REQUIRE(outcome.reaper_action_command_id == 1007);
	REQUIRE_FALSE(outcome.action_invoked);
	REQUIRE_FALSE(outcome.acknowledgement_queued);
	REQUIRE(outbound_queue.empty());

	// It was attempted, which is what distinguishes this from an unrecognised command.
	REQUIRE(action_invoker.invoked_action_command_ids == std::vector<int>{1007});
}

TEST_CASE("a request with no correlation identifier still moves the transport", "[transport][errors]")
{
	// response-transport.schema.json requires a requestId of at least one character,
	// so there is no acknowledgement to send — building one would produce a payload
	// the extension's own outbound validation refuses (requirement 4.1), which is
	// where that failure belongs rather than at the server.
	//
	// The action runs anyway. A producer pressing play wants the transport to move
	// whether or not the reply is addressable, and a missing identifier is the
	// server's problem rather than theirs.
	RecordingTransportActionInvoker action_invoker;
	StubOutboundQueue outbound_queue;
	StubTransportHandler handler{action_invoker, outbound_queue};

	const TransportRequestOutcome outcome = handler.handle_transport_command("stop", "");

	REQUIRE(outcome.command_recognised);
	REQUIRE(outcome.action_invoked);
	REQUIRE(action_invoker.invoked_action_command_ids == std::vector<int>{1016});

	REQUIRE_FALSE(outcome.acknowledgement_queued);
	REQUIRE(outbound_queue.empty());
}

TEST_CASE("repeated commands queue one acknowledgement each, in order", "[transport]")
{
	// A producer tapping a transport button several times is ordinary, and the phone
	// correlates each tap by its own requestId. So the acknowledgements have to be one
	// per request and in the order the requests arrived — which is the queue's
	// contract, exercised through the handler.
	RecordingTransportActionInvoker action_invoker;
	StubOutboundQueue outbound_queue;
	StubTransportHandler handler{action_invoker, outbound_queue};

	const std::vector<std::string> request_ids{"first-tap", "second-tap", "third-tap"};

	for (const std::string& request_id : request_ids) {
		REQUIRE(handler.handle_transport_command("loop_toggle", request_id).acknowledgement_queued);
	}

	REQUIRE(action_invoker.invoked_action_command_ids == std::vector<int>{1068, 1068, 1068});

	const std::vector<StubOutboundEnvelope> acknowledgements = outbound_queue.drain_up_to(16);

	REQUIRE(acknowledgements.size() == request_ids.size());

	for (std::size_t acknowledgement_index = 0; acknowledgement_index < acknowledgements.size();
		++acknowledgement_index) {
		REQUIRE(acknowledgements[acknowledgement_index].request_id == request_ids[acknowledgement_index]);
		REQUIRE(acknowledgements[acknowledgement_index].payload.at("requestId")
			== request_ids[acknowledgement_index]);
	}
}

TEST_CASE("the envelope entry point reads the command out of the payload", "[transport]")
{
	// This is what the Message Dispatcher (task 14.1) calls: hand it the
	// request:transport envelope it routed here and the handler does the rest.
	RecordingTransportActionInvoker action_invoker;
	StubOutboundQueue outbound_queue;
	StubTransportHandler handler{action_invoker, outbound_queue};

	const StubInboundEnvelope inbound_envelope = transport_request(
		StubPayload::object_value({{"command", StubPayload::string_value("next_marker")}}),
		"correlation-identifier"
	);

	const TransportRequestOutcome outcome = handler.handle(inbound_envelope);

	REQUIRE(outcome.command_recognised);
	REQUIRE(outcome.command_name == "next_marker");
	REQUIRE(outcome.action_invoked);
	REQUIRE(outcome.acknowledgement_queued);
	REQUIRE(action_invoker.invoked_action_command_ids == std::vector<int>{40173});

	const std::optional<StubOutboundEnvelope> acknowledgement = outbound_queue.try_pop();

	REQUIRE(acknowledgement.has_value());
	REQUIRE(acknowledgement->type == "response:transport");
	REQUIRE(acknowledgement->request_id == "correlation-identifier");
	REQUIRE(acknowledgement->payload.at("requestId") == "correlation-identifier");
}

TEST_CASE("a payload with no usable command executes nothing", "[transport][errors]")
{
	// transport-command.schema.json requires `command` and constrains it to a string,
	// so neither of these should reach a handler. Reading the field defensively anyway
	// is what keeps a malformed payload from throwing out of the main-thread tick —
	// both cases take the same path as an unrecognised command.
	SECTION("the field is absent")
	{
		RecordingTransportActionInvoker action_invoker;
		StubOutboundQueue outbound_queue;
		StubTransportHandler handler{action_invoker, outbound_queue};

		const TransportRequestOutcome outcome = handler.handle(
			transport_request(StubPayload::object_value({}), "correlation-identifier")
		);

		REQUIRE_FALSE(outcome.command_recognised);
		REQUIRE_FALSE(outcome.action_invoked);
		REQUIRE(action_invoker.invoked_action_command_ids.empty());
		REQUIRE(outbound_queue.empty());
	}

	SECTION("the field is not a string")
	{
		RecordingTransportActionInvoker action_invoker;
		StubOutboundQueue outbound_queue;
		StubTransportHandler handler{action_invoker, outbound_queue};

		const TransportRequestOutcome outcome = handler.handle(transport_request(
			StubPayload::object_value({{"command", StubPayload::number_value()}}),
			"correlation-identifier"
		));

		REQUIRE_FALSE(outcome.command_recognised);
		REQUIRE_FALSE(outcome.action_invoked);
		REQUIRE(action_invoker.invoked_action_command_ids.empty());
		REQUIRE(outbound_queue.empty());
	}

	SECTION("the payload is not an object at all")
	{
		RecordingTransportActionInvoker action_invoker;
		StubOutboundQueue outbound_queue;
		StubTransportHandler handler{action_invoker, outbound_queue};

		const TransportRequestOutcome outcome = handler.handle(
			transport_request(StubPayload::string_value("play"), "correlation-identifier")
		);

		REQUIRE_FALSE(outcome.command_recognised);
		REQUIRE_FALSE(outcome.action_invoked);
		REQUIRE(action_invoker.invoked_action_command_ids.empty());
		REQUIRE(outbound_queue.empty());
	}
}
