// The inbound bridge parser (task 24.3, ADR 0022, requirements 5.1, 5.2, 13.2, 13.3,
// 15.2, 15.4).
//
// Driven against the real JSON library rather than a stub, for the reason
// `tests/transport/tool_request_readers_test.cpp` gives at its own head: reading fields
// out of a document is the whole of what this does, so a stub would have the cases agree
// with each other and with nothing else. The parser itself names no JSON library — that
// is what lets the composition root's own suite drive it over a stand-in document — but
// the cases that matter are the ones where a real renderer's text is what arrived.
//
// Four groups, in the order of what each would cost if it were wrong.
//
// **A decision that contradicts its message name is refused.** The one piece of
// load-bearing logic here. `BridgeMessageDispatcherSink` takes the producer's verdict
// from the message *name* and treats the payload's `decision` as the UI's copy — which
// is correct, and is only correct because something refuses the case where the copy
// differs. Without this, a UI bug that swapped the two buttons would approve what the
// producer rejected, through a path where every component behaved as documented.
// `confirm-decision.schema.json` says so in its own words.
//
// **A payload missing a property its schema requires is refused at the bridge.** Not
// because the extension needs the field, but because of where the alternative check
// happens: the Envelope Codec validates on the network thread, so a prompt that fails
// there is one the producer watched disappear with nothing said.
//
// **Direction is answered from the contract.** A `view:` name arriving from JavaScript
// is the bridge pointed the wrong way round, and `ui_host.h` is what says so.
//
// **Nothing is rewritten on the way through.** The payload is the UI's document, carried
// verbatim, because the standalone Bedrock Guardrail's `dataPath` resolves to a named
// field inside it.

#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#if __has_include(<nlohmann/json.hpp>)
	#define SESH_AI_TESTS_HAVE_JSON 1
#else
	#define SESH_AI_TESTS_HAVE_JSON 0
#endif

#if SESH_AI_TESTS_HAVE_JSON
	#include <nlohmann/json.hpp>

	#include <ui/bridge_parsing.h>
#endif

#if SESH_AI_TESTS_HAVE_JSON

namespace {

	using sesh_ai::ui::InboundBridgeMessageProblem;
	using sesh_ai::ui::InboundBridgeMessageRead;
	using sesh_ai::ui::read_bridge_request_identifier;
	using sesh_ai::ui::read_inbound_bridge_message;
	using sesh_ai::ui::serialise_verbatim_bridge_payload;

	using Json = nlohmann::json;

	// The text-to-document step the parser deliberately leaves to its caller, as the
	// composition root supplies it: a document that did not parse comes back empty.
	std::optional<Json> parse(const std::string& text)
	{
		Json document = Json::parse(text, nullptr, false);

		if (document.is_discarded()) {
			return std::nullopt;
		}

		return document;
	}

	InboundBridgeMessageRead<Json> read(std::string_view message_name, const std::string& text)
	{
		return read_inbound_bridge_message<Json>(message_name, parse(text));
	}

}

TEST_CASE("a confirmation decision that contradicts its message name is refused", "[bridge][parsing]")
{
	SECTION("confirm:approve carrying a rejection")
	{
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:approve", "{\"requestId\":\"confirm-1\",\"decision\":\"rejected\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem
			== InboundBridgeMessageProblem::decision_disagrees_with_message_name);

		// No payload and no identifier, so there is nothing a caller could reach for and
		// act on by accident.
		REQUIRE_FALSE(refused.payload.has_value());
		REQUIRE(refused.request_identifier.empty());
	}

	SECTION("confirm:reject carrying an approval")
	{
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:reject", "{\"requestId\":\"confirm-1\",\"decision\":\"approved\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem
			== InboundBridgeMessageProblem::decision_disagrees_with_message_name);
	}

	SECTION("a decision value the schema does not enumerate is refused the same way")
	{
		// `expired` is a real resolution, but it is the server's own conclusion and is
		// never sent by a client — `confirm-decision.schema.json` enumerates two values
		// and that is the whole of it. It cannot agree with either name, so it is refused
		// on the agreement rather than needing a rule of its own.
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:approve", "{\"requestId\":\"confirm-1\",\"decision\":\"expired\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem
			== InboundBridgeMessageProblem::decision_disagrees_with_message_name);
	}

	SECTION("and both decisions that agree are accepted, carrying the identifier")
	{
		const InboundBridgeMessageRead<Json> approval =
			read("confirm:approve", "{\"requestId\":\"confirm-1\",\"decision\":\"approved\"}");

		REQUIRE(approval.readable());
		REQUIRE(approval.request_identifier == "confirm-1");

		const InboundBridgeMessageRead<Json> rejection =
			read("confirm:reject", "{\"requestId\":\"confirm-2\",\"decision\":\"rejected\"}");

		REQUIRE(rejection.readable());
		REQUIRE(rejection.request_identifier == "confirm-2");
	}
}

TEST_CASE("a payload missing a property its schema requires is refused", "[bridge][parsing]")
{
	SECTION("a prompt without promptText")
	{
		const InboundBridgeMessageRead<Json> refused = read("state:prompt", "{\"locale\":\"en\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("a prompt without locale")
	{
		const InboundBridgeMessageRead<Json> refused =
			read("state:prompt", "{\"promptText\":\"add a compressor to the kick\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("an empty promptText is the same refusal as an absent one")
	{
		// `minLength: 1`. An empty string is also the one value where the guardrail has
		// nothing to evaluate, so a turn started from it would be a turn nobody checked.
		const InboundBridgeMessageRead<Json> refused =
			read("state:prompt", "{\"promptText\":\"\",\"locale\":\"en\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("a promptText that is present but not a string")
	{
		// The failure a stub document cannot produce and a renderer can: JavaScript will
		// serialise a number, an array, or an object into that property without
		// complaint.
		const InboundBridgeMessageRead<Json> refused =
			read("state:prompt", "{\"promptText\":42,\"locale\":\"en\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("a decision without requestId")
	{
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:approve", "{\"decision\":\"approved\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("a decision with an empty requestId is refused here rather than one step later")
	{
		// The Confirmation Coordinator would refuse it as untracked, which is the same
		// outcome with less to say about why — and `minLength: 1` means a payload that
		// carried an empty one would not have been sendable either.
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:approve", "{\"requestId\":\"\",\"decision\":\"approved\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}

	SECTION("a decision without decision")
	{
		const InboundBridgeMessageRead<Json> refused =
			read("confirm:approve", "{\"requestId\":\"confirm-1\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::required_field_unreadable);
	}
}

TEST_CASE("direction and the message name are answered from the contract", "[bridge][parsing]")
{
	SECTION("every view model is refused on its declared direction")
	{
		// All six, rather than one of them. The `view:` namespace is what makes this
		// answerable at all — `ui_host.h` asserts it is not one of the seven
		// `envelope.schema.json` enumerates — and a name added to the set without a
		// direction is the drift this is guarding.
		for (const std::string_view view_model_name : {
			std::string_view{"view:confirmation_pending"},
			std::string_view{"view:confirmation_resolved"},
			std::string_view{"view:agent_response"},
			std::string_view{"view:stream_ingest"},
			std::string_view{"view:stream_error"},
			std::string_view{"view:connection_state"}
		}) {
			const InboundBridgeMessageRead<Json> refused = read(view_model_name, "{}");

			INFO("message name: " << view_model_name);

			REQUIRE_FALSE(refused.readable());
			REQUIRE(refused.problem == InboundBridgeMessageProblem::wrong_direction);
		}
	}

	SECTION("a script download is refused on direction too, despite being a protocol payload")
	{
		// The fourth verbatim payload, and the only one of the four that travels the
		// other way. Being a real envelope type is not what decides the direction.
		const InboundBridgeMessageRead<Json> refused =
			read("script:download", "{\"fileName\":\"sesh_ai_generated.lua\"}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::wrong_direction);
	}

	SECTION("a name neither half of the contract declares is its own problem")
	{
		// Distinct from a wrong direction, because the two have different fixes: this is
		// a message the React application invented, and that is a drift worth surfacing
		// rather than one to pass along and hope about.
		const InboundBridgeMessageRead<Json> refused =
			read("state:something_the_ui_invented", "{}");

		REQUIRE_FALSE(refused.readable());
		REQUIRE(refused.problem == InboundBridgeMessageProblem::undeclared_message_name);
	}

	SECTION("an empty message name is an undeclared one")
	{
		REQUIRE(read("", "{}").problem == InboundBridgeMessageProblem::undeclared_message_name);
	}

	SECTION("direction is decided before the payload, so a wrong-way name with no payload at all still reports the direction")
	{
		// Which problem is reported matters: "the bridge is pointed the wrong way" and
		// "the renderer sent something unparseable" lead to different places to look.
		const InboundBridgeMessageRead<Json> refused =
			read_inbound_bridge_message<Json>("view:connection_state", std::nullopt);

		REQUIRE(refused.problem == InboundBridgeMessageProblem::wrong_direction);
	}
}

TEST_CASE("a document that is not an object carries no protocol payload", "[bridge][parsing]")
{
	SECTION("text that is not JSON at all")
	{
		REQUIRE(read("state:prompt", "add a compressor to the kick").problem
			== InboundBridgeMessageProblem::not_a_json_document);
	}

	SECTION("empty text")
	{
		REQUIRE(read("state:prompt", "").problem
			== InboundBridgeMessageProblem::not_a_json_document);
	}

	SECTION("an object that was truncated in transit")
	{
		REQUIRE(read("state:prompt", "{\"promptText\":\"add a com").problem
			== InboundBridgeMessageProblem::not_a_json_document);
	}

	SECTION("valid JSON that is not an object")
	{
		// Each of these parses. None of them is a payload — every one of the protocol
		// schemas declares `"type": "object"` — and the distinction from unparseable text
		// is the useful one, because this is a renderer that serialised the wrong value
		// rather than one that produced broken text.
		REQUIRE(read("state:prompt", "[]").problem
			== InboundBridgeMessageProblem::not_a_json_object);
		REQUIRE(read("state:prompt", "\"add a compressor\"").problem
			== InboundBridgeMessageProblem::not_a_json_object);
		REQUIRE(read("state:prompt", "null").problem
			== InboundBridgeMessageProblem::not_a_json_object);
		REQUIRE(read("confirm:approve", "42").problem
			== InboundBridgeMessageProblem::not_a_json_object);
	}
}

TEST_CASE("an accepted payload crosses verbatim", "[bridge][parsing]")
{
	// Requirement 15.2 and the guardrail's `dataPath`. The UI builds the document and
	// the extension carries it — so this case is about what the parser does *not* do.
	const std::string text =
		"{\"promptText\":\"add a compressor to the kick\",\"locale\":\"pt-BR\","
		"\"conversationId\":\"01HQ8Z0000000000000000000\"}";

	const InboundBridgeMessageRead<Json> accepted = read("state:prompt", text);

	REQUIRE(accepted.readable());
	REQUIRE(accepted.payload.has_value());

	SECTION("field for field, including the optional property nothing here reads")
	{
		REQUIRE(*accepted.payload == Json::parse(text));
	}

	SECTION("the producer's own words are untouched")
	{
		// Not re-framed, not trimmed, not prefixed. The guardrail scores this string.
		REQUIRE(accepted.payload->at("promptText").get<std::string>()
			== "add a compressor to the kick");
	}

	SECTION("nothing was added")
	{
		// `additionalProperties: false`, so a property the parser helpfully supplied
		// would be refused by the schema on the network thread.
		REQUIRE(accepted.payload->size() == 3);
	}

	SECTION("a prompt carries no request identifier, and none is invented")
	{
		REQUIRE(accepted.request_identifier.empty());
	}
}

TEST_CASE("the sink's identifier reader agrees with the parser", "[bridge][parsing]")
{
	// The two are separate seams on `BridgeMessageDispatcherSink` and read the same
	// field, so they are held to the same answer here rather than by inspection.
	SECTION("the identifier a decision carried")
	{
		REQUIRE(read_bridge_request_identifier(
			Json::parse("{\"requestId\":\"confirm-1\",\"decision\":\"approved\"}")) == "confirm-1");
	}

	SECTION("empty for a payload that carried none")
	{
		REQUIRE(read_bridge_request_identifier(Json::parse("{\"decision\":\"approved\"}")).empty());
	}

	SECTION("empty for an identifier of the wrong type")
	{
		REQUIRE(read_bridge_request_identifier(Json::parse("{\"requestId\":7}")).empty());
	}

	SECTION("empty for a document that is not an object")
	{
		REQUIRE(read_bridge_request_identifier(Json::parse("[]")).empty());
		REQUIRE(read_bridge_request_identifier(Json{}).empty());
	}
}

TEST_CASE("the script download payload is serialised as it stands", "[bridge][parsing]")
{
	// The fourth verbatim payload. ADR 0011: a download, never an execution — so the
	// only operation on this path is turning the document into text.
	SECTION("an object becomes its own JSON text")
	{
		const Json payload = Json::parse(
			"{\"fileName\":\"sesh_ai_generated.lua\",\"scriptText\":\"reaper.Undo_BeginBlock()\"}"
		);

		const std::string serialised = serialise_verbatim_bridge_payload(payload);

		REQUIRE_FALSE(serialised.empty());
		REQUIRE(Json::parse(serialised) == payload);
	}

	SECTION("an empty object is a JSON document and publishes")
	{
		// `{}` is two characters and is not nothing. `UiHost::publish_to_javascript`
		// refuses an empty payload, so the distinction decides whether the panel hears
		// about the download at all.
		REQUIRE(serialise_verbatim_bridge_payload(Json::object()) == "{}");
	}

	SECTION("anything that is not an object renders as empty, which the UI Host refuses")
	{
		REQUIRE(serialise_verbatim_bridge_payload(Json::array()).empty());
		REQUIRE(serialise_verbatim_bridge_payload(Json{}).empty());
		REQUIRE(serialise_verbatim_bridge_payload(Json{"a bare string"}).empty());
	}
}

#else

TEST_CASE("the inbound bridge parser needs nlohmann/json")
{
	SKIP(
		"nlohmann/json is not on the include path, so the parser cannot be driven against "
		"the documents a renderer actually sends. Configure with a vcpkg toolchain."
	);
}

#endif
