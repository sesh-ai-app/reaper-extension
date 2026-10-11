// Undo Manager — the two layers, and the check that stands between "revert all" and
// a producer's own work.
//
// The component's failure mode is a silent one, so the suite is built around staging
// the situations that produce it rather than around calling each method once. The
// scripted `undo_stack` below is a real undo stack with a real position and a real
// history, and it also counts undo calls — which is how "a refused revert performs no
// undo actions at all" (requirement 10.5) becomes something a test can assert rather
// than something the code appears to do.
//
// Nothing here touches REAPER. The interleavings, the `Undo_EndBlock2` that throws,
// and the stack that reports success and refuses to move are all cases that cannot be
// staged inside a DAW, and they are the cases worth having.
//
// Task 9.2 layers the formal property tests (16 through 20) on top. These are the
// examples and the exhaustive sweeps underneath them.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/undo_manager.h>

using sesh_ai::daw::classify_undo_range;
using sesh_ai::daw::compose_agent_undo_description;
using sesh_ai::daw::foreign_undo_acknowledgement_field;
using sesh_ai::daw::foreign_undo_refusal_reason;
using sesh_ai::daw::maximum_undo_description_length;
using sesh_ai::daw::revert_outcome;
using sesh_ai::daw::undo_all_states;
using sesh_ai::daw::undo_block_guard;
using sesh_ai::daw::undo_block_report;
using sesh_ai::daw::undo_description_is_agent_created;
using sesh_ai::daw::undo_entry_blocking_kind;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_range_classification;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::unnamed_undo_entry_description;

namespace
{
	// A scripted REAPER undo stack.
	//
	// It behaves like the real one: entries accumulate as blocks close, the position
	// tracks the history, and an undo walks it back. What it adds is the ability to
	// pre-seed the producer's own edits anywhere in the history, to refuse to move, to
	// throw from `Undo_EndBlock2`, and to count everything.
	class scripted_undo_stack final : public undo_stack
	{
	public:
		// Pre-existing history, oldest first. The position starts at the top of it,
		// which is where REAPER would be sitting.
		void seed_entries(std::vector<std::string> descriptions)
		{
			entries_ = std::move(descriptions);
			position_ = static_cast<int>(entries_.size());
		}

		// The producer reaching into their own session mid-turn — a fader nudge, an
		// item drag. Records the entry REAPER would record, without going through the
		// block counters, so a test asserting on the agent's blocks is not measuring
		// the producer's edits too.
		void record_producer_entry(const std::string& description)
		{
			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		int current_position() override
		{
			++current_position_call_count;

			return position_;
		}

		std::string entry_description_at(int position) override
		{
			++description_call_count;

			if (position < 0 || static_cast<std::size_t>(position) >= entries_.size())
			{
				return {};
			}

			return entries_[static_cast<std::size_t>(position)];
		}

		void begin_block() override
		{
			++begin_block_call_count;
			++open_block_depth;

			if (open_block_depth > deepest_open_block_depth)
			{
				deepest_open_block_depth = open_block_depth;
			}
		}

		void end_block(const std::string& description, int extra_flags) override
		{
			++end_block_call_count;
			--open_block_depth;
			end_block_descriptions.push_back(description);
			end_block_extra_flags.push_back(extra_flags);

			if (throw_from_end_block)
			{
				throw std::runtime_error("REAPER refused to close the block");
			}

			// An entry lands at the position the stack was at, and the position moves
			// up — which is the convention `undo_stack` documents.
			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			++undo_call_count;

			if (refuse_to_undo || position_ <= 0)
			{
				return false;
			}

			if (report_success_without_moving)
			{
				return true;
			}

			position_ -= entries_undone_per_call > 0 ? entries_undone_per_call : 1;

			if (position_ < 0)
			{
				position_ = 0;
			}

			return true;
		}

		const std::vector<std::string>& entries() const { return entries_; }

		int position() const { return position_; }

		// Everything a test needs to make a claim about what was done to the stack
		// rather than only about what was returned.
		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int description_call_count = 0;
		int current_position_call_count = 0;
		int open_block_depth = 0;
		int deepest_open_block_depth = 0;

		std::vector<std::string> end_block_descriptions;
		std::vector<int> end_block_extra_flags;

		bool throw_from_end_block = false;
		bool refuse_to_undo = false;
		bool report_success_without_moving = false;
		int entries_undone_per_call = 1;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	std::string agent_entry(const std::string& tool_name)
	{
		return compose_agent_undo_description(tool_name);
	}

	// Applies one mutating tool the way the Tool Executor will: one block, one change.
	undo_block_report apply_mutating_tool(undo_manager& manager, const std::string& tool_name)
	{
		return manager.run_in_undo_block(tool_name, [] {});
	}
}

// ---------------------------------------------------------------------------
// Description tagging (requirement 10.4)
// ---------------------------------------------------------------------------

TEST_CASE("an undo block description names the agent and the tool", "[undo][description]")
{
	REQUIRE(compose_agent_undo_description("set_track_state") == "Sesh AI: set_track_state");
	REQUIRE(undo_description_is_agent_created(compose_agent_undo_description("set_track_state")));
}

TEST_CASE("classification recognises only the exact agent prefix", "[undo][description]")
{
	REQUIRE(undo_description_is_agent_created("Sesh AI: create_track"));
	REQUIRE(undo_description_is_agent_created("Sesh AI:"));

	// Every one of these is the producer's, or something whose origin cannot be
	// established. Reading any of them as the agent's is how their work gets
	// destroyed, so the match is exact rather than forgiving.
	REQUIRE_FALSE(undo_description_is_agent_created(""));
	REQUIRE_FALSE(undo_description_is_agent_created("Move media items"));
	REQUIRE_FALSE(undo_description_is_agent_created("sesh ai: create_track"));
	REQUIRE_FALSE(undo_description_is_agent_created(" Sesh AI: create_track"));
	REQUIRE_FALSE(undo_description_is_agent_created("SESH AI: create_track"));
	REQUIRE_FALSE(undo_description_is_agent_created("Sesh"));
	REQUIRE_FALSE(undo_description_is_agent_created("Undo Sesh AI: create_track"));
}

TEST_CASE("a missing tool name still produces a tagged description", "[undo][description]")
{
	// An untagged block would be read as the producer's work on the next revert — the
	// agent's own entry blocking the producer's recovery.
	const std::string description = compose_agent_undo_description("");

	REQUIRE(undo_description_is_agent_created(description));
	REQUIRE(description == "Sesh AI: unnamed tool");
}

TEST_CASE("a description is capped at the schema's length and keeps its prefix", "[undo][description]")
{
	const std::string description = compose_agent_undo_description(std::string(2000, 'x'));

	REQUIRE(description.size() == maximum_undo_description_length);
	REQUIRE(undo_description_is_agent_created(description));
}

// ---------------------------------------------------------------------------
// The per-tool layer: blocks that always close (requirements 9.9, 23.8)
// ---------------------------------------------------------------------------

TEST_CASE("an undo block opens and closes around one tool", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	const undo_block_report report = apply_mutating_tool(manager, "create_marker");

	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);
	REQUIRE(stack.end_block_descriptions == std::vector<std::string>{"Sesh AI: create_marker"});
	REQUIRE(stack.end_block_extra_flags == std::vector<int>{undo_all_states});
	REQUIRE(report.succeeded());
	REQUIRE(report.undo_description == "Sesh AI: create_marker");
}

TEST_CASE("the guard closes the block when the scope is left by an exception", "[undo][block]")
{
	scripted_undo_stack stack;

	{
		undo_manager manager{stack};

		// A tool that throws past its own return. The block must still close: one left
		// open in REAPER accumulates every subsequent producer edit into the agent's
		// entry.
		try
		{
			undo_block_guard guard = manager.open_undo_block("delete_track");
			throw std::runtime_error("the track vanished mid-operation");
		}
		catch (const std::runtime_error&)
		{
		}
	}

	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);
}

TEST_CASE("work that throws is reported as a failure with the block closed", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	const undo_block_report report = manager.run_in_undo_block("set_track_state", [] {
		throw std::runtime_error("SetMediaTrackInfo_Value failed");
	});

	// Requirement 9.9: the block closes, the action is reported failed with a reason,
	// and nothing unwinds towards the REAPER timer callback.
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);
	REQUIRE_FALSE(report.succeeded());
	REQUIRE(report.failure_reason == "SetMediaTrackInfo_Value failed");
}

TEST_CASE("work that throws a non-standard exception still reports a reason", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	const undo_block_report report = manager.run_in_undo_block("create_send", [] {
		throw 17;
	});

	// A reasonless failure is not a representable result (requirement 9.5).
	REQUIRE_FALSE(report.failure_reason.empty());
	REQUIRE(stack.end_block_call_count == 1);
}

TEST_CASE("a close that itself errors is attempted exactly once and reported", "[undo][block]")
{
	scripted_undo_stack stack;
	stack.throw_from_end_block = true;

	undo_manager manager{stack};

	const undo_block_report report = apply_mutating_tool(manager, "create_track");

	// Exactly one close attempt per open. A second would be a second Undo_EndBlock2
	// against one Undo_BeginBlock2, which unbalances REAPER's own refcount — so the
	// destructor must not retry what close() already tried.
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(report.close_failed);
	REQUIRE_FALSE(report.succeeded());
}

TEST_CASE("an explicit close makes the destructor a no-op", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	{
		undo_block_guard guard = manager.open_undo_block("rename_track");

		REQUIRE(guard.is_open());

		guard.close();

		REQUIRE_FALSE(guard.is_open());
		REQUIRE(stack.end_block_call_count == 1);
	}

	REQUIRE(stack.end_block_call_count == 1);
}

TEST_CASE("a moved guard closes once, from its new owner", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	{
		undo_block_guard moved_from = manager.open_undo_block("create_bus");
		undo_block_guard moved_to = std::move(moved_from);

		REQUIRE(moved_to.is_open());
		REQUIRE(stack.end_block_call_count == 0);
	}

	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);
}

TEST_CASE("assigning over a live guard closes the block it held", "[undo][block]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	{
		undo_block_guard guard = manager.open_undo_block("first_tool");
		guard = manager.open_undo_block("second_tool");

		// The first block is closed rather than abandoned — assignment must not be a
		// way to leak one.
		REQUIRE(stack.end_block_call_count == 1);
	}

	REQUIRE(stack.end_block_call_count == 2);
	REQUIRE(stack.open_block_depth == 0);
	REQUIRE(stack.end_block_descriptions
		== std::vector<std::string>{"Sesh AI: first_tool", "Sesh AI: second_tool"});
}

// ---------------------------------------------------------------------------
// Action arrays: one block for the whole array (requirement 10.8)
// ---------------------------------------------------------------------------

TEST_CASE("an action array produces exactly one undo block", "[undo][action-array]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	const std::vector<std::string> targets{"Kick", "Snare", "Hats", "Overheads", "Room"};
	std::vector<std::string> applied;

	const undo_block_report report = manager.run_in_undo_block("set_track_state", [&] {
		for (const std::string& target : targets)
		{
			applied.push_back(target);
		}
	});

	REQUIRE(applied.size() == targets.size());

	// Five targets, one Ctrl+Z step for the producer.
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.entries().size() == 1);
	REQUIRE(manager.undo_blocks_opened_this_turn() == 1);
	REQUIRE(report.succeeded());
}

TEST_CASE("an array whose action fails still produces one block covering what landed",
	"[undo][action-array]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	const std::vector<std::string> targets{"Kick", "Snare", "Hats"};
	std::vector<std::string> applied;
	std::vector<std::string> failed;

	const undo_block_report report = manager.run_in_undo_block("set_track_state", [&] {
		for (const std::string& target : targets)
		{
			// A locked track is a per-action failure, not an exception: requirement 9.4
			// keeps the siblings that landed and requirement 10.8 covers them with one
			// entry.
			if (target == "Snare")
			{
				failed.push_back(target);
				continue;
			}

			applied.push_back(target);
		}
	});

	REQUIRE(applied.size() == 2);
	REQUIRE(failed.size() == 1);

	// One block over a partial array. The successes are not rolled back, and the
	// producer gets one undo step covering them.
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.entries().size() == 1);
	REQUIRE(report.succeeded());
}

// ---------------------------------------------------------------------------
// The marker layer (requirements 10.1, 10.2, 10.3, 10.7)
// ---------------------------------------------------------------------------

TEST_CASE("a read-only turn records no marker", "[undo][marker]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader"});

	undo_manager manager{stack};
	manager.begin_turn();

	// list_tracks, get_track_state, list_selected_items — nothing opens a block.
	REQUIRE_FALSE(manager.has_undo_position_marker());
	REQUIRE(stack.begin_block_call_count == 0);
}

TEST_CASE("the marker is captured before the turn's first mutation", "[undo][marker]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader", "Trim item"});

	undo_manager manager{stack};
	manager.begin_turn();

	const undo_block_report report = apply_mutating_tool(manager, "create_track");

	REQUIRE(manager.has_undo_position_marker());
	REQUIRE(manager.undo_position_marker() == 3);
	REQUIRE(report.turn_undo_position_marker == 3);
}

TEST_CASE("the marker stays where the first mutation put it", "[undo][marker]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	const undo_block_report first = apply_mutating_tool(manager, "create_track");
	const undo_block_report second = apply_mutating_tool(manager, "add_fx");
	const undo_block_report third = apply_mutating_tool(manager, "create_send");

	// Requirement 10.2: every subsequent mutating result reports the same marker, not
	// the position it happened to find.
	REQUIRE(first.turn_undo_position_marker == 1);
	REQUIRE(second.turn_undo_position_marker == 1);
	REQUIRE(third.turn_undo_position_marker == 1);
	REQUIRE(manager.undo_position_marker() == 1);

	// And the per-tool positions really did differ, so the equality above is the
	// marker holding still rather than the stack not moving.
	REQUIRE(first.block_position_before == 1);
	REQUIRE(second.block_position_before == 2);
	REQUIRE(third.block_position_before == 3);
}

TEST_CASE("a refused mutation records no marker", "[undo][marker]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader"});

	undo_manager manager{stack};
	manager.begin_turn();

	// An unresolvable track selector, a render path collision, a send that would close
	// a loop: the tool returns a refusal and never opens a block.
	manager.record_refused_mutation();

	REQUIRE_FALSE(manager.has_undo_position_marker());
	REQUIRE(stack.begin_block_call_count == 0);
}

TEST_CASE("a refusal mid-turn does not shorten the range revert-all walks", "[undo][marker]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	// Requirement 10.7's actual consequence. A marker recorded here would sit at
	// position 2, above the two blocks that already landed, and "revert all" would
	// then walk back to 2 — leaving the agent's own work in place and silently making
	// it unrecoverable for the rest of the turn.
	manager.record_refused_mutation();

	apply_mutating_tool(manager, "create_send");

	REQUIRE(manager.undo_position_marker() == 0);

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE(outcome.reverted_entry_count == 3);
	REQUIRE(outcome.undo_stack_position_after == 0);
}

TEST_CASE("a new turn starts with no marker", "[undo][marker]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};

	manager.begin_turn();
	apply_mutating_tool(manager, "create_track");

	REQUIRE(manager.has_undo_position_marker());

	manager.begin_turn();

	REQUIRE_FALSE(manager.has_undo_position_marker());
	REQUIRE(manager.undo_descriptions_this_turn().empty());
	REQUIRE(manager.undo_blocks_opened_this_turn() == 0);
}

TEST_CASE("position zero is a real marker rather than an absent one", "[undo][marker]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	// An empty undo history is where a fresh project starts, so "no marker" cannot be
	// represented as zero.
	REQUIRE(manager.has_undo_position_marker());
	REQUIRE(manager.undo_position_marker() == 0);
}

// ---------------------------------------------------------------------------
// Range classification (requirements 10.5, 10.6)
// ---------------------------------------------------------------------------

TEST_CASE("classification places each entry at its stack position", "[undo][range]")
{
	const undo_range_classification range = classify_undo_range(7, {
		agent_entry("create_track"),
		"Adjust fader",
		agent_entry("add_fx"),
	});

	REQUIRE(range.marker_position == 7);
	REQUIRE(range.current_position == 10);
	REQUIRE(range.entries.size() == 3);
	REQUIRE(range.entries[0].stack_position == 7);
	REQUIRE(range.entries[1].stack_position == 8);
	REQUIRE(range.entries[2].stack_position == 9);
	REQUIRE(range.entries[0].agent_created);
	REQUIRE_FALSE(range.entries[1].agent_created);
	REQUIRE(range.entries[2].agent_created);
}

TEST_CASE("an entry REAPER cannot name is classified foreign and still named", "[undo][range]")
{
	const undo_range_classification range = classify_undo_range(0, {agent_entry("create_track"), ""});

	REQUIRE(range.contains_foreign_entries());
	REQUIRE_FALSE(range.entries[1].agent_created);

	// The schemas require a non-empty description, and an entry nobody can identify is
	// still one the producer needs told about.
	REQUIRE(range.entries[1].description == std::string{unnamed_undo_entry_description});
}

TEST_CASE("a foreign entry is detected at every position in the range", "[undo][range]")
{
	// Requirement 10.6, swept rather than sampled. A check that looks only at the top
	// of the stack passes the last of these and fails every earlier one, and the top
	// is the position a real interleaving is least likely to be at.
	constexpr std::size_t range_length = 8;

	for (std::size_t foreign_position = 0; foreign_position < range_length; ++foreign_position)
	{
		std::vector<std::string> descriptions;

		for (std::size_t position = 0; position < range_length; ++position)
		{
			descriptions.push_back(position == foreign_position
				? "Adjust fader"
				: agent_entry("set_track_state"));
		}

		const undo_range_classification range = classify_undo_range(0, descriptions);

		REQUIRE(range.contains_foreign_entries());
		REQUIRE(range.foreign_entries().size() == 1);
		REQUIRE(range.foreign_entries().front().stack_position == static_cast<int>(foreign_position));
	}
}

TEST_CASE("a range of only agent entries is clean at every length", "[undo][range]")
{
	// The negative control. Without it every detection test above would pass against
	// an implementation that called everything foreign — which would refuse every
	// revert and look perfectly safe.
	for (std::size_t length = 0; length <= 16; ++length)
	{
		std::vector<std::string> descriptions;

		for (std::size_t position = 0; position < length; ++position)
		{
			descriptions.push_back(agent_entry("set_track_state"));
		}

		const undo_range_classification range = classify_undo_range(3, descriptions);

		REQUIRE_FALSE(range.contains_foreign_entries());
		REQUIRE(range.foreign_entries().empty());
		REQUIRE(range.entry_count() == length);
	}
}

TEST_CASE("the range walked is the turn's, not the whole undo history", "[undo][range]")
{
	scripted_undo_stack stack;

	// The producer's afternoon, before the turn started. None of it is in the range.
	stack.seed_entries({"Move media items", "Adjust fader", "Trim item"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	const undo_range_classification range = manager.classify_turn_undo_range();

	REQUIRE(range.marker_position == 3);
	REQUIRE(range.current_position == 5);
	REQUIRE(range.entry_count() == 2);
	REQUIRE_FALSE(range.contains_foreign_entries());
}

TEST_CASE("a turn with no marker classifies an empty range and says so", "[undo][range]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	const undo_range_classification range = manager.classify_turn_undo_range();

	REQUIRE(range.turn_has_no_marker);
	REQUIRE(range.entry_count() == 0);
	REQUIRE(range.marker_position == range.current_position);
}

// ---------------------------------------------------------------------------
// Revert all (requirements 10.5, 10.6)
// ---------------------------------------------------------------------------

TEST_CASE("revert all walks the turn's blocks back to the marker", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");
	apply_mutating_tool(manager, "create_send");

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE_FALSE(outcome.refusal.refused);
	REQUIRE(outcome.reverted_entry_count == 3);
	REQUIRE(outcome.undo_position_marker == 2);
	REQUIRE(outcome.undo_stack_position_after == 2);
	REQUIRE_FALSE(outcome.reverted_foreign_entries);
	REQUIRE_FALSE(outcome.walk_stopped_short);

	// REAPER's own descriptions, in the order they were undone — newest first.
	REQUIRE(outcome.undone_entry_descriptions == std::vector<std::string>{
		"Sesh AI: create_send",
		"Sesh AI: add_fx",
		"Sesh AI: create_track",
	});

	// The producer's afternoon is untouched.
	REQUIRE(stack.position() == 2);
	REQUIRE(stack.undo_call_count == 3);
}

TEST_CASE("the marker and the position reached agree when the revert completed", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	const revert_outcome outcome = manager.revert_agent_changes();

	// The schema calls the two agreeing the evidence the revert completed rather than
	// stopping partway.
	REQUIRE(outcome.undo_position_marker == outcome.undo_stack_position_after);
}

TEST_CASE("a turn that changed nothing reverts zero entries and is not a failure", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader"});

	undo_manager manager{stack};
	manager.begin_turn();

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE(outcome.reverted_entry_count == 0);
	REQUIRE(outcome.undone_entry_descriptions.empty());
	REQUIRE(outcome.undo_position_marker == 2);
	REQUIRE(outcome.undo_stack_position_after == 2);
	REQUIRE(stack.undo_call_count == 0);
}

TEST_CASE("a foreign entry blocks the revert and nothing is undone", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	// The producer nudges a fader while the agent is working. This is the normal case
	// for someone working alongside an assistant, not an edge case.
	stack.record_producer_entry("Adjust fader volume");

	apply_mutating_tool(manager, "add_fx");

	const int position_before_request = stack.position();
	const std::vector<std::string> history_before_request = stack.entries();

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE_FALSE(outcome.performed);
	REQUIRE(outcome.refusal.refused);
	REQUIRE(outcome.refusal.reason == std::string{foreign_undo_refusal_reason});
	REQUIRE(outcome.refusal.acknowledgement_field == std::string{foreign_undo_acknowledgement_field});

	// Named with REAPER's own words, so what the producer is told matches what they
	// would have seen in REAPER's undo history.
	REQUIRE(outcome.refusal.blocking.size() == 1);
	REQUIRE(outcome.refusal.blocking.front().kind == std::string{undo_entry_blocking_kind});
	REQUIRE(outcome.refusal.blocking.front().description == "Adjust fader volume");

	// Requirement 10.5, the part that is easy to implement away: zero undo actions,
	// and the stack exactly as the request found it.
	REQUIRE(stack.undo_call_count == 0);
	REQUIRE(stack.position() == position_before_request);
	REQUIRE(stack.entries() == history_before_request);
	REQUIRE(outcome.reverted_entry_count == 0);
	REQUIRE(outcome.undone_entry_descriptions.empty());
}

TEST_CASE("only the producer's entries are named as blocking", "[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	stack.record_producer_entry("Adjust fader volume");
	apply_mutating_tool(manager, "add_fx");
	stack.record_producer_entry("Move media items");
	apply_mutating_tool(manager, "create_send");

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.refusal.refused);

	// The agent's three blocks are not what is blocking the call, and listing them
	// would bury the two lines the producer needs to recognise as theirs.
	REQUIRE(outcome.refusal.blocking.size() == 2);
	REQUIRE(outcome.refusal.blocking[0].description == "Adjust fader volume");
	REQUIRE(outcome.refusal.blocking[1].description == "Move media items");
}

TEST_CASE("a foreign entry blocks the revert from any position in the range", "[undo][revert]")
{
	// Requirement 10.6 against the whole component rather than the classifier alone.
	//
	// The producer's edit is moved through every position the turn's range can hold it,
	// from just above the marker to the very top of the stack. A detector that checks
	// the top passes the last iteration and fails every earlier one — and the top is
	// where an interleaved edit is least likely to be, because the agent's own most
	// recent block is usually sitting there.
	constexpr int agent_block_count = 6;

	for (int foreign_offset = 1; foreign_offset <= agent_block_count; ++foreign_offset)
	{
		scripted_undo_stack stack;
		stack.seed_entries({"Earlier producer work"});

		undo_manager manager{stack};
		manager.begin_turn();

		for (int block = 0; block < agent_block_count; ++block)
		{
			apply_mutating_tool(manager, "set_track_state");

			if (block + 1 == foreign_offset)
			{
				stack.record_producer_entry("Adjust fader volume");
			}
		}

		const int position_before_request = stack.position();
		const std::vector<std::string> history_before_request = stack.entries();

		const revert_outcome outcome = manager.revert_agent_changes();

		REQUIRE(outcome.refusal.refused);
		REQUIRE(outcome.refusal.blocking.size() == 1);
		REQUIRE(stack.undo_call_count == 0);
		REQUIRE(stack.position() == position_before_request);
		REQUIRE(stack.entries() == history_before_request);
	}
}

TEST_CASE("an acknowledged revert proceeds and records that it crossed the producer's work",
	"[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	stack.record_producer_entry("Adjust fader volume");
	apply_mutating_tool(manager, "add_fx");

	const revert_outcome outcome = manager.revert_agent_changes(true);

	REQUIRE(outcome.performed);
	REQUIRE_FALSE(outcome.refusal.refused);
	REQUIRE(outcome.reverted_entry_count == 3);

	// The record of an authorised revert past someone's work, not something that
	// happens by default.
	REQUIRE(outcome.reverted_foreign_entries);
	REQUIRE(outcome.undone_entry_descriptions == std::vector<std::string>{
		"Sesh AI: add_fx",
		"Adjust fader volume",
		"Sesh AI: create_track",
	});
	REQUIRE(stack.position() == 0);
}

TEST_CASE("a clean revert reports that it crossed nothing of the producer's", "[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	// The acknowledgement permits; it does not invent foreign entries where there are
	// none.
	const revert_outcome outcome = manager.revert_agent_changes(true);

	REQUIRE(outcome.performed);
	REQUIRE_FALSE(outcome.reverted_foreign_entries);
}

TEST_CASE("a stack that will not move stops the walk instead of spinning", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	stack.refuse_to_undo = true;

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE(outcome.walk_stopped_short);
	REQUIRE(outcome.reverted_entry_count == 0);
	REQUIRE(outcome.undo_stack_position_after == 3);
	REQUIRE(stack.undo_call_count == 1);

	// Agent work above the marker still exists, so the marker still has something to
	// point at.
	REQUIRE(manager.has_undo_position_marker());
}

TEST_CASE("a stack that reports success without moving stops the walk", "[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	stack.report_success_without_moving = true;

	const revert_outcome outcome = manager.revert_agent_changes();

	// The loop condition alone would spin forever here, which inside REAPER's timer
	// callback is a hung DAW.
	REQUIRE(outcome.walk_stopped_short);
	REQUIRE(stack.undo_call_count == 1);
}

TEST_CASE("an undo that collapses several entries reports all of them", "[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");
	apply_mutating_tool(manager, "create_send");
	apply_mutating_tool(manager, "rename_track");

	stack.entries_undone_per_call = 2;

	const revert_outcome outcome = manager.revert_agent_changes();

	// Driven by the position REAPER actually reached rather than by assuming one entry
	// per call, so the producer is told about everything that went.
	REQUIRE(outcome.performed);
	REQUIRE(outcome.reverted_entry_count == 4);
	REQUIRE(outcome.undone_entry_descriptions.size() == 4);
	REQUIRE(outcome.undo_stack_position_after == 0);
	REQUIRE(stack.undo_call_count == 2);
}

TEST_CASE("a completed revert leaves the turn with nothing more to revert", "[undo][revert]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	REQUIRE(manager.revert_agent_changes().performed);
	REQUIRE_FALSE(manager.has_undo_position_marker());

	const revert_outcome second = manager.revert_agent_changes();

	REQUIRE(second.performed);
	REQUIRE(second.reverted_entry_count == 0);
}

TEST_CASE("a revert records no marker of its own", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	// A rewinding tool in a turn that mutated nothing. Recording a marker here would
	// point at a position above the producer's own work.
	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE_FALSE(manager.has_undo_position_marker());
	REQUIRE(stack.begin_block_call_count == 0);
}

TEST_CASE("a marker the producer has already undone past is clamped, not trusted", "[undo][revert]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader", "Trim item"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	REQUIRE(manager.undo_position_marker() == 3);

	// The producer hits Ctrl+Z five times themselves, taking the stack well below the
	// marker. The agent's work in that region is already gone, so the range above the
	// cursor is genuinely empty — and a marker left pointing above the cursor would
	// make the walk's loop condition false for the wrong reason.
	stack.seed_entries({});

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE(outcome.performed);
	REQUIRE(outcome.reverted_entry_count == 0);
	REQUIRE(outcome.undo_position_marker == 0);
	REQUIRE(outcome.undo_stack_position_after == 0);
	REQUIRE(stack.undo_call_count == 0);
}

// ---------------------------------------------------------------------------
// Undo last action
// ---------------------------------------------------------------------------

TEST_CASE("undo last action reverts the agent's own top entry", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Move media items"});

	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");

	const auto outcome = manager.undo_last_action();

	REQUIRE(outcome.performed);
	REQUIRE(outcome.was_agent_entry);
	REQUIRE(outcome.undone_entry_descriptions == std::vector<std::string>{"Sesh AI: create_track"});
	REQUIRE(outcome.undo_stack_position_after == 1);
	REQUIRE(stack.position() == 1);
}

TEST_CASE("undo last action refuses when the top entry is the producer's", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Sesh AI: create_track", "Adjust fader volume"});

	undo_manager manager{stack};
	manager.begin_turn();

	const auto outcome = manager.undo_last_action();

	REQUIRE_FALSE(outcome.performed);
	REQUIRE_FALSE(outcome.was_agent_entry);
	REQUIRE(outcome.refusal.refused);
	REQUIRE(outcome.refusal.reason == std::string{foreign_undo_refusal_reason});
	REQUIRE(outcome.refusal.acknowledgement_field == std::string{foreign_undo_acknowledgement_field});
	REQUIRE(outcome.refusal.blocking.size() == 1);
	REQUIRE(outcome.refusal.blocking.front().description == "Adjust fader volume");

	// Nothing undone, stack untouched.
	REQUIRE(stack.undo_call_count == 0);
	REQUIRE(stack.position() == 2);
}

TEST_CASE("an acknowledged undo last action proceeds past the producer's entry", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Adjust fader volume"});

	undo_manager manager{stack};
	manager.begin_turn();

	const auto outcome = manager.undo_last_action(true);

	REQUIRE(outcome.performed);
	REQUIRE_FALSE(outcome.was_agent_entry);
	REQUIRE(outcome.undone_entry_descriptions == std::vector<std::string>{"Adjust fader volume"});
	REQUIRE(stack.position() == 0);
}

TEST_CASE("undo last action on an empty history does nothing", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	const auto outcome = manager.undo_last_action();

	REQUIRE_FALSE(outcome.performed);
	REQUIRE_FALSE(outcome.refusal.refused);
	REQUIRE(outcome.undo_stack_position_after == 0);
	REQUIRE(outcome.undone_entry_descriptions.empty());
	REQUIRE(stack.undo_call_count == 0);
}

TEST_CASE("undo last action records no marker", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	stack.seed_entries({"Sesh AI: create_track"});

	undo_manager manager{stack};
	manager.begin_turn();

	const auto outcome = manager.undo_last_action();

	REQUIRE(outcome.performed);

	// Requirement 10.7 for rewinding tools: undo-last-action.schema.json carries no
	// undoPositionBefore at all, so there is nothing for a marker to be reported as.
	REQUIRE_FALSE(manager.has_undo_position_marker());
	REQUIRE(stack.begin_block_call_count == 0);
}

TEST_CASE("undo last action keeps the marker from floating above the cursor", "[undo][undo-last]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	REQUIRE(manager.undo_position_marker() == 0);

	// Two hand-driven undos take the stack back to the marker. The marker is still
	// valid; the range above it is simply empty.
	REQUIRE(manager.undo_last_action().performed);
	REQUIRE(manager.undo_last_action().performed);

	REQUIRE(manager.undo_position_marker() == 0);
	REQUIRE(manager.classify_turn_undo_range().entry_count() == 0);
}

// ---------------------------------------------------------------------------
// Turn bookkeeping
// ---------------------------------------------------------------------------

TEST_CASE("the turn records the descriptions of the blocks it created", "[undo][turn]")
{
	scripted_undo_stack stack;
	undo_manager manager{stack};
	manager.begin_turn();

	apply_mutating_tool(manager, "create_track");
	apply_mutating_tool(manager, "add_fx");

	// Design "State ownership" lists these alongside the marker as the turn's state;
	// the chat UI lists them as what the agent changed.
	REQUIRE(manager.undo_descriptions_this_turn() == std::vector<std::string>{
		"Sesh AI: create_track",
		"Sesh AI: add_fx",
	});
	REQUIRE(manager.undo_blocks_opened_this_turn() == 2);
}
