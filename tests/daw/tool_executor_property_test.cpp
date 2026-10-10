// Tool Executor — Properties 21, 22, and 28 of the design, stated as properties.
//
// **Validates: Requirements 9.4, 9.5, 9.6**
//
// `tool_executor_test.cpp` beside this file is the example suite, and it is not thin:
// it already has a four-action array with the third action failing, a handler that
// throws after one change, an undo block that will not close, an inverted time
// selection, and the empty-and-oversized substitution cases. This file is deliberately
// not a second copy of those under property-shaped names. It adds the arrangements
// nobody would write down by hand, and the two things a single example cannot state:
// what holds over *every* failure pattern, and what holds over *every* pair of
// doubles.
//
// ---------------------------------------------------------------------------
// Property 21 — the assertion has to be about the session, not about the payload
//
// The example's array asserts `project.applied_changes` rather than the partial's
// outcome list, and that choice is the whole property. A result payload claiming three
// successes proves nothing about whether those three were rolled back a moment later —
// the payload is a description, and a description is exactly what a rollback would not
// update. So the fixture here carries the same idea one step further than the example
// needed to: `scripted_undo_stack::undo_one_entry` **truncates the project state** back
// to the change count recorded when the block it is undoing was opened.
//
// That is a small model of what REAPER's undo actually does, and it is what makes the
// property falsifiable. Under the mutation "roll back the array when a sibling fails",
// the framework's undo call reaches through the fake stack and removes the successes,
// and the project-state assertion fails — rather than the test passing because nothing
// in this process is capable of undoing a `std::vector::push_back`.
//
// The failure patterns are generated rather than chosen: first-fails, last-fails, both
// alternations, every-third, all-but-one, all, none, and a generated subset, crossed
// with seventeen array lengths from one action to six hundred. Two of those lengths are
// above `maximum_action_outcomes`, which is the arrangement nobody writes down and the
// most interesting one for this property: the payload is clamped to 512 outcomes while
// the project keeps every change that landed, so an implementation that trusted the
// payload as the record of what happened would be visibly wrong there.
//
// The failure pattern is computed twice, by two different routes. The handler consults
// a flag vector built from the pattern; the expectation consults a predicate that
// re-derives the same rule by different arithmetic — bit tests and division where the
// flag builder used modulo, `index != count / 2` where it used a marked position. Two
// formulations agreeing is evidence. The generated subset is the one case where both
// routes read the same draws, because there the draw *is* the input rather than the
// expectation.
//
// The throwing variant is the sharpest form of the property. When a handler throws
// part way through an array, the executor never receives the outcomes the handler had
// accumulated — `produced` is never assigned — so the payload reports a single failure
// and says nothing at all about the actions that landed before the throw. The project
// state still holds them. A property asserted against the payload would call that
// "zero successes, nothing to survive" and pass vacuously.
//
// ---------------------------------------------------------------------------
// Property 22 — half of it is a compile-time fact, and saying so is the point
//
// `action_error` has no default constructor, no single-argument constructor, and no
// state in which its code or its message is empty; `action_failed` holds one **by
// value**, so there is no null and no `std::optional` to leave unset. A reasonless
// failure is therefore not a value this program can hold, and asserting that at runtime
// over generated inputs would be asserting something the compiler already refuses. So
// the type-level half is `STATIC_REQUIRE`, and it checks the constructions that would
// *reintroduce* the hole rather than restating the example file's
// default-constructibility checks: a one-argument `action_error`, an `action_error`
// assembled from nothing, and an `error` member held indirectly.
//
// The runtime half is about the other direction. The bounds are real — the partial
// schema puts `minLength: 1` and a `maxLength` on all three strings — and a handler can
// hand back an empty string or a four-kilobyte one. Those are substituted and clamped
// rather than rejected, for the reason the header gives, so the invariant is "never
// empty *after* construction and never longer than the bound", not "rejected on the way
// in". Checked over a cross product of seven length shapes for each of the three
// fields, against a characterisation computed independently: non-empty, byte length
// equal to `min(input length, bound)`, a prefix of the input when the input was
// non-empty, and exactly the substitute when it was empty. Those four together pin the
// answer without recomputing it the way `within_schema_bounds` does.
//
// And then over the executor, because the property is about "any action array
// outcome", not about one factory function: every failure in every outcome this file
// produces — from all three properties' corpora, including the exception paths and the
// undo-block-close path — goes through one shared check. The count of failures it
// inspected is asserted, so a corpus that quietly stopped producing failures would not
// leave the property looking satisfied.
//
// One thing the bounds do *not* guarantee, noted here rather than asserted because it
// is a serialiser concern and not this framework's: the clamp resizes by bytes, so
// truncating a message whose 1024th byte falls inside a multi-byte UTF-8 sequence
// leaves a partial sequence. `maxLength` counts code points, so the length is
// conservative either way; the risk is invalid UTF-8 in the emitted JSON rather than a
// schema violation. It belongs to whichever component serialises the payload.
//
// ---------------------------------------------------------------------------
// Property 28 — swept, not sampled, and with the shape asserted
//
// Requirement 9.6 was amended: an inverted range is **an invalid call reported as a
// failed action carrying a reason, never a refusal**. A refusal names an
// acknowledgement the producer can confirm to unblock it, and there is no "yes, I meant
// end before start". `check_end_after_start` returns `std::optional<action_error>` and
// that return type is the amendment expressed in the type system, so the property
// asserts the type as well as the value: a `std::optional<tool_refusal>` would not
// compile past the static assertion below, and every range driven through the executor
// is checked for *not* being a refusal.
//
// The input space is small enough to sweep rather than sample. Thirty-four doubles
// crossed with themselves, chosen for the places a comparison goes wrong: equal pairs
// (a zero-length region is not a region), positive and negative zero (distinct bit
// patterns, the same position on the timeline), both quiet and signalling NaN in either
// slot, both infinities, the type's extremes, denormals, and pairs adjacent at the
// limit of double precision via `std::nextafter` — including at 2^53, where adding one
// second to a position changes nothing.
//
// The oracle is computed by a different route than the implementation compares by.
// `check_end_after_start` uses `>`; the reference maps each double to a
// monotonically increasing unsigned key derived from its bit pattern and compares the
// keys, with NaN and negative zero identified from their bits rather than through any
// floating-point operator. Two orderings agreeing over 1156 pairs is evidence that the
// NaN direction falls out on the rejecting side by design rather than by luck.
//
// `time_range_is_well_formed` is checked against `check_end_after_start` on every one
// of those pairs, because it is a second entry point onto the same rule and a
// disagreement between them would mean a handler could get a different answer depending
// on which one it asked.
//
// Nothing here includes the REAPER SDK or a JSON library, for the reason
// `tool_executor.h` gives: the payload type is a template parameter, and binding it to
// a plain struct is what lets the whole framework be driven without either.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tool_executor.h>

namespace
{
	using namespace sesh_ai::daw;

	// -------------------------------------------------------------------
	// Deterministic byte source
	// -------------------------------------------------------------------

	// The same xorshift `tests/daw/alias_store_test.cpp` and
	// `tests/daw/render_coordinator_property_test.cpp` use, for the same reason: the
	// seed is in the failure message, so a counterexample found on CI is one anybody
	// can replay.
	class DeterministicBytes
	{
	public:
		explicit DeterministicBytes(std::uint32_t seed)
			: state_{seed == 0 ? 0x9e3779b9u : seed}
		{
		}

		std::uint32_t next()
		{
			state_ ^= state_ << 13;
			state_ ^= state_ >> 17;
			state_ ^= state_ << 5;

			return state_;
		}

		std::size_t below(std::size_t bound) { return bound == 0 ? 0 : next() % bound; }

		bool coin() { return (next() & 1u) != 0u; }

	private:
		std::uint32_t state_;
	};

	// The seed as it is written in the source, so a failure message can be pasted back.
	std::string describe_seed(std::uint32_t seed)
	{
		static const char* const hexadecimal_digits = "0123456789abcdef";

		std::string description = "seed 0x";

		for (int shift = 28; shift >= 0; shift -= 4)
		{
			description += hexadecimal_digits[(seed >> shift) & 0xfu];
		}

		return description;
	}

	constexpr std::uint32_t failure_pattern_seed = 0x2545f491u;
	constexpr std::uint32_t failure_reason_seed = 0x9d2c5680u;

	// -------------------------------------------------------------------
	// The payload type and the framework's instantiations
	// -------------------------------------------------------------------

	// A plain struct rather than nlohmann::json. The framework never reads a field, so
	// the suite does not need a JSON library to drive it.
	struct test_payload
	{
		std::string described_result;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_context = tool_execution_context<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;
	using test_partial = tool_partial_outcome<test_payload>;

	// -------------------------------------------------------------------
	// A project state that REAPER's undo can actually roll back
	// -------------------------------------------------------------------

	// What the handlers write to, and what the framework has no way to reach except
	// through the undo stack. Property 21 is asserted against this rather than against
	// the payload describing it.
	struct scripted_project_state
	{
		std::vector<std::string> applied_changes;
	};

	// A scripted REAPER undo stack that models the one thing the example file's copy
	// did not need to: undoing an entry removes the project changes that entry covered.
	//
	// The checkpoint is the project's change count at `begin_block`, so undoing the
	// entry the block produced truncates back to it. That is what turns "a sibling
	// failure never rolls a success back" into a statement about the session rather
	// than a statement about a fake that could not have rolled anything back anyway.
	class scripted_undo_stack final : public undo_stack
	{
	public:
		explicit scripted_undo_stack(scripted_project_state& project)
			: project_{project}
		{
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

			change_count_when_block_opened_ = project_.applied_changes.size();
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;

			if (throw_from_end_block)
			{
				throw std::runtime_error("REAPER refused to close the block");
			}

			entries_.resize(static_cast<std::size_t>(position_));
			checkpoints_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			checkpoints_.push_back(change_count_when_block_opened_);
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

			// REAPER's undo, modelled: the edits the entry covered go away.
			const std::size_t checkpoint = checkpoints_[static_cast<std::size_t>(position_)];

			if (checkpoint < project_.applied_changes.size())
			{
				project_.applied_changes.resize(checkpoint);
			}

			return true;
		}

		void seed_entries(std::vector<std::string> descriptions)
		{
			checkpoints_.assign(descriptions.size(), 0);
			entries_ = std::move(descriptions);
			position_ = static_cast<int>(entries_.size());
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		int deepest_open_block_depth = 0;
		bool throw_from_end_block = false;

	private:
		scripted_project_state& project_;
		std::vector<std::string> entries_;
		std::vector<std::size_t> checkpoints_;
		std::size_t change_count_when_block_opened_ = 0;
		int position_ = 0;
	};

	class scripted_track_list final : public TrackListSource
	{
	public:
		explicit scripted_track_list(std::vector<ResolvableTrack> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		std::vector<ResolvableTrack> tracks_in_project_order() const override { return tracks_; }

	private:
		std::vector<ResolvableTrack> tracks_;
	};

	// REAPER's braced GUID form, with four hexadecimal digits of index so an array of
	// six hundred actions still addresses six hundred distinct tracks.
	std::string guid_for_index(std::size_t index)
	{
		static const char* const hexadecimal_digits = "0123456789abcdef";

		std::string guid{"{00000000-0000-0000-0000-00000000"};

		for (int shift = 12; shift >= 0; shift -= 4)
		{
			guid.push_back(hexadecimal_digits[(index >> shift) & 0xfu]);
		}

		guid.push_back('}');

		return guid;
	}

	std::vector<ResolvableTrack> tracks_numbering(std::size_t track_count)
	{
		std::vector<ResolvableTrack> tracks;
		tracks.reserve(track_count);

		for (std::size_t index = 0; index < track_count; ++index)
		{
			ResolvableTrack track;
			track.guid = guid_for_index(index);
			track.name = "Track " + std::to_string(index);
			track.project_index = static_cast<int>(index);

			tracks.push_back(std::move(track));
		}

		return tracks;
	}

	// -------------------------------------------------------------------
	// Reading an outcome without asserting on the way
	// -------------------------------------------------------------------
	//
	// The property loops collect a violation string and assert once at the end, so the
	// accessors here return null rather than calling `REQUIRE` — a `REQUIRE` inside a
	// thousand-iteration loop reports the iteration it happened to reach first and
	// nothing about the shape of the input that reached it.

	const test_result* result_pointer(const test_outcome& outcome)
	{
		return std::get_if<test_result>(&outcome);
	}

	const test_partial* partial_pointer(const test_outcome& outcome)
	{
		const test_result* const result = result_pointer(outcome);

		return result == nullptr ? nullptr : std::get_if<test_partial>(result);
	}

	bool outcome_is_refusal(const test_outcome& outcome)
	{
		const test_result* const result = result_pointer(outcome);

		return result != nullptr && std::holds_alternative<tool_refusal>(*result);
	}

	// The targets the payload says were applied, in payload order.
	std::vector<std::string> applied_targets_in(const test_partial& partial)
	{
		std::vector<std::string> targets;

		for (const action_outcome& action : partial.actions)
		{
			if (action_was_applied(action))
			{
				targets.push_back(action_target(action));
			}
		}

		return targets;
	}

	// -------------------------------------------------------------------
	// Property 22's shared check
	// -------------------------------------------------------------------

	// Every failure reported anywhere in this file goes through here, which is what
	// makes Property 22 a statement about the executor's output rather than about one
	// factory call. Returns a description of the first violation, empty when there is
	// none, and tallies how many failures it actually looked at so a corpus that
	// stopped producing them cannot leave the property looking satisfied.
	std::string failure_reason_violation(const test_outcome& outcome, std::size_t& failures_inspected)
	{
		const test_partial* const partial = partial_pointer(outcome);

		if (partial == nullptr)
		{
			return {};
		}

		for (std::size_t index = 0; index < partial->actions.size(); ++index)
		{
			const action_failed* const failed = std::get_if<action_failed>(&partial->actions[index]);

			if (failed == nullptr)
			{
				continue;
			}

			++failures_inspected;

			const std::string where = "action " + std::to_string(index) + " of "
				+ std::to_string(partial->actions.size()) + " ";

			if (failed->error.code().empty())
			{
				return where + "failed with an empty code";
			}

			if (failed->error.message().empty())
			{
				return where + "failed with an empty message";
			}

			if (failed->target.empty())
			{
				return where + "failed with an empty target";
			}

			if (failed->error.code().size() > maximum_action_error_code_length)
			{
				return where + "carries a code of " + std::to_string(failed->error.code().size())
					+ " bytes, over the schema bound";
			}

			if (failed->error.message().size() > maximum_action_error_message_length)
			{
				return where + "carries a message of " + std::to_string(failed->error.message().size())
					+ " bytes, over the schema bound";
			}

			if (failed->target.size() > maximum_action_target_length)
			{
				return where + "carries a target of " + std::to_string(failed->target.size())
					+ " bytes, over the schema bound";
			}
		}

		return {};
	}

	// -------------------------------------------------------------------
	// Property 21's failure patterns
	// -------------------------------------------------------------------

	enum class failure_pattern
	{
		none_fail,
		first_fails,
		last_fails,
		alternating_from_first,
		alternating_from_second,
		every_third,
		all_but_one_fail,
		all_fail,
		generated_subset
	};

	std::string describe_failure_pattern(failure_pattern pattern)
	{
		switch (pattern)
		{
			case failure_pattern::none_fail:
				return "none fail";
			case failure_pattern::first_fails:
				return "first fails";
			case failure_pattern::last_fails:
				return "last fails";
			case failure_pattern::alternating_from_first:
				return "alternating from the first";
			case failure_pattern::alternating_from_second:
				return "alternating from the second";
			case failure_pattern::every_third:
				return "every third";
			case failure_pattern::all_but_one_fail:
				return "all but one fail";
			case failure_pattern::all_fail:
				return "all fail";
			case failure_pattern::generated_subset:
				return "a generated subset";
		}

		return "an unnamed pattern";
	}

	const std::vector<failure_pattern>& deterministic_failure_patterns()
	{
		static const std::vector<failure_pattern> patterns{
			failure_pattern::none_fail,
			failure_pattern::first_fails,
			failure_pattern::last_fails,
			failure_pattern::alternating_from_first,
			failure_pattern::alternating_from_second,
			failure_pattern::every_third,
			failure_pattern::all_but_one_fail,
			failure_pattern::all_fail
		};

		return patterns;
	}

	// Seventeen lengths, two of them above `maximum_action_outcomes` so the clamped
	// payload is part of the swept space rather than a case somebody remembered.
	const std::vector<std::size_t>& swept_action_counts()
	{
		static const std::vector<std::size_t> counts{
			1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 32, 64, 520, 600
		};

		return counts;
	}

	// Route one: the flag vector the handler consults. Modulo arithmetic and marked
	// positions.
	std::vector<char> failure_flags_for(
		failure_pattern pattern,
		std::size_t action_count,
		DeterministicBytes& bytes)
	{
		std::vector<char> flags(action_count, 0);

		switch (pattern)
		{
			case failure_pattern::none_fail:
				break;

			case failure_pattern::first_fails:
				if (action_count > 0)
				{
					flags[0] = 1;
				}

				break;

			case failure_pattern::last_fails:
				if (action_count > 0)
				{
					flags[action_count - 1] = 1;
				}

				break;

			case failure_pattern::alternating_from_first:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = static_cast<char>(index % 2 == 0 ? 1 : 0);
				}

				break;

			case failure_pattern::alternating_from_second:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = static_cast<char>(index % 2 == 1 ? 1 : 0);
				}

				break;

			case failure_pattern::every_third:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = static_cast<char>(index % 3 == 0 ? 1 : 0);
				}

				break;

			case failure_pattern::all_but_one_fail:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = 1;
				}

				if (action_count > 0)
				{
					flags[action_count / 2] = 0;
				}

				break;

			case failure_pattern::all_fail:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = 1;
				}

				break;

			case failure_pattern::generated_subset:
				for (std::size_t index = 0; index < action_count; ++index)
				{
					flags[index] = static_cast<char>(bytes.coin() ? 1 : 0);
				}

				break;
		}

		return flags;
	}

	// Route two: the predicate the expectation consults. The same rules expressed by
	// different arithmetic — bit tests and division where route one used modulo, an
	// inequality against the middle where route one cleared a marked position. Only
	// the generated subset reads route one's draws, because there the draw is the
	// input rather than the expectation.
	bool action_is_scripted_to_fail(
		failure_pattern pattern,
		std::size_t index,
		std::size_t action_count,
		const std::vector<char>& generated_flags)
	{
		switch (pattern)
		{
			case failure_pattern::none_fail:
				return false;

			case failure_pattern::first_fails:
				return index == 0;

			case failure_pattern::last_fails:
				return index + 1 == action_count;

			case failure_pattern::alternating_from_first:
				return (index & static_cast<std::size_t>(1)) == 0;

			case failure_pattern::alternating_from_second:
				return (index & static_cast<std::size_t>(1)) == 1;

			case failure_pattern::every_third:
				return index - (3 * (index / 3)) == 0;

			case failure_pattern::all_but_one_fail:
				return index != action_count / 2;

			case failure_pattern::all_fail:
				return true;

			case failure_pattern::generated_subset:
				return generated_flags[index] != 0;
		}

		return false;
	}

	// What the handler is told to do on one call.
	struct action_array_script
	{
		failure_pattern pattern = failure_pattern::none_fail;
		std::vector<char> failure_flags;

		// When set, the handler throws instead of performing the action at this index,
		// which is how a REAPER call failing mid-array is staged.
		std::optional<std::size_t> throw_before_action_index;
	};

	// -------------------------------------------------------------------
	// Property 28's corpus and its independent ordering
	// -------------------------------------------------------------------

	constexpr std::uint64_t double_sign_bit = 0x8000000000000000ull;
	constexpr std::uint64_t double_exponent_mask = 0x7ff0000000000000ull;
	constexpr std::uint64_t double_mantissa_mask = 0x000fffffffffffffull;

	std::uint64_t bits_of(double value)
	{
		std::uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof bits);

		return bits;
	}

	// NaN identified from the bit pattern rather than through a comparison, so the
	// reference shares no machinery with the implementation it is checking.
	bool is_not_a_number_by_bits(double value)
	{
		const std::uint64_t bits = bits_of(value);

		return (bits & double_exponent_mask) == double_exponent_mask
			&& (bits & double_mantissa_mask) != 0;
	}

	// A monotonically increasing unsigned key over the non-NaN doubles, derived from
	// the sign-magnitude bit layout. Negative zero is folded onto positive zero from
	// its bit pattern, because the two are the same position on a timeline and a key
	// that separated them would disagree with the implementation for the right reason
	// and the wrong one at once.
	std::uint64_t total_order_key(double value)
	{
		std::uint64_t bits = bits_of(value);

		if (bits == double_sign_bit)
		{
			bits = 0;
		}

		return (bits & double_sign_bit) != 0 ? ~bits : bits | double_sign_bit;
	}

	// The oracle: end strictly after start, computed by bit-pattern ordering rather
	// than by `>`.
	bool reference_end_strictly_follows_start(double start_seconds, double end_seconds)
	{
		if (is_not_a_number_by_bits(start_seconds) || is_not_a_number_by_bits(end_seconds))
		{
			return false;
		}

		return total_order_key(end_seconds) > total_order_key(start_seconds);
	}

	std::string describe_double(double value)
	{
		static const char* const hexadecimal_digits = "0123456789abcdef";

		const std::uint64_t bits = bits_of(value);

		std::string description = "0x";

		for (int shift = 60; shift >= 0; shift -= 4)
		{
			description += hexadecimal_digits[(bits >> shift) & 0xfu];
		}

		return description;
	}

	// Thirty-four positions, chosen for where a comparison goes wrong rather than for
	// where a timeline usually sits.
	const std::vector<double>& swept_time_positions()
	{
		static const std::vector<double> positions = [] {
			const double infinity = std::numeric_limits<double>::infinity();
			const double quiet_not_a_number = std::numeric_limits<double>::quiet_NaN();
			const double signalling_not_a_number = std::numeric_limits<double>::signaling_NaN();
			const double largest = std::numeric_limits<double>::max();
			const double smallest_normal = std::numeric_limits<double>::min();
			const double smallest_denormal = std::numeric_limits<double>::denorm_min();

			// 2^53, where adding one second to a position changes nothing, and its two
			// neighbours.
			const double beyond_integer_precision = 9007199254740992.0;

			return std::vector<double>{
				0.0,
				-0.0,
				smallest_denormal,
				-smallest_denormal,
				smallest_normal,
				-smallest_normal,
				std::nextafter(0.0, 1.0),
				std::nextafter(0.0, -1.0),
				0.5,
				-0.5,
				1.0,
				-1.0,
				std::nextafter(1.0, 2.0),
				std::nextafter(1.0, 0.0),
				std::nextafter(-1.0, 0.0),
				std::nextafter(-1.0, -2.0),
				4.0,
				8.0,
				-4.5,
				-4.4,
				1e-9,
				-1e-9,
				44100.0,
				std::nextafter(44100.0, 44101.0),
				1e6,
				beyond_integer_precision,
				std::nextafter(beyond_integer_precision, infinity),
				-beyond_integer_precision,
				largest,
				-largest,
				infinity,
				-infinity,
				quiet_not_a_number,
				signalling_not_a_number
			};
		}();

		return positions;
	}

	// The three contexts requirement 9.6 names, so the property is swept over the
	// tools that actually perform the check rather than over one of them.
	const std::vector<std::string>& time_range_tool_names()
	{
		static const std::vector<std::string> names{
			"set_time_selection",
			"create_region",
			"update_marker_or_region"
		};

		return names;
	}

	// -------------------------------------------------------------------
	// Property 22's string shapes
	// -------------------------------------------------------------------

	// Seven lengths relative to a bound: nothing, the shortest the schema accepts, two
	// short, one under, exactly at, one over, and far over.
	std::vector<std::size_t> string_lengths_around(std::size_t bound)
	{
		return {0, 1, 2, bound - 1, bound, bound + 1, bound * 4};
	}

	// Content that is not all one character, so a clamp that reversed or reordered
	// rather than truncating would be visible in the prefix check.
	std::string generated_string_of_length(std::size_t length, DeterministicBytes& bytes)
	{
		static const std::string alphabet{
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.\n\t"};

		std::string generated;
		generated.reserve(length);

		for (std::size_t index = 0; index < length; ++index)
		{
			generated.push_back(alphabet[bytes.below(alphabet.size())]);
		}

		return generated;
	}

	// -------------------------------------------------------------------
	// Detecting which initialisations the types permit
	// -------------------------------------------------------------------

	// `std::is_constructible` answers for constructors and says nothing about aggregate
	// initialisation, and `action_failed` is an aggregate — it has no constructors at
	// all. So the question "can a failed action be brace-initialised without a reason"
	// needs asking directly, and this is what asks it: the probe is only viable when
	// `T{arguments...}` is well formed.
	template <typename Candidate, typename... Arguments>
	class is_brace_initializable
	{
		template <typename Probed>
		static auto probe(int) -> decltype(Probed{std::declval<Arguments>()...}, std::true_type{});

		template <typename>
		static std::false_type probe(...);

	public:
		static constexpr bool value = decltype(probe<Candidate>(0))::value;
	};

	// The independent characterisation of the clamp: not a second copy of
	// `within_schema_bounds`, but the four facts that together pin its answer.
	std::string bounded_string_violation(
		std::string_view field_name,
		const std::string& supplied,
		const std::string& produced,
		std::string_view substitute_when_empty,
		std::size_t bound)
	{
		const std::string where{field_name};

		if (produced.empty())
		{
			return where + " came back empty";
		}

		if (produced.size() > bound)
		{
			return where + " came back at " + std::to_string(produced.size())
				+ " bytes, over the bound of " + std::to_string(bound);
		}

		if (supplied.empty())
		{
			if (produced != std::string{substitute_when_empty})
			{
				return where + " was empty and came back as something other than the substitute";
			}

			return {};
		}

		const std::size_t expected_length = std::min(supplied.size(), bound);

		if (produced.size() != expected_length)
		{
			return where + " came back at " + std::to_string(produced.size()) + " bytes rather than "
				+ std::to_string(expected_length);
		}

		if (supplied.compare(0, produced.size(), produced) != 0)
		{
			return where + " came back as something other than a prefix of what was supplied";
		}

		return {};
	}
}

// ---------------------------------------------------------------------------
// Property 21
// ---------------------------------------------------------------------------

TEST_CASE("Property 21: every action that succeeded is still applied, whatever its siblings did", "[daw][tool_executor][property]")
{
	DeterministicBytes bytes{failure_pattern_seed};

	std::string first_violation;

	std::size_t configurations_checked = 0;
	std::size_t configurations_with_a_failing_first_action = 0;
	std::size_t configurations_with_a_failing_last_action = 0;
	std::size_t configurations_where_every_action_failed = 0;
	std::size_t configurations_where_no_action_failed = 0;
	std::size_t configurations_with_a_success_after_a_failure = 0;
	std::size_t configurations_whose_payload_was_clamped = 0;
	std::size_t clamped_payloads_naming_the_gap = 0;
	std::size_t total_actions_performed = 0;
	std::size_t total_actions_applied = 0;
	std::size_t failures_inspected = 0;

	// The deterministic patterns, plus repeated draws of the generated subset so the
	// arrangements nobody would write down are actually reached.
	std::vector<failure_pattern> patterns = deterministic_failure_patterns();

	for (int generated_draw = 0; generated_draw < 6; ++generated_draw)
	{
		patterns.push_back(failure_pattern::generated_subset);
	}

	for (const std::size_t action_count : swept_action_counts())
	{
		const std::vector<ResolvableTrack> tracks = tracks_numbering(action_count);

		for (const failure_pattern pattern : patterns)
		{
			configurations_checked += 1;

			action_array_script script;
			script.pattern = pattern;
			script.failure_flags = failure_flags_for(pattern, action_count, bytes);

			scripted_project_state project;
			scripted_undo_stack stack{project};
			undo_manager undo{stack};
			scripted_track_list track_list{tracks};
			NoLearnedAliases aliases;

			test_registry registry;

			REQUIRE(registry.register_mutating_tool(
				"set_track_state",
				[&project, &script](const test_context& context)
					-> mutating_handler_result<test_payload> {
					handler_action_outcomes produced;
					std::size_t index = 0;

					for (const ResolvedTrack& target : context.resolved_targets)
					{
						if (script.failure_flags[index] != 0)
						{
							produced.actions.push_back(failed_action(
								target.reference.guid,
								"reaper_rejected_value",
								"SetMediaTrackInfo_Value returned false for D_VOL"));

							++index;

							continue;
						}

						project.applied_changes.push_back(target.reference.guid);
						produced.actions.push_back(succeeded_action(target.reference.guid));
						++index;
					}

					return produced;
				})
				== tool_registration_outcome::registered);

			test_executor executor{registry, undo, track_list, aliases};

			undo.begin_turn();

			test_call call;
			call.tool_name = "set_track_state";
			call.track_selectors.reserve(action_count);

			for (std::size_t index = 0; index < action_count; ++index)
			{
				call.track_selectors.push_back(TrackGuidSelector{guid_for_index(index)});
			}

			// The expectation, built from the predicate route rather than from the flag
			// route the handler consulted, and without reading the result at all.
			std::vector<std::string> expected_applied_targets;
			bool a_failure_has_been_seen = false;
			bool a_success_followed_a_failure = false;

			for (std::size_t index = 0; index < action_count; ++index)
			{
				if (action_is_scripted_to_fail(pattern, index, action_count, script.failure_flags))
				{
					a_failure_has_been_seen = true;

					continue;
				}

				if (a_failure_has_been_seen)
				{
					a_success_followed_a_failure = true;
				}

				expected_applied_targets.push_back(guid_for_index(index));
			}

			const test_outcome outcome = executor.execute(call);

			const std::string configuration = describe_seed(failure_pattern_seed) + ", "
				+ std::to_string(action_count) + " actions, " + describe_failure_pattern(pattern)
				+ ": ";

			total_actions_performed += action_count;
			total_actions_applied += expected_applied_targets.size();

			if (action_count > 0
				&& action_is_scripted_to_fail(pattern, 0, action_count, script.failure_flags))
			{
				configurations_with_a_failing_first_action += 1;
			}

			if (action_count > 0
				&& action_is_scripted_to_fail(
					pattern, action_count - 1, action_count, script.failure_flags))
			{
				configurations_with_a_failing_last_action += 1;
			}

			if (expected_applied_targets.empty())
			{
				configurations_where_every_action_failed += 1;
			}

			if (expected_applied_targets.size() == action_count)
			{
				configurations_where_no_action_failed += 1;
			}

			if (a_success_followed_a_failure)
			{
				configurations_with_a_success_after_a_failure += 1;
			}

			if (action_count > maximum_action_outcomes)
			{
				configurations_whose_payload_was_clamped += 1;
			}

			// The property, asserted against the session.
			if (project.applied_changes != expected_applied_targets && first_violation.empty())
			{
				first_violation = configuration + "the project holds "
					+ std::to_string(project.applied_changes.size()) + " changes rather than the "
					+ std::to_string(expected_applied_targets.size()) + " that succeeded";
			}

			// And the mechanism it depends on: nothing in this framework's mutating
			// path moves the undo stack backwards.
			if (stack.undo_call_count != 0 && first_violation.empty())
			{
				first_violation = configuration + "the framework performed "
					+ std::to_string(stack.undo_call_count) + " undo actions";
			}

			// Requirement 10.8, swept alongside: one block for the whole array, closed.
			if ((stack.begin_block_call_count != 1 || stack.end_block_call_count != 1
					|| stack.open_block_depth != 0 || stack.deepest_open_block_depth != 1)
				&& first_violation.empty())
			{
				first_violation = configuration + "the array did not run inside exactly one closed block";
			}

			if (outcome_is_refusal(outcome) && first_violation.empty())
			{
				first_violation = configuration + "an action array came back as a refusal";
			}

			const test_partial* const partial = partial_pointer(outcome);

			if (partial == nullptr)
			{
				if (first_violation.empty())
				{
					first_violation = configuration + "the outcome was not a partial";
				}

				continue;
			}

			// The payload must not contradict the session. Clamped at
			// `maximum_action_outcomes`, so the comparison is against the prefix the
			// payload was allowed to carry rather than against the whole expectation.
			const std::vector<std::string> payload_applied = applied_targets_in(*partial);

			if (action_count <= maximum_action_outcomes)
			{
				if (payload_applied != expected_applied_targets && first_violation.empty())
				{
					first_violation = configuration + "the payload names "
						+ std::to_string(payload_applied.size())
						+ " applied actions where the project holds "
						+ std::to_string(project.applied_changes.size());
				}

				if (partial->actions.size() != action_count && first_violation.empty())
				{
					first_violation = configuration + "the payload carries "
						+ std::to_string(partial->actions.size()) + " outcomes rather than one per action";
				}
			}
			else
			{
				// The arrangement nobody writes down: the payload is clamped while the
				// project keeps everything that landed, so the payload is a strict
				// prefix of the truth rather than the whole of it.
				if (partial->actions.size() != maximum_action_outcomes && first_violation.empty())
				{
					first_violation = configuration + "the payload was not clamped to the schema bound";
				}

				if (payload_applied.size() > expected_applied_targets.size() && first_violation.empty())
				{
					first_violation = configuration + "the clamped payload claims more applied actions "
						"than actually landed";
				}

				if (!std::equal(payload_applied.begin(), payload_applied.end(),
						expected_applied_targets.begin())
					&& first_violation.empty())
				{
					first_violation = configuration + "the clamped payload is not a prefix of what landed";
				}

				// And the half that makes the prefix honest rather than a silent
				// truncation. Requirements 9.5 and 23.9 both forbid outcomes disappearing
				// off the end without a word, so the last slot carries a failed action
				// naming how many are missing — see `tool_executor.h`'s outcome-cap rule.
				// Without this, a clamped payload of 512 clean outcomes satisfies every
				// assertion above while telling the agent that 512 is all there was.
				const action_failed* const report_gap =
					std::get_if<action_failed>(&partial->actions.back());

				if (report_gap == nullptr && first_violation.empty())
				{
					first_violation = configuration + "the clamped payload ends in an outcome rather "
						"than in the failed action that names what is missing";
				}
				else if (report_gap != nullptr)
				{
					++clamped_payloads_naming_the_gap;

					if (report_gap->error.code() != std::string{outcome_report_truncated_code}
						&& first_violation.empty())
					{
						first_violation = configuration + "the clamped payload's last action is \""
							+ report_gap->error.code() + "\" rather than the outcome report gap";
					}

					// The count, not just the fact. `action_count` actions were performed
					// and `maximum_action_outcomes - 1` of them are named.
					const std::string unnamed =
						std::to_string(action_count - (maximum_action_outcomes - 1));

					if (report_gap->error.message().find(unnamed) == std::string::npos
						&& first_violation.empty())
					{
						first_violation = configuration + "the clamped payload does not say that "
							+ unnamed + " outcomes are missing";
					}
				}
			}

			// Requirement 9.4's undo report rule, which falls out of the same sweep.
			const bool expects_an_undo_report = !expected_applied_targets.empty();

			if (partial->undo.has_value() != expects_an_undo_report && first_violation.empty())
			{
				first_violation = configuration + "the undo report was "
					+ (partial->undo.has_value() ? "present" : "absent")
					+ " where the opposite was called for";
			}

			const std::string reason_violation = failure_reason_violation(outcome, failures_inspected);

			if (!reason_violation.empty() && first_violation.empty())
			{
				first_violation = configuration + reason_violation;
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Guards on the sweep, measured rather than hoped for — every number below is what
	// this grid and this seed actually reach, so they are exact rather than a threshold
	// somebody hoped was low enough. A grid that quietly stopped crossing would satisfy
	// everything above while checking almost none of it.
	//
	// 17 lengths * 14 patterns, and 1310 actions per pattern.
	REQUIRE(configurations_checked == 238);
	REQUIRE(total_actions_performed == 18340);
	REQUIRE(total_actions_applied == 9975);
	REQUIRE(configurations_with_a_failing_first_action == 143);
	REQUIRE(configurations_with_a_failing_last_action == 123);
	REQUIRE(configurations_where_every_action_failed == 27);
	REQUIRE(configurations_where_no_action_failed == 25);
	REQUIRE(configurations_whose_payload_was_clamped == 28);

	// Every clamped payload, and not a subset: the gap is named by
	// `build_partial_outcome` rather than by the handler, so a clamped array that did not
	// carry it would mean the framework's backstop had a hole in it.
	REQUIRE(clamped_payloads_naming_the_gap == 28);

	// 28 of these are the gap reports counted just above, leaving 7746 failures the handler
	// itself reported. This number was 7756 while `build_partial_outcome` cut the array
	// silently: the last slot then held whatever the 512th outcome happened to be, and for
	// 10 of the 28 clamped configurations that was a failure. Giving the slot over to the
	// gap report trades those 10 for 28, which is the +18.
	REQUIRE(failures_inspected == 7774);

	// The guard that matters most for this property. "A success survives a sibling
	// failure" is only tested by an arrangement where a success comes *after* a
	// failure — an array whose failures all sat at the end would satisfy a framework
	// that truncated the array at the first failure.
	REQUIRE(configurations_with_a_success_after_a_failure == 167);
}

TEST_CASE("Property 21: a handler that throws part way through an array leaves what landed", "[daw][tool_executor][property]")
{
	// The sharpest form of the property, and the one that shows why it cannot be
	// asserted against the payload. When the handler throws, the executor never
	// receives the outcomes it had accumulated, so the payload reports a single
	// failure and is silent about everything that landed first. The project is not.
	DeterministicBytes bytes{failure_pattern_seed};

	std::string first_violation;

	std::size_t configurations_checked = 0;
	std::size_t configurations_that_landed_something_before_throwing = 0;
	std::size_t configurations_that_threw_before_anything_landed = 0;
	std::size_t failures_inspected = 0;

	const std::vector<failure_pattern> patterns{
		failure_pattern::none_fail,
		failure_pattern::first_fails,
		failure_pattern::alternating_from_second,
		failure_pattern::generated_subset
	};

	for (const std::size_t action_count : {std::size_t{1}, std::size_t{2}, std::size_t{3},
			std::size_t{5}, std::size_t{8}, std::size_t{12}, std::size_t{32}, std::size_t{64}})
	{
		const std::vector<ResolvableTrack> tracks = tracks_numbering(action_count);

		for (const failure_pattern pattern : patterns)
		{
			// The first action, the middle, and the last: throwing before anything
			// landed, part way, and at the end.
			for (const std::size_t throw_index :
					{std::size_t{0}, action_count / 2, action_count - 1})
			{
				configurations_checked += 1;

				action_array_script script;
				script.pattern = pattern;
				script.failure_flags = failure_flags_for(pattern, action_count, bytes);
				script.throw_before_action_index = throw_index;

				scripted_project_state project;
				scripted_undo_stack stack{project};
				undo_manager undo{stack};
				scripted_track_list track_list{tracks};
				NoLearnedAliases aliases;

				test_registry registry;

				REQUIRE(registry.register_mutating_tool(
					"set_track_state",
					[&project, &script](const test_context& context)
						-> mutating_handler_result<test_payload> {
						handler_action_outcomes produced;
						std::size_t index = 0;

						for (const ResolvedTrack& target : context.resolved_targets)
						{
							if (script.throw_before_action_index.has_value()
								&& index == *script.throw_before_action_index)
							{
								throw std::runtime_error("SetMediaTrackInfo_Value stopped answering");
							}

							if (script.failure_flags[index] != 0)
							{
								produced.actions.push_back(failed_action(
									target.reference.guid,
									"reaper_rejected_value",
									"SetMediaTrackInfo_Value returned false for D_VOL"));

								++index;

								continue;
							}

							project.applied_changes.push_back(target.reference.guid);
							produced.actions.push_back(succeeded_action(target.reference.guid));
							++index;
						}

						return produced;
					})
					== tool_registration_outcome::registered);

				test_executor executor{registry, undo, track_list, aliases};

				undo.begin_turn();

				test_call call;
				call.tool_name = "set_track_state";
				call.track_selectors.reserve(action_count);

				for (std::size_t index = 0; index < action_count; ++index)
				{
					call.track_selectors.push_back(TrackGuidSelector{guid_for_index(index)});
				}

				// Everything before the throw point that was not scripted to fail.
				std::vector<std::string> expected_applied_targets;

				for (std::size_t index = 0; index < throw_index; ++index)
				{
					if (!action_is_scripted_to_fail(pattern, index, action_count, script.failure_flags))
					{
						expected_applied_targets.push_back(guid_for_index(index));
					}
				}

				const test_outcome outcome = executor.execute(call);

				const std::string configuration = describe_seed(failure_pattern_seed) + ", "
					+ std::to_string(action_count) + " actions, " + describe_failure_pattern(pattern)
					+ ", throwing before action " + std::to_string(throw_index) + ": ";

				if (expected_applied_targets.empty())
				{
					configurations_that_threw_before_anything_landed += 1;
				}
				else
				{
					configurations_that_landed_something_before_throwing += 1;
				}

				if (project.applied_changes != expected_applied_targets && first_violation.empty())
				{
					first_violation = configuration + "the project holds "
						+ std::to_string(project.applied_changes.size()) + " changes rather than the "
						+ std::to_string(expected_applied_targets.size()) + " that landed before the throw";
				}

				if (stack.undo_call_count != 0 && first_violation.empty())
				{
					first_violation = configuration + "the framework performed "
						+ std::to_string(stack.undo_call_count) + " undo actions";
				}

				// Requirements 9.9 and 23.8: the block is closed, exactly once.
				if ((stack.begin_block_call_count != 1 || stack.end_block_call_count != 1
						|| stack.open_block_depth != 0)
					&& first_violation.empty())
				{
					first_violation = configuration + "the block was not opened and closed exactly once";
				}

				const test_partial* const partial = partial_pointer(outcome);

				if (partial == nullptr)
				{
					if (first_violation.empty())
					{
						first_violation = configuration + "a throwing handler did not produce a partial";
					}

					continue;
				}

				// The payload's silence, stated as an assertion rather than left
				// implicit: one failed action, no successes named, and therefore no
				// undo report — while the project holds what landed.
				if ((partial->actions.size() != 1 || partial->applied_action_count() != 0)
					&& first_violation.empty())
				{
					first_violation = configuration + "the payload named "
						+ std::to_string(partial->applied_action_count())
						+ " applied actions where a throwing handler reports none";
				}

				if (partial->undo.has_value() && first_violation.empty())
				{
					first_violation = configuration + "a payload naming no applied action carried an "
						"undo report";
				}

				const std::string reason_violation = failure_reason_violation(outcome, failures_inspected);

				if (!reason_violation.empty() && first_violation.empty())
				{
					first_violation = configuration + reason_violation;
				}
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// 8 lengths * 4 patterns * 3 throw points, and exactly one reported failure each —
	// which is the payload's silence, counted.
	REQUIRE(configurations_checked == 96);
	REQUIRE(failures_inspected == 96);

	// Both sides of the interesting split are reached. Without the first guard the
	// property would be satisfied by a corpus that only ever threw on action zero,
	// where there is nothing for a rollback to remove.
	REQUIRE(configurations_that_landed_something_before_throwing == 50);
	REQUIRE(configurations_that_threw_before_anything_landed == 46);
}

// ---------------------------------------------------------------------------
// Property 22 — the type-level half
// ---------------------------------------------------------------------------

TEST_CASE("Property 22: a failure without a reason is not a constructible value", "[daw][tool_executor][property]")
{
	// The example file next door already records that `action_error` and
	// `action_failed` are not default constructible. These are the constructions that
	// would put the hole *back*, which is the part worth pinning.

	// No single-argument construction: there is no "failed with this code, message to
	// follow".
	STATIC_REQUIRE(std::is_constructible_v<action_error, std::string_view> == false);
	STATIC_REQUIRE(std::is_constructible_v<action_error, const char*> == false);
	STATIC_REQUIRE(std::is_constructible_v<action_error, std::string> == false);

	// Two arguments is the only way in.
	STATIC_REQUIRE(std::is_constructible_v<action_error, std::string_view, std::string_view>);

	// And `action_failed` holds the reason **by value**. An `std::optional`, a pointer,
	// or a reference would each reintroduce a state where the reason is not there, and
	// each of those would fail this assertion rather than being caught by a test that
	// happened to look.
	STATIC_REQUIRE(std::is_same_v<decltype(action_failed::error), action_error>);

	// The consequence, which is the property as a type fact: a container of outcomes
	// cannot be grown into a reasonless failure, because there is no such object to
	// emplace. `action_failed` is an aggregate with no constructors, so this is asked of
	// brace initialisation rather than of `std::is_constructible`, which would answer
	// "no" for both and prove nothing.
	STATIC_REQUIRE(is_brace_initializable<action_failed>::value == false);
	STATIC_REQUIRE(is_brace_initializable<action_failed, std::string>::value == false);
	STATIC_REQUIRE(is_brace_initializable<action_failed, std::string, action_error>::value);

	// The same question of `action_error` itself: naming a code without a message is
	// not an initialisation this program admits.
	STATIC_REQUIRE(is_brace_initializable<action_error>::value == false);
	STATIC_REQUIRE(is_brace_initializable<action_error, std::string_view>::value == false);
	STATIC_REQUIRE(is_brace_initializable<action_error, std::string_view, std::string_view>::value);

	// `action_outcome` has exactly the two alternatives, so there is no third shape
	// that could carry a failure without a reason.
	STATIC_REQUIRE(std::variant_size_v<action_outcome> == 2);
	STATIC_REQUIRE(std::is_same_v<std::variant_alternative_t<0, action_outcome>, action_succeeded>);
	STATIC_REQUIRE(std::is_same_v<std::variant_alternative_t<1, action_outcome>, action_failed>);

	// And the default alternative is the success, so a default-constructed outcome is
	// not a failure with an empty reason.
	STATIC_REQUIRE(std::is_default_constructible_v<action_outcome>);
	CHECK(action_was_applied(action_outcome{}));
}

// ---------------------------------------------------------------------------
// Property 22 — the runtime half
// ---------------------------------------------------------------------------

TEST_CASE("Property 22: a reason a handler hands in is substituted or clamped, never dropped", "[daw][tool_executor][property]")
{
	// The bounds are the schema's, and a handler can hand back anything. The invariant
	// is "never empty after construction and never over the bound" rather than
	// "rejected on the way in", because throwing from a result type on the REAPER main
	// thread would trade a vague failure report for a worse problem.
	DeterministicBytes bytes{failure_reason_seed};

	std::string first_violation;

	std::size_t combinations_checked = 0;
	std::size_t combinations_with_an_empty_field = 0;
	std::size_t combinations_with_an_over_long_field = 0;
	std::size_t combinations_exactly_at_a_bound = 0;

	const std::vector<std::size_t> target_lengths =
		string_lengths_around(maximum_action_target_length);
	const std::vector<std::size_t> code_lengths =
		string_lengths_around(maximum_action_error_code_length);
	const std::vector<std::size_t> message_lengths =
		string_lengths_around(maximum_action_error_message_length);

	for (const std::size_t target_length : target_lengths)
	{
		for (const std::size_t code_length : code_lengths)
		{
			for (const std::size_t message_length : message_lengths)
			{
				combinations_checked += 1;

				const std::string supplied_target = generated_string_of_length(target_length, bytes);
				const std::string supplied_code = generated_string_of_length(code_length, bytes);
				const std::string supplied_message = generated_string_of_length(message_length, bytes);

				if (target_length == 0 || code_length == 0 || message_length == 0)
				{
					combinations_with_an_empty_field += 1;
				}

				if (target_length > maximum_action_target_length
					|| code_length > maximum_action_error_code_length
					|| message_length > maximum_action_error_message_length)
				{
					combinations_with_an_over_long_field += 1;
				}

				if (target_length == maximum_action_target_length
					|| code_length == maximum_action_error_code_length
					|| message_length == maximum_action_error_message_length)
				{
					combinations_exactly_at_a_bound += 1;
				}

				const action_outcome outcome =
					failed_action(supplied_target, supplied_code, supplied_message);

				const std::string configuration = describe_seed(failure_reason_seed) + ", target "
					+ std::to_string(target_length) + ", code " + std::to_string(code_length)
					+ ", message " + std::to_string(message_length) + ": ";

				if (action_was_applied(outcome))
				{
					if (first_violation.empty())
					{
						first_violation = configuration + "a failed action came back as applied";
					}

					continue;
				}

				const action_failed& failed = std::get<action_failed>(outcome);

				const std::string target_violation = bounded_string_violation(
					"the target",
					supplied_target,
					failed.target,
					unnamed_action_target,
					maximum_action_target_length);

				const std::string code_violation = bounded_string_violation(
					"the code",
					supplied_code,
					failed.error.code(),
					unnamed_failure_code,
					maximum_action_error_code_length);

				const std::string message_violation = bounded_string_violation(
					"the message",
					supplied_message,
					failed.error.message(),
					unnamed_failure_message,
					maximum_action_error_message_length);

				for (const std::string& violation : {target_violation, code_violation, message_violation})
				{
					if (!violation.empty() && first_violation.empty())
					{
						first_violation = configuration + violation;
					}
				}
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// 7 * 7 * 7, asserted so a cross product that stopped crossing is visible. The
	// three splits are exact: 343 minus 6^3 combinations avoid an empty field, 343
	// minus 5^3 have at least one field over its bound, and the same arithmetic gives
	// the count sitting exactly on one.
	REQUIRE(combinations_checked == 343);
	REQUIRE(combinations_with_an_empty_field == 127);
	REQUIRE(combinations_with_an_over_long_field == 218);
	REQUIRE(combinations_exactly_at_a_bound == 127);
}

TEST_CASE("Property 22: every failure the executor reports carries a reason, on every path", "[daw][tool_executor][property]")
{
	// Over the outcomes rather than over the factory: the property is about "any
	// action array outcome", and the paths that produce one without a handler saying so
	// are the ones worth quantifying over — a body that threw, a precondition that
	// threw, a read tool that threw, an undo block that would not close, and a handler
	// that hands back nothing but empty strings.
	std::string first_violation;

	std::size_t paths_checked = 0;
	std::size_t failures_inspected = 0;

	enum class failure_path
	{
		handler_reports_empty_strings,
		handler_reports_only_failures,
		mutating_handler_throws,
		mutating_handler_throws_a_non_standard_exception,
		precondition_check_throws,
		read_tool_throws,
		rewinding_tool_throws,
		undo_block_will_not_close
	};

	const std::vector<failure_path> paths{
		failure_path::handler_reports_empty_strings,
		failure_path::handler_reports_only_failures,
		failure_path::mutating_handler_throws,
		failure_path::mutating_handler_throws_a_non_standard_exception,
		failure_path::precondition_check_throws,
		failure_path::read_tool_throws,
		failure_path::rewinding_tool_throws,
		failure_path::undo_block_will_not_close
	};

	for (const failure_path path : paths)
	{
		paths_checked += 1;

		scripted_project_state project;
		scripted_undo_stack stack{project};
		undo_manager undo{stack};
		scripted_track_list track_list{{}};
		NoLearnedAliases aliases;

		test_registry registry;
		std::string tool_name;

		switch (path)
		{
			case failure_path::handler_reports_empty_strings:
				tool_name = "set_track_state";
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						handler_action_outcomes produced;

						// A handler that knows something went wrong and has nothing to
						// say about it. Substituted rather than dropped.
						produced.actions.push_back(failed_action("", "", ""));
						produced.actions.push_back(failed_action(
							std::string(maximum_action_target_length * 2, 't'),
							std::string(maximum_action_error_code_length * 2, 'c'),
							std::string(maximum_action_error_message_length * 2, 'm')));

						return produced;
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::handler_reports_only_failures:
				tool_name = "set_item_properties";
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						handler_action_outcomes produced;

						produced.actions.push_back(failed_action(
							"item 0", "reaper_rejected_value", "SetMediaItemInfo_Value returned false"));

						return produced;
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::mutating_handler_throws:
				tool_name = "create_track";
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						throw std::runtime_error("InsertTrackAtIndex returned no track");
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::mutating_handler_throws_a_non_standard_exception:
				tool_name = "delete_track";
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						throw 17;
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::precondition_check_throws:
				tool_name = "create_send";
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						return handler_success<test_payload>{};
					},
					[](const test_context&) -> std::optional<tool_refusal> {
						throw std::runtime_error("GetTrackNumSends returned a negative count");
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::read_tool_throws:
				tool_name = "list_installed_fx";
				REQUIRE(registry.register_read_tool(
					tool_name,
					[](const test_context&) -> non_mutating_handler_result<test_payload> {
						throw std::runtime_error("EnumInstalledFX returned nothing");
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::rewinding_tool_throws:
				tool_name = "undo_last_action";
				REQUIRE(registry.register_rewinding_tool(
					tool_name,
					[](const test_context&) -> non_mutating_handler_result<test_payload> {
						throw std::runtime_error("Undo_DoUndo2 reported no entry to undo");
					})
					== tool_registration_outcome::registered);

				break;

			case failure_path::undo_block_will_not_close:
				tool_name = "duplicate_track";
				stack.throw_from_end_block = true;
				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[](const test_context&) -> mutating_handler_result<test_payload> {
						return handler_success<test_payload>{test_payload{"track duplicated"}};
					})
					== tool_registration_outcome::registered);

				break;
		}

		test_executor executor{registry, undo, track_list, aliases};

		undo.begin_turn();

		test_call call;
		call.tool_name = tool_name;

		const test_outcome outcome = executor.execute(call);

		const std::string configuration = "the " + tool_name + " path: ";

		const std::size_t failures_before = failures_inspected;
		const std::string reason_violation = failure_reason_violation(outcome, failures_inspected);

		if (!reason_violation.empty() && first_violation.empty())
		{
			first_violation = configuration + reason_violation;
		}

		// Every one of these paths must have produced at least one failure to inspect.
		// Without this the property would be satisfied by a framework that reported
		// nothing at all.
		if (failures_inspected == failures_before && first_violation.empty())
		{
			first_violation = configuration + "produced no failure for the property to check";
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Eight paths, nine failures: the empty-strings path reports two, every other path
	// reports one. Asserted exactly, because a path that stopped producing a failure is
	// caught by the per-path guard above and a path that started producing two would
	// mean the framework had begun reporting something new.
	REQUIRE(paths_checked == 8);
	REQUIRE(failures_inspected == 9);
}

// ---------------------------------------------------------------------------
// Property 28
// ---------------------------------------------------------------------------

TEST_CASE("Property 28: an inverted time range is a failed action with a reason, not a refusal", "[daw][tool_executor][property]")
{
	// The amended requirement 9.6 in the type system. `check_end_after_start` returns
	// an `action_error`, and a `tool_refusal` return would not survive this assertion —
	// which is the shape the property is here to hold, not merely the value.
	STATIC_REQUIRE(
		std::is_same_v<decltype(check_end_after_start(0.0, 1.0)), std::optional<action_error>>);
	STATIC_REQUIRE(
		std::is_same_v<decltype(check_end_after_start(0.0, 1.0)), std::optional<tool_refusal>>
			== false);

	// And `action_error` is not a refusal in disguise: it has no `blocking` list and no
	// acknowledgement field, so there is nothing for a producer to confirm.
	STATIC_REQUIRE(std::is_convertible_v<action_error, tool_refusal> == false);
	STATIC_REQUIRE(std::is_convertible_v<tool_refusal, action_error> == false);

	std::string first_violation;

	std::size_t pairs_checked = 0;
	std::size_t pairs_accepted = 0;
	std::size_t pairs_rejected = 0;
	std::size_t pairs_with_a_not_a_number = 0;
	std::size_t pairs_that_were_equal = 0;
	std::size_t pairs_with_an_infinity = 0;
	std::size_t pairs_with_both_infinities = 0;
	std::size_t pairs_that_were_both_negative = 0;
	std::size_t pairs_adjacent_in_double_precision = 0;

	const std::vector<double>& positions = swept_time_positions();

	for (const double start_seconds : positions)
	{
		for (const double end_seconds : positions)
		{
			pairs_checked += 1;

			const bool expected = reference_end_strictly_follows_start(start_seconds, end_seconds);
			const bool observed = time_range_is_well_formed(start_seconds, end_seconds);
			const std::optional<action_error> failure =
				check_end_after_start(start_seconds, end_seconds);

			const std::string configuration = "start " + describe_double(start_seconds) + ", end "
				+ describe_double(end_seconds) + ": ";

			if (expected)
			{
				pairs_accepted += 1;
			}
			else
			{
				pairs_rejected += 1;
			}

			if (is_not_a_number_by_bits(start_seconds) || is_not_a_number_by_bits(end_seconds))
			{
				pairs_with_a_not_a_number += 1;

				// The refusing direction, asserted explicitly rather than left to fall
				// out of the oracle. Every comparison against NaN is false, and this is
				// the direction that is safe to be wrong in.
				if (observed && first_violation.empty())
				{
					first_violation = configuration + "a range involving NaN was accepted";
				}
			}

			if (!is_not_a_number_by_bits(start_seconds) && !is_not_a_number_by_bits(end_seconds)
				&& total_order_key(start_seconds) == total_order_key(end_seconds))
			{
				pairs_that_were_equal += 1;

				// A zero-length region is not a region, and that includes the pair
				// (-0.0, 0.0), which is two bit patterns naming one position.
				if (observed && first_violation.empty())
				{
					first_violation = configuration + "a zero-length range was accepted";
				}
			}

			const bool start_is_infinite = bits_of(start_seconds) == bits_of(
				std::numeric_limits<double>::infinity())
				|| bits_of(start_seconds) == bits_of(-std::numeric_limits<double>::infinity());
			const bool end_is_infinite = bits_of(end_seconds) == bits_of(
				std::numeric_limits<double>::infinity())
				|| bits_of(end_seconds) == bits_of(-std::numeric_limits<double>::infinity());

			if (start_is_infinite || end_is_infinite)
			{
				pairs_with_an_infinity += 1;
			}

			if (start_is_infinite && end_is_infinite)
			{
				pairs_with_both_infinities += 1;
			}

			if (!is_not_a_number_by_bits(start_seconds) && !is_not_a_number_by_bits(end_seconds)
				&& (bits_of(start_seconds) & double_sign_bit) != 0
				&& (bits_of(end_seconds) & double_sign_bit) != 0)
			{
				pairs_that_were_both_negative += 1;
			}

			if (!is_not_a_number_by_bits(start_seconds) && !is_not_a_number_by_bits(end_seconds)
				&& total_order_key(end_seconds) == total_order_key(start_seconds) + 1)
			{
				pairs_adjacent_in_double_precision += 1;

				// Adjacent at the limit of double precision is still strictly greater,
				// so this is the side of the boundary that must be accepted.
				if (!observed && first_violation.empty())
				{
					first_violation = configuration + "a pair one representable step apart was rejected";
				}
			}

			// The oracle, computed by bit-pattern ordering rather than by `>`.
			if (observed != expected && first_violation.empty())
			{
				first_violation = configuration + "the implementation says "
					+ (observed ? "well formed" : "inverted") + " where the reference ordering says "
					+ (expected ? "well formed" : "inverted");
			}

			// The two entry points onto the same rule must not disagree: a handler
			// asking one and a handler asking the other would otherwise get different
			// answers for the same span.
			if (failure.has_value() == observed && first_violation.empty())
			{
				first_violation = configuration
					+ "check_end_after_start and time_range_is_well_formed disagree";
			}

			if (failure.has_value())
			{
				if (failure->code() != std::string{invalid_time_range_code} && first_violation.empty())
				{
					first_violation = configuration + "the rejection carried the code \""
						+ failure->code() + "\" rather than the invalid time range code";
				}

				if (failure->message().empty() && first_violation.empty())
				{
					first_violation = configuration + "the rejection carried no message";
				}

				if (failure->message().size() > maximum_action_error_message_length
					&& first_violation.empty())
				{
					first_violation = configuration + "the rejection's message is over the schema bound";
				}
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// 34 positions crossed with themselves, and every guard below is measured. They are
	// also derivable, which is the check on the check: two of the thirty-four are NaN,
	// so 34^2 - 32^2 = 132 pairs involve one — and the same arithmetic gives the
	// infinity count, since there are two of those too. Thirty-eight pairs share a
	// position: the corpus holds three values twice over (positive and negative zero
	// name one position, and `std::nextafter` from zero reproduces both denormal
	// neighbours), so 3 * 2^2 + 26 = 38. That leaves (1024 - 38) / 2 = 493 strictly
	// ordered pairs, which is exactly the accepted count.
	REQUIRE(pairs_checked == 1156);
	REQUIRE(pairs_accepted == 493);
	REQUIRE(pairs_rejected == 663);
	REQUIRE(pairs_with_a_not_a_number == 132);
	REQUIRE(pairs_that_were_equal == 38);
	REQUIRE(pairs_with_an_infinity == 132);
	REQUIRE(pairs_with_both_infinities == 4);
	REQUIRE(pairs_that_were_both_negative == 196);
	REQUIRE(pairs_adjacent_in_double_precision == 12);

	// The sweep reaches both answers in quantity. A corpus that only ever produced
	// well-formed ranges would satisfy a check that accepted everything, and one that
	// only ever produced inverted ranges would satisfy a check that accepted nothing.
	REQUIRE(pairs_accepted > 400);
	REQUIRE(pairs_rejected > 400);
}

TEST_CASE("Property 28: a rejected time range does not proceed, and comes back as a failed action", "[daw][tool_executor][property]")
{
	// The other half of requirement 9.6: the operation *does not proceed*. Asserted
	// against the project state a handler would have written to, over the three
	// contexts the requirement names, and over every pair in the sweep.
	std::string first_violation;

	std::size_t calls_checked = 0;
	std::size_t calls_that_applied_the_range = 0;
	std::size_t calls_that_rejected_the_range = 0;
	std::size_t refusals_seen = 0;
	std::size_t failures_inspected = 0;

	const std::vector<double>& positions = swept_time_positions();

	for (const std::string& tool_name : time_range_tool_names())
	{
		for (const double start_seconds : positions)
		{
			for (const double end_seconds : positions)
			{
				calls_checked += 1;

				scripted_project_state project;
				scripted_undo_stack stack{project};
				undo_manager undo{stack};
				scripted_track_list track_list{{}};
				NoLearnedAliases aliases;

				test_registry registry;

				REQUIRE(registry.register_mutating_tool(
					tool_name,
					[&project, start_seconds, end_seconds](const test_context&)
						-> mutating_handler_result<test_payload> {
						// What `marker_region_tools.h` will do in task 10.8: check the
						// span before writing anything.
						const std::optional<action_error> invalid_range =
							check_end_after_start(start_seconds, end_seconds);

						handler_action_outcomes produced;

						if (invalid_range.has_value())
						{
							produced.actions.push_back(failed_action(
								"the requested span", invalid_range->code(), invalid_range->message()));

							return produced;
						}

						project.applied_changes.push_back("the requested span");
						produced.actions.push_back(succeeded_action("the requested span"));

						return produced;
					})
					== tool_registration_outcome::registered);

				test_executor executor{registry, undo, track_list, aliases};

				undo.begin_turn();

				test_call call;
				call.tool_name = tool_name;

				const test_outcome outcome = executor.execute(call);

				const bool expected_to_apply =
					reference_end_strictly_follows_start(start_seconds, end_seconds);

				if (expected_to_apply)
				{
					calls_that_applied_the_range += 1;
				}
				else
				{
					calls_that_rejected_the_range += 1;
				}

				const std::string configuration = tool_name + ", start " + describe_double(start_seconds)
					+ ", end " + describe_double(end_seconds) + ": ";

				// "Never as a refusal." An inverted range names no acknowledgement the
				// producer could confirm, so a refusal here would offer them a question
				// with no answer.
				if (outcome_is_refusal(outcome))
				{
					refusals_seen += 1;

					if (first_violation.empty())
					{
						first_violation = configuration + "an inverted range came back as a refusal";
					}
				}

				// The operation did not proceed.
				const std::size_t expected_change_count = expected_to_apply ? 1 : 0;

				if (project.applied_changes.size() != expected_change_count && first_violation.empty())
				{
					first_violation = configuration + "the project holds "
						+ std::to_string(project.applied_changes.size()) + " changes rather than "
						+ std::to_string(expected_change_count);
				}

				const test_partial* const partial = partial_pointer(outcome);

				if (partial == nullptr)
				{
					if (first_violation.empty())
					{
						first_violation = configuration + "the outcome was not a partial";
					}

					continue;
				}

				if (partial->actions.size() != 1 && first_violation.empty())
				{
					first_violation = configuration + "the payload carries "
						+ std::to_string(partial->actions.size()) + " outcomes rather than one";
				}

				if (partial->applied_action_count() != expected_change_count && first_violation.empty())
				{
					first_violation = configuration + "the payload names "
						+ std::to_string(partial->applied_action_count())
						+ " applied actions rather than " + std::to_string(expected_change_count);
				}

				// Nothing landed, so there is no position for "revert all" to walk back
				// to and none is offered.
				if (partial->undo.has_value() != expected_to_apply && first_violation.empty())
				{
					first_violation = configuration + "the undo report was "
						+ (partial->undo.has_value() ? "present" : "absent")
						+ " where the opposite was called for";
				}

				if (!expected_to_apply)
				{
					const action_failed* const failed =
						std::get_if<action_failed>(&partial->actions.front());

					if (failed == nullptr)
					{
						if (first_violation.empty())
						{
							first_violation = configuration + "an inverted range was reported as applied";
						}

						continue;
					}

					if (failed->error.code() != std::string{invalid_time_range_code}
						&& first_violation.empty())
					{
						first_violation = configuration + "the failed action carried the code \""
							+ failed->error.code() + "\" rather than the invalid time range code";
					}
				}

				const std::string reason_violation = failure_reason_violation(outcome, failures_inspected);

				if (!reason_violation.empty() && first_violation.empty())
				{
					first_violation = configuration + reason_violation;
				}
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// 3 tools * 34 * 34, and the split is three times the sweep above — which is the
	// cross-check that the executor's answer and the bare check's answer are the same
	// answer.
	REQUIRE(calls_checked == 3468);
	REQUIRE(calls_that_applied_the_range == 1479);
	REQUIRE(calls_that_rejected_the_range == 1989);

	// Not one of the 1989 rejections took the refusal path, which is the amended
	// requirement 9.6 stated as a count.
	REQUIRE(refusals_seen == 0);

	// Every rejection produced exactly one failure for Property 22's check to inspect.
	REQUIRE(failures_inspected == 1989);
}
