// Undo Manager — the two undo layers of ADR 0015, and the foreign-entry check of
// ADR 0017 decision 6.
//
// This is the component the design says carries more safety weight than any other,
// so it is worth being plain about what the failure looks like. A producer works
// alongside the assistant. They nudge a fader while it is inserting tracks. Later
// they ask it to undo what it did. If "revert all" walks the undo stack back to the
// turn's starting position without looking at what it is walking through, it silently
// destroys their fader move along the way, and REAPER's redo will not reliably bring
// it back because the agent's own subsequent work has branched the history. Nothing
// about that failure is visible while it happens. That is the whole reason the range
// is classified before anything is undone.
//
// ---------------------------------------------------------------------------
// Layer one, per tool: an identifiable undo block
//
// Every mutating tool runs inside an `Undo_BeginBlock2` / `Undo_EndBlock2` pair
// described as `"Sesh AI: <tool_name>"` (requirement 10.4). Two things depend on the
// description rather than on it merely being present: the producer reading REAPER's
// undo history can tell which steps were the assistant's, and layer three below can
// classify an entry it finds in the range. An untagged block is not a cosmetic
// omission — it is an entry that will be read as the producer's own work and will
// block their own revert.
//
// ---------------------------------------------------------------------------
// Layer two, per turn: one stable undo position marker
//
// The stack position captured immediately before the turn's *first* mutating tool
// opens its block (requirement 10.1), reported unchanged on every later mutating
// result in the turn (requirement 10.2), and used as the walk-back target for
// "revert all".
//
// Three cases record nothing, and each is a requirement rather than an optimisation:
//
//   - A read-only turn. No mutating tool ran, so there is nothing to walk back to
//     and a marker would offer the producer a revert to nowhere (requirement 10.3).
//   - A refused mutation. Marker capture is bound to opening an undo block, and a
//     refusal never opens one, so this holds by construction rather than by a check
//     somebody has to remember. It matters because a marker recorded by a refusal
//     mid-turn would sit *above* the agent's earlier work, shortening the range
//     "revert all" walks and silently making the rest of the turn unrecoverable
//     (requirement 10.7).
//   - A rewinding tool. `undo_last_action` and `revert_agent_changes` move the stack
//     backwards, so a marker captured before one of them would point at a position
//     above the producer's own work; walking back to it would redo what had just
//     been reverted. Both output schemas leave `undoPositionBefore` out for exactly
//     this reason and report `undoStackPositionAfter` instead.
//
// ---------------------------------------------------------------------------
// Layer three: classify the range before undoing anything
//
// "Revert all" walks the entries from the current stack position back to the marker,
// classifies each as agent-created or foreign, and refuses if any entry in the range
// is foreign — naming it with REAPER's own description of what the producer did
// (requirement 10.5).
//
// Two properties of that check are the point of it, and both are easy to implement
// away by accident:
//
//   - **The whole range, not the top of the stack** (requirement 10.6). A producer's
//     edit lands wherever they happened to make it, and the top of the stack will
//     almost always be the agent's own most recent block — so a check that looks at
//     the top passes every obvious test and misses every real interleaving.
//   - **Zero undo actions when it refuses.** The stack must be left exactly as the
//     request found it, which means the entire range is read and classified before
//     the first `Undo_DoUndo2` call, not undone optimistically and stopped when
//     something foreign turns up. Undoing three of the agent's blocks and then
//     discovering the fourth entry is the producer's leaves the session in a state
//     neither of them asked for.
//
// Classification is a prefix test on the description, and it is deliberately exact:
// no trimming, no case folding, no fuzzy matching. The two ways to be wrong are not
// symmetric. Reading one of the agent's own entries as foreign produces a refusal —
// annoying, recoverable, and visible. Reading one of the producer's entries as the
// agent's destroys their work. So anything that widens the match widens it in the
// dangerous direction, and an entry REAPER gives no description for is classified
// foreign rather than guessed at.
//
// ---------------------------------------------------------------------------
// Shape of this header
//
// The classification and the walk are pure functions over positions and strings, and
// the REAPER dependency is behind `undo_stack` — five calls, the narrowest interface
// this component can do its job with, following the convention set by
// `entry/timer_registration.h`. No SDK header is reachable from here, which is what
// lets the suite drive the whole component against a scripted stack: interleaved
// producer edits, a `Undo_EndBlock2` that throws, a stack that refuses to move. Those
// are the cases that matter and none of them can be staged inside REAPER.
//
// Defined inline in the header, following `unit_conversion.h` and `cycle_detector.h`:
// the Catch2 target compiles the sources it finds under `tests/` and does not compile
// `src/`, so logic the suite exercises has to be visible through the header. The
// REAPER-backed `undo_stack` lives in `reaper_undo_stack.h` / `.cpp`, which is the
// only translation unit in this component that includes the SDK.

#ifndef SESH_AI_DAW_UNDO_MANAGER_H
#define SESH_AI_DAW_UNDO_MANAGER_H

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sesh_ai::daw
{
	// The prefix that marks an undo entry as the agent's work, and the separator
	// between it and the tool name. Split apart because the two have different jobs:
	// the prefix alone is what classification tests, so a later change to the
	// separator cannot quietly stop foreign-entry detection from recognising the
	// agent's own blocks.
	inline constexpr std::string_view agent_undo_description_prefix{"Sesh AI:"};
	inline constexpr std::string_view agent_undo_description_separator{" "};

	// Contract values from the schemas rather than prose. `foreign_undo_entries` is a
	// `reason` in `messages/tool-result-refusal.schema.json`, `undo_entry` is a
	// `blockingEntity.kind` in the same file, and `confirmedForeignUndo` is the input
	// property both `undo-last-action.schema.json` and
	// `revert-agent-changes.schema.json` expose for the retry.
	inline constexpr std::string_view foreign_undo_refusal_reason{"foreign_undo_entries"};
	inline constexpr std::string_view foreign_undo_acknowledgement_field{"confirmedForeignUndo"};
	inline constexpr std::string_view undo_entry_blocking_kind{"undo_entry"};

	// Schema bounds, so a payload this component fills in cannot fail the outbound
	// validation requirement 4.1 asks for. `undoDescription` is capped at 512
	// characters, a `blockingEntity.description` at 1024, `blocking` at 512 entries,
	// and `undoneEntryDescriptions` at 1024.
	inline constexpr std::size_t maximum_undo_description_length = 512;
	inline constexpr std::size_t maximum_blocking_description_length = 1024;
	inline constexpr std::size_t maximum_blocking_entity_count = 512;
	inline constexpr std::size_t maximum_undone_entry_description_count = 1024;

	// Every one of those schema fields has `minLength: 1`, and REAPER can hand back an
	// empty description. Substituted rather than dropped: an entry nobody can name is
	// still an entry the producer needs told about, and dropping it would remove the
	// only evidence that something unidentified is in the range.
	inline constexpr std::string_view unnamed_undo_entry_description{"Unnamed REAPER undo entry"};

	// Used when a tool name is missing. A block with no description at all would be
	// classified foreign on the next revert — the agent's own work blocking the
	// producer's recovery — so the fallback keeps the prefix and says what happened.
	inline constexpr std::string_view unnamed_tool_description{"unnamed tool"};

	// `Undo_EndBlock2`'s `extraflags`. REAPER treats -1 as "every state", which is the
	// correct default for a tool whose blast radius is not known here: under-declaring
	// the states a block touched produces an undo entry that does not restore
	// everything the tool changed, which is a silent partial revert.
	inline constexpr int undo_all_states = -1;

	// Whether REAPER's description identifies an entry as the agent's work.
	//
	// Exact prefix match, for the reason in the header comment: every relaxation of
	// this test relaxes it towards reading a producer's edit as the agent's.
	inline bool undo_description_is_agent_created(std::string_view description)
	{
		return description.size() >= agent_undo_description_prefix.size()
			&& description.compare(
				0,
				agent_undo_description_prefix.size(),
				agent_undo_description_prefix) == 0;
	}

	// The description a tool's undo block is tagged with: `"Sesh AI: <tool_name>"`.
	//
	// Truncated to the schema's limit from the right, which cannot damage the prefix —
	// the prefix is nine characters and the limit is 512.
	inline std::string compose_agent_undo_description(std::string_view tool_name)
	{
		std::string description{agent_undo_description_prefix};
		description += agent_undo_description_separator;
		description += tool_name.empty() ? unnamed_tool_description : tool_name;

		if (description.size() > maximum_undo_description_length)
		{
			description.resize(maximum_undo_description_length);
		}

		return description;
	}

	// REAPER's own words for an entry, in a form the schemas accept.
	inline std::string normalise_undo_entry_description(std::string_view description)
	{
		std::string normalised{description.empty() ? unnamed_undo_entry_description : description};

		if (normalised.size() > maximum_blocking_description_length)
		{
			normalised.resize(maximum_blocking_description_length);
		}

		return normalised;
	}

	// What REAPER's undo stack looks like from this side.
	//
	// Positions are the abstraction the whole component is stated in: a non-negative
	// count that goes up by one when an entry is recorded and down when one is undone.
	// The entry *at* position p is the one that took the stack from p to p + 1, so the
	// entries added since a marker sit at positions [marker, current) and the marker
	// itself names no entry. Stating it this way rather than in REAPER's index
	// convention is what keeps the classification and the walk testable, and confines
	// the mapping to `reaper_undo_stack.cpp`.
	class undo_stack
	{
	public:
		virtual ~undo_stack() = default;

		// `Undo_GetCurEntry`.
		virtual int current_position() = 0;

		// `Undo_GetEntryDesc`. Empty when REAPER has no description for the position,
		// which callers treat as an unidentified — and therefore foreign — entry
		// rather than as an absent one.
		virtual std::string entry_description_at(int position) = 0;

		// `Undo_BeginBlock2`.
		virtual void begin_block() = 0;

		// `Undo_EndBlock2`.
		virtual void end_block(const std::string& description, int extra_flags) = 0;

		// `Undo_DoUndo2`. False when REAPER performed no undo, which ends a walk
		// rather than being retried.
		virtual bool undo_one_entry() = 0;
	};

	// An open undo block, closed by its destructor.
	//
	// A destructor rather than a close call at the end of each tool, because C++ has
	// no `finally` and the paths out of a tool implementation are not all `return`:
	// an early refusal, a `MediaTrack*` that resolved to null halfway through, a
	// `std::bad_alloc` from a container. Requirement 9.9 and requirement 23.8 both
	// say no block is ever left open, and a block left open in REAPER is worse than
	// it sounds — every subsequent edit the producer makes accumulates into the
	// agent's entry until something closes it, so one leaked block turns the rest of
	// their session into a single undo step.
	//
	// Closing is idempotent and never throws. `close()` marks the block closed
	// *before* it calls `end_block`, so an `end_block` that throws cannot be called a
	// second time by the destructor — which would be a second `Undo_EndBlock2`
	// against one `Undo_BeginBlock2`, unbalancing REAPER's own refcount. Exactly one
	// close attempt per open is the guarantee available when the close itself can
	// fail; `close_failed()` reports that it did, so the tool result can say so
	// instead of claiming a clean block.
	class undo_block_guard
	{
	public:
		// A guard holding nothing. Lets a caller declare one before it knows whether
		// the tool mutates, and makes the moved-from state expressible.
		undo_block_guard() = default;

		undo_block_guard(undo_stack& stack, std::string description, int extra_flags = undo_all_states)
			: stack_{&stack},
			description_{std::move(description)},
			extra_flags_{extra_flags},
			position_before_{stack.current_position()}
		{
			// If this throws the guard is never constructed, and no block was opened
			// for a destructor to have to close.
			stack.begin_block();
			open_ = true;
		}

		~undo_block_guard()
		{
			close();
		}

		undo_block_guard(const undo_block_guard&) = delete;
		undo_block_guard& operator=(const undo_block_guard&) = delete;

		undo_block_guard(undo_block_guard&& other) noexcept
			: stack_{other.stack_},
			description_{std::move(other.description_)},
			extra_flags_{other.extra_flags_},
			position_before_{other.position_before_},
			open_{other.open_},
			close_failed_{other.close_failed_}
		{
			other.release();
		}

		undo_block_guard& operator=(undo_block_guard&& other) noexcept
		{
			if (this == &other)
			{
				return *this;
			}

			// The block this guard already holds is closed rather than abandoned:
			// assigning over a live guard must not be a way to leak a block.
			close();

			stack_ = other.stack_;
			description_ = std::move(other.description_);
			extra_flags_ = other.extra_flags_;
			position_before_ = other.position_before_;
			open_ = other.open_;
			close_failed_ = other.close_failed_;

			other.release();

			return *this;
		}

		void close() noexcept
		{
			if (!open_ || stack_ == nullptr)
			{
				return;
			}

			// Cleared first. See the class comment: one close attempt per open, even
			// when the attempt throws.
			open_ = false;

			try
			{
				stack_->end_block(description_, extra_flags_);
			}
			catch (...)
			{
				close_failed_ = true;
			}
		}

		bool is_open() const { return open_; }

		// True when `Undo_EndBlock2` itself failed. The block is not open as far as
		// this guard is concerned and will not be closed again, but the tool result
		// should not claim the change was recorded cleanly.
		bool close_failed() const { return close_failed_; }

		const std::string& description() const { return description_; }

		// The stack position before this block opened. Not the turn's marker — see
		// `undo_manager::turn_undo_position_marker`, which is what a tool result
		// reports.
		int position_before() const { return position_before_; }

	private:
		// Gives up the block without closing it. Only for a moved-from guard, whose
		// block the destination now owns.
		void release() noexcept
		{
			stack_ = nullptr;
			open_ = false;
			close_failed_ = false;
		}

		undo_stack* stack_ = nullptr;
		std::string description_;
		int extra_flags_ = undo_all_states;
		int position_before_ = 0;
		bool open_ = false;
		bool close_failed_ = false;
	};

	// One entry in the turn's undo range, and which side it came from.
	struct classified_undo_entry
	{
		// Where the entry sits on the stack. Carried rather than implied by list
		// order so a refusal can say where in the turn the producer's edit landed.
		int stack_position = 0;

		// REAPER's own words, which is what the producer is shown.
		std::string description;

		bool agent_created = false;
	};

	// A `blockingEntity` from `messages/tool-result-refusal.schema.json`.
	struct blocking_undo_entry
	{
		std::string kind{undo_entry_blocking_kind};
		std::string description;
	};

	// The turn's undo range, every entry classified.
	//
	// Produced in full before any undo happens, which is what makes "performing no
	// undo actions at all" (requirement 10.5) a property of the control flow rather
	// than a promise.
	struct undo_range_classification
	{
		// The walk-back target. Equal to `current_position` when the turn has no
		// marker or nothing has been recorded since it, which is an empty range and a
		// complete answer.
		int marker_position = 0;
		int current_position = 0;

		// Oldest first, one per position in [marker_position, current_position).
		std::vector<classified_undo_entry> entries;

		// True when the turn recorded no marker at all, as opposed to recording one
		// and having nothing above it. Both are empty ranges; only the first means
		// "revert all" should not have been offered.
		bool turn_has_no_marker = false;

		bool contains_foreign_entries() const
		{
			for (const classified_undo_entry& entry : entries)
			{
				if (!entry.agent_created)
				{
					return true;
				}
			}

			return false;
		}

		std::vector<classified_undo_entry> foreign_entries() const
		{
			std::vector<classified_undo_entry> foreign;

			for (const classified_undo_entry& entry : entries)
			{
				if (!entry.agent_created)
				{
					foreign.push_back(entry);
				}
			}

			return foreign;
		}

		std::size_t entry_count() const { return entries.size(); }
	};

	// The refusal payload's undo-specific fields. `refused` false means the fields
	// are unset and the operation may proceed.
	struct foreign_undo_refusal
	{
		bool refused = false;
		std::string reason;
		std::vector<blocking_undo_entry> blocking;
		std::string acknowledgement_field;
	};

	// The refusal for a range containing the producer's own work.
	//
	// Only the foreign entries are named. The agent's own blocks are not what is
	// blocking the call, and listing them would bury the one or two lines the
	// producer needs to recognise as theirs.
	inline foreign_undo_refusal build_foreign_undo_refusal(const undo_range_classification& range)
	{
		foreign_undo_refusal refusal;
		refusal.refused = true;
		refusal.reason = std::string{foreign_undo_refusal_reason};
		refusal.acknowledgement_field = std::string{foreign_undo_acknowledgement_field};

		for (const classified_undo_entry& entry : range.entries)
		{
			if (entry.agent_created)
			{
				continue;
			}

			if (refusal.blocking.size() >= maximum_blocking_entity_count)
			{
				break;
			}

			blocking_undo_entry blocking;
			blocking.description = entry.description;
			refusal.blocking.push_back(std::move(blocking));
		}

		return refusal;
	}

	// Classification as a pure function, so the interleaving cases can be written out
	// directly: descriptions in stack order from the marker upwards, oldest first.
	inline undo_range_classification classify_undo_range(
		int marker_position,
		const std::vector<std::string>& descriptions_oldest_first)
	{
		undo_range_classification range;
		range.marker_position = marker_position < 0 ? 0 : marker_position;
		range.current_position = range.marker_position + static_cast<int>(descriptions_oldest_first.size());
		range.entries.reserve(descriptions_oldest_first.size());

		for (std::size_t offset = 0; offset < descriptions_oldest_first.size(); ++offset)
		{
			classified_undo_entry entry;
			entry.stack_position = range.marker_position + static_cast<int>(offset);
			entry.description = normalise_undo_entry_description(descriptions_oldest_first[offset]);
			entry.agent_created = undo_description_is_agent_created(descriptions_oldest_first[offset]);
			range.entries.push_back(std::move(entry));
		}

		return range;
	}

	// What one mutating tool's undo block did, in the terms its result payload needs.
	struct undo_block_report
	{
		// `undoDescription` from `messages/tool-result-undo.schema.json`.
		std::string undo_description;

		// `undoPositionBefore` — the turn's marker, which requirement 10.2 has every
		// mutating result in the turn report unchanged.
		int turn_undo_position_marker = 0;

		// The position before this particular block opened. Diagnostic: the server
		// keeps the first position it is told about and ignores the rest, so this is
		// for a log that has to explain a marker, not for the payload.
		int block_position_before = 0;

		// True when `Undo_EndBlock2` itself failed. The block is not left open.
		bool close_failed = false;

		// Requirement 9.9: a REAPER call that failed mid-block is reported as a
		// failed action with a reason, and the block is closed either way. Empty when
		// the work completed.
		std::string failure_reason;

		bool succeeded() const { return failure_reason.empty() && !close_failed; }
	};

	// What `revert_agent_changes` returns, matching
	// `mcp-tools/outputs/revert-agent-changes.schema.json` field for field.
	//
	// No `undoPositionBefore`, deliberately: this is a rewinding tool, and the schema
	// is explicit that a marker recorded for one would point past the producer's own
	// work.
	struct revert_outcome
	{
		// False when the revert was refused. A refusal is not a failure and not a
		// success — the operation was understood and deliberately not performed.
		bool performed = false;

		// Populated only on a refusal. When it is, nothing was undone and the stack
		// is exactly as the request found it.
		foreign_undo_refusal refusal;

		int reverted_entry_count = 0;

		// REAPER's descriptions in the order they were undone, so newest first.
		std::vector<std::string> undone_entry_descriptions;

		int undo_position_marker = 0;
		int undo_stack_position_after = 0;

		// Only ever true when a refusal named the producer's edits and they confirmed
		// those should go too.
		bool reverted_foreign_entries = false;

		// True when the stack stopped moving before it reached the marker. Not a
		// refusal and not a clean revert: some of the turn's work is still there, and
		// the two positions disagreeing is how the caller can tell.
		bool walk_stopped_short = false;
	};

	// What `undo_last_action` returns, matching
	// `mcp-tools/outputs/undo-last-action.schema.json`. Also a rewinding tool, so
	// also no `undoPositionBefore`.
	struct undo_last_action_outcome
	{
		bool performed = false;
		foreign_undo_refusal refusal;
		std::vector<std::string> undone_entry_descriptions;
		int undo_stack_position_after = 0;
		bool was_agent_entry = false;
	};

	// Owns both undo layers for one REAPER project.
	//
	// One instance per loaded extension, living as long as the connection does. The
	// turn boundary is `begin_turn`, driven by the Message Dispatcher rather than
	// inferred here — the extension is told when a turn starts, and guessing from
	// tool traffic would merge two turns whose tools happened to arrive together.
	class undo_manager
	{
	public:
		explicit undo_manager(undo_stack& stack)
			: stack_{stack}
		{
		}

		undo_manager(const undo_manager&) = delete;
		undo_manager& operator=(const undo_manager&) = delete;
		undo_manager(undo_manager&&) = delete;
		undo_manager& operator=(undo_manager&&) = delete;

		// Starts a turn with no marker (requirement 10.3 — a read-only turn never
		// acquires one) and forgets the previous turn's blocks.
		void begin_turn()
		{
			marker_.reset();
			descriptions_this_turn_.clear();
			undo_blocks_opened_this_turn_ = 0;
		}

		bool has_undo_position_marker() const { return marker_.has_value(); }

		std::optional<int> undo_position_marker() const { return marker_; }

		// The value a mutating tool result reports as `undoPositionBefore`. The
		// turn's marker, not this tool's own position: requirement 10.2 asks for the
		// marker reported unchanged on every subsequent mutating result.
		int turn_undo_position_marker() const { return marker_.value_or(0); }

		// Opens one identifiable undo block for a mutating tool, capturing the
		// turn's marker if this is the turn's first.
		//
		// The order inside is the requirement: the position is read before the block
		// opens, so the marker is where the stack stood before the agent touched
		// anything.
		[[nodiscard]] undo_block_guard open_undo_block(std::string_view tool_name)
		{
			capture_marker_if_first_mutation();

			std::string description = compose_agent_undo_description(tool_name);
			descriptions_this_turn_.push_back(description);
			++undo_blocks_opened_this_turn_;

			return undo_block_guard{stack_, std::move(description)};
		}

		// Runs a tool's mutations inside exactly one undo block and closes it however
		// the work ends.
		//
		// One block per call is what requirement 10.8 needs from an action array: the
		// array's loop goes inside `work`, so a five-target call produces one
		// `Undo_BeginBlock2` / `Undo_EndBlock2` pair and one entry in the producer's
		// undo history, rather than five steps they have to Ctrl+Z through
		// individually.
		//
		// A throwing `work` is caught rather than propagated, which is requirement
		// 9.9: the block closes, the action is reported failed with a reason, and the
		// exception does not travel up towards the REAPER timer callback it would be
		// undefined behaviour to unwind through. Per-action outcomes inside an array
		// are the Tool Executor's to report; what this guarantees is that the block
		// covering them is closed exactly once.
		template <typename MutatingWork>
		undo_block_report run_in_undo_block(std::string_view tool_name, MutatingWork&& work)
		{
			undo_block_report report;

			undo_block_guard guard = open_undo_block(tool_name);

			report.undo_description = guard.description();
			report.block_position_before = guard.position_before();
			report.turn_undo_position_marker = turn_undo_position_marker();

			try
			{
				work();
			}
			catch (const std::exception& failure)
			{
				report.failure_reason = failure.what();

				if (report.failure_reason.empty())
				{
					report.failure_reason = "The tool threw an exception with no message.";
				}
			}
			catch (...)
			{
				report.failure_reason = "The tool threw a non-standard exception.";
			}

			guard.close();
			report.close_failed = guard.close_failed();

			return report;
		}

		// The refusal path, written down rather than left as an absence.
		//
		// It does nothing, and that is the requirement (10.7): a refused mutation
		// changed nothing, so it records no marker. Naming the no-op gives the Tool
		// Executor's refusal branch something to call, and gives the suite something
		// to assert against — "the code that would have recorded a marker was never
		// written" is not a property a test can check, but "calling this leaves no
		// marker" is.
		void record_refused_mutation() const
		{
		}

		// Reads and classifies the turn's range. Changes nothing.
		//
		// A marker above the current position means the producer has already undone
		// past it by hand. Clamped rather than treated as an error: the agent's work
		// in that region is already gone, so the range above the cursor is genuinely
		// empty and there is nothing to refuse or revert.
		undo_range_classification classify_turn_undo_range()
		{
			const int current_position = stack_.current_position();
			const int safe_current = current_position < 0 ? 0 : current_position;

			int walk_back_target = marker_.value_or(safe_current);

			if (walk_back_target < 0)
			{
				walk_back_target = 0;
			}

			if (walk_back_target > safe_current)
			{
				walk_back_target = safe_current;
			}

			std::vector<std::string> descriptions;
			descriptions.reserve(static_cast<std::size_t>(safe_current - walk_back_target));

			for (int position = walk_back_target; position < safe_current; ++position)
			{
				descriptions.push_back(stack_.entry_description_at(position));
			}

			undo_range_classification range = classify_undo_range(walk_back_target, descriptions);
			range.turn_has_no_marker = !marker_.has_value();

			return range;
		}

		// "Revert all": walk the turn's range back to the marker, or refuse.
		//
		// The classification happens in full first and the refusal returns before the
		// first `undo_one_entry` call, so a refused revert performs zero undo actions
		// and leaves the stack byte-identical (requirement 10.5).
		//
		// `producer_acknowledged_foreign_undo` is the `confirmedForeignUndo` input,
		// set only after a refusal named the producer's edits and they said those
		// should go too. It permits rather than overrides: the entries are still
		// classified and `reverted_foreign_entries` records that it happened.
		revert_outcome revert_agent_changes(bool producer_acknowledged_foreign_undo = false)
		{
			revert_outcome outcome;

			const undo_range_classification range = classify_turn_undo_range();

			outcome.undo_position_marker = range.marker_position;
			outcome.undo_stack_position_after = range.current_position;

			if (range.contains_foreign_entries() && !producer_acknowledged_foreign_undo)
			{
				outcome.refusal = build_foreign_undo_refusal(range);
				return outcome;
			}

			outcome.performed = true;

			int position = range.current_position;

			while (position > range.marker_position)
			{
				if (!stack_.undo_one_entry())
				{
					break;
				}

				const int position_after = stack_.current_position();

				// A stack that did not move, or moved the wrong way, ends the walk.
				// Trusting the loop condition alone would spin forever against a
				// REAPER that reports success and changes nothing.
				if (position_after >= position)
				{
					break;
				}

				// Driven by the position REAPER actually reached rather than by
				// assuming one entry per call, so an undo that collapsed several
				// entries is reported as all of them instead of one.
				const int first_undone = position_after < range.marker_position
					? range.marker_position
					: position_after;

				for (int undone = position - 1; undone >= first_undone; --undone)
				{
					record_undone_entry(range, undone, outcome);
				}

				position = position_after;
			}

			outcome.undo_stack_position_after = stack_.current_position();
			outcome.walk_stopped_short = position > range.marker_position;

			// The turn's changes are gone, so the marker has nothing left to point at.
			// Kept when the walk stopped short, because agent work above the marker
			// still exists and is still worth offering to revert.
			if (!outcome.walk_stopped_short)
			{
				marker_.reset();
			}

			return outcome;
		}

		// Undoes the single most recent entry, or refuses when that entry is the
		// producer's own.
		//
		// The check is on the entry this tool would actually remove, which is the one
		// it can destroy. Requirements 10.5 and 10.6 put the range walk on "revert
		// all", and applying it here as well would refuse a single-step undo of the
		// agent's own top block because the producer had edited something earlier in
		// the turn — work this call does not touch. The escalation is the same either
		// way: a refusal naming the entry with REAPER's description, and a retry with
		// `confirmedForeignUndo` performing the undo.
		undo_last_action_outcome undo_last_action(bool producer_acknowledged_foreign_undo = false)
		{
			undo_last_action_outcome outcome;

			const int current_position = stack_.current_position();
			outcome.undo_stack_position_after = current_position < 0 ? 0 : current_position;

			if (current_position <= 0)
			{
				return outcome;
			}

			const int position_to_undo = current_position - 1;
			const std::string reaper_description = stack_.entry_description_at(position_to_undo);

			outcome.was_agent_entry = undo_description_is_agent_created(reaper_description);

			if (!outcome.was_agent_entry && !producer_acknowledged_foreign_undo)
			{
				undo_range_classification single_entry_range =
					classify_undo_range(position_to_undo, {reaper_description});
				outcome.refusal = build_foreign_undo_refusal(single_entry_range);

				return outcome;
			}

			if (!stack_.undo_one_entry())
			{
				return outcome;
			}

			outcome.performed = true;
			outcome.undone_entry_descriptions.push_back(
				normalise_undo_entry_description(reaper_description));

			const int position_after = stack_.current_position();
			outcome.undo_stack_position_after = position_after < 0 ? 0 : position_after;

			// A hand-driven undo can take the stack below the marker. Clamping keeps
			// the marker from pointing above the cursor, where a later revert would
			// read it as an empty range for the wrong reason.
			if (marker_.has_value() && *marker_ > outcome.undo_stack_position_after)
			{
				marker_ = outcome.undo_stack_position_after;
			}

			return outcome;
		}

		// The blocks this turn opened, in order. Design "State ownership" lists these
		// alongside the marker as the turn's state, and the chat UI lists them as what
		// the agent changed.
		const std::vector<std::string>& undo_descriptions_this_turn() const
		{
			return descriptions_this_turn_;
		}

		std::size_t undo_blocks_opened_this_turn() const { return undo_blocks_opened_this_turn_; }

	private:
		// Requirement 10.1 and 10.2 in three lines. Bound to opening a block, so
		// nothing that does not mutate can reach it.
		void capture_marker_if_first_mutation()
		{
			if (marker_.has_value())
			{
				return;
			}

			const int position = stack_.current_position();
			marker_ = position < 0 ? 0 : position;
		}

		// Appends one undone entry to the outcome, in REAPER's words.
		//
		// The description comes from the classification taken before the walk started.
		// Reading it from the stack afterwards would be reading a stack the undo has
		// already changed.
		static void record_undone_entry(
			const undo_range_classification& range,
			int position,
			revert_outcome& outcome)
		{
			const int offset = position - range.marker_position;

			if (offset < 0 || static_cast<std::size_t>(offset) >= range.entries.size())
			{
				return;
			}

			const classified_undo_entry& entry = range.entries[static_cast<std::size_t>(offset)];

			++outcome.reverted_entry_count;

			if (!entry.agent_created)
			{
				outcome.reverted_foreign_entries = true;
			}

			// The count is the total that was undone; the list is capped by the
			// schema. Counting past the cap rather than stopping keeps the count
			// honest about what happened to the session.
			if (outcome.undone_entry_descriptions.size() < maximum_undone_entry_description_count)
			{
				outcome.undone_entry_descriptions.push_back(entry.description);
			}
		}

		undo_stack& stack_;

		// Absent is the meaningful state, not a zero to be defaulted: position zero is
		// a real marker on a project with an empty undo history.
		std::optional<int> marker_;

		std::vector<std::string> descriptions_this_turn_;
		std::size_t undo_blocks_opened_this_turn_ = 0;
	};
}

#endif
