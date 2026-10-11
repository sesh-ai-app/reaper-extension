// Structural role derivation.
//
// Three things are worth testing here and one thing is not. The schema spelling has
// to be exact, because the Project Context Builder serialises it and the server
// validates it. The summing-versus-silent line has to be right, because the role is
// reported to the agent and relayed to the producer, so a misclassification tells
// them something untrue about their own routing. And the derivation has to report the
// signals it used, because that is the only thing that makes a wrong role
// diagnosable.
//
// What is not tested here is that the function has no access to track order or
// position. There is nothing to test: `StructuralRoleSignals` carries no index,
// GUID, name, or pointer, so a derivation that depended on any of them would not
// compile. The order-independence cases below check the observable consequence
// anyway, since that is what requirement 18.3 states.
//
// Properties 8 and 9 (task 7.2) generate over the same surface. These are the
// examples: the specific combinations a reader needs to see spelled out.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/structural_role.h>

using sesh_ai::context::StructuralRole;
using sesh_ai::context::StructuralRoleSignals;
using sesh_ai::context::all_structural_roles;
using sesh_ai::context::derive_structural_role;
using sesh_ai::context::describe_structural_role_derivation;
using sesh_ai::context::normalize_structural_role_signals;
using sesh_ai::context::signals_describe_folder_parent;
using sesh_ai::context::structural_role_from_schema_string;
using sesh_ai::context::to_schema_string;

namespace
{
	// An ordinary top-level track carrying its own material. Cases below start from
	// this and change only the signals they are about.
	constexpr StructuralRoleSignals ordinary_track_signals()
	{
		StructuralRoleSignals signals{};
		signals.is_master_track = false;
		signals.accumulated_folder_depth = 0;
		signals.child_track_count = 0;
		signals.children_with_parent_send_enabled = 0;
		signals.parent_send_enabled = true;
		signals.item_count = 3;
		signals.has_fx = false;
		signals.receive_count = 0;

		return signals;
	}

	// A folder parent with three children, none of which feed it, and nothing of its
	// own: the silent case exactly.
	constexpr StructuralRoleSignals silent_folder_parent_signals()
	{
		StructuralRoleSignals signals{};
		signals.child_track_count = 3;
		signals.children_with_parent_send_enabled = 0;
		signals.parent_send_enabled = true;
		signals.item_count = 0;
		signals.has_fx = false;
		signals.receive_count = 0;

		return signals;
	}
}

TEST_CASE("role strings match the schema enumeration exactly", "[context][structural-role]")
{
	// The five strings as `structuralRole` spells them in project-context.schema.json
	// and tool-output-defs.schema.json. Written out rather than derived, so a change
	// to the enum cannot quietly agree with itself.
	const std::vector<std::string_view> schema_enumeration{
		"summing_folder_parent",
		"silent_folder_parent",
		"aux_return",
		"normal",
		"master"
	};

	SECTION("every role serialises to a schema value")
	{
		std::vector<std::string_view> serialised;

		for (StructuralRole role : all_structural_roles)
		{
			serialised.push_back(to_schema_string(role));
		}

		REQUIRE(serialised == schema_enumeration);
	}

	SECTION("every schema value parses back to the role it came from")
	{
		for (StructuralRole role : all_structural_roles)
		{
			const auto parsed = structural_role_from_schema_string(to_schema_string(role));

			REQUIRE(parsed.has_value());
			REQUIRE(*parsed == role);
		}
	}

	SECTION("anything else does not parse")
	{
		// "folder_parent" is the name the task list uses for the role the schemas call
		// summing_folder_parent, so it is the plausible wrong string and worth naming.
		REQUIRE_FALSE(structural_role_from_schema_string("folder_parent").has_value());
		REQUIRE_FALSE(structural_role_from_schema_string("bus").has_value());
		REQUIRE_FALSE(structural_role_from_schema_string("Normal").has_value());
		REQUIRE_FALSE(structural_role_from_schema_string("").has_value());
	}
}

TEST_CASE("the master track is the master role whatever else is true of it", "[context][structural-role]")
{
	StructuralRoleSignals signals = silent_folder_parent_signals();
	signals.is_master_track = true;
	signals.receive_count = 4;
	signals.has_fx = true;

	const auto derivation = derive_structural_role(signals);

	REQUIRE(derivation.role == StructuralRole::master);
	REQUIRE_FALSE(signals_describe_folder_parent(signals));
}

TEST_CASE("a folder parent sums when anything reaches it", "[context][structural-role]")
{
	SECTION("one child with its parent send on is enough")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.children_with_parent_send_enabled = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}

	SECTION("all children feeding it")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.children_with_parent_send_enabled = signals.child_track_count;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}

	SECTION("its own media items, with no child feeding it")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.item_count = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}

	SECTION("an explicit receive, with no child feeding it")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.receive_count = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}

	SECTION("an FX chain, with no child feeding it")
	{
		// The chain might hold an instrument. Since that cannot be told from the chain
		// without instantiating it, the parent is not claimed silent — telling the
		// producer a live bus is dead is worse than saying nothing about it.
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.has_fx = true;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}

	SECTION("being a folder parent overrides what it would otherwise look like")
	{
		// Receives and no items would be an aux return on a childless track.
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.receive_count = 2;
		signals.item_count = 0;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::summing_folder_parent);
	}
}

TEST_CASE("a folder parent is silent only when every source of signal is absent", "[context][structural-role]")
{
	const auto derivation = derive_structural_role(silent_folder_parent_signals());

	REQUIRE(derivation.role == StructuralRole::silent_folder_parent);
	REQUIRE(signals_describe_folder_parent(derivation.signals));

	SECTION("nesting level does not change it")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.accumulated_folder_depth = 2;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::silent_folder_parent);
	}

	SECTION("its own parent send state does not change it")
	{
		// Whether the parent reaches the master says nothing about whether anything
		// reaches the parent.
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.parent_send_enabled = false;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::silent_folder_parent);
	}
}

TEST_CASE("a track fed by sends with no material of its own is an aux return", "[context][structural-role]")
{
	SECTION("a reverb return")
	{
		StructuralRoleSignals signals{};
		signals.receive_count = 4;
		signals.item_count = 0;
		signals.has_fx = true;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::aux_return);
	}

	SECTION("a return with no FX is still a return")
	{
		StructuralRoleSignals signals{};
		signals.receive_count = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::aux_return);
	}

	SECTION("a return inside a folder is still a return")
	{
		StructuralRoleSignals signals{};
		signals.accumulated_folder_depth = 3;
		signals.receive_count = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::aux_return);
	}

	SECTION("a return whose output does not reach its parent is still a return")
	{
		StructuralRoleSignals signals{};
		signals.receive_count = 1;
		signals.parent_send_enabled = false;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::aux_return);
	}

	SECTION("receives plus its own items is not a return")
	{
		StructuralRoleSignals signals{};
		signals.receive_count = 2;
		signals.item_count = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::normal);
	}
}

TEST_CASE("anything else is a normal track", "[context][structural-role]")
{
	SECTION("a track carrying its own material")
	{
		REQUIRE(derive_structural_role(ordinary_track_signals()).role == StructuralRole::normal);
	}

	SECTION("an empty track")
	{
		REQUIRE(derive_structural_role(StructuralRoleSignals{}).role == StructuralRole::normal);
	}

	SECTION("an empty track with an FX chain")
	{
		StructuralRoleSignals signals{};
		signals.has_fx = true;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::normal);
	}

	SECTION("a track inside a folder feeding its parent")
	{
		StructuralRoleSignals signals = ordinary_track_signals();
		signals.accumulated_folder_depth = 1;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::normal);
	}

	SECTION("a track routed around its folder parent")
	{
		StructuralRoleSignals signals = ordinary_track_signals();
		signals.accumulated_folder_depth = 1;
		signals.parent_send_enabled = false;

		REQUIRE(derive_structural_role(signals).role == StructuralRole::normal);
	}
}

TEST_CASE("the derivation reports the signals it used", "[context][structural-role]")
{
	SECTION("in-range signals come back unchanged")
	{
		const StructuralRoleSignals signals = ordinary_track_signals();
		const auto derivation = derive_structural_role(signals);

		REQUIRE(derivation.signals == signals);
	}

	SECTION("the log line carries the role and every signal")
	{
		StructuralRoleSignals signals = silent_folder_parent_signals();
		signals.accumulated_folder_depth = 1;

		const std::string line = describe_structural_role_derivation(derive_structural_role(signals));

		REQUIRE(line.find("role=silent_folder_parent") != std::string::npos);
		REQUIRE(line.find("master=false") != std::string::npos);
		REQUIRE(line.find("accumulatedFolderDepth=1") != std::string::npos);
		REQUIRE(line.find("childTrackCount=3") != std::string::npos);
		REQUIRE(line.find("childrenWithParentSendEnabled=0") != std::string::npos);
		REQUIRE(line.find("parentSendEnabled=true") != std::string::npos);
		REQUIRE(line.find("itemCount=0") != std::string::npos);
		REQUIRE(line.find("hasFx=false") != std::string::npos);
		REQUIRE(line.find("receiveCount=0") != std::string::npos);
	}
}

TEST_CASE("out-of-range counts are normalized rather than refused", "[context][structural-role]")
{
	SECTION("negative counts clamp to zero and are reported clamped")
	{
		StructuralRoleSignals signals{};
		signals.accumulated_folder_depth = -4;
		signals.child_track_count = -1;
		signals.children_with_parent_send_enabled = -2;
		signals.item_count = -7;
		signals.receive_count = -3;

		const auto derivation = derive_structural_role(signals);

		REQUIRE(derivation.role == StructuralRole::normal);
		REQUIRE(derivation.signals.accumulated_folder_depth == 0);
		REQUIRE(derivation.signals.child_track_count == 0);
		REQUIRE(derivation.signals.children_with_parent_send_enabled == 0);
		REQUIRE(derivation.signals.item_count == 0);
		REQUIRE(derivation.signals.receive_count == 0);
	}

	SECTION("more feeding children than children caps at the number of children")
	{
		StructuralRoleSignals signals{};
		signals.child_track_count = 2;
		signals.children_with_parent_send_enabled = 9;

		const auto derivation = derive_structural_role(signals);

		REQUIRE(derivation.role == StructuralRole::summing_folder_parent);
		REQUIRE(derivation.signals.children_with_parent_send_enabled == 2);
	}

	SECTION("feeding children on a track with no children clamps to zero and leaves it silent-free")
	{
		// Contradictory input: nothing is inside the folder, yet something is said to
		// feed it through one. Clamping makes it a childless track, so it classifies on
		// its own signals rather than as a folder parent.
		StructuralRoleSignals signals{};
		signals.child_track_count = 0;
		signals.children_with_parent_send_enabled = 5;
		signals.receive_count = 1;

		const auto derivation = derive_structural_role(signals);

		REQUIRE(derivation.role == StructuralRole::aux_return);
		REQUIRE(derivation.signals.children_with_parent_send_enabled == 0);
	}

	SECTION("normalization is idempotent")
	{
		StructuralRoleSignals signals{};
		signals.accumulated_folder_depth = -1;
		signals.child_track_count = 1;
		signals.children_with_parent_send_enabled = 6;

		const StructuralRoleSignals once = normalize_structural_role_signals(signals);

		REQUIRE(normalize_structural_role_signals(once) == once);
	}
}

TEST_CASE("derivation depends on nothing but the signals", "[context][structural-role]")
{
	// Requirement 18.3. A grid of combinations, classified once in order and once in
	// reverse, with the roles compared. Nothing in the signals says where a track sits
	// or when it was visited, so visiting order cannot change an answer — this checks
	// the consequence.
	std::vector<StructuralRoleSignals> grid;

	for (int depth = 0; depth <= 2; ++depth)
	{
		for (int child_track_count = 0; child_track_count <= 2; ++child_track_count)
		{
			for (int feeding_children = 0; feeding_children <= child_track_count; ++feeding_children)
			{
				for (int item_count = 0; item_count <= 2; ++item_count)
				{
					for (int receive_count = 0; receive_count <= 2; ++receive_count)
					{
						for (bool has_fx : {false, true})
						{
							for (bool parent_send_enabled : {false, true})
							{
								StructuralRoleSignals signals{};
								signals.accumulated_folder_depth = depth;
								signals.child_track_count = child_track_count;
								signals.children_with_parent_send_enabled = feeding_children;
								signals.item_count = item_count;
								signals.receive_count = receive_count;
								signals.has_fx = has_fx;
								signals.parent_send_enabled = parent_send_enabled;

								grid.push_back(signals);
							}
						}
					}
				}
			}
		}
	}

	REQUIRE(grid.size() > 100);

	std::map<std::string, StructuralRole> role_by_signals;

	for (const StructuralRoleSignals& signals : grid)
	{
		const auto derivation = derive_structural_role(signals);

		// Keyed by the signal log line, which is the signals and nothing else.
		const std::string key = describe_structural_role_derivation(
			{StructuralRole::normal, derivation.signals}
		);

		const auto existing = role_by_signals.find(key);

		if (existing == role_by_signals.end())
		{
			role_by_signals.emplace(key, derivation.role);
		}
		else
		{
			REQUIRE(existing->second == derivation.role);
		}

		// The signals travel with the role every time, not just in the cases above.
		REQUIRE(derivation.signals == normalize_structural_role_signals(signals));
	}

	for (auto reverse = grid.rbegin(); reverse != grid.rend(); ++reverse)
	{
		const auto derivation = derive_structural_role(*reverse);
		const std::string key = describe_structural_role_derivation(
			{StructuralRole::normal, derivation.signals}
		);

		REQUIRE(role_by_signals.at(key) == derivation.role);
	}

	SECTION("every role in the grid is one of the five")
	{
		for (const auto& entry : role_by_signals)
		{
			const auto parsed = structural_role_from_schema_string(
				to_schema_string(entry.second)
			);

			REQUIRE(parsed.has_value());
		}
	}
}

TEST_CASE("derivation is a compile-time function", "[context][structural-role]")
{
	// Not a convenience. A constexpr derivation cannot read a global, call into
	// REAPER, or allocate, so requirement 18.3's purity is checked by the compiler
	// rather than by review.
	STATIC_REQUIRE(derive_structural_role(silent_folder_parent_signals()).role
		== StructuralRole::silent_folder_parent);
	STATIC_REQUIRE(derive_structural_role(ordinary_track_signals()).role == StructuralRole::normal);
	STATIC_REQUIRE(to_schema_string(StructuralRole::summing_folder_parent) == "summing_folder_parent");
}
