// The Envelope Codec.
//
// Five things are worth testing here, in roughly this order of consequence.
//
// **An invalid inbound payload never reaches a handler.** Requirement 4.3, and the
// reason it matters is what is on the other end of a handler: a producer's project.
// Acting on a shape the extension did not understand is worse than answering an
// error, so the case drives the queue-pushing entry point and asserts on both queues
// — the inbound one empty, the outbound one holding the refusal.
//
// **A tool call is not validated.** Requirement 4.4, checked exhaustively over all 42
// tool names, and checked by what the codec *asked* the validator rather than by
// whether it accepted: the assertion is that the only schema consulted was the
// envelope wrapper. A re-check that happened to pass would still be the bug, because
// the vendored copy is a commit behind the authoritative one the MCP Tool Server
// already used.
//
// **An outbound payload is validated before anything is serialized.** Requirement
// 4.1. A snapshot the Context Builder got the shape of should fail here, with the
// schema path and the failures named, rather than as a server rejection in the middle
// of a turn — and nothing should go out.
//
// **The refusal is answerable.** An error naming nothing is not diagnosable, and an
// error too long to describe cannot be sent at all. So the error envelope is checked
// for the code, the refused type, the rendered failures, and the requestId echo.
//
// **The sequence holds.** The wrapper schema before any field is trusted, the type
// before any payload schema is looked up, the payload schema before the envelope is
// constructed. Order is most of what a codec is.
//
// No validation library here, and no JSON library either. The validator is
// substituted, which is what the seam in schema_validator.h exists for, and the
// document type is substituted, which is what the codec's template parameter exists
// for. The limit of that is worth stating plainly: these cases settle what the codec
// does with a verdict, not whether a real payload earns one. Whether a snapshot
// satisfies project-context.schema.json needs the real validator and is task 4.3's.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/envelope_codec.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

using sesh_ai::transport::ConcurrentQueue;
using sesh_ai::transport::EnvelopeCodec;
using sesh_ai::transport::InboundRefusalReason;
using sesh_ai::transport::OutboundEncodeFailureReason;
using sesh_ai::transport::OutboundEncodeOutcome;
using sesh_ai::transport::PayloadSchemaValidator;
using sesh_ai::transport::SchemaValidationOutcome;
using sesh_ai::transport::ToolOutputSchemaBinding;
using sesh_ai::transport::build_validation_error_envelope;
using sesh_ai::transport::envelope_schema_path;
using sesh_ai::transport::error_code_property;
using sesh_ai::transport::error_envelope_type_property;
using sesh_ai::transport::error_message_property;
using sesh_ai::transport::error_validation_errors_property;
using sesh_ai::transport::invalid_envelope_error_code;
using sesh_ai::transport::invalid_payload_error_code;
using sesh_ai::transport::maximum_error_envelope_type_length;
using sesh_ai::transport::maximum_rendered_validation_errors_length;
using sesh_ai::transport::project_context_schema_path;
using sesh_ai::transport::tool_output_schema_bindings;
using sesh_ai::transport::tool_result_partial_contract_schema_path;
using sesh_ai::transport::tool_result_refusal_schema_path;
using sesh_ai::transport::validation_error_envelope_type;

namespace {

	// Stands in for nlohmann::json.
	//
	// It provides only the operations the codec performs on a document — is it
	// discarded, is it an object, does it have a property, that property's value, build
	// an object, write a property, render it — so what the cases exercise is which
	// branch the codec takes rather than a reimplementation of a JSON library. That the
	// real nlohmann::json satisfies the same calls is settled by the explicit
	// instantiation in envelope_codec.cpp, which is compiled wherever the dependency is
	// available.
	//
	// parse() resolves text through a table each case registers rather than parsing it.
	// Writing a parser here would be testing a parser; what the cases need to control
	// is whether a given message parsed, and an unregistered text standing for
	// unparseable gives exactly that control with nothing invented.
	class StubJson {
	public:
		StubJson() = default;

		static StubJson discarded_document()
		{
			StubJson document;
			document.kind_ = Kind::discarded;

			return document;
		}

		static StubJson object()
		{
			StubJson document;
			document.kind_ = Kind::object_value;

			return document;
		}

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

		// A property that is present but not a string — what a client sending a number
		// where the envelope wants a type would produce.
		static StubJson number_value()
		{
			StubJson document;
			document.kind_ = Kind::number_value;

			return document;
		}

		bool is_discarded() const { return kind_ == Kind::discarded; }
		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_string() const { return kind_ == Kind::string_value; }

		bool contains(const std::string& property_name) const
		{
			return kind_ == Kind::object_value && properties_.count(property_name) > 0;
		}

		const StubJson& at(const std::string& property_name) const
		{
			return properties_.at(property_name);
		}

		template <typename ValueType>
		ValueType get() const
		{
			return value_;
		}

		// Vivifies to an object the way nlohmann::json does, so the codec can write into
		// a default-constructed payload.
		StubJson& operator[](const std::string& property_name)
		{
			if (kind_ == Kind::null_value) {
				kind_ = Kind::object_value;
			}

			return properties_[property_name];
		}

		StubJson& operator=(std::string value)
		{
			kind_ = Kind::string_value;
			value_ = std::move(value);
			properties_.clear();

			return *this;
		}

		// Deterministic, because the properties are held in a sorted map. Enough to
		// assert that a property was written or omitted, which is all the cases need
		// from serialization.
		std::string dump() const
		{
			switch (kind_) {
				case Kind::discarded:
					return "<discarded>";
				case Kind::null_value:
					return "null";
				case Kind::string_value:
					return "\"" + value_ + "\"";
				case Kind::number_value:
					return "0";
				case Kind::object_value:
					break;
			}

			std::string rendered{"{"};

			for (const auto& property : properties_) {
				if (rendered.size() > 1) {
					rendered.append(",");
				}

				rendered.append("\"").append(property.first).append("\":").append(property.second.dump());
			}

			return rendered.append("}");
		}

		// Templated on the iterator type because std::string_view::const_iterator is not
		// guaranteed to be const char*, and the codec hands parse an iterator pair.
		template <typename IteratorType>
		static StubJson parse(IteratorType first, IteratorType last, std::nullptr_t, bool)
		{
			const std::string message_text{first, last};
			const auto registered = parse_table().find(message_text);

			return registered == parse_table().end() ? discarded_document() : registered->second;
		}

		static void register_parse(std::string message_text, StubJson document)
		{
			parse_table()[std::move(message_text)] = std::move(document);
		}

		static void reset_parse_table() { parse_table().clear(); }

	private:
		enum class Kind { null_value, discarded, object_value, string_value, number_value };

		static std::map<std::string, StubJson>& parse_table()
		{
			static std::map<std::string, StubJson> table;

			return table;
		}

		Kind kind_ = Kind::null_value;
		std::string value_;
		std::map<std::string, StubJson> properties_;
	};

	// Stands in for transport/envelope.h's InboundEnvelope and OutboundEnvelope, which
	// carry an nlohmann::json payload.
	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubJson payload;
	};

	struct StubOutboundEnvelope {
		std::string type;
		std::string request_id;
		StubJson payload;

		// Left empty by every case that is not about it, which is how the derivation
		// stays the default under test.
		std::string payload_schema_path_override;
	};

	// Stands in for the real validator: records every schema it was asked about, in
	// order, and lets a case make a named schema fail.
	//
	// Recording the requests is the more important half. "A tool input is never
	// validated" is a claim about what the codec asked, not about whether it accepted —
	// a re-check that happened to pass would still be the bug.
	class ScriptedPayloadSchemaValidator final : public PayloadSchemaValidator<StubJson> {
	public:
		SchemaValidationOutcome validate_against(
			std::string_view schema_path,
			const StubJson& payload
		) override
		{
			requested_schema_paths.emplace_back(schema_path);
			validated_payload_renderings.push_back(payload.dump());

			SchemaValidationOutcome outcome;
			outcome.schema_path.assign(schema_path);

			const auto scripted_failure = scripted_failures.find(std::string{schema_path});

			if (scripted_failure == scripted_failures.end()) {
				outcome.valid = true;

				return outcome;
			}

			outcome.errors = scripted_failure->second;

			return outcome;
		}

		void fail(std::string_view schema_path, std::vector<std::string> errors)
		{
			scripted_failures.emplace(std::string{schema_path}, std::move(errors));
		}

		std::vector<std::string> requested_schema_paths;
		std::vector<std::string> validated_payload_renderings;
		std::map<std::string, std::vector<std::string>> scripted_failures;
	};

	using StubCodec = EnvelopeCodec<StubJson>;
	using StubInboundQueue = ConcurrentQueue<StubInboundEnvelope>;
	using StubOutboundQueue = ConcurrentQueue<StubOutboundEnvelope>;

	// A well-formed envelope document, as the codec will find it after parsing.
	StubJson envelope_document(
		const std::string& envelope_type,
		StubJson payload,
		const std::string& request_id = std::string{}
	)
	{
		std::map<std::string, StubJson> properties;

		properties.emplace("type", StubJson::string_value(envelope_type));
		properties.emplace("payload", std::move(payload));

		if (!request_id.empty()) {
			properties.emplace("requestId", StubJson::string_value(request_id));
		}

		return StubJson::object_with(std::move(properties));
	}

	// Registers a document under a text and returns the text, so a case reads as one
	// statement rather than two.
	std::string parseable_message(const std::string& message_text, StubJson document)
	{
		StubJson::register_parse(message_text, std::move(document));

		return message_text;
	}

	std::string read_error_property(const StubOutboundEnvelope& envelope, std::string_view property_name)
	{
		const std::string property_key{property_name};

		if (!envelope.payload.contains(property_key)) {
			return std::string{};
		}

		return envelope.payload.at(property_key).template get<std::string>();
	}

}

// Requirement 4.3, and requirement 23.4. The case whose failure reaches a producer's
// project, so it goes first.
TEST_CASE("an inbound payload that fails its schema never reaches the inbound queue", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	validator.fail("messages/transport-command.schema.json", {"/command is not one of the seven"});

	StubCodec codec{validator};
	StubInboundQueue inbound_queue;
	StubOutboundQueue outbound_queue;

	const std::string message_text = parseable_message(
		"transport with a bad command",
		envelope_document("request:transport", StubJson::object(), "request-1")
	);

	const auto outcome =
		codec.accept_inbound_message<StubInboundEnvelope, StubOutboundEnvelope>(
			message_text,
			inbound_queue,
			outbound_queue
		);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.refusal_reason == std::optional<InboundRefusalReason>{InboundRefusalReason::payload_schema_violation});
	REQUIRE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path == "messages/transport-command.schema.json");
	REQUIRE(outcome.validation_errors == std::vector<std::string>{"/command is not one of the seven"});

	// The whole point. Nothing for the main thread to route.
	REQUIRE(inbound_queue.empty());
	REQUIRE_FALSE(outcome.envelope.has_value());

	// And the server hears about it.
	REQUIRE(outbound_queue.size() == 1);

	const auto error_envelope = outbound_queue.try_pop();

	REQUIRE(error_envelope.has_value());
	REQUIRE(error_envelope->type == validation_error_envelope_type);
	REQUIRE(error_envelope->request_id == "request-1");
	REQUIRE(read_error_property(*error_envelope, error_code_property) == invalid_payload_error_code);
	REQUIRE(read_error_property(*error_envelope, error_envelope_type_property) == "request:transport");
	REQUIRE(read_error_property(*error_envelope, error_validation_errors_property)
		== "/command is not one of the seven");
}

// Requirement 4.4.
TEST_CASE("a tool call's payload is never validated", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};
	StubInboundQueue inbound_queue;
	StubOutboundQueue outbound_queue;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		const std::string tool_request_envelope_type = "request:" + std::string{binding.tool_name};

		const std::string message_text = parseable_message(
			"call " + tool_request_envelope_type,
			envelope_document(tool_request_envelope_type, StubJson::object(), "relay-1")
		);

		const auto outcome =
			codec.accept_inbound_message<StubInboundEnvelope, StubOutboundEnvelope>(
				message_text,
				inbound_queue,
				outbound_queue
			);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.envelope_type == tool_request_envelope_type);

		// No schema was consulted for the payload, so there was nothing to reject it
		// with. This is the assertion that matters — a re-check that happened to pass
		// would still be the bug, because the vendored copy can be a commit behind the
		// authoritative one the MCP Tool Server already used.
		REQUIRE_FALSE(outcome.payload_validated);
		REQUIRE(outcome.payload_schema_path.empty());
	}

	// Across all 42 calls, the only schema the codec ever asked about was the envelope
	// wrapper — never a tool schema of either direction.
	REQUIRE(validator.requested_schema_paths.size() == tool_output_schema_bindings.size());

	for (const std::string& requested_schema_path : validator.requested_schema_paths) {
		REQUIRE(requested_schema_path == envelope_schema_path);
	}

	REQUIRE(inbound_queue.size() == tool_output_schema_bindings.size());
	REQUIRE(outbound_queue.empty());
}

TEST_CASE("text that cannot be read as JSON is refused without consulting a schema", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};
	StubInboundQueue inbound_queue;
	StubOutboundQueue outbound_queue;

	const auto outcome =
		codec.accept_inbound_message<StubInboundEnvelope, StubOutboundEnvelope>(
			"this is not an envelope",
			inbound_queue,
			outbound_queue
		);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.refusal_reason == std::optional<InboundRefusalReason>{InboundRefusalReason::malformed_json});

	// Nothing was read out of it, so there is nothing to name.
	REQUIRE(outcome.envelope_type.empty());
	REQUIRE(outcome.request_id.empty());
	REQUIRE(validator.requested_schema_paths.empty());

	REQUIRE(inbound_queue.empty());
	REQUIRE(outbound_queue.size() == 1);

	const auto error_envelope = outbound_queue.try_pop();

	REQUIRE(error_envelope.has_value());
	REQUIRE(read_error_property(*error_envelope, error_code_property) == invalid_envelope_error_code);

	// No failure list, because no schema ran. Omitted rather than sent empty: absence
	// says more than an empty string does.
	REQUIRE_FALSE(error_envelope->payload.contains(std::string{error_validation_errors_property}));

	// And no requestId, because there was none to echo. envelope.schema.json requires
	// minLength 1, so an empty one would fail the wrapper this error reports a failure
	// of.
	REQUIRE(error_envelope->request_id.empty());
}

TEST_CASE("an envelope that fails the wrapper schema is refused and still names itself", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	validator.fail(envelope_schema_path, {"/ additional property \"userId\" is not allowed"});

	StubCodec codec{validator};
	StubInboundQueue inbound_queue;
	StubOutboundQueue outbound_queue;

	const std::string message_text = parseable_message(
		"an envelope with a fourth property",
		envelope_document("stream:error", StubJson::object(), "stream-9")
	);

	const auto outcome =
		codec.accept_inbound_message<StubInboundEnvelope, StubOutboundEnvelope>(
			message_text,
			inbound_queue,
			outbound_queue
		);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.refusal_reason
		== std::optional<InboundRefusalReason>{InboundRefusalReason::envelope_schema_violation});

	// The type and correlation identifier were read before the wrapper was checked,
	// precisely so the refusal is diagnosable.
	REQUIRE(outcome.envelope_type == "stream:error");
	REQUIRE(outcome.request_id == "stream-9");

	// The wrapper failed, so no payload schema was consulted.
	REQUIRE(validator.requested_schema_paths == std::vector<std::string>{std::string{envelope_schema_path}});
	REQUIRE_FALSE(outcome.payload_validated);

	REQUIRE(inbound_queue.empty());
	REQUIRE(outbound_queue.size() == 1);

	const auto error_envelope = outbound_queue.try_pop();

	REQUIRE(error_envelope.has_value());
	REQUIRE(read_error_property(*error_envelope, error_code_property) == invalid_envelope_error_code);
	REQUIRE(read_error_property(*error_envelope, error_envelope_type_property) == "stream:error");
	REQUIRE(error_envelope->request_id == "stream-9");
}

TEST_CASE("an accepted envelope reaches the inbound queue and nothing else is sent", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};
	StubInboundQueue inbound_queue;
	StubOutboundQueue outbound_queue;

	StubJson payload = StubJson::object_with({
		{"command", StubJson::string_value("play")}
	});

	const std::string message_text = parseable_message(
		"a good transport command",
		envelope_document("request:transport", std::move(payload), "request-7")
	);

	const auto outcome =
		codec.accept_inbound_message<StubInboundEnvelope, StubOutboundEnvelope>(
			message_text,
			inbound_queue,
			outbound_queue
		);

	REQUIRE(outcome.accepted);
	REQUIRE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path == "messages/transport-command.schema.json");

	// The wrapper first, then the payload. Order is most of what a codec is.
	REQUIRE(validator.requested_schema_paths == std::vector<std::string>{
		std::string{envelope_schema_path},
		"messages/transport-command.schema.json"
	});

	// The envelope was moved onto the queue rather than copied, so the outcome no
	// longer holds one — there is exactly one and the main thread has it.
	REQUIRE_FALSE(outcome.envelope.has_value());
	REQUIRE(outbound_queue.empty());
	REQUIRE(inbound_queue.size() == 1);

	const auto queued_envelope = inbound_queue.try_pop();

	REQUIRE(queued_envelope.has_value());
	REQUIRE(queued_envelope->type == "request:transport");
	REQUIRE(queued_envelope->request_id == "request-7");
	REQUIRE(queued_envelope->payload.contains("command"));
	REQUIRE(queued_envelope->payload.at("command").get<std::string>() == "play");
}

TEST_CASE("an inbound type with no bundled schema is accepted after the wrapper check", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	// request:project_context carries an empty payload and has no bundled schema;
	// error:agent is one of the seven error types the server emits, also unbundled.
	// Both are dispatched on their type and read defensively by their handler.
	for (const std::string& unbundled_envelope_type : {
		std::string{"request:project_context"},
		std::string{"error:agent"}
	}) {
		const std::string message_text = parseable_message(
			"unbundled " + unbundled_envelope_type,
			envelope_document(unbundled_envelope_type, StubJson::object())
		);

		const auto outcome = codec.decode_inbound<StubInboundEnvelope>(message_text);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.envelope_type == unbundled_envelope_type);
		REQUIRE_FALSE(outcome.payload_validated);
		REQUIRE(outcome.payload_schema_path.empty());
		REQUIRE(outcome.envelope.has_value());
	}

	for (const std::string& requested_schema_path : validator.requested_schema_paths) {
		REQUIRE(requested_schema_path == envelope_schema_path);
	}
}

TEST_CASE("a type that is not a string leaves the refusal with nothing to name", "[codec]")
{
	StubJson::reset_parse_table();

	ScriptedPayloadSchemaValidator validator;
	validator.fail(envelope_schema_path, {"/type is not a string"});

	StubCodec codec{validator};

	const std::string message_text = parseable_message(
		"a numeric type",
		StubJson::object_with({
			{"type", StubJson::number_value()},
			{"payload", StubJson::object()}
		})
	);

	const auto outcome = codec.decode_inbound<StubInboundEnvelope>(message_text);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.envelope_type.empty());
	REQUIRE(outcome.refusal_reason
		== std::optional<InboundRefusalReason>{InboundRefusalReason::envelope_schema_violation});
}

// Requirement 4.1.
TEST_CASE("an outbound payload that fails its schema is not serialized", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	validator.fail(project_context_schema_path, {"/tracks/0/guid does not match the GUID pattern"});

	StubCodec codec{validator};

	StubOutboundEnvelope snapshot_envelope;
	snapshot_envelope.type = "state:project_context";
	snapshot_envelope.payload = StubJson::object();

	const OutboundEncodeOutcome outcome = codec.encode_outbound(snapshot_envelope);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.failure_reason
		== std::optional<OutboundEncodeFailureReason>{OutboundEncodeFailureReason::payload_schema_violation});
	REQUIRE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path == project_context_schema_path);
	REQUIRE(outcome.validation_errors
		== std::vector<std::string>{"/tracks/0/guid does not match the GUID pattern"});

	// Nothing to send. A payload known to be wrong is not improved by the server
	// seeing it, and this is the error requirement 4.1 wants instead of a rejection
	// mid-turn.
	REQUIRE(outcome.message_text.empty());

	// And the wrapper was never assembled, let alone checked.
	REQUIRE(validator.requested_schema_paths
		== std::vector<std::string>{std::string{project_context_schema_path}});
}

TEST_CASE("an outbound envelope is serialized with its payload and correlation identifier", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	StubOutboundEnvelope acknowledgement;
	acknowledgement.type = "response:transport";
	acknowledgement.request_id = "request-7";
	acknowledgement.payload = StubJson::object_with({
		{"requestId", StubJson::string_value("request-7")}
	});

	const OutboundEncodeOutcome outcome = codec.encode_outbound(acknowledgement);

	REQUIRE(outcome.accepted);
	REQUIRE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path == "messages/response-transport.schema.json");

	// The payload first, then the assembled wrapper.
	REQUIRE(validator.requested_schema_paths == std::vector<std::string>{
		"messages/response-transport.schema.json",
		std::string{envelope_schema_path}
	});

	REQUIRE(outcome.message_text
		== "{\"payload\":{\"requestId\":\"request-7\"},\"requestId\":\"request-7\",\"type\":\"response:transport\"}");
}

TEST_CASE("an outbound envelope with no correlation identifier omits the property", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	StubOutboundEnvelope prompt_envelope;
	prompt_envelope.type = "state:prompt";
	prompt_envelope.payload = StubJson::object_with({
		{"promptText", StubJson::string_value("add a marker at the chorus")},
		{"locale", StubJson::string_value("en")}
	});

	const OutboundEncodeOutcome outcome = codec.encode_outbound(prompt_envelope);

	REQUIRE(outcome.accepted);

	// Omitted rather than sent empty. envelope.schema.json requires minLength 1 and
	// forbids a fourth property, so an empty requestId would have the extension failing
	// its own wrapper check on every unsolicited message it sends.
	REQUIRE(outcome.message_text.find("requestId") == std::string::npos);
	REQUIRE(outcome.message_text
		== "{\"payload\":{\"locale\":\"en\",\"promptText\":\"add a marker at the chorus\"},\"type\":\"state:prompt\"}");
}

TEST_CASE("a tool result validates against the tool's own output schema", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		StubOutboundEnvelope result_envelope;
		result_envelope.type = "response:" + std::string{binding.tool_name};
		result_envelope.request_id = "relay-1";
		result_envelope.payload = StubJson::object();

		const OutboundEncodeOutcome outcome = codec.encode_outbound(result_envelope);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.payload_validated);
		REQUIRE(outcome.payload_schema_path == binding.schema_path);
	}
}

TEST_CASE("a tool refusal validates against the refusal contract instead", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	StubOutboundEnvelope refusal_envelope;
	refusal_envelope.type = "response:create_send";
	refusal_envelope.request_id = "relay-2";
	refusal_envelope.payload = StubJson::object_with({
		{"refused", StubJson::string_value("true")},
		{"reason", StubJson::string_value("signal_cycle")}
	});

	const OutboundEncodeOutcome outcome = codec.encode_outbound(refusal_envelope);

	REQUIRE(outcome.accepted);
	REQUIRE(outcome.payload_schema_path == tool_result_refusal_schema_path);
}

TEST_CASE("an action-array tool's ordinary result is not treated as a partial outcome", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	// set_track_state and set_item_properties carry an `actions` array as their normal
	// result. Reading that as the partial-outcome discriminator would send both to
	// tool-result-partial.schema.json, whose additionalProperties:false then rejects the
	// extra fields each of them requires — two tools' valid output refused mid-session.
	for (const std::string& tool_name : {std::string{"set_track_state"}, std::string{"set_item_properties"}}) {
		StubOutboundEnvelope result_envelope;
		result_envelope.type = "response:" + tool_name;
		result_envelope.payload = StubJson::object_with({
			{"actions", StubJson::object()}
		});

		const OutboundEncodeOutcome outcome = codec.encode_outbound(result_envelope);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.payload_schema_path.rfind("mcp-tools/outputs/", 0) == 0);
		REQUIRE(outcome.payload_schema_path != "messages/tool-result-partial.schema.json");
	}
}

// Task 22.2, and the case the previous one is the mirror image of.
//
// The framework's `tool_partial_outcome` for an action-array tool is the one payload the
// codec cannot classify: it carries `actions` exactly as that tool's own success does,
// and the case above is right to send the success to the tool's output schema. So a
// partial arriving by the same route is refused on `items`, which the framework cannot
// supply, and the producer sees the tool silently not working. Only
// `daw::result_schema_path_for` knows which it is, it knows on the main thread, and the
// envelope's carried answer is how that reaches the network thread.
TEST_CASE("a carried schema path overrides the one the codec would derive", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	StubOutboundEnvelope framework_partial;
	framework_partial.type = "response:set_item_properties";
	framework_partial.request_id = "relay-3";
	framework_partial.payload = StubJson::object_with({
		{"actions", StubJson::object()}
	});
	framework_partial.payload_schema_path_override =
		std::string{tool_result_partial_contract_schema_path};

	const OutboundEncodeOutcome outcome = codec.encode_outbound(framework_partial);

	REQUIRE(outcome.accepted);
	REQUIRE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path == tool_result_partial_contract_schema_path);

	// The tool's own output schema was never consulted — not consulted and passed over,
	// not consulted at all. The derivation that would have reached it is the thing being
	// overridden.
	REQUIRE(validator.requested_schema_paths == std::vector<std::string>{
		std::string{tool_result_partial_contract_schema_path},
		std::string{envelope_schema_path}
	});

	REQUIRE(outcome.message_text.find("\"type\":\"response:set_item_properties\"") != std::string::npos);
}

// The other half: an envelope that carries no answer is derived for exactly as before.
// Every other case in this file relies on that silently, so one case says it.
TEST_CASE("an envelope carrying no schema path is derived from its type and payload", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	StubOutboundEnvelope tool_success;
	tool_success.type = "response:set_item_properties";
	tool_success.payload = StubJson::object_with({
		{"actions", StubJson::object()}
	});

	REQUIRE(tool_success.payload_schema_path_override.empty());

	const OutboundEncodeOutcome outcome = codec.encode_outbound(tool_success);

	REQUIRE(outcome.accepted);
	REQUIRE(outcome.payload_schema_path == "mcp-tools/outputs/set-item-properties.schema.json");

	// And the question is answerable on the envelope directly, which is what
	// encode_outbound asks.
	REQUIRE(StubCodec::schema_path_for_outbound_envelope(tool_success)
		== std::optional<std::string_view>{"mcp-tools/outputs/set-item-properties.schema.json"});

	tool_success.payload_schema_path_override =
		std::string{tool_result_partial_contract_schema_path};

	REQUIRE(StubCodec::schema_path_for_outbound_envelope(tool_success)
		== std::optional<std::string_view>{tool_result_partial_contract_schema_path});
}

// Requirement 4.1 on the carried path. The override decides what is validated, not only
// what the outcome reports — an override that were merely reported would leave the wrong
// schema doing the actual checking, which is the bug it exists to fix.
TEST_CASE("a payload that fails its carried schema is not serialized", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	validator.fail(tool_result_partial_contract_schema_path, {"/items is required"});

	// The schema the derivation would have chosen passes, so an accepted envelope here
	// could only mean the carried path was ignored.
	StubCodec codec{validator};

	StubOutboundEnvelope framework_partial;
	framework_partial.type = "response:set_item_properties";
	framework_partial.payload = StubJson::object_with({
		{"actions", StubJson::object()}
	});
	framework_partial.payload_schema_path_override =
		std::string{tool_result_partial_contract_schema_path};

	const OutboundEncodeOutcome outcome = codec.encode_outbound(framework_partial);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.failure_reason
		== std::optional<OutboundEncodeFailureReason>{OutboundEncodeFailureReason::payload_schema_violation});
	REQUIRE(outcome.payload_schema_path == tool_result_partial_contract_schema_path);
	REQUIRE(outcome.validation_errors == std::vector<std::string>{"/items is required"});
	REQUIRE(outcome.message_text.empty());
}

TEST_CASE("an outbound type with no bundled schema is serialized after the wrapper check", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	StubCodec codec{validator};

	// stream:request_audio is sent and has no bundled schema for its payload, so there
	// is nothing to validate it against. The wrapper still applies.
	StubOutboundEnvelope stream_request;
	stream_request.type = "stream:request_audio";
	stream_request.payload = StubJson::object();

	const OutboundEncodeOutcome outcome = codec.encode_outbound(stream_request);

	REQUIRE(outcome.accepted);
	REQUIRE_FALSE(outcome.payload_validated);
	REQUIRE(outcome.payload_schema_path.empty());
	REQUIRE(validator.requested_schema_paths
		== std::vector<std::string>{std::string{envelope_schema_path}});
}

TEST_CASE("an outbound envelope that fails the wrapper schema is not sent", "[codec]")
{
	ScriptedPayloadSchemaValidator validator;
	validator.fail(envelope_schema_path, {"/type does not match the namespace:action pattern"});

	StubCodec codec{validator};

	StubOutboundEnvelope malformed_envelope;
	malformed_envelope.type = "Response:CreateTrack";
	malformed_envelope.payload = StubJson::object();

	const OutboundEncodeOutcome outcome = codec.encode_outbound(malformed_envelope);

	REQUIRE_FALSE(outcome.accepted);
	REQUIRE(outcome.failure_reason
		== std::optional<OutboundEncodeFailureReason>{OutboundEncodeFailureReason::envelope_schema_violation});
	REQUIRE(outcome.message_text.empty());

	// No payload schema, because the type is not one the tables bind — which is itself
	// the symptom the wrapper pattern catches.
	REQUIRE_FALSE(outcome.payload_validated);
}

TEST_CASE("the error envelope fits inside the schema that describes its shape", "[codec]")
{
	// stream-error.schema.json caps envelopeType at 64 characters and validationErrors
	// at 4096, and the error is the only thing the server gets when an envelope is
	// refused — so an error too long to describe is the worst available outcome.
	const std::vector<std::string> many_failures(400, std::string(80, 'x'));

	const StubOutboundEnvelope error_envelope =
		build_validation_error_envelope<StubOutboundEnvelope>(
			InboundRefusalReason::payload_schema_violation,
			std::string(300, 'r'),
			"request-3",
			many_failures
		);

	REQUIRE(error_envelope.type == validation_error_envelope_type);
	REQUIRE(error_envelope.request_id == "request-3");

	REQUIRE(read_error_property(error_envelope, error_envelope_type_property).size()
		== maximum_error_envelope_type_length);
	REQUIRE(read_error_property(error_envelope, error_validation_errors_property).size()
		== maximum_rendered_validation_errors_length);

	// The code is one of the two fixed values, so it satisfies the schema's pattern by
	// construction rather than by truncation.
	REQUIRE(read_error_property(error_envelope, error_code_property) == invalid_payload_error_code);

	// And every field is a string, which is what stream-error.schema.json declares —
	// including validationErrors, which is why the failures are rendered rather than
	// carried as an array.
	for (const std::string_view& property_name : {
		error_code_property,
		error_message_property,
		error_envelope_type_property,
		error_validation_errors_property
	}) {
		const std::string property_key{property_name};

		REQUIRE(error_envelope.payload.contains(property_key));
		REQUIRE(error_envelope.payload.at(property_key).is_string());
	}
}

TEST_CASE("each refusal reason carries its own code and sentence", "[codec]")
{
	using sesh_ai::transport::describe_inbound_refusal;
	using sesh_ai::transport::error_code_for_inbound_refusal;

	// Unparseable text and a bad wrapper are both "this is not a usable envelope"; a
	// payload failure is its own code because the envelope was fine.
	REQUIRE(error_code_for_inbound_refusal(InboundRefusalReason::malformed_json)
		== invalid_envelope_error_code);
	REQUIRE(error_code_for_inbound_refusal(InboundRefusalReason::envelope_schema_violation)
		== invalid_envelope_error_code);
	REQUIRE(error_code_for_inbound_refusal(InboundRefusalReason::payload_schema_violation)
		== invalid_payload_error_code);

	// Three distinct sentences, none empty, because the producer-visible cause differs.
	const std::string_view malformed_description =
		describe_inbound_refusal(InboundRefusalReason::malformed_json);
	const std::string_view envelope_description =
		describe_inbound_refusal(InboundRefusalReason::envelope_schema_violation);
	const std::string_view payload_description =
		describe_inbound_refusal(InboundRefusalReason::payload_schema_violation);

	REQUIRE_FALSE(malformed_description.empty());
	REQUIRE_FALSE(envelope_description.empty());
	REQUIRE_FALSE(payload_description.empty());
	REQUIRE(malformed_description != envelope_description);
	REQUIRE(envelope_description != payload_description);
	REQUIRE(malformed_description != payload_description);
}

TEST_CASE("the schema chosen for an outbound payload is answerable before encoding", "[codec]")
{
	// The Tool Executor needs the same answer the codec will reach, and asking is
	// better than a second copy of the rule.
	REQUIRE(StubCodec::outbound_schema_path_for("state:prompt", StubJson::object())
		== std::optional<std::string_view>{"messages/state-prompt.schema.json"});

	REQUIRE(StubCodec::outbound_schema_path_for("response:render", StubJson::object())
		== std::optional<std::string_view>{"mcp-tools/outputs/render.schema.json"});

	REQUIRE(StubCodec::outbound_schema_path_for(
		"response:render",
		StubJson::object_with({{"refused", StubJson::string_value("true")}})
	) == std::optional<std::string_view>{tool_result_refusal_schema_path});

	// A tool this build does not implement, and a type nothing binds.
	REQUIRE_FALSE(StubCodec::outbound_schema_path_for("response:summon_a_bass_player", StubJson::object())
		.has_value());
	REQUIRE_FALSE(StubCodec::outbound_schema_path_for("stream:request_audio", StubJson::object())
		.has_value());

	// Presence, not truth. tool-result-refusal.schema.json declares `refused` a
	// constant true, so a payload carrying a false one is a refusal that got its own
	// discriminator wrong — and validating it against the tool's output schema, which
	// has no `refused` property at all, would report a more confusing failure than the
	// actual problem.
	REQUIRE(StubCodec::payload_is_tool_refusal(
		StubJson::object_with({{"refused", StubJson::string_value("false")}})
	));

	REQUIRE_FALSE(StubCodec::payload_is_tool_refusal(StubJson::object()));
	REQUIRE_FALSE(StubCodec::payload_is_tool_refusal(StubJson::string_value("refused")));
}
