// Folder Invariant Keeper — Properties 1, 2, and 3 of the design, stated as properties.
//
// `folder_invariant_keeper_test.cpp` beside this file mirrors the server's model in
// `server/mcp-server/src/lib/folder-invariant.property.test.js` case for case, and
// on the way it already sweeps every well-formed track list of one to six tracks
// through insert, delete, and path reconstruction, and every permutation of every
// such list of one to four. That sweep is the examples and the exhaustive base. This
// file is deliberately not a second copy of it under new names; it adds the three
// things the design's properties claim that the sweep does not reach:
//
//   - **Arbitrary, not merely well-formed, input.** Property 1 quantifies over
//     *any* track list. The mirror file enumerates lists that already satisfy the
//     invariant, so the repair is the identity on all 1092 of them and the
//     balancing claim is never actually put to work. Here the deltas come from a
//     hostile set — values above one, closes that overshoot, folders left open —
//     which is the shape the extension meets when a producer's project, or a
//     half-applied edit, hands it something REAPER cannot represent.
//
//   - **Longer lists, deeper nesting, and reorder among them.** Six tracks over
//     three shapes reaches three levels of nesting, and the permutation sweep stops
//     at four tracks. A deterministic generator reaches lists of twenty-four tracks
//     nested a dozen deep and reorders those, which is where multi-level closing
//     deltas stop being a special case. What it generated, and how deep it actually
//     got, are asserted — for the same reason the mirror file asserts
//     `lists.size() == 1092`: a generator that quietly produced nothing but short
//     flat lists would make all of this vacuously true.
//
//   - **Properties 2 and 3 as properties.** The mirror file states folder
//     dissolution and depth preservation as five hand-written examples. Requirement
//     11.3 is a claim about every folder parent in every project, and the example
//     that would have caught the regression is the one nobody wrote. Both are
//     quantified here, over the exhaustive space *and* the deep generated lists.
//
// Each property is a predicate that returns the first violation it finds, which
// buys two things. A failure names a counterexample — seed, operation, and the
// deltas either side — rather than pointing at a line inside a loop. And the same
// predicate can be run against deliberately wrong implementations to show it
// rejects them: a property that holds of the real repair *and* of an implementation
// that flattened every folder is not saying anything. Those negative controls are
// separate test cases rather than sections of the sweeps, because Catch2 re-runs a
// case body once per section and re-running three thousand generated edits to reach
// four assertions is a slow suite for no extra coverage.
//
// Nothing here touches REAPER, and nothing here re-derives the arithmetic: the
// predicates are written from requirement 11 and the design's properties, so they
// are able to disagree with the implementation.
//
// **Validates: Requirements 11.1, 11.2, 11.3**

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/folder_invariant_keeper.h>

using sesh_ai::daw::TrackFolderDepth;
using sesh_ai::daw::accumulated_folder_depths;
using sesh_ai::daw::delete_track_repairing_folders;
using sesh_ai::daw::folder_depth_deltas_are_well_formed;
using sesh_ai::daw::folder_nesting_levels;
using sesh_ai::daw::insert_track_repairing_folders;
using sesh_ai::daw::maximum_folder_depth_delta;
using sesh_ai::daw::reconstruct_folder_paths;
using sesh_ai::daw::reorder_tracks_repairing_folders;
using sesh_ai::daw::repair_folder_depth_deltas;

namespace {

// Deterministic byte source, the same xorshift the alias suite uses. The point is
// reproducibility: the seed is in the failure message, so a counterexample found on
// CI is a counterexample anybody can replay.
class DeterministicBytes {
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

// ---------------------------------------------------------------------------
// Reading a track list
// ---------------------------------------------------------------------------

// The seed as it is written in the source, so a failure message can be pasted back.
std::string describe_seed(std::uint32_t seed)
{
	static const char* const hexadecimal_digits = "0123456789abcdef";

	std::string description = "seed 0x";

	for (int shift = 28; shift >= 0; shift -= 4) {
		description += hexadecimal_digits[(seed >> shift) & 0xfu];
	}

	return description;
}

std::string describe_track_list(const std::vector<TrackFolderDepth>& tracks)
{
	std::string description = "[";

	for (std::size_t index = 0; index < tracks.size(); ++index) {
		if (index > 0) {
			description += " ";
		}

		description += tracks[index].identity + ":" + std::to_string(tracks[index].folder_depth_delta);
	}

	return description + "]";
}

// The depth each track sits at, keyed by identity, since indices shift under every
// operation these properties are about.
//
// This is the reading of "accumulated depth" requirement 11.2 has to mean. The
// running total *after* a track is what the next track sits inside; the depth the
// track itself sits at is the total *before* it, which is the length of its folder
// path. The distinction matters here and nowhere else in this file: when a track
// carrying a closing delta is deleted, the repair moves that delta onto the
// preceding track, changing that track's after-total on purpose while leaving its
// own parentage — and everyone else's — exactly as it was. Read on the after-total,
// requirement 11.2 would forbid the repair requirement 11.1 demands.
std::map<std::string, int> nesting_level_by_identity(const std::vector<TrackFolderDepth>& tracks)
{
	const std::vector<int> levels = folder_nesting_levels(tracks);

	std::map<std::string, int> level_by_identity;

	for (std::size_t index = 0; index < tracks.size(); ++index) {
		level_by_identity[tracks[index].identity] = levels[index];
	}

	return level_by_identity;
}

std::map<std::string, int> delta_by_identity(const std::vector<TrackFolderDepth>& tracks)
{
	std::map<std::string, int> deltas;

	for (const TrackFolderDepth& track : tracks) {
		deltas[track.identity] = track.folder_depth_delta;
	}

	return deltas;
}

// Each track's enclosing folders named by identity, outermost first.
std::map<std::string, std::vector<std::string>> folder_path_by_identity(
	const std::vector<TrackFolderDepth>& tracks)
{
	const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

	std::map<std::string, std::vector<std::string>> path_by_identity;

	for (std::size_t index = 0; index < tracks.size(); ++index) {
		std::vector<std::string> path;
		path.reserve(paths[index].size());

		for (const std::size_t folder_index : paths[index]) {
			path.push_back(tracks[folder_index].identity);
		}

		path_by_identity[tracks[index].identity] = std::move(path);
	}

	return path_by_identity;
}

// Which tracks are folder parents, read off the structure rather than off the
// deltas: a track is a parent exactly when some other track is inside it. Stating it
// structurally is what makes requirement 11.3's "no child is promoted into the
// parent's folder role" checkable — a promoted child is one that tracks are suddenly
// inside, whatever its delta happens to say.
std::set<std::string> folder_parent_identities(const std::vector<TrackFolderDepth>& tracks)
{
	std::set<std::string> parents;

	for (const std::pair<const std::string, std::vector<std::string>>& entry :
		folder_path_by_identity(tracks)) {
		for (const std::string& folder_identity : entry.second) {
			parents.insert(folder_identity);
		}
	}

	return parents;
}

std::vector<std::string> identities_of(const std::vector<TrackFolderDepth>& tracks)
{
	std::vector<std::string> identities;
	identities.reserve(tracks.size());

	for (const TrackFolderDepth& track : tracks) {
		identities.push_back(track.identity);
	}

	return identities;
}

int deepest_nesting_level(const std::vector<TrackFolderDepth>& tracks)
{
	const std::vector<int> levels = folder_nesting_levels(tracks);

	return levels.empty() ? 0 : *std::max_element(levels.begin(), levels.end());
}

// ---------------------------------------------------------------------------
// Generating track lists
// ---------------------------------------------------------------------------

constexpr int shape_open = 0;
constexpr int shape_flat = 1;
constexpr int shape_close = 2;

// A well-formed list from a sequence of shapes: a close with nothing open degrades
// to a plain sibling, and whatever is still open is closed on the last track, as
// REAPER's own project files do.
std::vector<TrackFolderDepth> well_formed_list_from_shapes(const std::vector<int>& shapes)
{
	std::vector<TrackFolderDepth> tracks;
	tracks.reserve(shapes.size());

	int open_count = 0;

	for (std::size_t index = 0; index < shapes.size(); ++index) {
		int folder_depth_delta = 0;

		if (shapes[index] == shape_open) {
			folder_depth_delta = 1;
			open_count += 1;
		} else if (shapes[index] == shape_close && open_count > 0) {
			folder_depth_delta = -1;
			open_count -= 1;
		}

		TrackFolderDepth track;
		track.identity = "t" + std::to_string(index);
		track.folder_depth_delta = folder_depth_delta;
		tracks.push_back(std::move(track));
	}

	if (open_count > 0 && !tracks.empty()) {
		tracks.back().folder_depth_delta -= open_count;
	}

	return tracks;
}

// Every well-formed list of one to `maximum_length` tracks, counted in base three
// over the three shapes. The same space the mirror file enumerates, because
// Property 3 is worth stating over all of it and not only over a sample.
std::vector<std::vector<TrackFolderDepth>> enumerate_well_formed_lists(std::size_t maximum_length)
{
	std::vector<std::vector<TrackFolderDepth>> lists;

	for (std::size_t length = 1; length <= maximum_length; ++length) {
		std::vector<int> shapes(length, 0);

		while (true) {
			lists.push_back(well_formed_list_from_shapes(shapes));

			// Odometer over base three, least significant position first.
			std::size_t position = 0;

			while (position < length) {
				shapes[position] += 1;

				if (shapes[position] < 3) {
					break;
				}

				shapes[position] = 0;
				position += 1;
			}

			if (position == length) {
				break;
			}
		}
	}

	return lists;
}

constexpr std::size_t enumeration_maximum_length = 6;
constexpr std::size_t enumerated_list_count = 1092;

constexpr std::size_t generated_maximum_length = 24;

// A well-formed list of up to `generated_maximum_length` tracks. One iteration in
// three is opening-heavy, which is the only way a walk over three equally likely
// shapes reliably reaches the nesting depths the exhaustive sweep cannot see at all.
// How deep it actually got is asserted by the callers rather than assumed.
std::vector<TrackFolderDepth> generate_well_formed_list(DeterministicBytes& bytes)
{
	const std::size_t length = 1 + bytes.below(generated_maximum_length);
	const bool opening_heavy = bytes.below(3) == 0;

	std::vector<int> shapes;
	shapes.reserve(length);

	for (std::size_t index = 0; index < length; ++index) {
		if (opening_heavy) {
			const std::size_t roll = bytes.below(10);
			shapes.push_back(roll < 7 ? shape_open : (roll < 9 ? shape_flat : shape_close));
		} else {
			shapes.push_back(static_cast<int>(bytes.below(3)));
		}
	}

	return well_formed_list_from_shapes(shapes);
}

// A list whose deltas are whatever they like — including the three things REAPER
// cannot represent and the repair exists to absorb: an opening delta above one, a
// close deeper than what is open, and folders left open at the end. Property 1
// quantifies over *any* track list, and this is the part of "any" that the
// exhaustive sweep excludes by construction.
std::vector<TrackFolderDepth> generate_arbitrary_delta_list(DeterministicBytes& bytes)
{
	static const std::vector<int> hostile_deltas{-9, -4, -3, -2, -1, 0, 0, 1, 1, 2, 3, 7};

	const std::size_t length = bytes.below(generated_maximum_length + 1);

	std::vector<TrackFolderDepth> tracks;
	tracks.reserve(length);

	for (std::size_t index = 0; index < length; ++index) {
		TrackFolderDepth track;
		track.identity = "t" + std::to_string(index);
		track.folder_depth_delta = hostile_deltas[bytes.below(hostile_deltas.size())];
		tracks.push_back(std::move(track));
	}

	return tracks;
}

std::vector<std::size_t> generate_permutation(DeterministicBytes& bytes, std::size_t size)
{
	std::vector<std::size_t> order(size);

	for (std::size_t index = 0; index < size; ++index) {
		order[index] = index;
	}

	for (std::size_t index = size; index > 1; --index) {
		std::swap(order[index - 1], order[bytes.below(index)]);
	}

	return order;
}

// ---------------------------------------------------------------------------
// Property 1: the depth deltas balance
// ---------------------------------------------------------------------------

// Requirement 11.1 in full: the deltas sum to zero across the list, no track's
// accumulated depth is negative, and — since REAPER opens at most one level per
// track — no delta claims to open more than one. Stated directly rather than through
// `folder_depth_deltas_are_well_formed`, so that a repair and a predicate that were
// wrong in the same way would not agree their way past it.
std::string balance_violation(const std::vector<TrackFolderDepth>& tracks)
{
	const std::vector<int> accumulated = accumulated_folder_depths(tracks);

	if (accumulated.size() != tracks.size()) {
		return "accumulated depth was not reported for every track";
	}

	for (std::size_t index = 0; index < tracks.size(); ++index) {
		if (tracks[index].folder_depth_delta > maximum_folder_depth_delta) {
			return "track " + tracks[index].identity + " opens "
				+ std::to_string(tracks[index].folder_depth_delta) + " levels at once";
		}

		if (accumulated[index] < 0) {
			return "accumulated depth after " + tracks[index].identity + " is "
				+ std::to_string(accumulated[index]);
		}
	}

	if (!tracks.empty() && accumulated.back() != 0) {
		return "the deltas sum to " + std::to_string(accumulated.back()) + " rather than zero";
	}

	// Belt and braces: the component's own predicate has to agree with the invariant
	// as read off requirement 11.1, or one of the two is wrong.
	if (!folder_depth_deltas_are_well_formed(tracks)) {
		return "the invariant holds but folder_depth_deltas_are_well_formed disagrees";
	}

	return "";
}

// ---------------------------------------------------------------------------
// Property 2: tracks the edit did not target keep the depth they sat at
// ---------------------------------------------------------------------------

// An insert targets nothing that already exists, so every existing track keeps not
// just its depth but its whole parentage — and, the claim that stops this from being
// satisfiable by an implementation that does something useless, the inserted track
// lands at exactly the depth of the position it was inserted at. A track dropped
// between a folder parent and its closing delta joins that folder, which is the
// whole reason `create_track` is classified destructive; an implementation that
// quietly kept it outside the folder instead would satisfy "nothing else moved" and
// still be wrong.
std::string insert_preservation_violation(
	const std::vector<TrackFolderDepth>& before,
	std::size_t insert_at,
	const std::string& inserted_identity,
	const std::vector<TrackFolderDepth>& after)
{
	const std::string balance = balance_violation(after);

	if (!balance.empty()) {
		return balance;
	}

	if (after.size() != before.size() + 1) {
		return "the insert left " + std::to_string(after.size()) + " tracks rather than "
			+ std::to_string(before.size() + 1);
	}

	const std::map<std::string, int> levels_before = nesting_level_by_identity(before);
	const std::map<std::string, int> levels_after = nesting_level_by_identity(after);
	const std::map<std::string, std::vector<std::string>> paths_before = folder_path_by_identity(before);
	const std::map<std::string, std::vector<std::string>> paths_after = folder_path_by_identity(after);
	const std::map<std::string, int> deltas_before = delta_by_identity(before);
	const std::map<std::string, int> deltas_after = delta_by_identity(after);

	for (const TrackFolderDepth& track : before) {
		if (levels_after.count(track.identity) == 0) {
			return "existing track " + track.identity + " disappeared";
		}

		if (levels_after.at(track.identity) != levels_before.at(track.identity)) {
			return "untargeted track " + track.identity + " moved from depth "
				+ std::to_string(levels_before.at(track.identity)) + " to "
				+ std::to_string(levels_after.at(track.identity));
		}

		if (paths_after.at(track.identity) != paths_before.at(track.identity)) {
			return "untargeted track " + track.identity + " changed folders";
		}

		// An insert into a well-formed list needs no repair at all, so an
		// implementation that rewrote a delta it was not asked to touch got there by
		// accident.
		if (deltas_after.at(track.identity) != deltas_before.at(track.identity)) {
			return "untargeted track " + track.identity + " had its delta rewritten";
		}
	}

	if (levels_after.count(inserted_identity) == 0) {
		return "the inserted track is missing";
	}

	// The depth of the insertion point: whatever the track now at that index sat at,
	// or zero past the end of a list whose folders have all closed.
	const int depth_of_the_insertion_point =
		insert_at < before.size() ? levels_before.at(before[insert_at].identity) : 0;

	if (levels_after.at(inserted_identity) != depth_of_the_insertion_point) {
		return "the inserted track landed at depth "
			+ std::to_string(levels_after.at(inserted_identity)) + " rather than "
			+ std::to_string(depth_of_the_insertion_point);
	}

	if (deltas_after.at(inserted_identity) != 0) {
		return "the inserted track is not a plain sibling";
	}

	return "";
}

// A delete targets one track and, when that track is a folder parent, the folder it
// was holding open. Everything else keeps the depth it sat at; everything that was
// inside the deleted track rises by exactly one level. That second half is what
// keeps the property from being true of an implementation that flattened the
// project, and it is checked for every track by identity, because a delete shifts
// every index after it.
std::string delete_preservation_violation(
	const std::vector<TrackFolderDepth>& before,
	std::size_t delete_at,
	const std::vector<TrackFolderDepth>& after)
{
	const std::string balance = balance_violation(after);

	if (!balance.empty()) {
		return balance;
	}

	if (after.size() + 1 != before.size()) {
		return "the delete did not remove exactly one track";
	}

	const std::string deleted_identity = before[delete_at].identity;
	const int deleted_delta = before[delete_at].folder_depth_delta;

	const std::map<std::string, int> levels_before = nesting_level_by_identity(before);
	const std::map<std::string, int> levels_after = nesting_level_by_identity(after);
	const std::map<std::string, std::vector<std::string>> paths_before = folder_path_by_identity(before);
	const std::map<std::string, int> deltas_before = delta_by_identity(before);

	if (levels_after.count(deleted_identity) != 0) {
		return "the deleted track " + deleted_identity + " survived";
	}

	std::size_t survivors_with_a_rewritten_delta = 0;

	for (const TrackFolderDepth& track : after) {
		if (levels_before.count(track.identity) == 0) {
			return "the delete invented a track called " + track.identity;
		}

		const std::vector<std::string>& path_before = paths_before.at(track.identity);
		const bool was_inside_the_deleted_track =
			std::find(path_before.begin(), path_before.end(), deleted_identity) != path_before.end();

		const int expected_level = was_inside_the_deleted_track
			? levels_before.at(track.identity) - 1
			: levels_before.at(track.identity);

		if (levels_after.at(track.identity) != expected_level) {
			return std::string(was_inside_the_deleted_track ? "former member " : "untargeted track ")
				+ track.identity + " is at depth "
				+ std::to_string(levels_after.at(track.identity)) + " rather than "
				+ std::to_string(expected_level);
		}

		if (track.folder_depth_delta != deltas_before.at(track.identity)) {
			survivors_with_a_rewritten_delta += 1;
		}
	}

	// "Changing as little as it can" is checkable too. Dissolving a folder moves the
	// delta that closed it; deleting a track that closes folders moves that close
	// onto the preceding track. Either way exactly one delta, and deleting a plain
	// sibling moves none at all.
	if (survivors_with_a_rewritten_delta > 1) {
		return std::to_string(survivors_with_a_rewritten_delta)
			+ " surviving deltas were rewritten for one delete";
	}

	if (deleted_delta == 0 && survivors_with_a_rewritten_delta != 0) {
		return "deleting a plain sibling rewrote a surviving delta";
	}

	return "";
}

// ---------------------------------------------------------------------------
// Property 3: deleting a folder parent dissolves the folder
// ---------------------------------------------------------------------------

// Requirement 11.3 makes two claims and they fail independently.
//
// Former children rise to the parent's *own* depth. Not to zero — a folder nested
// inside another dissolves into the enclosing folder, and an implementation that
// pulled its children out to the top level would pass any test written only against
// top-level folders.
//
// And no child is promoted into the parent's folder role. Carrying the opening delta
// onto the first child is the obvious repair and it is wrong: that child becomes the
// new folder parent and its former siblings end up inside it. The result is
// perfectly well formed and balances, so only a claim about structure catches it.
std::string dissolution_violation(
	const std::vector<TrackFolderDepth>& before,
	std::size_t parent_index,
	const std::vector<TrackFolderDepth>& after)
{
	const std::string preservation = delete_preservation_violation(before, parent_index, after);

	if (!preservation.empty()) {
		return preservation;
	}

	const std::string parent_identity = before[parent_index].identity;

	const std::map<std::string, int> levels_before = nesting_level_by_identity(before);
	const std::map<std::string, int> levels_after = nesting_level_by_identity(after);
	const std::map<std::string, std::vector<std::string>> paths_before = folder_path_by_identity(before);

	const int depth_the_parent_sat_at = levels_before.at(parent_identity);

	std::size_t direct_children = 0;

	for (const TrackFolderDepth& track : after) {
		const std::vector<std::string>& path_before = paths_before.at(track.identity);
		const bool was_a_direct_child = !path_before.empty() && path_before.back() == parent_identity;

		if (!was_a_direct_child) {
			continue;
		}

		direct_children += 1;

		if (levels_after.at(track.identity) != depth_the_parent_sat_at) {
			return "former child " + track.identity + " rose to depth "
				+ std::to_string(levels_after.at(track.identity)) + " rather than to "
				+ std::to_string(depth_the_parent_sat_at) + ", the depth " + parent_identity
				+ " sat at";
		}
	}

	if (direct_children == 0) {
		return "the dissolved folder " + parent_identity + " had no direct children to rise";
	}

	// The folder is gone and nothing took its place. Stated as a set so it catches
	// both ways of getting it wrong at once: a promoted child shows up as a parent
	// that was not one before, and a folder that outlived its own parent shows up as
	// the deleted identity still being one.
	std::set<std::string> expected_parents = folder_parent_identities(before);
	expected_parents.erase(parent_identity);

	const std::set<std::string> parents_after = folder_parent_identities(after);

	if (parents_after != expected_parents) {
		std::string report = "the folder parents after dissolving " + parent_identity + " are {";

		for (const std::string& identity : parents_after) {
			report += " " + identity;
		}

		report += " } rather than {";

		for (const std::string& identity : expected_parents) {
			report += " " + identity;
		}

		return report + " }";
	}

	return "";
}

// ---------------------------------------------------------------------------
// Deliberately wrong implementations, for the negative controls
// ---------------------------------------------------------------------------

// Flattens the whole project. The point of having it: "no folders anywhere" is
// trivially consistent with itself, so any property this satisfies is not
// constraining the repair at all.
std::vector<TrackFolderDepth> delete_flattening_every_folder(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_delete)
{
	tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(index_to_delete));

	for (TrackFolderDepth& track : tracks) {
		track.folder_depth_delta = 0;
	}

	return tracks;
}

std::vector<TrackFolderDepth> insert_flattening_every_folder(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_insert_at,
	const std::string& identity)
{
	for (TrackFolderDepth& track : tracks) {
		track.folder_depth_delta = 0;
	}

	tracks.insert(
		tracks.begin() + static_cast<std::ptrdiff_t>(index_to_insert_at),
		TrackFolderDepth{identity, 0});

	return tracks;
}

// The mistake requirement 11.3 names: hand the opening delta to the first child,
// which keeps the deltas balanced and re-parents every sibling underneath it.
std::vector<TrackFolderDepth> delete_promoting_the_first_child(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_delete)
{
	const int removed_delta = tracks[index_to_delete].folder_depth_delta;

	if (removed_delta > 0 && index_to_delete + 1 < tracks.size()) {
		tracks[index_to_delete + 1].folder_depth_delta += removed_delta;
	}

	tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(index_to_delete));

	return tracks;
}

// Dissolves the folder but drags its children out to the top level, which is the
// plausible misreading of "rise to the parent's own depth" — right for a top-level
// folder, wrong for a nested one.
std::vector<TrackFolderDepth> delete_pulling_children_to_the_top_level(
	std::vector<TrackFolderDepth> tracks,
	std::size_t index_to_delete)
{
	const std::vector<int> levels = folder_nesting_levels(tracks);
	const int depth_the_parent_sat_at = levels[index_to_delete];

	tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(index_to_delete));

	if (depth_the_parent_sat_at > 0 && !tracks.empty()) {
		tracks.front().folder_depth_delta -= depth_the_parent_sat_at;
		tracks.back().folder_depth_delta += depth_the_parent_sat_at;
	}

	return repair_folder_depth_deltas(std::move(tracks));
}

constexpr std::uint32_t balance_seed = 0xf01de201u;
constexpr std::uint32_t preservation_seed = 0x1b5e47c3u;
constexpr std::uint32_t dissolution_seed = 0x0dd5f1a9u;

// How many lists each property draws. Property 3 draws fewer because it checks every
// folder parent of every list it is handed, on top of the whole exhaustive space
// underneath, so its work per list is an order of magnitude larger. What each sweep
// actually covered is asserted at the end of the case rather than inferred from the
// count.
//
// Property 3's count is set by the coverage its own case demands rather than by
// symmetry with the other two: the exhaustive space contributes 1641 dissolutions,
// all of them in lists of at most six tracks, so everything the case claims about
// deep nesting has to come from the generated lists. Two thousand of them carry the
// dissolution count past ten thousand for around a second, which puts this among the
// handful of slowest cases in the suite and leaves the whole run near ten.
constexpr int balance_iterations = 3000;
constexpr int preservation_iterations = 3000;
constexpr int dissolution_iterations = 2000;

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("Property 1: depth deltas balance after any structural edit on any track list", "[daw][folder][property]")
{
	// The exhaustive sweep next door only ever hands the keeper lists that already
	// satisfy the invariant, where the repair is the identity. Here the input is
	// arbitrary, which is the quantifier the property actually claims: deltas above
	// one, closes with nothing open, folders left hanging at the end.
	DeterministicBytes bytes{balance_seed};

	std::string first_violation;
	int cases_checked = 0;
	int malformed_inputs = 0;
	int inputs_opening_more_than_one_level = 0;
	int inputs_closing_past_zero = 0;
	int deepest_level_seen = 0;
	std::size_t longest_list_seen = 0;

	const auto check = [&](const std::string& operation,
		const std::vector<TrackFolderDepth>& input,
		const std::vector<TrackFolderDepth>& result) {
		cases_checked += 1;

		const std::string violation = balance_violation(result);

		if (!violation.empty() && first_violation.empty()) {
			first_violation = describe_seed(balance_seed) + ", " + operation + " on "
				+ describe_track_list(input) + " gave " + describe_track_list(result) + ": "
				+ violation;
		}
	};

	for (int iteration = 0; iteration < balance_iterations; ++iteration) {
		// Half the iterations on arbitrary deltas, half on well-formed lists longer
		// and nested deeper than the exhaustive sweep can reach.
		const bool arbitrary = iteration % 2 == 0;
		const std::vector<TrackFolderDepth> tracks =
			arbitrary ? generate_arbitrary_delta_list(bytes) : generate_well_formed_list(bytes);

		if (!folder_depth_deltas_are_well_formed(tracks)) {
			malformed_inputs += 1;
		}

		for (const TrackFolderDepth& track : tracks) {
			if (track.folder_depth_delta > maximum_folder_depth_delta) {
				inputs_opening_more_than_one_level += 1;
				break;
			}
		}

		int accumulated = 0;

		for (const TrackFolderDepth& track : tracks) {
			accumulated += track.folder_depth_delta;

			if (accumulated < 0) {
				inputs_closing_past_zero += 1;
				break;
			}
		}

		deepest_level_seen = std::max(deepest_level_seen, deepest_nesting_level(tracks));
		longest_list_seen = std::max(longest_list_seen, tracks.size());

		// All three structural edits, since "any structural edit" is all of them, and
		// an index one past the end so the out-of-range path is balanced too.
		const std::size_t insert_at = bytes.below(tracks.size() + 2);
		check("insert at " + std::to_string(insert_at),
			tracks,
			insert_track_repairing_folders(tracks, insert_at, "inserted"));

		const std::size_t delete_at = bytes.below(tracks.size() + 2);
		check("delete at " + std::to_string(delete_at),
			tracks,
			delete_track_repairing_folders(tracks, delete_at));

		const std::vector<std::size_t> order = generate_permutation(bytes, tracks.size());
		const std::vector<TrackFolderDepth> reordered =
			reorder_tracks_repairing_folders(tracks, order);
		check("reorder", tracks, reordered);

		// And a reorder must not lose or duplicate a track on its way to balancing.
		std::vector<std::string> reordered_identities = identities_of(reordered);
		std::vector<std::string> original_identities = identities_of(tracks);
		std::sort(reordered_identities.begin(), reordered_identities.end());
		std::sort(original_identities.begin(), original_identities.end());

		if (reordered_identities != original_identities && first_violation.empty()) {
			first_violation = describe_seed(balance_seed) + ", reorder of "
				+ describe_track_list(tracks) + " changed the track set to "
				+ describe_track_list(reordered);
		}

		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);
		check("repair", tracks, repaired);

		// The repair is a fixpoint, which is what lets every operation above run it
		// unconditionally without changing what they mean.
		const std::vector<TrackFolderDepth> repaired_twice = repair_folder_depth_deltas(repaired);

		if (delta_by_identity(repaired_twice) != delta_by_identity(repaired)
			&& first_violation.empty()) {
			first_violation = describe_seed(balance_seed) + ", repairing "
				+ describe_track_list(tracks) + " twice gave "
				+ describe_track_list(repaired_twice) + " rather than "
				+ describe_track_list(repaired);
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Guards on the generator, for the same reason the exhaustive sweep asserts its
	// own size. Each number below is what keeps the property above from being
	// vacuous: a sweep that silently produced nothing but short, already valid lists
	// would satisfy all of it while checking none of it.
	REQUIRE(cases_checked == balance_iterations * 4);
	REQUIRE(malformed_inputs > 1000);
	REQUIRE(inputs_opening_more_than_one_level > 500);
	REQUIRE(inputs_closing_past_zero > 500);
	REQUIRE(longest_list_seen == generated_maximum_length);
	// Far deeper than six tracks over three shapes can nest, which is the point of
	// generating rather than enumerating.
	REQUIRE(deepest_level_seen >= 12);
}

TEST_CASE("Property 2: an edit leaves the depth of every track it did not target alone", "[daw][folder][property]")
{
	// Over longer and deeper lists than the exhaustive sweep reaches, and with the
	// target's own move asserted rather than only the absence of collateral movement.
	// The controls below are what that second half buys.
	DeterministicBytes bytes{preservation_seed};

	std::string first_violation;
	int inserts_checked = 0;
	int deletes_checked = 0;
	int inserts_landing_inside_a_folder = 0;
	int deletes_that_dissolved_a_folder = 0;
	int tracks_that_rose_a_level = 0;
	int deepest_level_seen = 0;

	for (int iteration = 0; iteration < preservation_iterations; ++iteration) {
		const std::vector<TrackFolderDepth> tracks = generate_well_formed_list(bytes);
		deepest_level_seen = std::max(deepest_level_seen, deepest_nesting_level(tracks));

		const std::size_t insert_at = bytes.below(tracks.size() + 1);
		const std::vector<TrackFolderDepth> inserted =
			insert_track_repairing_folders(tracks, insert_at, "inserted");
		inserts_checked += 1;

		const std::string insert_violation =
			insert_preservation_violation(tracks, insert_at, "inserted", inserted);

		if (!insert_violation.empty() && first_violation.empty()) {
			first_violation = describe_seed(preservation_seed) + ", insert at "
				+ std::to_string(insert_at) + " into " + describe_track_list(tracks) + " gave "
				+ describe_track_list(inserted) + ": " + insert_violation;
		}

		if (nesting_level_by_identity(inserted).at("inserted") > 0) {
			inserts_landing_inside_a_folder += 1;
		}

		const std::size_t delete_at = bytes.below(tracks.size());
		const std::vector<TrackFolderDepth> deleted =
			delete_track_repairing_folders(tracks, delete_at);
		deletes_checked += 1;

		const std::string delete_violation =
			delete_preservation_violation(tracks, delete_at, deleted);

		if (!delete_violation.empty() && first_violation.empty()) {
			first_violation = describe_seed(preservation_seed) + ", delete at "
				+ std::to_string(delete_at) + " from " + describe_track_list(tracks) + " gave "
				+ describe_track_list(deleted) + ": " + delete_violation;
		}

		// How much structure the delete actually moved. Without these counters the
		// property would be satisfied by an implementation that never moved anything,
		// which is exactly what the controls below are.
		const std::map<std::string, int> levels_before = nesting_level_by_identity(tracks);
		const std::map<std::string, int> levels_after = nesting_level_by_identity(deleted);
		bool a_track_rose = false;

		for (const TrackFolderDepth& track : deleted) {
			if (levels_after.at(track.identity) < levels_before.at(track.identity)) {
				tracks_that_rose_a_level += 1;
				a_track_rose = true;
			}
		}

		if (a_track_rose) {
			deletes_that_dissolved_a_folder += 1;
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	REQUIRE(inserts_checked == preservation_iterations);
	REQUIRE(deletes_checked == preservation_iterations);
	REQUIRE(deepest_level_seen >= 12);
	// The insert half of the property is a claim about a track landing inside a
	// folder, so the sweep has to contain plenty that do.
	REQUIRE(inserts_landing_inside_a_folder > 1000);
	// And the delete half is a claim about structure moving where it was targeted, so
	// the sweep has to contain plenty of deletes that moved some.
	REQUIRE(deletes_that_dissolved_a_folder > 500);
	REQUIRE(tracks_that_rose_a_level > deletes_that_dissolved_a_folder);
}

// The controls for Property 2. Kept as their own cases because Catch2 re-runs a case
// body once per section, and the sweep above is not worth running three times.
TEST_CASE("Property 2 rejects an implementation that flattens every folder", "[daw][folder][property]")
{
	// `drums` holds a folder over `kick` and `snare`. Deleting `bass`, which is
	// outside that folder and holds nothing open, must leave it exactly as it was —
	// and inserting into it must leave everyone's parentage alone.
	const std::vector<TrackFolderDepth> tracks{
		{"drums", 1},
		{"kick", 0},
		{"snare", -1},
		{"bass", 0},
	};

	REQUIRE(delete_preservation_violation(
		tracks, 3, delete_track_repairing_folders(tracks, 3)).empty());
	REQUIRE_FALSE(delete_preservation_violation(
		tracks, 3, delete_flattening_every_folder(tracks, 3)).empty());

	REQUIRE(insert_preservation_violation(
		tracks, 2, "inserted", insert_track_repairing_folders(tracks, 2, "inserted")).empty());
	REQUIRE_FALSE(insert_preservation_violation(
		tracks, 2, "inserted", insert_flattening_every_folder(tracks, 2, "inserted")).empty());
}

TEST_CASE("Property 2 rejects an insert that dodges the folder it landed in", "[daw][folder][property]")
{
	// Inserting between `drums` and the delta that closes it puts the new track
	// inside the folder. An implementation that opened a level on the inserted track
	// to keep it outside leaves every existing track exactly where it was and is
	// still wrong, which is why the inserted track's own depth is asserted.
	const std::vector<TrackFolderDepth> tracks{
		{"drums", 1},
		{"kick", -1},
		{"bass", 0},
	};

	const std::vector<TrackFolderDepth> dodged{
		{"drums", 1},
		{"inserted", -1},
		{"kick", 0},
		{"bass", 0},
	};

	REQUIRE(balance_violation(dodged).empty());
	REQUIRE(nesting_level_by_identity(dodged).at("kick") == 0);
	REQUIRE_FALSE(insert_preservation_violation(tracks, 1, "inserted", dodged).empty());
}

TEST_CASE("Property 3: deleting a folder parent dissolves the folder", "[daw][folder][property]")
{
	// Both halves of requirement 11.3, over every folder parent of every well-formed
	// list of up to six tracks, and then over generated lists long enough to nest far
	// deeper than that.
	const std::vector<std::vector<TrackFolderDepth>> lists =
		enumerate_well_formed_lists(enumeration_maximum_length);

	// 3 + 9 + 27 + 81 + 243 + 729. A sweep that silently stopped enumerating would
	// make everything below vacuous.
	REQUIRE(lists.size() == enumerated_list_count);

	std::string first_violation;
	int dissolutions_checked = 0;
	int dissolutions_of_a_nested_folder = 0;
	int dissolutions_three_or_more_deep = 0;
	int dissolutions_with_several_children = 0;
	int dissolutions_with_a_grandchild = 0;
	int deepest_level_seen = 0;

	const auto check_every_folder_parent = [&](const std::vector<TrackFolderDepth>& tracks,
		const std::string& origin) {
		const std::set<std::string> parents = folder_parent_identities(tracks);
		const std::map<std::string, int> levels = nesting_level_by_identity(tracks);
		const std::map<std::string, std::vector<std::string>> paths = folder_path_by_identity(tracks);

		deepest_level_seen = std::max(deepest_level_seen, deepest_nesting_level(tracks));

		for (std::size_t parent_index = 0; parent_index < tracks.size(); ++parent_index) {
			const std::string parent_identity = tracks[parent_index].identity;

			if (parents.count(parent_identity) == 0) {
				continue;
			}

			dissolutions_checked += 1;

			const int depth_the_parent_sat_at = levels.at(parent_identity);

			if (depth_the_parent_sat_at >= 1) {
				dissolutions_of_a_nested_folder += 1;
			}

			if (depth_the_parent_sat_at >= 3) {
				dissolutions_three_or_more_deep += 1;
			}

			int direct_children = 0;
			int grandchildren = 0;

			for (const TrackFolderDepth& track : tracks) {
				const std::vector<std::string>& path = paths.at(track.identity);

				if (std::find(path.begin(), path.end(), parent_identity) == path.end()) {
					continue;
				}

				if (path.back() == parent_identity) {
					direct_children += 1;
				} else {
					grandchildren += 1;
				}
			}

			if (direct_children > 1) {
				dissolutions_with_several_children += 1;
			}

			if (grandchildren > 0) {
				dissolutions_with_a_grandchild += 1;
			}

			const std::string violation = dissolution_violation(
				tracks, parent_index, delete_track_repairing_folders(tracks, parent_index));

			if (!violation.empty() && first_violation.empty()) {
				first_violation = origin + ": deleting " + parent_identity + " from "
					+ describe_track_list(tracks) + " — " + violation;
			}
		}
	};

	for (const std::vector<TrackFolderDepth>& tracks : lists) {
		check_every_folder_parent(tracks, "exhaustive sweep");
	}

	DeterministicBytes bytes{dissolution_seed};

	for (int iteration = 0; iteration < dissolution_iterations; ++iteration) {
		check_every_folder_parent(
			generate_well_formed_list(bytes),
			describe_seed(dissolution_seed) + " iteration " + std::to_string(iteration));
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// The coverage the two claims need. Nesting is the one that matters most: "rise to
	// the parent's own depth" and "rise to the top level" are the same sentence until
	// the folder being dissolved is itself inside something.
	//
	// The corpus above dissolves 12144 folders — 1641 of them from the exhaustive
	// space, the rest from the 2000 generated lists — so each bound below sits a
	// comfortable way under what the sweep actually reaches while still being far
	// enough above zero to fail loudly if the generator stopped producing folder
	// parents, or stopped nesting them, and made the property vacuous. Every number
	// here is deterministic: the seed is fixed, so these are floors on a known
	// quantity rather than odds.
	REQUIRE(dissolutions_checked > 10000);
	REQUIRE(dissolutions_of_a_nested_folder > 6000);
	REQUIRE(dissolutions_three_or_more_deep > 3000);
	REQUIRE(dissolutions_with_several_children > 3000);
	REQUIRE(dissolutions_with_a_grandchild > 5000);
	REQUIRE(deepest_level_seen >= 12);
}

// The controls for Property 3, one per way of getting requirement 11.3 wrong.
TEST_CASE("Property 3 rejects promoting the first child into the parent's folder role", "[daw][folder][property]")
{
	// The repair that looks right and silently re-parents the siblings.
	const std::vector<TrackFolderDepth> tracks{
		{"drums", 1},
		{"kick", 0},
		{"snare", -1},
		{"bass", 0},
	};

	const std::vector<TrackFolderDepth> promoted = delete_promoting_the_first_child(tracks, 0);

	// It balances, so requirement 11.1 on its own would let it through.
	REQUIRE(balance_violation(promoted).empty());
	// And `snare` is now inside `kick`, which nobody asked for.
	REQUIRE(folder_parent_identities(promoted) == std::set<std::string>{"kick"});
	REQUIRE_FALSE(dissolution_violation(tracks, 0, promoted).empty());

	REQUIRE(dissolution_violation(tracks, 0, delete_track_repairing_folders(tracks, 0)).empty());
}

TEST_CASE("Property 3 rejects pulling the children of a nested folder to the top level", "[daw][folder][property]")
{
	// `inner` sits inside `outer`, so dissolving it leaves its children at depth one.
	// An implementation that pulled them out to the top level instead satisfies every
	// claim a test written only against top-level folders could make.
	const std::vector<TrackFolderDepth> tracks{
		{"outer", 1},
		{"inner", 1},
		{"leaf", 0},
		{"twig", -2},
		{"tail", 0},
	};

	REQUIRE(folder_nesting_levels(tracks) == std::vector<int>{0, 1, 2, 2, 0});

	const std::vector<TrackFolderDepth> pulled_out =
		delete_pulling_children_to_the_top_level(tracks, 1);

	REQUIRE(balance_violation(pulled_out).empty());
	REQUIRE_FALSE(dissolution_violation(tracks, 1, pulled_out).empty());

	const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 1);

	REQUIRE(dissolution_violation(tracks, 1, repaired).empty());
	// Spelled out, since this is the case the property exists for: both former
	// children rise to depth one, still inside `outer`, and neither becomes a folder
	// parent.
	REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 1, 1, 0});
	REQUIRE(folder_parent_identities(repaired) == std::set<std::string>{"outer"});
}

TEST_CASE("Property 3 rejects flattening the project instead of dissolving one folder", "[daw][folder][property]")
{
	const std::vector<TrackFolderDepth> tracks{
		{"outer", 1},
		{"inner", 1},
		{"leaf", -2},
		{"tail", 0},
	};

	REQUIRE_FALSE(dissolution_violation(tracks, 1, delete_flattening_every_folder(tracks, 1)).empty());
}
