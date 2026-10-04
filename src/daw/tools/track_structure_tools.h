// Track structure tools — `create_track`, `delete_track`, `duplicate_track`, and
// `set_folder_structure` (task 10.4, requirements 9.1, 9.2, 11.1, 11.2, 11.3).
//
// These four are the only tools that change the *shape* of the track list, and the
// shape is where REAPER is at its least forgiving. There is no parent pointer
// anywhere: nesting exists only as the running total of a delta per track across
// track order, so inserting, deleting, reordering, or re-nesting one track can
// silently re-parent tracks the call never named, and nothing looks wrong while it
// happens. That is the whole reason this file exists as its own component rather
// than four handlers written beside the tools they resemble.
//
// ---------------------------------------------------------------------------
// The arithmetic is the Keeper's, not this file's
//
// `daw/folder_invariant_keeper.h` is the canonical implementation of requirement 11
// and the contract the server's own property tests pin down. Everything here routes
// through it:
//
//   - `create_track` with no folder named inserts a plain sibling through
//     `insert_track_repairing_folders`.
//   - `delete_track` deletes through `delete_track_repairing_folders`, which is what
//     dissolves the folder when the deleted track was a folder parent. This file
//     does not reimplement the dissolution; it routes the delete through the Keeper
//     and reports what the Keeper did.
//   - `duplicate_track` and `set_folder_structure` compose two Keeper-backed steps
//     — read a subtree, attach it somewhere — and every one of those steps ends in
//     `repair_folder_depth_deltas`.
//
// There is not one running total in this file. Three places in this codebase already
// accumulate folder depth — the Keeper canonically, `cycle_detector.h` for
// folder-closed cycles, and `reaper_render_host.cpp` for the render taxonomy — and a
// fourth would be the one that drifts. Every depth question below is answered by
// `accumulated_folder_depths`, `folder_nesting_levels`, or
// `reconstruct_folder_paths`, all of them the Keeper's.
//
// ---------------------------------------------------------------------------
// Two primitives, and why they are enough
//
// The four tools reduce to two operations over a depth list:
//
//   `read_balanced_subtree`   — the run of tracks rooted at one index, with its
//                               closing delta relaxed so the run's deltas sum to
//                               zero on their own. A subtree that balances is one
//                               that can be moved or copied without taking an
//                               enclosing folder's closing delta with it.
//
//   `attach_subtree_after`    — put a balanced run directly after an anchor track,
//                               at a named nesting level. One subtraction decides
//                               everything: the anchor absorbs the difference
//                               between the level it currently leaves behind and the
//                               level the run has to land at, and the run's last
//                               track gives that difference back so every track
//                               after the insertion point is left where it was.
//
// That second function is where requirement 11.2 is actually satisfied, and it is
// worth being explicit about how. Naively "put this track inside that folder" is
// done by writing `1` onto the folder parent and `-1` onto the moved track, which
// works on a two-track project and captures every following track on a real one.
// Returning the level to what it was — rather than letting the Keeper's
// close-what-is-open-at-the-end repair sort it out — is what keeps the capture from
// happening, because the repair would honour the new opening by closing it at the
// end of the project, dragging everything in between inside.
//
// ---------------------------------------------------------------------------
// Duplicating a folder parent
//
// A folder parent is not one track. `duplicate_track` on one copies the whole
// subtree, and the copy's deltas have to balance on their own before anything is
// attached — otherwise the copy carries a closing delta belonging to a folder that
// encloses the original, and the copy either closes that folder twice or not at all.
// `read_balanced_subtree` is what makes the copy balanced, and it is exactly the
// case of "a folder that closes several levels at once" that makes the difference
// visible: the original's last track closes its own folder *and* its parent's, and
// only one of those closings belongs to the copy.
//
// ---------------------------------------------------------------------------
// These four tools cannot refuse, and that is a schema fact rather than a choice
//
// `messages/tool-result-refusal.schema.json` closes `reason` at five values:
// `foreign_undo_entries`, `output_file_collision`, `ambiguous_track_selector`,
// `unresolved_track_selector`, and `signal_cycle`. The first two belong to other
// tools. The middle two are produced by the Tool Executor's own target resolution,
// before any handler here runs. `signal_cycle` belongs to the routing tools.
//
// So nothing these four handlers can discover names a reason, and none of them
// registers a precondition check. The invalid calls they *can* meet — a destination
// that names both a folder and the top level, or neither; a track asked to be moved
// inside itself — are reported as failed actions carrying a reason, following the
// precedent requirement 9.6 sets for an inverted time range: an invalid call names
// no acknowledgement the producer could confirm, so it is not a refusal.
//
// ---------------------------------------------------------------------------
// Shape of this file
//
// Header-only and inline, following `tool_executor.h`, `folder_invariant_keeper.h`,
// and `render_coordinator.h`. The Catch2 target compiles what it finds under
// `tests/` and does not compile `src/`, so logic the suite exercises has to be
// visible through the header. Nothing here includes the REAPER SDK or a JSON
// library: REAPER is reached through `TrackStructureHost` below, and the tool
// payloads are reached through a codec the caller supplies, which is what lets the
// suite drive all four tools against plain structs.

#ifndef SESH_AI_DAW_TOOLS_TRACK_STRUCTURE_TOOLS_H
#define SESH_AI_DAW_TOOLS_TRACK_STRUCTURE_TOOLS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <daw/folder_invariant_keeper.h>
#include <daw/object_resolver.h>
#include <daw/tool_executor.h>
#include <daw/tools/shared_tool_definitions.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// The four tool names, spelled once. A typo here is rejected by
	// `register_mutating_tool` rather than surfacing as requirement 23.6's
	// unknown-tool error in a producer's session, which is why the registry checks
	// against the 42 — but naming them once is still cheaper than finding out.
	inline constexpr std::string_view create_track_tool_name{"create_track"};
	inline constexpr std::string_view delete_track_tool_name{"delete_track"};
	inline constexpr std::string_view duplicate_track_tool_name{"duplicate_track"};
	inline constexpr std::string_view set_folder_structure_tool_name{"set_folder_structure"};

	// `code` values on a failed action. Each one names something the producer or the
	// agent can act on, rather than restating that the tool did not work.
	//
	// `missing_tool_input_code` is one of them and is not here: it is shared with
	// `routing_tools.h` and lives in `tools/shared_tool_definitions.h`, which that
	// header explains.
	inline constexpr std::string_view unknown_track_code{"unknown_track"};
	inline constexpr std::string_view contradictory_folder_destination_code{
		"contradictory_folder_destination"};
	inline constexpr std::string_view folder_destination_not_named_code{"folder_destination_not_named"};
	inline constexpr std::string_view folder_target_is_the_moved_track_code{
		"folder_target_is_the_moved_track"};
	inline constexpr std::string_view folder_target_inside_moved_track_code{
		"folder_target_inside_moved_track"};
	inline constexpr std::string_view track_insert_failed_code{"track_insert_failed"};
	inline constexpr std::string_view track_delete_failed_code{"track_delete_failed"};
	inline constexpr std::string_view track_duplicate_failed_code{"track_duplicate_failed"};
	inline constexpr std::string_view track_reorder_failed_code{"track_reorder_failed"};
	inline constexpr std::string_view folder_depth_write_failed_code{"folder_depth_write_failed"};

	// `folderDepthRepairs` is bounded at 4096 items by every one of the four output
	// schemas, and `tracks` on the `set_folder_structure` result at 512.
	inline constexpr std::size_t maximum_folder_depth_repairs = 4096;
	inline constexpr std::size_t maximum_moved_tracks = 512;

	// ---------------------------------------------------------------------------
	// The REAPER seam
	//
	// One interface for the six things these four tools do to a project, following
	// `daw/reaper_render_host.h` and `daw/reaper_alias_storage.h`: the interface is
	// SDK-free, the production implementation is the only translation unit that
	// includes the SDK, and the suite substitutes a scripted project. The test target
	// has no SDK include path, which is what enforces the split rather than a
	// convention anybody has to remember.
	//
	// Narrow on purpose. It is not "the project" — it is the six operations, and a
	// tool that needs a seventh adds it here where the cost of the addition is
	// visible.
	// ---------------------------------------------------------------------------

	// One track as the structural tools read it.
	//
	// Plain data. The identity is the GUID rather than the index, because every
	// operation in this file shifts indices and the GUID is the only handle that
	// survives that — which is the same reason `TrackFolderDepth` is keyed on an
	// identity rather than a position.
	struct structural_track
	{
		// REAPER's braced GUID form, as `guidToString` produces it.
		std::string guid;

		// The track name as REAPER shows it. Empty is allowed and common: an unnamed
		// track in REAPER has no name rather than a placeholder.
		std::string name;

		// `I_FOLDERDEPTH`. `1` opens a folder, `0` is a sibling, negative closes that
		// many levels after this track.
		int folder_depth_delta = 0;

		// What goes with the track when it is deleted, and what is copied when it is
		// duplicated. Read here rather than after the edit for the obvious reason:
		// after a delete there is nothing left to count.
		int item_count = 0;
		int fx_count = 0;
		int send_count = 0;
	};

	// What these tools do to a project.
	class TrackStructureHost
	{
	public:
		virtual ~TrackStructureHost() = default;

		// Project order, which is the order the deltas are accumulated in. The master
		// track is not in this list, matching `TrackListSource`.
		virtual std::vector<structural_track> read_tracks_in_project_order() = 0;

		// Inserts a track at `index` in project order and returns the GUID REAPER
		// minted for it. Empty means the insert did not happen — the caller reports a
		// failed action rather than continuing against a track that is not there.
		//
		// The new track's `I_FOLDERDEPTH` is REAPER's own default for an inserted
		// track; the caller writes the planned delta afterwards.
		virtual std::string insert_track(
			int index,
			const std::string& name,
			std::optional<int> color) = 0;

		// Removes the track and everything on it.
		virtual bool delete_track(const std::string& guid) = 0;

		// Duplicates the named tracks, with their items, FX, and sends, placing the
		// copies directly after the last of them in project order. Returns the GUIDs
		// REAPER minted, in project order — one per requested track, or empty when the
		// duplication did not happen.
		//
		// Takes a list rather than one GUID because duplicating a folder parent
		// duplicates its subtree, and a subtree copied one track at a time is a
		// different project state at every intermediate step.
		virtual std::vector<std::string> duplicate_tracks(const std::vector<std::string>& guids) = 0;

		// Rearranges the project into exactly this order. The whole order rather than
		// a move-one-track call, because the plan this file computes *is* an order, and
		// asking REAPER to reach it in one step removes every intermediate state where
		// the deltas describe a structure nobody asked for.
		virtual bool reorder_tracks(const std::vector<std::string>& guids_in_project_order) = 0;

		// Writes `I_FOLDERDEPTH`.
		virtual bool write_folder_depth_delta(const std::string& guid, int folder_depth_delta) = 0;
	};

	// ---------------------------------------------------------------------------
	// Result shapes
	//
	// One struct per tool, mirroring that tool's vendored output schema field for
	// field. Plain structs rather than JSON, so the whole of this file is compilable
	// and testable without a JSON library; the codec at the bottom is where they
	// become payloads.
	// ---------------------------------------------------------------------------

	// `folderDepthRepair` is `tools/shared_tool_definitions.h`'s — routing and track
	// structure both report the Folder Invariant Keeper's repairs in the same shape, and
	// that header records why there is one definition rather than two.

	struct create_track_result
	{
		TrackReference track;

		// Zero-based position in project order.
		int index = 0;

		// Nesting level the new track ended up at. Zero is top level. Reported because
		// a track inserted inside a folder is summed by that folder's parent, which
		// changes what the producer hears.
		int accumulated_depth = 0;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	struct delete_track_result
	{
		TrackReference deleted_track;
		int deleted_item_count = 0;
		int deleted_fx_count = 0;

		// True when the deleted track was a folder parent, so the folder is gone.
		bool folder_dissolved = false;

		// The former children that rose out of the dissolved folder, in track order.
		// The tracks that were directly inside it — a track one level further down is
		// still inside its own folder parent, and it is the direct children whose
		// output now goes wherever the deleted parent's did.
		std::vector<TrackReference> reparented_tracks;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	struct duplicate_track_result
	{
		TrackReference source_track;

		// The copy of the source. REAPER mints a fresh GUID and reuses the name, so
		// this GUID is the only way to address the copy — a name pattern now matches
		// two tracks, and a destructive call against it is refused.
		TrackReference new_track;

		// Zero-based position the copy took, which is directly after the source track
		// when it is a plain track, and directly after the source's whole folder when
		// it is a folder parent.
		int index = 0;

		int copied_item_count = 0;
		int copied_fx_count = 0;
		int copied_send_count = 0;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	// `movedTrack`: one track the call moved, with the nesting level either side of
	// the move. Both, because a track already inside the target folder is a no-op the
	// producer should not be told was a change.
	struct moved_track
	{
		std::string guid;
		std::string name;
		int previous_accumulated_depth = 0;
		int accumulated_depth = 0;
		int index = 0;
	};

	struct set_folder_structure_result
	{
		std::vector<moved_track> tracks;

		// The folder parent the tracks ended up inside. An empty `guid` means the
		// property is absent, which the schema requires when the tracks went to the
		// top level — there is no longer a parent summing them.
		TrackReference folder_parent;

		bool moved_to_top_level = false;

		std::vector<folder_depth_repair> folder_depth_repairs;
	};

	// ---------------------------------------------------------------------------
	// Requests
	//
	// The tool input as each handler needs it, with every track selector already
	// resolved to a GUID by the Tool Executor (requirement 9.1). Plain structs for
	// the same reason the results are.
	// ---------------------------------------------------------------------------

	struct create_track_request
	{
		std::string name;

		// Empty means absent. `inside_folder_of_guid` decides placement when both are
		// given: folder membership is a structural claim about what sums the new track,
		// and a position is a preference.
		std::string after_track_guid;
		std::string inside_folder_of_guid;

		// Absent leaves REAPER's own default colour.
		std::optional<int> color;
	};

	struct delete_track_request
	{
		std::string track_guid;
	};

	struct duplicate_track_request
	{
		std::string track_guid;
	};

	struct set_folder_structure_request
	{
		// In the order the call supplied them, which is the order the result reports.
		std::vector<std::string> track_guids;

		// Empty when moving to the top level.
		std::string into_folder_of_guid;

		bool move_to_top_level = false;
	};

	// What a structural handler produced: the tool's own result, or the per-action
	// outcomes when something did not land. `handler_action_outcomes` is the
	// framework's own type, so this maps onto `mutating_handler_result` without a
	// conversion that could lose a reason.
	template <typename ResultType>
	using structural_outcome = std::variant<ResultType, handler_action_outcomes>;

	// ---------------------------------------------------------------------------
	// Reading a track list
	// ---------------------------------------------------------------------------

	// The depth list the Keeper operates on, keyed by GUID.
	inline std::vector<TrackFolderDepth> folder_depths_of(const std::vector<structural_track>& tracks)
	{
		std::vector<TrackFolderDepth> depths;
		depths.reserve(tracks.size());

		for (const structural_track& track : tracks)
		{
			depths.push_back(TrackFolderDepth{track.guid, track.folder_depth_delta});
		}

		return depths;
	}

	inline std::optional<std::size_t> index_of_track(
		const std::vector<structural_track>& tracks,
		std::string_view guid)
	{
		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (tracks[index].guid == guid)
			{
				return index;
			}
		}

		return std::nullopt;
	}

	inline std::optional<std::size_t> index_of_track(
		const std::vector<TrackFolderDepth>& tracks,
		std::string_view guid)
	{
		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (tracks[index].identity == guid)
			{
				return index;
			}
		}

		return std::nullopt;
	}

	// The name REAPER shows for a GUID, or empty when the track is not in the list.
	// Empty is a name REAPER genuinely reports for an unnamed track, so this does not
	// distinguish the two cases and does not need to: every caller here already knows
	// the track exists.
	inline std::string track_name_of(const std::vector<structural_track>& tracks, std::string_view guid)
	{
		const std::optional<std::size_t> index = index_of_track(tracks, guid);

		return index.has_value() ? tracks[*index].name : std::string{};
	}

	inline TrackReference track_reference_of(
		const std::vector<structural_track>& tracks,
		const std::string& guid)
	{
		return TrackReference{guid, track_name_of(tracks, guid)};
	}

	// ---------------------------------------------------------------------------
	// Folder spans
	// ---------------------------------------------------------------------------

	// Where the run of tracks rooted at one index begins and ends, and how much of
	// the closing delta on its last track belongs to folders that enclose it.
	//
	// For a folder parent the run is the parent and everything inside its folder. For
	// any other track the run is the track alone — which is not a special case so
	// much as the same answer, since a track that opens nothing encloses nothing.
	struct folder_span
	{
		std::size_t first_index = 0;
		std::size_t last_index = 0;

		// How many levels the last track closes beyond this run's own. Positive when
		// the last track also closes folders that enclose the run, which is the case
		// that makes moving or copying the run interesting: that part of the closing
		// delta is not the run's to take away.
		int enclosing_folders_closed = 0;
	};

	inline folder_span find_folder_span(const std::vector<TrackFolderDepth>& tracks, std::size_t index)
	{
		folder_span span;

		if (index >= tracks.size())
		{
			return span;
		}

		span.first_index = index;
		span.last_index = index;

		// Both from the Keeper. Nothing in this file accumulates depth itself.
		const std::vector<int> nesting_levels = folder_nesting_levels(tracks);
		const std::vector<int> depths_after_each_track = accumulated_folder_depths(tracks);

		const int level_of_track = nesting_levels[index];

		if (tracks[index].folder_depth_delta > 0)
		{
			// The folder closes at the first later track that brings the running total
			// back to the level the parent itself sits at. A folder REAPER left
			// unclosed runs to the end of the project, which is also what REAPER's own
			// project files mean by an unclosed folder.
			span.last_index = tracks.size() - 1;

			for (std::size_t candidate = index + 1; candidate < tracks.size(); ++candidate)
			{
				if (depths_after_each_track[candidate] <= level_of_track)
				{
					span.last_index = candidate;
					break;
				}
			}
		}

		span.enclosing_folders_closed = level_of_track - depths_after_each_track[span.last_index];

		return span;
	}

	// The innermost folder enclosing a track, or nothing when it is at the top level.
	inline std::optional<std::size_t> innermost_enclosing_folder_index(
		const std::vector<TrackFolderDepth>& tracks,
		std::size_t index)
	{
		if (index >= tracks.size())
		{
			return std::nullopt;
		}

		const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

		if (paths[index].empty())
		{
			return std::nullopt;
		}

		return paths[index].back();
	}

	// The outermost folder enclosing a track, or nothing when it is at the top level.
	inline std::optional<std::size_t> outermost_enclosing_folder_index(
		const std::vector<TrackFolderDepth>& tracks,
		std::size_t index)
	{
		if (index >= tracks.size())
		{
			return std::nullopt;
		}

		const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

		if (paths[index].empty())
		{
			return std::nullopt;
		}

		return paths[index].front();
	}

	// ---------------------------------------------------------------------------
	// The two primitives
	// ---------------------------------------------------------------------------

	// The run rooted at `index`, with its closing delta relaxed so the run's deltas
	// sum to zero.
	//
	// A run that balances is one that can be moved or copied without carrying an
	// enclosing folder's closing delta along with it. The relaxation is the whole
	// content of the function: the last track stops closing the folders that enclose
	// the run, and keeps closing the ones the run opened itself.
	inline std::vector<TrackFolderDepth> read_balanced_subtree(
		const std::vector<TrackFolderDepth>& tracks,
		std::size_t index)
	{
		if (index >= tracks.size())
		{
			return {};
		}

		const folder_span span = find_folder_span(tracks, index);

		std::vector<TrackFolderDepth> subtree(
			tracks.begin() + static_cast<std::ptrdiff_t>(span.first_index),
			tracks.begin() + static_cast<std::ptrdiff_t>(span.last_index) + 1);

		subtree.back().folder_depth_delta += span.enclosing_folders_closed;

		return subtree;
	}

	// A run lifted out of a track list, and the list without it.
	struct subtree_extraction
	{
		// The list with the run removed, repaired.
		std::vector<TrackFolderDepth> remaining;

		// The run, balanced.
		std::vector<TrackFolderDepth> subtree;

		// Where the run sat in the list it came out of.
		std::size_t first_index = 0;
		std::size_t last_index = 0;

		// The nesting level the run sat at.
		int nesting_level = 0;

		std::size_t track_count() const { return last_index - first_index + 1; }
	};

	// Lifts the run rooted at `index` out of the list.
	//
	// Two things move. The run takes only the closing delta that belongs to it, per
	// `read_balanced_subtree`. What is left over — the part that closed folders
	// enclosing the run — stays behind on the track immediately before the run, which
	// becomes the last member of whatever folder the run was the last member of.
	// Leaving it on the run instead would take an enclosing folder's closing delta
	// away with the move, and the Keeper's repair would then close that folder at the
	// end of the project, dragging every following track inside it.
	inline subtree_extraction extract_subtree(
		const std::vector<TrackFolderDepth>& tracks,
		std::size_t index)
	{
		subtree_extraction extraction;

		if (index >= tracks.size())
		{
			extraction.remaining = tracks;

			return extraction;
		}

		const folder_span span = find_folder_span(tracks, index);

		extraction.first_index = span.first_index;
		extraction.last_index = span.last_index;
		extraction.nesting_level = folder_nesting_levels(tracks)[span.first_index];
		extraction.subtree = read_balanced_subtree(tracks, index);

		std::vector<TrackFolderDepth> remaining = tracks;

		if (span.enclosing_folders_closed != 0 && span.first_index > 0)
		{
			remaining[span.first_index - 1].folder_depth_delta -= span.enclosing_folders_closed;
		}

		remaining.erase(
			remaining.begin() + static_cast<std::ptrdiff_t>(span.first_index),
			remaining.begin() + static_cast<std::ptrdiff_t>(span.last_index) + 1);

		extraction.remaining = repair_folder_depth_deltas(std::move(remaining));

		return extraction;
	}

	// Attaches a balanced run directly after `insert_after_index`, at
	// `target_nesting_level`.
	//
	// One subtraction decides the whole operation. The anchor currently leaves the
	// running total at some level; the run has to start at `target_nesting_level`; the
	// difference goes onto the anchor, and the run's last track gives the same
	// difference back so that every track after the insertion point is left at exactly
	// the level it was at before.
	//
	// That second half is what makes requirement 11.2 hold. Writing an opening delta
	// and letting the Keeper's close-what-is-open-at-the-end repair finish the job
	// would produce a valid structure that captures every track between the insertion
	// point and the end of the project — valid, and not what anybody asked for.
	inline std::vector<TrackFolderDepth> attach_subtree_after(
		std::vector<TrackFolderDepth> tracks,
		std::vector<TrackFolderDepth> subtree,
		std::size_t insert_after_index,
		int target_nesting_level)
	{
		if (subtree.empty())
		{
			return repair_folder_depth_deltas(std::move(tracks));
		}

		if (tracks.empty())
		{
			return repair_folder_depth_deltas(std::move(subtree));
		}

		if (insert_after_index >= tracks.size())
		{
			insert_after_index = tracks.size() - 1;
		}

		const int level_after_anchor = accumulated_folder_depths(tracks)[insert_after_index];
		const int level_change = target_nesting_level - level_after_anchor;

		tracks[insert_after_index].folder_depth_delta += level_change;
		subtree.back().folder_depth_delta -= level_change;

		tracks.insert(
			tracks.begin() + static_cast<std::ptrdiff_t>(insert_after_index) + 1,
			subtree.begin(),
			subtree.end());

		return repair_folder_depth_deltas(std::move(tracks));
	}

	// ---------------------------------------------------------------------------
	// Reporting what changed
	// ---------------------------------------------------------------------------

	// Every track whose delta the structure required to change, other than the tracks
	// the call named.
	//
	// The schemas require this array rather than making it optional, so that an empty
	// one means "checked, nothing needed repairing" instead of being
	// indistinguishable from "did not check". Tracks absent from `before` are new, and
	// a new track's delta is not a repair.
	inline std::vector<folder_depth_repair> folder_depth_repairs_between(
		const std::vector<structural_track>& before,
		const std::vector<TrackFolderDepth>& after,
		const std::vector<std::string>& guids_the_call_named)
	{
		std::vector<folder_depth_repair> repairs;

		for (const TrackFolderDepth& track : after)
		{
			if (repairs.size() >= maximum_folder_depth_repairs)
			{
				break;
			}

			const bool was_named = std::find(
				guids_the_call_named.begin(),
				guids_the_call_named.end(),
				track.identity) != guids_the_call_named.end();

			if (was_named)
			{
				continue;
			}

			const std::optional<std::size_t> index_before = index_of_track(before, track.identity);

			if (!index_before.has_value())
			{
				continue;
			}

			const int previous_delta = before[*index_before].folder_depth_delta;

			if (previous_delta == track.folder_depth_delta)
			{
				continue;
			}

			repairs.push_back(folder_depth_repair{
				track.identity,
				before[*index_before].name,
				previous_delta,
				track.folder_depth_delta
			});
		}

		return repairs;
	}

	// ---------------------------------------------------------------------------
	// Writing the planned deltas
	// ---------------------------------------------------------------------------

	// Writes every planned delta that differs from what the project currently holds,
	// and reports the first GUID it could not write.
	//
	// `deltas_now` is what REAPER holds after the structural edit but before any
	// repair: a track REAPER inserted carries REAPER's default, a track REAPER
	// duplicated carries the original's delta, and a track REAPER reordered carries
	// its own. Comparing against that rather than writing unconditionally keeps the
	// write count to the tracks that actually need one.
	struct folder_depth_write_outcome
	{
		std::size_t written_count = 0;

		// Empty when every write landed.
		std::string failed_guid;
	};

	inline folder_depth_write_outcome write_planned_folder_depths(
		TrackStructureHost& host,
		const std::vector<TrackFolderDepth>& planned,
		const std::vector<std::pair<std::string, int>>& deltas_now)
	{
		folder_depth_write_outcome outcome;

		for (const TrackFolderDepth& track : planned)
		{
			bool found_current = false;
			int current_delta = 0;

			for (const std::pair<std::string, int>& entry : deltas_now)
			{
				if (entry.first == track.identity)
				{
					found_current = true;
					current_delta = entry.second;
					break;
				}
			}

			if (found_current && current_delta == track.folder_depth_delta)
			{
				continue;
			}

			if (!host.write_folder_depth_delta(track.identity, track.folder_depth_delta))
			{
				outcome.failed_guid = track.identity;

				return outcome;
			}

			++outcome.written_count;
		}

		return outcome;
	}

	inline std::vector<std::pair<std::string, int>> current_deltas_of(
		const std::vector<structural_track>& tracks)
	{
		std::vector<std::pair<std::string, int>> deltas;
		deltas.reserve(tracks.size());

		for (const structural_track& track : tracks)
		{
			deltas.emplace_back(track.guid, track.folder_depth_delta);
		}

		return deltas;
	}

	// The failed action a delta write failure produces.
	//
	// The structural edit itself landed, so it is reported as applied and this is
	// appended beside it: requirement 9.4's no-rollback rule means the edit stands,
	// and a result claiming a clean success would hide a structure that is now
	// whatever REAPER's own repair made of it.
	inline action_outcome failed_folder_depth_write(std::string_view guid)
	{
		return failed_action(
			guid,
			folder_depth_write_failed_code,
			"REAPER would not accept the repaired folder depth for this track, so the folder "
			"structure around the edit may not be what was asked for.");
	}

	inline handler_action_outcomes one_failed_action(
		std::string_view target,
		std::string_view failure_code,
		std::string_view failure_message)
	{
		handler_action_outcomes outcomes;
		outcomes.actions.push_back(failed_action(target, failure_code, failure_message));

		return outcomes;
	}

	// ---------------------------------------------------------------------------
	// create_track
	// ---------------------------------------------------------------------------

	// The identity the plan uses for the new track before REAPER has minted its GUID.
	//
	// A placeholder rather than a lookup, because the plan has to be computed before
	// the insert: knowing which delta to write to the new track means knowing where
	// it lands, and the position is the input to the insert rather than its output.
	// Not a valid GUID, and it never reaches a payload — it is replaced the moment
	// REAPER answers.
	inline constexpr std::string_view planned_new_track_identity{"(the track being created)"};

	inline structural_outcome<create_track_result> apply_create_track(
		TrackStructureHost& host,
		const create_track_request& request)
	{
		const std::vector<structural_track> before = host.read_tracks_in_project_order();
		const std::vector<TrackFolderDepth> depths_before = folder_depths_of(before);

		std::vector<TrackFolderDepth> planned;
		std::size_t insert_at_index = before.size();

		if (!request.inside_folder_of_guid.empty())
		{
			const std::optional<std::size_t> target_index =
				index_of_track(before, request.inside_folder_of_guid);

			if (!target_index.has_value())
			{
				return one_failed_action(
					request.inside_folder_of_guid,
					unknown_track_code,
					"The track naming the folder to create this track inside is not in the project.");
			}

			// Which folder "inside the folder of X" means: X's own when X is a folder
			// parent, the folder enclosing X when it is not, and a folder X becomes the
			// parent of when it is neither. The third case is the producer grouping two
			// tracks that were not grouped, which is the ordinary reading of the
			// request and the only one that does not refuse most calls.
			const std::optional<std::size_t> enclosing_folder =
				innermost_enclosing_folder_index(depths_before, *target_index);

			const std::size_t folder_parent_index =
				depths_before[*target_index].folder_depth_delta > 0
					? *target_index
					: enclosing_folder.value_or(*target_index);

			const folder_span folder = find_folder_span(depths_before, folder_parent_index);
			const int folder_parent_level = folder_nesting_levels(depths_before)[folder_parent_index];

			std::vector<TrackFolderDepth> new_track;
			new_track.push_back(TrackFolderDepth{std::string{planned_new_track_identity}, 0});

			planned = attach_subtree_after(
				depths_before,
				std::move(new_track),
				folder.last_index,
				folder_parent_level + 1);

			insert_at_index = folder.last_index + 1;
		}
		else if (!request.after_track_guid.empty())
		{
			const std::optional<std::size_t> after_index =
				index_of_track(before, request.after_track_guid);

			if (!after_index.has_value())
			{
				return one_failed_action(
					request.after_track_guid,
					unknown_track_code,
					"The track this track was to be created after is not in the project.");
			}

			insert_at_index = *after_index + 1;

			// The Keeper's own plain-sibling insert. It leaves every existing track
			// parented exactly as it was, which is not the same as leaving the project
			// unchanged: a track inserted between a folder parent and its closing delta
			// joins that folder, which is why `create_track` is classified destructive
			// despite only adding something.
			planned = insert_track_repairing_folders(
				depths_before,
				insert_at_index,
				std::string{planned_new_track_identity});
		}
		else
		{
			insert_at_index = before.size();

			planned = insert_track_repairing_folders(
				depths_before,
				insert_at_index,
				std::string{planned_new_track_identity});
		}

		const std::string new_track_guid = host.insert_track(
			static_cast<int>(insert_at_index),
			request.name,
			request.color);

		if (new_track_guid.empty())
		{
			return one_failed_action(
				request.name,
				track_insert_failed_code,
				"REAPER did not insert the track, so nothing was created.");
		}

		for (TrackFolderDepth& track : planned)
		{
			if (track.identity == planned_new_track_identity)
			{
				track.identity = new_track_guid;
			}
		}

		// What REAPER holds now: every existing track's own delta, plus the new track
		// carrying REAPER's default of zero for an inserted track.
		std::vector<std::pair<std::string, int>> deltas_now = current_deltas_of(before);
		deltas_now.emplace_back(new_track_guid, 0);

		const folder_depth_write_outcome writes =
			write_planned_folder_depths(host, planned, deltas_now);

		create_track_result result;
		result.track = TrackReference{new_track_guid, request.name};
		result.index = static_cast<int>(insert_at_index);
		result.folder_depth_repairs =
			folder_depth_repairs_between(before, planned, {new_track_guid});

		const std::optional<std::size_t> planned_index = index_of_track(planned, new_track_guid);

		if (planned_index.has_value())
		{
			result.accumulated_depth = folder_nesting_levels(planned)[*planned_index];
			result.index = static_cast<int>(*planned_index);
		}

		if (!writes.failed_guid.empty())
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(succeeded_action(new_track_guid));
			outcomes.actions.push_back(failed_folder_depth_write(writes.failed_guid));

			return outcomes;
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// delete_track
	// ---------------------------------------------------------------------------

	inline structural_outcome<delete_track_result> apply_delete_track(
		TrackStructureHost& host,
		const delete_track_request& request)
	{
		const std::vector<structural_track> before = host.read_tracks_in_project_order();
		const std::vector<TrackFolderDepth> depths_before = folder_depths_of(before);

		const std::optional<std::size_t> delete_index = index_of_track(before, request.track_guid);

		if (!delete_index.has_value())
		{
			return one_failed_action(
				request.track_guid,
				unknown_track_code,
				"The track to delete is not in the project.");
		}

		const structural_track& doomed_track = before[*delete_index];

		// Requirement 11.3. The Keeper dissolves the folder: the former children rise
		// to the parent's own depth and none of them is promoted into the parent's
		// folder role. Carrying the opening delta onto the first child instead would
		// make that child a folder parent and silently re-parent all of its siblings
		// underneath it — a different structure, and the mistake the requirement names
		// explicitly. This handler routes the delete through the Keeper and reports
		// what it did; the dissolution is not reimplemented here.
		std::vector<TrackFolderDepth> planned =
			delete_track_repairing_folders(depths_before, *delete_index);

		delete_track_result result;
		result.deleted_track = TrackReference{doomed_track.guid, doomed_track.name};
		result.deleted_item_count = doomed_track.item_count;
		result.deleted_fx_count = doomed_track.fx_count;
		result.folder_dissolved = doomed_track.folder_depth_delta > 0;

		if (result.folder_dissolved)
		{
			const std::vector<std::vector<std::size_t>> paths_before =
				reconstruct_folder_paths(depths_before);

			for (std::size_t index = *delete_index + 1; index < before.size(); ++index)
			{
				if (paths_before[index].empty() || paths_before[index].back() != *delete_index)
				{
					continue;
				}

				result.reparented_tracks.push_back(
					TrackReference{before[index].guid, before[index].name});
			}
		}

		if (!host.delete_track(request.track_guid))
		{
			return one_failed_action(
				request.track_guid,
				track_delete_failed_code,
				"REAPER did not delete the track, so it and everything on it are still there.");
		}

		const folder_depth_write_outcome writes =
			write_planned_folder_depths(host, planned, current_deltas_of(before));

		result.folder_depth_repairs =
			folder_depth_repairs_between(before, planned, {request.track_guid});

		if (!writes.failed_guid.empty())
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(succeeded_action(request.track_guid));
			outcomes.actions.push_back(failed_folder_depth_write(writes.failed_guid));

			return outcomes;
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// duplicate_track
	// ---------------------------------------------------------------------------

	inline structural_outcome<duplicate_track_result> apply_duplicate_track(
		TrackStructureHost& host,
		const duplicate_track_request& request)
	{
		const std::vector<structural_track> before = host.read_tracks_in_project_order();
		const std::vector<TrackFolderDepth> depths_before = folder_depths_of(before);

		const std::optional<std::size_t> source_index = index_of_track(before, request.track_guid);

		if (!source_index.has_value())
		{
			return one_failed_action(
				request.track_guid,
				unknown_track_code,
				"The track to duplicate is not in the project.");
		}

		// A folder parent is not one track. The run rooted at the source is what gets
		// copied, and it is balanced before anything else happens: a copy carrying an
		// enclosing folder's closing delta would close that folder a second time.
		const folder_span span = find_folder_span(depths_before, *source_index);
		const std::vector<TrackFolderDepth> balanced_copy =
			read_balanced_subtree(depths_before, *source_index);

		std::vector<std::string> source_guids;
		source_guids.reserve(span.last_index - span.first_index + 1);

		for (std::size_t index = span.first_index; index <= span.last_index; ++index)
		{
			source_guids.push_back(before[index].guid);
		}

		const std::vector<std::string> copy_guids = host.duplicate_tracks(source_guids);

		if (copy_guids.size() != source_guids.size())
		{
			return one_failed_action(
				request.track_guid,
				track_duplicate_failed_code,
				copy_guids.empty()
					? "REAPER did not duplicate the track, so nothing was copied."
					: "REAPER duplicated a different number of tracks than the folder holds, so the "
						"copy is not the subtree that was asked for.");
		}

		std::vector<TrackFolderDepth> copy_with_new_identities = balanced_copy;

		for (std::size_t index = 0; index < copy_with_new_identities.size(); ++index)
		{
			copy_with_new_identities[index].identity = copy_guids[index];
		}

		const int source_level = folder_nesting_levels(depths_before)[span.first_index];

		// The copy lands directly after the source's own run, at the source's own
		// nesting level — so a duplicated folder sits beside the original inside
		// whatever folder encloses it, rather than beside the folder that encloses
		// them both.
		const std::vector<TrackFolderDepth> planned = attach_subtree_after(
			depths_before,
			copy_with_new_identities,
			span.last_index,
			source_level);

		// What REAPER holds now: every existing track's own delta, plus each copy
		// carrying the delta of the track it was copied from.
		std::vector<std::pair<std::string, int>> deltas_now = current_deltas_of(before);

		for (std::size_t index = 0; index < copy_guids.size(); ++index)
		{
			deltas_now.emplace_back(
				copy_guids[index],
				before[span.first_index + index].folder_depth_delta);
		}

		const folder_depth_write_outcome writes =
			write_planned_folder_depths(host, planned, deltas_now);

		duplicate_track_result result;
		result.source_track = TrackReference{before[*source_index].guid, before[*source_index].name};
		result.new_track = TrackReference{copy_guids.front(), before[span.first_index].name};
		result.index = static_cast<int>(span.last_index + 1);
		result.copied_item_count = before[*source_index].item_count;
		result.copied_fx_count = before[*source_index].fx_count;
		result.copied_send_count = before[*source_index].send_count;

		const std::optional<std::size_t> planned_index = index_of_track(planned, copy_guids.front());

		if (planned_index.has_value())
		{
			result.index = static_cast<int>(*planned_index);
		}

		result.folder_depth_repairs = folder_depth_repairs_between(before, planned, copy_guids);

		if (!writes.failed_guid.empty())
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(succeeded_action(copy_guids.front()));
			outcomes.actions.push_back(failed_folder_depth_write(writes.failed_guid));

			return outcomes;
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// set_folder_structure
	// ---------------------------------------------------------------------------

	// What the call asked for, once the two input fields have been read together.
	enum class folder_destination_kind
	{
		into_folder,
		top_level,

		// Both fields set. The input schema says to use one instead of the other and
		// cannot express it, so it arrives here.
		contradictory,

		// Neither field set.
		not_named
	};

	inline folder_destination_kind folder_destination_of(const set_folder_structure_request& request)
	{
		const bool names_folder = !request.into_folder_of_guid.empty();

		if (request.move_to_top_level && names_folder)
		{
			return folder_destination_kind::contradictory;
		}

		if (request.move_to_top_level)
		{
			return folder_destination_kind::top_level;
		}

		if (names_folder)
		{
			return folder_destination_kind::into_folder;
		}

		return folder_destination_kind::not_named;
	}

	// One track's move, planned but not yet applied.
	struct planned_move
	{
		std::string guid;

		// Set when the move cannot be planned. The move is skipped and this is
		// reported as a failed action.
		std::string failure_code;
		std::string failure_message;

		// True when the track was already where it was asked to be, so nothing moved.
		bool already_in_place = false;

		bool failed() const { return !failure_code.empty(); }
	};

	// Where every named track ends up, and what that does to the deltas.
	struct folder_structure_plan
	{
		// The depth list after every move, repaired.
		std::vector<TrackFolderDepth> tracks;

		// One entry per GUID the call named, in the order it named them.
		std::vector<planned_move> moves;

		// The folder parent the tracks ended up inside. Empty when moving to the top
		// level, or when no move could be planned.
		std::string folder_parent_guid;
	};

	// Plans the whole call, one track at a time.
	//
	// One at a time and recomputing between moves, rather than a single permutation.
	// Every move shifts indices, and a plan computed against positions that a
	// previous move invalidated is how a tool ends up re-parenting a track nobody
	// named. Recomputing costs a walk of the track list per moved track, which
	// against a project's track count is nothing next to being wrong.
	inline folder_structure_plan plan_folder_structure_change(
		const std::vector<structural_track>& before,
		const set_folder_structure_request& request)
	{
		folder_structure_plan plan;
		plan.tracks = folder_depths_of(before);

		const folder_destination_kind destination = folder_destination_of(request);

		for (const std::string& guid : request.track_guids)
		{
			planned_move move;
			move.guid = guid;

			if (destination == folder_destination_kind::contradictory)
			{
				move.failure_code = std::string{contradictory_folder_destination_code};
				move.failure_message =
					"The call asked for both a folder to move into and a move to the top level. "
					"Name one or the other.";
				plan.moves.push_back(std::move(move));
				continue;
			}

			if (destination == folder_destination_kind::not_named)
			{
				move.failure_code = std::string{folder_destination_not_named_code};
				move.failure_message =
					"The call named no folder to move into and did not ask for the top level, so "
					"there is nowhere to move the track to.";
				plan.moves.push_back(std::move(move));
				continue;
			}

			const std::optional<std::size_t> move_index = index_of_track(plan.tracks, guid);

			if (!move_index.has_value())
			{
				move.failure_code = std::string{unknown_track_code};
				move.failure_message = "The track to move is not in the project.";
				plan.moves.push_back(std::move(move));
				continue;
			}

			const folder_span span = find_folder_span(plan.tracks, *move_index);

			std::size_t anchor_index = 0;
			int target_level = 0;

			if (destination == folder_destination_kind::top_level)
			{
				const std::optional<std::size_t> outermost_folder =
					outermost_enclosing_folder_index(plan.tracks, *move_index);

				if (!outermost_folder.has_value())
				{
					// Already at the top level. Reported as moved and left alone: the
					// schema is explicit that a track already where it was asked to be
					// is a no-op the producer should not be told was a change.
					move.already_in_place = true;
					plan.moves.push_back(std::move(move));
					continue;
				}

				anchor_index = find_folder_span(plan.tracks, *outermost_folder).last_index;
				target_level = 0;
			}
			else
			{
				const std::optional<std::size_t> target_index =
					index_of_track(plan.tracks, request.into_folder_of_guid);

				if (!target_index.has_value())
				{
					move.failure_code = std::string{unknown_track_code};
					move.failure_message =
						"The track naming the folder to move into is not in the project.";
					plan.moves.push_back(std::move(move));
					continue;
				}

				if (*target_index == *move_index)
				{
					move.failure_code = std::string{folder_target_is_the_moved_track_code};
					move.failure_message =
						"A track cannot be moved into its own folder — the call named the same "
						"track as both the track to move and the folder to move it into.";
					plan.moves.push_back(std::move(move));
					continue;
				}

				if (*target_index >= span.first_index && *target_index <= span.last_index)
				{
					move.failure_code = std::string{folder_target_inside_moved_track_code};
					move.failure_message =
						"The folder to move into is inside the track being moved, so the move would "
						"put the track inside itself.";
					plan.moves.push_back(std::move(move));
					continue;
				}

				const std::optional<std::size_t> enclosing_folder =
					innermost_enclosing_folder_index(plan.tracks, *target_index);

				// Which folder "into the folder of X" means. Same three cases as
				// `create_track`'s `insideFolderOf`: X's own folder when X is a folder
				// parent, the folder enclosing X when it is not, and a folder X becomes
				// the parent of when it is neither — the producer grouping two tracks
				// that were not grouped.
				const std::size_t folder_parent_index =
					plan.tracks[*target_index].folder_depth_delta > 0
						? *target_index
						: enclosing_folder.value_or(*target_index);

				plan.folder_parent_guid = plan.tracks[folder_parent_index].identity;

				const std::optional<std::size_t> current_folder =
					innermost_enclosing_folder_index(plan.tracks, *move_index);

				if (current_folder.has_value() && *current_folder == folder_parent_index)
				{
					move.already_in_place = true;
					plan.moves.push_back(std::move(move));
					continue;
				}

				anchor_index = find_folder_span(plan.tracks, folder_parent_index).last_index;
				target_level = folder_nesting_levels(plan.tracks)[folder_parent_index] + 1;
			}

			const subtree_extraction extraction = extract_subtree(plan.tracks, *move_index);

			// The anchor's index in the list the run came out of is not its index in
			// the list without it. Two adjustments, and the second is the one that is
			// easy to miss: the anchor can be *inside* the run, which happens when the
			// run's last track was also the last track of the folder being left. The
			// track before the run is then the anchor, because it is what the folder
			// now ends at.
			std::size_t adjusted_anchor = anchor_index;

			if (anchor_index >= extraction.first_index && anchor_index <= extraction.last_index)
			{
				adjusted_anchor = extraction.first_index == 0 ? 0 : extraction.first_index - 1;
			}
			else if (anchor_index > extraction.last_index)
			{
				adjusted_anchor = anchor_index - extraction.track_count();
			}

			plan.tracks = attach_subtree_after(
				extraction.remaining,
				extraction.subtree,
				adjusted_anchor,
				target_level);

			plan.moves.push_back(std::move(move));
		}

		return plan;
	}

	inline structural_outcome<set_folder_structure_result> apply_set_folder_structure(
		TrackStructureHost& host,
		const set_folder_structure_request& request)
	{
		const std::vector<structural_track> before = host.read_tracks_in_project_order();
		const std::vector<TrackFolderDepth> depths_before = folder_depths_of(before);

		const folder_structure_plan plan = plan_folder_structure_change(before, request);

		// Requirement 9.4 and 9.5: every action reported, in the order the call named
		// them, every failure carrying a reason, and a sibling's failure not undoing
		// what its siblings did.
		//
		// Built in one pass over the plan so that the outcome list and the call's own
		// track list line up index for index. A planned move that failed keeps its own
		// reason whatever happens to its siblings.
		const auto build_action_outcomes = [&plan](
			std::string_view moved_failure_code,
			std::string_view moved_failure_message) {
			handler_action_outcomes outcomes;

			for (const planned_move& move : plan.moves)
			{
				if (move.failed())
				{
					outcomes.actions.push_back(
						failed_action(move.guid, move.failure_code, move.failure_message));
					continue;
				}

				if (moved_failure_code.empty())
				{
					outcomes.actions.push_back(succeeded_action(move.guid));
					continue;
				}

				outcomes.actions.push_back(
					failed_action(move.guid, moved_failure_code, moved_failure_message));
			}

			return outcomes;
		};

		bool any_planned_move_failed = false;
		std::vector<std::string> moved_guids;

		for (const planned_move& move : plan.moves)
		{
			if (move.failed())
			{
				any_planned_move_failed = true;
				continue;
			}

			moved_guids.push_back(move.guid);
		}

		if (moved_guids.empty())
		{
			// Nothing to apply. Either every named track failed to plan, or the call
			// named no tracks at all — which the input schema's `minItems` should have
			// stopped, and which is still reported rather than returning a success with
			// an empty `tracks` array the output schema forbids.
			handler_action_outcomes outcomes = build_action_outcomes({}, {});

			if (outcomes.actions.empty())
			{
				outcomes.actions.push_back(failed_action(
					set_folder_structure_tool_name,
					folder_destination_not_named_code,
					"The call named no tracks to move."));
			}

			return outcomes;
		}

		// The order the plan reached is the order REAPER is asked for, in one call.
		std::vector<std::string> planned_order;
		planned_order.reserve(plan.tracks.size());

		for (const TrackFolderDepth& track : plan.tracks)
		{
			planned_order.push_back(track.identity);
		}

		std::vector<std::string> order_before;
		order_before.reserve(before.size());

		for (const structural_track& track : before)
		{
			order_before.push_back(track.guid);
		}

		const bool order_changed = planned_order != order_before;

		if (order_changed && !host.reorder_tracks(planned_order))
		{
			// Nothing was written, so the structure is exactly as it was. Reported per
			// action rather than as one failure, because a call naming five tracks needs
			// five outcomes for the agent to know none of them landed.
			return build_action_outcomes(
				track_reorder_failed_code,
				"REAPER did not move the track, so the folder structure is unchanged.");
		}

		const folder_depth_write_outcome writes =
			write_planned_folder_depths(host, plan.tracks, current_deltas_of(before));

		set_folder_structure_result result;
		result.moved_to_top_level =
			folder_destination_of(request) == folder_destination_kind::top_level;

		if (!result.moved_to_top_level && !plan.folder_parent_guid.empty())
		{
			result.folder_parent = track_reference_of(before, plan.folder_parent_guid);
		}

		const std::vector<int> levels_before = folder_nesting_levels(depths_before);
		const std::vector<int> levels_after = folder_nesting_levels(plan.tracks);

		for (const std::string& guid : moved_guids)
		{
			if (result.tracks.size() >= maximum_moved_tracks)
			{
				break;
			}

			const std::optional<std::size_t> index_before = index_of_track(before, guid);
			const std::optional<std::size_t> index_after = index_of_track(plan.tracks, guid);

			if (!index_before.has_value() || !index_after.has_value())
			{
				continue;
			}

			result.tracks.push_back(moved_track{
				guid,
				before[*index_before].name,
				levels_before[*index_before],
				levels_after[*index_after],
				static_cast<int>(*index_after)
			});
		}

		result.folder_depth_repairs = folder_depth_repairs_between(before, plan.tracks, moved_guids);

		if (!writes.failed_guid.empty())
		{
			// The moves landed; a repair did not. Reported as applied moves plus the
			// failed repair, because the moves are not rolled back (requirement 9.4) and
			// a success payload would claim a structure nobody verified.
			handler_action_outcomes outcomes = build_action_outcomes({}, {});
			outcomes.actions.push_back(failed_folder_depth_write(writes.failed_guid));

			return outcomes;
		}

		if (any_planned_move_failed)
		{
			// Some tracks moved and some did not. Per-action outcomes are the only shape
			// that can carry both, and the ones that moved stay moved.
			return build_action_outcomes({}, {});
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------------

	// How each tool's input and result cross the JSON boundary.
	//
	// Supplied by the caller rather than implemented here, and that is the same
	// decision `tool_executor.h` makes for the same reason: this file is compiled by
	// the Catch2 target, which has no JSON library, and the framework's payload type
	// is a template parameter precisely so that the logic can be driven without one.
	// Reading a tool input is the Envelope Codec's job, and it has already validated
	// nothing — requirement 4.4 keeps the 42 input schemas out of the bundle, so what
	// arrives here is input the MCP Tool Server checked against the authoritative
	// copy.
	//
	// Each reader takes the whole execution context, not just the payload, because the
	// track selectors were resolved by the executor before the handler ran
	// (requirement 9.1) and mapping a selector to the track it resolved to is the
	// reader's business.
	template <typename JsonValue>
	struct track_structure_payload_codec
	{
		std::function<create_track_request(const tool_execution_context<JsonValue>&)>
			read_create_track_request;
		std::function<delete_track_request(const tool_execution_context<JsonValue>&)>
			read_delete_track_request;
		std::function<duplicate_track_request(const tool_execution_context<JsonValue>&)>
			read_duplicate_track_request;
		std::function<set_folder_structure_request(const tool_execution_context<JsonValue>&)>
			read_set_folder_structure_request;

		std::function<JsonValue(const create_track_result&)> write_create_track_result;
		std::function<JsonValue(const delete_track_result&)> write_delete_track_result;
		std::function<JsonValue(const duplicate_track_result&)> write_duplicate_track_result;
		std::function<JsonValue(const set_folder_structure_result&)> write_set_folder_structure_result;

		bool is_complete() const
		{
			return read_create_track_request
				&& read_delete_track_request
				&& read_duplicate_track_request
				&& read_set_folder_structure_request
				&& write_create_track_result
				&& write_delete_track_result
				&& write_duplicate_track_result
				&& write_set_folder_structure_result;
		}
	};

	// Turns one handler's outcome into the framework's mutating result.
	//
	// The framework attaches the undo report; this only chooses between the tool's own
	// fields and the per-action outcomes. A handler cannot produce a refusal here, and
	// `mutating_handler_result` has no alternative for one — see the file header on
	// why none of the five refusal reasons fits these four tools.
	template <typename JsonValue, typename ResultType, typename ResultWriter>
	mutating_handler_result<JsonValue> as_mutating_handler_result(
		structural_outcome<ResultType> outcome,
		const ResultWriter& write_result)
	{
		if (handler_action_outcomes* const outcomes = std::get_if<handler_action_outcomes>(&outcome))
		{
			return std::move(*outcomes);
		}

		handler_success<JsonValue> success;
		success.fields = write_result(std::get<ResultType>(outcome));

		return success;
	}

	// One outcome per tool, in registration order, so a caller can report exactly
	// which of the four did not register rather than that something did not.
	struct track_structure_registration_report
	{
		tool_registration_outcome create_track = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome delete_track = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome duplicate_track = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome set_folder_structure = tool_registration_outcome::no_handler_supplied;

		bool every_tool_registered() const
		{
			return create_track == tool_registration_outcome::registered
				&& delete_track == tool_registration_outcome::registered
				&& duplicate_track == tool_registration_outcome::registered
				&& set_folder_structure == tool_registration_outcome::registered;
		}
	};

	// Registers all four through the framework's mutating seam.
	//
	// `register_mutating_tool` with no precondition check, for all four. The seam's
	// precondition is the only place a mutating tool may refuse, and these four have
	// nothing to refuse with: the refusal schema's `reason` is closed at five values
	// and none of them describes anything these tools can discover. Every invalid call
	// they can meet is a failed action carrying a reason instead.
	//
	// `host` and the codec's callables must outlive the registry, which outlives one
	// dispatch. The extension constructs both at startup and the suite on the stack;
	// neither is captured by value, because a track structure host copied into four
	// closures would be four views of one project.
	template <typename JsonValue>
	track_structure_registration_report register_track_structure_tools(
		tool_handler_registry<JsonValue>& registry,
		TrackStructureHost& host,
		track_structure_payload_codec<JsonValue> codec)
	{
		track_structure_registration_report report;

		if (!codec.is_complete())
		{
			return report;
		}

		report.create_track = registry.register_mutating_tool(
			create_track_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return as_mutating_handler_result<JsonValue>(
					apply_create_track(host, codec.read_create_track_request(context)),
					codec.write_create_track_result);
			});

		report.delete_track = registry.register_mutating_tool(
			delete_track_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return as_mutating_handler_result<JsonValue>(
					apply_delete_track(host, codec.read_delete_track_request(context)),
					codec.write_delete_track_result);
			});

		report.duplicate_track = registry.register_mutating_tool(
			duplicate_track_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return as_mutating_handler_result<JsonValue>(
					apply_duplicate_track(host, codec.read_duplicate_track_request(context)),
					codec.write_duplicate_track_result);
			});

		report.set_folder_structure = registry.register_mutating_tool(
			set_folder_structure_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return as_mutating_handler_result<JsonValue>(
					apply_set_folder_structure(host, codec.read_set_folder_structure_request(context)),
					codec.write_set_folder_structure_result);
			});

		return report;
	}
}

#endif  // SESH_AI_DAW_TOOLS_TRACK_STRUCTURE_TOOLS_H
