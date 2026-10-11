// The Envelope Codec: the network thread's whole share of the work, and the reason
// the main thread never sees text.
//
// Every message on the wire is `{ type, payload, requestId? }`. This turns one of
// those into an InboundEnvelope the Main-Thread Dispatcher can route, or refuses it;
// and turns an OutboundEnvelope into the text the Transport Client sends, or refuses
// that. Both directions run on the network thread inside a Socket.IO callback
// (requirement 4.5), which is what keeps JSON parsing and schema validation off
// REAPER's main thread — a producer's UI must not pay for a large project context
// snapshot being checked against its schema.
//
// That thread relationship is the point of accept_inbound_message below rather than
// a note in a comment. It takes the text and the two queues and is the only entry
// point the Transport Client needs: on success a parsed, validated envelope goes on
// the inbound queue; on refusal an `error:validation` envelope goes on the outbound
// queue and nothing goes on the inbound one. There is no overload that hands text to
// the main thread, because requirement 22.2 is that the main thread receives trusted
// structures — and the cheapest way to guarantee that is for no function to exist
// that would do otherwise.
//
// The four rules about *what* gets validated live in schema_validator.h, next to the
// tables that answer them. What lives here is the *consequence* of a rule, and one
// of those is worth stating twice.
//
// **An inbound payload that fails its bundled schema is refused** (requirement 4.3).
// It does not reach a handler, it is not repaired, and it is not passed along with a
// flag saying it looked wrong. The extension answers with an error naming what
// failed, which is requirement 23.4 — acting on a shape it did not understand is
// worse than answering an error, because the producer's project is on the other end
// of the handler.
//
// **A tool call is not validated on the way in.** `request:<tool_name>` carries tool
// input, the MCP Tool Server already checked it against the authoritative schema, and
// the 42 input schemas are deliberately absent from the bundle. So the codec looks
// the type up, finds no bundled schema, and dispatches. That is the same path an
// unbundled type takes, and it is deliberate that it is not a special case with a
// comment asking to be filled in later: there is nothing to fill in.
//
// Three things about the shape of this file.
//
// **Templated on the JSON type.** transport/envelope.h pulls nlohmann/json, and the
// test target links it only when it is installed, so the codec is written against the
// operations it actually performs on a document — parse, is_discarded, is_object,
// contains, at, is_string, get<std::string>, object(), operator[], dump. The suite
// drives it with a type that provides those and nothing else, so what the cases
// exercise is which branch the codec takes. The explicit instantiation in
// envelope_codec.cpp is the compile-time proof that nlohmann::json satisfies the same
// calls.
//
// **Validation is a substituted seam.** PayloadSchemaValidator decides valid or not;
// this file decides what follows. That split is what makes "an invalid payload never
// reaches a handler" checkable without a validation library, and it is also why the
// suite cannot check that a real snapshot satisfies project-context.schema.json —
// that needs the real validator and is task 4.3's business.
//
// **Every path is total.** No exception escapes either direction. A JSON library
// throwing out of a Socket.IO callback is a dropped connection, which a producer sees
// as Sesh losing their session; a refused envelope is a line in the log and an error
// the server can act on.

#ifndef SESH_AI_TRANSPORT_ENVELOPE_CODEC_H
#define SESH_AI_TRANSPORT_ENVELOPE_CODEC_H

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

namespace sesh_ai::transport {

	// The envelope's three properties, spelled once. envelope.schema.json sets
	// `additionalProperties: false`, so this is the whole wire vocabulary.
	inline constexpr std::string_view envelope_type_property{"type"};
	inline constexpr std::string_view envelope_payload_property{"payload"};
	inline constexpr std::string_view envelope_request_id_property{"requestId"};

	// The envelope type a refusal is answered with, and the two codes it carries.
	//
	// Both codes are the server's own — `invalid_envelope` and `invalid_payload` from
	// its routing error codes — so a rejection reads the same whichever side produced
	// it. Deliberately not a third code for unparseable text: text that is not JSON is
	// not an envelope, which is what `invalid_envelope` already says.
	inline constexpr std::string_view validation_error_envelope_type{"error:validation"};
	inline constexpr std::string_view invalid_envelope_error_code{"invalid_envelope"};
	inline constexpr std::string_view invalid_payload_error_code{"invalid_payload"};

	// The error payload's four properties, as stream-error.schema.json spells them.
	inline constexpr std::string_view error_code_property{"code"};
	inline constexpr std::string_view error_message_property{"message"};
	inline constexpr std::string_view error_envelope_type_property{"envelopeType"};
	inline constexpr std::string_view error_validation_errors_property{"validationErrors"};

	// Why an inbound message was refused.
	//
	// Three reasons, and they are separate because the producer-visible cause differs.
	// Unparseable text means the connection carried something that is not this
	// protocol. An envelope that fails the wrapper schema means the type or the shape
	// is wrong — a misspelled namespace, a payload that is not an object, a fourth
	// property. A payload that fails its own schema means the type was understood and
	// the contents were not, which is the case most likely to mean the server is a
	// version ahead.
	enum class InboundRefusalReason {
		malformed_json,
		envelope_schema_violation,
		payload_schema_violation
	};

	// Why an outbound envelope was not sent.
	//
	// Both are the extension's own bug, caught locally with a diagnosable error rather
	// than as a server rejection mid-turn, which is the whole of requirement 4.1.
	enum class OutboundEncodeFailureReason {
		payload_schema_violation,
		envelope_schema_violation
	};

	// What decoding one inbound message produced.
	//
	// The envelope is present exactly when `accepted` is true. `envelope_type` and
	// `request_id` are filled in whenever they could be read at all, including on a
	// refusal — they are what the log line and the error payload need, and a refusal
	// that cannot name what it refused is not diagnosable.
	template <typename InboundEnvelopeType>
	struct InboundDecodeOutcome {
		bool accepted = false;

		std::optional<InboundEnvelopeType> envelope;

		// As it arrived. Empty when the document had no readable string `type`.
		std::string envelope_type;

		std::string request_id;

		// Empty when the message was accepted.
		std::optional<InboundRefusalReason> refusal_reason;

		// The bundled schema the payload was checked against. Empty when the type has
		// no bundled schema — which includes every `request:<tool_name>`, and is not a
		// failure.
		std::string payload_schema_path;

		// True when a bundled schema applied and was run. False both when none applied
		// and when the message never got that far.
		bool payload_validated = false;

		std::vector<std::string> validation_errors;
	};

	// What encoding one outbound envelope produced.
	struct OutboundEncodeOutcome {
		bool accepted = false;

		// The text to send. Empty unless accepted.
		std::string message_text;

		std::string envelope_type;

		std::string payload_schema_path;

		bool payload_validated = false;

		std::optional<OutboundEncodeFailureReason> failure_reason;

		std::vector<std::string> validation_errors;
	};

	// The producer-facing sentence for a refusal. Short, because the diagnosable
	// detail is in the rendered failure list beside it.
	constexpr std::string_view describe_inbound_refusal(InboundRefusalReason reason)
	{
		switch (reason) {
			case InboundRefusalReason::malformed_json:
				return "The message could not be read as JSON.";
			case InboundRefusalReason::envelope_schema_violation:
				return "The envelope did not match the vendored envelope schema.";
			case InboundRefusalReason::payload_schema_violation:
				return "The payload did not match the vendored schema for its envelope type.";
		}

		return "";
	}

	// The error code a refusal carries. Unparseable text and a bad wrapper are both
	// "this is not a usable envelope"; a payload failure is its own code because the
	// envelope was fine.
	constexpr std::string_view error_code_for_inbound_refusal(InboundRefusalReason reason)
	{
		switch (reason) {
			case InboundRefusalReason::malformed_json:
			case InboundRefusalReason::envelope_schema_violation:
				return invalid_envelope_error_code;
			case InboundRefusalReason::payload_schema_violation:
				return invalid_payload_error_code;
		}

		return invalid_envelope_error_code;
	}

	// Builds the `error:validation` envelope a refusal is answered with
	// (requirements 4.3, 23.4).
	//
	// The payload is four strings, which is what stream-error.schema.json allows and
	// all any of them needs to be: `validationErrors` is declared a string there, not
	// an array, so the failures are rendered and length-capped rather than carried
	// structured. Both caps are applied here rather than trusted, because an error too
	// long to describe is an error that cannot be sent.
	//
	// The requestId is echoed when the refused envelope carried one, so the server can
	// match the rejection to what it asked for. When it did not, the envelope goes out
	// without one — envelope.schema.json requires `minLength: 1`, so an empty string
	// would fail the wrapper it is meant to report a failure of.
	//
	// The payload of this envelope is not itself validated against a bundled schema:
	// the bundle has no schema registered for `error:validation`, so there is nothing
	// to check it against. It is in bounds by construction instead, which is what the
	// caps and the fixed codes are for.
	template <typename OutboundEnvelopeType>
	OutboundEnvelopeType build_validation_error_envelope(
		InboundRefusalReason reason,
		std::string_view refused_envelope_type,
		std::string_view request_id,
		const std::vector<std::string>& validation_errors
	)
	{
		OutboundEnvelopeType error_envelope;

		error_envelope.type = std::string{validation_error_envelope_type};
		error_envelope.request_id = std::string{request_id};

		error_envelope.payload[std::string{error_code_property}] =
			std::string{error_code_for_inbound_refusal(reason)};

		error_envelope.payload[std::string{error_message_property}] =
			truncate_to_length(describe_inbound_refusal(reason), maximum_error_message_length);

		error_envelope.payload[std::string{error_envelope_type_property}] =
			truncate_to_length(refused_envelope_type, maximum_error_envelope_type_length);

		const std::string rendered_validation_errors =
			render_validation_errors(validation_errors, maximum_rendered_validation_errors_length);

		// Omitted rather than sent empty when there is nothing to render, which is the
		// case for unparseable text. The schema allows the property to be absent and
		// an empty string says less than its absence does.
		if (!rendered_validation_errors.empty()) {
			error_envelope.payload[std::string{error_validation_errors_property}] =
				rendered_validation_errors;
		}

		return error_envelope;
	}

	// Parses, validates, serializes.
	//
	// Templated on the JSON document type — see the file comment. The production
	// instantiation lives in envelope_codec.cpp.
	template <typename JsonType>
	class EnvelopeCodec {
	public:
		using PayloadType = JsonType;

		explicit EnvelopeCodec(PayloadSchemaValidator<JsonType>& payload_validator)
			: payload_validator_{payload_validator}
		{
		}

		EnvelopeCodec(const EnvelopeCodec&) = delete;
		EnvelopeCodec& operator=(const EnvelopeCodec&) = delete;
		EnvelopeCodec(EnvelopeCodec&&) = delete;
		EnvelopeCodec& operator=(EnvelopeCodec&&) = delete;

		// The network thread's entry point, and the reason the main thread never
		// receives text (requirements 4.5, 22.2).
		//
		// One of two things happens and never both: a trusted envelope is pushed onto
		// the inbound queue, or an `error:validation` envelope is pushed onto the
		// outbound queue. The returned outcome is for the log line; the queues are the
		// effect.
		template <typename InboundEnvelopeType, typename OutboundEnvelopeType>
		InboundDecodeOutcome<InboundEnvelopeType> accept_inbound_message(
			std::string_view message_text,
			ConcurrentQueue<InboundEnvelopeType>& inbound_queue,
			ConcurrentQueue<OutboundEnvelopeType>& outbound_queue
		)
		{
			InboundDecodeOutcome<InboundEnvelopeType> outcome =
				decode_inbound<InboundEnvelopeType>(message_text);

			if (outcome.accepted) {
				// Moved out of the outcome, so the envelope the main thread routes and
				// the envelope the caller logs cannot diverge — there is only one, and
				// after this the outcome no longer holds it.
				inbound_queue.push(std::move(*outcome.envelope));
				outcome.envelope.reset();

				return outcome;
			}

			outbound_queue.push(build_validation_error_envelope<OutboundEnvelopeType>(
				*outcome.refusal_reason,
				outcome.envelope_type,
				outcome.request_id,
				outcome.validation_errors
			));

			return outcome;
		}

		// Text in, a trusted envelope or a refusal out. No queues — the sequence is
		// what this is about, and the suite drives it directly.
		//
		// The order is the contract. Nothing is read out of the document until the
		// wrapper schema passed, no payload schema is consulted until the type was
		// read, and the envelope is not constructed until either its payload validated
		// or the type had no bundled schema to validate it against.
		template <typename InboundEnvelopeType>
		InboundDecodeOutcome<InboundEnvelopeType> decode_inbound(std::string_view message_text)
		{
			InboundDecodeOutcome<InboundEnvelopeType> outcome;

			// allow_exceptions false, so malformed text produces a discarded document
			// rather than an exception crossing the Socket.IO callback. The iterator
			// pair overload is used rather than passing the view directly, because it
			// is the overload every version of the library has.
			JsonType document =
				JsonType::parse(message_text.begin(), message_text.end(), nullptr, false);

			if (document.is_discarded()) {
				outcome.refusal_reason = InboundRefusalReason::malformed_json;

				return outcome;
			}

			// Read before validating, and defensively, because the refusal needs to
			// name what it refused. A document whose `type` is a number has nothing to
			// report here, and the empty string is what the error payload then carries.
			outcome.envelope_type = read_string_property(document, envelope_type_property);
			outcome.request_id = read_string_property(document, envelope_request_id_property);

			const SchemaValidationOutcome envelope_validation =
				payload_validator_.validate_against(envelope_schema_path, document);

			if (!envelope_validation.valid) {
				outcome.refusal_reason = InboundRefusalReason::envelope_schema_violation;
				outcome.validation_errors = envelope_validation.errors;

				return outcome;
			}

			// Lifted out of the document once. It is validated and then moved into the
			// envelope, so a large project context snapshot is not copied twice on the
			// network thread for the sake of a tidier-looking call.
			JsonType payload = read_payload(document);

			const std::optional<std::string_view> schema_path =
				inbound_payload_schema_path(outcome.envelope_type);

			// No bundled schema. The envelope is dispatched on its type and read
			// defensively by whichever handler owns it (requirement 4.2). This is the
			// path every `request:<tool_name>` takes, and taking it is requirement 4.4:
			// the tool input schemas are absent from the bundle, the MCP Tool Server
			// already validated the input against the authoritative copy, and a
			// vendored re-check could only reject something valid inside a producer's
			// session.
			if (schema_path.has_value()) {
				outcome.payload_schema_path.assign(*schema_path);

				const SchemaValidationOutcome payload_validation =
					payload_validator_.validate_against(*schema_path, payload);

				outcome.payload_validated = true;

				if (!payload_validation.valid) {
					outcome.refusal_reason = InboundRefusalReason::payload_schema_violation;
					outcome.validation_errors = payload_validation.errors;

					return outcome;
				}
			}

			InboundEnvelopeType envelope;

			envelope.type = outcome.envelope_type;
			envelope.request_id = outcome.request_id;
			envelope.payload = std::move(payload);

			outcome.envelope = std::move(envelope);
			outcome.accepted = true;

			return outcome;
		}

		// An envelope the main thread produced, on its way to the wire.
		//
		// The payload is validated before anything is serialized (requirement 4.1), so
		// a snapshot the Context Builder got wrong fails here with the schema path and
		// the failures named, rather than as a server rejection in the middle of a
		// turn. A failure means nothing is sent: a payload known to be wrong is not
		// improved by the server seeing it.
		//
		// *Which* schema is the envelope's answer when it carried one and this function's
		// otherwise — see `schema_path_for_outbound_envelope` below for why a carried
		// answer wins.
		template <typename OutboundEnvelopeType>
		OutboundEncodeOutcome encode_outbound(const OutboundEnvelopeType& envelope)
		{
			OutboundEncodeOutcome outcome;
			outcome.envelope_type = envelope.type;

			const std::optional<std::string_view> schema_path =
				schema_path_for_outbound_envelope(envelope);

			if (schema_path.has_value()) {
				outcome.payload_schema_path.assign(*schema_path);

				const SchemaValidationOutcome payload_validation =
					payload_validator_.validate_against(*schema_path, envelope.payload);

				outcome.payload_validated = true;

				if (!payload_validation.valid) {
					outcome.failure_reason = OutboundEncodeFailureReason::payload_schema_violation;
					outcome.validation_errors = payload_validation.errors;

					return outcome;
				}
			}

			JsonType document = JsonType::object();

			document[std::string{envelope_type_property}] = envelope.type;
			document[std::string{envelope_payload_property}] = envelope.payload;

			// Omitted rather than sent empty. envelope.schema.json requires
			// `minLength: 1` on requestId and forbids a fourth property, so an empty
			// string would make the extension fail its own wrapper check on every
			// unsolicited message — every snapshot, every prompt.
			if (!envelope.request_id.empty()) {
				document[std::string{envelope_request_id_property}] = envelope.request_id;
			}

			const SchemaValidationOutcome envelope_validation =
				payload_validator_.validate_against(envelope_schema_path, document);

			if (!envelope_validation.valid) {
				outcome.failure_reason = OutboundEncodeFailureReason::envelope_schema_violation;
				outcome.validation_errors = envelope_validation.errors;

				return outcome;
			}

			outcome.message_text = document.dump();
			outcome.accepted = true;

			return outcome;
		}

		// Which schema an outbound envelope's payload is checked against: the answer the
		// envelope carried, or this file's derivation when it carried none.
		//
		// The carried answer wins, and is neither cross-checked against the derivation
		// nor warned about when the two differ, because the case it exists for is exactly
		// the case where the derivation is wrong. A framework `tool_partial_outcome` for
		// `set_item_properties` is indistinguishable from that tool's own success here —
		// both carry `actions`, and `payload_is_tool_refusal` is the only discriminator
		// this side has — so deriving routes the partial to the tool's output schema,
		// which refuses it on `items`. `daw::result_schema_path_for` is the only thing
		// that can tell them apart, it runs on the main thread, and
		// `OutboundEnvelope::payload_schema_path_override` is how its answer crosses the
		// `QueuePair` to get here (requirement 22.2).
		//
		// An override naming a schema the bundle does not hold is a validation failure
		// rather than a silent fallback, which is requirement 4.1's answer and the right
		// one: a producer of envelopes that names the wrong schema is the extension's own
		// bug, and the bundled validator reporting it by path is more diagnosable than a
		// derivation quietly standing in.
		template <typename OutboundEnvelopeType>
		static std::optional<std::string_view> schema_path_for_outbound_envelope(
			const OutboundEnvelopeType& envelope
		)
		{
			// A view into the envelope, which the caller holds by const reference for
			// longer than it holds this.
			if (!envelope.payload_schema_path_override.empty()) {
				return std::string_view{envelope.payload_schema_path_override};
			}

			return outbound_schema_path_for(envelope.type, envelope.payload);
		}

		// Which bundled schema governs an outbound payload: the envelope type's own
		// binding, or — for a `response:<tool_name>` — the refusal contract when the
		// payload carries the `refused` discriminator and the tool's own output schema
		// otherwise.
		//
		// The derivation, with no envelope involved. Public so the Tool Executor can ask
		// the same question the codec will ask, rather than a second copy of the answer —
		// and so a caller that has a type and a payload but no envelope can still ask.
		template <typename PayloadCandidateType>
		static std::optional<std::string_view> outbound_schema_path_for(
			std::string_view envelope_type,
			const PayloadCandidateType& payload
		)
		{
			const std::optional<std::string_view> bound_schema_path =
				outbound_payload_schema_path(envelope_type);

			if (bound_schema_path.has_value()) {
				return bound_schema_path;
			}

			const std::optional<std::string_view> tool_name =
				tool_name_from_response_envelope_type(envelope_type);

			if (!tool_name.has_value()) {
				return std::nullopt;
			}

			return tool_result_schema_path(*tool_name, payload_is_tool_refusal(payload));
		}

		// True when a tool result payload carries the refusal discriminator.
		//
		// Presence, not truth: tool-result-refusal.schema.json declares `refused` a
		// constant true, so a payload carrying `refused: false` is a refusal that got
		// its own discriminator wrong. Reading it as a result instead would validate it
		// against the tool's output schema, which has no `refused` property at all, and
		// report a confusing failure. Selecting the refusal schema reports the actual
		// problem.
		template <typename PayloadCandidateType>
		static bool payload_is_tool_refusal(const PayloadCandidateType& payload)
		{
			return payload.is_object()
				&& payload.contains(std::string{tool_result_refusal_discriminator_property});
		}

	private:
		// A string property, or the empty string when it is absent or not a string.
		// The same defensive read the Transport Handler does on its own payload, and
		// for the same reason: one path for "no usable value" rather than an exception
		// from the JSON library.
		static std::string read_string_property(const JsonType& document, std::string_view property_name)
		{
			const std::string property_key{property_name};

			if (!document.is_object() || !document.contains(property_key)) {
				return std::string{};
			}

			const auto& property_value = document.at(property_key);

			if (!property_value.is_string()) {
				return std::string{};
			}

			return property_value.template get<std::string>();
		}

		// The payload, or an empty object when the document has none.
		//
		// Only reached after the wrapper schema passed, which requires `payload` and
		// requires it to be an object — so the fallback is unreachable in production.
		// It is here because a validator seam that was substituted, or a wrapper schema
		// that stops requiring the property, should not turn into a read of a missing
		// key.
		static JsonType read_payload(const JsonType& document)
		{
			const std::string payload_key{envelope_payload_property};

			if (!document.is_object() || !document.contains(payload_key)) {
				return JsonType::object();
			}

			return document.at(payload_key);
		}

		PayloadSchemaValidator<JsonType>& payload_validator_;
	};

}

#endif
