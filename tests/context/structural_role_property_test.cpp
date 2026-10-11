// Structural role derivation — Properties 8 and 9 (task 7.2).
//
// `structural_role_test.cpp` holds the examples: the specific combinations a reader
// needs to see spelled out, plus a grid of a few hundred that checks the derivation
// against itself in two traversal directions. This file makes the two formal
// properties, and it does so exhaustively rather than by sampling.
//
// Catch2 v3 in this build has no generator library, so the repo's convention is to
// enumerate the input space where it is small enough and to use a deterministic
// seeded generator where it is not (see `tests/daw/folder_invariant_keeper_test.cpp`
// for the first, `DeterministicBytes` in `tests/daw/alias_store_test.cpp` for the
// second). `StructuralRoleSignals` is eight scalars, five of which are counts, so a
// bounded sweep over every one of them is 50,400 combinations — cheap enough to
// check exhaustively, which is a stronger statement than sampling and a
// reproducible one. The sweep asserts its own size against a literal, so a sweep
// that silently stopped generating cannot make the properties below vacuous.
//
// The bounds run below zero deliberately. Requirement 18.1 constrains accumulated
// depth, item count, and receive count to non-negative values, and
// `normalize_structural_role_signals` is what makes that true of every input rather
// than of well-behaved ones — so the negative and the contradictory cases (more
// children feeding the parent than there are children, children feeding a folder
// that holds none) are the cases worth sweeping, not the ones to exclude.
//
// **Two things about these properties are structural rather than tested, and saying
// so is part of stating them honestly.**
//
// "Exactly one role" is true by construction: `derive_structural_role` returns a
// single `StructuralRole`, so there is no representation in which two roles come
// back, and a test asserting one role per call asserts nothing. What has content is
// that the derivation is *total* — every combination of signals, including the
// nonsense ones, yields a role drawn from the five-value enumeration, and no value
// outside it is reachable. That is Property 8 below, checked against
// `all_structural_roles` directly rather than through `to_schema_string`, whose
// unreachable fallback would report a non-enumerator as "normal" and hide exactly
// the failure the property is looking for.
//
// Order-independence is structural in the same way: `StructuralRoleSignals` carries
// no index, GUID, name, pointer, or iterator, so a derivation that depended on
// where a track sits in the list could not be written, let alone compile. What has
// content is that classification is a deterministic function of the signals — same
// signals, same answer, whenever and in whatever sequence it is asked. That is
// Property 9 below: the whole sweep derived forward, again in reverse, and again in
// a seeded permutation, keyed by the *normalized* signals so that the 50,400 raw
// inputs collapsing onto 2,880 normalized ones are all required to agree with each
// other.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/structural_role.h>

using sesh_ai::context::StructuralRole;
using sesh_ai::context::StructuralRoleDerivation;
using sesh_ai::context::StructuralRoleSignals;
using sesh_ai::context::all_structural_roles;
using sesh_ai::context::derive_structural_role;
using sesh_ai::context::describe_structural_role_derivation;
using sesh_ai::context::normalize_structural_role_signals;
using sesh_ai::context::to_schema_string;

namespace
{
	// The swept range of each counted signal. Every one starts below zero, because
	// the clamping in `normalize_structural_role_signals` is the reason requirement
	// 18.1's non-negative constraint holds for arbitrary input, and a sweep that
	// avoided negatives would never exercise it.
	//
	// The feeding-children range runs one past the largest child count so that
	// "more children feed me than are inside me" — contradictory, and reachable
	// from a depth accumulation bug upstream — is in the sweep.
	constexpr std::array<int, 6> swept_accumulated_folder_depths{-2, -1, 0, 1, 2, 3};
	constexpr std::array<int, 6> swept_child_track_counts{-2, -1, 0, 1, 2, 3};
	constexpr std::array<int, 7> swept_children_with_parent_send_enabled{-2, -1, 0, 1, 2, 3, 4};
	constexpr std::array<int, 5> swept_item_counts{-2, -1, 0, 1, 2};
	constexpr std::array<int, 5> swept_receive_counts{-2, -1, 0, 1, 2};
	constexpr std::array<bool, 2> swept_flags{false, true};

	// 6 × 6 × 7 × 5 × 5 × 2 × 2 × 2. Written as a literal rather than as a product of
	// the sizes above, so that a range narrowed by accident disagrees with it instead
	// of quietly agreeing.
	constexpr std::size_t expected_sweep_size = 50400;

	// How many distinct signal sets those 50,400 inputs normalize onto, which is the
	// number of answers the derivation actually has to give.
	//
	// Normalization folds the three negative depths onto 0 and the three negative
	// child counts onto 0, leaving 4 depths and 4 child counts; it folds the two
	// negative item counts and the two negative receive counts onto 0, leaving 3 of
	// each; and it caps the feeding count at the child count, leaving child + 1
	// values per child count — 1 + 2 + 3 + 4 = 10 (child, feeding) pairs across the
	// four child counts. The three booleans are untouched.
	//
	//   4 depths × 3 item counts × 3 receive counts × 8 boolean combinations × 10
	//   (child, feeding) pairs = 2,880
	//
	// The gap between this and 50,400 is the point: 47,520 of the sweep's inputs are
	// a second (or eleventh) route to a signal set some other input also produces, so
	// Property 9's agreement check is exercised rather than trivially satisfied.
	constexpr std::size_t expected_distinct_normalized_signal_count = 2880;

	// Orders the aggregate so it can key a map. Not part of the component's
	// interface — `StructuralRoleSignals` has equality because the derivation's
	// callers compare signal sets, and nothing in the extension needs them sorted.
	struct SignalsLessThan
	{
		bool operator()(const StructuralRoleSignals& left, const StructuralRoleSignals& right) const
		{
			return std::tie(
				left.is_master_track,
				left.accumulated_folder_depth,
				left.child_track_count,
				left.children_with_parent_send_enabled,
				left.parent_send_enabled,
				left.item_count,
				left.has_fx,
				left.receive_count
			) < std::tie(
				right.is_master_track,
				right.accumulated_folder_depth,
				right.child_track_count,
				right.children_with_parent_send_enabled,
				right.parent_send_enabled,
				right.item_count,
				right.has_fx,
				right.receive_count
			);
		}
	};

	using RoleBySignals = std::map<StructuralRoleSignals, StructuralRole, SignalsLessThan>;

	// Every combination of the swept ranges, in a fixed order.
	std::vector<StructuralRoleSignals> exhaustive_signal_sweep()
	{
		std::vector<StructuralRoleSignals> sweep;
		sweep.reserve(expected_sweep_size);

		for (bool is_master_track : swept_flags)
		{
			for (int accumulated_folder_depth : swept_accumulated_folder_depths)
			{
				for (int child_track_count : swept_child_track_counts)
				{
					for (int feeding_children : swept_children_with_parent_send_enabled)
					{
						for (bool parent_send_enabled : swept_flags)
						{
							for (int item_count : swept_item_counts)
							{
								for (bool has_fx : swept_flags)
								{
									for (int receive_count : swept_receive_counts)
									{
										StructuralRoleSignals signals{};
										signals.is_master_track = is_master_track;
										signals.accumulated_folder_depth = accumulated_folder_depth;
										signals.child_track_count = child_track_count;
										signals.children_with_parent_send_enabled = feeding_children;
										signals.parent_send_enabled = parent_send_enabled;
										signals.item_count = item_count;
										signals.has_fx = has_fx;
										signals.receive_count = receive_count;

										sweep.push_back(signals);
									}
								}
							}
						}
					}
				}
			}
		}

		return sweep;
	}

	// The same xorshift the alias store's suite uses, for the same reason: a
	// permutation has to be the same one on every machine, so a failure is
	// reproducible from the seed in the assertion message.
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

	private:
		std::uint32_t state_;
	};

	// Fisher-Yates over the indices, so the third traversal is neither the sweep's
	// order nor its reverse.
	std::vector<std::size_t> seeded_permutation(std::size_t count, std::uint32_t seed)
	{
		std::vector<std::size_t> order(count);

		for (std::size_t index = 0; index < count; ++index)
		{
			order[index] = index;
		}

		DeterministicBytes bytes{seed};

		for (std::size_t index = count; index > 1; --index)
		{
			const std::size_t swap_with = bytes.below(index);
			std::swap(order[index - 1], order[swap_with]);
		}

		return order;
	}

	bool is_one_of_the_five_roles(StructuralRole role)
	{
		// Deliberately not `structural_role_from_schema_string(to_schema_string(role))`.
		// `to_schema_string` ends in an unreachable `return "normal"` for values that
		// are not enumerators, so the round trip would report a role outside the
		// enumeration as a valid `normal` — which is the one failure this check exists
		// to catch.
		return std::find(all_structural_roles.begin(), all_structural_roles.end(), role)
			!= all_structural_roles.end();
	}

	// 50,400 cases is too many to assert one at a time and still read the output, so
	// failures are collected and the first few reported together. A property that
	// fails usually fails for a family of inputs, and three or four members of the
	// family say more about why than one does.
	class CounterexampleLog
	{
	public:
		void record(const std::string& description)
		{
			failure_count_ += 1;

			if (reported_.size() < 4)
			{
				reported_.push_back(description);
			}
		}

		std::size_t failure_count() const { return failure_count_; }

		std::string report() const
		{
			std::string text = "counterexamples: " + std::to_string(failure_count_);

			for (const std::string& failure : reported_)
			{
				text += "\n  " + failure;
			}

			return text;
		}

	private:
		std::size_t failure_count_ = 0;
		std::vector<std::string> reported_;
	};

	// Signals rendered the way the component renders them, which is the form a
	// counterexample should arrive in: the same line an operator would read out of a
	// log while diagnosing the misclassification.
	std::string describe_signals(const StructuralRoleSignals& signals)
	{
		return describe_structural_role_derivation(
			StructuralRoleDerivation{derive_structural_role(signals).role, signals}
		);
	}
}

TEST_CASE(
	"Property 8: derivation is total over every signal combination and reports the signals it used",
	"[context][structural-role][property]"
)
{
	// *For any* combination of accumulated depth, parent send state, item count, FX
	// presence, and receive count, derivation yields exactly one role, and the raw
	// signals it derived from are reported alongside it.
	//
	// "Exactly one" is the return type's doing, so what is checked here is the part
	// that could fail: that a role comes back for every input including the
	// contradictory ones, that it is always one of the five, that all five are
	// reachable, and that the signals travelling with it are the normalized ones for
	// every input rather than for the handful the example tests name.
	//
	// **Validates: Requirements 18.1, 18.2**

	const std::vector<StructuralRoleSignals> sweep = exhaustive_signal_sweep();

	REQUIRE(sweep.size() == expected_sweep_size);

	SECTION("every combination yields a role from the five-value enumeration")
	{
		CounterexampleLog log;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation derivation = derive_structural_role(signals);

			if (!is_one_of_the_five_roles(derivation.role))
			{
				log.record(
					"role outside the enumeration, raw value "
					+ std::to_string(static_cast<int>(derivation.role))
					+ " from " + describe_signals(signals)
				);
			}
		}

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}

	SECTION("all five roles are reachable, so totality is not the derivation answering normal")
	{
		// Without this the section above would pass against a derivation that returned
		// `normal` unconditionally. The counts are the sweep's, computed from the
		// normalized space described at the top of this file, and they cross-check
		// against each other: 1,440 master plus 1,440 non-master is the whole of the
		// 2,880 distinct normalized signal sets.
		//
		//   master                — is_master_track, whatever else is true: 1,440
		//   folder parents        — 9 of the 10 (child, feeding) pairs have a child:
		//                           4 × 2 × 2 × 3 × 3 × 9 = 1,296, of which
		//     silent_folder_parent  no child feeding, no items, no receives, no FX:
		//                           4 depths × 2 parent send states × 3 pairs = 24
		//     summing_folder_parent everything else: 1,296 − 24 = 1,272
		//   childless             — the remaining pair: 4 × 2 × 2 × 3 × 3 = 144, of which
		//     aux_return            receives and no items: 4 × 2 × 2 × 2 = 32
		//     normal                everything else: 144 − 32 = 112
		std::map<StructuralRole, std::size_t> distinct_signal_sets_by_role;
		RoleBySignals role_by_signals;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation derivation = derive_structural_role(signals);

			if (role_by_signals.emplace(derivation.signals, derivation.role).second)
			{
				distinct_signal_sets_by_role[derivation.role] += 1;
			}
		}

		REQUIRE(role_by_signals.size() == expected_distinct_normalized_signal_count);

		for (StructuralRole role : all_structural_roles)
		{
			INFO("role " << to_schema_string(role));
			REQUIRE(distinct_signal_sets_by_role[role] > 0);
		}

		REQUIRE(distinct_signal_sets_by_role[StructuralRole::master] == 1440);
		REQUIRE(distinct_signal_sets_by_role[StructuralRole::summing_folder_parent] == 1272);
		REQUIRE(distinct_signal_sets_by_role[StructuralRole::silent_folder_parent] == 24);
		REQUIRE(distinct_signal_sets_by_role[StructuralRole::aux_return] == 32);
		REQUIRE(distinct_signal_sets_by_role[StructuralRole::normal] == 112);
	}

	SECTION("the signals reported are the normalized ones, for every input")
	{
		CounterexampleLog log;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation derivation = derive_structural_role(signals);

			if (derivation.signals != normalize_structural_role_signals(signals))
			{
				log.record("reported signals are not the normalized ones: " + describe_signals(signals));
				continue;
			}

			// Requirement 18.1's constraint, stated over the reported signals because
			// those are what reaches a snapshot and a log.
			const bool counts_are_non_negative = derivation.signals.accumulated_folder_depth >= 0
				&& derivation.signals.child_track_count >= 0
				&& derivation.signals.children_with_parent_send_enabled >= 0
				&& derivation.signals.item_count >= 0
				&& derivation.signals.receive_count >= 0;

			if (!counts_are_non_negative)
			{
				log.record("a reported count is negative: " + describe_signals(signals));
				continue;
			}

			if (derivation.signals.children_with_parent_send_enabled > derivation.signals.child_track_count)
			{
				log.record("more children feed it than are inside it: " + describe_signals(signals));
				continue;
			}

			// The three signals normalization does not touch have to arrive unchanged,
			// or "the raw signals alongside the role" is reporting something the caller
			// did not say.
			const bool flags_pass_through = derivation.signals.is_master_track == signals.is_master_track
				&& derivation.signals.parent_send_enabled == signals.parent_send_enabled
				&& derivation.signals.has_fx == signals.has_fx;

			if (!flags_pass_through)
			{
				log.record("a boolean signal was altered: " + describe_signals(signals));
			}
		}

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}

	SECTION("the log line carries the role and every signal, for every input")
	{
		// Requirement 18.2 is about diagnosability, and the log line is where that
		// lands. The example tests check one line; this checks that no combination of
		// signals produces a line missing one of them — a clamped count in particular,
		// which is the value an operator most needs to see and the one most likely to
		// go unrendered.
		CounterexampleLog log;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation derivation = derive_structural_role(signals);
			const std::string line = describe_structural_role_derivation(derivation);

			const std::vector<std::string> expected_fragments{
				"role=" + std::string{to_schema_string(derivation.role)},
				std::string{"master="} + (derivation.signals.is_master_track ? "true" : "false"),
				"accumulatedFolderDepth=" + std::to_string(derivation.signals.accumulated_folder_depth),
				"childTrackCount=" + std::to_string(derivation.signals.child_track_count),
				"childrenWithParentSendEnabled="
					+ std::to_string(derivation.signals.children_with_parent_send_enabled),
				std::string{"parentSendEnabled="} + (derivation.signals.parent_send_enabled ? "true" : "false"),
				"itemCount=" + std::to_string(derivation.signals.item_count),
				std::string{"hasFx="} + (derivation.signals.has_fx ? "true" : "false"),
				"receiveCount=" + std::to_string(derivation.signals.receive_count)
			};

			for (const std::string& fragment : expected_fragments)
			{
				if (line.find(fragment) == std::string::npos)
				{
					log.record("log line is missing '" + fragment + "': " + line);
					break;
				}
			}
		}

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}
}

TEST_CASE(
	"Property 9: classification is a deterministic function of the signals alone",
	"[context][structural-role][property]"
)
{
	// *For any* pair of identical signal sets, derivation is a pure function:
	// identical signals yield an identical role, independent of track order or
	// position in the list.
	//
	// Track order and position are not representable in `StructuralRoleSignals` —
	// there is no index, GUID, name, pointer, or iterator in the aggregate, so a
	// derivation that consulted one would not compile, and `derive_structural_role`
	// being `constexpr` rules out the global state and the REAPER call that would be
	// the other routes to the same dependency. What remains to check is the
	// observable claim: that asking the same question at different points in a
	// traversal, and in different traversals, never produces a different answer.
	//
	// **Validates: Requirements 18.3**

	const std::vector<StructuralRoleSignals> sweep = exhaustive_signal_sweep();

	REQUIRE(sweep.size() == expected_sweep_size);

	// Keyed by the normalized signals rather than by the raw input, which is what
	// makes this stronger than a traversal check. 50,400 raw inputs normalize onto
	// 2,880 signal sets, so every entry in this map is written once and then
	// confirmed by an average of sixteen other inputs that reached the same signals
	// by a different route — a negative depth against a zero one, a feeding count of
	// four against one of three on a three-child folder.
	RoleBySignals role_by_signals;

	SECTION("forward, reverse, and a seeded permutation all agree")
	{
		CounterexampleLog log;

		const auto check = [&log, &role_by_signals](const StructuralRoleSignals& signals, const char* pass) {
			const StructuralRoleDerivation derivation = derive_structural_role(signals);
			const auto existing = role_by_signals.find(derivation.signals);

			if (existing == role_by_signals.end())
			{
				role_by_signals.emplace(derivation.signals, derivation.role);
				return;
			}

			if (existing->second != derivation.role)
			{
				log.record(
					std::string{"on the "} + pass + " pass the same signals gave "
					+ std::string{to_schema_string(derivation.role)} + " where they gave "
					+ std::string{to_schema_string(existing->second)} + " before: "
					+ describe_structural_role_derivation(derivation)
				);
			}
		};

		for (const StructuralRoleSignals& signals : sweep)
		{
			check(signals, "forward");
		}

		REQUIRE(role_by_signals.size() == expected_distinct_normalized_signal_count);

		for (auto reverse = sweep.rbegin(); reverse != sweep.rend(); ++reverse)
		{
			check(*reverse, "reverse");
		}

		// Forward and reverse both visit a signal set's neighbours in the same
		// relative arrangement, so neither would catch an answer that depended on what
		// was asked two questions ago. A permutation does. The seed is fixed, so a
		// failure here is one anybody can reproduce.
		constexpr std::uint32_t permutation_seed = 0x57a1c1e5u;

		CAPTURE(permutation_seed);

		for (std::size_t index : seeded_permutation(sweep.size(), permutation_seed))
		{
			check(sweep[index], "shuffled");
		}

		// Unchanged by the second and third passes: they only confirmed answers the
		// first pass recorded.
		REQUIRE(role_by_signals.size() == expected_distinct_normalized_signal_count);

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}

	SECTION("deriving the same signals twice in a row gives the same role and the same evidence")
	{
		CounterexampleLog log;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation first = derive_structural_role(signals);
			const StructuralRoleDerivation second = derive_structural_role(signals);

			if (first.role != second.role || first.signals != second.signals)
			{
				log.record("two derivations of the same signals disagreed: " + describe_signals(signals));
			}
		}

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}

	SECTION("deriving from the normalized signals gives what deriving from the raw ones gave")
	{
		// The other half of "a function of the signals": since the derivation reports
		// normalized signals as its evidence, feeding that evidence back in has to
		// reproduce the role. If it did not, the reported signals would not be the ones
		// the role was derived from, and requirement 18.2's diagnosability would be
		// showing an operator the wrong evidence.
		CounterexampleLog log;

		for (const StructuralRoleSignals& signals : sweep)
		{
			const StructuralRoleDerivation from_raw = derive_structural_role(signals);
			const StructuralRoleDerivation from_reported = derive_structural_role(from_raw.signals);

			if (from_raw.role != from_reported.role || from_raw.signals != from_reported.signals)
			{
				log.record(
					"re-deriving from the reported signals changed the answer to "
					+ std::string{to_schema_string(from_reported.role)} + ": "
					+ describe_structural_role_derivation(from_raw)
				);
			}
		}

		INFO(log.report());
		REQUIRE(log.failure_count() == 0);
	}
}
