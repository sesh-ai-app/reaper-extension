// Signal routing cycle detection for send creation and bus construction
// (requirement 9.7, design "Tool Executor" — the second of the four runtime checks
// JSON Schema cannot express).
//
// A feedback loop in REAPER is not a refusal the schema can carry. Whether
// `create_send` closes a loop depends on every other routing relationship in the
// project, so the check has to happen here, and `create-send.schema.json` and
// `create-bus.schema.json` both carry a `confirmedSignalCycle` acknowledgement flag
// for the escalation this produces. The refusal it feeds is
// `messages/tool-result-refusal.schema.json` with reason `signal_cycle`.
//
// ---------------------------------------------------------------------------
// Why the sends list is not the graph
//
// This is the whole reason the component exists, and a detector that searches only
// the sends list passes every obvious test while missing the case requirement 9.7
// was written for.
//
// REAPER sums signal through three separate mechanisms (taxonomy, "Routing and
// Buses"), and only one of them appears in send enumeration:
//
//   - An explicit send, created with `CreateTrackSend`. Enumerable.
//   - Folder summing, where a child routes into its folder parent because its
//     parent send flag (`B_MAINSEND`) is set. **Invisible to send enumeration.**
//   - The master send, which a top-level track's parent send flag targets instead.
//
// So a cycle can close through folder routing without a second send existing. Give a
// folder parent an explicit send down to one of its own children and the loop is
// child -> parent (folder summing) -> child (the send). One send, one loop, nothing
// in the sends list that looks wrong.
//
// Folder parentage itself has no parent pointer to read. A folder is a positional
// property encoded as `I_FOLDERDEPTH` deltas across the project's track order, so
// the graph searched here derives parentage by accumulating those deltas and then
// adds an implicit edge per child whose parent send is on. Both edge kinds go into
// one directed graph and one search runs over it.
//
// ---------------------------------------------------------------------------
// The master send is not an edge
//
// A top-level track with its parent send on routes to the master track. That is a
// sink: the master has no parent send of its own and cannot be the source of a send
// back into the project. Modelling it as a node would add a vertex that no cycle can
// pass through, so a parent send with no folder parent above it contributes no edge.
//
// ---------------------------------------------------------------------------
// No false refusals
//
// Property 27 is as binding as property 26, and in the harder direction: an
// over-eager detector refuses routing a producer is entitled to build, which is a
// worse bug than the one it prevents because it blocks work rather than warning
// about it. Two specific ways to get this wrong are ruled out by construction:
//
//   - The search distinguishes "on the current path" from "already finished" rather
//     than keeping one visited set. A diamond — A into both B and C, both into D —
//     reaches D twice and is perfectly acyclic. One visited set calls it a cycle.
//   - Every edge is directed and stays directed. A send from a child up to the
//     folder parent it already feeds is a parallel edge, not a loop; undirected
//     reachability cannot tell those apart.
//
// ---------------------------------------------------------------------------
// Pure graph work
//
// Nothing here calls the REAPER C API. The graph arrives as plain data — track
// identities, folder depth deltas, parent send flags, send edges — which is what
// makes the whole component testable outside REAPER, and is why the design lists
// "cycle detection over a synthetic routing graph, including folder-closed cycles"
// under what is well covered without it.
//
// Defined inline in the header, following `unit_conversion.h`: the logic under test
// is compiled into the Catch2 binary directly, and a header-only unit is testable
// without being linked into the extension's shared library.
//
// ---------------------------------------------------------------------------
// Overlap worth knowing about
//
// Folder depth accumulation also belongs to the Folder Invariant Keeper
// (requirement 11), which repairs those deltas after a structural edit. The
// accumulation here is read-only — it derives parentage and changes nothing — so the
// two are not in conflict, but they are two implementations of the same arithmetic
// and are worth reconciling once both exist.

#ifndef SESH_AI_DAW_CYCLE_DETECTOR_H
#define SESH_AI_DAW_CYCLE_DETECTOR_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sesh_ai::daw
{
	// A track's stable REAPER GUID. Opaque here — nothing in this file parses it, so
	// a test is free to use readable names instead.
	using track_identity = std::string;

	// The reason carried in the refusal payload, and the input property the agent
	// sets on the retry. Both are contract values from the schemas rather than
	// prose, so they are stated once here instead of spelled out at each call site.
	inline constexpr std::string_view signal_cycle_refusal_reason{"signal_cycle"};
	inline constexpr std::string_view signal_cycle_acknowledgement_field{"confirmedSignalCycle"};

	// One track, as much of it as routing depends on.
	struct track_node
	{
		track_identity identity;

		// REAPER's `I_FOLDERDEPTH`. Zero for an ordinary track, 1 for a track that
		// opens a folder, and negative for a track that closes that many folders
		// after itself. There is no parent pointer — this delta and the track's
		// position are the entire representation.
		int folder_depth_delta = 0;

		// REAPER's `B_MAINSEND`, owned by the child. On means the track's output
		// reaches its folder parent, or the master track when it has no folder
		// parent. REAPER's default for a new track is on.
		bool parent_send_enabled = true;
	};

	// An explicit send, as it would come back from send enumeration.
	struct send_edge
	{
		track_identity source_track;
		track_identity destination_track;
	};

	// The routing state a cycle search needs, in project order.
	//
	// Track order is load-bearing rather than incidental: folder parentage is derived
	// from it, so the same tracks in a different order are a different graph.
	//
	// Identities are assumed unique, which they are when they are REAPER GUIDs. Two
	// entries sharing one identity collapse to a single node, which is the
	// conservative reading of a malformed graph — it can report a cycle that the
	// distinct tracks would not have, and cannot miss one.
	struct routing_graph
	{
		std::vector<track_node> tracks_in_project_order;
		std::vector<send_edge> explicit_sends;
	};

	// Which mechanism an edge came from. Needed on the way out, not just on the way
	// in: a refusal that says "this closes through folder summing" is actionable in a
	// way that "this closes" is not, because folder summing is the edge the producer
	// cannot see in the routing window.
	enum class signal_edge_kind
	{
		explicit_send,
		folder_parent_send
	};

	// One directed edge in the signal graph, from either mechanism.
	struct signal_edge
	{
		track_identity source_track;
		track_identity destination_track;
		signal_edge_kind kind = signal_edge_kind::explicit_send;
	};

	// What the search found.
	//
	// One cycle, not all of them. Requirement 9.7 refuses the operation, so a second
	// loop changes nothing about the outcome, and one witness is what the refusal has
	// room to explain.
	struct cycle_detection_result
	{
		bool cycle_detected = false;

		// The loop in traversal order. Consecutive edges join up, and the last edge's
		// destination is the first edge's source. Empty when nothing was found; a
		// single self-referencing edge for a send from a track to itself.
		std::vector<signal_edge> cycle_edges;

		// True when the loop passes through folder summing at least once — the case
		// that is invisible in the sends list.
		bool closes_through_folder_routing() const
		{
			for (const signal_edge& edge : cycle_edges)
			{
				if (edge.kind == signal_edge_kind::folder_parent_send)
				{
					return true;
				}
			}

			return false;
		}

		// The tracks on the loop, each once, in traversal order. What a refusal
		// enumerates in `blocking`.
		std::vector<track_identity> tracks_on_cycle() const
		{
			std::vector<track_identity> tracks;
			tracks.reserve(cycle_edges.size());

			for (const signal_edge& edge : cycle_edges)
			{
				tracks.push_back(edge.source_track);
			}

			return tracks;
		}

		// The loop as one line, naming the mechanism at each step. For the
		// description on a blocking entity and for a test failure that says which
		// loop was found rather than only that one was.
		std::string describe() const
		{
			if (!cycle_detected || cycle_edges.empty())
			{
				return {};
			}

			std::string description = cycle_edges.front().source_track;

			for (const signal_edge& edge : cycle_edges)
			{
				description += (edge.kind == signal_edge_kind::folder_parent_send)
					? " -(folder parent send)-> "
					: " -(send)-> ";
				description += edge.destination_track;
			}

			return description;
		}
	};

	// Which of the two things `create_bus` builds, matching the tool schema's `type`.
	enum class bus_type
	{
		// A folder parent above the sources. The sources are re-parented into it and
		// reach it through folder summing, so this changes implicit edges, not sends.
		summing,

		// A return track the sources feed. This adds explicit sends and leaves the
		// sources routed where they already were.
		aux
	};

	// What a `create_bus` call would do to routing.
	//
	// Stated as the routing outcome rather than as track moves and depth deltas, and
	// that is deliberate. Laying out a summing bus means inserting a track and
	// repairing deltas, which is the Folder Invariant Keeper's arithmetic and not
	// something to reimplement here for a cycle search. What the search needs is who
	// ends up feeding whom, which is exactly what this records.
	struct bus_construction_plan
	{
		bus_type type = bus_type::summing;

		// The track being created. Not present in the graph yet.
		track_identity bus_track;

		// The tracks being bussed. An identity not in the graph contributes its edges
		// anyway rather than being dropped, so a plan naming a track the caller has
		// not listed still gets checked.
		std::vector<track_identity> source_tracks;

		// The folder the new bus itself lands in, which decides where the bus's own
		// output goes. Absent means top level, where the bus's parent send reaches
		// the master and contributes no edge.
		//
		// For a summing bus this is normally the sources' former folder parent, so
		// the submix keeps arriving where the sources used to arrive. For an aux bus
		// it is wherever the return track is inserted. Supplied by the caller in both
		// cases, because the Tool Executor knows the placement it intends and sources
		// with differing parents have no single answer to derive.
		std::optional<track_identity> bus_folder_parent;

		// The bus's own parent send flag. REAPER's default for a new track is on.
		bool bus_parent_send_enabled = true;

		// Whether the sources of a summing bus keep their parent send on, which is
		// what makes them actually sum into the new folder parent. Off means they
		// reach the bus in the folder but do not feed it, which is unusual and
		// representable. Ignored for an aux bus, where the sources' parent sends are
		// untouched.
		bool source_parent_send_enabled = true;
	};

	// Whether a routing change may proceed, and what to say when it may not.
	struct routing_change_decision
	{
		bool permitted = true;

		// Always populated, including when the change is permitted anyway because the
		// producer acknowledged the loop. A caller that wants to report what it is
		// about to build has the loop to hand.
		cycle_detection_result cycle;

		// The refusal payload's `reason` and `acknowledgementField`. Empty when
		// permitted — a refusal is the only thing that carries them.
		std::string refusal_reason;
		std::string acknowledgement_field;
	};

	namespace detail
	{
		inline constexpr std::size_t no_signal_edge = static_cast<std::size_t>(-1);

		// Where the search is up to for one node on the current path.
		struct cycle_search_frame
		{
			std::size_t node = 0;
			std::size_t next_outgoing_slot = 0;
		};

		// A node's relationship to the current depth-first path. The distinction
		// between the last two is what keeps an acyclic diamond from being reported
		// as a loop.
		enum class node_visit_state : unsigned char
		{
			unvisited,
			on_current_path,
			finished
		};
	}

	// Folder parentage by track index, derived from the depth deltas.
	//
	// The innermost folder still open at a track's position is that track's parent.
	// Absent means top level. The delta is applied *after* the track is assigned its
	// own parent, because a track that opens a folder is a sibling of the tracks
	// above it, not a child of the folder it is opening.
	//
	// Two malformed inputs are tolerated rather than rejected, since the graph can
	// arrive mid-edit and refusing to search it would be worse than searching a
	// repaired reading of it. A delta above 1 opens one folder, which is all REAPER
	// can represent — a track cannot be two folder parents. A delta closing more
	// folders than are open closes the ones that are. Repairing the deltas themselves
	// is requirement 11's job, not this one's.
	inline std::vector<std::optional<std::size_t>> derive_folder_parent_indices(
		const std::vector<track_node>& tracks_in_project_order)
	{
		std::vector<std::optional<std::size_t>> folder_parent_indices(tracks_in_project_order.size());
		std::vector<std::size_t> open_folder_indices;

		for (std::size_t track_index = 0; track_index < tracks_in_project_order.size(); ++track_index)
		{
			if (!open_folder_indices.empty())
			{
				folder_parent_indices[track_index] = open_folder_indices.back();
			}

			const int folder_depth_delta = tracks_in_project_order[track_index].folder_depth_delta;

			if (folder_depth_delta > 0)
			{
				open_folder_indices.push_back(track_index);
			}
			else if (folder_depth_delta < 0)
			{
				// Widened before negating so the most negative representable delta
				// does not overflow on the way.
				const long long requested_closes = -static_cast<long long>(folder_depth_delta);
				const std::size_t folders_to_close =
					(requested_closes > static_cast<long long>(open_folder_indices.size()))
						? open_folder_indices.size()
						: static_cast<std::size_t>(requested_closes);

				open_folder_indices.resize(open_folder_indices.size() - folders_to_close);
			}
		}

		return folder_parent_indices;
	}

	// The same parentage keyed by identity, for callers that have a track rather than
	// an index.
	inline std::vector<std::optional<track_identity>> derive_folder_parents(
		const std::vector<track_node>& tracks_in_project_order)
	{
		const std::vector<std::optional<std::size_t>> folder_parent_indices =
			derive_folder_parent_indices(tracks_in_project_order);

		std::vector<std::optional<track_identity>> folder_parents(tracks_in_project_order.size());

		for (std::size_t track_index = 0; track_index < folder_parent_indices.size(); ++track_index)
		{
			if (folder_parent_indices[track_index].has_value())
			{
				folder_parents[track_index] =
					tracks_in_project_order[*folder_parent_indices[track_index]].identity;
			}
		}

		return folder_parents;
	}

	// Every directed edge signal can travel along, from both mechanisms.
	//
	// Implicit folder edges come first in project order, then explicit sends in the
	// order they were enumerated, so the edge set is a deterministic function of the
	// graph and a reported cycle does not move between runs.
	inline std::vector<signal_edge> collect_signal_edges(const routing_graph& graph)
	{
		const std::vector<std::optional<std::size_t>> folder_parent_indices =
			derive_folder_parent_indices(graph.tracks_in_project_order);

		std::vector<signal_edge> edges;
		edges.reserve(graph.tracks_in_project_order.size() + graph.explicit_sends.size());

		for (std::size_t track_index = 0; track_index < graph.tracks_in_project_order.size(); ++track_index)
		{
			const track_node& track = graph.tracks_in_project_order[track_index];

			if (!track.parent_send_enabled)
			{
				continue;
			}

			// No folder parent means the parent send targets the master, which is a
			// sink and contributes nothing a cycle can pass through.
			if (!folder_parent_indices[track_index].has_value())
			{
				continue;
			}

			const track_node& folder_parent =
				graph.tracks_in_project_order[*folder_parent_indices[track_index]];

			edges.push_back(signal_edge{
				track.identity,
				folder_parent.identity,
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

	// Depth-first search for a directed cycle over an edge set, returning the first
	// loop found.
	//
	// Iterative rather than recursive: the path can be as long as the project's track
	// count, and a producer's project is allowed to be large without the check
	// deciding how much call stack it may have.
	//
	// Nodes are numbered in the order they first appear in the edge set, so the
	// traversal order is the graph's rather than a hash table's, and the loop reported
	// for a given graph is always the same one. A node with no edges at all is not
	// numbered, since it cannot be on a cycle.
	inline cycle_detection_result detect_cycle_in_signal_edges(const std::vector<signal_edge>& edges)
	{
		cycle_detection_result result;

		std::unordered_map<track_identity, std::size_t> node_index_by_identity;
		std::vector<std::vector<std::size_t>> outgoing_edge_indices;

		const auto node_index_for = [&](const track_identity& identity) {
			const auto existing = node_index_by_identity.find(identity);

			if (existing != node_index_by_identity.end())
			{
				return existing->second;
			}

			const std::size_t new_index = outgoing_edge_indices.size();
			node_index_by_identity.emplace(identity, new_index);
			outgoing_edge_indices.emplace_back();

			return new_index;
		};

		std::vector<std::size_t> edge_source_node(edges.size(), 0);
		std::vector<std::size_t> edge_destination_node(edges.size(), 0);

		for (std::size_t edge_index = 0; edge_index < edges.size(); ++edge_index)
		{
			const std::size_t source_node = node_index_for(edges[edge_index].source_track);
			const std::size_t destination_node = node_index_for(edges[edge_index].destination_track);

			edge_source_node[edge_index] = source_node;
			edge_destination_node[edge_index] = destination_node;
			outgoing_edge_indices[source_node].push_back(edge_index);
		}

		const std::size_t node_count = outgoing_edge_indices.size();

		std::vector<detail::node_visit_state> visit_states(node_count, detail::node_visit_state::unvisited);

		// The edge the search arrived on, per node. Walking these backwards from the
		// node that closed the loop recovers the loop itself.
		std::vector<std::size_t> arriving_edge_index(node_count, detail::no_signal_edge);

		std::vector<detail::cycle_search_frame> search_stack;

		for (std::size_t start_node = 0; start_node < node_count; ++start_node)
		{
			if (visit_states[start_node] != detail::node_visit_state::unvisited)
			{
				continue;
			}

			visit_states[start_node] = detail::node_visit_state::on_current_path;
			search_stack.push_back(detail::cycle_search_frame{start_node, 0});

			while (!search_stack.empty())
			{
				// Copied out rather than held by reference: pushing the next frame can
				// reallocate the stack.
				const std::size_t current_node = search_stack.back().node;
				const std::vector<std::size_t>& outgoing = outgoing_edge_indices[current_node];

				if (search_stack.back().next_outgoing_slot >= outgoing.size())
				{
					visit_states[current_node] = detail::node_visit_state::finished;
					search_stack.pop_back();
					continue;
				}

				const std::size_t edge_index = outgoing[search_stack.back().next_outgoing_slot];
				++search_stack.back().next_outgoing_slot;

				const std::size_t destination_node = edge_destination_node[edge_index];

				if (visit_states[destination_node] == detail::node_visit_state::on_current_path)
				{
					// The edge reaches back onto the path the search is standing on,
					// which is a loop. Recover it by walking the arriving edges from
					// here back to where the loop closes, then put them in traversal
					// order and add the closing edge last.
					std::vector<signal_edge> edges_towards_closure;

					for (std::size_t node_on_loop = current_node; node_on_loop != destination_node;)
					{
						const std::size_t arriving = arriving_edge_index[node_on_loop];

						// Unreachable: a node on the current path other than the start
						// was arrived at over an edge. Guarded rather than asserted so a
						// future change cannot turn this into a walk off the end.
						if (arriving == detail::no_signal_edge)
						{
							break;
						}

						edges_towards_closure.push_back(edges[arriving]);
						node_on_loop = edge_source_node[arriving];
					}

					result.cycle_detected = true;
					result.cycle_edges.assign(
						edges_towards_closure.rbegin(),
						edges_towards_closure.rend());
					result.cycle_edges.push_back(edges[edge_index]);

					return result;
				}

				if (visit_states[destination_node] == detail::node_visit_state::unvisited)
				{
					arriving_edge_index[destination_node] = edge_index;
					visit_states[destination_node] = detail::node_visit_state::on_current_path;
					search_stack.push_back(detail::cycle_search_frame{destination_node, 0});
				}

				// A finished node is a subgraph already searched and found acyclic.
				// Reaching it again is the diamond case and is not a loop.
			}
		}

		return result;
	}

	// Whether the project as it stands already contains a loop.
	inline cycle_detection_result detect_signal_cycle(const routing_graph& graph)
	{
		return detect_cycle_in_signal_edges(collect_signal_edges(graph));
	}

	// The edge set the project would have if this send were created.
	inline std::vector<signal_edge> project_send_creation(
		const routing_graph& graph,
		const track_identity& source_track,
		const track_identity& destination_track)
	{
		std::vector<signal_edge> edges = collect_signal_edges(graph);

		edges.push_back(signal_edge{
			source_track,
			destination_track,
			signal_edge_kind::explicit_send,
		});

		return edges;
	}

	// The edge set the project would have if this bus were built.
	//
	// A summing bus re-parents its sources, so their existing folder edges are
	// replaced rather than added to: each source stops feeding the folder parent it
	// had and starts feeding the bus, and the bus feeds whatever it was placed in.
	// That replacement is where a folder-closed cycle can appear out of a call that
	// creates no sends at all — bussing a child whose former parent sends down to it
	// turns that send into a loop.
	//
	// An aux bus adds sends and re-parents nothing, so the existing edges all stand.
	inline std::vector<signal_edge> project_bus_construction(
		const routing_graph& graph,
		const bus_construction_plan& plan)
	{
		const std::vector<signal_edge> existing_edges = collect_signal_edges(graph);

		const auto is_source_track = [&plan](const track_identity& identity) {
			for (const track_identity& source_track : plan.source_tracks)
			{
				if (source_track == identity)
				{
					return true;
				}
			}

			return false;
		};

		std::vector<signal_edge> edges;
		edges.reserve(existing_edges.size() + plan.source_tracks.size() + 1);

		for (const signal_edge& edge : existing_edges)
		{
			const bool edge_is_replaced_by_the_bus =
				plan.type == bus_type::summing
				&& edge.kind == signal_edge_kind::folder_parent_send
				&& is_source_track(edge.source_track);

			if (!edge_is_replaced_by_the_bus)
			{
				edges.push_back(edge);
			}
		}

		for (const track_identity& source_track : plan.source_tracks)
		{
			if (plan.type == bus_type::summing)
			{
				if (plan.source_parent_send_enabled)
				{
					edges.push_back(signal_edge{
						source_track,
						plan.bus_track,
						signal_edge_kind::folder_parent_send,
					});
				}
			}
			else
			{
				edges.push_back(signal_edge{
					source_track,
					plan.bus_track,
					signal_edge_kind::explicit_send,
				});
			}
		}

		// Absent parent means top level, where the bus's own output reaches the
		// master and contributes no edge.
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

	namespace detail
	{
		// The shared shape of both decisions: refuse on a loop, unless the producer
		// has already been shown it and said they meant it.
		inline routing_change_decision decide_on_projected_edges(
			const std::vector<signal_edge>& projected_edges,
			bool producer_acknowledged_signal_cycle)
		{
			routing_change_decision decision;
			decision.cycle = detect_cycle_in_signal_edges(projected_edges);

			if (!decision.cycle.cycle_detected || producer_acknowledged_signal_cycle)
			{
				decision.permitted = true;
				return decision;
			}

			decision.permitted = false;
			decision.refusal_reason = std::string{signal_cycle_refusal_reason};
			decision.acknowledgement_field = std::string{signal_cycle_acknowledgement_field};

			return decision;
		}
	}

	// Whether `create_send` may proceed.
	//
	// The acknowledgement is the producer's, not the agent's: `confirmedSignalCycle`
	// is documented as something set only after a refusal reported the loop and the
	// producer confirmed they intend it anyway. A deliberate feedback path is a real
	// thing to build, so the flag permits rather than overriding — and the loop is
	// still reported on the decision either way.
	inline routing_change_decision evaluate_send_creation(
		const routing_graph& graph,
		const track_identity& source_track,
		const track_identity& destination_track,
		bool producer_acknowledged_signal_cycle = false)
	{
		return detail::decide_on_projected_edges(
			project_send_creation(graph, source_track, destination_track),
			producer_acknowledged_signal_cycle);
	}

	// Whether `create_bus` may proceed.
	inline routing_change_decision evaluate_bus_construction(
		const routing_graph& graph,
		const bus_construction_plan& plan,
		bool producer_acknowledged_signal_cycle = false)
	{
		return detail::decide_on_projected_edges(
			project_bus_construction(graph, plan),
			producer_acknowledged_signal_cycle);
	}
}

#endif
