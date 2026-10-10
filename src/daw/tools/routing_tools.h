// The seven routing tools (task 10.5, requirements 9.1, 9.2, 9.7).
//
//   create_send, remove_send, set_send_state, set_parent_send,
//   create_bus, remove_bus, get_routing
//
// Everything that decides anything lives in this header as pure functions over plain
// data. The REAPER calls live behind `routing_host`, implemented once in
// reaper_routing_host.cpp. Same split as `render_coordinator.h` /
// `reaper_render_host.h`, and for the same reason: the Catch2 target compiles what it
// finds under `tests/` and does not compile `src/`, so logic the suite exercises has
// to be reachable through the header, and the test target has no SDK include path,
// which is what keeps the split honest rather than aspirational.
//
// ---------------------------------------------------------------------------
// Three summing mechanisms, and only one of them is a send
//
// The taxonomy's "Routing and Buses" is load-bearing here rather than background
// reading. REAPER sums signal three ways:
//
//   - an explicit **send**, which is the only one send enumeration can see;
//   - **folder** summing, where a child reaches its **folder parent** because the
//     child's **parent send** (`B_MAINSEND`) is on — a flag on the child, not a send
//     object, and therefore invisible to send enumeration;
//   - the master send, which is the same flag targeting the master track when the
//     track has no folder parent.
//
// Two consequences shape this file. `get_routing` reports folder depth and the parent
// send flag alongside sends and receives, because a result carrying sends alone would
// describe a fully routed session as unrouted. And `create_bus` is two genuinely
// different builds behind one name: a **summing bus** is a folder, so its structural
// half is `I_FOLDERDEPTH` arithmetic and belongs to the Folder Invariant Keeper; an
// **aux bus** is a return track fed by sends, so its structural half is empty and its
// routing half is a send per source. `remove_bus` splits the same way, and dissolving
// a summing bus is a folder dissolution — requirement 11.3, the Keeper's business.
//
// ---------------------------------------------------------------------------
// Cycle detection is a precondition, not something the mutating body does
//
// Requirement 9.7 refuses a routing change that would close a signal loop, and
// requirement 9.3 with requirement 10.7 says a refusal records no undo block and no
// undo position marker. `tool_executor.h` makes that control flow rather than a rule:
// `register_mutating_tool` takes a precondition that runs *before* any block is
// opened and is the only place a mutating tool may refuse. So every cycle check in
// this file is a precondition, and every mutating body here returns a type with no
// refusal alternative.
//
// The checks ask `cycle_detector.h` about the **projected** graph — the graph as it
// would be after the change — because a detector asked about the present state never
// refuses anything. `evaluate_send_creation` and `evaluate_bus_construction` project
// internally from the current graph plus the change; `set_parent_send` has no
// dedicated entry point, so this file builds the projected graph by flipping the
// flags and asks `detect_signal_cycle` about it. No second graph traversal is written
// here, and `refusal_from_routing_decision` in `tool_executor.h` is what maps a
// decision to a refusal, including returning empty when the change may proceed.
//
// `set_parent_send` and `create_bus` are the two that can close a loop through folder
// routing with no explicit send involved, which is the case a detector searching the
// sends list misses. Both are checked, and the suite beside this file drives exactly
// those paths.
//
// ---------------------------------------------------------------------------
// One block per call, per-action outcomes only where an action can fail
//
// A handler returns `handler_success` when everything it attempted landed, and
// `handler_action_outcomes` as soon as anything did not. The framework attaches the
// undo report either way, keeps successful actions (there is no undo call on this
// path, so requirement 9.4's no-rollback rule holds by omission), and requires a
// reason on every failure because `action_failed` cannot be constructed without one.
//
// An invalid call — two selectors needed and one supplied, a send index past the end
// of the track's sends, `remove_bus` pointed at a track that is neither kind of bus —
// is a failed action carrying a reason, not a refusal. It names no acknowledgement
// the producer could confirm, which is the same reasoning requirement 9.6 gives for
// an inverted time range.
//
// ---------------------------------------------------------------------------
// Where the JSON is, and is not
//
// The framework is templated on the payload type and never reads a field of one
// (requirement 4.4: the MCP Tool Server already validated the input against the
// authoritative schema). This file inherits that and extends it in the obvious
// direction: `routing_payload_codec` is a struct of callables that extract a request
// from the already-validated input and serialise a result into the payload type.
// Extraction, not validation. The real build binds those to `nlohmann::json`; the
// suite binds them to plain structs, which is what lets the whole of this file be
// driven without a JSON library present.

#ifndef SESH_AI_DAW_TOOLS_ROUTING_TOOLS_H
#define SESH_AI_DAW_TOOLS_ROUTING_TOOLS_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <context/project_context_builder.h>
#include <context/structural_role.h>
#include <daw/cycle_detector.h>
#include <daw/folder_invariant_keeper.h>
#include <daw/tool_executor.h>
#include <daw/tools/shared_tool_definitions.h>
#include <daw/unit_conversion.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// `code` values these handlers produce on a failed action. Each names a call the
	// producer cannot acknowledge their way out of, which is why none of them is a
	// refusal reason.
	//
	// `missing_tool_input_code` is one of them and is not here: it is shared with
	// `track_structure_tools.h` and lives in `tools/shared_tool_definitions.h`, which
	// that header explains.
	inline constexpr std::string_view missing_routing_target_code{"missing_routing_target"};
	inline constexpr std::string_view unknown_routing_target_code{"unknown_routing_target"};
	inline constexpr std::string_view invalid_send_index_code{"invalid_send_index"};
	inline constexpr std::string_view not_a_bus_code{"not_a_bus"};
	inline constexpr std::string_view reaper_routing_failure_code{"reaper_routing_failure"};

	// Schema bounds from the seven output schemas, so a payload these handlers fill in
	// cannot fail the outbound validation requirement 4.1 asks for.
	inline constexpr std::size_t maximum_reported_tracks = 4096;
	inline constexpr std::size_t maximum_reported_sends = 512;
	inline constexpr std::size_t maximum_reported_sends_per_track = 256;
	inline constexpr std::size_t maximum_reported_bus_fx = 32;
	inline constexpr std::size_t maximum_reported_parent_send_changes = 512;

	// The level and pan bounds are `unit_conversion.h`'s — `minimum_volume_decibels`,
	// `maximum_volume_decibels`, `minimum_pan_percent`, `maximum_pan_percent`, resolved
	// from the enclosing namespace. Restating them here would be a second copy of a
	// schema bound, which is how the two come to disagree.

	// The identity the cycle check uses for a bus track that does not exist yet.
	//
	// `cycle_detector.h` treats an identity as an opaque string, so this only has to
	// be something no REAPER GUID can be — the schemas' `guid` pattern requires
	// braces around 36 hex-and-dash characters, and this is not that.
	inline constexpr std::string_view planned_bus_track_identity{"(the bus this call would create)"};

	// ---------------------------------------------------------------------------
	// Send mode
	// ---------------------------------------------------------------------------

	// `sendMode` from the output schemas. Where in the source track's chain the send
	// is taken from, which is a routing decision rather than a level.
	enum class routing_send_mode
	{
		post_fader,
		pre_fx,
		post_fx
	};

	inline constexpr std::string_view to_schema_string(routing_send_mode mode)
	{
		switch (mode)
		{
			case routing_send_mode::post_fader:
				return "post_fader";
			case routing_send_mode::pre_fx:
				return "pre_fx";
			case routing_send_mode::post_fx:
				return "post_fx";
		}

		return "post_fader";
	}

	// Empty for anything that is not one of the three schema strings, so an unknown
	// mode is a failed action at the call site rather than a silent fall back to
	// post-fader — which would move a send to a different point in the chain and
	// change what the producer hears.
	inline std::optional<routing_send_mode> routing_send_mode_from_schema_string(std::string_view mode)
	{
		if (mode == "post_fader")
		{
			return routing_send_mode::post_fader;
		}

		if (mode == "pre_fx")
		{
			return routing_send_mode::pre_fx;
		}

		if (mode == "post_fx")
		{
			return routing_send_mode::post_fx;
		}

		return std::nullopt;
	}

	// ---------------------------------------------------------------------------
	// The routing state a decision is made from
	// ---------------------------------------------------------------------------

	// One explicit send or receive. The same shape serves both directions, as
	// `sendDetail` does: the far end is named, so on a track's sends that is the
	// destination and on its receives it is the source.
	struct routing_send
	{
		// Index on the *source* track, which is where REAPER stores it and how
		// `remove_send` and `set_send_state` address it. A track can hold more than one
		// send to the same destination, so the far end alone does not identify it.
		int index = 0;

		std::string track_guid;
		std::string track_name;

		double volume_decibels = 0.0;
		double pan_percent = 0.0;
		bool muted = false;
		routing_send_mode mode = routing_send_mode::post_fader;
	};

	// One track's routing. Plain data, so every decision below is testable against a
	// synthetic session.
	struct routing_track
	{
		std::string guid;
		std::string name;

		// REAPER's `I_FOLDERDEPTH`. Parentage is this delta plus the track's position
		// and nothing else — there is no parent pointer to read.
		int folder_depth_delta = 0;

		// REAPER's `B_MAINSEND`, owned by this track. On means its output reaches its
		// folder parent, or the master track when it has none.
		bool parent_send_enabled = true;

		std::vector<routing_send> sends;
		std::vector<routing_send> receives;

		int item_count = 0;
		int fx_count = 0;
	};

	// The master track. Shaped like a track and not one: it sits outside the indexed
	// track list, so it has no folder depth and no parent to send to.
	struct routing_master_track
	{
		bool present = false;

		std::string guid;
		std::string name;

		std::vector<routing_send> receives;
		int fx_count = 0;
	};

	// One project's routing, read in one pass on the main thread.
	struct routing_snapshot
	{
		// False when the host could not read the project. A snapshot that failed reads
		// as a session with no tracks, and every decision here would then be made about
		// an empty project, so the handlers report a failed action instead.
		bool readable = false;

		std::vector<routing_track> tracks_in_project_order;
		routing_master_track master_track;
	};

	// One FX on the bus's chain. Chain position changes the sound, so the index is
	// part of the identity.
	struct routing_fx_placement
	{
		int index = 0;
		std::string name;
		bool bypassed = false;
	};

	// What `set_send_state` changes. Every field optional, because the tool sets the
	// properties the call named and leaves the rest as the producer had them.
	struct send_state_change
	{
		std::optional<double> volume_decibels;
		std::optional<double> pan_percent;
		std::optional<bool> muted;
		std::optional<routing_send_mode> mode;
	};

	// ---------------------------------------------------------------------------
	// The REAPER seam
	// ---------------------------------------------------------------------------

	// Everything these seven tools need REAPER for, and nothing else.
	//
	// Declared here rather than taken from a shared DAW facade on purpose: a seam this
	// narrow is one the suite can implement completely, and a handler cannot reach a
	// REAPER call nobody listed. `reaper_routing_host.h` implements it.
	class routing_host
	{
	public:
		virtual ~routing_host() = default;

		// --- reads ---

		// The whole project's routing. Read once per call, for the reason
		// `tool_executor.h` reads the track list once: reading per target lets the
		// producer's edit land between two reads of one call.
		virtual routing_snapshot read_routing_snapshot() = 0;

		// One send as it stands now, for reporting a write back. Empty when the index
		// no longer addresses a send.
		virtual std::optional<routing_send> read_send(
			const std::string& source_track_guid,
			int send_index) = 0;

		// --- sends ---

		// `CreateTrackSend`. The index the new send took on the source track, or empty
		// when REAPER would not create it.
		virtual std::optional<int> create_send(
			const std::string& source_track_guid,
			const std::string& destination_track_guid) = 0;

		// `RemoveTrackSend`. REAPER renumbers the sends after the removed one.
		virtual bool remove_send(const std::string& source_track_guid, int send_index) = 0;

		virtual bool write_send_state(
			const std::string& source_track_guid,
			int send_index,
			const send_state_change& change) = 0;

		// --- the parent send, which is a flag on the child ---

		virtual bool write_parent_send(const std::string& track_guid, bool enabled) = 0;

		// --- track structure, for the two bus builds ---

		virtual std::optional<TrackReference> insert_track(int project_index, const std::string& name) = 0;

		virtual bool delete_track(const std::string& track_guid) = 0;

		// Puts the project's tracks in this order. One call rather than a move per
		// track: gathering a summing bus's sources is one rearrangement, and applying
		// it in pieces leaves the folder structure invalid in between.
		virtual bool apply_track_order(const std::vector<std::string>& track_guids_in_project_order) = 0;

		// `I_FOLDERDEPTH`. Only ever called with a delta the Folder Invariant Keeper
		// produced.
		virtual bool write_folder_depth(const std::string& track_guid, int folder_depth_delta) = 0;

		// --- FX on a bus, which is most of why a producer wants one ---

		virtual std::optional<routing_fx_placement> add_fx(
			const std::string& track_guid,
			const std::string& fx_name) = 0;
	};

	// ---------------------------------------------------------------------------
	// Snapshot reading
	// ---------------------------------------------------------------------------

	namespace routing_detail
	{
		inline double clamped(double value, double lowest, double highest)
		{
			if (value < lowest)
			{
				return lowest;
			}

			if (value > highest)
			{
				return highest;
			}

			return value;
		}

		inline double clamped_volume_decibels(double volume_decibels)
		{
			return clamped(volume_decibels, minimum_volume_decibels, maximum_volume_decibels);
		}

		inline double clamped_pan_percent(double pan_percent)
		{
			return clamped(pan_percent, minimum_pan_percent, maximum_pan_percent);
		}

		template <typename Element>
		void truncate_to(std::vector<Element>& elements, std::size_t maximum)
		{
			if (elements.size() > maximum)
			{
				elements.resize(maximum);
			}
		}

		inline bool contains(const std::vector<std::string>& values, const std::string& value)
		{
			for (const std::string& candidate : values)
			{
				if (candidate == value)
				{
					return true;
				}
			}

			return false;
		}
	}

	// Where a track sits, or empty when the snapshot does not hold it.
	inline std::optional<std::size_t> find_track_index(
		const routing_snapshot& snapshot,
		const std::string& track_guid)
	{
		for (std::size_t index = 0; index < snapshot.tracks_in_project_order.size(); ++index)
		{
			if (snapshot.tracks_in_project_order[index].guid == track_guid)
			{
				return index;
			}
		}

		return std::nullopt;
	}

	inline const routing_track* find_track(const routing_snapshot& snapshot, const std::string& track_guid)
	{
		const std::optional<std::size_t> index = find_track_index(snapshot, track_guid);

		return index.has_value() ? &snapshot.tracks_in_project_order[*index] : nullptr;
	}

	inline TrackReference reference_to(const routing_track& track)
	{
		return TrackReference{track.guid, track.name};
	}

	// The folder depth deltas, in the shape the Folder Invariant Keeper works in.
	inline std::vector<TrackFolderDepth> folder_depths_of(const routing_snapshot& snapshot)
	{
		std::vector<TrackFolderDepth> depths;
		depths.reserve(snapshot.tracks_in_project_order.size());

		for (const routing_track& track : snapshot.tracks_in_project_order)
		{
			depths.push_back(TrackFolderDepth{track.guid, track.folder_depth_delta});
		}

		return depths;
	}

	// ---------------------------------------------------------------------------
	// The routing graph the cycle check runs over
	// ---------------------------------------------------------------------------

	// The snapshot as `cycle_detector.h`'s graph.
	//
	// Only each track's `sends` contribute edges. Its `receives` are the same sends
	// seen from the other end, and adding both would double every edge — harmless for
	// detection, and misleading in a reported cycle.
	//
	// Folder edges are not added here. The detector derives them itself from the depth
	// deltas and the parent send flags, which is the part that must not be
	// reimplemented: a folder edge exists for a child whose parent send is on and whose
	// folder parent is derived from track order, and that derivation is the detector's.
	inline routing_graph routing_graph_from(const routing_snapshot& snapshot)
	{
		routing_graph graph;
		graph.tracks_in_project_order.reserve(snapshot.tracks_in_project_order.size());

		for (const routing_track& track : snapshot.tracks_in_project_order)
		{
			graph.tracks_in_project_order.push_back(track_node{
				track.guid,
				track.folder_depth_delta,
				track.parent_send_enabled,
			});

			for (const routing_send& send : track.sends)
			{
				graph.explicit_sends.push_back(send_edge{track.guid, send.track_guid});
			}
		}

		return graph;
	}

	// The graph as it would be with these tracks' parent sends set to `enabled`.
	//
	// This is the projection `set_parent_send` is checked against. Nothing else
	// changes: the flag is owned by the child, so switching it does not move a track or
	// touch a send.
	inline routing_graph projected_graph_with_parent_send(
		routing_graph graph,
		const std::vector<std::string>& track_guids,
		bool enabled)
	{
		for (track_node& track : graph.tracks_in_project_order)
		{
			if (routing_detail::contains(track_guids, track.identity))
			{
				track.parent_send_enabled = enabled;
			}
		}

		return graph;
	}

	// Whether a routing change expressed as a whole projected graph may proceed.
	//
	// `cycle_detector.h` offers `evaluate_send_creation` and
	// `evaluate_bus_construction`, which project their own change and then decide.
	// A parent send change has no equivalent entry point, and modelling it as a send
	// creation would misreport the mechanism — the refusal would say "-(send)->" for a
	// hop the producer cannot see in the routing window, and
	// `closes_through_folder_routing` would answer for the wrong edge. So the change is
	// projected here as a graph and the detector is asked about that graph.
	//
	// The search itself is `detect_signal_cycle`, and the refusal reason and
	// acknowledgement field are the detector's own published constants. What this adds
	// is the two-line policy the detector applies to its own decisions: refuse a loop
	// unless the producer has been shown it and said they meant it.
	//
	// Note this refuses a loop the change did not cause, if the project already held
	// one. That matches `evaluate_send_creation`, which detects any cycle in the graph
	// it projects rather than only one through the new edge, and it is the right way
	// round: adding routing to a session that already feeds back is not the moment to
	// stay quiet about it.
	inline routing_change_decision decide_on_projected_routing_graph(
		const routing_graph& projected_graph,
		bool producer_acknowledged_signal_cycle)
	{
		routing_change_decision decision;
		decision.cycle = detect_signal_cycle(projected_graph);

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

	// ---------------------------------------------------------------------------
	// Structural roles, derived once and not twice
	// ---------------------------------------------------------------------------

	// The role signals for every track in a snapshot.
	//
	// Delegates to the Project Context Builder's derivation rather than repeating it.
	// `get_routing` reports the same five roles the project context snapshot reports,
	// and deriving them a second way here is exactly how the two come to disagree about
	// whether a producer's folder parent sums — which the agent would then relay to the
	// producer as a statement about their own session.
	inline std::vector<context::StructuralRoleSignals> role_signals_of(const routing_snapshot& snapshot)
	{
		std::vector<context::TrackReading> readings;
		readings.reserve(snapshot.tracks_in_project_order.size());

		for (const routing_track& track : snapshot.tracks_in_project_order)
		{
			context::TrackReading reading;
			reading.guid = track.guid;
			reading.name = track.name;
			reading.folder_depth_delta = track.folder_depth_delta;
			reading.parent_send_enabled = track.parent_send_enabled;
			reading.item_count = track.item_count;
			reading.has_fx = track.fx_count > 0;
			reading.receive_count = static_cast<int>(track.receives.size());

			readings.push_back(std::move(reading));
		}

		return context::derive_track_role_signals(readings);
	}

	inline std::vector<context::StructuralRole> roles_of(const routing_snapshot& snapshot)
	{
		const std::vector<context::StructuralRoleSignals> signals = role_signals_of(snapshot);

		std::vector<context::StructuralRole> roles;
		roles.reserve(signals.size());

		for (const context::StructuralRoleSignals& track_signals : signals)
		{
			roles.push_back(context::derive_structural_role(track_signals).role);
		}

		return roles;
	}

	// The folder parents whose children all route around them and which carry nothing
	// of their own — the shape that looks like a summing bus and sums nothing.
	//
	// Named on `set_parent_send`'s result rather than discovered at render time, which
	// is what the schema's `silencedFolderParents` is for. This is the
	// `silent_folder_parent` role, so the condition is not restated here.
	inline std::vector<TrackReference> find_silenced_folder_parents(const routing_snapshot& snapshot)
	{
		const std::vector<context::StructuralRole> roles = roles_of(snapshot);

		std::vector<TrackReference> silenced;

		for (std::size_t index = 0; index < roles.size() && index < snapshot.tracks_in_project_order.size();
			++index)
		{
			if (roles[index] == context::StructuralRole::silent_folder_parent)
			{
				silenced.push_back(reference_to(snapshot.tracks_in_project_order[index]));
			}
		}

		routing_detail::truncate_to(silenced, maximum_reported_parent_send_changes);

		return silenced;
	}

	// ---------------------------------------------------------------------------
	// Results
	// ---------------------------------------------------------------------------

	// `folderDepthRepair` is `tools/shared_tool_definitions.h`'s — routing and track
	// structure both report the Folder Invariant Keeper's repairs in the same shape, and
	// that header records why there is one definition rather than two.

	struct create_send_result
	{
		TrackReference source_track;
		TrackReference destination_track;
		int send_index = 0;
		double volume_decibels = 0.0;
		double pan_percent = 0.0;
		routing_send_mode mode = routing_send_mode::post_fader;
	};

	struct remove_send_result
	{
		TrackReference source_track;
		TrackReference destination_track;
		int send_index = 0;
		int remaining_send_count = 0;
	};

	struct set_send_state_result
	{
		TrackReference source_track;
		TrackReference destination_track;
		routing_send send;
	};

	// `parentSendChange`. Names what the flag routes to for that track, because the
	// target is context-dependent: the folder parent inside a folder, the master track
	// at top level.
	struct parent_send_change
	{
		TrackReference track;
		bool previous_enabled = true;
		bool targets_folder_parent = false;
		std::string target_guid;
	};

	struct set_parent_send_result
	{
		std::vector<parent_send_change> tracks;
		bool enabled = true;
		std::vector<TrackReference> silenced_folder_parents;
	};

	// One send created from a source track into a new aux bus. The index is on the
	// source, not the bus.
	struct created_send
	{
		std::string source_track_guid;
		int send_index = 0;
		double volume_decibels = 0.0;
		routing_send_mode mode = routing_send_mode::post_fader;
	};

	struct create_bus_result
	{
		TrackReference bus_track;
		bus_type type = bus_type::summing;
		int index = 0;
		std::vector<TrackReference> source_tracks;
		std::vector<routing_fx_placement> fx;

		// One per source for an aux bus; empty for a summing bus, which carries signal
		// through folder summing and so appears in no send enumeration at all.
		std::vector<created_send> sends;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	struct remove_bus_result
	{
		TrackReference removed_bus;
		bus_type type = bus_type::summing;
		int removed_fx_count = 0;

		// The former children that rose out of a dissolved summing folder. Empty for an
		// aux bus, whose sources were never parented to it.
		std::vector<TrackReference> reparented_tracks;

		// The source tracks whose sends fed an aux bus and went with it. Empty for a
		// summing bus.
		std::vector<TrackReference> removed_sends;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	// One track's routing as `get_routing` reports it.
	struct track_routing_report
	{
		TrackReference track;
		context::StructuralRole role = context::StructuralRole::normal;

		// The raw `I_FOLDERDEPTH` delta.
		int folder_depth_delta = 0;

		// Nesting level, accumulated across track order. Zero is top level. Derived,
		// because REAPER does not store it.
		int accumulated_folder_depth = 0;

		// Empty for a track at top level.
		std::optional<std::string> folder_parent_guid;

		bool parent_send_enabled = true;

		std::vector<routing_send> sends;
		std::vector<routing_send> receives;

		int item_count = 0;
		int fx_count = 0;
	};

	struct master_routing_report
	{
		bool present = false;
		TrackReference track;
		context::StructuralRole role = context::StructuralRole::master;
		std::vector<routing_send> receives;
		int fx_count = 0;
	};

	struct get_routing_result
	{
		std::vector<track_routing_report> tracks;
		master_routing_report master_track;
	};

	// ---------------------------------------------------------------------------
	// Requests — the parts of an already-validated input these tools read
	// ---------------------------------------------------------------------------

	struct create_send_request
	{
		double volume_decibels = 0.0;
		double pan_percent = 0.0;
		routing_send_mode mode = routing_send_mode::post_fader;
	};

	struct remove_send_request
	{
		int send_index = 0;
	};

	struct set_send_state_request
	{
		int send_index = 0;
		send_state_change change;
	};

	struct set_parent_send_request
	{
		bool enabled = true;
	};

	struct bus_construction_request
	{
		// Which of the two things to build. The producer said "bus"; the agent inferred
		// the kind, and that inference is a documented heuristic rather than a fact,
		// which is why it arrives as an explicit parameter.
		bus_type type = bus_type::summing;

		std::string bus_name;

		// FX to place on the bus, in chain order. Usually the point of an aux bus.
		std::vector<std::string> fx_names;

		// Applied to each send into an aux bus. Ignored for a summing bus, which
		// creates no sends.
		double send_volume_decibels = 0.0;
		routing_send_mode send_mode = routing_send_mode::post_fader;
	};

	// ---------------------------------------------------------------------------
	// Planning a summing bus — folder arithmetic, which belongs to the Keeper
	// ---------------------------------------------------------------------------

	// What building a summing bus does to track order and to the depth deltas.
	//
	// A folder parent is a position, not a track type: it must precede its children and
	// the last child must carry the closing delta. So the sources are gathered directly
	// beneath the new track, and the deltas that result are whatever
	// `repair_folder_depth_deltas` makes of that — the two structural edits this plan
	// makes by hand are the bus opening a folder and the last source closing it.
	// Everything else is the Keeper's.
	struct summing_bus_layout
	{
		// Where the bus track is inserted: immediately above the first source, since a
		// folder parent must precede its children.
		int insert_index = 0;

		// The project's track order afterwards, holding `planned_bus_track_identity`
		// where the bus goes until the real GUID is known.
		std::vector<std::string> track_order;

		// The repaired deltas, in that order.
		std::vector<TrackFolderDepth> depths_after;

		// Every track other than the bus whose delta changed. Reported so a structural
		// side effect is something the agent can tell the producer about rather than
		// something they find later.
		std::vector<folder_depth_repair> repairs;

		bool track_order_changed = false;
	};

	inline summing_bus_layout plan_summing_bus_layout(
		const routing_snapshot& snapshot,
		const std::vector<std::string>& source_track_guids,
		const std::string& bus_track_identity)
	{
		summing_bus_layout layout;

		const std::vector<routing_track>& tracks = snapshot.tracks_in_project_order;

		// Sources in the order the call supplied them, ignoring anything the snapshot
		// does not hold and any repeat.
		std::vector<std::string> sources;

		for (const std::string& source_track_guid : source_track_guids)
		{
			if (!routing_detail::contains(sources, source_track_guid)
				&& find_track_index(snapshot, source_track_guid).has_value())
			{
				sources.push_back(source_track_guid);
			}
		}

		std::size_t insert_index = tracks.size();

		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (routing_detail::contains(sources, tracks[index].guid))
			{
				insert_index = index;
				break;
			}
		}

		layout.insert_index = static_cast<int>(insert_index);

		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (index == insert_index)
			{
				layout.track_order.push_back(bus_track_identity);

				for (const std::string& source_track_guid : sources)
				{
					layout.track_order.push_back(source_track_guid);
				}
			}

			if (routing_detail::contains(sources, tracks[index].guid))
			{
				continue;
			}

			layout.track_order.push_back(tracks[index].guid);
		}

		// No source the snapshot holds: the bus lands at the end as a plain track, and
		// the call's own failure to name anything bussable is reported by the handler.
		if (insert_index >= tracks.size())
		{
			layout.track_order.push_back(bus_track_identity);
		}

		std::vector<TrackFolderDepth> depths;
		depths.reserve(layout.track_order.size());

		for (const std::string& identity : layout.track_order)
		{
			if (identity == bus_track_identity)
			{
				depths.push_back(TrackFolderDepth{identity, 0});
				continue;
			}

			const routing_track* const track = find_track(snapshot, identity);
			depths.push_back(TrackFolderDepth{identity, track == nullptr ? 0 : track->folder_depth_delta});
		}

		if (!sources.empty())
		{
			for (TrackFolderDepth& depth : depths)
			{
				if (depth.identity == bus_track_identity)
				{
					depth.folder_depth_delta = maximum_folder_depth_delta;
					break;
				}
			}

			// The last source closes the folder the bus opened, on top of whatever it
			// already closed — a source that was the last member of its own folder
			// carries that closing delta with it.
			for (TrackFolderDepth& depth : depths)
			{
				if (depth.identity == sources.back())
				{
					--depth.folder_depth_delta;
					break;
				}
			}
		}

		layout.depths_after = repair_folder_depth_deltas(std::move(depths));

		for (const TrackFolderDepth& depth : layout.depths_after)
		{
			if (depth.identity == bus_track_identity)
			{
				continue;
			}

			const routing_track* const track = find_track(snapshot, depth.identity);

			if (track == nullptr || track->folder_depth_delta == depth.folder_depth_delta)
			{
				continue;
			}

			layout.repairs.push_back(folder_depth_repair{
				track->guid,
				track->name,
				track->folder_depth_delta,
				depth.folder_depth_delta,
			});
		}

		routing_detail::truncate_to(layout.repairs, maximum_reported_tracks);

		// The order REAPER has after a plain insertion at `insert_index`, which is what
		// the desired order is compared against.
		std::vector<std::string> order_after_insertion;
		order_after_insertion.reserve(layout.track_order.size());

		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (index == insert_index)
			{
				order_after_insertion.push_back(bus_track_identity);
			}

			order_after_insertion.push_back(tracks[index].guid);
		}

		if (insert_index >= tracks.size())
		{
			order_after_insertion.push_back(bus_track_identity);
		}

		layout.track_order_changed = order_after_insertion != layout.track_order;

		return layout;
	}

	// What building a bus does to routing, in the form the cycle check needs.
	//
	// Stated as who ends up feeding whom rather than as track moves, which is what
	// `bus_construction_plan` asks for and why the folder arithmetic above is not
	// repeated inside it.
	inline bus_construction_plan plan_bus_construction(
		const routing_snapshot& snapshot,
		const bus_construction_request& request,
		const std::vector<std::string>& source_track_guids,
		const std::string& bus_track_identity)
	{
		bus_construction_plan plan;
		plan.type = request.type;
		plan.bus_track = bus_track_identity;
		plan.source_tracks = source_track_guids;
		plan.bus_parent_send_enabled = true;
		plan.source_parent_send_enabled = true;

		if (request.type != bus_type::summing)
		{
			// An aux return is inserted at top level and re-parents nothing, so its own
			// output reaches the master and contributes no edge.
			return plan;
		}

		// A summing bus takes the sources' former folder parent, so the submix keeps
		// arriving where the sources used to arrive. Sources with differing parents have
		// no single answer, and the first source's parent is the one the layout above
		// actually produces — the bus is inserted at its position.
		const std::vector<TrackFolderDepth> depths = folder_depths_of(snapshot);
		const std::vector<std::vector<std::size_t>> folder_paths = reconstruct_folder_paths(depths);

		for (const std::string& source_track_guid : source_track_guids)
		{
			const std::optional<std::size_t> source_index = find_track_index(snapshot, source_track_guid);

			if (!source_index.has_value())
			{
				continue;
			}

			if (!folder_paths[*source_index].empty())
			{
				plan.bus_folder_parent =
					snapshot.tracks_in_project_order[folder_paths[*source_index].back()].guid;
			}

			break;
		}

		return plan;
	}

	// ---------------------------------------------------------------------------
	// Dissolving a summing bus — requirement 11.3, the Keeper's business
	// ---------------------------------------------------------------------------

	struct summing_bus_dissolution
	{
		// The repaired deltas for the surviving tracks, in project order.
		std::vector<TrackFolderDepth> depths_after;

		// The former direct children, in track order. They now feed whatever the bus
		// fed, without its FX or fader in the path.
		std::vector<TrackReference> reparented_tracks;

		std::vector<folder_depth_repair> repairs;
	};

	// The children rise to the bus's own depth, and no child is promoted into its
	// folder role — which is the Keeper's `delete_track_repairing_folders`, not a
	// second implementation of it here. Carrying the opening delta onto the first child
	// instead would silently re-parent all of its siblings underneath it, which is the
	// mistake requirement 11.3 names explicitly.
	inline summing_bus_dissolution plan_summing_bus_dissolution(
		const routing_snapshot& snapshot,
		const std::string& bus_track_guid)
	{
		summing_bus_dissolution dissolution;

		const std::optional<std::size_t> bus_index = find_track_index(snapshot, bus_track_guid);

		if (!bus_index.has_value())
		{
			dissolution.depths_after = folder_depths_of(snapshot);

			return dissolution;
		}

		const std::vector<TrackFolderDepth> depths_before = folder_depths_of(snapshot);
		const std::vector<std::vector<std::size_t>> folder_paths = reconstruct_folder_paths(depths_before);

		for (std::size_t index = 0; index < folder_paths.size(); ++index)
		{
			if (!folder_paths[index].empty() && folder_paths[index].back() == *bus_index)
			{
				dissolution.reparented_tracks.push_back(
					reference_to(snapshot.tracks_in_project_order[index]));
			}
		}

		routing_detail::truncate_to(dissolution.reparented_tracks, maximum_reported_tracks);

		dissolution.depths_after = delete_track_repairing_folders(depths_before, *bus_index);

		for (const TrackFolderDepth& depth : dissolution.depths_after)
		{
			const routing_track* const track = find_track(snapshot, depth.identity);

			if (track == nullptr || track->folder_depth_delta == depth.folder_depth_delta)
			{
				continue;
			}

			dissolution.repairs.push_back(folder_depth_repair{
				track->guid,
				track->name,
				track->folder_depth_delta,
				depth.folder_depth_delta,
			});
		}

		routing_detail::truncate_to(dissolution.repairs, maximum_reported_tracks);

		return dissolution;
	}

	// Which mechanism a track being removed turns out to be. `remove_bus` is given a
	// track rather than a type, so this is the extension's reading of what it found,
	// derived from the structural role. Empty for a track that is neither — a plain
	// track is not a bus, and deleting it because the agent called it one would be the
	// tool doing something nobody asked for.
	inline std::optional<bus_type> classify_bus(const routing_snapshot& snapshot, const std::string& track_guid)
	{
		const std::optional<std::size_t> index = find_track_index(snapshot, track_guid);

		if (!index.has_value())
		{
			return std::nullopt;
		}

		const std::vector<context::StructuralRole> roles = roles_of(snapshot);

		if (*index >= roles.size())
		{
			return std::nullopt;
		}

		switch (roles[*index])
		{
			case context::StructuralRole::summing_folder_parent:
			case context::StructuralRole::silent_folder_parent:
				return bus_type::summing;
			case context::StructuralRole::aux_return:
				return bus_type::aux;
			case context::StructuralRole::normal:
			case context::StructuralRole::master:
				return std::nullopt;
		}

		return std::nullopt;
	}

	// ---------------------------------------------------------------------------
	// get_routing
	// ---------------------------------------------------------------------------

	// Both summing mechanisms in one result.
	//
	// `parentSendEnabled` and the folder fields are not decoration beside the sends:
	// folder summing is invisible to send enumeration, so a result carrying sends alone
	// would report a fully routed session as unrouted.
	inline get_routing_result build_get_routing_result(
		const routing_snapshot& snapshot,
		const std::vector<std::string>& requested_track_guids)
	{
		get_routing_result result;

		const std::vector<TrackFolderDepth> depths = folder_depths_of(snapshot);
		const std::vector<std::vector<std::size_t>> folder_paths = reconstruct_folder_paths(depths);
		const std::vector<context::StructuralRoleSignals> signals = role_signals_of(snapshot);

		for (std::size_t index = 0; index < snapshot.tracks_in_project_order.size(); ++index)
		{
			const routing_track& track = snapshot.tracks_in_project_order[index];

			// No selectors means the whole project, which is how a stem set is checked
			// against what actually reaches the mix.
			if (!requested_track_guids.empty() && !routing_detail::contains(requested_track_guids, track.guid))
			{
				continue;
			}

			track_routing_report report;
			report.track = reference_to(track);
			report.role = context::derive_structural_role(signals[index]).role;
			report.folder_depth_delta = track.folder_depth_delta;
			report.accumulated_folder_depth = static_cast<int>(folder_paths[index].size());

			if (!folder_paths[index].empty())
			{
				report.folder_parent_guid =
					snapshot.tracks_in_project_order[folder_paths[index].back()].guid;
			}

			report.parent_send_enabled = track.parent_send_enabled;
			report.sends = track.sends;
			report.receives = track.receives;
			report.item_count = track.item_count;
			report.fx_count = track.fx_count;

			routing_detail::truncate_to(report.sends, maximum_reported_sends_per_track);
			routing_detail::truncate_to(report.receives, maximum_reported_sends_per_track);

			result.tracks.push_back(std::move(report));
		}

		routing_detail::truncate_to(result.tracks, maximum_reported_tracks);

		// Present when the call read the whole project. The master's receives do not
		// include the tracks that reach it through their parent send, which is how most
		// signal arrives — those are found through `parentSendEnabled` on the tracks
		// themselves, which is why both are in this result.
		if (requested_track_guids.empty() && snapshot.master_track.present)
		{
			context::MasterTrackReading master_reading;
			master_reading.present = true;
			master_reading.guid = snapshot.master_track.guid;
			master_reading.name = snapshot.master_track.name;
			master_reading.has_fx = snapshot.master_track.fx_count > 0;
			master_reading.receive_count = static_cast<int>(snapshot.master_track.receives.size());

			result.master_track.present = true;
			result.master_track.track =
				TrackReference{snapshot.master_track.guid, snapshot.master_track.name};
			result.master_track.role =
				context::derive_structural_role(context::derive_master_track_role_signals(master_reading))
					.role;
			result.master_track.receives = snapshot.master_track.receives;
			result.master_track.fx_count = snapshot.master_track.fx_count;

			routing_detail::truncate_to(result.master_track.receives, maximum_reported_sends_per_track);
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// The payload seam
	// ---------------------------------------------------------------------------

	// How a request is read out of an already-validated input, and how a result is
	// written into the payload type.
	//
	// Every member may be empty. An empty reader is a failed action carrying a reason
	// rather than a default-constructed request: a `create_bus` call whose type nobody
	// extracted would otherwise build a folder because `bus_type` happens to default to
	// summing, which is a different session than the producer asked for.
	template <typename JsonValue>
	struct routing_payload_codec
	{
		std::function<create_send_request(const JsonValue&)> read_create_send_request;
		std::function<remove_send_request(const JsonValue&)> read_remove_send_request;
		std::function<set_send_state_request(const JsonValue&)> read_set_send_state_request;
		std::function<set_parent_send_request(const JsonValue&)> read_set_parent_send_request;
		std::function<bus_construction_request(const JsonValue&)> read_create_bus_request;

		std::function<JsonValue(const create_send_result&)> write_create_send_result;
		std::function<JsonValue(const remove_send_result&)> write_remove_send_result;
		std::function<JsonValue(const set_send_state_result&)> write_set_send_state_result;
		std::function<JsonValue(const set_parent_send_result&)> write_set_parent_send_result;
		std::function<JsonValue(const create_bus_result&)> write_create_bus_result;
		std::function<JsonValue(const remove_bus_result&)> write_remove_bus_result;
		std::function<JsonValue(const get_routing_result&)> write_get_routing_result;
	};

	namespace routing_detail
	{
		inline handler_action_outcomes one_failed_action(
			std::string_view target,
			std::string_view failure_code,
			std::string_view failure_message)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, failure_code, failure_message));

			return outcomes;
		}

		// The request, or the failure that says why there is none.
		template <typename JsonValue, typename Request>
		std::optional<Request> read_request(
			const tool_execution_context<JsonValue>& context,
			const std::function<Request(const JsonValue&)>& reader)
		{
			if (!reader || context.call.validated_input == nullptr)
			{
				return std::nullopt;
			}

			return reader(*context.call.validated_input);
		}

		inline std::vector<std::string> resolved_track_guids(const std::vector<ResolvedTrack>& targets)
		{
			std::vector<std::string> guids;
			guids.reserve(targets.size());

			for (const ResolvedTrack& target : targets)
			{
				guids.push_back(target.reference.guid);
			}

			return guids;
		}

		// Whether every action in a set landed. A handler returns its own result payload
		// only when it did; otherwise the per-action outcomes are the result, because a
		// payload claiming the tool succeeded is the worse of the two reports.
		inline bool every_action_applied(const std::vector<action_outcome>& actions)
		{
			return count_failed_actions(actions) == 0;
		}
	}

	// ---------------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------------

	// What `register_routing_tools` did. Reported rather than asserted, so a name the
	// protocol does not know or a handler registered twice is something the extension
	// can say at startup rather than discover one unknown-tool error at a time.
	struct routing_tool_registration
	{
		std::vector<std::string> registered_tool_names;
		std::vector<std::string> rejected_tool_names;

		bool every_tool_registered() const { return rejected_tool_names.empty(); }
	};

	// Registers all seven through `tool_executor.h`'s existing seam.
	//
	// The choice of registration call per tool is the choice of whether a refusal is
	// possible and whether an undo block is opened, so it is the part worth reading:
	//
	//   - `create_send`, `create_bus`, `set_parent_send` — mutating, with a cycle
	//     precondition. The precondition runs before any block opens and is the only
	//     place they may refuse (requirements 9.3, 9.7, 10.7).
	//   - `remove_send`, `set_send_state`, `remove_bus` — mutating, no precondition.
	//     None of them can create an edge, so none of them can close a loop.
	//   - `get_routing` — a read tool. No block, no marker (requirement 10.3).
	//
	// `host` and the codec's callables must outlive the registry.
	template <typename JsonValue>
	routing_tool_registration register_routing_tools(
		tool_handler_registry<JsonValue>& registry,
		routing_host& host,
		routing_payload_codec<JsonValue> codec)
	{
		routing_tool_registration registration;

		routing_host* const routing = &host;

		const auto record = [&registration](std::string_view tool_name, tool_registration_outcome outcome) {
			if (outcome == tool_registration_outcome::registered)
			{
				registration.registered_tool_names.emplace_back(tool_name);
				return;
			}

			registration.rejected_tool_names.emplace_back(tool_name);
		};

		// ------------------------------------------------------------------
		// create_send
		// ------------------------------------------------------------------

		record(
			"create_send",
			registry.register_mutating_tool(
				"create_send",
				[routing, write_result = codec.write_create_send_result,
					read_request = codec.read_create_send_request](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					if (context.resolved_targets.size() < 2)
					{
						return routing_detail::one_failed_action(
							"create_send",
							missing_routing_target_code,
							"create_send needs both a source track and a destination track");
					}

					const std::optional<create_send_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !write_result)
					{
						return routing_detail::one_failed_action(
							"create_send",
							missing_tool_input_code,
							"the send's level, pan, and mode could not be read from the call");
					}

					const TrackReference& source = context.resolved_targets[0].reference;
					const TrackReference& destination = context.resolved_targets[1].reference;

					const std::optional<int> send_index = routing->create_send(source.guid, destination.guid);

					if (!send_index.has_value())
					{
						return routing_detail::one_failed_action(
							source.guid,
							reaper_routing_failure_code,
							"REAPER would not create a send from \"" + source.name + "\" to \""
								+ destination.name + "\"");
					}

					send_state_change change;
					change.volume_decibels = routing_detail::clamped_volume_decibels(request->volume_decibels);
					change.pan_percent = routing_detail::clamped_pan_percent(request->pan_percent);
					change.mode = request->mode;

					routing->write_send_state(source.guid, *send_index, change);

					create_send_result result;
					result.source_track = source;
					result.destination_track = destination;
					result.send_index = *send_index;
					result.volume_decibels = *change.volume_decibels;
					result.pan_percent = *change.pan_percent;
					result.mode = request->mode;

					// What the send actually is now, where REAPER will say. A send it
					// clamped or refused a mode on is reported as it stands rather than
					// as it was asked for.
					const std::optional<routing_send> written = routing->read_send(source.guid, *send_index);

					if (written.has_value())
					{
						result.volume_decibels =
							routing_detail::clamped_volume_decibels(written->volume_decibels);
						result.pan_percent = routing_detail::clamped_pan_percent(written->pan_percent);
						result.mode = written->mode;
					}

					return handler_success<JsonValue>{write_result(result)};
				},
				[routing](const tool_execution_context<JsonValue>& context)
					-> std::optional<tool_refusal> {
					// Requirement 9.7. Asked about the graph as it would be after the
					// send exists, not about the graph as it is — a detector asked about
					// the present state never refuses anything.
					if (context.resolved_targets.size() < 2)
					{
						// Not a refusal: an incomplete call names no acknowledgement the
						// producer could confirm, so the body reports it as a failed
						// action carrying a reason.
						return std::nullopt;
					}

					const routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return std::nullopt;
					}

					return refusal_from_routing_decision(evaluate_send_creation(
						routing_graph_from(snapshot),
						context.resolved_targets[0].reference.guid,
						context.resolved_targets[1].reference.guid,
						context.call.confirmed_signal_cycle));
				}));

		// ------------------------------------------------------------------
		// remove_send
		// ------------------------------------------------------------------

		record(
			"remove_send",
			registry.register_mutating_tool(
				"remove_send",
				[routing, write_result = codec.write_remove_send_result,
					read_request = codec.read_remove_send_request](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					if (context.resolved_targets.empty())
					{
						return routing_detail::one_failed_action(
							"remove_send",
							missing_routing_target_code,
							"remove_send needs the source track the send sits on");
					}

					const std::optional<remove_send_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !write_result)
					{
						return routing_detail::one_failed_action(
							"remove_send",
							missing_tool_input_code,
							"the send index could not be read from the call");
					}

					const TrackReference& source = context.resolved_targets[0].reference;

					const routing_snapshot snapshot = routing->read_routing_snapshot();
					const routing_track* const source_track = find_track(snapshot, source.guid);

					if (!snapshot.readable || source_track == nullptr)
					{
						return routing_detail::one_failed_action(
							source.guid,
							unknown_routing_target_code,
							"the source track's routing could not be read");
					}

					if (request->send_index < 0
						|| static_cast<std::size_t>(request->send_index) >= source_track->sends.size())
					{
						return routing_detail::one_failed_action(
							source.guid,
							invalid_send_index_code,
							"send index " + std::to_string(request->send_index) + " does not address a send on \""
								+ source_track->name + "\", which holds "
								+ std::to_string(source_track->sends.size()));
					}

					const routing_send& send =
						source_track->sends[static_cast<std::size_t>(request->send_index)];

					if (!routing->remove_send(source.guid, request->send_index))
					{
						return routing_detail::one_failed_action(
							source.guid,
							reaper_routing_failure_code,
							"REAPER would not remove send " + std::to_string(request->send_index) + " from \""
								+ source_track->name + "\"");
					}

					remove_send_result result;
					result.source_track = source;
					result.destination_track = TrackReference{send.track_guid, send.track_name};
					result.send_index = request->send_index;
					result.remaining_send_count = static_cast<int>(source_track->sends.size()) - 1;

					return handler_success<JsonValue>{write_result(result)};
				}));

		// ------------------------------------------------------------------
		// set_send_state
		// ------------------------------------------------------------------

		record(
			"set_send_state",
			registry.register_mutating_tool(
				"set_send_state",
				[routing, write_result = codec.write_set_send_state_result,
					read_request = codec.read_set_send_state_request](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					if (context.resolved_targets.empty())
					{
						return routing_detail::one_failed_action(
							"set_send_state",
							missing_routing_target_code,
							"set_send_state needs the source track the send sits on");
					}

					const std::optional<set_send_state_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !write_result)
					{
						return routing_detail::one_failed_action(
							"set_send_state",
							missing_tool_input_code,
							"the send index and the properties to change could not be read from the call");
					}

					const TrackReference& source = context.resolved_targets[0].reference;

					const routing_snapshot snapshot = routing->read_routing_snapshot();
					const routing_track* const source_track = find_track(snapshot, source.guid);

					if (!snapshot.readable || source_track == nullptr)
					{
						return routing_detail::one_failed_action(
							source.guid,
							unknown_routing_target_code,
							"the source track's routing could not be read");
					}

					if (request->send_index < 0
						|| static_cast<std::size_t>(request->send_index) >= source_track->sends.size())
					{
						return routing_detail::one_failed_action(
							source.guid,
							invalid_send_index_code,
							"send index " + std::to_string(request->send_index) + " does not address a send on \""
								+ source_track->name + "\", which holds "
								+ std::to_string(source_track->sends.size()));
					}

					const routing_send& send =
						source_track->sends[static_cast<std::size_t>(request->send_index)];

					send_state_change change = request->change;

					if (change.volume_decibels.has_value())
					{
						change.volume_decibels =
							routing_detail::clamped_volume_decibels(*change.volume_decibels);
					}

					if (change.pan_percent.has_value())
					{
						change.pan_percent = routing_detail::clamped_pan_percent(*change.pan_percent);
					}

					if (!routing->write_send_state(source.guid, request->send_index, change))
					{
						return routing_detail::one_failed_action(
							source.guid,
							reaper_routing_failure_code,
							"REAPER would not change send " + std::to_string(request->send_index) + " on \""
								+ source_track->name + "\"");
					}

					set_send_state_result result;
					result.source_track = source;
					result.destination_track = TrackReference{send.track_guid, send.track_name};

					const std::optional<routing_send> written =
						routing->read_send(source.guid, request->send_index);

					// The send as REAPER now holds it, falling back to the send that was
					// read plus what was asked of it — a result has to describe the send,
					// and the request is a closer description of it than the pre-change
					// reading alone.
					result.send = written.value_or(send);

					if (!written.has_value())
					{
						result.send.volume_decibels =
							change.volume_decibels.value_or(result.send.volume_decibels);
						result.send.pan_percent = change.pan_percent.value_or(result.send.pan_percent);
						result.send.muted = change.muted.value_or(result.send.muted);
						result.send.mode = change.mode.value_or(result.send.mode);
					}

					result.send.volume_decibels =
						routing_detail::clamped_volume_decibels(result.send.volume_decibels);
					result.send.pan_percent = routing_detail::clamped_pan_percent(result.send.pan_percent);

					return handler_success<JsonValue>{write_result(result)};
				}));

		// ------------------------------------------------------------------
		// set_parent_send
		// ------------------------------------------------------------------

		record(
			"set_parent_send",
			registry.register_mutating_tool(
				"set_parent_send",
				[routing, write_result = codec.write_set_parent_send_result,
					read_request = codec.read_set_parent_send_request](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					if (context.resolved_targets.empty())
					{
						return routing_detail::one_failed_action(
							"set_parent_send",
							missing_routing_target_code,
							"set_parent_send needs at least one track");
					}

					const std::optional<set_parent_send_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !write_result)
					{
						return routing_detail::one_failed_action(
							"set_parent_send",
							missing_tool_input_code,
							"the state to apply could not be read from the call");
					}

					routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return routing_detail::one_failed_action(
							"set_parent_send",
							unknown_routing_target_code,
							"the project's routing could not be read");
					}

					const std::vector<TrackFolderDepth> depths = folder_depths_of(snapshot);
					const std::vector<std::vector<std::size_t>> folder_paths =
						reconstruct_folder_paths(depths);

					std::vector<action_outcome> actions;
					set_parent_send_result result;
					result.enabled = request->enabled;

					for (const ResolvedTrack& target : context.resolved_targets)
					{
						const std::optional<std::size_t> track_index =
							find_track_index(snapshot, target.reference.guid);

						if (!track_index.has_value())
						{
							actions.push_back(failed_action(
								target.reference.guid,
								unknown_routing_target_code,
								"\"" + target.reference.name + "\" is not in the project's track list"));

							continue;
						}

						const routing_track& track = snapshot.tracks_in_project_order[*track_index];

						if (!routing->write_parent_send(track.guid, request->enabled))
						{
							actions.push_back(failed_action(
								track.guid,
								reaper_routing_failure_code,
								"REAPER would not change the parent send on \"" + track.name + "\""));

							continue;
						}

						parent_send_change change;
						change.track = reference_to(track);
						change.previous_enabled = track.parent_send_enabled;

						// The target is context-dependent: the enclosing folder parent
						// inside a folder, the master track at top level.
						if (!folder_paths[*track_index].empty())
						{
							change.targets_folder_parent = true;
							change.target_guid =
								snapshot.tracks_in_project_order[folder_paths[*track_index].back()].guid;
						}
						else
						{
							change.targets_folder_parent = false;
							change.target_guid = snapshot.master_track.guid;
						}

						result.tracks.push_back(std::move(change));

						// The flag as it now stands, so the silenced-folder-parent reading
						// below is taken from what was actually written rather than from
						// what was read before the call.
						snapshot.tracks_in_project_order[*track_index].parent_send_enabled =
							request->enabled;

						actions.push_back(succeeded_action(track.guid));
					}

					if (!routing_detail::every_action_applied(actions))
					{
						// Requirement 9.4 and 9.5: the successes stand, every failure
						// carries a reason, and the tool's own payload is not offered for a
						// call that only partly landed.
						return handler_action_outcomes{std::move(actions)};
					}

					routing_detail::truncate_to(result.tracks, maximum_reported_parent_send_changes);

					result.silenced_folder_parents = find_silenced_folder_parents(snapshot);

					return handler_success<JsonValue>{write_result(result)};
				},
				[routing, read_request = codec.read_set_parent_send_request](
					const tool_execution_context<JsonValue>& context) -> std::optional<tool_refusal> {
					// Requirement 9.7, and the folder-closed half of it. Switching a
					// parent send on adds an edge nothing in the sends list will ever
					// show, so this is one of the two places a loop appears without an
					// explicit send being involved.
					const std::optional<set_parent_send_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !request->enabled)
					{
						// Switching a parent send off removes an edge. Nothing that
						// removes an edge can close a loop.
						return std::nullopt;
					}

					const routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return std::nullopt;
					}

					return refusal_from_routing_decision(decide_on_projected_routing_graph(
						projected_graph_with_parent_send(
							routing_graph_from(snapshot),
							routing_detail::resolved_track_guids(context.resolved_targets),
							true),
						context.call.confirmed_signal_cycle));
				}));

		// ------------------------------------------------------------------
		// create_bus
		// ------------------------------------------------------------------

		record(
			"create_bus",
			registry.register_mutating_tool(
				"create_bus",
				[routing, write_result = codec.write_create_bus_result,
					read_request = codec.read_create_bus_request](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					const std::optional<bus_construction_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || !write_result)
					{
						return routing_detail::one_failed_action(
							"create_bus",
							missing_tool_input_code,
							"the kind of bus to build could not be read from the call");
					}

					if (context.resolved_targets.empty())
					{
						return routing_detail::one_failed_action(
							"create_bus",
							missing_routing_target_code,
							"create_bus needs at least one source track to bus");
					}

					const routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return routing_detail::one_failed_action(
							"create_bus",
							unknown_routing_target_code,
							"the project's routing could not be read");
					}

					const std::vector<std::string> source_guids =
						routing_detail::resolved_track_guids(context.resolved_targets);

					create_bus_result result;
					result.type = request->type;

					for (const ResolvedTrack& target : context.resolved_targets)
					{
						result.source_tracks.push_back(target.reference);
					}

					routing_detail::truncate_to(result.source_tracks, maximum_reported_sends);

					std::vector<action_outcome> actions;

					// A summing bus is a folder, so it must precede its children; an aux
					// bus is a return track, which goes at the end of the track list where
					// it disturbs no existing structure.
					const summing_bus_layout layout = request->type == bus_type::summing
						? plan_summing_bus_layout(
							snapshot, source_guids, std::string{planned_bus_track_identity})
						: summing_bus_layout{};

					const int insert_index = request->type == bus_type::summing
						? layout.insert_index
						: static_cast<int>(snapshot.tracks_in_project_order.size());

					const std::optional<TrackReference> bus_track =
						routing->insert_track(insert_index, request->bus_name);

					if (!bus_track.has_value())
					{
						return routing_detail::one_failed_action(
							"create_bus",
							reaper_routing_failure_code,
							"REAPER would not insert a track for the bus");
					}

					result.bus_track = *bus_track;
					result.index = insert_index;
					actions.push_back(succeeded_action(bus_track->guid));

					if (request->type == bus_type::summing)
					{
						// The structural half. The deltas written are the Folder Invariant
						// Keeper's, not this handler's arithmetic — requirement 11.1.
						if (layout.track_order_changed)
						{
							std::vector<std::string> order = layout.track_order;

							for (std::string& identity : order)
							{
								if (identity == planned_bus_track_identity)
								{
									identity = bus_track->guid;
								}
							}

							if (!routing->apply_track_order(order))
							{
								actions.push_back(failed_action(
									bus_track->guid,
									reaper_routing_failure_code,
									"REAPER would not gather the source tracks beneath the summing bus"));
							}
						}

						for (const TrackFolderDepth& depth : layout.depths_after)
						{
							const std::string identity = depth.identity == planned_bus_track_identity
								? bus_track->guid
								: depth.identity;

							const routing_track* const existing = find_track(snapshot, depth.identity);

							// A delta that did not change needs no write. The bus's own is
							// always written, since the track was created flat.
							if (existing != nullptr
								&& existing->folder_depth_delta == depth.folder_depth_delta)
							{
								continue;
							}

							if (!routing->write_folder_depth(identity, depth.folder_depth_delta))
							{
								actions.push_back(failed_action(
									identity,
									reaper_routing_failure_code,
									"REAPER would not write the folder depth the structure needs"));
							}
						}

						result.folder_depth_repairs = layout.repairs;
					}
					else
					{
						// The routing half. An aux bus leaves the sources going where they
						// already went and adds one send each.
						for (const ResolvedTrack& target : context.resolved_targets)
						{
							const std::optional<int> send_index =
								routing->create_send(target.reference.guid, bus_track->guid);

							if (!send_index.has_value())
							{
								actions.push_back(failed_action(
									target.reference.guid,
									reaper_routing_failure_code,
									"REAPER would not create a send from \"" + target.reference.name
										+ "\" into the aux bus"));

								continue;
							}

							send_state_change change;
							change.volume_decibels =
								routing_detail::clamped_volume_decibels(request->send_volume_decibels);
							change.mode = request->send_mode;

							routing->write_send_state(target.reference.guid, *send_index, change);

							result.sends.push_back(created_send{
								target.reference.guid,
								*send_index,
								*change.volume_decibels,
								request->send_mode,
							});

							actions.push_back(succeeded_action(target.reference.guid));
						}

						routing_detail::truncate_to(result.sends, maximum_reported_sends);
					}

					for (const std::string& fx_name : request->fx_names)
					{
						const std::optional<routing_fx_placement> placed =
							routing->add_fx(bus_track->guid, fx_name);

						if (!placed.has_value())
						{
							actions.push_back(failed_action(
								bus_track->guid,
								reaper_routing_failure_code,
								"REAPER would not add \"" + fx_name + "\" to the bus"));

							continue;
						}

						result.fx.push_back(*placed);
					}

					routing_detail::truncate_to(result.fx, maximum_reported_bus_fx);

					if (!routing_detail::every_action_applied(actions))
					{
						return handler_action_outcomes{std::move(actions)};
					}

					return handler_success<JsonValue>{write_result(result)};
				},
				[routing, read_request = codec.read_create_bus_request](
					const tool_execution_context<JsonValue>& context) -> std::optional<tool_refusal> {
					// Requirement 9.7, and the other folder-closed case. A summing bus
					// re-parents its sources, so an existing send from a source's former
					// folder parent down into that source becomes a loop with no new send
					// created at all.
					const std::optional<bus_construction_request> request =
						routing_detail::read_request(context, read_request);

					if (!request.has_value() || context.resolved_targets.empty())
					{
						return std::nullopt;
					}

					const routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return std::nullopt;
					}

					const std::vector<std::string> source_guids =
						routing_detail::resolved_track_guids(context.resolved_targets);

					return refusal_from_routing_decision(evaluate_bus_construction(
						routing_graph_from(snapshot),
						plan_bus_construction(
							snapshot, *request, source_guids, std::string{planned_bus_track_identity}),
						context.call.confirmed_signal_cycle));
				}));

		// ------------------------------------------------------------------
		// remove_bus
		// ------------------------------------------------------------------

		record(
			"remove_bus",
			registry.register_mutating_tool(
				"remove_bus",
				[routing, write_result = codec.write_remove_bus_result](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					if (context.resolved_targets.empty() || !write_result)
					{
						return routing_detail::one_failed_action(
							"remove_bus",
							missing_routing_target_code,
							"remove_bus needs the bus track to dissolve");
					}

					const TrackReference& bus = context.resolved_targets[0].reference;

					const routing_snapshot snapshot = routing->read_routing_snapshot();
					const routing_track* const bus_track = find_track(snapshot, bus.guid);

					if (!snapshot.readable || bus_track == nullptr)
					{
						return routing_detail::one_failed_action(
							bus.guid,
							unknown_routing_target_code,
							"the bus track's routing could not be read");
					}

					// Derived rather than taken from the call: `remove_bus` is given a
					// track, not a type, so this is the extension's reading of what it
					// found. A plain track is neither kind of bus, and deleting one
					// because the agent called it a bus is not a mistake to make quietly.
					const std::optional<bus_type> type = classify_bus(snapshot, bus.guid);

					if (!type.has_value())
					{
						return routing_detail::one_failed_action(
							bus.guid,
							not_a_bus_code,
							"\"" + bus_track->name
								+ "\" is neither a folder that sums nor a return fed by sends, so there is "
								"no bus to remove");
					}

					remove_bus_result result;
					result.removed_bus = bus;
					result.type = *type;
					result.removed_fx_count = bus_track->fx_count;

					const summing_bus_dissolution dissolution = *type == bus_type::summing
						? plan_summing_bus_dissolution(snapshot, bus.guid)
						: summing_bus_dissolution{};

					if (*type == bus_type::summing)
					{
						result.reparented_tracks = dissolution.reparented_tracks;
						result.folder_depth_repairs = dissolution.repairs;
					}
					else
					{
						// The sends that fed the return go with it — REAPER removes a
						// track's receives when the track goes. Reported by their source
						// track, because each of those tracks has lost its path to the
						// shared effect.
						for (const routing_send& receive : bus_track->receives)
						{
							result.removed_sends.push_back(
								TrackReference{receive.track_guid, receive.track_name});
						}

						routing_detail::truncate_to(result.removed_sends, maximum_reported_sends);
					}

					std::vector<action_outcome> actions;

					if (!routing->delete_track(bus.guid))
					{
						return routing_detail::one_failed_action(
							bus.guid,
							reaper_routing_failure_code,
							"REAPER would not delete \"" + bus_track->name + "\"");
					}

					actions.push_back(succeeded_action(bus.guid));

					// Dissolving a summing bus is a folder dissolution, so the surviving
					// deltas are the Keeper's answer rather than REAPER's — requirement
					// 11.3: the children rise to the parent's own depth and none of them
					// is promoted into its folder role.
					if (*type == bus_type::summing)
					{
						for (const folder_depth_repair& repair : dissolution.repairs)
						{
							if (!routing->write_folder_depth(repair.guid, repair.folder_depth))
							{
								actions.push_back(failed_action(
									repair.guid,
									reaper_routing_failure_code,
									"REAPER would not write the folder depth \"" + repair.name
										+ "\" needs after the folder was dissolved"));
							}
						}
					}

					if (!routing_detail::every_action_applied(actions))
					{
						return handler_action_outcomes{std::move(actions)};
					}

					return handler_success<JsonValue>{write_result(result)};
				}));

		// ------------------------------------------------------------------
		// get_routing
		// ------------------------------------------------------------------

		record(
			"get_routing",
			registry.register_read_tool(
				"get_routing",
				[routing, write_result = codec.write_get_routing_result](
					const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					if (!write_result)
					{
						return routing_detail::one_failed_action(
							"get_routing",
							missing_tool_input_code,
							"the routing result could not be written");
					}

					const routing_snapshot snapshot = routing->read_routing_snapshot();

					if (!snapshot.readable)
					{
						return routing_detail::one_failed_action(
							"get_routing",
							unknown_routing_target_code,
							"the project's routing could not be read");
					}

					return handler_success<JsonValue>{write_result(build_get_routing_result(
						snapshot,
						routing_detail::resolved_track_guids(context.resolved_targets)))};
				}));

		return registration;
	}
}

#endif
