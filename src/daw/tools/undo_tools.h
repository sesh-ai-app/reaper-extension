// The two rewinding tool handlers (task 10.9, requirements 9.1, 10.5).
//
//   undo_last_action, revert_agent_changes
//
// Both delegate to `undo_manager.h`, which already owns every decision either one
// makes: the range walk, the classification of each entry as the agent's or the
// producer's, the refusal payload, and the guarantee that a refused revert performs no
// undo actions at all. Nothing here re-implements any of it, and nothing here builds a
// refusal — `refusal_from_foreign_undo` in `tool_executor.h` already maps the
// manager's payload.
//
// ---------------------------------------------------------------------------
// Two tools, and the distinction is the producer's
//
// `undo_last_action` is one step: "that was wrong". `revert_agent_changes` is the
// whole turn: "start again". They are not two settings of one tool, and the range
// they check differs accordingly — requirements 10.5 and 10.6 put the range walk on
// "revert all", while a single-step undo is checked against the one entry it would
// actually remove. `undo_manager.h` says why at the declaration of each: applying the
// full-range check to a single step would refuse an undo of the agent's own top block
// because the producer had edited something earlier in the turn, which is work that
// call does not touch.
//
// ---------------------------------------------------------------------------
// Registered through the path that opens no block and captures no marker
//
// `register_rewinding_tool` for both. These tools move the undo stack backwards, so
// wrapping either in an undo block would record an entry describing the removal of
// entries, and capturing a marker before one would point past the producer's own
// work — after which "revert all" would walk back to a position that redoes what was
// just reverted. Both output schemas omit `undoPositionBefore` for exactly that
// reason and report `undoStackPositionAfter` instead, so a result carrying an undo
// report would fail the extension's own outbound validation (requirement 4.1).
//
// It is also the registration call whose body may refuse, which these need:
// requirement 10.5's foreign-entry refusal comes out of
// `undo_manager::revert_agent_changes` as part of the same call that would do the
// reverting, and `register_mutating_tool`'s body type has no refusal alternative.
//
// ---------------------------------------------------------------------------
// The guarantee this file must not undo
//
// Requirement 10.5: a foreign entry anywhere in the turn's range blocks the revert,
// performing no undo actions at all and leaving the undo stack exactly as it was,
// naming the foreign entries using REAPER's own descriptions. The manager makes that
// control flow rather than a promise — it classifies the whole range first and returns
// the refusal before the first `undo_one_entry` call.
//
// So the only way to break it from here is to add an undo call of this file's own, and
// there is none: neither handler touches `undo_stack`, and the only manager methods
// they reach are the two the tools are named after. The suite beside this file counts
// undo calls on a scripted stack rather than trusting that, because "no undo happened"
// is exactly the kind of claim a later refactor can quietly falsify.
//
// ---------------------------------------------------------------------------
// The manager is the executor's own
//
// Both handlers capture a pointer to the `undo_manager` the Tool Executor was
// constructed with, and it has to be that instance rather than another one: the
// turn's undo position marker lives there, and a second manager would have no marker
// at all, so "revert all" would walk an empty range and report a clean revert of
// nothing. One per loaded extension, as `undo_manager.h` says.
//
// ---------------------------------------------------------------------------
// Where the JSON is, and is not
//
// Same split as `routing_tools.h` and `render_tool.h`, with one asymmetry worth
// naming: the codec here writes results and reads nothing. Both tool input schemas
// carry exactly one property, `confirmedForeignUndo`, and the framework already
// extracts it onto `tool_call::confirmed_foreign_undo` — so a reader would be a second
// extraction of one field, which is how a producer's acknowledgement comes to be seen
// by one of two code paths.
//
// Header-only, following the components it wires: the Catch2 target compiles what it
// finds under `tests/` and does not compile `src/`. There is no REAPER adapter here.
// `undo_stack` is the Undo Manager's own seam, implemented once in
// `reaper_undo_stack.cpp`, and a second adapter at this level would be a second copy
// of REAPER's undo calls.

#ifndef SESH_AI_DAW_TOOLS_UNDO_TOOLS_H
#define SESH_AI_DAW_TOOLS_UNDO_TOOLS_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/tool_executor.h>
#include <daw/undo_manager.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// The tool names, as `schema_validator.h`'s binding table spells them.
	inline constexpr std::string_view undo_last_action_tool_name{"undo_last_action"};
	inline constexpr std::string_view revert_agent_changes_tool_name{"revert_agent_changes"};

	// `code` values these handlers produce on a failed action.
	//
	// Neither is a refusal reason. A result nobody could serialise and a REAPER that
	// reported success and moved nothing both name nothing the producer could
	// acknowledge their way past — which is the same reasoning requirement 9.6 gives
	// for an inverted time range.
	inline constexpr std::string_view missing_undo_result_writer_code{"missing_tool_input"};
	inline constexpr std::string_view reaper_undo_failure_code{"reaper_undo_failure"};

	// ---------------------------------------------------------------------------
	// The payload seam
	// ---------------------------------------------------------------------------

	// How each result is written into the payload type.
	//
	// The manager's own outcome types are what the writers take. `revert_outcome` and
	// `undo_last_action_outcome` already match their output schemas field for field,
	// so restating them as a second pair of structs here would be two shapes to keep in
	// step with one schema.
	//
	// Either member may be empty, and an empty one is a failed action carrying a reason
	// rather than a silent success: a producer told their turn was reverted, by a build
	// that could not describe what it reverted, has no way to check.
	template <typename JsonValue>
	struct undo_payload_codec
	{
		std::function<JsonValue(const undo_last_action_outcome&)> write_undo_last_action_result;
		std::function<JsonValue(const revert_outcome&)> write_revert_agent_changes_result;
	};

	// ---------------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------------

	// What `register_undo_tools` did. Reported rather than asserted, so a name the
	// protocol does not know or a handler registered twice is something the extension
	// can say at startup rather than discover one unknown-tool error at a time.
	struct undo_tool_registration
	{
		std::vector<std::string> registered_tool_names;
		std::vector<std::string> rejected_tool_names;

		bool every_tool_registered() const { return rejected_tool_names.empty(); }
	};

	namespace undo_detail
	{
		inline handler_action_outcomes one_failed_action(
			std::string_view target,
			std::string_view failure_code,
			std::string_view failure_message)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, failure_code, failure_message));

			return outcomes;
		}
	}

	// Registers both through `tool_executor.h`'s existing seam.
	//
	// `undo` must be the same `undo_manager` the Tool Executor holds — see the file
	// header — and it, along with the codec's callables, must outlive the registry.
	template <typename JsonValue>
	undo_tool_registration register_undo_tools(
		tool_handler_registry<JsonValue>& registry,
		undo_manager& undo,
		undo_payload_codec<JsonValue> codec)
	{
		undo_tool_registration registration;

		undo_manager* const manager = &undo;

		const auto record = [&registration](std::string_view tool_name, tool_registration_outcome outcome) {
			if (outcome == tool_registration_outcome::registered)
			{
				registration.registered_tool_names.emplace_back(tool_name);
				return;
			}

			registration.rejected_tool_names.emplace_back(tool_name);
		};

		// ------------------------------------------------------------------
		// undo_last_action — one step
		// ------------------------------------------------------------------

		record(
			undo_last_action_tool_name,
			registry.register_rewinding_tool(
				undo_last_action_tool_name,
				[manager, write_result = std::move(codec.write_undo_last_action_result)](
					const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					if (!write_result)
					{
						return undo_detail::one_failed_action(
							undo_last_action_tool_name,
							missing_undo_result_writer_code,
							"the undo result could not be written");
					}

					const undo_last_action_outcome outcome =
						manager->undo_last_action(context.call.confirmed_foreign_undo);

					if (const std::optional<tool_refusal> refusal =
							refusal_from_foreign_undo(outcome.refusal);
						refusal.has_value())
					{
						// The entry this call would have removed is the producer's own,
						// and nothing was undone. Named with REAPER's own description, so
						// what the producer is shown matches their undo history.
						return *refusal;
					}

					// Not performed, and not refused. Two situations reach here and they
					// are not the same answer:
					//
					//   - an empty undo history, where the position after is zero. There
					//     was nothing to undo, which the output schema can say — no
					//     descriptions and a position of zero — and which is a complete
					//     answer rather than a failure.
					//   - REAPER declining to move a stack that had an entry on it. That
					//     is a failed action carrying a reason, because the tool was
					//     asked to do something possible and it did not happen.
					if (!outcome.performed && outcome.undo_stack_position_after > 0)
					{
						return undo_detail::one_failed_action(
							undo_last_action_tool_name,
							reaper_undo_failure_code,
							"REAPER did not undo the entry, and the undo stack is where it was");
					}

					return handler_success<JsonValue>{write_result(outcome)};
				}));

		// ------------------------------------------------------------------
		// revert_agent_changes — the whole turn
		// ------------------------------------------------------------------

		record(
			revert_agent_changes_tool_name,
			registry.register_rewinding_tool(
				revert_agent_changes_tool_name,
				[manager, write_result = std::move(codec.write_revert_agent_changes_result)](
					const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					if (!write_result)
					{
						return undo_detail::one_failed_action(
							revert_agent_changes_tool_name,
							missing_undo_result_writer_code,
							"the revert result could not be written");
					}

					const revert_outcome outcome =
						manager->revert_agent_changes(context.call.confirmed_foreign_undo);

					if (const std::optional<tool_refusal> refusal =
							refusal_from_foreign_undo(outcome.refusal);
						refusal.has_value())
					{
						// Requirement 10.5. The manager classified the whole range and
						// returned before its first undo call, so the stack is exactly as
						// the request found it — and this branch adds no undo of its own,
						// which is the only way this file could take that away.
						return *refusal;
					}

					// A revert that walked nothing back is a complete answer: the turn
					// had made no changes, and `revertedEntryCount` has a minimum of
					// zero for that case.
					//
					// A walk that stopped short of the marker is reported the same way
					// rather than as a failure. The schema's own account of it is the two
					// positions disagreeing — `undoPositionMarker` against
					// `undoStackPositionAfter` — and converting it into a failed action
					// would drop both, leaving the producer told that a partial revert
					// failed without being told how far it got.
					return handler_success<JsonValue>{write_result(outcome)};
				}));

		return registration;
	}
}

#endif
