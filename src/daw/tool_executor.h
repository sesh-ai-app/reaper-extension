// Tool Executor — dispatch, target resolution, undo block wrapping, and the three
// result shapes (requirement 9, design "Tool Executor").
//
// This file is the framework and nothing else. The 39 tool handlers arrive in tasks
// 10.3 through 10.9 and plug into the registration seam below; what lives here is the
// part every one of them shares, and the part where getting it wrong is expensive:
// which shapes a result can take, when an undo block is opened, when a marker is
// recorded, and what happens to the block when a REAPER call fails halfway through
// it.
//
// ---------------------------------------------------------------------------
// The result set is closed at three
//
// Requirement 9.2, 9.3, and 9.4 name three outcomes and the design tabulates them:
//
//   | Outcome | Shape                                            | Undo marker        |
//   | Success | tool-specific fields plus the undo report         | unless the handler |
//   |         |                                                  | says nothing landed|
//   | Refusal | the refusal payload, closed, no extra fields      | NOT                |
//   | Partial | per-action outcomes plus the undo report          | if anything landed |
//
// Modelled as a `std::variant`, for the reason `object_resolver.h` gives for its own
// three outcomes: a struct with a status field and three optional payloads can hold a
// status saying "success" next to a populated refusal, which is a fourth outcome
// nobody designed and which one forgotten assignment reaches. Naming a variant
// alternative is the only way to construct a result, and reading one means handling
// all three.
//
// An unknown tool name (requirement 23.6) is deliberately *not* a fourth alternative.
// It is not a tool result at all — there is no tool, so there are no tool-specific
// fields, and `transport::tool_output_schema_path` returns nothing for the name, so
// there is no schema for a payload to validate against. It is an envelope-level error
// naming the tool, and `dispatch_outcome` is the variant that says so: either a tool
// result (one of exactly three shapes) or the error that means no tool ran.
//
// ---------------------------------------------------------------------------
// Each shape validates against the contract that describes *it*, not against the tool
//
// Requirement 4.1 has the extension validate its own outbound payloads, so each of the
// three shapes has to name a schema, and the answer is not "the tool's own output
// schema" for all three. Two of them are shapes this framework fills in, and the tool
// whose name is on the envelope had no say in either:
//
//   | Shape   | Payload built by | Validates against                          |
//   | Success | the handler      | that tool's own output schema               |
//   | Refusal | this framework   | messages/tool-result-refusal.schema.json    |
//   | Partial | this framework   | messages/tool-result-partial.schema.json    |
//
// The partial row is the one that is easy to get wrong, because two different things
// are called "partial" and only one of them is `tool_partial_outcome`.
//
// `set_item_properties` and `set_track_state` — the two `schema_validator.h` names —
// carry `actions` in their *own* output schemas, so an action-by-action report really
// is their ordinary result. But neither returns `handler_action_outcomes` to do it, and
// neither can: `set-item-properties.schema.json` requires `actions` and `items`
// together, `set-track-state.schema.json` requires `actions` and `aliasesLearned`
// together, and both are closed with `additionalProperties: false`.
// `handler_action_outcomes` has nowhere to hold the sibling field. So both tools build
// the whole payload themselves and return `handler_success`, which is what makes every
// `tool_partial_outcome` that exists this framework's shape and never a tool's own.
//
// That is what makes the row above answerable without asking which tool it was. A
// framework partial carries `actions` and nothing else — no `tracks`, no `items`, no
// `truncated` — so routing it to the tool's own output schema refuses it on the fields
// the tool requires and this framework cannot supply. A read tool meets this every time
// it fails, because a failure's only shape is `handler_action_outcomes`:
// `list-tracks.schema.json` requires `tracks`, `list-track-fx.schema.json` requires
// `track` and `fx`, `list-selected-items.schema.json` requires four fields including
// `truncated`, and `get-fx-parameters.schema.json` requires six. Each of those is a
// payload refused by the extension's own outbound validation, which a producer
// experiences as the tool silently not working.
//
// `result_schema_path_for` below is therefore the only place that answers this, and it
// does not delegate the partial case to `transport::tool_result_schema_path`. That
// function answers from a payload and a tool name, which is all the Envelope Codec ever
// has, and from there a framework partial for `set_item_properties` is
// indistinguishable from that tool's own result — so it cannot answer, and the comment
// on it saying `actions` is not a discriminator is right about the codec's problem. This
// framework built the payload, so it knows which shape it is. Anything that encodes a
// result should carry this answer rather than re-deriving one from the JSON.
//
// ---------------------------------------------------------------------------
// An outcome array is never cut to fit
//
// `tool-result-partial.schema.json` caps `actions` at 512 — `maximum_action_outcomes`
// below — while `set_item_properties` accepts 4096 changes, the item tools accept 4096
// GUIDs, and `change_tempo_map` accepts 1024 points. So a handler can produce more
// outcomes than one result can carry, and requirements 9.5 and 23.9 both forbid the
// obvious response: an array quietly shortened to fit is actions that happened with
// nothing said about them, which is the same silence 23.9 forbids of a capped read.
//
// One rule, in two halves, and which half applies is decided by whether the count is
// knowable before anything is touched:
//
//   - **Knowable: refuse before touching.** A handler reporting one outcome per entry
//     of an input array knows the count from the input. It reports a single failed
//     action naming the cap and changes nothing, so the agent retries with fewer
//     targets against an untouched session. `set_item_properties` has no choice here —
//     its *own* output schema caps `actions` at 512 while its input allows 4096 — and
//     the item tools and `set_track_state` follow it.
//
//   - **Not knowable: report what fits and name what does not.** `change_tempo_map`
//     reports aggregate counts on success, so 1024 points are perfectly reportable, and
//     refusing the call up front would refuse a call the contract allows and the tool
//     can answer. The cap only binds when a point fails and the result falls back to
//     per-action outcomes, which is not knowable from the input. `build_partial_outcome`
//     then keeps the outcomes in the order they were supplied, stops one short of the
//     cap, and spends the last slot on a failed action naming how many outcomes are not
//     reported.
//
// The second half lives here rather than in a handler so that a handler which gets the
// arithmetic wrong, or which never considered the cap, still cannot emit a silently
// shortened array. It is a backstop, not a strategy: a handler that can refuse should.
//
// ---------------------------------------------------------------------------
// A refusal cannot record an undo block, structurally
//
// Requirement 9.3 and requirement 10.7 together say a refused mutation records no
// undo position marker, because a marker recorded mid-turn by something that changed
// nothing sits above the agent's earlier work and silently shortens the range "revert
// all" would walk. `undo_manager` already binds marker capture to opening a block, so
// the only way to break this is to open a block and *then* refuse.
//
// So the registration seam does not let a mutating tool do that. A mutating tool is
// registered as two pieces: a precondition check that runs **before** any block is
// opened and may refuse, and a mutating body that runs **inside** the block and
// cannot — its return type has no refusal alternative. The refusal branch of `execute`
// returns before `open_undo_block` is reachable. That is the requirement expressed as
// control flow rather than as a rule somebody has to remember while writing the
// thirty-ninth handler.
//
// Read tools and the two rewinding tools are registered through the other path, whose
// body *may* refuse, because no block is ever opened for them — `render`'s undo effect
// is `none` (requirement 12.7) and its collision refusal comes out of
// `render_coordinator::queue_render` as one indivisible step, and
// `revert_agent_changes` refuses on foreign undo entries from inside
// `undo_manager::revert_agent_changes`. Neither can be split into a precondition and
// a body without reimplementing a component that already exists.
//
// ---------------------------------------------------------------------------
// A success can also be a call in which nothing landed, and must say so
//
// The undo report is not "this tool mutates" — it is "there is a position `revert all`
// can walk back to". `tool_partial_outcome` gets this right on its own, because the
// framework can count its applied actions. A `tool_success` cannot be read that way:
// requirement 4.4 keeps the payload opaque, and this framework never dereferences it, so
// it has no way to discover that every entry of the array inside failed.
//
// That matters for exactly the two tools whose own output schema carries `actions` and
// which therefore always return `handler_success`. Both
// `set-item-properties.schema.json` and `set-track-state.schema.json` say in their own
// descriptions that `undoPositionBefore` is optional and that its *absence* is the
// signal: a result without it is a call in which nothing changed. Attaching it
// unconditionally tells the producer a marker exists for a call that moved nothing, and
// a marker sitting above their earlier work shortens the range `revert all` would walk —
// the same harm requirement 10.7 names for a refused mutation.
//
// So the handler signals it, because the handler is the only thing that can:
// `handler_success::nothing_was_applied`. One optional flag rather than a fourth result
// shape, because the payload, the schema, and the undo rule are all unchanged — the only
// thing the handler knows and the framework does not is whether anything landed. It
// defaults to false, which is the answer for the thirty-odd mutating tools whose success
// means the mutation happened.
//
// ---------------------------------------------------------------------------
// A failure without a reason is not representable
//
// Requirement 9.5 says a reasonless failure is not a representable result, and
// `tool-result-partial.schema.json` says the same by splitting `actionOutcome` into
// two closed shapes on `ok` rather than hanging an optional error off one. Mirrored
// here: `action_outcome` is a variant of `action_succeeded` and `action_failed`, and
// `action_failed` holds an `action_error` that has no default constructor and no state
// in which its code or message is empty. There is no way to name a failed action
// without naming why, and no way to reach one by forgetting a field.
//
// ---------------------------------------------------------------------------
// One block per call, and no rollback
//
// Requirement 9.4 and requirement 10.8: an action array executes inside one undo
// block, and a sibling's failure does not roll back what already landed.
// `undo_manager::run_in_undo_block` already provides the one block — the array's loop
// goes inside the body — so this file does not re-solve it. The no-rollback half is
// provided by omission: there is no undo call anywhere in this framework's mutating
// path. The only component that moves the stack backwards is `undo_manager`, reached
// only by the two rewinding tools the producer asked for explicitly.
//
// Requirement 9.9 and requirement 23.8 — a REAPER call failing mid-block closes the
// block and reports the action failed — are also `undo_manager`'s already:
// `run_in_undo_block` catches, the guard's destructor closes, and the report carries
// the reason. What this file adds is the mapping from that report to a result the
// agent can read.
//
// ---------------------------------------------------------------------------
// The four runtime checks
//
// The checks JSON Schema cannot express live here as named seams over the components
// that already implement them, not as reimplementations:
//
//   - End strictly after start, for regions, marker and region updates, and time
//     selections (requirement 9.6) — `check_end_after_start` below.
//   - Signal cycles on send creation and bus construction, including the ones that
//     close only through folder routing (requirement 9.7) — `cycle_detector.h`'s
//     `evaluate_send_creation` and `evaluate_bus_construction`, mapped by
//     `refusal_from_routing_decision`.
//   - Output path collisions before any file is written (requirement 9.8) —
//     `render_coordinator.h` detects them; `refusal_from_render_refusal` maps the
//     payload.
//   - Foreign undo entries in the turn's range (requirement 10.5) —
//     `undo_manager.h`'s classification; `refusal_from_foreign_undo` maps the payload.
//
// ---------------------------------------------------------------------------
// The executor does not validate tool inputs, and cannot
//
// Requirement 4.4 keeps the 42 tool input schemas out of the vendored bundle because
// the MCP Tool Server already validated the input against the authoritative copy.
// Here that is stronger than a rule: the framework is templated on the payload type
// and holds the input as an opaque `const JsonValue*` it never dereferences. It has no
// way to read a field, so it has no way to check one. The selectors a tool addresses
// come in on `tool_call::track_selectors`, extracted by the codec from the
// already-validated input — extraction, not validation.
//
// ---------------------------------------------------------------------------
// Shape of this file
//
// Header-only and inline, following `undo_manager.h`, `cycle_detector.h`, and
// `render_coordinator.h`: the Catch2 target compiles what it finds under `tests/` and
// does not compile `src/`, so logic the suite exercises has to be visible through the
// header. Nothing here includes the REAPER SDK or a JSON library — the payload type
// is a template parameter, which is what lets the suite drive the whole framework
// against a plain struct, and what lets the real build bind it to `nlohmann::json` in
// one alias when the handlers land.

#ifndef SESH_AI_DAW_TOOL_EXECUTOR_H
#define SESH_AI_DAW_TOOL_EXECUTOR_H

#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <daw/cycle_detector.h>
#include <daw/object_resolver.h>
#include <daw/render_coordinator.h>
#include <daw/undo_manager.h>
#include <transport/schema_validator.h>

namespace sesh_ai::daw
{
	// ---------------------------------------------------------------------------
	// Contract values
	//
	// Spelled once, because every one of them is a string the server or the model
	// reads, where a typo is a protocol bug rather than a compile error.
	// ---------------------------------------------------------------------------

	// The fourth `blockingEntity.kind` from the refusal schema. The other three are
	// already spelled by the components that produce them — `undo_entry` in
	// `undo_manager.h`, `track` in `object_resolver.h`, `file_path` in
	// `render_coordinator.h` — and `send` is the one nothing had needed yet, because a
	// routing cycle is reported by the executor rather than by the detector.
	inline constexpr std::string_view send_blocking_entity_kind{"send"};

	// `code` values this framework itself produces on a failed action. The handlers
	// bring their own for their own failures; these are the three the framework is
	// the only thing in a position to report.
	inline constexpr std::string_view reaper_api_failure_code{"reaper_api_failure"};
	inline constexpr std::string_view invalid_time_range_code{"invalid_time_range"};
	inline constexpr std::string_view undo_block_close_failure_code{"undo_block_close_failure"};

	// The fourth, and the one that reports a gap in the report itself: more outcomes were
	// produced than `maximum_action_outcomes` can carry, so some of them are not named.
	//
	// A failed action rather than a field, because a field would need
	// `tool-result-partial.schema.json` to grow one — and this is reportable without that.
	// It is `ok: false` because something did go wrong: not the actions, which stand, but
	// the report of them, and an agent reading a clean array of 512 would otherwise
	// conclude that 512 is all there was.
	inline constexpr std::string_view outcome_report_truncated_code{"outcome_report_truncated"};

	// `actionTarget` for the above. The report is what is incomplete, and no single object
	// is the thing that failed, so naming a GUID here would point at an action that
	// probably succeeded.
	inline constexpr std::string_view outcome_report_target{"this tool's outcome report"};

	// Requirement 23.6's error, which is not a tool result. `unknown_tool` is the
	// machine-readable code; the message names the tool, because a missing
	// implementation being visible is the entire point of the requirement.
	inline constexpr std::string_view unknown_tool_error_code{"unknown_tool"};

	// Schema bounds from `messages/tool-result-partial.schema.json`, so a payload this
	// framework fills in cannot fail the outbound validation requirement 4.1 asks for.

	// `actions` at `maxItems: 512`, and the one bound with a rule attached to it, because
	// three tool inputs allow more entries than one result can report the outcome of:
	// `set_item_properties` 4096 changes, the item tools 4096 GUIDs, `change_tempo_map`
	// 1024 points.
	//
	// **An outcome array is never cut to fit.** Stated once, in full, in the file header;
	// in short, a handler that can know the count before it touches anything reports one
	// failed action naming this cap and changes nothing, and a handler that cannot gets
	// `build_partial_outcome`'s backstop — the outcomes that fit, in order, and the last
	// slot spent naming how many are missing. Neither half loses an outcome in silence,
	// which requirements 9.5 and 23.9 both forbid.
	inline constexpr std::size_t maximum_action_outcomes = 512;
	inline constexpr std::size_t maximum_action_target_length = 1024;
	inline constexpr std::size_t maximum_action_error_code_length = 128;
	inline constexpr std::size_t maximum_action_error_message_length = 1024;

	// Every one of those fields has `minLength: 1`, and a handler can hand back an
	// empty string. Substituted rather than dropped, for the same reason
	// `undo_manager.h` substitutes for an unnamed undo entry: an outcome nobody can
	// name is still an outcome the agent needs told about, and dropping it would
	// remove the only evidence that something happened.
	inline constexpr std::string_view unnamed_action_target{"unnamed target"};
	inline constexpr std::string_view unnamed_failure_code{"unspecified_failure"};
	inline constexpr std::string_view unnamed_failure_message{
		"The tool reported a failure without describing it."};

	// What a tool does to the undo system. Three values, because "mutating or not" is
	// not enough: two tools move the stack themselves.
	enum class tool_undo_effect
	{
		// No block, no marker. Read tools, and `render` — requirement 12.7 is explicit
		// that a queued render records neither, so a render-only turn offers no
		// "revert all".
		none,

		// One identifiable block per call, and the turn's marker captured before the
		// first of them (requirements 10.1, 10.4, 10.8).
		undo_block,

		// `undo_last_action` and `revert_agent_changes`. These move the stack
		// backwards, so wrapping them in a block would record an entry describing the
		// removal of entries, and capturing a marker before one would point past the
		// producer's own work. Both output schemas omit `undoPositionBefore` for
		// exactly this reason.
		rewinds_undo_stack
	};

	inline constexpr std::string_view describe_tool_undo_effect(tool_undo_effect effect)
	{
		switch (effect)
		{
			case tool_undo_effect::none:
				return "none";
			case tool_undo_effect::undo_block:
				return "undo block";
			case tool_undo_effect::rewinds_undo_stack:
				return "rewinds the undo stack";
		}

		return "unknown undo effect";
	}

	namespace detail
	{
		// Clamps to a schema maximum and substitutes for an empty value, in one place.
		inline std::string within_schema_bounds(
			std::string_view value,
			std::string_view substitute_when_empty,
			std::size_t maximum_length)
		{
			std::string bounded{value.empty() ? substitute_when_empty : value};

			if (bounded.size() > maximum_length)
			{
				bounded.resize(maximum_length);
			}

			return bounded;
		}
	}

	// ---------------------------------------------------------------------------
	// Per-action outcomes
	// ---------------------------------------------------------------------------

	// Why one action failed — `actionError` from the partial schema.
	//
	// Requirement 9.5 in the type system. No default constructor, so a failure cannot
	// be brought into existence and filled in later; and no state in which either
	// string is empty, so "failed, reason to follow" is not a value this program can
	// hold. An empty argument is substituted rather than rejected, because throwing
	// from a result type on the REAPER main thread would trade a vague failure report
	// for a worse problem.
	class action_error
	{
	public:
		action_error() = delete;

		action_error(std::string_view code, std::string_view message)
			: code_{detail::within_schema_bounds(
				code,
				unnamed_failure_code,
				maximum_action_error_code_length)},
			message_{detail::within_schema_bounds(
				message,
				unnamed_failure_message,
				maximum_action_error_message_length)}
		{
		}

		// Never empty.
		const std::string& code() const { return code_; }

		// Never empty.
		const std::string& message() const { return message_; }

	private:
		std::string code_;
		std::string message_;
	};

	// One action that was applied. `ok` is a schema constant rather than a flag, so it
	// is not settable.
	struct action_succeeded
	{
		// `actionTarget` — the resolved GUID where the action addressed an object, so
		// the agent can retry precisely rather than re-resolving a pattern that may
		// now be ambiguous.
		std::string target;

		static constexpr bool ok = true;
	};

	// One action that was not applied, which must say why.
	//
	// No default constructor, inherited from `action_error` having none. That is the
	// whole mechanism: `std::vector<action_outcome>::emplace_back` cannot produce a
	// reasonless failure because there is no such object to produce.
	struct action_failed
	{
		std::string target;
		action_error error;

		static constexpr bool ok = false;
	};

	// The `actionOutcome` oneOf as a type: exactly one of the two shapes, never a
	// third, never a success carrying an error.
	using action_outcome = std::variant<action_succeeded, action_failed>;

	// An applied action, with its target bounded to what the schema accepts.
	inline action_outcome succeeded_action(std::string_view target)
	{
		return action_succeeded{
			detail::within_schema_bounds(target, unnamed_action_target, maximum_action_target_length)};
	}

	// A failed action. There is no overload taking only a target.
	inline action_outcome failed_action(
		std::string_view target,
		std::string_view failure_code,
		std::string_view failure_message)
	{
		return action_failed{
			detail::within_schema_bounds(target, unnamed_action_target, maximum_action_target_length),
			action_error{failure_code, failure_message}
		};
	}

	inline bool action_was_applied(const action_outcome& outcome)
	{
		return std::holds_alternative<action_succeeded>(outcome);
	}

	inline const std::string& action_target(const action_outcome& outcome)
	{
		if (const action_succeeded* const applied = std::get_if<action_succeeded>(&outcome))
		{
			return applied->target;
		}

		return std::get<action_failed>(outcome).target;
	}

	inline std::size_t count_applied_actions(const std::vector<action_outcome>& actions)
	{
		std::size_t applied = 0;

		for (const action_outcome& outcome : actions)
		{
			if (action_was_applied(outcome))
			{
				++applied;
			}
		}

		return applied;
	}

	inline std::size_t count_failed_actions(const std::vector<action_outcome>& actions)
	{
		return actions.size() - count_applied_actions(actions);
	}

	inline bool any_action_was_applied(const std::vector<action_outcome>& actions)
	{
		return count_applied_actions(actions) > 0;
	}

	// ---------------------------------------------------------------------------
	// The undo report
	// ---------------------------------------------------------------------------

	// `messages/tool-result-undo.schema.json`. Present on a mutating tool's result and
	// absent otherwise — which is why every result type below holds it as an
	// `std::optional` rather than defaulting the position to zero. Zero is a real
	// marker on a project with an empty undo history, so a default would be
	// indistinguishable from the start of a session.
	struct undo_report
	{
		// `undoPositionBefore` — the *turn's* marker, reported unchanged on every
		// mutating result in the turn (requirement 10.2). Not this block's own
		// position.
		int undo_position_before = 0;

		// `undoDescription` — what `Undo_EndBlock2` was given, `"Sesh AI: <tool>"`.
		std::string undo_description;
	};

	inline undo_report undo_report_from(const undo_block_report& report)
	{
		return undo_report{report.turn_undo_position_marker, report.undo_description};
	}

	// ---------------------------------------------------------------------------
	// The three result shapes
	// ---------------------------------------------------------------------------

	// Outcome one: the tool did what was asked (requirement 9.2).
	//
	// `fields` is the tool's own result payload, opaque to this framework — it is the
	// handler's, and the framework has no business knowing what is in it.
	template <typename JsonValue>
	struct tool_success
	{
		std::string tool_name;

		// Tool-specific result fields. Validated against the tool's own vendored
		// output schema by the codec, not here.
		JsonValue fields{};

		// Present for a mutating tool that changed something; absent for a read tool, for
		// `render`, and for a mutating tool whose handler said nothing landed — see
		// `handler_success::nothing_was_applied`. Absence is the signal, not a gap: both
		// action array output schemas describe a result without `undoPositionBefore` as a
		// call in which nothing changed.
		std::optional<undo_report> undo{};
	};

	// Outcome two: a precondition failed (requirement 9.3).
	//
	// `object_resolver.h`'s `ToolResultRefusal`, reused rather than redeclared — it is
	// already the refusal payload field for field, already has `refused` as a constant
	// rather than a settable flag, and `build_unresolved_track_selector_refusal` and
	// `build_ambiguous_track_selector_refusal` already produce two of the five.
	//
	// Note what it does not have: any undo field. A refusal cannot carry an undo
	// report because the type has nowhere to put one.
	using tool_refusal = ToolResultRefusal;

	// Outcome three: an action array, reported per action (requirement 9.4).
	//
	// Always this framework's shape, never a tool's own, whatever `tool_name` says. The
	// two tools whose output schemas carry `actions` build their payload themselves and
	// come back through `tool_success` — see the file header — so a value of this type is
	// a payload holding `actions` and nothing else, and it validates against
	// `messages/tool-result-partial.schema.json` rather than against the named tool's
	// output schema.
	template <typename JsonValue>
	struct tool_partial_outcome
	{
		// Which tool produced it. Carried for the envelope type and the log line, not to
		// choose a schema with — the shape is the same for all 42.
		std::string tool_name;

		// One entry per action, in the order the actions were supplied. At least one —
		// the schema's `minItems` — and at most `maximum_action_outcomes`, with the last
		// slot naming what is missing when the handler produced more than that.
		std::vector<action_outcome> actions;

		// Present when any action was applied. The schema reports
		// `undoPositionBefore` "whenever any action succeeded", and an array where
		// every action failed left nothing for "revert all" to walk back to.
		std::optional<undo_report> undo{};

		std::size_t applied_action_count() const { return count_applied_actions(actions); }
		std::size_t failed_action_count() const { return count_failed_actions(actions); }
	};

	// Requirement 9.2, 9.3, and 9.4 as one type. Three alternatives, so a fourth
	// result shape is not something this codebase can express.
	template <typename JsonValue>
	using tool_result = std::variant<tool_success<JsonValue>, tool_refusal, tool_partial_outcome<JsonValue>>;

	// Requirement 23.6. Not a tool result — see the file header.
	struct unknown_tool_error
	{
		// The name as it arrived, so a missing implementation is visible rather than
		// silent. Carried separately from the message because the server reads one and
		// the producer reads the other.
		std::string tool_name;

		std::string code{unknown_tool_error_code};
		std::string message;
	};

	// What a dispatch produced: a tool result, or the error that means no tool ran.
	//
	// Nested rather than flattened to four alternatives, deliberately. Flattening
	// would make "the result shapes are a closed set of three" untrue of the type that
	// names them, and the two levels say something real: `tool_result` is what a tool
	// returns, and an unknown tool returns nothing because there is no tool.
	template <typename JsonValue>
	using dispatch_outcome = std::variant<tool_result<JsonValue>, unknown_tool_error>;

	inline unknown_tool_error build_unknown_tool_error(std::string_view tool_name)
	{
		unknown_tool_error error;
		error.tool_name = tool_name.empty() ? std::string{"(unnamed)"} : std::string{tool_name};
		error.message = "This build of the extension has no implementation for the tool \""
			+ error.tool_name
			+ "\".";

		return error;
	}

	template <typename JsonValue>
	bool is_success(const tool_result<JsonValue>& result)
	{
		return std::holds_alternative<tool_success<JsonValue>>(result);
	}

	template <typename JsonValue>
	bool is_refusal(const tool_result<JsonValue>& result)
	{
		return std::holds_alternative<tool_refusal>(result);
	}

	template <typename JsonValue>
	bool is_partial(const tool_result<JsonValue>& result)
	{
		return std::holds_alternative<tool_partial_outcome<JsonValue>>(result);
	}

	template <typename JsonValue>
	bool is_unknown_tool(const dispatch_outcome<JsonValue>& outcome)
	{
		return std::holds_alternative<unknown_tool_error>(outcome);
	}

	// The undo report a result carries, or nothing.
	//
	// One function over all three shapes, so "a refusal records no undo block"
	// (requirement 9.3) is a single readable line rather than an assertion spread
	// across the call sites.
	template <typename JsonValue>
	std::optional<undo_report> undo_report_of(const tool_result<JsonValue>& result)
	{
		if (const tool_success<JsonValue>* const success = std::get_if<tool_success<JsonValue>>(&result))
		{
			return success->undo;
		}

		if (const tool_partial_outcome<JsonValue>* const partial =
				std::get_if<tool_partial_outcome<JsonValue>>(&result))
		{
			return partial->undo;
		}

		// A refusal. The type has no undo field, so this is not a lookup that
		// returned empty — there is nowhere for one to have been.
		return std::nullopt;
	}

	// Which vendored schema this outcome's payload validates against.
	//
	// Three answers for three shapes, and the file header tabulates why. Two of them are
	// contracts naming a shape this framework fills in, and only the success case is
	// answered by asking which tool it was:
	//
	//   - A refusal is `messages/tool-result-refusal.schema.json`. The payload is
	//     `ToolResultRefusal` whatever declined, so the tool name does not enter into it.
	//   - A partial is `messages/tool-result-partial.schema.json`. Named directly rather
	//     than through `transport::tool_result_schema_path`, which cannot answer this:
	//     it decides from a payload and a tool name, and a framework partial for
	//     `set_item_properties` looks from there exactly like that tool's own result. This
	//     is the shape the framework built, and naming the contract that describes it is
	//     the whole of the answer.
	//   - A success is the tool's own output schema, which is the one case where the
	//     handler built the payload and the tool name is therefore the question.
	//
	// Empty only for an unknown tool, or for a success naming something that is not one of
	// the 42 — there is nothing to validate against, which requirement 23.6 makes the
	// correct answer rather than a failure. A partial always has an answer, because the
	// contract it names does not depend on the tool existing.
	template <typename JsonValue>
	std::optional<std::string_view> result_schema_path_for(const dispatch_outcome<JsonValue>& outcome)
	{
		const tool_result<JsonValue>* const result = std::get_if<tool_result<JsonValue>>(&outcome);

		if (result == nullptr)
		{
			return std::nullopt;
		}

		return std::visit(
			[](const auto& shape) -> std::optional<std::string_view> {
				using ShapeType = std::decay_t<decltype(shape)>;

				if constexpr (std::is_same_v<ShapeType, tool_refusal>)
				{
					return transport::tool_result_schema_path({}, true);
				}
				else if constexpr (std::is_same_v<ShapeType, tool_partial_outcome<JsonValue>>)
				{
					return transport::tool_result_partial_contract_schema_path;
				}
				else
				{
					return transport::tool_result_schema_path(shape.tool_name, false);
				}
			},
			*result);
	}

	// ---------------------------------------------------------------------------
	// The runtime checks JSON Schema cannot express
	// ---------------------------------------------------------------------------

	// Requirement 9.6: end strictly greater than start, for regions, marker and region
	// updates, and time selections.
	//
	// Returns a reason rather than a refusal, and that follows the precedent
	// `render_coordinator.h` already set for the same check on render bounds: the
	// refusal schema's `reason` enum has no value for an inverted time range, and no
	// acknowledgement the producer could give would make an end before a start mean
	// something. So it is a failure that carries why, which is what requirement 9.5
	// asks of every failure and what property 28 needs — the operation does not
	// proceed.
	//
	// Strictly greater, so an equal pair is rejected: a zero-length region is not a
	// region. A NaN operand fails every comparison and so is rejected too, which is
	// the refusing direction.
	inline std::optional<action_error> check_end_after_start(double start_seconds, double end_seconds)
	{
		if (end_seconds > start_seconds)
		{
			return std::nullopt;
		}

		return action_error{
			invalid_time_range_code,
			"the end position must be strictly greater than the start position"
		};
	}

	inline bool time_range_is_well_formed(double start_seconds, double end_seconds)
	{
		return !check_end_after_start(start_seconds, end_seconds).has_value();
	}

	// Requirement 9.7, mapped from `cycle_detector.h`'s decision.
	//
	// Empty when the change may proceed — including when it may proceed because the
	// producer acknowledged a loop they meant to build, which the detector already
	// decides. The loop is described using the detector's own words so the producer is
	// shown the path rather than told a cycle exists.
	inline std::optional<tool_refusal> refusal_from_routing_decision(
		const routing_change_decision& decision)
	{
		if (decision.permitted)
		{
			return std::nullopt;
		}

		tool_refusal refusal;
		refusal.reason = decision.refusal_reason;
		refusal.acknowledgement_field = decision.acknowledgement_field;

		BlockingEntity entity;
		entity.kind = std::string{send_blocking_entity_kind};
		entity.description = detail::within_schema_bounds(
			decision.cycle.describe(),
			"a signal cycle the detector could not describe",
			maximum_blocking_description_length);
		refusal.blocking.push_back(std::move(entity));

		return refusal;
	}

	// Requirement 9.8 and the render taxonomy, mapped from `render_coordinator.h`.
	//
	// The reason travels unchanged rather than being reclassified here. The Render
	// Coordinator already chose the reason the refusal schema names, and rewriting it
	// on the way out would have the agent offering the producer the wrong
	// acknowledgement.
	inline tool_refusal refusal_from_render_refusal(const render_refusal& render)
	{
		tool_refusal refusal;
		refusal.reason = render.reason;
		refusal.acknowledgement_field = render.acknowledgement_field;
		refusal.blocking.reserve(render.blocking.size());

		for (const render_blocking_entity& blocking : render.blocking)
		{
			if (refusal.blocking.size() >= maximum_blocking_entities)
			{
				break;
			}

			BlockingEntity entity;
			entity.kind = blocking.kind;
			entity.description = detail::within_schema_bounds(
				blocking.description,
				"unnamed render target",
				maximum_blocking_description_length);
			entity.guid = blocking.guid;

			refusal.blocking.push_back(std::move(entity));
		}

		return refusal;
	}

	// Requirement 10.5, mapped from `undo_manager.h`'s foreign-entry check.
	//
	// Empty when the range is clean. The manager has already classified the whole
	// range and performed no undo actions by the time this is reachable, so nothing
	// here can be the thing that leaves the stack half-reverted.
	inline std::optional<tool_refusal> refusal_from_foreign_undo(const foreign_undo_refusal& foreign)
	{
		if (!foreign.refused)
		{
			return std::nullopt;
		}

		tool_refusal refusal;
		refusal.reason = foreign.reason;
		refusal.acknowledgement_field = foreign.acknowledgement_field;
		refusal.blocking.reserve(foreign.blocking.size());

		for (const blocking_undo_entry& blocking : foreign.blocking)
		{
			BlockingEntity entity;
			entity.kind = blocking.kind;
			entity.description = blocking.description;

			refusal.blocking.push_back(std::move(entity));
		}

		return refusal;
	}

	// ---------------------------------------------------------------------------
	// The call
	// ---------------------------------------------------------------------------

	// One inbound `request:<tool_name>` envelope, as this framework sees it.
	//
	// The input is held as a pointer this framework never dereferences — see the file
	// header on requirement 4.4. The selectors are separate because target resolution
	// happens before the handler runs (requirement 9.1) and the framework cannot read
	// them out of an opaque payload; extracting them from the already-validated input
	// is the codec's job.
	template <typename JsonValue>
	struct tool_call
	{
		std::string tool_name;

		// Echoed onto the response envelope by the dispatcher (requirement 5.9). Held
		// here so a handler's log line can name the call it belongs to.
		std::string request_id;

		// The already-validated tool input. Never read by this framework.
		const JsonValue* validated_input = nullptr;

		// Every track the call addresses, in the order the input named them. Resolved
		// before the handler runs; an unresolved or ambiguous one refuses the call and
		// the handler never runs.
		std::vector<TrackSelector> track_selectors;

		// The acknowledgement fields the refusal schema names, set by the agent on a
		// retry after the producer resolved what was blocking. Forwarded to the
		// handlers and the precondition checks, which are the things that know which
		// of them applies; the framework itself reads none of them, because target
		// resolution has no acknowledgement that changes its answer — an ambiguous
		// pattern is resolved by naming a GUID, not by approving the ambiguity.
		bool confirmed_track_selection = false;
		bool confirmed_signal_cycle = false;
		bool confirmed_overwrite = false;
		bool confirmed_foreign_undo = false;
	};

	// What a handler is given: the call, and the targets already resolved from it.
	//
	// References rather than copies, and therefore no default constructor. The
	// executor builds one per call on the stack and it does not outlive the call.
	template <typename JsonValue>
	struct tool_execution_context
	{
		const tool_call<JsonValue>& call;

		// In the order `tool_call::track_selectors` named them. Empty for a tool that
		// addresses no track.
		const std::vector<ResolvedTrack>& resolved_targets;
	};

	// ---------------------------------------------------------------------------
	// What a handler returns
	// ---------------------------------------------------------------------------

	// The tool's own result payload.
	template <typename JsonValue>
	struct handler_success
	{
		JsonValue fields{};

		// Set by a handler whose payload records that nothing landed, so the framework
		// omits the undo report — see the file header.
		//
		// False for almost every tool, because a mutating tool's success means the
		// mutation happened and there is a position `revert all` can walk back to. True
		// only where the payload can say "succeeded in reporting that nothing changed":
		// `set_item_properties` and `set_track_state` carry their own per-entry outcomes,
		// and an array in which every entry failed left nothing behind. Both schemas say
		// in their own descriptions that the absence of `undoPositionBefore` is what tells
		// the producer nothing changed, so reporting one anyway is a claim, not a detail.
		//
		// A flag the handler sets rather than something the framework works out, because
		// requirement 4.4 keeps `fields` opaque: the framework never reads a property of
		// it and so cannot count the outcomes inside.
		bool nothing_was_applied = false;
	};

	// Per-action outcomes from a tool taking an action array. The framework attaches
	// the undo report; the handler reports what landed.
	//
	// This is the framework's shape and validates against
	// `messages/tool-result-partial.schema.json`. A tool whose *own* output schema carries
	// `actions` alongside a sibling field it also requires cannot use this — there is
	// nowhere to hold the sibling — and returns `handler_success` instead.
	struct handler_action_outcomes
	{
		std::vector<action_outcome> actions;
	};

	// What the body of a mutating tool may return.
	//
	// Two alternatives, and the absent third is the point: there is no refusal here,
	// so a body running inside an undo block cannot produce one. See the file header.
	template <typename JsonValue>
	using mutating_handler_result = std::variant<handler_success<JsonValue>, handler_action_outcomes>;

	// What a read tool or a rewinding tool may return. Refusal is available here
	// because no undo block is ever opened for these, so requirement 9.3 holds
	// whatever they return.
	template <typename JsonValue>
	using non_mutating_handler_result =
		std::variant<handler_success<JsonValue>, handler_action_outcomes, tool_refusal>;

	// ---------------------------------------------------------------------------
	// The handler registration seam
	//
	// This is what tasks 10.3 through 10.9 plug into. Three registration calls, one
	// per undo effect, and the choice of call is the choice of when a refusal is
	// possible and whether a block is opened.
	// ---------------------------------------------------------------------------

	// Runs before any undo block is opened, and is the only place a mutating tool may
	// refuse. Empty means proceed.
	template <typename JsonValue>
	using tool_precondition_check =
		std::function<std::optional<tool_refusal>(const tool_execution_context<JsonValue>&)>;

	// Runs inside the undo block. One call, whatever the action array's length — the
	// loop belongs in here, which is what makes requirement 10.8's one block per array
	// true by construction.
	template <typename JsonValue>
	using mutating_tool_handler =
		std::function<mutating_handler_result<JsonValue>(const tool_execution_context<JsonValue>&)>;

	// Runs with no block open.
	template <typename JsonValue>
	using non_mutating_tool_handler =
		std::function<non_mutating_handler_result<JsonValue>(const tool_execution_context<JsonValue>&)>;

	template <typename JsonValue>
	struct mutating_tool_body
	{
		// May be empty, for a mutating tool with no precondition beyond target
		// resolution.
		tool_precondition_check<JsonValue> precondition_check;

		mutating_tool_handler<JsonValue> apply;
	};

	template <typename JsonValue>
	struct non_mutating_tool_body
	{
		// `none` or `rewinds_undo_stack`. Never `undo_block` — a body registered here
		// is one no block is opened for, and the factory functions are what keep the
		// two from being confused.
		tool_undo_effect undo_effect = tool_undo_effect::none;

		non_mutating_tool_handler<JsonValue> run;
	};

	// Which of the two shapes a registered tool has. A variant rather than two
	// optional members, so "the mutating handler is set if and only if the undo effect
	// is `undo_block`" is not an invariant anybody has to maintain.
	template <typename JsonValue>
	using tool_body = std::variant<mutating_tool_body<JsonValue>, non_mutating_tool_body<JsonValue>>;

	template <typename JsonValue>
	struct registered_tool
	{
		std::string tool_name;
		tool_body<JsonValue> body;

		tool_undo_effect undo_effect() const
		{
			if (const non_mutating_tool_body<JsonValue>* const non_mutating =
					std::get_if<non_mutating_tool_body<JsonValue>>(&body))
			{
				return non_mutating->undo_effect;
			}

			return tool_undo_effect::undo_block;
		}
	};

	// Why a registration was not accepted.
	enum class tool_registration_outcome
	{
		registered,

		// The name is not one of the 42 constrained tools. Rejected rather than
		// accepted, because a handler registered under a name the server will never
		// send is dead code that looks like coverage, and a typo in a tool name would
		// otherwise surface as requirement 23.6's unknown-tool error at runtime.
		not_a_constrained_tool,

		// Two handlers for one tool. Rejected rather than overwritten: whichever won
		// would be decided by registration order, which is file discovery order.
		already_registered,

		// An empty `std::function`. Rejected because calling it would throw
		// `std::bad_function_call` from inside the undo block.
		no_handler_supplied
	};

	inline constexpr std::string_view describe_tool_registration_outcome(tool_registration_outcome outcome)
	{
		switch (outcome)
		{
			case tool_registration_outcome::registered:
				return "registered";
			case tool_registration_outcome::not_a_constrained_tool:
				return "not one of the 42 constrained tools";
			case tool_registration_outcome::already_registered:
				return "already registered";
			case tool_registration_outcome::no_handler_supplied:
				return "no handler supplied";
		}

		return "unknown registration outcome";
	}

	// Where the handlers live.
	//
	// A vector and a walk rather than a hash map, following the reasoning
	// `object_resolver.h` gives for the GUID walk: 42 entries is not a lookup worth a
	// second data structure, and the vector keeps registration order, which is what
	// makes `registered_tool_names` reportable.
	template <typename JsonValue>
	class tool_handler_registry
	{
	public:
		// Registers a mutating tool. The check runs before any block opens and may
		// refuse; `apply` runs inside the block and cannot.
		tool_registration_outcome register_mutating_tool(
			std::string_view tool_name,
			mutating_tool_handler<JsonValue> apply,
			tool_precondition_check<JsonValue> precondition_check = {})
		{
			if (!apply)
			{
				return tool_registration_outcome::no_handler_supplied;
			}

			return register_body(
				tool_name,
				mutating_tool_body<JsonValue>{std::move(precondition_check), std::move(apply)});
		}

		// Registers a tool that opens no undo block and records no marker: a read
		// tool, or `render` (requirement 12.7).
		tool_registration_outcome register_read_tool(
			std::string_view tool_name,
			non_mutating_tool_handler<JsonValue> run)
		{
			return register_non_mutating(tool_name, tool_undo_effect::none, std::move(run));
		}

		// Registers `undo_last_action` or `revert_agent_changes` — a tool that moves
		// the undo stack itself, so no block and no marker.
		tool_registration_outcome register_rewinding_tool(
			std::string_view tool_name,
			non_mutating_tool_handler<JsonValue> run)
		{
			return register_non_mutating(tool_name, tool_undo_effect::rewinds_undo_stack, std::move(run));
		}

		// Null for a name nobody registered, which is requirement 23.6's case.
		const registered_tool<JsonValue>* find_tool(std::string_view tool_name) const
		{
			for (const registered_tool<JsonValue>& tool : tools_)
			{
				if (tool.tool_name == tool_name)
				{
					return &tool;
				}
			}

			return nullptr;
		}

		bool has_tool(std::string_view tool_name) const { return find_tool(tool_name) != nullptr; }

		std::size_t registered_tool_count() const { return tools_.size(); }

		std::vector<std::string> registered_tool_names() const
		{
			std::vector<std::string> names;
			names.reserve(tools_.size());

			for (const registered_tool<JsonValue>& tool : tools_)
			{
				names.push_back(tool.tool_name);
			}

			return names;
		}

		// The 42 minus what is registered.
		//
		// Here so the gap between the protocol and this build is a value the extension
		// can report at startup, rather than something discovered one unknown-tool
		// error at a time while a producer waits.
		std::vector<std::string> unregistered_constrained_tool_names() const
		{
			std::vector<std::string> missing;

			for (const transport::ToolOutputSchemaBinding& binding : transport::tool_output_schema_bindings)
			{
				if (!has_tool(binding.tool_name))
				{
					missing.emplace_back(binding.tool_name);
				}
			}

			return missing;
		}

	private:
		tool_registration_outcome register_non_mutating(
			std::string_view tool_name,
			tool_undo_effect undo_effect,
			non_mutating_tool_handler<JsonValue> run)
		{
			if (!run)
			{
				return tool_registration_outcome::no_handler_supplied;
			}

			return register_body(
				tool_name,
				non_mutating_tool_body<JsonValue>{undo_effect, std::move(run)});
		}

		tool_registration_outcome register_body(std::string_view tool_name, tool_body<JsonValue> body)
		{
			if (!transport::is_constrained_tool_name(tool_name))
			{
				return tool_registration_outcome::not_a_constrained_tool;
			}

			if (has_tool(tool_name))
			{
				return tool_registration_outcome::already_registered;
			}

			tools_.push_back(registered_tool<JsonValue>{std::string{tool_name}, std::move(body)});

			return tool_registration_outcome::registered;
		}

		std::vector<registered_tool<JsonValue>> tools_;
	};

	// ---------------------------------------------------------------------------
	// Target resolution
	// ---------------------------------------------------------------------------

	// Either every selector resolved, or the first refusal that stopped it. A variant,
	// so a caller cannot read a populated target list next to a populated refusal.
	using target_resolution = std::variant<std::vector<ResolvedTrack>, tool_refusal>;

	// Requirement 9.1: resolve the call's targets before the handler runs.
	//
	// Every selector, and it stops at the first that does not resolve. Resolving the
	// rest would be work whose only use is a longer log line, and a tool that resolved
	// four of five targets is not a tool that can run — the refusal is about the call,
	// not about one action.
	//
	// The refusals are `object_resolver.h`'s, built by its own builders. Nothing about
	// which candidates are named or how is decided here.
	inline target_resolution resolve_call_targets(
		const std::vector<ResolvableTrack>& tracks_in_project_order,
		const LearnedAliasLookup& learned_aliases,
		const std::vector<TrackSelector>& selectors)
	{
		std::vector<ResolvedTrack> resolved_targets;
		resolved_targets.reserve(selectors.size());

		for (const TrackSelector& selector : selectors)
		{
			TrackResolution resolution = resolve_track(tracks_in_project_order, learned_aliases, selector);

			if (const ResolvedTrack* const resolved = std::get_if<ResolvedTrack>(&resolution))
			{
				resolved_targets.push_back(*resolved);
				continue;
			}

			if (const AmbiguousTrackSelector* const ambiguous =
					std::get_if<AmbiguousTrackSelector>(&resolution))
			{
				return build_ambiguous_track_selector_refusal(*ambiguous);
			}

			return build_unresolved_track_selector_refusal();
		}

		return resolved_targets;
	}

	// ---------------------------------------------------------------------------
	// The executor
	// ---------------------------------------------------------------------------

	// Dispatches one tool call.
	//
	// Read `execute` top to bottom as the requirement it implements. Unknown tool
	// first, because there is nothing else to do with one. Then target resolution,
	// before anything is opened. Then the precondition check for a mutating tool,
	// still before anything is opened. Only then a block, and the block is
	// `undo_manager`'s, closed by `undo_manager`'s guard.
	//
	// Templated on the payload type. The real build binds it to `nlohmann::json` in
	// one alias; the suite binds it to whatever is convenient, which is what lets the
	// whole framework be driven without a JSON library present.
	template <typename JsonValue>
	class tool_executor_of
	{
	public:
		tool_executor_of(
			const tool_handler_registry<JsonValue>& registry,
			undo_manager& undo,
			const TrackListSource& track_list,
			const LearnedAliasLookup& learned_aliases)
			: registry_{registry},
			undo_{undo},
			track_list_{track_list},
			learned_aliases_{learned_aliases}
		{
		}

		tool_executor_of(const tool_executor_of&) = delete;
		tool_executor_of& operator=(const tool_executor_of&) = delete;
		tool_executor_of(tool_executor_of&&) = delete;
		tool_executor_of& operator=(tool_executor_of&&) = delete;

		dispatch_outcome<JsonValue> execute(const tool_call<JsonValue>& call)
		{
			// Requirement 23.6. Before anything else, because there is no target worth
			// resolving for a tool that does not exist and no block worth opening.
			const registered_tool<JsonValue>* const tool = registry_.find_tool(call.tool_name);

			if (tool == nullptr)
			{
				return build_unknown_tool_error(call.tool_name);
			}

			// Requirement 9.1. The track list is read once per call: reading it per
			// selector would let the producer's edit land between two resolutions of
			// one call, which is how a two-target tool ends up addressing two
			// different projects.
			target_resolution targets = resolve_call_targets(
				track_list_.tracks_in_project_order(),
				learned_aliases_,
				call.track_selectors);

			if (tool_refusal* const refusal = std::get_if<tool_refusal>(&targets))
			{
				return refuse(std::move(*refusal));
			}

			const std::vector<ResolvedTrack>& resolved_targets =
				std::get<std::vector<ResolvedTrack>>(targets);

			const tool_execution_context<JsonValue> context{call, resolved_targets};

			if (const non_mutating_tool_body<JsonValue>* const non_mutating =
					std::get_if<non_mutating_tool_body<JsonValue>>(&tool->body))
			{
				return run_without_undo_block(call, *non_mutating, context);
			}

			return run_inside_undo_block(
				call,
				std::get<mutating_tool_body<JsonValue>>(tool->body),
				context);
		}

		const tool_handler_registry<JsonValue>& registry() const { return registry_; }

	private:
		// Requirement 9.3 and 10.7. The call to `record_refused_mutation` does
		// nothing, and that is why it is here: `undo_manager` names the no-op so the
		// refusal path has something to say out loud, and so a test can assert that
		// taking it leaves no marker.
		dispatch_outcome<JsonValue> refuse(tool_refusal refusal)
		{
			undo_.record_refused_mutation();

			return tool_result<JsonValue>{std::move(refusal)};
		}

		// A read tool, `render`, or one of the two rewinding tools. No block is opened,
		// so every shape the body can return is reportable as it stands.
		dispatch_outcome<JsonValue> run_without_undo_block(
			const tool_call<JsonValue>& call,
			const non_mutating_tool_body<JsonValue>& body,
			const tool_execution_context<JsonValue>& context)
		{
			non_mutating_handler_result<JsonValue> produced;

			try
			{
				produced = body.run(context);
			}
			catch (const std::exception& failure)
			{
				return single_failure_result(call.tool_name, describe_exception(failure), std::nullopt);
			}
			catch (...)
			{
				return single_failure_result(
					call.tool_name,
					"The tool threw a non-standard exception.",
					std::nullopt);
			}

			if (tool_refusal* const refusal = std::get_if<tool_refusal>(&produced))
			{
				return refuse(std::move(*refusal));
			}

			return attach_undo_report(call.tool_name, std::move(produced), std::nullopt);
		}

		// A mutating tool. Precondition first and outside the block; then exactly one
		// block, opened and closed by `undo_manager`.
		dispatch_outcome<JsonValue> run_inside_undo_block(
			const tool_call<JsonValue>& call,
			const mutating_tool_body<JsonValue>& body,
			const tool_execution_context<JsonValue>& context)
		{
			if (body.precondition_check)
			{
				std::optional<tool_refusal> refusal;

				try
				{
					refusal = body.precondition_check(context);
				}
				catch (const std::exception& failure)
				{
					// A check that could not run is not a check that passed. Reported
					// as a failure with a reason, and no block was opened, so the
					// session is untouched.
					return single_failure_result(call.tool_name, describe_exception(failure), std::nullopt);
				}
				catch (...)
				{
					return single_failure_result(
						call.tool_name,
						"The tool's precondition check threw a non-standard exception.",
						std::nullopt);
				}

				if (refusal.has_value())
				{
					return refuse(std::move(*refusal));
				}
			}

			// One block for the whole call, action array included (requirements 9.4,
			// 10.8). `run_in_undo_block` captures the turn's marker if this is its
			// first mutation, catches a throwing body, and closes the block through the
			// guard's destructor whatever happens — requirement 9.9 and requirement
			// 23.8, already solved there and not re-solved here.
			std::optional<mutating_handler_result<JsonValue>> produced;

			const undo_block_report report = undo_.run_in_undo_block(
				call.tool_name,
				[&] { produced = body.apply(context); });

			// A block that failed to close leaves no report. `undoPositionBefore`
			// promises "revert all" a position to walk back to, and an unbalanced
			// `Undo_EndBlock2` means nobody knows what walking back to it would undo —
			// possibly the producer's own work. Omitting it costs the agent the offer
			// to revert a change the producer can still undo by hand; promising it
			// costs them their session.
			const std::optional<undo_report> undo =
				report.close_failed ? std::nullopt : std::optional<undo_report>{undo_report_from(report)};

			if (!report.failure_reason.empty() || !produced.has_value())
			{
				// Requirement 9.9: the block is closed by now, and the action is
				// reported failed with a reason. A body that threw part way through an
				// array left whatever it had already applied in place, which is
				// requirement 9.4's no-rollback rule — there is no undo call on this
				// path, so there is nothing that could roll it back.
				const std::string reason = report.failure_reason.empty()
					? std::string{"The tool produced no result."}
					: report.failure_reason;

				if (produced.has_value())
				{
					// Outcomes the body did report before the failure are kept, and the
					// block-level failure is appended rather than replacing them: the
					// agent needs to know which actions landed.
					dispatch_outcome<JsonValue> partial =
						attach_undo_report(call.tool_name, std::move(*produced), undo);

					append_failure(partial, call.tool_name, reaper_api_failure_code, reason);

					return partial;
				}

				return single_failure_result(call.tool_name, reason, undo);
			}

			if (report.close_failed)
			{
				// The body's work stands; what failed is the record of it. Reported as
				// its own failed action so the result does not claim a clean block.
				dispatch_outcome<JsonValue> outcome =
					attach_undo_report(call.tool_name, std::move(*produced), undo);

				append_failure(
					outcome,
					call.tool_name,
					undo_block_close_failure_code,
					"REAPER failed to close the undo block for this tool, so no undo point is "
					"reported for it.");

				return outcome;
			}

			return attach_undo_report(call.tool_name, std::move(*produced), undo);
		}

		static std::string describe_exception(const std::exception& failure)
		{
			const std::string reason{failure.what() == nullptr ? "" : failure.what()};

			return reason.empty() ? std::string{"The tool threw an exception with no message."} : reason;
		}

		// Turns what a handler produced into one of the two non-refusal shapes, with
		// the undo report attached. The handler never sees the undo report, which is
		// what keeps a handler from reporting a marker for a tool that should not have
		// one.
		template <typename HandlerResult>
		static dispatch_outcome<JsonValue> attach_undo_report(
			std::string_view tool_name,
			HandlerResult produced,
			std::optional<undo_report> undo)
		{
			if (handler_action_outcomes* const actions = std::get_if<handler_action_outcomes>(&produced))
			{
				return tool_result<JsonValue>{
					build_partial_outcome(tool_name, std::move(actions->actions), std::move(undo))};
			}

			handler_success<JsonValue>& success = std::get<handler_success<JsonValue>>(produced);

			tool_success<JsonValue> result;
			result.tool_name = std::string{tool_name};
			result.fields = std::move(success.fields);

			// The handler's own answer to "did anything land", which is the one thing about
			// its payload the framework cannot see. Omitting the report is the signal the
			// two action array schemas describe: a result without `undoPositionBefore` is a
			// call in which nothing changed.
			result.undo = success.nothing_was_applied ? std::nullopt : std::move(undo);

			return tool_result<JsonValue>{std::move(result)};
		}

		// Bounds an outcome array to what `tool-result-partial.schema.json` accepts,
		// leaving `reserved_slots` free for entries the caller is about to add, and names
		// what it could not carry rather than dropping it.
		//
		// The backstop half of the outcome-cap rule — see the file header. A handler that
		// can refuse before touching anything should, and the three that can do; this is
		// for `change_tempo_map`, whose success shape reports 1024 points by aggregate
		// count and which therefore only discovers it needs per-action outcomes once a
		// point has already failed.
		//
		// The order is the schema's: "in the order the actions were supplied, so the agent
		// can match an outcome to what it asked for". So the array is cut from the end and
		// never reordered — putting the failures first would report more of what the agent
		// can act on and break the pairing the schema promises, which is the worse trade.
		static void report_within_outcome_cap(
			std::vector<action_outcome>& actions,
			std::size_t reserved_slots)
		{
			const std::size_t reportable = maximum_action_outcomes - reserved_slots;

			if (actions.size() <= reportable)
			{
				return;
			}

			// One slot goes to saying what is missing, which is the entire point: the
			// alternative is the silent truncation requirements 9.5 and 23.9 forbid.
			const std::size_t named = reportable - 1;
			const std::size_t unnamed = actions.size() - named;

			actions.resize(named);
			actions.push_back(failed_action(
				outcome_report_target,
				outcome_report_truncated_code,
				"This call produced more outcomes than one result can carry, so "
					+ std::to_string(unnamed)
					+ " of them are not named here — the first "
					+ std::to_string(named)
					+ " are, in the order the actions were supplied. What those actions did to the "
					  "session stands; read the session back to see what it now holds."));
		}

		// A partial whose only action failed. The shape a failure takes when the tool
		// reported nothing of its own: it is the only one of the three that can carry a
		// reason, and requirement 9.5 says the reason is not optional.
		static dispatch_outcome<JsonValue> single_failure_result(
			std::string_view tool_name,
			std::string_view failure_reason,
			std::optional<undo_report> undo)
		{
			std::vector<action_outcome> actions;
			actions.push_back(failed_action(tool_name, reaper_api_failure_code, failure_reason));

			return tool_result<JsonValue>{
				build_partial_outcome(tool_name, std::move(actions), std::move(undo))};
		}

		static tool_partial_outcome<JsonValue> build_partial_outcome(
			std::string_view tool_name,
			std::vector<action_outcome> actions,
			std::optional<undo_report> undo)
		{
			tool_partial_outcome<JsonValue> partial;
			partial.tool_name = std::string{tool_name};
			partial.actions = std::move(actions);

			report_within_outcome_cap(partial.actions, 0);

			// The schema reports `undoPositionBefore` whenever any action succeeded.
			// An array where everything failed left nothing for "revert all" to walk
			// back to, and offering a position anyway would have the producer reverting
			// past the turn's start into their own work.
			partial.undo = any_action_was_applied(partial.actions) ? std::move(undo) : std::nullopt;

			return partial;
		}

		// Appends one failed action to whichever shape the outcome already is,
		// converting a success into a partial because a success has nowhere to carry a
		// failure.
		//
		// The conversion loses the tool-specific fields, which is a real cost and the
		// right trade: the two cases that reach here are a body that threw after
		// reporting and an undo block that would not close, and in both of them a
		// payload claiming the tool succeeded is the worse outcome.
		static void append_failure(
			dispatch_outcome<JsonValue>& outcome,
			std::string_view tool_name,
			std::string_view failure_code,
			std::string_view failure_message)
		{
			tool_result<JsonValue>* const result = std::get_if<tool_result<JsonValue>>(&outcome);

			if (result == nullptr)
			{
				return;
			}

			if (tool_partial_outcome<JsonValue>* const partial =
					std::get_if<tool_partial_outcome<JsonValue>>(result))
			{
				// One slot held back for the failure about to be appended, rather than
				// dropping it when the array is already full. The two cases that reach here
				// are a body that threw and an undo block that would not close, and both are
				// the most important thing in the result — silently discarding one because
				// the handler had already filled the array would leave a payload claiming a
				// clean block.
				report_within_outcome_cap(partial->actions, 1);

				partial->actions.push_back(failed_action(tool_name, failure_code, failure_message));

				partial->undo = any_action_was_applied(partial->actions) ? partial->undo : std::nullopt;

				return;
			}

			tool_success<JsonValue>* const success = std::get_if<tool_success<JsonValue>>(result);

			if (success == nullptr)
			{
				return;
			}

			std::vector<action_outcome> actions;
			actions.push_back(succeeded_action(tool_name));
			actions.push_back(failed_action(tool_name, failure_code, failure_message));

			// The success's own undo report travels across rather than being rebuilt from
			// the fabricated applied action. A handler that said nothing landed has already
			// had its report omitted by `attach_undo_report`, and the conversion must not
			// hand it back.
			*result = build_partial_outcome(tool_name, std::move(actions), success->undo);
		}

		const tool_handler_registry<JsonValue>& registry_;
		undo_manager& undo_;
		const TrackListSource& track_list_;
		const LearnedAliasLookup& learned_aliases_;
	};
}

#endif
