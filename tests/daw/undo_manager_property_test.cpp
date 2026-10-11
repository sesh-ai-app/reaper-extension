// Undo Manager — Properties 16, 17, 18, 19, and 20 of the design, stated as properties.
//
// `undo_manager_test.cpp` beside this file is the example suite, and it is already
// thorough about the situations that produce this component's failure mode. What it
// covers, so that nothing below is a second copy of it under a property name:
//
//   - **One foreign entry swept through every position.** At the classifier level,
//     eight positions in an eight-entry range. At the whole-component level, the
//     producer's edit moved through every one of six offsets in a six-block turn,
//     each iteration asserting a refusal, zero undo calls, and a byte-identical
//     history.
//   - **A clean range at every length** from zero to sixteen, which is the negative
//     control against an implementation that calls everything foreign.
//   - **The marker** captured before the first mutation, stable across three
//     mutations with three genuinely different per-tool positions, absent for a
//     read-only turn, absent after a refusal, and not shortened by one refusal
//     placed between two mutations.
//   - **An action array** of five targets producing one block, and one array with a
//     single failing sibling producing one block.
//   - The stack that will not move, the stack that lies about moving, the
//     `Undo_EndBlock2` that throws, the collapsed multi-entry undo, the clamped
//     marker, and `undo_last_action` in each of its branches.
//
// Every one of those is a chosen case. The design's properties are quantified over
// *any* range and *any* turn, and the three things the examples cannot reach are
// exactly where this component's real failure lives:
//
//   - **Arbitrary interleavings, not one foreign entry at a time.** Properties 16
//     and 17 are one claim quantified over the number and the arrangement of the
//     producer's edits, not just over the position of a single one. A producer
//     working alongside the assistant makes two edits, or five, or two adjacent
//     ones; the sweep below enumerates every arrangement of agent blocks and
//     producer edits a turn of up to eight entries can hold — 510 turns at the
//     component level, 1533 classifications at the pure-function level — and
//     asserts the refusal, the zero undo actions, the byte-identical stack, and the
//     naming for all of them.
//
//   - **Arbitrary turn shapes for the marker.** Property 19 is about a marker that
//     holds still through whatever the turn happens to contain. Three mutations in
//     a row is the easy shape. The sweep below enumerates every sequence of up to
//     six steps drawn from reads, mutations, action arrays, and refusals — 5461
//     sequences, run at two different starting stack positions — and checks the
//     marker against the position the stack actually stood at before the first
//     mutating step.
//
//   - **Property 18 as an equivalence rather than one example.** "A refusal records
//     no marker" is only interesting because of what it protects: the range a later
//     "revert all" walks. So each of those turns is run twice, once with its
//     refusals and once with them removed, and the marker, the range, and the
//     resulting history are required to be identical. A refusal that shortened the
//     range would show up as a disagreement no matter where in the turn it sat.
//
// The negative controls are not optional here. Every detection property in this file
// would pass against an implementation that refused every revert, which would look
// perfectly safe and would make the tool useless. So the interleaving sweep also
// quantifies over the arrangements with no foreign entry at all and requires the
// revert to proceed and to revert exactly the expected entries, and it retries every
// refused turn with the producer's acknowledgement and requires that one to proceed
// too.
//
// No new dependency and no change to the build: Catch2 in this build has no generator
// library, so the corpora are enumerated where the space is small enough to enumerate
// whole, following `tests/daw/folder_invariant_keeper_test.cpp` and
// `tests/cycle_detector_property_test.cpp`. Every sweep counts what it generated and
// asserts the count, because a sweep that silently stopped enumerating would make all
// of this vacuously true. The counts below are the measured values.
//
// Nothing here touches REAPER.
//
// **Validates: Requirements 10.1, 10.2, 10.5, 10.6, 10.7, 10.8**

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/undo_manager.h>

using sesh_ai::daw::build_foreign_undo_refusal;
using sesh_ai::daw::blocking_undo_entry;
using sesh_ai::daw::classified_undo_entry;
using sesh_ai::daw::classify_undo_range;
using sesh_ai::daw::foreign_undo_acknowledgement_field;
using sesh_ai::daw::foreign_undo_refusal;
using sesh_ai::daw::foreign_undo_refusal_reason;
using sesh_ai::daw::revert_outcome;
using sesh_ai::daw::undo_block_report;
using sesh_ai::daw::undo_entry_blocking_kind;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_range_classification;
using sesh_ai::daw::undo_stack;

namespace
{
	// A scripted REAPER undo stack.
	//
	// A second copy of the one in `undo_manager_test.cpp` rather than a shared header,
	// because that one is translation-unit local and the two files are being written
	// by separate pieces of work. It behaves the same way: entries accumulate as
	// blocks close, an entry lands at the position the stack was at, and the position
	// moves up. What matters for the properties below is the undo call counter and
	// `entries()`, which together are what make "a refused revert performed no undo
	// actions and left the stack exactly as it found it" a claim a test can make
	// rather than a shape the code appears to have.
	class recording_undo_stack final : public undo_stack
	{
	public:
		// Pre-existing history, oldest first. The producer's afternoon, below the
		// turn's marker and outside its range.
		void seed_entries(std::vector<std::string> descriptions)
		{
			entries_ = std::move(descriptions);
			position_ = static_cast<int>(entries_.size());
		}

		// The producer reaching into their own session mid-turn. Deliberately not
		// routed through the block counters, so a claim about the agent's blocks is
		// not also measuring the producer's edits.
		void record_producer_entry(const std::string& description)
		{
			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		int current_position() override { return position_; }

		std::string entry_description_at(int position) override
		{
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

			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			++undo_call_count;

			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		const std::vector<std::string>& entries() const { return entries_; }

		int position() const { return position_; }

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		int deepest_open_block_depth = 0;

		std::vector<std::string> end_block_descriptions;
		std::vector<int> end_block_extra_flags;

		bool throw_from_end_block = false;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	undo_block_report apply_mutating_tool(undo_manager& manager, const std::string& tool_name)
	{
		return manager.run_in_undo_block(tool_name, [] {});
	}

	// Where an entry in a range came from.
	enum class entry_origin
	{
		agent_block,
		producer_edit,
	};

	// The producer's edits, cycled through so that each foreign entry in a range is
	// individually identifiable — which is what lets the naming claim be an equality
	// on an ordered list rather than a count.
	//
	// The list carries the near misses on purpose. An entry REAPER could not name, a
	// lower-case spelling, a leading space, and the prefix appearing somewhere other
	// than the front are all the producer's work or unidentifiable, and reading any of
	// them as the agent's is the direction of this component's dangerous failure. They
	// are in the corpus rather than in a separate case so they appear at arbitrary
	// positions and in arbitrary combinations with each other.
	const std::vector<std::string>& producer_edit_descriptions()
	{
		static const std::vector<std::string> descriptions{
			"Adjust fader volume",
			"Move media items",
			"",
			"sesh ai: create_track",
			" Sesh AI: create_track",
			"Undo Sesh AI: create_track",
			"Trim item left edge",
			"Insert new MIDI item",
		};

		return descriptions;
	}

	// What the producer should be shown for a raw REAPER description, restated from
	// requirement 10.5 and the schemas rather than borrowed from the implementation:
	// REAPER's own words, and a substitute when REAPER had none, because the schemas
	// give `blockingEntity.description` a `minLength` of one and an entry nobody can
	// name is still one the producer needs told about.
	std::string expected_producer_facing_description(const std::string& reaper_description)
	{
		return reaper_description.empty() ? "Unnamed REAPER undo entry" : reaper_description;
	}

	// A description no classification should read as the agent's.
	std::string agent_block_description(std::size_t block_index)
	{
		return "Sesh AI: agent_tool_" + std::to_string(block_index);
	}

	// Every arrangement of agent blocks and producer edits of the given length.
	std::vector<std::vector<entry_origin>> enumerate_origin_arrangements(std::size_t length)
	{
		std::vector<std::vector<entry_origin>> arrangements;
		const std::size_t arrangement_count = std::size_t{1} << length;

		for (std::size_t mask = 0; mask < arrangement_count; ++mask)
		{
			std::vector<entry_origin> arrangement;
			arrangement.reserve(length);

			for (std::size_t offset = 0; offset < length; ++offset)
			{
				arrangement.push_back((mask & (std::size_t{1} << offset)) != 0
					? entry_origin::producer_edit
					: entry_origin::agent_block);
			}

			arrangements.push_back(std::move(arrangement));
		}

		return arrangements;
	}

	std::string describe_arrangement(const std::vector<entry_origin>& arrangement)
	{
		std::string description = "arrangement (oldest first) ";

		if (arrangement.empty())
		{
			description += "<empty>";

			return description;
		}

		for (const entry_origin origin : arrangement)
		{
			description += origin == entry_origin::agent_block ? 'A' : 'P';
		}

		return description;
	}

	std::size_t count_producer_edits(const std::vector<entry_origin>& arrangement)
	{
		std::size_t producer_edits = 0;

		for (const entry_origin origin : arrangement)
		{
			if (origin == entry_origin::producer_edit)
			{
				++producer_edits;
			}
		}

		return producer_edits;
	}

	bool contains_adjacent_producer_edits(const std::vector<entry_origin>& arrangement)
	{
		for (std::size_t offset = 0; offset + 1 < arrangement.size(); ++offset)
		{
			if (arrangement[offset] == entry_origin::producer_edit
				&& arrangement[offset + 1] == entry_origin::producer_edit)
			{
				return true;
			}
		}

		return false;
	}

	std::vector<std::string> blocking_descriptions(const foreign_undo_refusal& refusal)
	{
		std::vector<std::string> descriptions;
		descriptions.reserve(refusal.blocking.size());

		for (const blocking_undo_entry& blocking : refusal.blocking)
		{
			descriptions.push_back(blocking.description);
		}

		return descriptions;
	}

	// ---------------------------------------------------------------------------
	// Turn shapes, for Properties 18 and 19
	// ---------------------------------------------------------------------------

	// What a turn can contain, as far as the marker is concerned. A read touches
	// nothing; a refusal opens no block; a mutation and an action array each open
	// exactly one.
	enum class turn_step
	{
		read,
		mutation,
		action_array,
		refusal,
	};

	bool step_mutates(turn_step step)
	{
		return step == turn_step::mutation || step == turn_step::action_array;
	}

	// Everything a property needs to know about how a turn went, gathered without
	// asserting anything, so the assertions live in the test cases where a failure
	// can name the turn that produced it.
	struct turn_observation
	{
		// The marker as it stood after each step that was actually performed, so a
		// property can check that the non-mutating steps left it alone.
		std::vector<std::optional<int>> marker_after_step;
		std::vector<turn_step> steps_performed;

		// One entry per mutating step, in order.
		std::vector<int> block_position_before;
		std::vector<int> reported_marker;

		std::optional<int> final_marker;
		std::size_t blocks_opened = 0;
		int begin_block_call_count = 0;
		int end_block_call_count = 0;

		int stack_position = 0;
		std::vector<std::string> stack_entries;

		int range_marker_position = 0;
		int range_current_position = 0;
		std::size_t range_entry_count = 0;
		bool range_contains_foreign = false;
	};

	// Runs one turn of the given shape against a fresh manager.
	//
	// `omit_refusals` is what makes Property 18 an equivalence: the same turn with its
	// refusals removed has to produce the same marker and the same range, because a
	// refusal changed nothing and so must be invisible to both.
	turn_observation run_turn(
		recording_undo_stack& stack,
		const std::vector<turn_step>& steps,
		int pre_existing_history_length,
		bool omit_refusals)
	{
		std::vector<std::string> history;

		for (int entry = 0; entry < pre_existing_history_length; ++entry)
		{
			// Foreign, and below the marker. Nothing in the turn's range.
			history.push_back("Earlier producer work " + std::to_string(entry));
		}

		stack.seed_entries(std::move(history));

		undo_manager manager{stack};
		manager.begin_turn();

		turn_observation observation;
		std::size_t mutating_steps_so_far = 0;

		for (const turn_step step : steps)
		{
			if (step == turn_step::refusal && omit_refusals)
			{
				continue;
			}

			switch (step)
			{
			case turn_step::read:
				// list_tracks, get_track_state, list_selected_items. Nothing opens a
				// block and nothing touches the stack.
				static_cast<void>(manager.undo_descriptions_this_turn());
				static_cast<void>(manager.has_undo_position_marker());
				break;

			case turn_step::mutation:
			{
				const undo_block_report report =
					apply_mutating_tool(manager, "tool_" + std::to_string(mutating_steps_so_far));

				observation.block_position_before.push_back(report.block_position_before);
				observation.reported_marker.push_back(report.turn_undo_position_marker);
				++mutating_steps_so_far;
				break;
			}

			case turn_step::action_array:
			{
				const std::vector<std::string> targets{"Kick", "Snare", "Hats"};
				std::vector<std::string> applied;

				const undo_block_report report =
					manager.run_in_undo_block("set_track_state", [&] {
						for (const std::string& target : targets)
						{
							applied.push_back(target);
						}
					});

				observation.block_position_before.push_back(report.block_position_before);
				observation.reported_marker.push_back(report.turn_undo_position_marker);
				++mutating_steps_so_far;
				break;
			}

			case turn_step::refusal:
				// An unresolvable selector, a render path collision, a send that would
				// close a loop.
				manager.record_refused_mutation();
				break;
			}

			observation.steps_performed.push_back(step);
			observation.marker_after_step.push_back(manager.undo_position_marker());
		}

		observation.final_marker = manager.undo_position_marker();
		observation.blocks_opened = manager.undo_blocks_opened_this_turn();
		observation.begin_block_call_count = stack.begin_block_call_count;
		observation.end_block_call_count = stack.end_block_call_count;
		observation.stack_position = stack.position();
		observation.stack_entries = stack.entries();

		const undo_range_classification range = manager.classify_turn_undo_range();

		observation.range_marker_position = range.marker_position;
		observation.range_current_position = range.current_position;
		observation.range_entry_count = range.entry_count();
		observation.range_contains_foreign = range.contains_foreign_entries();

		return observation;
	}

	std::vector<std::vector<turn_step>> enumerate_turn_shapes(std::size_t maximum_length)
	{
		static const std::vector<turn_step> alphabet{
			turn_step::read,
			turn_step::mutation,
			turn_step::action_array,
			turn_step::refusal,
		};

		std::vector<std::vector<turn_step>> shapes{{}};
		std::vector<std::vector<turn_step>> frontier{{}};

		for (std::size_t length = 1; length <= maximum_length; ++length)
		{
			std::vector<std::vector<turn_step>> next_frontier;

			for (const std::vector<turn_step>& shape : frontier)
			{
				for (const turn_step step : alphabet)
				{
					std::vector<turn_step> extended = shape;
					extended.push_back(step);
					next_frontier.push_back(std::move(extended));
				}
			}

			for (const std::vector<turn_step>& shape : next_frontier)
			{
				shapes.push_back(shape);
			}

			frontier = std::move(next_frontier);
		}

		return shapes;
	}

	std::string describe_turn_shape(const std::vector<turn_step>& shape)
	{
		std::string description = "turn ";

		if (shape.empty())
		{
			description += "<empty>";

			return description;
		}

		for (const turn_step step : shape)
		{
			switch (step)
			{
			case turn_step::read:
				description += 'r';
				break;
			case turn_step::mutation:
				description += 'm';
				break;
			case turn_step::action_array:
				description += 'a';
				break;
			case turn_step::refusal:
				description += 'x';
				break;
			}
		}

		return description;
	}

	std::size_t count_mutating_steps(const std::vector<turn_step>& shape)
	{
		std::size_t mutating = 0;

		for (const turn_step step : shape)
		{
			if (step_mutates(step))
			{
				++mutating;
			}
		}

		return mutating;
	}

	// The pre-turn history lengths every turn shape is run at. Two values rather than
	// one so that "the marker equals the position before the first mutation" cannot
	// pass against an implementation that always answers zero.
	const std::vector<int>& pre_existing_history_lengths()
	{
		static const std::vector<int> lengths{0, 2};

		return lengths;
	}
}

// ---------------------------------------------------------------------------
// Properties 16 and 17: foreign entries block, wherever and however many
// ---------------------------------------------------------------------------

TEST_CASE("Property 16 and 17: classification and refusal over every interleaving",
	"[undo][property][range]")
{
	// The pure-function half of the claim, which is where offset zero of the range
	// lives. A real turn's range always opens with the agent's own first block —
	// the marker is captured immediately before that block opens, so the entry at
	// the marker position is the agent's by construction — but
	// `classify_undo_range` is stated over any range, and the component can reach a
	// foreign entry at offset zero when the agent's first block fails to record
	// one. That path is a separate case below; this sweep covers the arithmetic for
	// every arrangement at once.
	//
	// **Validates: Requirements 10.5, 10.6**

	constexpr std::size_t maximum_range_length = 8;

	// Three marker positions, so nothing here can pass by treating range offsets and
	// stack positions as the same number.
	static const std::vector<int> marker_positions{0, 1, 41};

	std::size_t classifications = 0;
	std::size_t clean_ranges = 0;
	std::size_t ranges_with_foreign = 0;
	std::size_t ranges_with_foreign_at_first_offset = 0;
	std::size_t ranges_with_foreign_at_final_offset = 0;
	std::size_t ranges_with_adjacent_foreign = 0;
	std::size_t most_foreign_entries_in_one_range = 0;

	for (const int marker_position : marker_positions)
	{
		for (std::size_t length = 0; length <= maximum_range_length; ++length)
		{
			for (const std::vector<entry_origin>& arrangement : enumerate_origin_arrangements(length))
			{
				INFO("marker " << marker_position << ", " << describe_arrangement(arrangement));

				++classifications;

				std::vector<std::string> descriptions;
				std::vector<std::string> agent_descriptions;
				std::vector<std::string> expected_blocking;
				std::vector<int> expected_foreign_positions;
				std::size_t producer_edits_so_far = 0;

				for (std::size_t offset = 0; offset < arrangement.size(); ++offset)
				{
					if (arrangement[offset] == entry_origin::agent_block)
					{
						const std::string description = agent_block_description(offset);
						descriptions.push_back(description);
						agent_descriptions.push_back(description);

						continue;
					}

					const std::vector<std::string>& producer_edits = producer_edit_descriptions();
					const std::string& raw =
						producer_edits[producer_edits_so_far % producer_edits.size()];
					++producer_edits_so_far;

					descriptions.push_back(raw);
					expected_blocking.push_back(expected_producer_facing_description(raw));
					expected_foreign_positions.push_back(
						marker_position + static_cast<int>(offset));
				}

				const undo_range_classification range =
					classify_undo_range(marker_position, descriptions);

				// The range is the whole span from the marker to the current
				// position, one entry per position, each at its own position.
				REQUIRE(range.marker_position == marker_position);
				REQUIRE(range.current_position == marker_position + static_cast<int>(length));
				REQUIRE(range.entry_count() == length);

				for (std::size_t offset = 0; offset < length; ++offset)
				{
					const classified_undo_entry& entry = range.entries[offset];

					REQUIRE(entry.stack_position == marker_position + static_cast<int>(offset));
					REQUIRE(entry.agent_created
						== (arrangement[offset] == entry_origin::agent_block));
				}

				const std::size_t producer_edits = count_producer_edits(arrangement);

				REQUIRE(range.contains_foreign_entries() == (producer_edits > 0));
				REQUIRE(range.foreign_entries().size() == producer_edits);

				std::vector<int> foreign_positions;

				for (const classified_undo_entry& entry : range.foreign_entries())
				{
					foreign_positions.push_back(entry.stack_position);
				}

				REQUIRE(foreign_positions == expected_foreign_positions);

				if (producer_edits > 0)
				{
					const foreign_undo_refusal refusal = build_foreign_undo_refusal(range);

					REQUIRE(refusal.refused);
					REQUIRE(refusal.reason == std::string{foreign_undo_refusal_reason});
					REQUIRE(refusal.acknowledgement_field
						== std::string{foreign_undo_acknowledgement_field});

					// Named with REAPER's own words, in stack order, one line per
					// edit the producer made.
					REQUIRE(blocking_descriptions(refusal) == expected_blocking);

					for (const blocking_undo_entry& blocking : refusal.blocking)
					{
						REQUIRE(blocking.kind == std::string{undo_entry_blocking_kind});
						REQUIRE_FALSE(blocking.description.empty());

						// And none of the agent's own blocks is named. Listing them
						// would bury the lines the producer has to recognise as
						// theirs.
						for (const std::string& agent_description : agent_descriptions)
						{
							REQUIRE(blocking.description != agent_description);
						}
					}

					++ranges_with_foreign;
				}
				else
				{
					// The negative control. Without it every assertion above holds
					// of an implementation that classified everything as foreign,
					// which would refuse every revert and look perfectly safe.
					REQUIRE(range.foreign_entries().empty());

					for (const classified_undo_entry& entry : range.entries)
					{
						REQUIRE(entry.agent_created);
					}

					++clean_ranges;
				}

				if (producer_edits > most_foreign_entries_in_one_range)
				{
					most_foreign_entries_in_one_range = producer_edits;
				}

				if (!arrangement.empty()
					&& arrangement.front() == entry_origin::producer_edit)
				{
					++ranges_with_foreign_at_first_offset;
				}

				if (!arrangement.empty() && arrangement.back() == entry_origin::producer_edit)
				{
					++ranges_with_foreign_at_final_offset;
				}

				if (contains_adjacent_producer_edits(arrangement))
				{
					++ranges_with_adjacent_foreign;
				}
			}
		}
	}

	// The corpus guard. Measured values: a sweep that quietly stopped enumerating, or
	// one that never reached the bottom of a range or two adjacent edits, would make
	// everything above vacuously true.
	//
	// 3 marker positions x (1 + 2 + 4 + ... + 256) arrangements = 3 x 511.
	REQUIRE(classifications == 1533);
	REQUIRE(clean_ranges == 27);
	REQUIRE(ranges_with_foreign == 1506);
	REQUIRE(ranges_with_foreign_at_first_offset == 765);
	REQUIRE(ranges_with_foreign_at_final_offset == 765);
	REQUIRE(ranges_with_adjacent_foreign == 1107);
	REQUIRE(most_foreign_entries_in_one_range == 8);
}

TEST_CASE("Property 16 and 17: every interleaving blocks the revert and undoes nothing",
	"[undo][property][revert]")
{
	// The same claim against the whole component, which is what actually stands
	// between "revert all" and a producer's fader nudge. Every arrangement of the
	// agent's blocks and the producer's edits a turn can hold, at two different
	// starting stack positions, with the assertion that matters most in each one:
	// zero undo actions and a byte-identical history.
	//
	// The turn opens with an agent mutation in every case, because that is what
	// gives the turn a marker at all — requirement 10.1 captures it on the first
	// mutating tool, so a turn that begins with producer edits has those edits
	// below the marker and outside the range. The foreign entry at the very bottom
	// of the range is reachable by a different route and is covered separately.
	//
	// **Validates: Requirements 10.5, 10.6**

	constexpr std::size_t maximum_tail_length = 7;

	std::size_t turns = 0;
	std::size_t clean_turns = 0;
	std::size_t refused_turns = 0;
	std::size_t acknowledged_reverts = 0;
	std::size_t turns_with_foreign_above_the_first_block = 0;
	std::size_t turns_with_foreign_at_the_top_of_the_range = 0;
	std::size_t turns_with_adjacent_foreign = 0;
	std::size_t most_foreign_entries_in_one_turn = 0;

	for (const int pre_existing_history : pre_existing_history_lengths())
	{
		for (std::size_t tail_length = 0; tail_length <= maximum_tail_length; ++tail_length)
		{
			for (const std::vector<entry_origin>& tail : enumerate_origin_arrangements(tail_length))
			{
				INFO("history " << pre_existing_history << ", " << describe_arrangement(tail));

				++turns;

				recording_undo_stack stack;

				std::vector<std::string> history;

				for (int entry = 0; entry < pre_existing_history; ++entry)
				{
					history.push_back("Earlier producer work " + std::to_string(entry));
				}

				stack.seed_entries(std::move(history));

				undo_manager manager{stack};
				manager.begin_turn();

				// The turn's first mutating tool, which is what captures the marker.
				std::vector<std::string> expected_agent_descriptions{"Sesh AI: tool_0"};
				apply_mutating_tool(manager, "tool_0");

				std::vector<std::string> expected_blocking;
				std::size_t agent_blocks = 1;
				std::size_t producer_edits_so_far = 0;

				for (const entry_origin origin : tail)
				{
					if (origin == entry_origin::agent_block)
					{
						const std::string tool_name = "tool_" + std::to_string(agent_blocks);
						expected_agent_descriptions.push_back("Sesh AI: " + tool_name);
						apply_mutating_tool(manager, tool_name);
						++agent_blocks;

						continue;
					}

					const std::vector<std::string>& producer_edits = producer_edit_descriptions();
					const std::string& raw =
						producer_edits[producer_edits_so_far % producer_edits.size()];
					++producer_edits_so_far;

					stack.record_producer_entry(raw);
					expected_blocking.push_back(expected_producer_facing_description(raw));
				}

				const std::size_t producer_edits = count_producer_edits(tail);
				const std::size_t range_length = 1 + tail.size();

				REQUIRE(manager.has_undo_position_marker());
				REQUIRE(manager.undo_position_marker() == pre_existing_history);
				REQUIRE(manager.undo_blocks_opened_this_turn() == agent_blocks);

				const undo_range_classification range = manager.classify_turn_undo_range();

				REQUIRE(range.marker_position == pre_existing_history);
				REQUIRE(range.entry_count() == range_length);
				REQUIRE(range.current_position
					== pre_existing_history + static_cast<int>(range_length));
				REQUIRE_FALSE(range.turn_has_no_marker);

				// The turn's range, not the whole undo history: the producer's
				// afternoon below the marker is foreign and is not in it.
				REQUIRE(range.entries.front().agent_created);

				const int position_before_request = stack.position();
				const std::vector<std::string> history_before_request = stack.entries();
				const int undo_calls_before_request = stack.undo_call_count;

				const revert_outcome outcome = manager.revert_agent_changes();

				if (producer_edits > 0)
				{
					REQUIRE_FALSE(outcome.performed);
					REQUIRE(outcome.refusal.refused);
					REQUIRE(outcome.refusal.reason == std::string{foreign_undo_refusal_reason});
					REQUIRE(outcome.refusal.acknowledgement_field
						== std::string{foreign_undo_acknowledgement_field});

					// REAPER's own descriptions, in stack order, one per edit.
					REQUIRE(blocking_descriptions(outcome.refusal) == expected_blocking);

					for (const blocking_undo_entry& blocking : outcome.refusal.blocking)
					{
						REQUIRE(blocking.kind == std::string{undo_entry_blocking_kind});

						for (const std::string& agent_description : expected_agent_descriptions)
						{
							REQUIRE(blocking.description != agent_description);
						}
					}

					// Requirement 10.5, the part that is easy to implement away.
					REQUIRE(stack.undo_call_count == undo_calls_before_request);
					REQUIRE(stack.position() == position_before_request);
					REQUIRE(stack.entries() == history_before_request);
					REQUIRE(outcome.reverted_entry_count == 0);
					REQUIRE(outcome.undone_entry_descriptions.empty());
					REQUIRE_FALSE(outcome.reverted_foreign_entries);

					// A refusal is not a revert, so the marker still has the turn's
					// work to point at.
					REQUIRE(manager.has_undo_position_marker());
					REQUIRE(manager.undo_position_marker() == pre_existing_history);

					// The second half of the negative control. An implementation
					// that refused unconditionally would pass everything above and
					// fail here: after the producer confirms, the revert has to
					// actually happen.
					const revert_outcome acknowledged = manager.revert_agent_changes(true);

					REQUIRE(acknowledged.performed);
					REQUIRE_FALSE(acknowledged.refusal.refused);
					REQUIRE(acknowledged.reverted_entry_count
						== static_cast<int>(range_length));
					REQUIRE(acknowledged.reverted_foreign_entries);
					REQUIRE(acknowledged.undo_stack_position_after == pre_existing_history);
					REQUIRE(stack.position() == pre_existing_history);

					++acknowledged_reverts;
					++refused_turns;
				}
				else
				{
					// The negative control proper: a range of only the agent's own
					// blocks reverts, and reverts exactly what the turn put there.
					REQUIRE(outcome.performed);
					REQUIRE_FALSE(outcome.refusal.refused);
					REQUIRE(outcome.refusal.blocking.empty());
					REQUIRE(outcome.reverted_entry_count == static_cast<int>(range_length));
					REQUIRE_FALSE(outcome.reverted_foreign_entries);
					REQUIRE_FALSE(outcome.walk_stopped_short);
					REQUIRE(outcome.undo_position_marker == pre_existing_history);
					REQUIRE(outcome.undo_stack_position_after == pre_existing_history);
					REQUIRE(stack.position() == pre_existing_history);
					REQUIRE(stack.undo_call_count
						== undo_calls_before_request + static_cast<int>(range_length));

					// Newest first, which is the order they were undone in.
					std::vector<std::string> expected_undone = expected_agent_descriptions;
					std::reverse(expected_undone.begin(), expected_undone.end());

					REQUIRE(outcome.undone_entry_descriptions == expected_undone);

					// The producer's afternoon below the marker is untouched.
					REQUIRE(stack.position() == pre_existing_history);

					REQUIRE_FALSE(manager.has_undo_position_marker());

					++clean_turns;
				}

				if (producer_edits > most_foreign_entries_in_one_turn)
				{
					most_foreign_entries_in_one_turn = producer_edits;
				}

				if (!tail.empty() && tail.front() == entry_origin::producer_edit)
				{
					++turns_with_foreign_above_the_first_block;
				}

				if (!tail.empty() && tail.back() == entry_origin::producer_edit)
				{
					++turns_with_foreign_at_the_top_of_the_range;
				}

				if (contains_adjacent_producer_edits(tail))
				{
					++turns_with_adjacent_foreign;
				}
			}
		}
	}

	// Measured values. 2 history lengths x (1 + 2 + ... + 128) arrangements = 2 x 255.
	REQUIRE(turns == 510);
	REQUIRE(clean_turns == 16);
	REQUIRE(refused_turns == 494);
	REQUIRE(acknowledged_reverts == 494);
	REQUIRE(turns_with_foreign_above_the_first_block == 254);
	REQUIRE(turns_with_foreign_at_the_top_of_the_range == 254);
	REQUIRE(turns_with_adjacent_foreign == 336);
	REQUIRE(most_foreign_entries_in_one_turn == 7);
}

TEST_CASE("Property 17: a foreign entry at the very bottom of the range still blocks",
	"[undo][property][revert]")
{
	// The one position the sweep above cannot reach through the real path, reached
	// through the other real path.
	//
	// The marker is captured immediately before the turn's first block opens, so the
	// entry at the marker position is normally that block. Unless it never lands:
	// REAPER refuses to close it, `Undo_EndBlock2` throws, and no entry is recorded.
	// The producer's next edit then sits exactly at the marker — the oldest entry in
	// the range, the last one a walk would reach, and the one a detector that stopped
	// short would miss.
	//
	// **Validates: Requirements 10.5, 10.6**

	recording_undo_stack stack;
	stack.seed_entries({"Move media items", "Adjust fader"});
	stack.throw_from_end_block = true;

	undo_manager manager{stack};
	manager.begin_turn();

	const undo_block_report failed_block = apply_mutating_tool(manager, "create_track");

	REQUIRE(failed_block.close_failed);
	REQUIRE(manager.undo_position_marker() == 2);
	REQUIRE(stack.position() == 2);

	stack.throw_from_end_block = false;

	stack.record_producer_entry("Adjust fader volume");
	apply_mutating_tool(manager, "add_fx");

	const undo_range_classification range = manager.classify_turn_undo_range();

	REQUIRE(range.marker_position == 2);
	REQUIRE(range.entry_count() == 2);
	REQUIRE(range.entries.front().stack_position == 2);
	REQUIRE_FALSE(range.entries.front().agent_created);

	const int position_before_request = stack.position();
	const std::vector<std::string> history_before_request = stack.entries();

	const revert_outcome outcome = manager.revert_agent_changes();

	REQUIRE_FALSE(outcome.performed);
	REQUIRE(outcome.refusal.refused);
	REQUIRE(blocking_descriptions(outcome.refusal)
		== std::vector<std::string>{"Adjust fader volume"});
	REQUIRE(stack.undo_call_count == 0);
	REQUIRE(stack.position() == position_before_request);
	REQUIRE(stack.entries() == history_before_request);
}

// ---------------------------------------------------------------------------
// Properties 18 and 19: one stable marker, and refusals that change nothing
// ---------------------------------------------------------------------------

TEST_CASE("Property 18 and 19: the marker over every turn shape, with and without refusals",
	"[undo][property][marker]")
{
	// Every sequence of up to six steps drawn from reads, mutations, action arrays,
	// and refusals, run at two starting stack positions, and each one run twice —
	// once as written and once with its refusals removed.
	//
	// Property 19 is the marker: captured at the position the stack stood at before
	// the first mutating step, reported unchanged by every mutating result after it,
	// and never acquired by a turn that only read or only refused. The per-tool
	// positions are asserted to be all different, so the equality is the marker
	// holding still rather than the stack failing to move.
	//
	// Property 18 is the pair: a refusal records no marker, which matters because a
	// marker recorded mid-turn would sit above the agent's earlier work and shorten
	// the range "revert all" walks. Stated as an equivalence between the two runs —
	// same marker, same range, same history — so a refusal anywhere in the turn is
	// caught, whether it lands before the first mutation, between two of them, or
	// after the last.
	//
	// **Validates: Requirements 10.1, 10.2, 10.7**

	constexpr std::size_t maximum_turn_length = 6;

	const std::vector<std::vector<turn_step>> shapes = enumerate_turn_shapes(maximum_turn_length);

	std::size_t observed_turns = 0;
	std::size_t turns_with_no_mutating_step = 0;
	std::size_t turns_with_one_mutating_step = 0;
	std::size_t turns_with_two_or_more_mutating_steps = 0;
	std::size_t turns_with_a_refusal_before_the_first_mutation = 0;
	std::size_t turns_with_a_refusal_between_mutations = 0;
	std::size_t turns_with_a_refusal_after_the_last_mutation = 0;
	std::size_t most_mutating_steps_in_one_turn = 0;

	for (const int pre_existing_history : pre_existing_history_lengths())
	{
		for (const std::vector<turn_step>& shape : shapes)
		{
			INFO("history " << pre_existing_history << ", " << describe_turn_shape(shape));

			++observed_turns;

			recording_undo_stack stack_with_refusals;
			recording_undo_stack stack_without_refusals;

			const turn_observation with_refusals =
				run_turn(stack_with_refusals, shape, pre_existing_history, false);
			const turn_observation without_refusals =
				run_turn(stack_without_refusals, shape, pre_existing_history, true);

			const std::size_t mutating_steps = count_mutating_steps(shape);

			// ---- Property 19: exactly one marker, and it is the right one.

			if (mutating_steps == 0)
			{
				// Requirement 10.3: a read-only turn, or a turn that only refused,
				// offers no revert because there is nothing to revert to.
				REQUIRE_FALSE(with_refusals.final_marker.has_value());
				REQUIRE(with_refusals.begin_block_call_count == 0);
				REQUIRE(with_refusals.blocks_opened == 0);
				REQUIRE(with_refusals.range_entry_count == 0);
			}
			else
			{
				REQUIRE(with_refusals.final_marker.has_value());

				// Requirement 10.1: the position before the turn's first mutating
				// tool. Each mutating step records exactly one entry, so that is the
				// pre-turn history length.
				REQUIRE(*with_refusals.final_marker == pre_existing_history);

				// Requirement 10.2: every mutating result reports it unchanged.
				REQUIRE(with_refusals.reported_marker.size() == mutating_steps);

				for (const int reported : with_refusals.reported_marker)
				{
					REQUIRE(reported == pre_existing_history);
				}

				// And the per-tool positions genuinely differed, so the equality
				// above is the marker holding still and not the stack standing
				// still.
				REQUIRE(with_refusals.block_position_before.size() == mutating_steps);

				for (std::size_t mutation = 0; mutation < mutating_steps; ++mutation)
				{
					REQUIRE(with_refusals.block_position_before[mutation]
						== pre_existing_history + static_cast<int>(mutation));
				}

				// One block per mutating step — no more from an action array, and
				// none at all from a read or a refusal.
				REQUIRE(with_refusals.blocks_opened == mutating_steps);
				REQUIRE(with_refusals.begin_block_call_count
					== static_cast<int>(mutating_steps));
				REQUIRE(with_refusals.end_block_call_count
					== static_cast<int>(mutating_steps));

				// The range is the turn's work and nothing else. The pre-turn
				// history is the producer's and is foreign, and none of it is in
				// here.
				REQUIRE(with_refusals.range_marker_position == pre_existing_history);
				REQUIRE(with_refusals.range_entry_count == mutating_steps);
				REQUIRE_FALSE(with_refusals.range_contains_foreign);
			}

			// The marker only ever moves on a mutating step. Every read and every
			// refusal leaves it exactly as it was.
			std::optional<int> marker_before_step;

			for (std::size_t step = 0; step < with_refusals.steps_performed.size(); ++step)
			{
				const std::optional<int> marker_after = with_refusals.marker_after_step[step];

				if (!step_mutates(with_refusals.steps_performed[step]))
				{
					REQUIRE(marker_after.has_value() == marker_before_step.has_value());
					REQUIRE(marker_after == marker_before_step);
				}

				marker_before_step = marker_after;
			}

			// ---- Property 18: the refusals were invisible.

			REQUIRE(with_refusals.final_marker == without_refusals.final_marker);
			REQUIRE(with_refusals.range_marker_position == without_refusals.range_marker_position);
			REQUIRE(with_refusals.range_current_position
				== without_refusals.range_current_position);
			REQUIRE(with_refusals.range_entry_count == without_refusals.range_entry_count);
			REQUIRE(with_refusals.stack_position == without_refusals.stack_position);
			REQUIRE(with_refusals.stack_entries == without_refusals.stack_entries);
			REQUIRE(with_refusals.blocks_opened == without_refusals.blocks_opened);
			REQUIRE(with_refusals.block_position_before
				== without_refusals.block_position_before);
			REQUIRE(with_refusals.reported_marker == without_refusals.reported_marker);

			// ---- Coverage bookkeeping.

			if (mutating_steps == 0)
			{
				++turns_with_no_mutating_step;
			}
			else if (mutating_steps == 1)
			{
				++turns_with_one_mutating_step;
			}
			else
			{
				++turns_with_two_or_more_mutating_steps;
			}

			if (mutating_steps > most_mutating_steps_in_one_turn)
			{
				most_mutating_steps_in_one_turn = mutating_steps;
			}

			if (mutating_steps > 0)
			{
				std::size_t first_mutation = shape.size();
				std::size_t last_mutation = 0;

				for (std::size_t step = 0; step < shape.size(); ++step)
				{
					if (!step_mutates(shape[step]))
					{
						continue;
					}

					if (first_mutation == shape.size())
					{
						first_mutation = step;
					}

					last_mutation = step;
				}

				bool refusal_before = false;
				bool refusal_between = false;
				bool refusal_after = false;

				for (std::size_t step = 0; step < shape.size(); ++step)
				{
					if (shape[step] != turn_step::refusal)
					{
						continue;
					}

					if (step < first_mutation)
					{
						refusal_before = true;
					}
					else if (step > last_mutation)
					{
						refusal_after = true;
					}
					else
					{
						refusal_between = true;
					}
				}

				if (refusal_before)
				{
					++turns_with_a_refusal_before_the_first_mutation;
				}

				if (refusal_between)
				{
					++turns_with_a_refusal_between_mutations;
				}

				if (refusal_after)
				{
					++turns_with_a_refusal_after_the_last_mutation;
				}
			}
		}
	}

	// The corpus guard, measured. 4 step kinds over lengths 0 to 6 is
	// 1 + 4 + 16 + ... + 4096 = 5461 shapes, each run at 2 starting positions.
	REQUIRE(shapes.size() == 5461);
	REQUIRE(observed_turns == 10922);
	REQUIRE(turns_with_no_mutating_step == 254);
	REQUIRE(turns_with_one_mutating_step == 1284);
	REQUIRE(turns_with_two_or_more_mutating_steps == 9384);
	REQUIRE(most_mutating_steps_in_one_turn == 6);

	// The three placements requirement 10.7 is actually about. A refusal before the
	// first mutation must not record a marker of its own; one between two mutations
	// must not move the marker up past the earlier work; one after the last must not
	// touch it either.
	REQUIRE(turns_with_a_refusal_before_the_first_mutation == 3396);
	REQUIRE(turns_with_a_refusal_between_mutations == 4224);
	REQUIRE(turns_with_a_refusal_after_the_last_mutation == 3396);
}

TEST_CASE("Property 18: a refusal never shortens the range a later revert all walks",
	"[undo][property][marker]")
{
	// The consequence, spelled out over the same corpus but measured on the revert
	// rather than on the marker. Every turn shape that mutates at least once is run
	// with its refusals and without them, and the revert has to undo the same
	// entries and land on the same position both times.
	//
	// A marker recorded by a refusal mid-turn would sit above the agent's earlier
	// blocks. The revert would then walk back only as far as that, report a smaller
	// count, and leave the rest of the turn in the session with nothing left
	// pointing at it.
	//
	// **Validates: Requirements 10.7**

	constexpr std::size_t maximum_turn_length = 5;

	const std::vector<std::vector<turn_step>> shapes = enumerate_turn_shapes(maximum_turn_length);

	std::size_t compared_turns = 0;
	std::size_t turns_reverting_at_least_two_entries = 0;

	for (const int pre_existing_history : pre_existing_history_lengths())
	{
		for (const std::vector<turn_step>& shape : shapes)
		{
			const std::size_t mutating_steps = count_mutating_steps(shape);

			if (mutating_steps == 0)
			{
				continue;
			}

			INFO("history " << pre_existing_history << ", " << describe_turn_shape(shape));

			++compared_turns;

			const auto revert_a_turn = [&](bool omit_refusals) {
				recording_undo_stack stack;

				std::vector<std::string> history;

				for (int entry = 0; entry < pre_existing_history; ++entry)
				{
					history.push_back("Earlier producer work " + std::to_string(entry));
				}

				stack.seed_entries(std::move(history));

				undo_manager manager{stack};
				manager.begin_turn();

				std::size_t mutations_so_far = 0;

				for (const turn_step step : shape)
				{
					switch (step)
					{
					case turn_step::read:
						static_cast<void>(manager.has_undo_position_marker());
						break;

					case turn_step::mutation:
						apply_mutating_tool(manager, "tool_" + std::to_string(mutations_so_far));
						++mutations_so_far;
						break;

					case turn_step::action_array:
						manager.run_in_undo_block(
							"tool_" + std::to_string(mutations_so_far),
							[] {});
						++mutations_so_far;
						break;

					case turn_step::refusal:
						if (!omit_refusals)
						{
							manager.record_refused_mutation();
						}

						break;
					}
				}

				const revert_outcome outcome = manager.revert_agent_changes();

				return std::pair<revert_outcome, int>{outcome, stack.position()};
			};

			const std::pair<revert_outcome, int> with_refusals = revert_a_turn(false);
			const std::pair<revert_outcome, int> without_refusals = revert_a_turn(true);

			REQUIRE(with_refusals.first.performed);
			REQUIRE(with_refusals.first.reverted_entry_count
				== static_cast<int>(mutating_steps));
			REQUIRE(with_refusals.first.undo_position_marker == pre_existing_history);
			REQUIRE(with_refusals.first.undo_stack_position_after == pre_existing_history);
			REQUIRE(with_refusals.second == pre_existing_history);

			REQUIRE(with_refusals.first.reverted_entry_count
				== without_refusals.first.reverted_entry_count);
			REQUIRE(with_refusals.first.undone_entry_descriptions
				== without_refusals.first.undone_entry_descriptions);
			REQUIRE(with_refusals.first.undo_position_marker
				== without_refusals.first.undo_position_marker);
			REQUIRE(with_refusals.first.undo_stack_position_after
				== without_refusals.first.undo_stack_position_after);
			REQUIRE(with_refusals.second == without_refusals.second);

			if (mutating_steps >= 2)
			{
				++turns_reverting_at_least_two_entries;
			}
		}
	}

	// Measured. Shapes of length 0 to 5 over 4 step kinds is 1365; the 63 drawn only
	// from reads and refusals mutate nothing and are skipped, leaving 1302 per
	// starting position.
	REQUIRE(shapes.size() == 1365);
	REQUIRE(compared_turns == 2604);
	REQUIRE(turns_reverting_at_least_two_entries == 2088);
}

// ---------------------------------------------------------------------------
// Property 20: one undo block per action array
// ---------------------------------------------------------------------------

TEST_CASE("Property 20: an action array of any size produces exactly one undo block",
	"[undo][property][action-array]")
{
	// Every array size from zero to six, and for each size every pattern of which
	// actions failed — 127 arrays, from the empty one through the one where every
	// action was refused by REAPER.
	//
	// Requirement 10.8 is about the producer's hands: a five-target call has to be
	// one Ctrl+Z, not five. So the claim is on the block count and on the number of
	// entries the stack ended up with, not only on the report.
	//
	// **Validates: Requirements 10.8**

	constexpr std::size_t maximum_action_count = 6;

	std::size_t arrays = 0;
	std::size_t arrays_with_no_failures = 0;
	std::size_t arrays_with_some_failures = 0;
	std::size_t arrays_where_every_action_failed = 0;
	std::size_t largest_array = 0;

	for (std::size_t action_count = 0; action_count <= maximum_action_count; ++action_count)
	{
		const std::size_t failure_pattern_count = std::size_t{1} << action_count;

		for (std::size_t failure_pattern = 0; failure_pattern < failure_pattern_count;
			++failure_pattern)
		{
			INFO("array of " << action_count << ", failure pattern " << failure_pattern);

			++arrays;

			recording_undo_stack stack;
			undo_manager manager{stack};
			manager.begin_turn();

			std::vector<std::size_t> applied;
			std::vector<std::size_t> failed;

			const undo_block_report report = manager.run_in_undo_block("set_track_state", [&] {
				for (std::size_t action = 0; action < action_count; ++action)
				{
					// A locked track, a selector that resolved to nothing: a
					// per-action failure, not an exception. Requirement 9.4 keeps
					// the siblings that landed.
					if ((failure_pattern & (std::size_t{1} << action)) != 0)
					{
						failed.push_back(action);

						continue;
					}

					applied.push_back(action);
				}
			});

			REQUIRE(applied.size() + failed.size() == action_count);

			// One block, opened once, closed once, never nested.
			REQUIRE(stack.begin_block_call_count == 1);
			REQUIRE(stack.end_block_call_count == 1);
			REQUIRE(stack.deepest_open_block_depth == 1);
			REQUIRE(stack.open_block_depth == 0);
			REQUIRE(manager.undo_blocks_opened_this_turn() == 1);

			// And one entry in the producer's undo history, whatever the array did.
			REQUIRE(stack.entries().size() == 1);
			REQUIRE(stack.entries().front() == "Sesh AI: set_track_state");
			REQUIRE(stack.end_block_descriptions
				== std::vector<std::string>{"Sesh AI: set_track_state"});
			REQUIRE(manager.undo_descriptions_this_turn()
				== std::vector<std::string>{"Sesh AI: set_track_state"});

			REQUIRE(report.succeeded());
			REQUIRE(report.undo_description == "Sesh AI: set_track_state");

			if (failed.empty())
			{
				++arrays_with_no_failures;
			}
			else
			{
				++arrays_with_some_failures;
			}

			if (action_count > 0 && failed.size() == action_count)
			{
				++arrays_where_every_action_failed;
			}

			if (action_count > largest_array)
			{
				largest_array = action_count;
			}
		}
	}

	// Measured. 1 + 2 + 4 + ... + 64 = 127 arrays.
	REQUIRE(arrays == 127);
	REQUIRE(arrays_with_no_failures == 7);
	REQUIRE(arrays_with_some_failures == 120);
	REQUIRE(arrays_where_every_action_failed == 6);
	REQUIRE(largest_array == 6);
}

TEST_CASE("Property 20: an array that throws partway still produces exactly one block",
	"[undo][property][action-array]")
{
	// The same claim where the array does not finish. An exception out of the middle
	// of the loop is requirement 9.9's case: the block closes, the failure is
	// reported with a reason, and there is still exactly one block rather than none
	// or two. A block left open in REAPER accumulates every later producer edit into
	// the agent's entry, so "one" has to hold on this path too.
	//
	// **Validates: Requirements 10.8**

	constexpr std::size_t maximum_action_count = 6;

	std::size_t arrays = 0;

	for (std::size_t action_count = 1; action_count <= maximum_action_count; ++action_count)
	{
		for (std::size_t throwing_action = 0; throwing_action < action_count; ++throwing_action)
		{
			INFO("array of " << action_count << ", throwing at " << throwing_action);

			++arrays;

			recording_undo_stack stack;
			undo_manager manager{stack};
			manager.begin_turn();

			std::vector<std::size_t> applied;

			const undo_block_report report = manager.run_in_undo_block("set_track_state", [&] {
				for (std::size_t action = 0; action < action_count; ++action)
				{
					if (action == throwing_action)
					{
						throw std::runtime_error("SetMediaTrackInfo_Value failed");
					}

					applied.push_back(action);
				}
			});

			REQUIRE(applied.size() == throwing_action);

			REQUIRE(stack.begin_block_call_count == 1);
			REQUIRE(stack.end_block_call_count == 1);
			REQUIRE(stack.open_block_depth == 0);
			REQUIRE(stack.entries().size() == 1);
			REQUIRE(manager.undo_blocks_opened_this_turn() == 1);

			REQUIRE_FALSE(report.succeeded());
			REQUIRE(report.failure_reason == "SetMediaTrackInfo_Value failed");

			// The block covering what landed is still the agent's, still identifiable,
			// and so will not block the producer's own revert later.
			REQUIRE(stack.entries().front() == "Sesh AI: set_track_state");
		}
	}

	// Measured. 1 + 2 + 3 + 4 + 5 + 6 = 21.
	REQUIRE(arrays == 21);
}
