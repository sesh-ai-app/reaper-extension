// Signal routing cycle detection (requirement 9.7, design properties 26 and 27).
//
// The graph is plain data, so every case here is a synthetic project written out in
// full. That is the point of the component's shape: the routing a producer can
// destroy is checkable without REAPER running.
//
// Two of these cases carry the weight. "a loop that closes only through folder
// routing" is the one a sends-list-only detector fails, and it is the case
// requirement 9.7 names. "an acyclic graph is never refused" is property 27, and its
// diamond and parallel-edge sections are the two specific ways an over-eager
// detector refuses routing a producer is entitled to build.
//
// Readable names stand in for GUIDs throughout. Nothing in the detector parses an
// identity, so a failure that names "kick" rather than a GUID is strictly easier to
// read.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/cycle_detector.h>

namespace
{
	using sesh_ai::daw::bus_construction_plan;
	using sesh_ai::daw::bus_type;
	using sesh_ai::daw::collect_signal_edges;
	using sesh_ai::daw::cycle_detection_result;
	using sesh_ai::daw::derive_folder_parent_indices;
	using sesh_ai::daw::derive_folder_parents;
	using sesh_ai::daw::detect_signal_cycle;
	using sesh_ai::daw::evaluate_bus_construction;
	using sesh_ai::daw::evaluate_send_creation;
	using sesh_ai::daw::project_bus_construction;
	using sesh_ai::daw::project_send_creation;
	using sesh_ai::daw::routing_change_decision;
	using sesh_ai::daw::routing_graph;
	using sesh_ai::daw::send_edge;
	using sesh_ai::daw::signal_edge;
	using sesh_ai::daw::signal_edge_kind;
	using sesh_ai::daw::track_identity;
	using sesh_ai::daw::track_node;

	track_node track(
		track_identity identity,
		int folder_depth_delta = 0,
		bool parent_send_enabled = true)
	{
		track_node node;
		node.identity = std::move(identity);
		node.folder_depth_delta = folder_depth_delta;
		node.parent_send_enabled = parent_send_enabled;

		return node;
	}

	send_edge send(track_identity source_track, track_identity destination_track)
	{
		return send_edge{std::move(source_track), std::move(destination_track)};
	}

	bool contains_edge(
		const std::vector<signal_edge>& edges,
		const track_identity& source_track,
		const track_identity& destination_track,
		signal_edge_kind kind)
	{
		for (const signal_edge& edge : edges)
		{
			if (edge.source_track == source_track
				&& edge.destination_track == destination_track
				&& edge.kind == kind)
			{
				return true;
			}
		}

		return false;
	}

	std::size_t count_edges_from(const std::vector<signal_edge>& edges, const track_identity& source_track)
	{
		std::size_t matches = 0;

		for (const signal_edge& edge : edges)
		{
			if (edge.source_track == source_track)
			{
				++matches;
			}
		}

		return matches;
	}

	// A reported loop has to actually be one: consecutive edges join up and the last
	// edge returns to where the first started. Checked rather than assumed, because a
	// detector that says "cycle" while reporting an unrelated edge list would satisfy
	// every boolean assertion in this file.
	bool edges_form_a_closed_loop(const std::vector<signal_edge>& cycle_edges)
	{
		if (cycle_edges.empty())
		{
			return false;
		}

		for (std::size_t position = 1; position < cycle_edges.size(); ++position)
		{
			if (cycle_edges[position - 1].destination_track != cycle_edges[position].source_track)
			{
				return false;
			}
		}

		return cycle_edges.back().destination_track == cycle_edges.front().source_track;
	}

	// Two tracks, one containing the other. The smallest graph with an implicit edge
	// in it, used by several cases below.
	//
	//   mix      opens a folder, top level
	//     guitars   closes it, parent send on, so it sums into mix
	routing_graph folder_with_one_child()
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("mix", 1),
			track("guitars", -1),
		};

		return graph;
	}
}

TEST_CASE("folder parentage is derived from depth deltas", "[daw][routing]")
{
	SECTION("a flat track list has no parents")
	{
		const std::vector<track_node> tracks = {track("kick"), track("snare"), track("bass")};
		const std::vector<std::optional<std::size_t>> parents = derive_folder_parent_indices(tracks);

		REQUIRE(parents.size() == 3);
		CHECK_FALSE(parents[0].has_value());
		CHECK_FALSE(parents[1].has_value());
		CHECK_FALSE(parents[2].has_value());
	}

	SECTION("a folder parent is a sibling of the tracks above it, not its own child")
	{
		// The delta is applied after the track has been given its own parent. A
		// detector that applied it first would make every folder parent its own
		// parent, which is a self-loop in the graph and a false refusal on every
		// project that has a folder in it.
		const std::vector<track_node> tracks = {track("drums", 1), track("kick", -1)};
		const std::vector<std::optional<std::size_t>> parents = derive_folder_parent_indices(tracks);

		CHECK_FALSE(parents[0].has_value());
		REQUIRE(parents[1].has_value());
		CHECK(*parents[1] == 0);
	}

	SECTION("nesting resolves to the innermost open folder")
	{
		const std::vector<track_node> tracks = {
			track("mix", 1),
			track("drums", 1),
			track("kick"),
			track("snare", -1),
			track("bass", -1),
		};

		const std::vector<std::optional<track_identity>> parents = derive_folder_parents(tracks);

		CHECK_FALSE(parents[0].has_value());
		CHECK(parents[1] == std::optional<track_identity>{"mix"});
		CHECK(parents[2] == std::optional<track_identity>{"drums"});
		CHECK(parents[3] == std::optional<track_identity>{"drums"});
		CHECK(parents[4] == std::optional<track_identity>{"mix"});
	}

	SECTION("one track can close several folders at once")
	{
		const std::vector<track_node> tracks = {
			track("mix", 1),
			track("drums", 1),
			track("kick", -2),
			track("vocals"),
		};

		const std::vector<std::optional<track_identity>> parents = derive_folder_parents(tracks);

		CHECK(parents[2] == std::optional<track_identity>{"drums"});
		CHECK_FALSE(parents[3].has_value());
	}

	SECTION("closing more folders than are open closes the ones that are")
	{
		const std::vector<track_node> tracks = {
			track("drums", 1),
			track("kick", -9),
			track("vocals"),
		};

		const std::vector<std::optional<track_identity>> parents = derive_folder_parents(tracks);

		CHECK(parents[1] == std::optional<track_identity>{"drums"});
		CHECK_FALSE(parents[2].has_value());
	}

	SECTION("a delta above one opens a single folder, which is all REAPER can represent")
	{
		const std::vector<track_node> tracks = {
			track("drums", 3),
			track("kick", -1),
			track("vocals"),
		};

		const std::vector<std::optional<track_identity>> parents = derive_folder_parents(tracks);

		CHECK(parents[1] == std::optional<track_identity>{"drums"});
		CHECK_FALSE(parents[2].has_value());
	}
}

TEST_CASE("the signal graph carries both routing mechanisms", "[daw][routing]")
{
	SECTION("a child with its parent send on feeds its folder parent")
	{
		const std::vector<signal_edge> edges = collect_signal_edges(folder_with_one_child());

		CHECK(contains_edge(edges, "guitars", "mix", signal_edge_kind::folder_parent_send));
	}

	SECTION("a child with its parent send off feeds nothing")
	{
		routing_graph graph = folder_with_one_child();
		graph.tracks_in_project_order[1].parent_send_enabled = false;

		const std::vector<signal_edge> edges = collect_signal_edges(graph);

		CHECK(count_edges_from(edges, "guitars") == 0);
	}

	SECTION("a top-level parent send reaches the master, which is not an edge")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {track("kick"), track("snare")};

		CHECK(collect_signal_edges(graph).empty());
	}

	SECTION("explicit sends appear alongside the implicit edges")
	{
		routing_graph graph = folder_with_one_child();
		graph.explicit_sends = {send("guitars", "mix")};

		const std::vector<signal_edge> edges = collect_signal_edges(graph);

		CHECK(contains_edge(edges, "guitars", "mix", signal_edge_kind::folder_parent_send));
		CHECK(contains_edge(edges, "guitars", "mix", signal_edge_kind::explicit_send));
		CHECK(count_edges_from(edges, "guitars") == 2);
	}
}

TEST_CASE("a loop that closes only through folder routing is detected", "[daw][routing]")
{
	// This is the case requirement 9.7 exists for, and the one a detector that
	// searches the sends list passes over in silence.
	SECTION("a folder parent sending down to its own child")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("drum bus", 1),
			track("kick", -1),
		};
		graph.explicit_sends = {send("drum bus", "kick")};

		// There is one send in the project and it is not a loop on its own. The
		// second half of the loop is kick summing into the folder above it, which
		// appears nowhere in send enumeration.
		REQUIRE(graph.explicit_sends.size() == 1);

		const cycle_detection_result result = detect_signal_cycle(graph);

		CHECK(result.cycle_detected);
		CHECK(result.closes_through_folder_routing());
		CHECK(edges_form_a_closed_loop(result.cycle_edges));
		CHECK(result.cycle_edges.size() == 2);
		CHECK(result.tracks_on_cycle().size() == 2);
		CHECK(result.describe().find("folder parent send") != std::string::npos);
	}

	SECTION("the same project is acyclic once the child stops summing into the folder")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("drum bus", 1),
			track("kick", -1, false),
		};
		graph.explicit_sends = {send("drum bus", "kick")};

		const cycle_detection_result result = detect_signal_cycle(graph);

		CHECK_FALSE(result.cycle_detected);
		CHECK(result.cycle_edges.empty());
		CHECK(result.describe().empty());
	}

	SECTION("a loop closing through two nested folders")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("mix", 1),
			track("drums", 1),
			track("kick", -2),
		};
		graph.explicit_sends = {send("mix", "kick")};

		const cycle_detection_result result = detect_signal_cycle(graph);

		CHECK(result.cycle_detected);
		CHECK(result.closes_through_folder_routing());
		CHECK(edges_form_a_closed_loop(result.cycle_edges));
		CHECK(result.cycle_edges.size() == 3);
	}

	SECTION("the same nesting is acyclic when the send runs up instead of down")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("mix", 1),
			track("drums", 1),
			track("kick", -2),
		};
		graph.explicit_sends = {send("kick", "mix")};

		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}
}

TEST_CASE("a loop through explicit sends alone is detected", "[daw][routing]")
{
	routing_graph graph;
	graph.tracks_in_project_order = {track("verb"), track("delay"), track("chorus")};
	graph.explicit_sends = {
		send("verb", "delay"),
		send("delay", "chorus"),
		send("chorus", "verb"),
	};

	const cycle_detection_result result = detect_signal_cycle(graph);

	CHECK(result.cycle_detected);
	CHECK_FALSE(result.closes_through_folder_routing());
	CHECK(edges_form_a_closed_loop(result.cycle_edges));
	CHECK(result.cycle_edges.size() == 3);
}

TEST_CASE("an acyclic graph is never refused", "[daw][routing]")
{
	SECTION("an empty project")
	{
		CHECK_FALSE(detect_signal_cycle(routing_graph{}).cycle_detected);
	}

	SECTION("a diamond, where one track is reached by two paths")
	{
		// The classic false refusal. A search keeping one visited set reaches
		// "master bus" twice and calls the second arrival a loop. It is a perfectly
		// ordinary parallel effect chain.
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("vocal"),
			track("plate"),
			track("tape delay"),
			track("master bus"),
		};
		graph.explicit_sends = {
			send("vocal", "plate"),
			send("vocal", "tape delay"),
			send("plate", "master bus"),
			send("tape delay", "master bus"),
		};

		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}

	SECTION("an ordinary nested folder tree with every parent send on")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("mix", 1),
			track("drums", 1),
			track("kick"),
			track("snare"),
			track("overheads", -1),
			track("guitars", 1),
			track("rhythm"),
			track("lead", -1),
			track("vocals", -1),
		};

		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}

	SECTION("a child sending up to the folder parent it already sums into")
	{
		// Two parallel edges in the same direction, not a loop. Undirected
		// reachability cannot tell the difference and would refuse a send a producer
		// is entitled to make.
		routing_graph graph = folder_with_one_child();
		graph.explicit_sends = {send("guitars", "mix")};

		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}

	SECTION("a long chain")
	{
		routing_graph graph;

		for (int position = 0; position < 200; ++position)
		{
			graph.tracks_in_project_order.push_back(track("track " + std::to_string(position)));
		}

		for (int position = 0; position + 1 < 200; ++position)
		{
			graph.explicit_sends.push_back(
				send("track " + std::to_string(position), "track " + std::to_string(position + 1)));
		}

		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}
}

TEST_CASE("a projected change describes the routing it would produce", "[daw][routing]")
{
	// The verdict is what the Tool Executor acts on, but it is not the only thing the
	// projection owes a caller — a refusal explains the loop, and it can only do that
	// over an edge set that is actually the routing the change would produce.
	//
	// The summing case is here for a specific reason. Leaving a source's old folder
	// edge in place cannot change the verdict: any loop that needs the stale edge
	// implies a loop the correct projection has too, or one the project already had.
	// So a bus that forgets to re-parent its sources is invisible from the verdict
	// alone, and pinning the edge set is what catches it.
	SECTION("a new send is added and nothing else moves")
	{
		const std::vector<signal_edge> edges =
			project_send_creation(folder_with_one_child(), "mix", "guitars");

		CHECK(edges.size() == 2);
		CHECK(contains_edge(edges, "guitars", "mix", signal_edge_kind::folder_parent_send));
		CHECK(contains_edge(edges, "mix", "guitars", signal_edge_kind::explicit_send));
	}

	SECTION("a summing bus re-parents its sources rather than adding to their routing")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};
		plan.bus_folder_parent = "mix";

		const std::vector<signal_edge> edges =
			project_bus_construction(folder_with_one_child(), plan);

		// The source stops summing into its old folder parent.
		CHECK_FALSE(contains_edge(edges, "guitars", "mix", signal_edge_kind::folder_parent_send));
		CHECK(count_edges_from(edges, "guitars") == 1);

		// It sums into the bus instead, and the bus carries the submix on to where
		// the source used to arrive.
		CHECK(contains_edge(edges, "guitars", "guitar bus", signal_edge_kind::folder_parent_send));
		CHECK(contains_edge(edges, "guitar bus", "mix", signal_edge_kind::folder_parent_send));
		CHECK(edges.size() == 2);
	}

	SECTION("a summing bus at top level leaves the submix reaching the master")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};

		const std::vector<signal_edge> edges =
			project_bus_construction(folder_with_one_child(), plan);

		CHECK(contains_edge(edges, "guitars", "guitar bus", signal_edge_kind::folder_parent_send));
		CHECK(count_edges_from(edges, "guitar bus") == 0);
		CHECK(edges.size() == 1);
	}

	SECTION("an aux bus adds sends and re-parents nothing")
	{
		bus_construction_plan plan;
		plan.type = bus_type::aux;
		plan.bus_track = "plate";
		plan.source_tracks = {"guitars"};

		const std::vector<signal_edge> edges =
			project_bus_construction(folder_with_one_child(), plan);

		// The source keeps going where it already went.
		CHECK(contains_edge(edges, "guitars", "mix", signal_edge_kind::folder_parent_send));
		CHECK(contains_edge(edges, "guitars", "plate", signal_edge_kind::explicit_send));
		CHECK(count_edges_from(edges, "guitars") == 2);
		CHECK(edges.size() == 2);
	}

	SECTION("a summing bus whose sources do not sum into it adds no source edges")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};
		plan.bus_folder_parent = "mix";
		plan.source_parent_send_enabled = false;

		const std::vector<signal_edge> edges =
			project_bus_construction(folder_with_one_child(), plan);

		CHECK(count_edges_from(edges, "guitars") == 0);
		CHECK(contains_edge(edges, "guitar bus", "mix", signal_edge_kind::folder_parent_send));
		CHECK(edges.size() == 1);
	}

	SECTION("a bus whose own parent send is off carries its submix nowhere")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};
		plan.bus_folder_parent = "mix";
		plan.bus_parent_send_enabled = false;

		const std::vector<signal_edge> edges =
			project_bus_construction(folder_with_one_child(), plan);

		CHECK(count_edges_from(edges, "guitar bus") == 0);
		CHECK(contains_edge(edges, "guitars", "guitar bus", signal_edge_kind::folder_parent_send));
		CHECK(edges.size() == 1);
	}
}

TEST_CASE("send creation is refused when it would close a loop", "[daw][routing]")
{
	SECTION("a folder parent sending down to its own child")
	{
		const routing_graph graph = folder_with_one_child();

		REQUIRE_FALSE(detect_signal_cycle(graph).cycle_detected);

		const routing_change_decision decision = evaluate_send_creation(graph, "mix", "guitars");

		CHECK_FALSE(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.cycle.closes_through_folder_routing());
		CHECK(edges_form_a_closed_loop(decision.cycle.cycle_edges));
		CHECK(decision.refusal_reason == "signal_cycle");
		CHECK(decision.acknowledgement_field == "confirmedSignalCycle");
	}

	SECTION("a track sending to itself")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {track("kick")};

		const routing_change_decision decision = evaluate_send_creation(graph, "kick", "kick");

		CHECK_FALSE(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.cycle.cycle_edges.size() == 1);
		CHECK(edges_form_a_closed_loop(decision.cycle.cycle_edges));
	}

	SECTION("a send up to the folder parent the source already sums into is permitted")
	{
		const routing_change_decision decision =
			evaluate_send_creation(folder_with_one_child(), "guitars", "mix");

		CHECK(decision.permitted);
		CHECK_FALSE(decision.cycle.cycle_detected);
		CHECK(decision.refusal_reason.empty());
		CHECK(decision.acknowledgement_field.empty());
	}

	SECTION("a send between unrelated tracks is permitted")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {track("vocal"), track("plate")};

		CHECK(evaluate_send_creation(graph, "vocal", "plate").permitted);
	}

	SECTION("a project that is already looping refuses the next send as well")
	{
		// Requirement 9.7 refuses when a cycle exists, not only when this call is the
		// one that closed it. A producer can build a feedback path by hand, and the
		// agent adding routing on top of it should say so rather than quietly
		// extending a project that is already oscillating. The acknowledgement flag is
		// how the producer says they meant it.
		routing_graph graph;
		graph.tracks_in_project_order = {track("verb"), track("delay"), track("vocal")};
		graph.explicit_sends = {send("verb", "delay"), send("delay", "verb")};

		REQUIRE(detect_signal_cycle(graph).cycle_detected);

		const routing_change_decision decision = evaluate_send_creation(graph, "vocal", "verb");

		CHECK_FALSE(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(evaluate_send_creation(graph, "vocal", "verb", true).permitted);
	}

	SECTION("an acknowledged loop proceeds, and is still reported")
	{
		const routing_change_decision decision =
			evaluate_send_creation(folder_with_one_child(), "mix", "guitars", true);

		CHECK(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.refusal_reason.empty());
		CHECK(decision.acknowledgement_field.empty());
	}

	SECTION("evaluating a send does not change the project")
	{
		const routing_graph graph = folder_with_one_child();

		CHECK_FALSE(evaluate_send_creation(graph, "mix", "guitars").permitted);

		CHECK(graph.explicit_sends.empty());
		CHECK_FALSE(detect_signal_cycle(graph).cycle_detected);
	}

	SECTION("the same graph reports the same loop every time")
	{
		const routing_graph graph = folder_with_one_child();

		const cycle_detection_result first = evaluate_send_creation(graph, "mix", "guitars").cycle;
		const cycle_detection_result second = evaluate_send_creation(graph, "mix", "guitars").cycle;

		CHECK(first.describe() == second.describe());
	}
}

TEST_CASE("bus construction is refused when it would close a loop", "[daw][routing]")
{
	// A project where the folder parent sends down to a child that does not sum back
	// into it. Acyclic as it stands, and the thing a summing bus can break.
	//
	//   mix       opens a folder, top level
	//     guitars   parent send off, so nothing flows back up
	//     keys      closes the folder, parent send on
	//
	//   mix -> guitars, as an explicit send
	const auto project_with_a_downward_send = []() {
		routing_graph graph;
		graph.tracks_in_project_order = {
			track("mix", 1),
			track("guitars", 0, false),
			track("keys", -1),
		};
		graph.explicit_sends = {send("mix", "guitars")};

		return graph;
	};

	SECTION("the starting project is acyclic")
	{
		REQUIRE_FALSE(detect_signal_cycle(project_with_a_downward_send()).cycle_detected);
	}

	SECTION("a summing bus placed where its own source is fed from closes a loop")
	{
		// Folding guitars into a bus that sits inside mix puts the submix back into
		// mix, and mix was already sending down to guitars. No send is created by
		// this call: the loop closes entirely through folder summing.
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};
		plan.bus_folder_parent = "mix";

		const routing_change_decision decision =
			evaluate_bus_construction(project_with_a_downward_send(), plan);

		CHECK_FALSE(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.cycle.closes_through_folder_routing());
		CHECK(edges_form_a_closed_loop(decision.cycle.cycle_edges));
		CHECK(decision.refusal_reason == "signal_cycle");
		CHECK(decision.acknowledgement_field == "confirmedSignalCycle");
	}

	SECTION("the same bus at top level is permitted")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};

		CHECK(evaluate_bus_construction(project_with_a_downward_send(), plan).permitted);
	}

	SECTION("the same bus is permitted when its sources do not sum into it")
	{
		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "guitar bus";
		plan.source_tracks = {"guitars"};
		plan.bus_folder_parent = "mix";
		plan.source_parent_send_enabled = false;

		CHECK(evaluate_bus_construction(project_with_a_downward_send(), plan).permitted);
	}

	SECTION("a summing bus over ordinary top-level tracks is permitted")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {track("kick"), track("snare"), track("overheads")};

		bus_construction_plan plan;
		plan.type = bus_type::summing;
		plan.bus_track = "drum bus";
		plan.source_tracks = {"kick", "snare", "overheads"};

		const routing_change_decision decision = evaluate_bus_construction(graph, plan);

		CHECK(decision.permitted);
		CHECK_FALSE(decision.cycle.cycle_detected);
	}

	SECTION("an aux bus placed inside the folder that feeds it closes a loop")
	{
		bus_construction_plan plan;
		plan.type = bus_type::aux;
		plan.bus_track = "plate";
		plan.source_tracks = {"mix"};
		plan.bus_folder_parent = "mix";

		const routing_change_decision decision =
			evaluate_bus_construction(folder_with_one_child(), plan);

		CHECK_FALSE(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.cycle.closes_through_folder_routing());
		CHECK(edges_form_a_closed_loop(decision.cycle.cycle_edges));
	}

	SECTION("the same aux bus at top level is permitted")
	{
		bus_construction_plan plan;
		plan.type = bus_type::aux;
		plan.bus_track = "plate";
		plan.source_tracks = {"mix"};

		CHECK(evaluate_bus_construction(folder_with_one_child(), plan).permitted);
	}

	SECTION("an aux bus fed by several sources is permitted")
	{
		routing_graph graph;
		graph.tracks_in_project_order = {track("vocal"), track("snare"), track("guitars")};

		bus_construction_plan plan;
		plan.type = bus_type::aux;
		plan.bus_track = "plate";
		plan.source_tracks = {"vocal", "snare", "guitars"};

		CHECK(evaluate_bus_construction(graph, plan).permitted);
	}

	SECTION("an acknowledged loop proceeds, and is still reported")
	{
		bus_construction_plan plan;
		plan.type = bus_type::aux;
		plan.bus_track = "plate";
		plan.source_tracks = {"mix"};
		plan.bus_folder_parent = "mix";

		const routing_change_decision decision =
			evaluate_bus_construction(folder_with_one_child(), plan, true);

		CHECK(decision.permitted);
		CHECK(decision.cycle.cycle_detected);
		CHECK(decision.refusal_reason.empty());
	}
}
