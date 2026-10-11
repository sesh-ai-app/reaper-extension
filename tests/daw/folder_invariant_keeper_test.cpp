// Folder Invariant Keeper — the folder depth arithmetic the server's tests specify.
//
// This file mirrors `server/mcp-server/src/lib/folder-invariant.property.test.js`,
// which states what the delta arithmetic has to preserve as a model the C++
// implementation is obliged to satisfy. Every case there has a case here, named
// the same way, asserting the same thing — including the negative control,
// without which the preservation tests would pass against an implementation that
// simply flattened every folder, since "no folders anywhere" is trivially
// consistent with itself.
//
// Where the server file draws its track lists from fast-check, this file
// enumerates them. Catch2 has no generator library in this build, and for lists
// of up to six tracks over three shapes the whole space is 1092 lists — cheap
// enough to check exhaustively, which is a stronger statement than sampling and a
// deterministic one. Task 6.2 layers the formal property tests on top; these are
// the examples and the exhaustive sweep underneath them.
//
// Nothing here touches REAPER. That is the point of the component: the repair is
// sequence arithmetic, so it can be wrong in ways only arithmetic tests find.

#include <algorithm>
#include <cstddef>
#include <map>
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
using sesh_ai::daw::reconstruct_folder_paths;
using sesh_ai::daw::reorder_tracks_repairing_folders;
using sesh_ai::daw::repair_folder_depth_deltas;

namespace {

// Folder paths named by track identity rather than by index, which is the
// comparison that survives the shift an insert or a delete causes.
std::vector<std::vector<std::string>> folder_path_identities(const std::vector<TrackFolderDepth>& tracks)
{
	const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

	std::vector<std::vector<std::string>> identities;
	identities.reserve(paths.size());

	for (const std::vector<std::size_t>& path : paths) {
		std::vector<std::string> path_identities;
		path_identities.reserve(path.size());

		for (const std::size_t folder_index : path) {
			path_identities.push_back(tracks[folder_index].identity);
		}

		identities.push_back(std::move(path_identities));
	}

	return identities;
}

std::vector<int> deltas_of(const std::vector<TrackFolderDepth>& tracks)
{
	std::vector<int> deltas;
	deltas.reserve(tracks.size());

	for (const TrackFolderDepth& track : tracks) {
		deltas.push_back(track.folder_depth_delta);
	}

	return deltas;
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

// One track list from a sequence of shapes — the C++ counterpart of the server
// file's `trackListArbitrary`, built the same way so the two suites are checking
// the same space. A close on a track with nothing open degrades to a flat track,
// and whatever is still open is closed on the last track, as REAPER's own project
// files do.
std::vector<TrackFolderDepth> track_list_from_shapes(const std::vector<int>& shapes)
{
	// 0 opens a folder, 1 is a plain sibling, 2 closes one level.
	constexpr int shape_open = 0;
	constexpr int shape_close = 2;

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

// Every well-formed track list of one to `maximum_length` tracks.
std::vector<std::vector<TrackFolderDepth>> enumerate_track_lists(std::size_t maximum_length)
{
	std::vector<std::vector<TrackFolderDepth>> lists;

	for (std::size_t length = 1; length <= maximum_length; ++length) {
		std::vector<int> shapes(length, 0);

		while (true) {
			lists.push_back(track_list_from_shapes(shapes));

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

}  // namespace

TEST_CASE("the enumerated track lists are well formed and cover the space", "[daw][folder]")
{
	const std::vector<std::vector<TrackFolderDepth>> lists = enumerate_track_lists(enumeration_maximum_length);

	// 3 + 9 + 27 + 81 + 243 + 729. A guard on the generator itself: a sweep that
	// silently stopped enumerating would make every test below vacuously true.
	REQUIRE(lists.size() == 1092);

	for (const std::vector<TrackFolderDepth>& tracks : lists) {
		REQUIRE(folder_depth_deltas_are_well_formed(tracks));
	}

	// And the space is not all flat tracks — otherwise the folder cases below
	// would never see a folder.
	const bool some_list_has_a_folder = std::any_of(
		lists.begin(),
		lists.end(),
		[](const std::vector<TrackFolderDepth>& tracks) {
			const std::vector<int> levels = folder_nesting_levels(tracks);
			return std::any_of(levels.begin(), levels.end(), [](int level) { return level > 0; });
		});

	REQUIRE(some_list_has_a_folder);

	// Including nesting two deep, which is where the multi-level closing deltas
	// that make this arithmetic awkward actually appear.
	const bool some_list_nests_twice = std::any_of(
		lists.begin(),
		lists.end(),
		[](const std::vector<TrackFolderDepth>& tracks) {
			const std::vector<int> levels = folder_nesting_levels(tracks);
			return std::any_of(levels.begin(), levels.end(), [](int level) { return level > 1; });
		});

	REQUIRE(some_list_nests_twice);
}

TEST_CASE("reconstructs a folder path for every track", "[daw][folder]")
{
	for (const std::vector<TrackFolderDepth>& tracks : enumerate_track_lists(enumeration_maximum_length)) {
		const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

		REQUIRE(paths.size() == tracks.size());

		for (std::size_t index = 0; index < paths.size(); ++index) {
			// A track's enclosing folders all precede it, and none is itself.
			for (const std::size_t folder_index : paths[index]) {
				REQUIRE(folder_index < index);
			}

			// The path is the accumulated depth the track sits at, stated the other
			// way round. If these two ever disagree the invariant is unstateable.
			REQUIRE(static_cast<int>(paths[index].size()) == folder_nesting_levels(tracks)[index]);
		}
	}
}

// Inserting is the case that looks harmless and is not: a track added inside a
// folder is the reason `create_track` is classified destructive.
TEST_CASE("leaves every existing track parented as it was when a flat track is inserted", "[daw][folder]")
{
	for (const std::vector<TrackFolderDepth>& tracks : enumerate_track_lists(enumeration_maximum_length)) {
		const std::vector<std::vector<std::string>> before = folder_path_identities(tracks);

		for (std::size_t insert_at = 0; insert_at <= tracks.size(); ++insert_at) {
			const std::vector<TrackFolderDepth> inserted =
				insert_track_repairing_folders(tracks, insert_at, "inserted");

			REQUIRE(inserted.size() == tracks.size() + 1);
			REQUIRE(inserted[insert_at].identity == "inserted");
			REQUIRE(folder_depth_deltas_are_well_formed(inserted));

			const std::vector<std::vector<std::string>> after = folder_path_identities(inserted);

			// Compared by track identity, since indices shift by the insertion.
			for (std::size_t original_index = 0; original_index < tracks.size(); ++original_index) {
				const std::size_t moved_index =
					original_index < insert_at ? original_index : original_index + 1;

				REQUIRE(after[moved_index] == before[original_index]);
			}
		}
	}
}

TEST_CASE("leaves the surviving tracks parented as they were when one is deleted", "[daw][folder]")
{
	for (const std::vector<TrackFolderDepth>& tracks : enumerate_track_lists(enumeration_maximum_length)) {
		if (tracks.size() < 2) {
			continue;
		}

		const std::vector<std::vector<std::string>> before = folder_path_identities(tracks);

		std::map<std::string, std::vector<std::string>> before_by_identity;

		for (std::size_t index = 0; index < tracks.size(); ++index) {
			before_by_identity[tracks[index].identity] = before[index];
		}

		for (std::size_t delete_at = 0; delete_at < tracks.size(); ++delete_at) {
			const std::string deleted_identity = tracks[delete_at].identity;

			const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, delete_at);
			const std::vector<std::vector<std::string>> after = folder_path_identities(repaired);

			REQUIRE(repaired.size() == tracks.size() - 1);
			REQUIRE(folder_depth_deltas_are_well_formed(repaired));

			// Every survivor keeps the same enclosing folders, named by identity
			// rather than by index so the comparison survives the shift. The deleted
			// track drops out of the paths of whatever it used to enclose, and
			// changes nothing else.
			for (std::size_t index = 0; index < repaired.size(); ++index) {
				std::vector<std::string> expected = before_by_identity[repaired[index].identity];
				expected.erase(
					std::remove(expected.begin(), expected.end(), deleted_identity),
					expected.end());

				REQUIRE(after[index] == expected);
			}
		}
	}
}

// The negative control, carried over from the server file. Without it the two
// tests above could pass against an implementation that flattened everything,
// since "no folders anywhere" is trivially consistent with itself.
TEST_CASE("detects the corruption an unrepaired delete causes", "[daw][folder]")
{
	// A folder parent, one child, and a track after the folder closes.
	const std::vector<TrackFolderDepth> tracks{
		{"drums", 1},
		{"kick", -1},
		{"bass", 0},
	};

	REQUIRE(folder_nesting_levels(tracks) == std::vector<int>{0, 1, 0});

	// Naively dropping the folder parent without moving its delta: the closing
	// delta on `kick` now closes a folder that was never opened, and `bass` is
	// pulled to a level it did not ask for.
	std::vector<TrackFolderDepth> naive = tracks;
	naive.erase(naive.begin());

	REQUIRE(deltas_of(naive) == std::vector<int>{-1, 0});
	REQUIRE_FALSE(folder_depth_deltas_are_well_formed(naive));

	const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 0);

	REQUIRE(deltas_of(repaired) == std::vector<int>{0, 0});
	REQUIRE(identities_of(repaired) == std::vector<std::string>{"kick", "bass"});
}

TEST_CASE("dissolves the folder when its parent is deleted rather than promoting a child", "[daw][folder]")
{
	SECTION("a top level folder with two children")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 1},
			{"kick", 0},
			{"snare", -1},
			{"bass", 0},
		};

		const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 0);

		// Former children rise to the parent's own depth — which was zero — and no
		// child inherits the opening delta, so neither `kick` nor `snare` becomes
		// the new folder parent.
		REQUIRE(identities_of(repaired) == std::vector<std::string>{"kick", "snare", "bass"});
		REQUIRE(deltas_of(repaired) == std::vector<int>{0, 0, 0});
		REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 0, 0});
	}

	SECTION("a nested folder rises to the depth its parent sat at, not to zero")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"outer", 1},
			{"inner", 1},
			{"leaf", -2},
			{"tail", 0},
		};

		REQUIRE(folder_nesting_levels(tracks) == std::vector<int>{0, 1, 2, 0});

		const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 1);

		// `inner` sat at depth one, so `leaf` rises to one — still inside `outer`,
		// which the operation was not asked to touch. `tail` stays outside.
		REQUIRE(identities_of(repaired) == std::vector<std::string>{"outer", "leaf", "tail"});
		REQUIRE(deltas_of(repaired) == std::vector<int>{1, -1, 0});
		REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 1, 0});
	}

	SECTION("deleting a track that closes folders keeps those folders closing")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"outer", 1},
			{"inner", 1},
			{"leaf", 0},
			{"last", -2},
			{"tail", 0},
		};

		const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 3);

		// The closing delta transfers to `leaf`, which becomes the last member of
		// both folders. `tail` is not pulled in.
		REQUIRE(identities_of(repaired) == std::vector<std::string>{"outer", "inner", "leaf", "tail"});
		REQUIRE(deltas_of(repaired) == std::vector<int>{1, 1, -2, 0});
		REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 1, 2, 0});
	}

	SECTION("a folder whose only child is deleted stops being a folder")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 1},
			{"kick", -1},
			{"bass", 0},
		};

		const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 1);

		REQUIRE(identities_of(repaired) == std::vector<std::string>{"drums", "bass"});
		REQUIRE(deltas_of(repaired) == std::vector<int>{0, 0});
		REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 0});
	}
}

TEST_CASE("preserves the accumulated depth of tracks outside the one being changed", "[daw][folder]")
{
	// Requirement 11.2 read at its narrowest: a delete inside one folder must not
	// move a track in a different folder, and a track enclosed by the deleted
	// parent rises by exactly one level and no more.
	const std::vector<TrackFolderDepth> tracks{
		{"drums", 1},
		{"kick", 0},
		{"snare", -1},
		{"guitars", 1},
		{"rhythm", 0},
		{"lead", -1},
		{"bass", 0},
	};

	REQUIRE(folder_nesting_levels(tracks) == std::vector<int>{0, 1, 1, 0, 1, 1, 0});

	const std::vector<TrackFolderDepth> repaired = delete_track_repairing_folders(tracks, 0);

	REQUIRE(identities_of(repaired)
		== std::vector<std::string>{"kick", "snare", "guitars", "rhythm", "lead", "bass"});
	REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 0, 0, 1, 1, 0});
}

TEST_CASE("repairing a well formed track list changes nothing", "[daw][folder]")
{
	// The operations above run the repair unconditionally, so this is what makes
	// their results identical to the model the server's tests specify rather than
	// merely compatible with it.
	for (const std::vector<TrackFolderDepth>& tracks : enumerate_track_lists(enumeration_maximum_length)) {
		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);

		REQUIRE(deltas_of(repaired) == deltas_of(tracks));
		REQUIRE(identities_of(repaired) == identities_of(tracks));
	}
}

TEST_CASE("repairs deltas so that they sum to zero and no accumulated depth is negative", "[daw][folder]")
{
	SECTION("an unclosed folder is closed on the last track")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 1},
			{"kick", 0},
		};

		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);

		REQUIRE(deltas_of(repaired) == std::vector<int>{1, -1});
		REQUIRE(folder_depth_deltas_are_well_formed(repaired));
	}

	SECTION("a folder with no children stops claiming to be one")
	{
		const std::vector<TrackFolderDepth> tracks{{"drums", 1}};

		REQUIRE(deltas_of(repair_folder_depth_deltas(tracks)) == std::vector<int>{0});
	}

	SECTION("a close with nothing open is shortened rather than driving the depth negative")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"kick", -1},
			{"bass", 0},
		};

		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);

		REQUIRE(deltas_of(repaired) == std::vector<int>{0, 0});
		REQUIRE(folder_depth_deltas_are_well_formed(repaired));
	}

	SECTION("a close that overshoots closes only what is open")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 1},
			{"kick", -3},
			{"bass", 0},
		};

		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);

		REQUIRE(deltas_of(repaired) == std::vector<int>{1, -1, 0});
		REQUIRE(folder_nesting_levels(repaired) == std::vector<int>{0, 1, 0});
	}

	SECTION("a delta above one is clamped, since REAPER opens at most one level per track")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 3},
			{"kick", 0},
			{"bass", 0},
		};

		const std::vector<TrackFolderDepth> repaired = repair_folder_depth_deltas(tracks);

		REQUIRE(deltas_of(repaired) == std::vector<int>{1, 0, -1});
		REQUIRE(folder_depth_deltas_are_well_formed(repaired));
	}

	SECTION("an empty track list is already repaired")
	{
		const std::vector<TrackFolderDepth> tracks;

		REQUIRE(repair_folder_depth_deltas(tracks).empty());
		REQUIRE(folder_depth_deltas_are_well_formed(tracks));
	}
}

TEST_CASE("a reorder leaves the deltas summing to zero with no negative accumulated depth", "[daw][folder]")
{
	// The permutation decides parentage — nothing in a reorder says which folders
	// were meant to survive it — so what the keeper owes requirement 11.1 is that
	// the result is a structure REAPER can represent at all.
	for (const std::vector<TrackFolderDepth>& tracks : enumerate_track_lists(4)) {
		std::vector<std::size_t> order(tracks.size());

		for (std::size_t index = 0; index < order.size(); ++index) {
			order[index] = index;
		}

		do {
			const std::vector<TrackFolderDepth> reordered = reorder_tracks_repairing_folders(tracks, order);

			REQUIRE(reordered.size() == tracks.size());
			REQUIRE(folder_depth_deltas_are_well_formed(reordered));

			const std::vector<int> depths = accumulated_folder_depths(reordered);

			for (const int depth : depths) {
				REQUIRE(depth >= 0);
			}

			// The reorder moves tracks; it does not invent or lose any.
			std::vector<std::string> reordered_identities = identities_of(reordered);
			std::vector<std::string> original_identities = identities_of(tracks);
			std::sort(reordered_identities.begin(), reordered_identities.end());
			std::sort(original_identities.begin(), original_identities.end());

			REQUIRE(reordered_identities == original_identities);
		} while (std::next_permutation(order.begin(), order.end()));
	}

	SECTION("an order that is not a permutation is refused rather than half applied")
	{
		const std::vector<TrackFolderDepth> tracks{
			{"drums", 1},
			{"kick", -1},
			{"bass", 0},
		};

		REQUIRE(identities_of(reorder_tracks_repairing_folders(tracks, {0, 0, 1}))
			== identities_of(tracks));
		REQUIRE(identities_of(reorder_tracks_repairing_folders(tracks, {0, 1}))
			== identities_of(tracks));
		REQUIRE(identities_of(reorder_tracks_repairing_folders(tracks, {0, 1, 7}))
			== identities_of(tracks));
	}
}
