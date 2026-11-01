// The parser that turns one message from the renderer into the verbatim protocol
// payload it claims to be (ADR 0022, requirements 5.1, 5.2, 13.2, 13.3, 15.2, 15.4).
//
// `ui/bridge_serialisation.h` is the outbound half of the bridge contract: six view
// models, each written into the camelCase JSON `bridge-messages/*.schema.json`
// describes. This is the inbound half, and the asymmetry between them is the whole
// reason it is a separate header.
//
// Outbound, the extension *builds* the document and the only thing that can go wrong is
// escaping. Inbound, the document is built by a Chromium renderer running the React
// application, arrives as text, and the extension's job is to decide whether what
// arrived is one of the three payloads the UI is allowed to originate — before it is
// pushed onto the outbound queue, because once it is on the queue the only remaining
// check is the Envelope Codec's on the network thread, and a prompt refused there is a
// prompt the producer watched disappear.
//
// ---------------------------------------------------------------------------
// Three inbound names, two schemas, and one agreement that has to be checked
//
// `ui_host.h` declares which direction each of the ten names travels and which schema
// each payload answers to. Nothing here restates either — `bridge_message_direction_for`
// and `bridge_message_schema_path_for` are read, the same rule the serialisers follow.
//
// What the two schemas require is the part this header carries:
//
//   - `state:prompt` → `messages/state-prompt.schema.json` requires `promptText` and
//     `locale`. `promptText` is the field the standalone Bedrock Guardrail's `dataPath`
//     resolves to, which is why the UI originates the payload rather than handing over
//     bare text for the C++ to frame — and why a prompt arriving without it is refused
//     here rather than reframed into something the guardrail would score.
//
//   - `confirm:approve` and `confirm:reject` → `messages/confirm-decision.schema.json`
//     requires `requestId` and `decision`. The schema's own description settles what to
//     do when the two halves of the message disagree: a `confirm:approve` carrying
//     `decision: "rejected"` "is rejected here rather than resolved by precedence,
//     since neither reading is safe to guess at for an operation the producer was asked
//     about". So the agreement is checked, and a disagreement is refused.
//
//     That check is this header's one piece of load-bearing logic.
//     `BridgeMessageDispatcherSink` takes the verdict from the message *name* and treats
//     the payload's `decision` as the UI's copy of the same answer — which is correct,
//     and is only correct because something refuses the case where the copy differs.
//     Without it a UI bug that swapped the two buttons would approve what the producer
//     rejected, through a path where every component behaved as documented.
//
// ---------------------------------------------------------------------------
// What this header does not do, and why
//
// **It does not parse text.** `read_inbound_bridge_message` takes an already-parsed
// document as an optional, where empty means "the text was not a JSON document at all".
// Turning text into a document is one call into a JSON library, and keeping it outside
// is what lets the whole of the decision above be compiled and driven by the Catch2
// suite — which does not compile `src/`'s translation units and runs on machines with no
// vcpkg. The same split `transport/tool_payload_reading.h` makes, for the same reason.
//
// **It does not validate the payload against its schema.** The three fields it reads are
// the ones a *decision here* depends on: whether to queue the message at all, and which
// confirmation it answers. Everything else — `locale`'s BCP 47 pattern, `promptText`'s
// 8192-character ceiling, `additionalProperties: false` — is checked once, by the
// Envelope Codec against the vendored bundle, on the thread requirement 22.2 puts
// validation on. Checking a subset of it twice would be two places to update and one
// place to disagree.
//
// **It does not touch the view models.** A `view:` name arriving from JavaScript is the
// bridge pointed the wrong way round. It is refused on the direction `ui_host.h`
// declares, and the refusal is reported as such rather than as a malformed payload,
// because the two are different faults with different fixes.

#ifndef SESH_AI_UI_BRIDGE_PARSING_H
#define SESH_AI_UI_BRIDGE_PARSING_H

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ui/ui_host.h>

namespace sesh_ai::ui {

	// ---------------------------------------------------------------------------
	// The field names and the two decision values
	// ---------------------------------------------------------------------------

	// `messages/state-prompt.schema.json`'s two required properties.
	//
	// Named constants rather than string literals at the read, because `promptText` is
	// also the guardrail's `dataPath` target and the schema says it is "named promptText
	// rather than text so that no future metadata property can be mistaken for it". A
	// spelling that matters that much is worth being a value.
	inline constexpr std::string_view producer_prompt_text_field{"promptText"};
	inline constexpr std::string_view producer_prompt_locale_field{"locale"};

	// `messages/confirm-decision.schema.json`'s two required properties, and the two
	// values its `decision` enumerates.
	inline constexpr std::string_view confirmation_request_identifier_field{"requestId"};
	inline constexpr std::string_view confirmation_decision_field{"decision"};
	inline constexpr std::string_view confirmation_approved_decision{"approved"};
	inline constexpr std::string_view confirmation_rejected_decision{"rejected"};

	// Which `decision` value a message name must carry, or empty for a name that carries
	// no decision at all.
	//
	// The agreement the schema requires, stated once. Both the check below and
	// `BridgeMessageDispatcherSink`'s verdict are derived from the name, so this is the
	// single place the pairing lives.
	constexpr std::string_view confirmation_decision_for(std::string_view message_name)
	{
		if (message_name == confirmation_approval_bridge_message_name) {
			return confirmation_approved_decision;
		}

		if (message_name == confirmation_rejection_bridge_message_name) {
			return confirmation_rejected_decision;
		}

		return std::string_view{};
	}

	// ---------------------------------------------------------------------------
	// What was wrong with it
	// ---------------------------------------------------------------------------

	// Why a message from the renderer was not accepted.
	//
	// Separate values rather than one failure, because the three groups have three
	// different causes and a log line that cannot tell them apart is a log line that
	// cannot be acted on: a wrong direction or an undeclared name is a drift between the
	// two halves of the bridge contract, unreadable text is a serialisation fault in the
	// renderer, and a missing or disagreeing field is a bug in the React application's
	// payload construction.
	enum class InboundBridgeMessageProblem {
		// Accepted.
		none,

		// A name neither half of the contract declares.
		undeclared_message_name,

		// A name that travels the other way. `view:` namespaced, in every case this
		// build has.
		wrong_direction,

		// The text was not a JSON document.
		not_a_json_document,

		// It parsed, but to something other than an object — an array, a number, a bare
		// string. No protocol payload is any of those.
		not_a_json_object,

		// A property the message's schema requires is absent, or is present and not a
		// string.
		required_field_unreadable,

		// `confirm:approve` carrying `decision: "rejected"`, or the reverse. See the
		// file comment: neither reading is safe to guess at.
		decision_disagrees_with_message_name
	};

	constexpr std::string_view describe(InboundBridgeMessageProblem problem)
	{
		switch (problem) {
		case InboundBridgeMessageProblem::none:
			return "the message was readable";
		case InboundBridgeMessageProblem::undeclared_message_name:
			return "neither half of the bridge contract declares this message name";
		case InboundBridgeMessageProblem::wrong_direction:
			return "this message name travels to JavaScript, so it cannot arrive from it";
		case InboundBridgeMessageProblem::not_a_json_document:
			return "the payload was not a JSON document";
		case InboundBridgeMessageProblem::not_a_json_object:
			return "the payload parsed to something other than an object";
		case InboundBridgeMessageProblem::required_field_unreadable:
			return "a property this message's schema requires is absent or is not a string";
		case InboundBridgeMessageProblem::decision_disagrees_with_message_name:
			return "the payload's decision contradicts the message name it arrived under";
		}

		return "";
	}

	// One message from the renderer, read.
	//
	// The payload is held rather than returned bare so that the identifier can travel
	// with it: `BridgeMessageDispatcherSink` needs both, and reading the identifier back
	// out of the payload a second time at the call site would be a second place to decide
	// what counts as absent.
	//
	// Empty `payload` and `problem != none` are the same condition said twice, and that
	// is deliberate — `readable()` is what a caller branches on and the problem is what a
	// log line names.
	template <typename JsonValue>
	struct InboundBridgeMessageRead {
		std::optional<JsonValue> payload;

		// `requestId`, for the two confirmation decisions. Empty for a prompt, which
		// carries none, and empty for anything refused.
		//
		// A `confirm:*` whose identifier read as empty is refused before this is
		// returned: `confirm-decision.schema.json` puts `minLength: 1` on it, so an
		// empty answer would not have been sendable either, and the Confirmation
		// Coordinator would refuse it as untracked one step later with less to say
		// about why.
		std::string request_identifier;

		InboundBridgeMessageProblem problem = InboundBridgeMessageProblem::none;

		bool readable() const { return problem == InboundBridgeMessageProblem::none; }
	};

	namespace detail {

		// One required string property, or empty when it is absent or of another type.
		//
		// `at` rather than `operator[]`, because the document is const here and every
		// JSON type this is instantiated over vivifies a missing member through the
		// mutable subscript. The `contains` guard is what makes `at` safe on a document
		// that throws for an absent key.
		template <typename JsonValue>
		std::string read_required_string_field(
			const JsonValue& document,
			std::string_view field_name)
		{
			const std::string field{field_name};

			if (!document.contains(field)) {
				return {};
			}

			const JsonValue& value = document.at(field);

			if (!value.is_string()) {
				return {};
			}

			return value.template get<std::string>();
		}

	}

	// ---------------------------------------------------------------------------
	// The read
	// ---------------------------------------------------------------------------

	// Reads one message the renderer sent.
	//
	// `parsed_payload` is empty for text that was not a JSON document — see the file
	// comment on why the parse itself is the caller's. Everything after that is this
	// function's, and the order of the checks is the order of consequence: the contract
	// first, because a name outside it means the two halves have drifted and nothing
	// about the payload matters; then the document's shape; then the fields the schema
	// requires; then the agreement between the payload and the name it arrived under.
	//
	// The payload is moved out rather than rebuilt. It is carried verbatim all the way to
	// the server — the UI's document is the one the guardrail and the schema both see —
	// so there is deliberately no step here that could normalise, reframe, or re-key it.
	template <typename JsonValue>
	InboundBridgeMessageRead<JsonValue> read_inbound_bridge_message(
		std::string_view message_name,
		std::optional<JsonValue> parsed_payload)
	{
		InboundBridgeMessageRead<JsonValue> read;

		const std::optional<BridgeMessageDirection> direction =
			bridge_message_direction_for(message_name);

		if (!direction.has_value()) {
			read.problem = InboundBridgeMessageProblem::undeclared_message_name;

			return read;
		}

		if (*direction != BridgeMessageDirection::from_javascript) {
			read.problem = InboundBridgeMessageProblem::wrong_direction;

			return read;
		}

		if (!parsed_payload.has_value()) {
			read.problem = InboundBridgeMessageProblem::not_a_json_document;

			return read;
		}

		if (!parsed_payload->is_object()) {
			read.problem = InboundBridgeMessageProblem::not_a_json_object;

			return read;
		}

		const std::string_view required_decision = confirmation_decision_for(message_name);

		if (required_decision.empty()) {
			// A prompt. Both of `state-prompt.schema.json`'s required properties are
			// read, and `locale` is read for the same reason `promptText` is rather than
			// because anything here uses it: the payload crosses to the server untouched,
			// so a field the schema requires and the UI omitted is a turn that fails at
			// the far end of the socket instead of at the bridge.
			if (detail::read_required_string_field(*parsed_payload, producer_prompt_text_field)
					.empty()
				|| detail::read_required_string_field(
					*parsed_payload,
					producer_prompt_locale_field).empty()) {
				read.problem = InboundBridgeMessageProblem::required_field_unreadable;

				return read;
			}

			read.payload = std::move(parsed_payload);

			return read;
		}

		// A confirmation decision.
		const std::string request_identifier = detail::read_required_string_field(
			*parsed_payload,
			confirmation_request_identifier_field
		);

		const std::string decision = detail::read_required_string_field(
			*parsed_payload,
			confirmation_decision_field
		);

		if (request_identifier.empty() || decision.empty()) {
			read.problem = InboundBridgeMessageProblem::required_field_unreadable;

			return read;
		}

		if (decision != required_decision) {
			read.problem = InboundBridgeMessageProblem::decision_disagrees_with_message_name;

			return read;
		}

		read.request_identifier = request_identifier;
		read.payload = std::move(parsed_payload);

		return read;
	}

	// The `requestId` out of a payload this function already accepted.
	//
	// The signature `BridgeMessageDispatcherSink::RequestIdentifierReader` holds. It
	// reads the field rather than the outcome because the sink holds the two seams
	// separately, and it agrees with `read_inbound_bridge_message` by reading the same
	// field the same way — which is why both go through
	// `detail::read_required_string_field` rather than each spelling the read.
	template <typename JsonValue>
	std::string read_bridge_request_identifier(const JsonValue& payload)
	{
		if (!payload.is_object()) {
			return {};
		}

		return detail::read_required_string_field(
			payload,
			confirmation_request_identifier_field
		);
	}

	// ---------------------------------------------------------------------------
	// The fourth verbatim payload, going the other way
	// ---------------------------------------------------------------------------

	// `script:download`'s payload, as the text the bridge carries (requirement 5.7,
	// ADR 0011).
	//
	// The one verbatim protocol payload that travels *to* the renderer, and the only one
	// of the four with no view model: the Message Dispatcher routes the envelope to the
	// UI Host and the payload crosses untouched, because there is nothing for the C++
	// side to decide. A download, never an execution.
	//
	// Empty for anything that is not an object, which is the refusal rather than an
	// oversight: `UiHost::publish_to_javascript` declines an empty payload, and a
	// `script:download` whose payload was a bare string or a null is a message the React
	// application cannot read either. `{}` is a JSON document and publishes; `""` is not
	// and does not.
	//
	// Templated on the document type for the usual reason, and `dump()` is the one
	// operation it needs — which keeps the serialisation of agent-generated ReaScript out
	// of this header entirely. The text then goes through
	// `to_javascript_string_literal`, which is where its escaping is defended.
	template <typename JsonValue>
	std::string serialise_verbatim_bridge_payload(const JsonValue& payload)
	{
		if (!payload.is_object()) {
			return {};
		}

		return payload.dump();
	}

	// Every name this header reads a payload for arrives from JavaScript, and both
	// confirmation names pair with a decision value. Asserted rather than trusted,
	// because the pairing is what `BridgeMessageDispatcherSink`'s verdict relies on and a
	// name that fell out of `confirmation_decision_for` would be read as a prompt — which
	// would queue a confirmation decision with no `requestId` check at all.
	static_assert(
		bridge_message_direction_for(producer_prompt_bridge_message_name)
				== BridgeMessageDirection::from_javascript
			&& bridge_message_direction_for(confirmation_approval_bridge_message_name)
				== BridgeMessageDirection::from_javascript
			&& bridge_message_direction_for(confirmation_rejection_bridge_message_name)
				== BridgeMessageDirection::from_javascript,
		"every message this parser reads must be one the UI originates"
	);

	static_assert(
		confirmation_decision_for(confirmation_approval_bridge_message_name)
				== confirmation_approved_decision
			&& confirmation_decision_for(confirmation_rejection_bridge_message_name)
				== confirmation_rejected_decision
			&& confirmation_decision_for(producer_prompt_bridge_message_name).empty(),
		"the two confirmation names carry a decision and the prompt carries none"
	);

	// The one verbatim payload that goes the other way, held to its direction for the
	// same reason `bridge_serialisation.h` holds the six view models to theirs.
	static_assert(
		bridge_message_direction_for(script_download_bridge_message_name)
			== BridgeMessageDirection::to_javascript,
		"a script download travels to JavaScript and never the other way"
	);

}

#endif
