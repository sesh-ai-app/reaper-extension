// Signal routing cycle detection — the formal properties, over generated graphs.
//
// `cycle_detector_test.cpp` is the example file: hand-written synthetic projects,
// each one chosen because it is a way to get the detector wrong. This file is the
// other half. Properties 26 and 27 are stated over *any* routing graph, so they are
// checked over a corpus of graphs nobody chose.
//
// **Validates: Requirements 9.7**
//
// ---------------------------------------------------------------------------
// The oracle, and why there has to be one
//
// Property 26 is an "if and only if". The hard half is not "a cycle is reported when
// one exists" — it is that the expected answer has to come from somewhere other than
// the implementation. A test that computes what it expects by calling
// `detect_signal_cycle` and then asserts `detect_signal_cycle` agrees with it proves
// only that the function is deterministic.
//
// So `a_cycle_exists` below is a deliberately naive reference: it builds a
// reachability matrix from the edge set and relaxes it until it stops changing, then
// reports a cycle exactly when some node reaches itself. It is quartic in the worst
// case and would be a poor choice in production. It shares nothing with the detector
// under test — no depth-first search, no on-path colouring, no witness — so the two
// agreeing is evidence rather than tautology.
//
// The oracle also derives the graph's edges itself, including folder parentage
// (`naive_folder_parent_indices`, which scans backwards for the most recent opener a
// level up rather than keeping a stack of open folders). Reusing
// `collect_signal_edges` to feed the oracle would leave the folder half of
// requirement 9.7 — the half the requirement was written for — checked against
// itself.
//
// ---------------------------------------------------------------------------
// Where the corpus comes from
//
// Two corpora, following the convention the rest of this suite settled on.
//
// Exhaustive where the space is small enough. Every routing graph over one to three
// tracks: every well-formed folder structure, crossed with every combination of
// parent send flags, crossed with every subset of the nine possible explicit sends
// including self-sends. That is 20,612 graphs, and the count is asserted — see
// `tests/daw/folder_invariant_keeper_test.cpp`, where the same guard exists for the
// same reason: a sweep that silently stopped enumerating would make every property
// below vacuously true.
//
// Deterministically seeded beyond that, because four tracks alone is 14.6 million
// graphs. `DeterministicBytes` is the xorshift source from
// `tests/daw/alias_store_test.cpp`; a failure here is reproducible from the seed in
// the test on any machine.
//
// Catch2 in this build has no generator library, and adding one would mean touching
// CMakeLists.txt for a dependency that only the tests want. Enumeration and a seeded
// source get the same evidence without it.
//
// ---------------------------------------------------------------------------
// Folder-closed cycles are the coverage claim that gets checked
//
// A generator that never produces a cycle closing through folder summing would let
// Property 26 pass against a detector that only ever searched the sends list — which
// is precisely the bug requirement 9.7 exists to rule out. So the corpus guard does
// not just assert that cycles appear; it counts the graphs where a cycle exists and
// the explicit sends *alone* are acyclic, and requires that there are some, including
// ones carrying a single send. That is the shape a sends-list-only detector passes
// over in silence.
//
// ---------------------------------------------------------------------------
// Scope note
//
// The generated structures are all well-formed: deltas summing to zero, no negative
// accumulated depth, no delta above one. The detector tolerates malformed deltas
// deliberately (a graph can arrive mid-edit) and the example file covers that
// tolerance case by case. It is left out here because the independent parentage
// oracle is only obviously correct for well-formed input, and an oracle that needs
// its own repair rules is no longer obviously correct.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

	// The track a bus plan creates. Distinct from every generated track identity, so
	// a plan never collides with a track already in the graph.
	const track_identity bus_track_identity{"new bus"};

	// -----------------------------------------------------------------------
	// The oracle
	// -----------------------------------------------------------------------

	// Does this edge set contain a directed cycle?
	//
	// Reachability by repeated relaxation: seed the matrix with the edges, then keep
	// extending — if `from` reaches `v` and `v -> w` is an edge, `from` reaches `w` —
	// until a pass changes nothing. A cycle exists exactly when some node reaches
	// itself, which is true of a self-send as much as of a long loop.
	//
	// Nothing here searches. There is no path, no stack, no arrival order, and no
	// witness: the answer falls out of a fixed point. That is the whole point of it
	// being the reference for an "if and only if".
	bool a_cycle_exists(const std::vector<signal_edge>& edges)
	{
		std::vector<track_identity> nodes;

		const auto node_index_for = [&nodes](const track_identity& identity) -> std::size_t {
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index] == identity)
				{
					return index;
				}
			}

			nodes.push_back(identity);

			return nodes.size() - 1;
		};

		std::vector<std::pair<std::size_t, std::size_t>> numbered_edges;
		numbered_edges.reserve(edges.size());

		for (const signal_edge& edge : edges)
		{
			const std::size_t source_node = node_index_for(edge.source_track);
			const std::size_t destination_node = node_index_for(edge.destination_track);

			numbered_edges.emplace_back(source_node, destination_node);
		}

		const std::size_t node_count = nodes.size();
		std::vector<char> reaches(node_count * node_count, 0);

		for (const std::pair<std::size_t, std::size_t>& edge : numbered_edges)
		{
			reaches[(edge.first * node_count) + edge.second] = 1;
		}

		bool something_changed = true;

		while (something_changed)
		{
			something_changed = false;

			for (std::size_t from_node = 0; from_node < node_count; ++from_node)
			{
				for (const std::pair<std::size_t, std::size_t>& edge : numbered_edges)
				{
					const std::size_t reaches_source = (from_node * node_count) + edge.first;
					const std::size_t reaches_destination = (from_node * node_count) + edge.second;

					if (reaches[reaches_source] != 0 && reaches[reaches_destination] == 0)
					{
						reaches[reaches_destination] = 1;
						something_changed = true;
					}
				}
			}
		}

		for (std::size_t node = 0; node < node_count; ++node)
		{
			if (reaches[(node * node_count) + node] != 0)
			{
				return true;
			}
		}

		return false;
	}

	// Folder parentage, derived the long way round.
	//
	// A track's nesting level is the sum of the deltas before it. A track at level
	// zero is top level. A track at level L sits inside the most recent earlier track
	// that is itself at level L - 1 and opens a folder — found by scanning backwards
	// one track at a time, with no stack of open folders to get wrong.
	//
	// Correct only for well-formed deltas, which is all this file generates.
	std::vector<std::optional<std::size_t>> naive_folder_parent_indices(
		const std::vector<track_node>& tracks_in_project_order)
	{
		std::vector<int> nesting_levels(tracks_in_project_order.size(), 0);
		int level = 0;

		for (std::size_t index = 0; index < tracks_in_project_order.size(); ++index)
		{
			nesting_levels[index] = level;
			level += tracks_in_project_order[index].folder_depth_delta;
		}

		std::vector<std::optional<std::size_t>> folder_parent_indices(tracks_in_project_order.size());

		for (std::size_t index = 0; index < tracks_in_project_order.size(); ++index)
		{
			if (nesting_levels[index] == 0)
			{
				continue;
			}

			for (std::size_t scanned = index; scanned > 0; --scanned)
			{
				const std::size_t earlier_index = scanned - 1;

				if (nesting_levels[earlier_index] == nesting_levels[index] - 1
					&& tracks_in_project_order[earlier_index].folder_depth_delta > 0)
				{
					folder_parent_indices[index] = earlier_index;
					break;
				}
			}
		}

		return folder_parent_indices;
	}

	// The graph's edges, derived from the oracle's own parentage. A child with its
	// parent send on feeds its folder parent; with no folder parent above it the
	// parent send reaches the master, which is a sink and not an edge.
	std::vector<signal_edge> naive_signal_edges(const routing_graph& graph)
	{
		const std::vector<std::optional<std::size_t>> folder_parent_indices =
			naive_folder_parent_indices(graph.tracks_in_project_order);

		std::vector<signal_edge> edges;

		for (std::size_t index = 0; index < graph.tracks_in_project_order.size(); ++index)
		{
			const track_node& track = graph.tracks_in_project_order[index];

			if (!track.parent_send_enabled || !folder_parent_indices[index].has_value())
			{
				continue;
			}

			edges.push_back(signal_edge{
				track.identity,
				graph.tracks_in_project_order[*folder_parent_indices[index]].identity,
				signal_edge_kind::folder_parent_send,
			});
		}

		for (const send_edge& send : graph.explicit_sends)
		{
			edges.push_back(signal_edge{
				send.source_track,
				send.destination_track,
				signal_edge_kind::explicit_send,
			});
		}

		return edges;
	}

	// The explicit sends on their own — the graph a detector that only enumerated
	// sends would search. Used to identify the folder-closed cases in the corpus, not
	// to check anything about the implementation.
	std::vector<signal_edge> sends_only_edges(const routing_graph& graph)
	{
		std::vector<signal_edge> edges;
		edges.reserve(graph.explicit_sends.size());

		for (const send_edge& send : graph.explicit_sends)
		{
			edges.push_back(signal_edge{
				send.source_track,
				send.destination_track,
				signal_edge_kind::explicit_send,
			});
		}

		return edges;
	}

	// The routing a bus plan would produce, restated from the semantics the header
	// documents: a summing bus re-parents its sources, so their old folder edges go
	// away and they sum into the bus instead; an aux bus adds sends and moves nothing;
	// the bus's own output reaches whatever folder it was placed in, or the master —
	// a sink — when it was placed at top level.
	//
	// This is a restatement rather than an independent derivation, and the example
	// file is what pins the projected edge set edge for edge. What it buys here is
	// that the *verdict* the properties below compare against comes from the oracle
	// walking this edge set, not from the detector walking its own.
	std::vector<signal_edge> naive_bus_projection(
		const routing_graph& graph,
		const bus_construction_plan& plan)
	{
		const auto is_a_source = [&plan](const track_identity& identity) {
			return std::find(plan.source_tracks.begin(), plan.source_tracks.end(), identity)
				!= plan.source_tracks.end();
		};

		std::vector<signal_edge> edges;

		for (const signal_edge& edge : naive_signal_edges(graph))
		{
			const bool the_bus_takes_this_source_over =
				plan.type == bus_type::summing
				&& edge.kind == signal_edge_kind::folder_parent_send
				&& is_a_source(edge.source_track);

			if (!the_bus_takes_this_source_over)
			{
				edges.push_back(edge);
			}
		}

		for (const track_identity& source_track : plan.source_tracks)
		{
			if (plan.type == bus_type::aux)
			{
				edges.push_back(signal_edge{
					source_track,
					plan.bus_track,
					signal_edge_kind::explicit_send,
				});
			}
			else if (plan.source_parent_send_enabled)
			{
				edges.push_back(signal_edge{
					source_track,
					plan.bus_track,
					signal_edge_kind::folder_parent_send,
				});
			}
		}

		if (plan.bus_parent_send_enabled && plan.bus_folder_parent.has_value())
		{
			edges.push_back(signal_edge{
				plan.bus_track,
				*plan.bus_folder_parent,
				signal_edge_kind::folder_parent_send,
			});
		}

		return edges;
	}

	// -----------------------------------------------------------------------
	// Describing a counterexample
	// -----------------------------------------------------------------------

	std::string describe_graph(const routing_graph& graph)
	{
		std::string description = "tracks:";

		for (const track_node& track : graph.tracks_in_project_order)
		{
			description += " " + track.identity
				+ "(delta " + std::to_string(track.folder_depth_delta)
				+ (track.parent_send_enabled ? ", parent send on)" : ", parent send off)");
		}

		description += " | sends:";

		if (graph.explicit_sends.empty())
		{
			description += " none";
		}

		for (const send_edge& send : graph.explicit_sends)
		{
			description += " " + send.source_track + "->" + send.destination_track;
		}

		return description;
	}

	std::string describe_plan(const bus_construction_plan& plan)
	{
		std::string description = (plan.type == bus_type::summing) ? "summing bus " : "aux bus ";
		description += plan.bus_track + " over";

		for (const track_identity& source_track : plan.source_tracks)
		{
			description += " " + source_track;
		}

		description += plan.bus_folder_parent.has_value()
			? (", placed in " + *plan.bus_folder_parent)
			: std::string{", placed at top level"};

		description += plan.bus_parent_send_enabled
			? ", bus parent send on"
			: ", bus parent send off";

		description += plan.source_parent_send_enabled
			? ", source parent send on"
			: ", source parent send off";

		return description;
	}

	// Edges as a sorted list of readable keys, so two edge sets can be compared as
	// multisets and a mismatch names the edges rather than reporting a size.
	std::vector<std::string> sorted_edge_keys(const std::vector<signal_edge>& edges)
	{
		std::vector<std::string> keys;
		keys.reserve(edges.size());

		for (const signal_edge& edge : edges)
		{
			keys.push_back(
				edge.source_track
				+ ((edge.kind == signal_edge_kind::folder_parent_send) ? " -folder-> " : " -send-> ")
				+ edge.destination_track);
		}

		std::sort(keys.begin(), keys.end());

		return keys;
	}

	bool any_edge_is_folder_summing(const std::vector<signal_edge>& edges)
	{
		return std::any_of(edges.begin(), edges.end(), [](const signal_edge& edge) {
			return edge.kind == signal_edge_kind::folder_parent_send;
		});
	}

	std::string join_edge_keys(const std::vector<std::string>& keys)
	{
		std::string joined;

		for (const std::string& key : keys)
		{
			if (!joined.empty())
			{
				joined += "; ";
			}

			joined += key;
		}

		return joined.empty() ? std::string{"none"} : joined;
	}

	// A reported loop has to be one: consecutive edges join up, the last returns to
	// where the first started, and every edge on it is an edge the graph actually has.
	// Without this a detector could satisfy the boolean half of property 26 while
	// reporting a witness that explains nothing.
	bool is_a_closed_loop_within(
		const std::vector<signal_edge>& cycle_edges,
		const std::vector<signal_edge>& available_edges)
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

		if (cycle_edges.back().destination_track != cycle_edges.front().source_track)
		{
			return false;
		}

		for (const signal_edge& reported : cycle_edges)
		{
			const bool graph_has_this_edge = std::any_of(
				available_edges.begin(),
				available_edges.end(),
				[&reported](const signal_edge& available) {
					return available.source_track == reported.source_track
						&& available.destination_track == reported.destination_track
						&& available.kind == reported.kind;
				});

			if (!graph_has_this_edge)
			{
				return false;
			}
		}

		return true;
	}

	// -----------------------------------------------------------------------
	// Generating graphs
	// -----------------------------------------------------------------------

	// Well formed in the sense requirement 11.1 means: the deltas sum to zero, no
	// track sits at a negative accumulated depth, and no track opens more than one
	// folder because a track cannot be two folder parents.
	bool folder_deltas_are_well_formed(const std::vector<track_node>& tracks_in_project_order)
	{
		int level = 0;

		for (const track_node& track : tracks_in_project_order)
		{
			if (track.folder_depth_delta > 1)
			{
				return false;
			}

			level += track.folder_depth_delta;

			if (level < 0)
			{
				return false;
			}
		}

		return level == 0;
	}

	void extend_well_formed_folder_deltas(
		std::size_t length,
		int open_folder_count,
		std::vector<int>& prefix,
		std::vector<std::vector<int>>& delta_lists)
	{
		if (prefix.size() == length)
		{
			if (open_folder_count == 0)
			{
				delta_lists.push_back(prefix);
			}

			return;
		}

		// At most one folder opened, and never more closed than are open — closing
		// more is exactly the negative accumulated depth well-formedness rules out.
		for (int folder_depth_delta = -open_folder_count; folder_depth_delta <= 1; ++folder_depth_delta)
		{
			prefix.push_back(folder_depth_delta);
			extend_well_formed_folder_deltas(
				length,
				open_folder_count + folder_depth_delta,
				prefix,
				delta_lists);
			prefix.pop_back();
		}
	}

	// Every well-formed folder structure of a given length.
	//
	// The deltas themselves are enumerated rather than assembled from an
	// open/plain/close shape alphabet, which matters by length three: one track
	// closing two folders at once — (1, 1, -2), the doubly nested case — is not
	// reachable from a one-close-at-a-time alphabet except by a fixup on the final
	// track.
	//
	// A well-formed structure over tracks in project order is an ordered forest, so
	// the count is the Catalan number: 1, 2, 5, 14 for lengths one to four. Asserted
	// in the corpus guard below.
	std::vector<std::vector<int>> enumerate_well_formed_folder_deltas(std::size_t length)
	{
		std::vector<std::vector<int>> delta_lists;
		std::vector<int> prefix;
		prefix.reserve(length);

		extend_well_formed_folder_deltas(length, 0, prefix, delta_lists);

		return delta_lists;
	}

	track_identity generated_track_identity(std::size_t index)
	{
		return "t" + std::to_string(index);
	}

	// Every routing graph over one to `maximum_tracks` tracks: every well-formed
	// folder structure, crossed with every combination of parent send flags, crossed
	// with every subset of the possible explicit sends. Self-sends are among the
	// possible sends, since a send from a track to itself is a loop of length one.
	std::vector<routing_graph> enumerate_routing_graphs(std::size_t maximum_tracks)
	{
		std::vector<routing_graph> graphs;

		for (std::size_t track_count = 1; track_count <= maximum_tracks; ++track_count)
		{
			const std::size_t possible_send_count = track_count * track_count;

			for (const std::vector<int>& deltas : enumerate_well_formed_folder_deltas(track_count))
			{
				const std::size_t parent_send_combinations = std::size_t{1} << track_count;
				const std::size_t send_subsets = std::size_t{1} << possible_send_count;

				for (std::size_t parent_send_mask = 0;
					parent_send_mask < parent_send_combinations;
					++parent_send_mask)
				{
					std::vector<track_node> tracks;
					tracks.reserve(track_count);

					for (std::size_t index = 0; index < track_count; ++index)
					{
						track_node track;
						track.identity = generated_track_identity(index);
						track.folder_depth_delta = deltas[index];
						track.parent_send_enabled = (parent_send_mask & (std::size_t{1} << index)) != 0;
						tracks.push_back(std::move(track));
					}

					for (std::size_t send_mask = 0; send_mask < send_subsets; ++send_mask)
					{
						routing_graph graph;
						graph.tracks_in_project_order = tracks;

						for (std::size_t send_slot = 0; send_slot < possible_send_count; ++send_slot)
						{
							if ((send_mask & (std::size_t{1} << send_slot)) == 0)
							{
								continue;
							}

							graph.explicit_sends.push_back(send_edge{
								generated_track_identity(send_slot / track_count),
								generated_track_identity(send_slot % track_count),
							});
						}

						graphs.push_back(std::move(graph));
					}
				}
			}
		}

		return graphs;
	}

	// Deterministic byte source, as in `tests/daw/alias_store_test.cpp`. The corpus
	// beyond three tracks is sampled rather than enumerated, and a sampled failure is
	// only worth having if anybody can reproduce it from the seed.
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

		bool chance_in(std::size_t bound) { return below(bound) == 0; }

	private:
		std::uint32_t state_;
	};

	// One graph of `minimum_tracks` to `maximum_tracks` tracks.
	//
	// The folder structure is built by choosing a legal delta at each position — open
	// one, stay flat, or close some of what is open — with whatever is still open
	// closed on the last track, the way REAPER's own project files end. Parent sends
	// are on most of the time, matching REAPER's default for a new track and keeping
	// folder summing edges plentiful, since those are the edges the folder-closed
	// cycles need.
	routing_graph generate_routing_graph(
		DeterministicBytes& bytes,
		std::size_t minimum_tracks,
		std::size_t maximum_tracks)
	{
		const std::size_t track_count =
			minimum_tracks + bytes.below((maximum_tracks - minimum_tracks) + 1);

		routing_graph graph;
		graph.tracks_in_project_order.reserve(track_count);

		int open_folder_count = 0;

		for (std::size_t index = 0; index < track_count; ++index)
		{
			int folder_depth_delta = 0;
			const std::size_t shape = bytes.below(3);

			if (shape == 0)
			{
				folder_depth_delta = 1;
			}
			else if (shape == 2 && open_folder_count > 0)
			{
				folder_depth_delta =
					-1 - static_cast<int>(bytes.below(static_cast<std::size_t>(open_folder_count)));
			}

			open_folder_count += folder_depth_delta;

			track_node track;
			track.identity = generated_track_identity(index);
			track.folder_depth_delta = folder_depth_delta;
			track.parent_send_enabled = !bytes.chance_in(4);
			graph.tracks_in_project_order.push_back(std::move(track));
		}

		if (open_folder_count > 0)
		{
			// Closing on the final track leaves every earlier track's level untouched,
			// so the structure stays well formed.
			graph.tracks_in_project_order.back().folder_depth_delta -= open_folder_count;
		}

		const std::size_t send_count = bytes.below(track_count + 2);

		for (std::size_t index = 0; index < send_count; ++index)
		{
			graph.explicit_sends.push_back(send_edge{
				generated_track_identity(bytes.below(track_count)),
				generated_track_identity(bytes.below(track_count)),
			});
		}

		return graph;
	}

	bus_construction_plan generate_bus_plan(DeterministicBytes& bytes, const routing_graph& graph)
	{
		const std::size_t track_count = graph.tracks_in_project_order.size();

		bus_construction_plan plan;
		plan.type = bytes.chance_in(2) ? bus_type::summing : bus_type::aux;
		plan.bus_track = bus_track_identity;
		plan.bus_parent_send_enabled = !bytes.chance_in(4);
		plan.source_parent_send_enabled = !bytes.chance_in(4);

		const std::size_t folder_parent_choice = bytes.below(track_count + 1);

		if (folder_parent_choice > 0)
		{
			plan.bus_folder_parent =
				graph.tracks_in_project_order[folder_parent_choice - 1].identity;
		}

		const std::size_t source_count = 1 + bytes.below(track_count);

		for (std::size_t index = 0; index < source_count; ++index)
		{
			const track_identity candidate =
				graph.tracks_in_project_order[bytes.below(track_count)].identity;

			if (std::find(plan.source_tracks.begin(), plan.source_tracks.end(), candidate)
				== plan.source_tracks.end())
			{
				plan.source_tracks.push_back(candidate);
			}
		}

		return plan;
	}

	// Every bus plan over a graph: both types, every non-empty set of sources, every
	// placement including top level, and both parent send flags each way. Only used
	// for the graphs small enough that the product stays finite in the useful sense.
	std::vector<bus_construction_plan> enumerate_bus_plans(const routing_graph& graph)
	{
		const std::size_t track_count = graph.tracks_in_project_order.size();
		const std::size_t source_subsets = std::size_t{1} << track_count;

		std::vector<bus_construction_plan> plans;

		for (const bus_type type : {bus_type::summing, bus_type::aux})
		{
			for (std::size_t source_mask = 1; source_mask < source_subsets; ++source_mask)
			{
				for (std::size_t folder_parent_choice = 0;
					folder_parent_choice <= track_count;
					++folder_parent_choice)
				{
					for (const bool bus_parent_send_enabled : {true, false})
					{
						for (const bool source_parent_send_enabled : {true, false})
						{
							bus_construction_plan plan;
							plan.type = type;
							plan.bus_track = bus_track_identity;
							plan.bus_parent_send_enabled = bus_parent_send_enabled;
							plan.source_parent_send_enabled = source_parent_send_enabled;

							if (folder_parent_choice > 0)
							{
								plan.bus_folder_parent =
									graph.tracks_in_project_order[folder_parent_choice - 1].identity;
							}

							for (std::size_t index = 0; index < track_count; ++index)
							{
								if ((source_mask & (std::size_t{1} << index)) != 0)
								{
									plan.source_tracks.push_back(
										graph.tracks_in_project_order[index].identity);
								}
							}

							plans.push_back(std::move(plan));
						}
					}
				}
			}
		}

		return plans;
	}

	// -----------------------------------------------------------------------
	// The corpora, built once
	// -----------------------------------------------------------------------

	constexpr std::size_t exhaustive_maximum_tracks = 3;
	constexpr std::size_t sampled_graph_count = 4000;
	constexpr std::size_t sampled_minimum_tracks = 2;
	constexpr std::size_t sampled_maximum_tracks = 8;
	constexpr std::uint32_t sampled_graph_seed = 0xc17c1e5u;

	const std::vector<routing_graph>& exhaustive_graphs()
	{
		static const std::vector<routing_graph> graphs =
			enumerate_routing_graphs(exhaustive_maximum_tracks);

		return graphs;
	}

	const std::vector<routing_graph>& sampled_graphs()
	{
		static const std::vector<routing_graph> graphs = [] {
			DeterministicBytes bytes{sampled_graph_seed};

			std::vector<routing_graph> generated;
			generated.reserve(sampled_graph_count);

			for (std::size_t index = 0; index < sampled_graph_count; ++index)
			{
				generated.push_back(
					generate_routing_graph(bytes, sampled_minimum_tracks, sampled_maximum_tracks));
			}

			return generated;
		}();

		return graphs;
	}
}

// ---------------------------------------------------------------------------
// The corpus guard
//
// Everything below is a claim about "any routing graph", which is worth exactly as
// much as the corpus behind it. A sweep that silently stopped enumerating, or one
// that never produced a cycle closing through folder summing, would let the
// properties pass while checking nothing that matters.
// ---------------------------------------------------------------------------

TEST_CASE("the generated routing graphs cover what the properties claim", "[daw][routing][property]")
{
	SECTION("the folder structures are every ordered forest, which is the Catalan count")
	{
		// 1, 2, 5, 14 — the number of ways tracks in project order can nest. A
		// generator producing fewer would leave folder shapes unchecked, and one
		// producing more would be producing structures REAPER cannot represent.
		CHECK(enumerate_well_formed_folder_deltas(1).size() == 1);
		CHECK(enumerate_well_formed_folder_deltas(2).size() == 2);
		CHECK(enumerate_well_formed_folder_deltas(3).size() == 5);
		CHECK(enumerate_well_formed_folder_deltas(4).size() == 14);

		// Including the doubly nested one, which is where the multi-level closing
		// delta that makes this arithmetic awkward actually appears.
		const std::vector<std::vector<int>> deltas_of_three = enumerate_well_formed_folder_deltas(3);

		CHECK(std::find(deltas_of_three.begin(), deltas_of_three.end(), std::vector<int>{1, 1, -2})
			!= deltas_of_three.end());
	}

	SECTION("the exhaustive corpus is the whole space it claims to be")
	{
		const std::vector<routing_graph>& graphs = exhaustive_graphs();

		// Catalan(n) folder structures x 2^n parent send combinations x 2^(n*n) send
		// subsets, summed over one to three tracks:
		//   1 x 2 x 2     = 4
		//   2 x 4 x 16    = 128
		//   5 x 8 x 512   = 20480
		REQUIRE(graphs.size() == 20612);

		for (const routing_graph& graph : graphs)
		{
			REQUIRE(folder_deltas_are_well_formed(graph.tracks_in_project_order));
		}
	}

	SECTION("the exhaustive corpus contains cyclic and acyclic graphs")
	{
		std::size_t cyclic_count = 0;
		std::size_t acyclic_count = 0;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			if (a_cycle_exists(naive_signal_edges(graph)))
			{
				++cyclic_count;
			}
			else
			{
				++acyclic_count;
			}
		}

		// Both directions of property 26 have something to bite on, and property 27
		// has acyclic graphs to be permissive about.
		CHECK(cyclic_count > 0);
		CHECK(acyclic_count > 0);
	}

	SECTION("the exhaustive corpus contains cycles that close only through folder routing")
	{
		// The coverage claim that is actually load-bearing. A cycle that exists in the
		// full graph while the explicit sends alone are acyclic is the case requirement
		// 9.7 was written for, and the case a detector searching only the sends list
		// passes over in silence. Counted rather than assumed: without these graphs,
		// property 26 would hold against exactly the detector the requirement rules
		// out.
		std::size_t folder_closed_count = 0;
		std::size_t folder_closed_with_a_single_send_count = 0;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			if (!a_cycle_exists(naive_signal_edges(graph)) || a_cycle_exists(sends_only_edges(graph)))
			{
				continue;
			}

			++folder_closed_count;

			if (graph.explicit_sends.size() == 1)
			{
				++folder_closed_with_a_single_send_count;
			}
		}

		CHECK(folder_closed_count > 0);

		// The narrowest version: one send in the whole project, and it is not a loop
		// by itself. The second half of the loop is folder summing, which appears
		// nowhere in send enumeration.
		CHECK(folder_closed_with_a_single_send_count > 0);
	}

	SECTION("the sampled corpus is well formed and spans its track range")
	{
		const std::vector<routing_graph>& graphs = sampled_graphs();

		REQUIRE(graphs.size() == sampled_graph_count);

		std::size_t smallest_track_count = sampled_maximum_tracks;
		std::size_t largest_track_count = 0;
		int deepest_nesting = 0;

		for (const routing_graph& graph : graphs)
		{
			REQUIRE(folder_deltas_are_well_formed(graph.tracks_in_project_order));

			const std::size_t track_count = graph.tracks_in_project_order.size();
			smallest_track_count = std::min(smallest_track_count, track_count);
			largest_track_count = std::max(largest_track_count, track_count);

			int level = 0;

			for (const track_node& track : graph.tracks_in_project_order)
			{
				deepest_nesting = std::max(deepest_nesting, level);
				level += track.folder_depth_delta;
			}
		}

		CHECK(smallest_track_count == sampled_minimum_tracks);
		CHECK(largest_track_count == sampled_maximum_tracks);

		// Graphs bigger than the exhaustive sweep reaches, nested deeper than it can.
		CHECK(deepest_nesting >= 3);
	}

	SECTION("the sampled corpus contains cyclic, acyclic, and folder-closed graphs")
	{
		std::size_t cyclic_count = 0;
		std::size_t acyclic_count = 0;
		std::size_t folder_closed_count = 0;

		for (const routing_graph& graph : sampled_graphs())
		{
			if (!a_cycle_exists(naive_signal_edges(graph)))
			{
				++acyclic_count;
				continue;
			}

			++cyclic_count;

			if (!a_cycle_exists(sends_only_edges(graph)))
			{
				++folder_closed_count;
			}
		}

		CHECK(cyclic_count > 0);
		CHECK(acyclic_count > 0);
		CHECK(folder_closed_count > 0);
	}
}

// ---------------------------------------------------------------------------
// Property 26: Routing — cycle detection is exact
//
// For any synthetic routing graph, a cycle is detected if and only if one exists,
// including cycles that close only through folder routing rather than through an
// explicit send.
//
// **Validates: Requirements 9.7**
// ---------------------------------------------------------------------------

TEST_CASE("Property 26: a cycle is detected if and only if one exists", "[daw][routing][property]")
{
	// The verdict compared against comes from `a_cycle_exists` over the oracle's own
	// edge derivation. Nothing in the expected answer is computed by the code under
	// test, which is the only way an "if and only if" says anything.
	const auto sweep = [](const std::vector<routing_graph>& graphs) {
		std::string first_counterexample;
		std::size_t detected_count = 0;

		for (const routing_graph& graph : graphs)
		{
			const std::vector<signal_edge> reference_edges = naive_signal_edges(graph);
			const bool a_cycle_is_there = a_cycle_exists(reference_edges);

			const cycle_detection_result result = detect_signal_cycle(graph);

			if (result.cycle_detected != a_cycle_is_there)
			{
				if (first_counterexample.empty())
				{
					first_counterexample = describe_graph(graph)
						+ " | detector said " + (result.cycle_detected ? "cycle" : "no cycle")
						+ ", a cycle " + (a_cycle_is_there ? "exists" : "does not exist")
						+ " over edges: " + join_edge_keys(sorted_edge_keys(reference_edges));
				}

				continue;
			}

			if (!result.cycle_detected)
			{
				// Nothing found means nothing reported. A detector that said "no cycle"
				// while handing back edges would pass the boolean half of this property
				// and mean something different by it.
				if (!result.cycle_edges.empty() && first_counterexample.empty())
				{
					first_counterexample = describe_graph(graph)
						+ " | no cycle detected but a witness was reported: "
						+ join_edge_keys(sorted_edge_keys(result.cycle_edges));
				}

				continue;
			}

			++detected_count;

			// The witness has to be a real loop over real edges, or "detected" is a
			// boolean that happens to be right rather than a finding the refusal can
			// explain.
			if (!is_a_closed_loop_within(result.cycle_edges, reference_edges)
				&& first_counterexample.empty())
			{
				first_counterexample = describe_graph(graph)
					+ " | reported witness is not a closed loop over the graph's edges: "
					+ join_edge_keys(sorted_edge_keys(result.cycle_edges))
					+ " | graph edges: " + join_edge_keys(sorted_edge_keys(reference_edges));
			}

			// When the explicit sends are acyclic on their own, every loop in the graph
			// has to pass through folder summing — so the witness must say so. This is
			// the half of requirement 9.7 that a sends-list-only detector cannot even
			// express.
			if (!a_cycle_exists(sends_only_edges(graph))
				&& !result.closes_through_folder_routing()
				&& first_counterexample.empty())
			{
				first_counterexample = describe_graph(graph)
					+ " | the sends alone are acyclic, so the loop must close through folder"
					" routing, but the witness does not: " + result.describe();
			}
		}

		return std::pair<std::string, std::size_t>{first_counterexample, detected_count};
	};

	SECTION("over every routing graph of up to three tracks")
	{
		const std::pair<std::string, std::size_t> outcome = sweep(exhaustive_graphs());

		REQUIRE(outcome.first == std::string{});

		// Not a vacuous pass: the sweep actually found loops.
		CHECK(outcome.second > 0);
	}

	SECTION("over sampled graphs of up to eight tracks")
	{
		// Reproducible from the seed: 0xc17c1e5.
		const std::pair<std::string, std::size_t> outcome = sweep(sampled_graphs());

		REQUIRE(outcome.first == std::string{});
		CHECK(outcome.second > 0);
	}

	SECTION("the graph the detector searches is the graph the oracle derived")
	{
		// Both edge kinds, derived two independent ways — a backwards scan for the most
		// recent opener a level up against the detector's stack of open folders. If
		// these disagree the property above is comparing verdicts about two different
		// graphs, and the folder half of requirement 9.7 is unchecked.
		std::string first_counterexample;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			const std::vector<std::string> reference = sorted_edge_keys(naive_signal_edges(graph));
			const std::vector<std::string> produced = sorted_edge_keys(collect_signal_edges(graph));

			if (reference != produced)
			{
				first_counterexample = describe_graph(graph)
					+ " | oracle edges: " + join_edge_keys(reference)
					+ " | detector edges: " + join_edge_keys(produced);
				break;
			}
		}

		REQUIRE(first_counterexample == std::string{});
	}

	SECTION("folder parentage agrees with the oracle over sampled graphs too")
	{
		std::string first_counterexample;

		for (const routing_graph& graph : sampled_graphs())
		{
			if (naive_folder_parent_indices(graph.tracks_in_project_order)
				!= derive_folder_parent_indices(graph.tracks_in_project_order))
			{
				first_counterexample = describe_graph(graph);
				break;
			}
		}

		REQUIRE(first_counterexample == std::string{});
	}
}

// ---------------------------------------------------------------------------
// Property 27: Routing — acyclic graphs are never refused
//
// For any acyclic graph, send creation and bus construction are permitted — cycle
// detection raises no false refusal.
//
// **Validates: Requirements 9.7**
//
// Stated over the routing the change would produce, which is the only reading that
// is both true and useful: a send that closes a loop is refused correctly however
// acyclic the project was beforehand. So the claim checked is that a change whose
// result is acyclic is permitted, and — property 26 carried through to the decision —
// a change whose result is cyclic is refused, with the reason and acknowledgement
// field the refusal schema expects.
// ---------------------------------------------------------------------------

TEST_CASE("Property 27: send creation is permitted whenever the result is acyclic", "[daw][routing][property]")
{
	struct send_sweep_outcome
	{
		std::string first_counterexample;
		std::size_t permitted_count = 0;
		std::size_t permitted_with_folder_routing_count = 0;
		std::size_t refused_count = 0;
	};

	const auto check_send = [](
		const routing_graph& graph,
		const track_identity& source_track,
		const track_identity& destination_track,
		send_sweep_outcome& outcome) {
		std::string& first_counterexample = outcome.first_counterexample;

		std::vector<signal_edge> projected = naive_signal_edges(graph);
		projected.push_back(signal_edge{
			source_track,
			destination_track,
			signal_edge_kind::explicit_send,
		});

		const bool the_result_would_cycle = a_cycle_exists(projected);
		const routing_change_decision decision =
			evaluate_send_creation(graph, source_track, destination_track);

		if (decision.permitted == the_result_would_cycle)
		{
			if (first_counterexample.empty())
			{
				first_counterexample = describe_graph(graph)
					+ " | send " + source_track + "->" + destination_track
					+ (decision.permitted ? " was permitted" : " was refused")
					+ ", and the resulting routing "
					+ (the_result_would_cycle ? "does cycle" : "does not cycle")
					+ ": " + join_edge_keys(sorted_edge_keys(projected));
			}

			return;
		}

		if (decision.permitted)
		{
			++outcome.permitted_count;

			if (any_edge_is_folder_summing(projected))
			{
				++outcome.permitted_with_folder_routing_count;
			}

			// A permitted change carries nothing a refusal would.
			if (!decision.refusal_reason.empty() && first_counterexample.empty())
			{
				first_counterexample = describe_graph(graph)
					+ " | permitted send " + source_track + "->" + destination_track
					+ " carried refusal reason " + decision.refusal_reason;
			}

			return;
		}

		++outcome.refused_count;

		// The refusal the schema expects, not just a negative verdict.
		if ((decision.refusal_reason != "signal_cycle"
				|| decision.acknowledgement_field != "confirmedSignalCycle"
				|| !decision.cycle.cycle_detected
				|| !is_a_closed_loop_within(decision.cycle.cycle_edges, projected))
			&& first_counterexample.empty())
		{
			first_counterexample = describe_graph(graph)
				+ " | refused send " + source_track + "->" + destination_track
				+ " reported reason '" + decision.refusal_reason
				+ "', field '" + decision.acknowledgement_field
				+ "', loop " + decision.cycle.describe();
		}
	};

	SECTION("every possible send over every routing graph of up to three tracks")
	{
		send_sweep_outcome outcome;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			for (const track_node& source : graph.tracks_in_project_order)
			{
				for (const track_node& destination : graph.tracks_in_project_order)
				{
					check_send(graph, source.identity, destination.identity, outcome);
				}
			}
		}

		REQUIRE(outcome.first_counterexample == std::string{});

		// Both outcomes occur, so neither direction is vacuous.
		CHECK(outcome.permitted_count > 0);
		CHECK(outcome.refused_count > 0);

		// And the permissive direction is not carried entirely by projects with no
		// folder routing in them. Requirement 9.7's no-false-refusal half has to hold
		// on graphs where folder summing is one of the mechanisms, not only on flat
		// ones where the detector has nothing implicit to trip over.
		CHECK(outcome.permitted_with_folder_routing_count > 0);
	}

	SECTION("sampled sends over sampled graphs of up to eight tracks")
	{
		// Reproducible from the seed: 0x5e4d5.
		DeterministicBytes bytes{0x5e4d5u};

		send_sweep_outcome outcome;

		for (const routing_graph& graph : sampled_graphs())
		{
			const std::size_t track_count = graph.tracks_in_project_order.size();

			for (int attempt = 0; attempt < 4; ++attempt)
			{
				check_send(
					graph,
					graph.tracks_in_project_order[bytes.below(track_count)].identity,
					graph.tracks_in_project_order[bytes.below(track_count)].identity,
					outcome);
			}
		}

		REQUIRE(outcome.first_counterexample == std::string{});
		CHECK(outcome.permitted_count > 0);
		CHECK(outcome.refused_count > 0);
		CHECK(outcome.permitted_with_folder_routing_count > 0);
	}
}

TEST_CASE("Property 27: bus construction is permitted whenever the result is acyclic", "[daw][routing][property]")
{
	// Both bus types are swept because they project different edge sets. A summing bus
	// re-parents its sources — it can close a loop through folder summing while
	// creating no send at all, which is the folder-closed case in the shape a
	// `create_bus` call produces. An aux bus adds sends and leaves the sources where
	// they were.
	struct bus_sweep_outcome
	{
		std::string first_counterexample;
		std::size_t permitted_count = 0;
		std::size_t permitted_with_folder_routing_count = 0;
		std::size_t refused_count = 0;
		std::size_t summing_refusals_closing_through_folder_routing = 0;
		std::size_t aux_plans_checked = 0;
		std::size_t summing_plans_checked = 0;
	};

	const auto check_plan = [](
		const routing_graph& graph,
		const bus_construction_plan& plan,
		bus_sweep_outcome& outcome) {
		const std::vector<signal_edge> projected = naive_bus_projection(graph, plan);
		const bool the_result_would_cycle = a_cycle_exists(projected);

		const routing_change_decision decision = evaluate_bus_construction(graph, plan);

		if (plan.type == bus_type::summing)
		{
			++outcome.summing_plans_checked;
		}
		else
		{
			++outcome.aux_plans_checked;
		}

		if (decision.permitted == the_result_would_cycle)
		{
			if (outcome.first_counterexample.empty())
			{
				outcome.first_counterexample = describe_graph(graph)
					+ " | " + describe_plan(plan)
					+ (decision.permitted ? " was permitted" : " was refused")
					+ ", and the resulting routing "
					+ (the_result_would_cycle ? "does cycle" : "does not cycle")
					+ ": " + join_edge_keys(sorted_edge_keys(projected));
			}

			return;
		}

		// The projected routing itself, not only the verdict over it. A bus that
		// forgot to re-parent its sources is invisible from the verdict alone: a loop
		// needing the stale folder edge implies a loop the correct projection has too.
		const std::vector<std::string> reference = sorted_edge_keys(projected);
		const std::vector<std::string> produced =
			sorted_edge_keys(project_bus_construction(graph, plan));

		if (reference != produced && outcome.first_counterexample.empty())
		{
			outcome.first_counterexample = describe_graph(graph)
				+ " | " + describe_plan(plan)
				+ " | oracle routing: " + join_edge_keys(reference)
				+ " | detector routing: " + join_edge_keys(produced);

			return;
		}

		if (decision.permitted)
		{
			++outcome.permitted_count;

			if (any_edge_is_folder_summing(projected))
			{
				++outcome.permitted_with_folder_routing_count;
			}

			if (!decision.refusal_reason.empty() && outcome.first_counterexample.empty())
			{
				outcome.first_counterexample = describe_graph(graph)
					+ " | permitted " + describe_plan(plan)
					+ " carried refusal reason " + decision.refusal_reason;
			}

			return;
		}

		++outcome.refused_count;

		if (plan.type == bus_type::summing && decision.cycle.closes_through_folder_routing())
		{
			++outcome.summing_refusals_closing_through_folder_routing;
		}

		if ((decision.refusal_reason != "signal_cycle"
				|| decision.acknowledgement_field != "confirmedSignalCycle"
				|| !decision.cycle.cycle_detected
				|| !is_a_closed_loop_within(decision.cycle.cycle_edges, projected))
			&& outcome.first_counterexample.empty())
		{
			outcome.first_counterexample = describe_graph(graph)
				+ " | refused " + describe_plan(plan)
				+ " reported reason '" + decision.refusal_reason
				+ "', field '" + decision.acknowledgement_field
				+ "', loop " + decision.cycle.describe();
		}
	};

	SECTION("every bus plan over every routing graph of up to two tracks")
	{
		bus_sweep_outcome outcome;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			if (graph.tracks_in_project_order.size() > 2)
			{
				continue;
			}

			for (const bus_construction_plan& plan : enumerate_bus_plans(graph))
			{
				check_plan(graph, plan, outcome);
			}
		}

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.permitted_count > 0);
		CHECK(outcome.permitted_with_folder_routing_count > 0);
		CHECK(outcome.refused_count > 0);

		// Both projections were exercised, and a summing bus did close a loop through
		// folder summing — the case that needs no send to exist.
		CHECK(outcome.summing_plans_checked > 0);
		CHECK(outcome.aux_plans_checked > 0);
		CHECK(outcome.summing_refusals_closing_through_folder_routing > 0);
	}

	SECTION("sampled bus plans over every routing graph of up to three tracks")
	{
		// The exhaustive plan product over the three-track graphs is 224 plans each,
		// so plans are sampled there rather than enumerated. Reproducible from the
		// seed: 0xb0511e5.
		DeterministicBytes bytes{0xb0511e5u};

		bus_sweep_outcome outcome;

		for (const routing_graph& graph : exhaustive_graphs())
		{
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				check_plan(graph, generate_bus_plan(bytes, graph), outcome);
			}
		}

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.permitted_count > 0);
		CHECK(outcome.permitted_with_folder_routing_count > 0);
		CHECK(outcome.refused_count > 0);
		CHECK(outcome.summing_refusals_closing_through_folder_routing > 0);
	}

	SECTION("sampled bus plans over sampled graphs of up to eight tracks")
	{
		// Reproducible from the seed: 0xb0558.
		DeterministicBytes bytes{0xb0558u};

		bus_sweep_outcome outcome;

		for (const routing_graph& graph : sampled_graphs())
		{
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				check_plan(graph, generate_bus_plan(bytes, graph), outcome);
			}
		}

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.permitted_count > 0);
		CHECK(outcome.permitted_with_folder_routing_count > 0);
		CHECK(outcome.refused_count > 0);
		CHECK(outcome.summing_refusals_closing_through_folder_routing > 0);
	}
}
