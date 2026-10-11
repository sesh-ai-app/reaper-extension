// Track structure tools — `create_track`, `delete_track`, `duplicate_track`, and
// `set_folder_structure`.
//
// These four tools are the only ones that change the shape of the track list, and the
// shape is the one piece of REAPER state with no redundancy to check against: nesting
// exists purely as the running total of a delta per track, so a wrong delta is not an
// error REAPER reports, it is a project where the producer's drums are now inside
// their vocals. Every test below is built around trying to reach one of four states:
//
//   - Deltas that do not sum to zero, or a track at a negative accumulated depth
//     (requirement 11.1).
//   - A track the call never named sitting inside a different set of folders
//     afterwards (requirement 11.2).
//   - A former child promoted into a deleted folder parent's role (requirement 11.3).
//   - An action reported as failed with no reason, or a sibling's failure undoing what
//     already landed (requirements 9.4, 9.5).
//
// The invariant assertions call the Folder Invariant Keeper's own checking functions
// rather than re-deriving the arithmetic. That is deliberate and it is the only way
// the assertion means anything: a hand-written depth walk in the test would be a
// fourth implementation of the thing under test, and it would agree with the code it
// was written beside rather than with the contract.
//
// Comparisons of structure are made on folder *paths* named by identity, never on
// deltas and never on indices. A correct repair changes deltas on purpose and every
// operation here shifts indices, so those two are the wrong things to diff; the list
// of folders enclosing a track is what "the structure did not change" actually means,
// and the Keeper's `reconstruct_folder_paths` is what answers it.
//
// The scripted project below is what makes all of this checkable without REAPER: it
// applies the same edits REAPER would, including the parts that matter for the
// arithmetic — an inserted track arrives with REAPER's default delta of zero, and a
// duplicated track arrives carrying the delta of the track it was copied from.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/track_structure_tools.h>

using sesh_ai::daw::accumulated_folder_depths;
using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_succeeded;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::folder_depth_deltas_are_well_formed;
using sesh_ai::daw::folder_nesting_levels;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::reconstruct_folder_paths;
using sesh_ai::daw::repair_folder_depth_deltas;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::ResolvedTrack;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_registration_outcome;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackFolderDepth;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::undo_block_report;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;

using sesh_ai::daw::tools::apply_create_track;
using sesh_ai::daw::tools::apply_delete_track;
using sesh_ai::daw::tools::apply_duplicate_track;
using sesh_ai::daw::tools::apply_set_folder_structure;
using sesh_ai::daw::tools::attach_subtree_after;
using sesh_ai::daw::tools::contradictory_folder_destination_code;
using sesh_ai::daw::tools::create_track_request;
using sesh_ai::daw::tools::create_track_result;
using sesh_ai::daw::tools::create_track_tool_name;
using sesh_ai::daw::tools::delete_track_request;
using sesh_ai::daw::tools::delete_track_result;
using sesh_ai::daw::tools::delete_track_tool_name;
using sesh_ai::daw::tools::duplicate_track_request;
using sesh_ai::daw::tools::duplicate_track_result;
using sesh_ai::daw::tools::duplicate_track_tool_name;
using sesh_ai::daw::tools::extract_subtree;
using sesh_ai::daw::tools::find_folder_span;
using sesh_ai::daw::tools::folder_depth_repair;
using sesh_ai::daw::tools::folder_depths_of;
using sesh_ai::daw::tools::folder_destination_not_named_code;
using sesh_ai::daw::tools::folder_target_inside_moved_track_code;
using sesh_ai::daw::tools::folder_target_is_the_moved_track_code;
using sesh_ai::daw::tools::index_of_track;
using sesh_ai::daw::tools::read_balanced_subtree;
using sesh_ai::daw::tools::register_track_structure_tools;
using sesh_ai::daw::tools::set_folder_structure_request;
using sesh_ai::daw::tools::set_folder_structure_result;
using sesh_ai::daw::tools::set_folder_structure_tool_name;
using sesh_ai::daw::tools::structural_outcome;
using sesh_ai::daw::tools::structural_track;
using sesh_ai::daw::tools::track_duplicate_failed_code;
using sesh_ai::daw::tools::track_insert_failed_code;
using sesh_ai::daw::tools::track_reorder_failed_code;
using sesh_ai::daw::tools::track_structure_payload_codec;
using sesh_ai::daw::tools::TrackStructureHost;
using sesh_ai::daw::tools::unknown_track_code;

namespace
{
	// ---------------------------------------------------------------------------
	// Building a project
	// ---------------------------------------------------------------------------

	// REAPER's braced GUID form, which every output schema's `guid` pattern expects.
	std::string guid_for(int distinguishing_number)
	{
		std::string digits = std::to_string(distinguishing_number);

		while (digits.size() < 12)
		{
			digits.insert(digits.begin(), '0');
		}

		return "{00000000-0000-0000-0000-" + digits + "}";
	}

	structural_track track_of(std::string name, int folder_depth_delta, int distinguishing_number)
	{
		structural_track track;
		track.guid = guid_for(distinguishing_number);
		track.name = std::move(name);
		track.folder_depth_delta = folder_depth_delta;

		return track;
	}

	// A project built from name-and-delta pairs, with GUIDs minted in track order. The
	// deltas are written the way REAPER stores them: `1` opens a folder, `0` is a
	// sibling, a negative value closes that many levels.
	std::vector<structural_track> project_of(
		std::vector<std::pair<std::string, int>> names_and_deltas)
	{
		std::vector<structural_track> tracks;
		int next_number = 1;

		for (std::pair<std::string, int>& entry : names_and_deltas)
		{
			tracks.push_back(track_of(std::move(entry.first), entry.second, next_number));
			++next_number;
		}

		return tracks;
	}

	std::string guid_of(const std::vector<structural_track>& tracks, std::string_view name)
	{
		for (const structural_track& track : tracks)
		{
			if (track.name == name)
			{
				return track.guid;
			}
		}

		return {};
	}

	// ---------------------------------------------------------------------------
	// Invariant assertions, all of them the Keeper's own
	// ---------------------------------------------------------------------------

	// Requirement 11.1. `folder_depth_deltas_are_well_formed` is the Keeper's own
	// statement of the invariant — deltas sum to zero, no accumulated depth negative,
	// nothing opening more than one level — and it is what the server's property tests
	// pin down. The per-track loop follows it so that a failure names which track went
	// negative rather than only that something did.
	void require_folder_invariants(const std::vector<TrackFolderDepth>& tracks)
	{
		REQUIRE(folder_depth_deltas_are_well_formed(tracks));

		const std::vector<int> depths = accumulated_folder_depths(tracks);

		for (std::size_t index = 0; index < depths.size(); ++index)
		{
			INFO("track " << index << " is " << tracks[index].identity);
			REQUIRE(depths[index] >= 0);
		}
	}

	void require_folder_invariants(const std::vector<structural_track>& tracks)
	{
		require_folder_invariants(folder_depths_of(tracks));
	}

	// Every track's folder path, named by identity rather than by index.
	//
	// The only comparison the invariant can be stated in, for the reason the Keeper's
	// own header gives: two track lists have the same structure exactly when every
	// track's path matches, and indices shift under every operation here.
	std::map<std::string, std::vector<std::string>> folder_paths_by_identity(
		const std::vector<TrackFolderDepth>& tracks)
	{
		const std::vector<std::vector<std::size_t>> paths = reconstruct_folder_paths(tracks);

		std::map<std::string, std::vector<std::string>> by_identity;

		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			std::vector<std::string> path;
			path.reserve(paths[index].size());

			for (const std::size_t folder_index : paths[index])
			{
				path.push_back(tracks[folder_index].identity);
			}

			by_identity[tracks[index].identity] = std::move(path);
		}

		return by_identity;
	}

	std::map<std::string, std::vector<std::string>> folder_paths_by_identity(
		const std::vector<structural_track>& tracks)
	{
		return folder_paths_by_identity(folder_depths_of(tracks));
	}

	std::map<std::string, int> nesting_levels_by_identity(const std::vector<structural_track>& tracks)
	{
		const std::vector<TrackFolderDepth> depths = folder_depths_of(tracks);
		const std::vector<int> levels = folder_nesting_levels(depths);

		std::map<std::string, int> by_identity;

		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			by_identity[tracks[index].guid] = levels[index];
		}

		return by_identity;
	}

	// Every identity in the run rooted at each named track: the track itself, and
	// everything inside its folder when it is a folder parent.
	//
	// Needed because "the tracks the call named" is not the same set as "the tracks the
	// call was allowed to move". Moving or duplicating a folder parent moves its
	// children by construction — a folder parent is not one track — so its descendants
	// are named implicitly and are excluded from the untargeted check.
	std::vector<std::string> with_subtrees(
		const std::vector<structural_track>& tracks,
		const std::vector<std::string>& guids)
	{
		const std::vector<TrackFolderDepth> depths = folder_depths_of(tracks);

		std::vector<std::string> named = guids;

		for (const std::string& guid : guids)
		{
			const std::optional<std::size_t> index = index_of_track(tracks, guid);

			if (!index.has_value())
			{
				continue;
			}

			const auto span = find_folder_span(depths, *index);

			for (std::size_t inside = span.first_index; inside <= span.last_index; ++inside)
			{
				named.push_back(tracks[inside].guid);
			}
		}

		return named;
	}

	// Requirement 11.2. A track the operation was not asked to change is inside exactly
	// the folders it was inside before.
	//
	// Stated on paths rather than on deltas because a repair changes deltas on purpose:
	// a delta diff would fail on every correct repair, and would therefore have to be
	// weakened until it checked nothing.
	void require_untargeted_tracks_keep_their_folders(
		const std::vector<structural_track>& before,
		const std::vector<structural_track>& after,
		const std::vector<std::string>& guids_the_call_named)
	{
		const std::vector<std::string> named = with_subtrees(before, guids_the_call_named);

		const std::map<std::string, std::vector<std::string>> paths_before =
			folder_paths_by_identity(before);
		const std::map<std::string, std::vector<std::string>> paths_after =
			folder_paths_by_identity(after);

		for (const std::pair<const std::string, std::vector<std::string>>& entry : paths_before)
		{
			if (std::find(named.begin(), named.end(), entry.first) != named.end())
			{
				continue;
			}

			const auto found_after = paths_after.find(entry.first);

			if (found_after == paths_after.end())
			{
				continue;
			}

			INFO("track " << entry.first);
			REQUIRE(found_after->second == entry.second);
		}
	}

	// ---------------------------------------------------------------------------
	// The scripted project
	//
	// Applies the same edits REAPER would, including the two details the arithmetic
	// depends on: an inserted track arrives with REAPER's default delta of zero, and a
	// duplicated track arrives carrying the delta of the track it was copied from.
	// Getting either of those wrong in the substitute would make the suite agree with a
	// project REAPER never produces.
	// ---------------------------------------------------------------------------

	class scripted_project final : public TrackStructureHost
	{
	public:
		explicit scripted_project(std::vector<structural_track> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		std::vector<structural_track> read_tracks_in_project_order() override
		{
			++read_call_count;

			return tracks_;
		}

		std::string insert_track(int index, const std::string& name, std::optional<int> color) override
		{
			++insert_call_count;
			inserted_color = color;

			if (refuse_insert)
			{
				return {};
			}

			structural_track inserted;
			inserted.guid = guid_for(next_minted_number_++);
			inserted.name = name;

			// REAPER's own default for an inserted track. A substitute that inserted the
			// planned delta instead would hide every missing write.
			inserted.folder_depth_delta = 0;

			const std::size_t clamped = index < 0
				? 0
				: std::min(static_cast<std::size_t>(index), tracks_.size());

			tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(clamped), inserted);

			return inserted.guid;
		}

		bool delete_track(const std::string& guid) override
		{
			++delete_call_count;

			if (refuse_delete)
			{
				return false;
			}

			const std::optional<std::size_t> index = index_of_track(tracks_, guid);

			if (!index.has_value())
			{
				return false;
			}

			tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(*index));

			return true;
		}

		std::vector<std::string> duplicate_tracks(const std::vector<std::string>& guids) override
		{
			++duplicate_call_count;
			duplicated_guids = guids;

			if (refuse_duplicate || guids.empty())
			{
				return {};
			}

			if (duplicate_one_track_too_few)
			{
				return std::vector<std::string>(guids.size() - 1, guid_for(next_minted_number_++));
			}

			std::vector<structural_track> copies;
			std::vector<std::string> copy_guids;
			std::size_t insert_after = 0;

			for (const std::string& guid : guids)
			{
				const std::optional<std::size_t> index = index_of_track(tracks_, guid);

				if (!index.has_value())
				{
					return {};
				}

				insert_after = std::max(insert_after, *index);

				structural_track copy = tracks_[*index];
				copy.guid = guid_for(next_minted_number_++);

				// REAPER copies the delta along with everything else, which is exactly
				// the value the repair has to overwrite.
				copy_guids.push_back(copy.guid);
				copies.push_back(std::move(copy));
			}

			tracks_.insert(
				tracks_.begin() + static_cast<std::ptrdiff_t>(insert_after) + 1,
				copies.begin(),
				copies.end());

			return copy_guids;
		}

		bool reorder_tracks(const std::vector<std::string>& guids_in_project_order) override
		{
			++reorder_call_count;
			requested_order = guids_in_project_order;

			if (refuse_reorder)
			{
				return false;
			}

			std::vector<structural_track> reordered;
			reordered.reserve(guids_in_project_order.size());

			for (const std::string& guid : guids_in_project_order)
			{
				const std::optional<std::size_t> index = index_of_track(tracks_, guid);

				if (!index.has_value())
				{
					return false;
				}

				reordered.push_back(tracks_[*index]);
			}

			if (reordered.size() != tracks_.size())
			{
				return false;
			}

			tracks_ = std::move(reordered);

			return true;
		}

		bool write_folder_depth_delta(const std::string& guid, int folder_depth_delta) override
		{
			++write_call_count;

			if (guid == refuse_write_for_guid)
			{
				return false;
			}

			const std::optional<std::size_t> index = index_of_track(tracks_, guid);

			if (!index.has_value())
			{
				return false;
			}

			tracks_[*index].folder_depth_delta = folder_depth_delta;

			return true;
		}

		const std::vector<structural_track>& tracks() const { return tracks_; }

		std::vector<std::string> track_names_in_order() const
		{
			std::vector<std::string> names;
			names.reserve(tracks_.size());

			for (const structural_track& track : tracks_)
			{
				names.push_back(track.name);
			}

			return names;
		}

		int read_call_count = 0;
		int insert_call_count = 0;
		int delete_call_count = 0;
		int duplicate_call_count = 0;
		int reorder_call_count = 0;
		int write_call_count = 0;

		bool refuse_insert = false;
		bool refuse_delete = false;
		bool refuse_duplicate = false;
		bool refuse_reorder = false;
		bool duplicate_one_track_too_few = false;
		std::string refuse_write_for_guid;

		std::optional<int> inserted_color;
		std::vector<std::string> duplicated_guids;
		std::vector<std::string> requested_order;

	private:
		std::vector<structural_track> tracks_;
		int next_minted_number_ = 900;
	};

	// ---------------------------------------------------------------------------
	// Reading an outcome
	// ---------------------------------------------------------------------------

	// By value, not by reference. The handlers return their outcome by value, so a
	// reference returned out of here would point into a temporary that dies at the end
	// of the calling expression — and the reads afterwards would be of freed memory
	// that mostly still holds plausible-looking values, which is the worst kind of
	// green test.
	template <typename ResultType>
	ResultType succeeded(const structural_outcome<ResultType>& outcome)
	{
		REQUIRE(std::holds_alternative<ResultType>(outcome));

		return std::get<ResultType>(outcome);
	}

	template <typename ResultType>
	handler_action_outcomes reported_actions(const structural_outcome<ResultType>& outcome)
	{
		REQUIRE(std::holds_alternative<handler_action_outcomes>(outcome));

		return std::get<handler_action_outcomes>(outcome);
	}

	action_failed failure_at(const handler_action_outcomes& outcomes, std::size_t index)
	{
		REQUIRE(index < outcomes.actions.size());
		REQUIRE(std::holds_alternative<action_failed>(outcomes.actions[index]));

		return std::get<action_failed>(outcomes.actions[index]);
	}
}

// ---------------------------------------------------------------------------
// The two primitives
// ---------------------------------------------------------------------------

TEST_CASE("a folder span covers a folder parent and everything inside it", "[daw][tools][structure]")
{
	const std::vector<TrackFolderDepth> tracks = folder_depths_of(project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	}));

	const auto drums = find_folder_span(tracks, 0);

	REQUIRE(drums.first_index == 0);
	REQUIRE(drums.last_index == 2);
	REQUIRE(drums.enclosing_folders_closed == 0);

	// A track that opens nothing encloses nothing, so its run is itself. Not a special
	// case — the same answer.
	const auto kick = find_folder_span(tracks, 1);

	REQUIRE(kick.first_index == 1);
	REQUIRE(kick.last_index == 1);
}

TEST_CASE("a folder span reports how much of the close belongs to enclosing folders", "[daw][tools][structure]")
{
	// Snare closes two levels at once: Drums' folder and Band's.
	const std::vector<TrackFolderDepth> tracks = folder_depths_of(project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
	}));

	const auto drums = find_folder_span(tracks, 1);

	REQUIRE(drums.first_index == 1);
	REQUIRE(drums.last_index == 3);
	REQUIRE(drums.enclosing_folders_closed == 1);
}

TEST_CASE("a subtree read out of a project balances on its own", "[daw][tools][structure]")
{
	const std::vector<TrackFolderDepth> tracks = folder_depths_of(project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
	}));

	const std::vector<TrackFolderDepth> subtree = read_balanced_subtree(tracks, 1);

	REQUIRE(subtree.size() == 3);

	// The whole point: the run's own deltas sum to zero, so it carries no part of
	// Band's closing delta with it. A copy that did would close Band a second time.
	int total = 0;

	for (const TrackFolderDepth& track : subtree)
	{
		total += track.folder_depth_delta;
	}

	REQUIRE(total == 0);
	require_folder_invariants(subtree);
}

TEST_CASE("extracting a subtree leaves the survivors parented as they were", "[daw][tools][structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Outer", 1},
		{"Moved", 1},
		{"MovedChild", -2},
		{"After", 0},
	});

	const auto extraction = extract_subtree(folder_depths_of(before), 1);

	require_folder_invariants(extraction.remaining);
	REQUIRE(extraction.subtree.size() == 2);

	// Outer had only the moved run inside it, so it is no longer a folder parent — and
	// crucially `After`, which sat outside Outer, is still outside it. Leaving the
	// enclosing folder's closing delta on the extracted run is what would have pulled
	// `After` in.
	const auto paths = folder_paths_by_identity(extraction.remaining);

	REQUIRE(paths.at(guid_of(before, "After")).empty());
	REQUIRE(paths.at(guid_of(before, "Outer")).empty());
}

TEST_CASE("attaching a subtree leaves every track after the insertion point where it was", "[daw][tools][structure]")
{
	// The naive "write 1 on the parent, -1 on the child" approach produces a valid
	// structure here and captures `Untouched` in the process. This is the test that
	// catches it.
	const std::vector<structural_track> before = project_of({
		{"Target", 0},
		{"Untouched", 0},
	});

	std::vector<TrackFolderDepth> subtree;
	subtree.push_back(TrackFolderDepth{"{new}", 0});

	const std::vector<TrackFolderDepth> after =
		attach_subtree_after(folder_depths_of(before), subtree, 0, 1);

	require_folder_invariants(after);

	const auto paths = folder_paths_by_identity(after);

	REQUIRE(paths.at("{new}") == std::vector<std::string>{guid_of(before, "Target")});
	REQUIRE(paths.at(guid_of(before, "Untouched")).empty());
}

// ---------------------------------------------------------------------------
// create_track
// ---------------------------------------------------------------------------

TEST_CASE("a track created in an empty project lands at the top level", "[daw][tools][create_track]")
{
	scripted_project project{{}};

	create_track_request request;
	request.name = "Lead Vocal";

	const auto outcome = apply_create_track(project, request);
	const create_track_result result = succeeded(outcome);

	REQUIRE(result.index == 0);
	REQUIRE(result.accumulated_depth == 0);
	REQUIRE(result.track.name == "Lead Vocal");
	REQUIRE(result.folder_depth_repairs.empty());
	require_folder_invariants(project.tracks());
}

TEST_CASE("a track created with nothing named lands at the end and disturbs nothing", "[daw][tools][create_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
	});

	scripted_project project{before};

	create_track_request request;
	request.name = "Bass";
	request.color = 0x00FF00;

	const auto outcome = apply_create_track(project, request);
	const create_track_result result = succeeded(outcome);

	REQUIRE(result.index == 3);
	REQUIRE(result.accumulated_depth == 0);
	REQUIRE(project.inserted_color == std::optional<int>{0x00FF00});

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {});
}

TEST_CASE("a track created after a folder parent joins that folder", "[daw][tools][create_track]")
{
	// The case that makes `create_track` destructive rather than safe: adding
	// something changes what the tracks around it are parented to. The tool does what
	// it was asked and the structural consequence is real, which is what the
	// confirmation has to say.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	create_track_request request;
	request.name = "Overheads";
	request.after_track_guid = guid_of(before, "Drums");

	const auto outcome = apply_create_track(project, request);
	const create_track_result result = succeeded(outcome);

	REQUIRE(result.index == 1);
	REQUIRE(result.accumulated_depth == 1);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {});

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(result.track.guid) == std::vector<std::string>{guid_of(before, "Drums")});
}

TEST_CASE("a track created inside a folder lands at the end of it and captures nothing", "[daw][tools][create_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
		{"Vocal", 0},
	});

	scripted_project project{before};

	create_track_request request;
	request.name = "Hats";
	request.inside_folder_of_guid = guid_of(before, "Kick");

	const auto outcome = apply_create_track(project, request);
	const create_track_result result = succeeded(outcome);

	REQUIRE(result.accumulated_depth == 1);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {});

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(result.track.guid) == std::vector<std::string>{guid_of(before, "Drums")});

	// The two tracks after the folder are still outside it. This is the assertion the
	// naive opening-delta write fails.
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
	REQUIRE(paths.at(guid_of(before, "Vocal")).empty());

	// Snare was the folder's last member and is no longer, so its delta moved. That is
	// a repair the schema requires reported rather than left for the producer to find.
	REQUIRE(result.folder_depth_repairs.size() == 1);
	REQUIRE(result.folder_depth_repairs.front().guid == guid_of(before, "Snare"));
	REQUIRE(result.folder_depth_repairs.front().previous_folder_depth == -1);
	REQUIRE(result.folder_depth_repairs.front().folder_depth == 0);
}

TEST_CASE("a track created inside the folder of a track in no folder makes that track a folder parent", "[daw][tools][create_track]")
{
	// "Put this in with the kick" when the kick is not in a folder. Grouping the two is
	// the ordinary reading, and it is the only one that does not fail most calls.
	const std::vector<structural_track> before = project_of({
		{"Kick", 0},
		{"Bass", 0},
	});

	scripted_project project{before};

	create_track_request request;
	request.name = "Kick Sub";
	request.inside_folder_of_guid = guid_of(before, "Kick");

	const auto outcome = apply_create_track(project, request);
	const create_track_result result = succeeded(outcome);

	REQUIRE(result.accumulated_depth == 1);

	require_folder_invariants(project.tracks());

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(result.track.guid) == std::vector<std::string>{guid_of(before, "Kick")});

	// Bass was never named and is still at the top level. Closing the new folder at the
	// end of the project instead would have swallowed it.
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("a track created after a track that is not there is a failed action with a reason", "[daw][tools][create_track]")
{
	scripted_project project{project_of({{"Drums", 0}})};

	create_track_request request;
	request.name = "Bass";
	request.after_track_guid = guid_for(404);

	const auto outcome = apply_create_track(project, request);
	const handler_action_outcomes actions = reported_actions(outcome);

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == unknown_track_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());

	// Nothing was attempted, so nothing changed.
	REQUIRE(project.insert_call_count == 0);
	REQUIRE(project.tracks().size() == 1);
}

TEST_CASE("an insert REAPER refuses is a failed action with a reason", "[daw][tools][create_track]")
{
	scripted_project project{project_of({{"Drums", 0}})};
	project.refuse_insert = true;

	create_track_request request;
	request.name = "Bass";

	const auto outcome = apply_create_track(project, request);
	const handler_action_outcomes actions = reported_actions(outcome);

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == track_insert_failed_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.write_call_count == 0);
}

// ---------------------------------------------------------------------------
// delete_track
// ---------------------------------------------------------------------------

TEST_CASE("deleting a plain track leaves every survivor parented as it was", "[daw][tools][delete_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", 0},
		{"Hats", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	delete_track_request request;
	request.track_guid = guid_of(before, "Snare");

	const auto outcome = apply_delete_track(project, request);
	const delete_track_result result = succeeded(outcome);

	REQUIRE_FALSE(result.folder_dissolved);
	REQUIRE(result.reparented_tracks.empty());

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});
}

TEST_CASE("deleting a folder's last member moves the close onto the track before it", "[daw][tools][delete_track]")
{
	// The defect this repair prevents: leaving the closing delta on a deleted track
	// means the folder never closes, and the Keeper's end-of-project repair then pulls
	// every following track inside it.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	delete_track_request request;
	request.track_guid = guid_of(before, "Snare");

	const auto outcome = apply_delete_track(project, request);
	const delete_track_result result = succeeded(outcome);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Kick")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());

	REQUIRE(result.folder_depth_repairs.size() == 1);
	REQUIRE(result.folder_depth_repairs.front().guid == guid_of(before, "Kick"));
}

TEST_CASE("deleting a folder parent dissolves the folder and promotes no child", "[daw][tools][delete_track]")
{
	// Requirement 11.3, which the Keeper implements and this routes through. The
	// mistake the requirement names explicitly is carrying the opening delta onto the
	// first child: that child would become a folder parent and silently re-parent all
	// of its siblings underneath it.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", 0},
		{"Hats", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	delete_track_request request;
	request.track_guid = guid_of(before, "Drums");

	const auto outcome = apply_delete_track(project, request);
	const delete_track_result result = succeeded(outcome);

	REQUIRE(result.folder_dissolved);
	require_folder_invariants(project.tracks());

	// The former children, in track order, so the agent can tell the producer which
	// tracks now feed whatever the parent fed.
	REQUIRE(result.reparented_tracks.size() == 3);
	REQUIRE(result.reparented_tracks[0].name == "Kick");
	REQUIRE(result.reparented_tracks[1].name == "Snare");
	REQUIRE(result.reparented_tracks[2].name == "Hats");

	const int parent_level_before = nesting_levels_by_identity(before).at(guid_of(before, "Drums"));
	const std::map<std::string, int> levels_after = nesting_levels_by_identity(project.tracks());
	const auto paths_after = folder_paths_by_identity(project.tracks());

	for (const auto& child : result.reparented_tracks)
	{
		INFO("former child " << child.name);

		// Rose to the parent's own depth.
		REQUIRE(levels_after.at(child.guid) == parent_level_before);

		// And none of them is inside anything now, so none was promoted into the
		// parent's folder role.
		REQUIRE(paths_after.at(child.guid).empty());
	}

	// The sharper form of the same check: no former child gained a folder role it did
	// not already have.
	for (const auto& child : result.reparented_tracks)
	{
		const std::optional<std::size_t> index_before = index_of_track(before, child.guid);
		const std::optional<std::size_t> index_after = index_of_track(project.tracks(), child.guid);

		REQUIRE(index_before.has_value());
		REQUIRE(index_after.has_value());

		INFO("former child " << child.name);

		if (project.tracks()[*index_after].folder_depth_delta > 0)
		{
			REQUIRE(before[*index_before].folder_depth_delta > 0);
		}
	}

	// Bass was outside the folder and stays outside it.
	REQUIRE(paths_after.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("deleting a nested folder parent dissolves only its own folder", "[daw][tools][delete_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
		{"Bass", 0},
	});

	scripted_project project{before};

	delete_track_request request;
	request.track_guid = guid_of(before, "Drums");

	const auto outcome = apply_delete_track(project, request);
	const delete_track_result result = succeeded(outcome);

	REQUIRE(result.folder_dissolved);
	require_folder_invariants(project.tracks());

	const auto paths_after = folder_paths_by_identity(project.tracks());

	// The former children rose to Drums' own depth, which is inside Band — not to the
	// top level.
	REQUIRE(paths_after.at(guid_of(before, "Kick")) == std::vector<std::string>{guid_of(before, "Band")});
	REQUIRE(paths_after.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Band")});

	// And Bass, which the call never named, is still outside Band.
	REQUIRE(paths_after.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("a delete reports what went with the track", "[daw][tools][delete_track]")
{
	std::vector<structural_track> before = project_of({{"Drums", 0}});
	before.front().item_count = 11;
	before.front().fx_count = 3;

	scripted_project project{before};

	delete_track_request request;
	request.track_guid = before.front().guid;

	const delete_track_result result = succeeded(apply_delete_track(project, request));

	// The number worth quoting: a track the producer thought was empty removing eleven
	// items is the case this field exists for.
	REQUIRE(result.deleted_item_count == 11);
	REQUIRE(result.deleted_fx_count == 3);
	REQUIRE(result.deleted_track.name == "Drums");
}

TEST_CASE("deleting a track that is not there is a failed action with a reason", "[daw][tools][delete_track]")
{
	scripted_project project{project_of({{"Drums", 0}})};

	delete_track_request request;
	request.track_guid = guid_for(404);

	const handler_action_outcomes actions = reported_actions(apply_delete_track(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == unknown_track_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.delete_call_count == 0);
}

// ---------------------------------------------------------------------------
// duplicate_track
// ---------------------------------------------------------------------------

TEST_CASE("duplicating a plain track puts the copy directly after it", "[daw][tools][duplicate_track]")
{
	std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});
	before[1].item_count = 4;
	before[1].fx_count = 2;
	before[1].send_count = 1;

	scripted_project project{before};

	duplicate_track_request request;
	request.track_guid = guid_of(before, "Kick");

	const duplicate_track_result result = succeeded(apply_duplicate_track(project, request));

	REQUIRE(result.index == 2);
	REQUIRE(result.copied_item_count == 4);
	REQUIRE(result.copied_fx_count == 2);
	REQUIRE(result.copied_send_count == 1);
	REQUIRE(result.new_track.guid != result.source_track.guid);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});

	// The copy is inside the same folder as the original, which is what makes it a
	// duplicate rather than a new track that happens to share a name.
	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(result.new_track.guid) == std::vector<std::string>{guid_of(before, "Drums")});
}

TEST_CASE("duplicating a folder parent copies the whole subtree", "[daw][tools][duplicate_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	duplicate_track_request request;
	request.track_guid = guid_of(before, "Drums");

	const duplicate_track_result result = succeeded(apply_duplicate_track(project, request));

	// Three tracks asked for, three tracks copied. A folder parent is not one track.
	REQUIRE(project.duplicated_guids.size() == 3);
	REQUIRE(project.tracks().size() == 7);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});

	const auto paths = folder_paths_by_identity(project.tracks());

	// The copy is a folder parent at the top level, like the original, and the two
	// copied children are inside it rather than inside the original.
	REQUIRE(paths.at(result.new_track.guid).empty());

	const std::vector<std::string> names = project.track_names_in_order();

	REQUIRE(names == std::vector<std::string>{
		"Drums", "Kick", "Snare", "Drums", "Kick", "Snare", "Bass",
	});

	const std::optional<std::size_t> copy_index = index_of_track(project.tracks(), result.new_track.guid);

	REQUIRE(copy_index == std::optional<std::size_t>{3});
	REQUIRE(result.index == 3);

	for (std::size_t index = 4; index <= 5; ++index)
	{
		REQUIRE(paths.at(project.tracks()[index].guid)
			== std::vector<std::string>{result.new_track.guid});
	}
}

TEST_CASE("duplicating a folder parent whose folder closes several levels at once balances both copies", "[daw][tools][duplicate_track]")
{
	// The case that separates a correct subtree copy from a plausible one. `Snare`
	// closes Drums' folder *and* Band's, and only one of those closings belongs to the
	// copy. A verbatim copy closes Band twice; a copy that keeps the whole closing
	// delta and leaves the original relaxed puts the copy outside Band.
	const std::vector<structural_track> before = project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
		{"Bass", 0},
	});

	scripted_project project{before};

	duplicate_track_request request;
	request.track_guid = guid_of(before, "Drums");

	const duplicate_track_result result = succeeded(apply_duplicate_track(project, request));

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});

	const auto paths = folder_paths_by_identity(project.tracks());

	// The copy sits inside Band, beside the original — not beside Band.
	REQUIRE(paths.at(result.new_track.guid) == std::vector<std::string>{guid_of(before, "Band")});
	REQUIRE(paths.at(guid_of(before, "Drums")) == std::vector<std::string>{guid_of(before, "Band")});

	// Both copied children are inside the copy.
	const std::optional<std::size_t> copy_index =
		index_of_track(project.tracks(), result.new_track.guid);

	REQUIRE(copy_index.has_value());

	for (std::size_t index = *copy_index + 1; index <= *copy_index + 2; ++index)
	{
		INFO("copied child at " << index);

		// Inside the copy, which is itself inside Band — the same two-deep path the
		// originals have. A copy placed outside Band would give a one-deep path here,
		// and one that closed Band twice would not produce a valid list at all.
		REQUIRE(paths.at(project.tracks()[index].guid) == std::vector<std::string>{
			guid_of(before, "Band"),
			result.new_track.guid,
		});
	}

	// And Bass, which the call never named, is still outside Band. A copy that closed
	// Band twice would have driven the accumulated depth negative here; one that closed
	// it not at all would have pulled Bass inside.
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("duplicating the last member of a folder keeps both inside it", "[daw][tools][duplicate_track]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	duplicate_track_request request;
	request.track_guid = guid_of(before, "Snare");

	const duplicate_track_result result = succeeded(apply_duplicate_track(project, request));

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {request.track_guid});

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(result.new_track.guid) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("a duplication REAPER did not perform is a failed action with a reason", "[daw][tools][duplicate_track]")
{
	scripted_project project{project_of({{"Kick", 0}})};
	project.refuse_duplicate = true;

	duplicate_track_request request;
	request.track_guid = project.tracks().front().guid;

	const handler_action_outcomes actions = reported_actions(apply_duplicate_track(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == track_duplicate_failed_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.write_call_count == 0);
}

TEST_CASE("a duplication that returns the wrong number of tracks is a failed action", "[daw][tools][duplicate_track]")
{
	// A copy that is not the subtree asked for cannot be repaired into one, and
	// proceeding would write deltas to tracks chosen by position in a list that does
	// not match.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
	});

	scripted_project project{before};
	project.duplicate_one_track_too_few = true;

	duplicate_track_request request;
	request.track_guid = guid_of(before, "Drums");

	const handler_action_outcomes actions = reported_actions(apply_duplicate_track(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == track_duplicate_failed_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.write_call_count == 0);
}

// ---------------------------------------------------------------------------
// set_folder_structure
// ---------------------------------------------------------------------------

TEST_CASE("a track moved into a folder becomes its last member and captures nothing", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
		{"Vocal", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Bass")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE_FALSE(result.moved_to_top_level);
	REQUIRE(result.folder_parent.name == "Drums");
	REQUIRE(result.tracks.size() == 1);
	REQUIRE(result.tracks.front().previous_accumulated_depth == 0);
	REQUIRE(result.tracks.front().accumulated_depth == 1);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Bass")) == std::vector<std::string>{guid_of(before, "Drums")});

	// Vocal was never named, and it is still outside the folder. This is the assertion
	// that fails when a folder is opened and left for the end-of-project repair to
	// close.
	REQUIRE(paths.at(guid_of(before, "Vocal")).empty());
}

TEST_CASE("a track moved into a nested folder lands inside it, not beside it", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Bass")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.tracks.front().accumulated_depth == 2);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Bass")) == std::vector<std::string>{
		guid_of(before, "Band"),
		guid_of(before, "Drums"),
	});
}

TEST_CASE("a track moved into the folder of a track in no folder makes that track a folder parent", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Kick", 0},
		{"Snare", 0},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare")};
	request.into_folder_of_guid = guid_of(before, "Kick");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.folder_parent.name == "Kick");
	REQUIRE(result.tracks.front().accumulated_depth == 1);

	require_folder_invariants(project.tracks());

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Kick")});

	// Bass was never named. The naive version of this operation swallows it.
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);
}

TEST_CASE("a folder parent moved into another folder takes its children with it", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Band", 1},
		{"Guitars", -1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Drums")};
	request.into_folder_of_guid = guid_of(before, "Band");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.tracks.front().accumulated_depth == 1);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Drums")) == std::vector<std::string>{guid_of(before, "Band")});

	// The children came along and are still inside Drums. Moving the parent alone would
	// have left them behind, re-parented into whatever followed.
	REQUIRE(paths.at(guid_of(before, "Kick")) == std::vector<std::string>{
		guid_of(before, "Band"),
		guid_of(before, "Drums"),
	});
	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{
		guid_of(before, "Band"),
		guid_of(before, "Drums"),
	});
}

TEST_CASE("a track moved to the top level leaves the folder it was in", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", 0},
		{"Hats", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare")};
	request.move_to_top_level = true;

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.moved_to_top_level);
	REQUIRE(result.folder_parent.guid.empty());
	REQUIRE(result.tracks.front().previous_accumulated_depth == 1);
	REQUIRE(result.tracks.front().accumulated_depth == 0);

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Snare")).empty());
	REQUIRE(paths.at(guid_of(before, "Kick")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Hats")) == std::vector<std::string>{guid_of(before, "Drums")});
}

TEST_CASE("a folder's last member moved to the top level leaves the folder closing behind it", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare")};
	request.move_to_top_level = true;

	succeeded(apply_set_folder_structure(project, request));

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Snare")).empty());
	REQUIRE(paths.at(guid_of(before, "Kick")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("a nested folder parent moved to the top level takes its children out with it", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Band", 1},
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -2},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Drums")};
	request.move_to_top_level = true;

	succeeded(apply_set_folder_structure(project, request));

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Drums")).empty());
	REQUIRE(paths.at(guid_of(before, "Kick")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());

	// Band had only the moved folder inside it, so it is no longer a folder parent —
	// and Bass, which was always outside Band, is still outside it.
	REQUIRE(paths.at(guid_of(before, "Band")).empty());
}

TEST_CASE("a track already inside the target folder is not reported as a change", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Kick")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.tracks.size() == 1);
	REQUIRE(result.tracks.front().previous_accumulated_depth
		== result.tracks.front().accumulated_depth);
	REQUIRE(result.folder_depth_repairs.empty());

	// Nothing was written, so the project is untouched rather than rearranged into the
	// same shape.
	REQUIRE(project.reorder_call_count == 0);
	REQUIRE(project.write_call_count == 0);
	require_folder_invariants(project.tracks());
}

TEST_CASE("a track already at the top level is not reported as a change", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Bass")};
	request.move_to_top_level = true;

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.tracks.front().previous_accumulated_depth == 0);
	REQUIRE(result.tracks.front().accumulated_depth == 0);
	REQUIRE(project.reorder_call_count == 0);
	REQUIRE(project.write_call_count == 0);
}

TEST_CASE("several tracks moved into one folder all land inside it", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Snare", 0},
		{"Hats", 0},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare"), guid_of(before, "Hats")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const set_folder_structure_result result =
		succeeded(apply_set_folder_structure(project, request));

	REQUIRE(result.tracks.size() == 2);
	REQUIRE(result.tracks[0].name == "Snare");
	REQUIRE(result.tracks[1].name == "Hats");

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), request.track_guids);

	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Hats")) == std::vector<std::string>{guid_of(before, "Drums")});
	REQUIRE(paths.at(guid_of(before, "Bass")).empty());
}

TEST_CASE("a track that is not there fails while its siblings still move", "[daw][tools][set_folder_structure]")
{
	// Requirement 9.4: the whole array runs in one undo block, and a sibling's failure
	// does not roll back what landed. The assertion is about the project, not about the
	// payload describing it.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Snare", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare"), guid_for(404)};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 2);
	REQUIRE(action_was_applied(actions.actions[0]));
	REQUIRE(failure_at(actions, 1).error.code() == unknown_track_code);
	REQUIRE_FALSE(failure_at(actions, 1).error.message().empty());

	// Snare moved and stayed moved.
	const auto paths = folder_paths_by_identity(project.tracks());

	REQUIRE(paths.at(guid_of(before, "Snare")) == std::vector<std::string>{guid_of(before, "Drums")});
	require_folder_invariants(project.tracks());
}

TEST_CASE("a destination naming both a folder and the top level is a failed action with a reason", "[daw][tools][set_folder_structure]")
{
	// Not a refusal. The refusal schema's `reason` is closed at five values and none of
	// them describes a contradictory destination, and no acknowledgement the producer
	// could give would make "into this folder, and also out of every folder" mean
	// something — the same reasoning requirement 9.6 applies to an inverted range.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Bass", 0},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Bass")};
	request.into_folder_of_guid = guid_of(before, "Drums");
	request.move_to_top_level = true;

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == contradictory_folder_destination_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.reorder_call_count == 0);
	REQUIRE(project.write_call_count == 0);
}

TEST_CASE("a destination naming nothing is a failed action with a reason", "[daw][tools][set_folder_structure]")
{
	scripted_project project{project_of({{"Bass", 0}})};

	set_folder_structure_request request;
	request.track_guids = {project.tracks().front().guid};

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == folder_destination_not_named_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
}

TEST_CASE("a call naming no tracks is a failed action with a reason", "[daw][tools][set_folder_structure]")
{
	scripted_project project{project_of({{"Bass", 0}})};

	set_folder_structure_request request;
	request.move_to_top_level = true;

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE_FALSE(failure_at(actions, 0).error.code().empty());
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
}

TEST_CASE("a track asked to move into its own folder is a failed action with a reason", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Drums")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == folder_target_is_the_moved_track_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());
	REQUIRE(project.reorder_call_count == 0);
}

TEST_CASE("a folder parent asked to move inside one of its own children is a failed action", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
	});

	scripted_project project{before};

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Drums")};
	request.into_folder_of_guid = guid_of(before, "Kick");

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 1);
	REQUIRE(failure_at(actions, 0).error.code() == folder_target_inside_moved_track_code);
	REQUIRE_FALSE(failure_at(actions, 0).error.message().empty());

	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {});
}

TEST_CASE("a reorder REAPER refuses leaves the structure untouched and every action failed", "[daw][tools][set_folder_structure]")
{
	// `Bass` sits between the folder and the tracks being moved, so the move genuinely
	// reorders the project rather than only rewriting deltas — which is what makes this
	// a test of the reorder failing rather than of a reorder that was never needed.
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Bass", 0},
		{"Snare", 0},
		{"Hats", 0},
	});

	scripted_project project{before};
	project.refuse_reorder = true;

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Snare"), guid_of(before, "Hats")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	REQUIRE(actions.actions.size() == 2);
	REQUIRE(failure_at(actions, 0).error.code() == track_reorder_failed_code);
	REQUIRE(failure_at(actions, 1).error.code() == track_reorder_failed_code);

	// No delta was written, so the project is exactly as it was rather than half moved.
	REQUIRE(project.write_call_count == 0);
	REQUIRE(project.tracks().size() == before.size());
	require_folder_invariants(project.tracks());
	require_untargeted_tracks_keep_their_folders(before, project.tracks(), {});
}

TEST_CASE("a folder depth REAPER would not write is reported beside the move that landed", "[daw][tools][set_folder_structure]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", 0},
		{"Snare", -1},
		{"Bass", 0},
	});

	scripted_project project{before};
	project.refuse_write_for_guid = guid_of(before, "Snare");

	set_folder_structure_request request;
	request.track_guids = {guid_of(before, "Bass")};
	request.into_folder_of_guid = guid_of(before, "Drums");

	const handler_action_outcomes& actions =
		reported_actions(apply_set_folder_structure(project, request));

	// The move landed and is reported as applied; the repair did not and carries its
	// own reason. A success payload here would claim a structure nobody verified.
	REQUIRE(actions.actions.size() == 2);
	REQUIRE(action_was_applied(actions.actions[0]));
	REQUIRE_FALSE(action_was_applied(actions.actions[1]));
	REQUIRE_FALSE(failure_at(actions, 1).error.code().empty());
	REQUIRE_FALSE(failure_at(actions, 1).error.message().empty());
}

// ---------------------------------------------------------------------------
// Registration through the Tool Executor's seam
// ---------------------------------------------------------------------------

namespace
{
	// The payload type the framework is templated on. A plain struct, for the reason
	// `tool_executor_test.cpp` gives: the framework never reads a field, so the suite
	// does not need a JSON library to drive it.
	struct test_payload
	{
		std::string described_result;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_context = tool_execution_context<test_payload>;

	// A codec that reads its request from a scripted value rather than from JSON.
	// Reading a tool input is the Envelope Codec's job; what is under test here is that
	// the four tools reach the registry and come back out as results.
	track_structure_payload_codec<test_payload> codec_for(
		create_track_request create,
		delete_track_request remove,
		duplicate_track_request duplicate,
		set_folder_structure_request folder)
	{
		track_structure_payload_codec<test_payload> codec;

		codec.read_create_track_request = [create](const test_context&) { return create; };
		codec.read_delete_track_request = [remove](const test_context&) { return remove; };
		codec.read_duplicate_track_request = [duplicate](const test_context&) { return duplicate; };
		codec.read_set_folder_structure_request = [folder](const test_context&) { return folder; };

		codec.write_create_track_result = [](const create_track_result& result) {
			return test_payload{"created " + result.track.guid};
		};
		codec.write_delete_track_result = [](const delete_track_result& result) {
			return test_payload{"deleted " + result.deleted_track.guid};
		};
		codec.write_duplicate_track_result = [](const duplicate_track_result& result) {
			return test_payload{"duplicated as " + result.new_track.guid};
		};
		codec.write_set_folder_structure_result = [](const set_folder_structure_result& result) {
			return test_payload{"moved " + std::to_string(result.tracks.size()) + " tracks"};
		};

		return codec;
	}

	// A scripted REAPER undo stack, counting everything the framework does to it.
	class scripted_undo_stack final : public undo_stack
	{
	public:
		int current_position() override { return position_; }

		std::string entry_description_at(int position) override
		{
			if (position < 0 || static_cast<std::size_t>(position) >= entries_.size())
			{
				return {};
			}

			return entries_[static_cast<std::size_t>(position)];
		}

		void begin_block() override
		{
			++begin_block_call_count;
			++open_block_depth;
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;
			end_block_descriptions.push_back(description);

			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int open_block_depth = 0;
		std::vector<std::string> end_block_descriptions;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class empty_track_list final : public TrackListSource
	{
	public:
		std::vector<ResolvableTrack> tracks_in_project_order() const override { return {}; }
	};
}

TEST_CASE("all four tools register as mutating tools", "[daw][tools][structure][registration]")
{
	test_registry registry;
	scripted_project project{project_of({{"Drums", 0}})};

	const auto report = register_track_structure_tools(
		registry,
		project,
		codec_for({}, {}, {}, {}));

	REQUIRE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 4);

	// Registered through the mutating seam, so a block is opened for each and the
	// turn's marker is captured before the first. Requirement 9.2 rather than 12.7.
	for (const std::string_view& tool_name : {
			create_track_tool_name,
			delete_track_tool_name,
			duplicate_track_tool_name,
			set_folder_structure_tool_name,
		})
	{
		INFO("tool " << tool_name);
		const auto* const tool = registry.find_tool(tool_name);

		REQUIRE(tool != nullptr);
		REQUIRE(tool->undo_effect() == tool_undo_effect::undo_block);
	}
}

TEST_CASE("an incomplete codec registers nothing", "[daw][tools][structure][registration]")
{
	// An empty `std::function` would throw `std::bad_function_call` from inside the
	// undo block, which is the one place a throw costs the producer their undo point.
	test_registry registry;
	scripted_project project{{}};

	track_structure_payload_codec<test_payload> codec = codec_for({}, {}, {}, {});
	codec.write_duplicate_track_result = {};

	const auto report = register_track_structure_tools(registry, project, codec);

	REQUIRE_FALSE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 0);
}

TEST_CASE("a second registration of the same tool is rejected", "[daw][tools][structure][registration]")
{
	test_registry registry;
	scripted_project project{{}};

	REQUIRE(register_track_structure_tools(registry, project, codec_for({}, {}, {}, {}))
		.every_tool_registered());

	const auto second = register_track_structure_tools(registry, project, codec_for({}, {}, {}, {}));

	REQUIRE(second.create_track == tool_registration_outcome::already_registered);
	REQUIRE(second.delete_track == tool_registration_outcome::already_registered);
	REQUIRE(second.duplicate_track == tool_registration_outcome::already_registered);
	REQUIRE(second.set_folder_structure == tool_registration_outcome::already_registered);
	REQUIRE(registry.registered_tool_count() == 4);
}

TEST_CASE("a dispatched create_track produces a success carrying one undo block", "[daw][tools][structure][registration]")
{
	test_registry registry;
	scripted_project project{project_of({{"Drums", 0}})};

	create_track_request create;
	create.name = "Bass";

	REQUIRE(register_track_structure_tools(registry, project, codec_for(create, {}, {}, {}))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	sesh_ai::daw::NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{create_track_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	REQUIRE(success->tool_name == create_track_tool_name);
	REQUIRE(success->undo.has_value());
	REQUIRE(success->undo->undo_description == "Sesh AI: create_track");

	// Exactly one block, opened and closed. Requirement 10.8.
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);

	REQUIRE(project.tracks().size() == 2);
	require_folder_invariants(project.tracks());
}

TEST_CASE("a dispatched set_folder_structure that partly fails reports per action and records the block", "[daw][tools][structure][registration]")
{
	const std::vector<structural_track> before = project_of({
		{"Drums", 1},
		{"Kick", -1},
		{"Snare", 0},
	});

	test_registry registry;
	scripted_project project{before};

	set_folder_structure_request folder;
	folder.track_guids = {guid_of(before, "Snare"), guid_for(404)};
	folder.into_folder_of_guid = guid_of(before, "Drums");

	REQUIRE(register_track_structure_tools(registry, project, codec_for({}, {}, {}, folder))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	sesh_ai::daw::NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{set_folder_structure_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const partial =
		std::get_if<sesh_ai::daw::tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);
	REQUIRE(partial->actions.size() == 2);
	REQUIRE(partial->applied_action_count() == 1);
	REQUIRE(partial->failed_action_count() == 1);

	// An action array landed something, so the undo report is present — there is a
	// position for "revert all" to walk back to.
	REQUIRE(partial->undo.has_value());
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);

	require_folder_invariants(project.tracks());
}
