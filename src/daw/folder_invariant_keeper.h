// Folder Invariant Keeper — folder depth delta arithmetic.
//
// REAPER stores folder nesting as a depth delta per track, read and written as
// `I_FOLDERDEPTH`: `1` opens a folder, `0` is a sibling, and a negative value
// closes that many levels after the track it sits on. There is no parent pointer
// anywhere. Parentage exists only as the running total of those deltas across
// track order, which is why any insert, delete, or reorder can silently change
// what tracks *other than the target* are parented to, and why nothing looks
// wrong while it happens.
//
// This component owns the repair. The arithmetic it implements is not invented
// here: it is specified by the server's property tests in
// `server/mcp-server/src/lib/folder-invariant.property.test.js`, written as a
// model the extension is obliged to satisfy rather than a description of what the
// extension does. The Catch2 suite beside this file mirrors those tests, case for
// case, including the negative control.
//
// Two things about the shape of this header.
//
// It is pure arithmetic over a list of deltas and deliberately knows nothing
// about REAPER. `MediaTrack*` never appears, no SDK header is included, and
// nothing here calls into the API. That is what makes the whole component
// testable outside REAPER, which is where the interesting failures live — the
// repair is a sequence calculation, not an API interaction.
//
// The definitions are `inline` in the header rather than in a translation unit.
// The reason is the build, not the design: the Catch2 target compiles the sources
// it finds under `tests/` and does not compile `src/`, so logic the suite
// exercises has to be visible through the header today. When the test target
// starts compiling source domains directly, these move into
// `folder_invariant_keeper.cpp` unchanged.

#ifndef SESH_AI_DAW_FOLDER_INVARIANT_KEEPER_H
#define SESH_AI_DAW_FOLDER_INVARIANT_KEEPER_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sesh_ai {
namespace daw {

// REAPER's largest meaningful opening delta. `I_FOLDERDEPTH` opens at most one
// level per track — the project-context schema bounds the field at 1 for the same
// reason — so anything above this is corruption to be clamped, not nesting to be
// honoured.
constexpr int maximum_folder_depth_delta = 1;

// One track's contribution to the folder structure: who it is, and what its delta
// does. The identity is whatever the caller uses to recognise a track across a
// structural change — a GUID in the extension, a readable name in tests. Nothing
// here interprets it, but the repair has to be checkable by identity, because
// indices shift under every operation this component exists to repair.
struct TrackFolderDepth
{
	std::string identity;
	int folder_depth_delta{0};
};

// The list of enclosing folder indices for each track, outermost first.
//
// This is the parentage the deltas encode, and it is the only comparison the
// invariant can be stated in. Two track lists have the same structure exactly
// when every track's path matches; a delta-by-delta diff cannot express that,
// because a correct repair changes deltas on purpose.
inline std::vector<std::vector<std::size_t>> reconstruct_folder_paths(
	const std::vector<TrackFolderDepth>& tracks)
{
	std::vector<std::vector<std::size_t>> paths;
	paths.reserve(tracks.size());

	std::vector<std::size_t> open_folders;

	for (std::size_t index = 0; index < tracks.size(); ++index) {
		// Recorded before the track's own delta is applied: a folder parent is not
		// inside itself, and its own path is the one its children extend.
		paths.push_back(open_folders);

		const int delta = tracks[index].folder_depth_delta;

		if (delta > 0) {
			open_folders.push_back(index);
		} else if (delta < 0) {
			for (int closes = 0; closes < -delta && !open_folders.empty(); ++closes) {
				open_folders.pop_back();
			}
		}
	}

	return paths;
}

// The running total of the deltas after each track — the value requirement 11.1
// calls accumulated depth, and the thing that must never go negative.
//
// Note which side of the track this lands on. The total *after* track i is what a
// following track sits inside; a track's own nesting level is the total *before*
// it, which is the length of its folder path.
inline std::vector<int> accumulated_folder_depths(const std::vector<TrackFolderDepth>& tracks)
{
	std::vector<int> depths;
	depths.reserve(tracks.size());

	int accumulated = 0;

	for (const TrackFolderDepth& track : tracks) {
		accumulated += track.folder_depth_delta;
		depths.push_back(accumulated);
	}

	return depths;
}

// The nesting level of each track: how many folders enclose it.
inline std::vector<int> folder_nesting_levels(const std::vector<TrackFolderDepth>& tracks)
{
	std::vector<int> levels;
	levels.reserve(tracks.size());

	int accumulated = 0;

	for (const TrackFolderDepth& track : tracks) {
		levels.push_back(accumulated);
		accumulated += track.folder_depth_delta;
	}

	return levels;
}

// Whether a track list already satisfies the invariant: deltas sum to zero, no
// accumulated depth is negative, and no track claims to open more than one level.
inline bool folder_depth_deltas_are_well_formed(const std::vector<TrackFolderDepth>& tracks)
{
	int accumulated = 0;

	for (const TrackFolderDepth& track : tracks) {
		if (track.folder_depth_delta > maximum_folder_depth_delta) {
			return false;
		}

		accumulated += track.folder_depth_delta;

		if (accumulated < 0) {
			return false;
		}
	}

	return accumulated == 0;
}

// Brings a track list back inside the invariant, changing as little as it can.
//
// Three repairs, in the order the walk meets them:
//
//   - A delta above one is clamped. REAPER cannot represent it and it would
//     invent a nesting level nobody asked for.
//   - A close that would drive the accumulated depth below zero is shortened to
//     close only what is actually open. Closing a folder that was never opened
//     pulls every following track to a level it did not ask for.
//   - Folders still open after the last track are closed on it, which is what
//     REAPER's own project files do.
//
// Deltas that violate nothing are left alone, so tracks the caller was not asked
// to change keep their accumulated depth (requirement 11.2). On a list that is
// already well formed this is the identity — worth stating, because it is what
// lets the structural operations below run it unconditionally without altering
// the contract the server's tests pin down.
inline std::vector<TrackFolderDepth> repair_folder_depth_deltas(std::vector<TrackFolderDepth> tracks)
{
	int accumulated = 0;

	for (TrackFolderDepth& track : tracks) {
		if (track.folder_depth_delta > maximum_folder_depth_delta) {
			track.folder_depth_delta = maximum_folder_depth_delta;
		}

		if (accumulated + track.folder_depth_delta < 0) {
			track.folder_depth_delta = -accumulated;
		}

		accumulated += track.folder_depth_delta;
	}

	// Only the last delta changes here, so every earlier running total — and so
	// every earlier track's parentage — is untouched.
	if (accumulated > 0 && !tracks.empty()) {
		tracks.back().folder_depth_delta -= accumulated;
	}

	return tracks;
}

// Inserts a track as a plain sibling at `index_to_insert_at`, and leaves every
// existing track parented exactly as it was.
//
// A flat delta is the whole repair, and the case is listed here because it looks
// like it needs no repair at all and still is not harmless: a track inserted
// between a folder parent and its closing delta joins that folder. The operation
// is correct and the structural change is real, which is why `create_track` is
// classified destructive rather than safe.
inline std::vector<TrackFolderDepth> insert_track_repairing_folders(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_insert_at,
	std::string identity)
{
	if (index_to_insert_at > tracks.size()) {
		index_to_insert_at = tracks.size();
	}

	TrackFolderDepth inserted;
	inserted.identity = std::move(identity);
	inserted.folder_depth_delta = 0;

	tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(index_to_insert_at), std::move(inserted));

	return repair_folder_depth_deltas(std::move(tracks));
}

// Removes a track and repairs the deltas so that every survivor keeps the
// parentage it had.
//
// The defect this exists to prevent: removing a track that opens a folder while
// leaving the closing delta on a later track re-parents everything between them
// into whatever folder encloses it.
//
// The repair depends on what the removed track was doing, and the two cases are
// not symmetric.
//
//   - **It opened a folder.** The folder ceases to exist, so its children rise to
//     the parent's own depth and the delta that closed it closes one fewer level.
//     Carrying the opening delta onto the first child instead would make that
//     child the new folder parent, which silently re-parents all of its siblings
//     underneath it — a different structure, and the mistake requirement 11.3
//     names explicitly.
//   - **It closed folders.** Those folders still have to close, so the closing
//     delta transfers to the preceding track, which becomes the last member.
inline std::vector<TrackFolderDepth> delete_track_repairing_folders(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_delete)
{
	if (index_to_delete >= tracks.size()) {
		return repair_folder_depth_deltas(std::move(tracks));
	}

	const int removed_delta = tracks[index_to_delete].folder_depth_delta;

	if (removed_delta > 0) {
		// Find where the removed track's folder closed: the first later track at
		// which the running total drops below the level the opener sat at.
		int level_at_opener = 0;

		for (std::size_t index = 0; index <= index_to_delete; ++index) {
			level_at_opener += tracks[index].folder_depth_delta;
		}

		int level = level_at_opener;

		for (std::size_t index = index_to_delete + 1; index < tracks.size(); ++index) {
			level += tracks[index].folder_depth_delta;

			if (level < level_at_opener) {
				tracks[index].folder_depth_delta += removed_delta;
				break;
			}
		}
	} else if (removed_delta < 0 && index_to_delete > 0) {
		tracks[index_to_delete - 1].folder_depth_delta += removed_delta;
	}

	tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(index_to_delete));

	return repair_folder_depth_deltas(std::move(tracks));
}

// Applies a reordering given as the new sequence of original indices, then
// repairs.
//
// Reordering is the one structural change where the caller's intent cannot be
// inferred from the deltas — a permutation says nothing about which folders were
// meant to survive it. So this does exactly what requirement 11.1 asks and no
// more: the tracks land in the requested order carrying their own deltas, and the
// repair guarantees the result sums to zero with no negative accumulated depth.
// Parentage that the permutation itself changed is the permutation's business.
//
// An order that is not a permutation of the track list is ignored rather than
// half-applied, since a partially reordered project is worse than an unreordered
// one.
inline std::vector<TrackFolderDepth> reorder_tracks_repairing_folders(
	std::vector<TrackFolderDepth> tracks,
	const std::vector<std::size_t>& new_order)
{
	if (new_order.size() != tracks.size()) {
		return repair_folder_depth_deltas(std::move(tracks));
	}

	std::vector<bool> already_placed(tracks.size(), false);

	for (const std::size_t original_index : new_order) {
		if (original_index >= tracks.size() || already_placed[original_index]) {
			return repair_folder_depth_deltas(std::move(tracks));
		}

		already_placed[original_index] = true;
	}

	std::vector<TrackFolderDepth> reordered;
	reordered.reserve(tracks.size());

	for (const std::size_t original_index : new_order) {
		reordered.push_back(tracks[original_index]);
	}

	return repair_folder_depth_deltas(std::move(reordered));
}

}  // namespace daw
}  // namespace sesh_ai

#endif  // SESH_AI_DAW_FOLDER_INVARIANT_KEEPER_H
