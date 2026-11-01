// `write_dispatch_outcome` — one dispatched tool call, as the outbound envelope carries
// it (task 24.2, requirements 4.1, 9.2, 9.4, 20.1).
//
// `ToolCallPayloadCodec::write_dispatch_outcome` (`src/entry/extension_composition.h:125`)
// is the function this file implements, and it is deliberately one function over every
// shape rather than one per shape. The comment at that declaration says why: the shape is
// a variant the caller visits, and splitting it would put the same `std::visit` in two
// places.
//
// ---------------------------------------------------------------------------
// Four shapes, and only one of them belongs to a tool
//
// `tool_executor.h` closes the result set at three and wraps it in a fourth for the case
// where there is no tool at all:
//
//   | Shape   | Payload built by          | Validates against                        |
//   | Success | the handler, through the  | that tool's own output schema            |
//   |         | writers in               |                                          |
//   |         | tool_result_writers.h    |                                          |
//   | Refusal | this file                | messages/tool-result-refusal.schema.json |
//   | Partial | this file                | messages/tool-result-partial.schema.json |
//   | Unknown | this file                | nothing — there is no tool, so there are |
//   |         |                          | no tool-specific fields and no schema    |
//
// So the success case is a merge rather than a write: the per-tool writer already
// produced the tool-specific fields and this file adds the undo report the *framework*
// holds. The other three are written here in full, because the tool whose name is on the
// envelope had no say in any of them.
//
// ---------------------------------------------------------------------------
// This is the only place in the codebase that writes the undo report
//
// `messages/tool-result-undo.schema.json` declares two fields and the bundler hoists them
// into every mutating tool's output schema, so thirty-odd of the 42 payloads carry
// `undoPositionBefore` and `undoDescription`. None of the per-tool writers emits either,
// and no result struct holds either, because neither is the tool's to report: the turn's
// marker lives in `undo_manager`, and whether a result carries one is decided by three
// rules that are all `tool_executor.h`'s.
//
//   - A refusal has nowhere to put one. `tool_refusal` has no undo field at all, which is
//     requirement 9.3 and requirement 10.7 expressed as a type rather than as a rule
//     somebody maintains.
//   - A success carries one unless the handler said nothing landed
//     (`handler_success::nothing_was_applied`) or the undo block failed to close. Both
//     arrive here as an absent `tool_success::undo`, and absence is the signal the two
//     action-array schemas name in their own descriptions: a result without
//     `undoPositionBefore` is a call in which nothing changed.
//   - A partial carries one when any action was applied, which the framework can count.
//
// Every one of those decisions is already made by the time an outcome reaches this file.
// What is left is to emit the fields when the optional is populated and to leave them out
// when it is not — which is why there is no branch here on which tool it was.
//
// ---------------------------------------------------------------------------
// Which schema the payload is validated against, and why that answer has to travel
//
// Requirement 4.1 validates every outbound payload, and for a tool result the schema
// cannot be worked out from the payload. A framework `tool_partial_outcome` carries
// `actions` and nothing else; so does `set_item_properties`' own success, plus `items`.
// From a payload and a tool name the two are indistinguishable, so
// `EnvelopeCodec::outbound_schema_path_for` — which is all the network thread has —
// routes the framework partial to the named tool's output schema and it is refused on
// fields the framework cannot supply. The producer sees the tool silently not working.
//
// `daw::result_schema_path_for` is the one function that can answer, because it reads the
// variant rather than the JSON, and `OutboundEnvelope::payload_schema_path_override`
// (task 22.2) is how its answer crosses the `QueuePair` to the thread that validates.
// `ExecutorToolCallRouter` asks and `QueuedToolResultSink` assigns; this file writes the
// payload that answer describes.
//
// `written_outcome_schema_path` below is the pairing stated once, in the file that builds
// the payload: for any outcome, the schema its written payload is to be validated
// against. It is `result_schema_path_for` rather than a second derivation, deliberately —
// a writer and an override that disagreed would be the same bug in a new place — and it
// exists so that "this payload validates against the schema the override will name" is
// one expression the suite can check for all four shapes rather than a correspondence
// held together by two call sites.
//
// Header-only and templated on the document type, like the rest of this seam.

#ifndef SESH_AI_TRANSPORT_TOOL_OUTCOME_WRITER_H
#define SESH_AI_TRANSPORT_TOOL_OUTCOME_WRITER_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <daw/object_resolver.h>
#include <daw/tool_executor.h>
#include <daw/undo_manager.h>
#include <transport/tool_payload_writing.h>
#include <transport/tool_result_writers.h>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// `messages/tool-result-undo.schema.json`, hoisted into every mutating tool's output
	// schema by the bundler.
	inline constexpr std::string_view undo_position_before_property{"undoPositionBefore"};
	inline constexpr std::string_view undo_description_property{"undoDescription"};

	// `messages/tool-result-refusal.schema.json`.
	inline constexpr std::string_view refusal_discriminator_property{"refused"};
	inline constexpr std::string_view refusal_reason_property{"reason"};
	inline constexpr std::string_view refusal_blocking_property{"blocking"};
	inline constexpr std::string_view refusal_acknowledgement_field_property{
		"acknowledgementField"};

	// `messages/tool-result-partial.schema.json`.
	inline constexpr std::string_view partial_actions_property{"actions"};

	// The unknown-tool error (requirement 23.6), which has no bundled schema because it
	// is not a tool result — there is no tool, so there are no tool-specific fields and
	// `tool_output_schema_path` answers with nothing for the name.
	//
	// `code` and `message` are spelled to match the two properties every other error
	// payload in this protocol carries, which `envelope_codec.h` names as
	// `error_code_property` and `error_message_property`. Those constants are not reached
	// for here: `envelope_codec.h` includes `envelope.h`, which names `nlohmann::json`,
	// and this header is templated on the document type precisely so the suite can drive
	// it without one.
	inline constexpr std::string_view unknown_tool_name_property{"tool"};
	inline constexpr std::string_view unknown_tool_code_property{"code"};
	inline constexpr std::string_view unknown_tool_message_property{"message"};

	// `acknowledgementField` is `maxLength: 128`.
	inline constexpr std::size_t maximum_written_acknowledgement_field_length = 128;

	// ---------------------------------------------------------------------------
	// The undo report
	// ---------------------------------------------------------------------------

	// An undo stack position, as the schema's `minimum: 0` requires.
	//
	// `undo_manager` already clamps at every place it reads `current_position`, and this
	// is the backstop rather than a second policy: the manager's own answer for a stack
	// position below zero is zero, which reads as the start of the session. A negative
	// position emitted as-is would fail the payload's own validation, and a mutating call
	// that happened and could not be reported is the worst of the available outcomes.
	inline int non_negative_written_position(int position)
	{
		return position < 0 ? 0 : position;
	}

	// Adds `undoPositionBefore` and `undoDescription` to a payload that already holds the
	// tool's own fields.
	//
	// Does nothing when the result carried no undo report, which is the case for a read
	// tool, for `render`, for the two rewinding tools, and for a mutating tool whose
	// handler said nothing landed. Three of those four have output schemas that are closed
	// and name neither field, so adding one would fail validation; the fourth is the two
	// action-array tools, where the schema permits both and absence is what tells the
	// producer nothing changed.
	//
	// `undoDescription` is optional with `minLength: 1`, so an empty description is an
	// absence rather than an empty string. The description is `"Sesh AI: <tool_name>"`
	// composed by `undo_manager`, which substitutes for an unnamed tool, so this is the
	// backstop for a report that reached here without one.
	template <typename JsonValue>
	void add_undo_report(JsonValue& payload, const std::optional<daw::undo_report>& undo)
	{
		if (!undo.has_value() || !payload.is_object()) {
			return;
		}

		payload[std::string{undo_position_before_property}] =
			non_negative_written_position(undo->undo_position_before);

		if (undo->undo_description.empty()) {
			return;
		}

		payload[std::string{undo_description_property}] = within_written_bounds(
			undo->undo_description,
			std::string_view{},
			daw::maximum_undo_description_length);
	}

	// ---------------------------------------------------------------------------
	// The three shapes this file writes in full
	// ---------------------------------------------------------------------------

	// A refusal: a precondition the producer can resolve, naming what is in the way and —
	// where one exists — the field to set on the retry.
	//
	// `refused` is written as the schema constant it is rather than from a flag, because
	// `ToolResultRefusal::refused` is a `static constexpr bool` and there is no settable
	// one to get wrong.
	//
	// `reason` travels unchanged. The schema closes it at five values and each of the four
	// components that can refuse already chose one of them from its own named constant —
	// `object_resolver.h`'s two, `render_coordinator.h`'s collision, `cycle_detector.h`'s
	// cycle, `undo_manager.h`'s foreign entries. A value outside the five is that
	// component's bug and fails the outbound validation with the field named, which is
	// more diagnosable than a writer quietly substituting a reason nobody decided.
	template <typename JsonValue>
	JsonValue write_tool_refusal(const daw::tool_refusal& refusal)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_constant(
			refusal_discriminator_property,
			daw::tool_refusal::refused);
		writer.set_string(
			refusal_reason_property,
			refusal.reason,
			maximum_written_name_length);

		// Required, and emitted even when empty. What is in the way is the whole content
		// of a refusal: requirement 7.4 has every candidate of an ambiguous selector
		// named, requirement 9.8 has every colliding path enumerated, and requirement 10.5
		// has every foreign undo entry named using REAPER's own description. An empty array
		// is a refusal that named nothing, which is worth being visible rather than
		// omitted.
		writer.set_value(
			refusal_blocking_property,
			write_array_of<JsonValue>(refusal.blocking, [](const daw::BlockingEntity& blocking) {
				ToolOutputWriter<JsonValue> entity;

				entity.set_string("kind", blocking.kind, maximum_written_name_length);
				entity.set_named_string(
					"description",
					blocking.description,
					"an unnamed blocking entity",
					daw::maximum_blocking_description_length);

				// Optional, and omitted when empty rather than emitted as an empty string:
				// the schema's `guid` pattern would reject one, and a file path or an undo
				// entry has no GUID to report. `BlockingEntity` says so at its own
				// declaration.
				entity.set_optional_string("guid", blocking.guid, maximum_written_name_length);

				return entity.release();
			}));

		// Optional, and absent is meaningful: a pattern matching no track names no
		// acknowledgement, because acknowledging it would not make the track exist.
		// `object_resolver.h` makes exactly that point about the unresolved selector
		// refusal, which is the one of the five that omits this.
		writer.set_optional_string(
			refusal_acknowledgement_field_property,
			refusal.acknowledgement_field,
			maximum_written_acknowledgement_field_length);

		return writer.release();
	}

	// An action array, reported per action (requirement 9.4).
	//
	// Always the framework's shape and never a tool's own, whatever `tool_name` says — the
	// two tools whose output schemas carry `actions` build their whole payload themselves
	// and come back as a success. So this payload holds `actions`, the undo report when
	// anything landed, and nothing else, and it validates against the partial contract
	// rather than against the named tool's output schema.
	//
	// Requirement 9.4's no-rollback rule is visible here as what is *not* done: the
	// succeeded actions are reported as succeeded alongside their failed siblings, because
	// nothing rolled them back. Requirement 9.5 is visible as what cannot be written — a
	// failed action's `error` comes from `daw::action_error`, which has no state in which
	// its code or message is empty, so a reasonless failure is not a payload this function
	// could produce.
	template <typename JsonValue>
	JsonValue write_tool_partial_outcome(const daw::tool_partial_outcome<JsonValue>& partial)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			partial_actions_property,
			write_action_outcomes<JsonValue>(partial.actions));

		JsonValue payload = writer.release();

		add_undo_report(payload, partial.undo);

		return payload;
	}

	// Requirement 23.6. Not a tool result: no tool ran, so there are no tool-specific
	// fields and no output schema for a payload to be checked against.
	//
	// The tool's name is carried separately from the message because the server reads one
	// and the producer reads the other, and a missing implementation being *visible* is
	// the whole of what the requirement asks for — silence here is how a tool the server
	// knows about and this build does not becomes a turn that appears to do nothing.
	template <typename JsonValue>
	JsonValue write_unknown_tool_error(const daw::unknown_tool_error& error)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string(
			unknown_tool_name_property,
			error.tool_name,
			maximum_written_name_length);
		writer.set_string(
			unknown_tool_code_property,
			error.code,
			maximum_written_name_length);
		writer.set_string(
			unknown_tool_message_property,
			error.message,
			daw::maximum_action_error_message_length);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// The whole outcome
	// ---------------------------------------------------------------------------

	// The signature `ToolCallPayloadCodec::write_dispatch_outcome` holds.
	//
	// One `std::visit` over the result shapes, and the success branch is the merge the
	// file header describes: the handler's fields as the per-tool writer produced them,
	// plus the framework's undo report. The fields are moved out of a copy rather than
	// read through the variant, because `tool_success::fields` is the tool's own payload
	// and this function has no business rebuilding it.
	//
	// A success whose `fields` is not an object is passed through as it stands. That is a
	// handler or a codec that produced no payload, and the honest report of it is the
	// outbound validation failing by path — substituting an empty object would produce a
	// result that looks like a tool reporting nothing rather than a build that could not
	// write one.
	template <typename JsonValue>
	JsonValue write_dispatch_outcome(const daw::dispatch_outcome<JsonValue>& outcome)
	{
		if (const daw::unknown_tool_error* const unknown =
				std::get_if<daw::unknown_tool_error>(&outcome)) {
			return write_unknown_tool_error<JsonValue>(*unknown);
		}

		const daw::tool_result<JsonValue>& result =
			std::get<daw::tool_result<JsonValue>>(outcome);

		if (const daw::tool_refusal* const refusal =
				std::get_if<daw::tool_refusal>(&result)) {
			return write_tool_refusal<JsonValue>(*refusal);
		}

		if (const daw::tool_partial_outcome<JsonValue>* const partial =
				std::get_if<daw::tool_partial_outcome<JsonValue>>(&result)) {
			return write_tool_partial_outcome<JsonValue>(*partial);
		}

		const daw::tool_success<JsonValue>& success =
			std::get<daw::tool_success<JsonValue>>(result);

		JsonValue payload = success.fields;

		add_undo_report(payload, success.undo);

		return payload;
	}

	// Which bundled schema the payload above is to be validated against, which is also the
	// value `OutboundEnvelope::payload_schema_path_override` carries for this outcome.
	//
	// `daw::result_schema_path_for`'s answer, forwarded rather than re-derived. Stating it
	// here is what makes the writer and the override one claim instead of two: a test can
	// write an outcome's payload and validate it against this, and a disagreement between
	// the shape written and the schema named shows up as a failed validation rather than
	// as a payload the server rejects in a producer's session.
	//
	// Empty for an unknown tool, and for a success naming something outside the 42. Both
	// are correct rather than a failure — there is no bundled schema for either, which is
	// what requirement 23.6 means by reporting the gap rather than refusing.
	template <typename JsonValue>
	std::string written_outcome_schema_path(const daw::dispatch_outcome<JsonValue>& outcome)
	{
		const std::optional<std::string_view> schema_path =
			daw::result_schema_path_for(outcome);

		return schema_path.has_value() ? std::string{*schema_path} : std::string{};
	}

}

#endif
