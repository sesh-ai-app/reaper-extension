// The serialisers that turn the six view models into the camelCase JSON the bridge
// carries (design "The CEF bridge contract", requirements 13.1, 13.5, 15.4, 16.1, 16.2,
// 23.2, 23.7, 3.4).
//
// `ui/ui_host.h` declares the ten message names and the schema each payload answers to.
// This header is the other half of that contract: for each of the six view models, the
// function that produces the payload and the `BridgeMessage` that carries it under the
// name `ui_host.h` already spells. Nothing here invents a message name or a schema path
// — `bridge_message_schema_path_for` is the only answer and it is read, not restated,
// which is the same rule `ToolCallResponse::result_schema_path` follows for a tool
// result.
//
// ---------------------------------------------------------------------------
// Why this writes its own JSON rather than reaching for nlohmann
//
// nlohmann/json is a library dependency of the extension's shared library, and
// `tests/CMakeLists.txt` does not compile `src/` — so a serialiser built on
// `nlohmann::json` is one the Catch2 suite cannot reach, on a machine with no vcpkg it
// is one that cannot be compiled at all, and in both cases the escaping of
// agent-authored conversation text would be defended by nothing. That text crosses into
// a Chromium renderer, so its escaping is a security property rather than a formatting
// one, and the same reasoning that puts `to_javascript_string_literal` in `ui_host.h`
// rather than in the CEF adapter puts this here.
//
// The cost is a JSON writer written by hand, which is only acceptable because of how
// narrow the job is. Every one of the six payloads is a flat object of strings,
// booleans, integers, and — in exactly one field — an array of integers.
// `bridge-messages/*.schema.json` fixes that shape with `additionalProperties: false`,
// so there is no nesting to get wrong, no floating point to round, and no object-valued
// member to recurse into. `JsonObjectWriter` below refuses to be more than that: it has
// no `begin_object`, so a nested payload is not something a future field can quietly
// add here instead of asking whether the schema still describes the bridge.
//
// What that leaves to defend is string escaping, which is `escape_json_string`'s whole
// job and is where this header's tests concentrate.
//
// ---------------------------------------------------------------------------
// Absent is omitted, never null
//
// Five fields across three of the payloads are `std::optional` on the C++ side and
// absent-when-unset in the schema: `resolution`, `answeringClient`, and `localVerdict`
// on the resolution view, `highestSequenceSeen` and `stopReason` on the response view,
// and `lastRejection` on the connection state. None of the six schemas admits `null`
// anywhere — every property names one concrete type — so an unset optional is omitted
// from the object rather than written as `null`, which the schemas would refuse.
//
// The three bridge schemas say what each absence means, and the meanings are not
// interchangeable with an empty string: an absent `stopReason` is a response still in
// flight, while `"stopReason": ""` would be a response that stopped for a reason nobody
// can read.
//
// ---------------------------------------------------------------------------
// The credential
//
// `StreamIngestDisplay::stream_key_plaintext` is a publish token and
// `view:stream_ingest` is the one message that carries it, because the producer
// transcribes it into ReaCast by hand (requirement 16.2). So its serialiser exists and
// is deliberately the only route: there is no "serialise any view model" entry point
// here that a diagnostic could be pointed at by accident, `stream_presenter.h` deletes
// the stream insertion operator for both `StreamKey` and `StreamIngestDisplay`, and
// `StreamIngestLogFields` is the type a log line is written from. Requirement 26.4.

#ifndef SESH_AI_UI_BRIDGE_SERIALISATION_H
#define SESH_AI_UI_BRIDGE_SERIALISATION_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <transport/confirmation_coordinator.h>
#include <transport/transport_client.h>
#include <ui/stream_presenter.h>
#include <ui/ui_host.h>

namespace sesh_ai::ui {

	// ---------------------------------------------------------------------------
	// The writer
	// ---------------------------------------------------------------------------

	// One JSON string, escaped to RFC 8259.
	//
	// Escaped: the quote and the backslash, the five characters with short escapes, and
	// every remaining control character below U+0020 as `\u00XX`. Nothing else is
	// touched — a payload is UTF-8 on both sides of the bridge, so a multi-byte
	// sequence is copied through byte for byte rather than re-encoded.
	//
	// U+2028 and U+2029 are *not* escaped here, and that is worth being explicit about
	// because `to_javascript_string_literal` in `ui_host.h` does escape them. The two
	// functions answer different questions: that one is putting text inside JavaScript
	// source, where those two code points terminate a string literal, and this one is
	// producing a JSON document, where they are ordinary characters. The payload goes
	// through both on its way into the page — this function first, then that one — so
	// escaping them twice would be the one that mangles them.
	inline std::string escape_json_string(std::string_view value)
	{
		static constexpr char hexadecimal_digits[] = "0123456789abcdef";

		std::string escaped;

		// Room for the quotes, plus slack for a handful of escapes before the first
		// reallocation.
		escaped.reserve(value.size() + 2);
		escaped.push_back('"');

		for (const char character : value) {
			switch (character) {
			case '"':
				escaped.append("\\\"");
				continue;
			case '\\':
				escaped.append("\\\\");
				continue;
			case '\b':
				escaped.append("\\b");
				continue;
			case '\f':
				escaped.append("\\f");
				continue;
			case '\n':
				escaped.append("\\n");
				continue;
			case '\r':
				escaped.append("\\r");
				continue;
			case '\t':
				escaped.append("\\t");
				continue;
			default:
				break;
			}

			const auto code = static_cast<unsigned char>(character);

			if (code < 0x20) {
				escaped.append("\\u00");
				escaped.push_back(hexadecimal_digits[(code >> 4) & 0x0f]);
				escaped.push_back(hexadecimal_digits[code & 0x0f]);
				continue;
			}

			escaped.push_back(character);
		}

		escaped.push_back('"');

		return escaped;
	}

	// Builds one flat JSON object.
	//
	// Deliberately incapable of nesting. There is no operation that opens a child
	// object, so the six payloads cannot grow a nested member without someone adding
	// one here — and a nested member is a change to
	// `bridge-messages/*.schema.json`, which is generated and digest-verified, so the
	// conversation it forces is the right one.
	//
	// The comma is the writer's business rather than each caller's: a serialiser that
	// omits an absent optional is exactly the shape that gets a trailing or doubled
	// comma wrong, and there are six of them.
	class JsonObjectWriter {
	public:
		JsonObjectWriter()
		{
			document_.push_back('{');
		}

		void write_string(std::string_view field_name, std::string_view value)
		{
			begin_field(field_name);
			document_.append(escape_json_string(value));
		}

		void write_boolean(std::string_view field_name, bool value)
		{
			begin_field(field_name);
			document_.append(value ? "true" : "false");
		}

		void write_integer(std::string_view field_name, long long value)
		{
			begin_field(field_name);
			document_.append(std::to_string(value));
		}

		// `std::size_t` through the same path, so a count does not have to be cast at
		// every call site. Every count the six payloads carry is bounded by what the
		// presenter accumulated in one turn, so the narrowing cannot bite in practice —
		// and the schemas put `minimum: 0` on all of them, which is what a signed
		// serialisation would otherwise be able to violate.
		void write_count(std::string_view field_name, std::size_t value)
		{
			write_integer(field_name, static_cast<long long>(value));
		}

		void write_integer_array(std::string_view field_name, const std::vector<long long>& values)
		{
			begin_field(field_name);
			document_.push_back('[');

			bool first_element = true;

			for (const long long value : values) {
				if (!first_element) {
					document_.push_back(',');
				}

				document_.append(std::to_string(value));
				first_element = false;
			}

			document_.push_back(']');
		}

		// Absent is omitted. See the file comment — none of the six schemas admits
		// `null`.
		void write_optional_string(
			std::string_view field_name,
			const std::optional<std::string>& value)
		{
			if (value.has_value()) {
				write_string(field_name, *value);
			}
		}

		void write_optional_integer(
			std::string_view field_name,
			const std::optional<long long>& value)
		{
			if (value.has_value()) {
				write_integer(field_name, *value);
			}
		}

		// The document, closed. Taken by value because a writer is used once.
		std::string finish()
		{
			document_.push_back('}');

			return std::move(document_);
		}

		std::size_t written_field_count() const noexcept { return written_field_count_; }

	private:
		void begin_field(std::string_view field_name)
		{
			if (written_field_count_ > 0) {
				document_.push_back(',');
			}

			document_.append(escape_json_string(field_name));
			document_.push_back(':');

			++written_field_count_;
		}

		std::string document_;
		std::size_t written_field_count_ = 0;
	};

	// ---------------------------------------------------------------------------
	// The enumerations, in the spelling the schemas enumerate
	// ---------------------------------------------------------------------------

	// `ResponseStreamState` is the one view-model enumeration with no `to_schema_string`
	// of its own — `confirmation_coordinator.h` carries one for all four of its
	// enumerations and `transport_client.h` has `describe_connection_state`,
	// `describe_producer_action`, and `describe_handshake_rejection`, each of which
	// already answers in the schema's spelling. This is the gap, filled here rather
	// than in `stream_presenter.h` so that the one component that needs the schema
	// spelling is the one that carries it.
	//
	// Total over the three enumerators. An empty answer would be a `state` the schema
	// refuses, which is the right failure for a value this build does not know, but
	// there is no such value — the enumeration is closed and the switch is exhaustive.
	constexpr std::string_view to_schema_string(ResponseStreamState state)
	{
		switch (state) {
		case ResponseStreamState::idle:
			return "idle";
		case ResponseStreamState::streaming:
			return "streaming";
		case ResponseStreamState::ended:
			return "ended";
		}

		return "";
	}

	// ---------------------------------------------------------------------------
	// The six payloads
	// ---------------------------------------------------------------------------

	// `bridge-messages/confirmation-pending.schema.json`. All seven fields are
	// required, so none is conditional.
	inline std::string serialise_pending_confirmation_view(
		const transport::PendingConfirmationView& pending_confirmation)
	{
		JsonObjectWriter writer;

		writer.write_string("requestId", pending_confirmation.request_id);
		writer.write_string("actionSummary", pending_confirmation.action_summary);
		writer.write_string("details", pending_confirmation.details);
		writer.write_string("riskLevel", transport::to_schema_string(pending_confirmation.risk_level));
		writer.write_boolean("riskLevelReported", pending_confirmation.risk_level_reported);
		writer.write_integer(
			"confirmationWindowSeconds",
			pending_confirmation.confirmation_window_seconds
		);
		writer.write_boolean("descriptionComplete", pending_confirmation.description_complete);

		return writer.finish();
	}

	// `bridge-messages/confirmation-resolved.schema.json`. Three of the seven fields are
	// optional, and each absence is a statement the UI reads — see the view model's own
	// comments.
	inline std::string serialise_confirmation_resolution_view(
		const transport::ConfirmationResolutionView& resolution)
	{
		JsonObjectWriter writer;

		writer.write_string("requestId", resolution.request_id);

		if (resolution.resolution.has_value()) {
			writer.write_string("resolution", transport::to_schema_string(*resolution.resolution));
		}

		if (resolution.answering_client.has_value()) {
			writer.write_string(
				"answeringClient",
				transport::to_schema_string(*resolution.answering_client)
			);
		}

		writer.write_boolean("answeredByThisClient", resolution.answered_by_this_client);

		if (resolution.local_verdict.has_value()) {
			writer.write_string("localVerdict", transport::to_schema_string(*resolution.local_verdict));
		}

		writer.write_boolean("contradictsLocalVerdict", resolution.contradicts_local_verdict);
		writer.write_boolean("nothingWasChanged", resolution.nothing_was_changed);

		return writer.finish();
	}

	// `bridge-messages/agent-response.schema.json`.
	inline std::string serialise_response_view(const ResponseView& response)
	{
		JsonObjectWriter writer;

		writer.write_boolean("active", response.active);
		writer.write_string("conversationId", response.conversation_id);
		writer.write_string("turnId", response.turn_id);
		writer.write_string("messageId", response.message_id);
		writer.write_string("state", to_schema_string(response.state));
		writer.write_string("text", response.text);
		writer.write_boolean("openedImplicitly", response.opened_implicitly);
		writer.write_count("appliedDeltaCount", response.applied_delta_count);
		writer.write_optional_integer("highestSequenceSeen", response.highest_sequence_seen);
		writer.write_integer_array("missingSequences", response.missing_sequences);
		writer.write_boolean("textMayBeIncomplete", response.text_may_be_incomplete);
		writer.write_optional_string("stopReason", response.stop_reason);
		writer.write_boolean(
			"reconciledWithAssembledText",
			response.reconciled_with_assembled_text
		);

		return writer.finish();
	}

	// `bridge-messages/stream-ingest.schema.json`. Carries the publish token — see the
	// file comment.
	inline std::string serialise_stream_ingest_display(const StreamIngestDisplay& ingest)
	{
		JsonObjectWriter writer;

		writer.write_string("stageArn", ingest.stage_arn);
		writer.write_string("rtmpsIngestUrl", ingest.rtmps_ingest_url);
		writer.write_string("streamKeyPlaintext", ingest.stream_key_plaintext);
		writer.write_string("participantId", ingest.participant_id);
		writer.write_string("expiresAt", ingest.expires_at);
		writer.write_boolean("reissued", ingest.reissued);

		return writer.finish();
	}

	// `bridge-messages/stream-error.schema.json`. All four fields are required and all
	// four may legitimately be empty: an `error:*` envelope has no bundled payload
	// schema, so it folds into this view with whatever it carried and nothing else.
	inline std::string serialise_stream_error_view(const StreamErrorView& error)
	{
		JsonObjectWriter writer;

		writer.write_string("code", error.code);
		writer.write_string("message", error.message);
		writer.write_string("envelopeType", error.envelope_type);
		writer.write_string("validationErrors", error.validation_errors);

		return writer.finish();
	}

	// `bridge-messages/connection-state.schema.json`.
	//
	// `reconnectionDelayMilliseconds` names its unit because the C++ member is a
	// `std::chrono::milliseconds` and a bare number arriving in TypeScript has no unit
	// at all — the schema makes the same point. `count()` is the only conversion, so
	// nothing here can change the unit by accident.
	//
	// There is no field for a token, an expiry, or any part of one, and the schema's
	// `additionalProperties: false` is the second half of that: `connection_status`
	// carries no token material, this writes every field it does carry, and the two
	// assertions meet.
	inline std::string serialise_connection_status(const transport::connection_status& status)
	{
		JsonObjectWriter writer;

		writer.write_string("state", transport::describe_connection_state(status.state));
		writer.write_count("consecutiveFailedAttempts", status.consecutive_failed_attempts);
		writer.write_integer(
			"reconnectionDelayMilliseconds",
			static_cast<long long>(status.reconnection_delay.count())
		);
		writer.write_string(
			"requiredProducerAction",
			transport::describe_producer_action(status.required_producer_action)
		);
		writer.write_string("notice", status.notice);

		if (status.last_rejection.has_value()) {
			writer.write_string(
				"lastRejection",
				transport::describe_handshake_rejection(*status.last_rejection)
			);
		}

		return writer.finish();
	}

	// ---------------------------------------------------------------------------
	// The messages
	// ---------------------------------------------------------------------------

	// One overload per view model, each pairing its payload with the name
	// `ui_host.h` declares for it.
	//
	// Overloads rather than six differently-named factories, and that is the point of
	// them: the view model's type is what selects the message name, so a confirmation
	// resolution cannot be published under the pending confirmation's name. The names
	// appear exactly once each, here, and come from the constants rather than from a
	// string literal.
	inline BridgeMessage bridge_message_for(
		const transport::PendingConfirmationView& pending_confirmation)
	{
		return bridge_message(
			std::string{pending_confirmation_bridge_message_name},
			serialise_pending_confirmation_view(pending_confirmation)
		);
	}

	inline BridgeMessage bridge_message_for(
		const transport::ConfirmationResolutionView& resolution)
	{
		return bridge_message(
			std::string{confirmation_resolution_bridge_message_name},
			serialise_confirmation_resolution_view(resolution)
		);
	}

	inline BridgeMessage bridge_message_for(const ResponseView& response)
	{
		return bridge_message(
			std::string{agent_response_bridge_message_name},
			serialise_response_view(response)
		);
	}

	inline BridgeMessage bridge_message_for(const StreamIngestDisplay& ingest)
	{
		return bridge_message(
			std::string{stream_ingest_bridge_message_name},
			serialise_stream_ingest_display(ingest)
		);
	}

	inline BridgeMessage bridge_message_for(const StreamErrorView& error)
	{
		return bridge_message(
			std::string{stream_error_bridge_message_name},
			serialise_stream_error_view(error)
		);
	}

	inline BridgeMessage bridge_message_for(const transport::connection_status& status)
	{
		return bridge_message(
			std::string{connection_state_bridge_message_name},
			serialise_connection_status(status)
		);
	}

	// Every one of the six names a view model, and every one of the six resolves to a
	// `bridge-messages/` schema. Both halves are asserted here rather than left to the
	// serialisers: a payload published under a name the UI does not listen for is
	// silence, which is the failure `ui_host.h`'s contract comment exists to prevent.
	static_assert(
		is_view_model_bridge_message(pending_confirmation_bridge_message_name)
			&& is_view_model_bridge_message(confirmation_resolution_bridge_message_name)
			&& is_view_model_bridge_message(agent_response_bridge_message_name)
			&& is_view_model_bridge_message(stream_ingest_bridge_message_name)
			&& is_view_model_bridge_message(stream_error_bridge_message_name)
			&& is_view_model_bridge_message(connection_state_bridge_message_name),
		"every serialised payload must be published under a view model name"
	);

	static_assert(
		bridge_message_direction_for(pending_confirmation_bridge_message_name)
				== BridgeMessageDirection::to_javascript
			&& bridge_message_direction_for(confirmation_resolution_bridge_message_name)
				== BridgeMessageDirection::to_javascript
			&& bridge_message_direction_for(agent_response_bridge_message_name)
				== BridgeMessageDirection::to_javascript
			&& bridge_message_direction_for(stream_ingest_bridge_message_name)
				== BridgeMessageDirection::to_javascript
			&& bridge_message_direction_for(stream_error_bridge_message_name)
				== BridgeMessageDirection::to_javascript
			&& bridge_message_direction_for(connection_state_bridge_message_name)
				== BridgeMessageDirection::to_javascript,
		"a serialised view model travels to JavaScript and never the other way"
	);

}

#endif
