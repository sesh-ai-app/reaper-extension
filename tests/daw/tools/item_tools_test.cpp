// The eight item tools (task 10.7, requirements 9.1 and 9.2).
//
// The suite is built around the five places these handlers can lie to a producer, since
// the rest is arithmetic:
//
//   - Moving a session by the wrong amount. Both move tools take an *offset*, and a
//     handler that wrote it as an absolute position would pass any test starting from
//     zero. So every move test starts from a non-zero position and asserts the exact
//     positions the host was asked to write, not only the result payload.
//   - Reporting one half of a split. REAPER mints a new GUID for the right half, so a
//     result naming only the half the agent already had a handle on is useless to it.
//     Both halves are asserted, by GUID and by bounds.
//   - Collapsing three import failures into one. A missing path, an unreadable path, and
//     a file REAPER will not decode send the producer three different ways, and the
//     scripted host is what makes all three reachable.
//   - Losing a failure's reason. Every failed outcome in this file is checked for a code
//     and a message, and the action array tests walk the whole outcome list rather than
//     the first entry.
//   - Telling the producer something changed when it did not. `move_all_items` counts
//     movement rather than writes, `clampedItemCount` is asserted against items held at
//     zero, and `list_selected_items` reports the count before its cap.
//
// The scripted host is what makes all of that reachable: it holds a mutable project the
// handlers write to, records every write in order, and can be told to refuse any one
// operation — none of which can be staged inside REAPER.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/item_tools.h>

using sesh_ai::daw::action_error;
using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_target;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::count_applied_actions;
using sesh_ai::daw::count_failed_actions;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::handler_success;
using sesh_ai::daw::maximum_action_outcomes;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_partial_outcome;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackGuidSelector;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::TrackReference;
using sesh_ai::daw::TrackSelector;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;

using sesh_ai::daw::tools::apply_delete_items;
using sesh_ai::daw::tools::apply_import_audio_file;
using sesh_ai::daw::tools::apply_import_midi_file;
using sesh_ai::daw::tools::apply_list_selected_items;
using sesh_ai::daw::tools::apply_move_all_items;
using sesh_ai::daw::tools::apply_move_items;
using sesh_ai::daw::tools::apply_offset;
using sesh_ai::daw::tools::apply_set_item_properties;
using sesh_ai::daw::tools::apply_split_items;
using sesh_ai::daw::tools::check_addressed_item_count;
using sesh_ai::daw::tools::check_item_host_usable;
using sesh_ai::daw::tools::delete_items_request;
using sesh_ai::daw::tools::delete_items_result;
using sesh_ai::daw::tools::delete_items_tool_name;
using sesh_ai::daw::tools::every_item_tool_registered;
using sesh_ai::daw::tools::import_audio_file_result;
using sesh_ai::daw::tools::import_audio_file_tool_name;
using sesh_ai::daw::tools::import_file_request;
using sesh_ai::daw::tools::import_midi_file_result;
using sesh_ai::daw::tools::import_midi_file_tool_name;
using sesh_ai::daw::tools::imported_media;
using sesh_ai::daw::tools::item_action_outcomes;
using sesh_ai::daw::tools::item_delete_failed_code;
using sesh_ai::daw::tools::item_duplicate_guid_code;
using sesh_ai::daw::tools::item_file_access;
using sesh_ai::daw::tools::item_file_not_found_code;
using sesh_ai::daw::tools::item_file_not_importable_code;
using sesh_ai::daw::tools::item_file_not_readable_code;
using sesh_ai::daw::tools::item_host;
using sesh_ai::daw::tools::item_host_unavailable_code;
using sesh_ai::daw::tools::item_import_unreportable_code;
using sesh_ai::daw::tools::item_names_no_change_code;
using sesh_ai::daw::tools::item_not_found_code;
using sesh_ai::daw::tools::item_outcome;
using sesh_ai::daw::tools::item_placement;
using sesh_ai::daw::tools::item_position_write_failed_code;
using sesh_ai::daw::tools::item_property_change;
using sesh_ai::daw::tools::item_property_write_failed_code;
using sesh_ai::daw::tools::item_scope;
using sesh_ai::daw::tools::item_split;
using sesh_ai::daw::tools::item_split_failed_code;
using sesh_ai::daw::tools::item_split_halves;
using sesh_ai::daw::tools::item_state;
using sesh_ai::daw::tools::item_time_range;
using sesh_ai::daw::tools::item_tool_count;
using sesh_ai::daw::tools::item_tool_registration;
using sesh_ai::daw::tools::item_too_many_items_code;
using sesh_ai::daw::tools::item_track_move_failed_code;
using sesh_ai::daw::tools::list_selected_items_request;
using sesh_ai::daw::tools::list_selected_items_result;
using sesh_ai::daw::tools::list_selected_items_tool_name;
using sesh_ai::daw::tools::maximum_items_per_reported_call;
using sesh_ai::daw::tools::maximum_reported_items;
using sesh_ai::daw::tools::move_all_items_outcome;
using sesh_ai::daw::tools::move_all_items_request;
using sesh_ai::daw::tools::move_all_items_result;
using sesh_ai::daw::tools::move_all_items_tool_name;
using sesh_ai::daw::tools::move_items_request;
using sesh_ai::daw::tools::move_items_result;
using sesh_ai::daw::tools::move_items_tool_name;
using sesh_ai::daw::tools::register_item_tools;
using sesh_ai::daw::tools::reported_item_outcome;
using sesh_ai::daw::tools::selected_item;
using sesh_ai::daw::tools::set_item_properties_request;
using sesh_ai::daw::tools::set_item_properties_result;
using sesh_ai::daw::tools::set_item_properties_tool_name;
using sesh_ai::daw::tools::split_falls_inside;
using sesh_ai::daw::tools::split_items_request;
using sesh_ai::daw::tools::split_items_result;
using sesh_ai::daw::tools::split_items_tool_name;

namespace
{
	// ---------------------------------------------------------------------------
	// The scripted session
	// ---------------------------------------------------------------------------

	const std::string drums_track_guid{"{00000000-0000-0000-0000-0000000000aa}"};
	const std::string drums_track_name{"Kick"};
	const std::string vocal_track_guid{"{00000000-0000-0000-0000-0000000000bb}"};
	const std::string vocal_track_name{"Scratch Vocal"};

	std::string item_guid_for(char marker)
	{
		std::string guid{"{00000000-0000-0000-0000-00000000000"};
		guid.push_back(marker);
		guid.push_back('}');

		return guid;
	}

	TrackReference drums_track()
	{
		TrackReference track;
		track.guid = drums_track_guid;
		track.name = drums_track_name;

		return track;
	}

	TrackReference vocal_track()
	{
		TrackReference track;
		track.guid = vocal_track_guid;
		track.name = vocal_track_name;

		return track;
	}

	item_state scripted_item(
		char marker,
		double position_seconds,
		double length_seconds,
		const TrackReference& track,
		int track_project_index)
	{
		item_state item;
		item.guid = item_guid_for(marker);
		item.track_guid = track.guid;
		item.track_name = track.name;
		item.track_project_index = track_project_index;
		item.position_seconds = position_seconds;
		item.length_seconds = length_seconds;
		item.take_name = std::string{"take "} + marker;
		item.selected = true;
		item.track_selected = true;

		return item;
	}

	// A scripted REAPER, recording everything the handlers do to it.
	//
	// Positions are recorded in the order they were written, which is what catches a
	// handler that moved an item to an offset instead of by one: the result payload alone
	// would still look plausible.
	class scripted_item_host final : public item_host
	{
	public:
		bool is_usable() const override { return usable; }

		std::vector<std::string> unresolved_function_names() const override
		{
			return missing_function_names;
		}

		std::vector<item_state> read_project_items() override
		{
			++project_read_count;

			return items;
		}

		std::optional<item_time_range> read_time_selection() override
		{
			++time_selection_read_count;

			return time_selection;
		}

		item_file_access probe_file(const std::string& file_path) override
		{
			for (const std::pair<std::string, item_file_access>& entry : file_access)
			{
				if (entry.first == file_path)
				{
					return entry.second;
				}
			}

			return item_file_access::missing;
		}

		bool set_item_position(const std::string& item_guid, double position_seconds) override
		{
			if (refuses(items_that_refuse_position, item_guid))
			{
				return false;
			}

			item_state* const item = find(item_guid);

			if (item == nullptr)
			{
				return false;
			}

			positions_written.emplace_back(item_guid, position_seconds);
			item->position_seconds = position_seconds;

			return true;
		}

		bool set_item_length(const std::string& item_guid, double length_seconds) override
		{
			if (refuses(items_that_refuse_length, item_guid))
			{
				return false;
			}

			item_state* const item = find(item_guid);

			if (item == nullptr)
			{
				return false;
			}

			// A trim is clamped by the underlying audio, which is why the handlers read
			// the length back rather than echoing what was asked for.
			item->length_seconds =
				length_seconds > source_length_ceiling ? source_length_ceiling : length_seconds;

			return true;
		}

		bool set_item_fade_in(const std::string& item_guid, double fade_seconds) override
		{
			if (find(item_guid) == nullptr)
			{
				return false;
			}

			fades_written.emplace_back(item_guid, fade_seconds);

			return true;
		}

		bool set_item_fade_out(const std::string& item_guid, double fade_seconds) override
		{
			if (find(item_guid) == nullptr)
			{
				return false;
			}

			fades_written.emplace_back(item_guid, fade_seconds);

			return true;
		}

		bool set_item_volume(const std::string& item_guid, double reaper_volume) override
		{
			if (find(item_guid) == nullptr)
			{
				return false;
			}

			volumes_written.emplace_back(item_guid, reaper_volume);

			return true;
		}

		bool set_item_muted(const std::string& item_guid, bool muted) override
		{
			item_state* const item = find(item_guid);

			if (item == nullptr)
			{
				return false;
			}

			item->muted = muted;
			++mute_write_count;

			return true;
		}

		bool move_item_to_track(const std::string& item_guid, const std::string& track_guid) override
		{
			if (refuses(items_that_refuse_track_move, item_guid))
			{
				return false;
			}

			item_state* const item = find(item_guid);

			if (item == nullptr)
			{
				return false;
			}

			item->track_guid = track_guid;
			item->track_name = track_guid == vocal_track_guid ? vocal_track_name : drums_track_name;
			item->track_project_index = track_guid == vocal_track_guid ? 1 : 0;
			track_moves.emplace_back(item_guid, track_guid);

			return true;
		}

		std::optional<item_split_halves> split_item(
			const std::string& item_guid,
			double position_seconds) override
		{
			if (refuses(items_that_refuse_split, item_guid))
			{
				return std::nullopt;
			}

			item_state* const item = find(item_guid);

			if (item == nullptr)
			{
				return std::nullopt;
			}

			item_state right = *item;
			right.guid = item_guid_for(next_split_marker);
			++next_split_marker;
			right.position_seconds = position_seconds;
			right.length_seconds = item->position_seconds + item->length_seconds - position_seconds;

			item->length_seconds = position_seconds - item->position_seconds;

			item_split_halves halves;
			halves.left_item_guid = item->guid;
			halves.right_item_guid = right.guid;

			if (right_halves_vanish)
			{
				// REAPER split and the new half cannot be read back, which the handler has
				// to report rather than name one side of a pair.
				return halves;
			}

			items.push_back(std::move(right));

			return halves;
		}

		bool delete_item(const std::string& item_guid) override
		{
			if (refuses(items_that_refuse_delete, item_guid))
			{
				return false;
			}

			for (std::size_t position = 0; position < items.size(); ++position)
			{
				if (items[position].guid == item_guid)
				{
					items.erase(items.begin() + static_cast<std::ptrdiff_t>(position));
					deleted_item_guids.push_back(item_guid);

					return true;
				}
			}

			return false;
		}

		std::optional<imported_media> import_media_file(
			const std::string& track_guid,
			const std::string& file_path,
			double position_seconds) override
		{
			if (!import_lands)
			{
				return std::nullopt;
			}

			item_state imported;
			imported.guid = item_guid_for(next_split_marker);
			++next_split_marker;
			imported.track_guid = track_guid;
			imported.track_name = track_guid == vocal_track_guid ? vocal_track_name : drums_track_name;
			imported.track_project_index = track_guid == vocal_track_guid ? 1 : 0;
			imported.position_seconds = position_seconds;
			imported.length_seconds = imported_length_seconds;
			imported.take_name = "imported take";

			imported_media media;
			media.item_guid = imported.guid;
			media.resolved_file_path = resolved_import_path.empty() ? file_path : resolved_import_path;
			media.sample_rate = imported_sample_rate;
			media.channel_count = imported_channel_count;
			media.imported_track_count = imported_track_count;
			media.note_count = imported_note_count;

			if (!imported_item_is_readable_afterwards)
			{
				return media;
			}

			items.push_back(std::move(imported));

			return media;
		}

		item_state* find(const std::string& item_guid)
		{
			for (item_state& item : items)
			{
				if (item.guid == item_guid)
				{
					return &item;
				}
			}

			return nullptr;
		}

		const item_state* find(const std::string& item_guid) const
		{
			for (const item_state& item : items)
			{
				if (item.guid == item_guid)
				{
					return &item;
				}
			}

			return nullptr;
		}

		std::vector<item_state> items;
		std::optional<item_time_range> time_selection;
		std::vector<std::pair<std::string, item_file_access>> file_access;

		bool usable = true;
		std::vector<std::string> missing_function_names;

		std::vector<std::string> items_that_refuse_position;
		std::vector<std::string> items_that_refuse_length;
		std::vector<std::string> items_that_refuse_track_move;
		std::vector<std::string> items_that_refuse_split;
		std::vector<std::string> items_that_refuse_delete;

		double source_length_ceiling = 1.0e9;
		bool right_halves_vanish = false;

		bool import_lands = true;
		bool imported_item_is_readable_afterwards = true;
		double imported_length_seconds = 3.5;
		int imported_sample_rate = 48000;
		int imported_channel_count = 2;
		int imported_track_count = 1;
		int imported_note_count = 0;
		std::string resolved_import_path;

		std::vector<std::pair<std::string, double>> positions_written;
		std::vector<std::pair<std::string, double>> fades_written;
		std::vector<std::pair<std::string, double>> volumes_written;
		std::vector<std::pair<std::string, std::string>> track_moves;
		std::vector<std::string> deleted_item_guids;
		int mute_write_count = 0;
		int project_read_count = 0;
		int time_selection_read_count = 0;

	private:
		static bool refuses(const std::vector<std::string>& refusing, const std::string& item_guid)
		{
			return std::find(refusing.begin(), refusing.end(), item_guid) != refusing.end();
		}

		char next_split_marker = '5';
	};

	// Three items on one track at 10, 20 and 30 seconds. Non-zero positions throughout,
	// deliberately: a handler that wrote the offset as an absolute position would look
	// correct against a project starting at zero.
	scripted_item_host host_with_three_items()
	{
		scripted_item_host host;
		host.items = {
			scripted_item('1', 10.0, 4.0, drums_track(), 0),
			scripted_item('2', 20.0, 4.0, drums_track(), 0),
			scripted_item('3', 30.0, 4.0, drums_track(), 0)};

		return host;
	}

	// Every failed outcome in a list has a code and a message, which is requirement 9.5's
	// claim checked rather than assumed. Used everywhere an action array is asserted, so a
	// handler that grew a reasonless failure would be caught wherever it appeared.
	void require_every_failure_carries_a_reason(const std::vector<action_outcome>& actions)
	{
		for (const action_outcome& outcome : actions)
		{
			if (action_was_applied(outcome))
			{
				continue;
			}

			const action_failed& failed = std::get<action_failed>(outcome);

			CHECK(failed.error.code().empty() == false);
			CHECK(failed.error.message().empty() == false);
			CHECK(failed.target.empty() == false);
		}
	}

	std::string failure_code_at(const std::vector<action_outcome>& actions, std::size_t position)
	{
		REQUIRE(position < actions.size());
		REQUIRE(action_was_applied(actions[position]) == false);

		return std::get<action_failed>(actions[position]).error.code();
	}

	double position_written_for(const scripted_item_host& host, const std::string& item_guid)
	{
		for (const std::pair<std::string, double>& written : host.positions_written)
		{
			if (written.first == item_guid)
			{
				return written.second;
			}
		}

		return -1.0;
	}
}

// ---------------------------------------------------------------------------
// set_item_properties
// ---------------------------------------------------------------------------

TEST_CASE("set_item_properties reports one outcome per entry and the bounds that landed", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	set_item_properties_request request;

	item_property_change moved;
	moved.item_guid = item_guid_for('1');
	moved.position_seconds = 12.0;
	moved.muted = true;
	request.changes.push_back(moved);

	item_property_change faded;
	faded.item_guid = item_guid_for('2');
	faded.fade_in_seconds = 0.25;
	faded.fade_out_seconds = 0.5;
	faded.volume_decibels = -6.0;
	request.changes.push_back(faded);

	item_outcome<set_item_properties_result> outcome = apply_set_item_properties(host, request);

	REQUIRE(std::holds_alternative<set_item_properties_result>(outcome));

	const set_item_properties_result& result = std::get<set_item_properties_result>(outcome);

	REQUIRE(result.actions.size() == 2);
	CHECK(result.every_change_landed());
	REQUIRE(result.items.size() == 2);

	// The bounds come from the project after the writes, in the order the entries were
	// supplied.
	CHECK(result.items[0].guid == item_guid_for('1'));
	CHECK(result.items[0].position_seconds == 12.0);
	CHECK(result.items[1].guid == item_guid_for('2'));

	// The volume crossed the seam as REAPER's linear gain rather than as decibels: -6 dB
	// is roughly half amplitude, and a handler that passed the decibel figure straight
	// through would have written a number far outside REAPER's range.
	REQUIRE(host.volumes_written.size() == 1);
	CHECK(host.volumes_written[0].second > 0.45);
	CHECK(host.volumes_written[0].second < 0.55);

	CHECK(host.fades_written.size() == 2);
	CHECK(host.mute_write_count == 1);
}

TEST_CASE("set_item_properties reports a trim clamped by the source, not the length asked for", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	// The take runs out at two seconds, so a four-second trim cannot be honoured.
	host.source_length_ceiling = 2.0;

	set_item_properties_request request;

	item_property_change trimmed;
	trimmed.item_guid = item_guid_for('1');
	trimmed.length_seconds = 4.0;
	request.changes.push_back(trimmed);

	const item_outcome<set_item_properties_result> outcome =
		apply_set_item_properties(host, request);

	const set_item_properties_result& result = std::get<set_item_properties_result>(outcome);

	REQUIRE(result.items.size() == 1);

	// The length the producer hears, read back rather than echoed from the request.
	CHECK(result.items[0].length_seconds == 2.0);
}

TEST_CASE("a set_item_properties entry that fails part way says what already landed", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_length = {item_guid_for('1')};

	set_item_properties_request request;

	item_property_change change;
	change.item_guid = item_guid_for('1');
	change.position_seconds = 15.0;
	change.length_seconds = 2.0;
	request.changes.push_back(change);

	const item_outcome<set_item_properties_result> outcome =
		apply_set_item_properties(host, request);

	const set_item_properties_result& result = std::get<set_item_properties_result>(outcome);

	REQUIRE(result.actions.size() == 1);
	require_every_failure_carries_a_reason(result.actions);
	CHECK(failure_code_at(result.actions, 0) == std::string{item_property_write_failed_code});

	// Requirement 9.4 inside one entry: the position write is not rolled back, and the
	// reason names it so the producer is not told nothing happened.
	CHECK(host.find(item_guid_for('1'))->position_seconds == 15.0);
	CHECK(
		std::get<action_failed>(result.actions[0]).error.message().find("position")
		!= std::string::npos);

	// Only successful entries appear in `items`, so this one does not.
	CHECK(result.items.empty());
}

TEST_CASE("set_item_properties fails an entry naming an item and nothing to change", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	set_item_properties_request request;

	item_property_change empty;
	empty.item_guid = item_guid_for('1');
	request.changes.push_back(empty);

	const item_outcome<set_item_properties_result> outcome =
		apply_set_item_properties(host, request);

	const set_item_properties_result& result = std::get<set_item_properties_result>(outcome);

	REQUIRE(result.actions.size() == 1);
	CHECK(failure_code_at(result.actions, 0) == std::string{item_names_no_change_code});
	CHECK(host.positions_written.empty());
}

TEST_CASE("an item GUID that names nothing in the project fails with a reason", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	set_item_properties_request request;

	item_property_change change;
	change.item_guid = item_guid_for('9');
	change.muted = true;
	request.changes.push_back(change);

	const item_outcome<set_item_properties_result> outcome =
		apply_set_item_properties(host, request);

	const set_item_properties_result& result = std::get<set_item_properties_result>(outcome);

	REQUIRE(result.actions.size() == 1);
	require_every_failure_carries_a_reason(result.actions);
	CHECK(failure_code_at(result.actions, 0) == std::string{item_not_found_code});
	CHECK(host.mute_write_count == 0);
}

TEST_CASE("a call naming more items than one result can report changes nothing", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	set_item_properties_request request;

	for (std::size_t entry = 0; entry <= maximum_items_per_reported_call; ++entry)
	{
		item_property_change change;
		change.item_guid = item_guid_for('1');
		change.muted = true;
		request.changes.push_back(change);
	}

	const item_outcome<set_item_properties_result> outcome =
		apply_set_item_properties(host, request);

	// The outcome array caps at 512 while the input allows 4096, and an outcome list cut
	// to fit would be actions that happened with nothing said about them.
	REQUIRE(std::holds_alternative<action_error>(outcome));
	CHECK(std::get<action_error>(outcome).code() == std::string{item_too_many_items_code});
	CHECK(host.mute_write_count == 0);
	CHECK(host.project_read_count == 0);
}

// ---------------------------------------------------------------------------
// move_items
// ---------------------------------------------------------------------------

TEST_CASE("move_items moves by the offset rather than to it", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2')};
	request.offset_seconds = 4.0;

	reported_item_outcome<move_items_result> outcome = apply_move_items(host, std::nullopt, request);

	REQUIRE(std::holds_alternative<item_action_outcomes<move_items_result>>(outcome));

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	CHECK(outcomes.every_item_landed());

	// The positions the host was actually asked to write. An offset written as an
	// absolute position would put both items at four seconds, and the result payload
	// would still read as a clean move.
	CHECK(position_written_for(host, item_guid_for('1')) == 14.0);
	CHECK(position_written_for(host, item_guid_for('2')) == 24.0);

	REQUIRE(outcomes.result.items.size() == 2);
	CHECK(outcomes.result.items[0].position_seconds == 14.0);
	CHECK(outcomes.result.items[1].position_seconds == 24.0);
	CHECK(outcomes.result.applied_offset_seconds == 4.0);
	CHECK(outcomes.result.clamped_item_count == 0);
	CHECK(outcomes.result.to_track.has_value() == false);
}

TEST_CASE("move_items holds an item at zero and reports that it was clamped", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('3')};

	// Pulls the first item past the start of the timeline and the third cleanly.
	request.offset_seconds = -15.0;

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, std::nullopt, request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	CHECK(position_written_for(host, item_guid_for('1')) == 0.0);
	CHECK(position_written_for(host, item_guid_for('3')) == 15.0);

	// One item held, so the two no longer sit at their original spacing — which is a
	// musical consequence rather than a rounding detail, and is why the field exists.
	CHECK(outcomes.result.clamped_item_count == 1);
	CHECK(outcomes.result.applied_offset_seconds == -15.0);
}

TEST_CASE("move_items across tracks reports both the new track and the new position", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.offset_seconds = 2.0;

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, vocal_track(), request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	REQUIRE(outcomes.result.items.size() == 1);

	// A cross-track move is two changes, so "where is it now" is two answers.
	CHECK(outcomes.result.items[0].track_guid == vocal_track_guid);
	CHECK(outcomes.result.items[0].position_seconds == 12.0);
	REQUIRE(outcomes.result.to_track.has_value());
	CHECK(outcomes.result.to_track->name == vocal_track_name);
	REQUIRE(host.track_moves.size() == 1);
}

TEST_CASE("move_items reports an offset of zero when the call only changed track", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1')};

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, vocal_track(), request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	CHECK(outcomes.result.applied_offset_seconds == 0.0);
	CHECK(host.positions_written.empty());
	CHECK(outcomes.result.items[0].position_seconds == 10.0);
}

TEST_CASE("move_items naming neither an offset nor a track is reported as no move", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1')};

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, std::nullopt, request);

	REQUIRE(std::holds_alternative<action_error>(outcome));
	CHECK(std::get<action_error>(outcome).code() == std::string{item_names_no_change_code});
	CHECK(host.positions_written.empty());
}

TEST_CASE("move_items acts on a repeated GUID once and says so", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('1')};
	request.offset_seconds = 4.0;

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, std::nullopt, request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	// Moved once, not twice. Applying the offset per mention would put the item at 18
	// seconds while the result reported a four-second shift.
	CHECK(host.find(item_guid_for('1'))->position_seconds == 14.0);
	REQUIRE(outcomes.actions.size() == 2);
	require_every_failure_carries_a_reason(outcomes.actions);
	CHECK(failure_code_at(outcomes.actions, 1) == std::string{item_duplicate_guid_code});
}

TEST_CASE("a move that REAPER refuses leaves its siblings where they landed", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_position = {item_guid_for('2')};

	move_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2'), item_guid_for('3')};
	request.offset_seconds = 4.0;

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, std::nullopt, request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	REQUIRE(outcomes.actions.size() == 3);
	CHECK(count_applied_actions(outcomes.actions) == 2);
	require_every_failure_carries_a_reason(outcomes.actions);
	CHECK(failure_code_at(outcomes.actions, 1) == std::string{item_position_write_failed_code});

	// Requirement 9.4: no rollback on a sibling's failure.
	CHECK(host.find(item_guid_for('1'))->position_seconds == 14.0);
	CHECK(host.find(item_guid_for('2'))->position_seconds == 20.0);
	CHECK(host.find(item_guid_for('3'))->position_seconds == 34.0);
}

TEST_CASE("an item that cannot be moved to the destination track is not moved along the timeline", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_track_move = {item_guid_for('1')};

	move_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.offset_seconds = 4.0;

	const reported_item_outcome<move_items_result> outcome =
		apply_move_items(host, vocal_track(), request);

	const item_action_outcomes<move_items_result>& outcomes =
		std::get<item_action_outcomes<move_items_result>>(outcome);

	CHECK(failure_code_at(outcomes.actions, 0) == std::string{item_track_move_failed_code});

	// Half a cross-track move is worse than none: an item shifted four seconds on the
	// track it was already on is not what the call asked for.
	CHECK(host.positions_written.empty());
	CHECK(host.find(item_guid_for('1'))->position_seconds == 10.0);
}

// ---------------------------------------------------------------------------
// move_all_items
// ---------------------------------------------------------------------------

TEST_CASE("move_all_items shifts every track when the call names none", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items.push_back(scripted_item('4', 5.0, 2.0, vocal_track(), 1));

	move_all_items_request request;
	request.offset_seconds = 3.0;

	std::variant<move_all_items_outcome, action_error> outcome =
		apply_move_all_items(host, {}, request);

	REQUIRE(std::holds_alternative<move_all_items_outcome>(outcome));

	const move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

	CHECK(moved.every_item_moved());
	CHECK(moved.result.whole_project);
	CHECK(moved.result.tracks.empty());
	CHECK(moved.result.moved_item_count == 4);
	CHECK(moved.result.applied_offset_seconds == 3.0);

	// Every item shifted by the offset rather than moved to it.
	CHECK(host.find(item_guid_for('1'))->position_seconds == 13.0);
	CHECK(host.find(item_guid_for('4'))->position_seconds == 8.0);
}

TEST_CASE("move_all_items shifts only the tracks the call named", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items.push_back(scripted_item('4', 5.0, 2.0, vocal_track(), 1));

	move_all_items_request request;
	request.offset_seconds = 3.0;

	const std::variant<move_all_items_outcome, action_error> outcome =
		apply_move_all_items(host, {vocal_track()}, request);

	const move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

	CHECK(moved.result.whole_project == false);
	REQUIRE(moved.result.tracks.size() == 1);
	CHECK(moved.result.tracks[0].guid == vocal_track_guid);
	CHECK(moved.result.moved_item_count == 1);

	// The three items on the other track are untouched.
	CHECK(host.find(item_guid_for('1'))->position_seconds == 10.0);
	CHECK(host.find(item_guid_for('4'))->position_seconds == 8.0);
}

TEST_CASE("move_all_items counts movement rather than writes", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	move_all_items_request request;
	request.offset_seconds = 0.0;

	const std::variant<move_all_items_outcome, action_error> outcome =
		apply_move_all_items(host, {}, request);

	const move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

	// A schema-valid call that moves nothing. Reporting three items as shifted would be
	// a count of writes, and the producer cannot hear a shift that did not happen.
	CHECK(moved.result.moved_item_count == 0);
	CHECK(moved.result.clamped_item_count == 0);
	CHECK(moved.every_item_moved());
}

TEST_CASE("move_all_items counts an item held at zero as clamped and not as moved", "[item_tools]")
{
	scripted_item_host host;
	host.items = {
		scripted_item('1', 0.0, 2.0, drums_track(), 0),
		scripted_item('2', 20.0, 2.0, drums_track(), 0)};

	move_all_items_request request;
	request.offset_seconds = -5.0;

	const std::variant<move_all_items_outcome, action_error> outcome =
		apply_move_all_items(host, {}, request);

	const move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

	CHECK(moved.result.clamped_item_count == 1);
	CHECK(moved.result.moved_item_count == 1);
}

TEST_CASE("move_all_items reports a refused write as one summary carrying both counts", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_position = {item_guid_for('2')};

	move_all_items_request request;
	request.offset_seconds = 3.0;

	const std::variant<move_all_items_outcome, action_error> outcome =
		apply_move_all_items(host, {}, request);

	const move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

	CHECK(moved.every_item_moved() == false);
	REQUIRE(moved.actions.size() == 2);
	require_every_failure_carries_a_reason(moved.actions);

	// One applied action alongside the failure, because the session really did change and
	// a result with nothing applied carries no undo position for "revert all" to walk to.
	CHECK(count_applied_actions(moved.actions) == 1);
	CHECK(count_failed_actions(moved.actions) == 1);
	CHECK(moved.result.moved_item_count == 2);
}

// ---------------------------------------------------------------------------
// split_items
// ---------------------------------------------------------------------------

TEST_CASE("split_items reports both halves of every split", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	split_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.position_seconds = 12.0;

	reported_item_outcome<split_items_result> outcome = apply_split_items(host, request);

	REQUIRE(std::holds_alternative<item_action_outcomes<split_items_result>>(outcome));

	const item_action_outcomes<split_items_result>& outcomes =
		std::get<item_action_outcomes<split_items_result>>(outcome);

	CHECK(outcomes.every_item_landed());
	REQUIRE(outcomes.result.splits.size() == 1);

	const item_split& split = outcomes.result.splits[0];

	// The left half keeps the original GUID and the right half is new. Without both, the
	// agent cannot address the piece after the split — which is usually what the producer
	// meant to act on next.
	CHECK(split.original_item_guid == item_guid_for('1'));
	CHECK(split.left_item.guid == item_guid_for('1'));
	CHECK(split.right_item.guid != item_guid_for('1'));
	CHECK(split.right_item.guid.empty() == false);

	CHECK(split.left_item.position_seconds == 10.0);
	CHECK(split.left_item.length_seconds == 2.0);
	CHECK(split.right_item.position_seconds == 12.0);
	CHECK(split.right_item.length_seconds == 2.0);

	CHECK(outcomes.result.skipped_item_guids.empty());
	CHECK(outcomes.result.position_seconds == 12.0);
}

TEST_CASE("a split position outside an item is skipped rather than failed", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	split_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2')};

	// Inside the first item, past the end of the second.
	request.position_seconds = 12.0;

	const reported_item_outcome<split_items_result> outcome = apply_split_items(host, request);

	const item_action_outcomes<split_items_result>& outcomes =
		std::get<item_action_outcomes<split_items_result>>(outcome);

	// The output schema has `skippedItemGuids` for exactly this, so it is not a failure —
	// nothing was done to the item and the result says so by name.
	CHECK(outcomes.every_item_landed());
	REQUIRE(outcomes.result.splits.size() == 1);
	REQUIRE(outcomes.result.skipped_item_guids.size() == 1);
	CHECK(outcomes.result.skipped_item_guids[0] == item_guid_for('2'));
}

TEST_CASE("a split position on an item edge splits nothing", "[item_tools]")
{
	const item_state item = scripted_item('1', 10.0, 4.0, drums_track(), 0);

	// A zero-length half is not a half.
	CHECK(split_falls_inside(item, 10.0) == false);
	CHECK(split_falls_inside(item, 14.0) == false);
	CHECK(split_falls_inside(item, 10.001));
	CHECK(split_falls_inside(item, 13.999));
}

TEST_CASE("a split REAPER refuses inside an item is a failure, not a skip", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_split = {item_guid_for('1')};

	split_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.position_seconds = 12.0;

	const reported_item_outcome<split_items_result> outcome = apply_split_items(host, request);

	const item_action_outcomes<split_items_result>& outcomes =
		std::get<item_action_outcomes<split_items_result>>(outcome);

	require_every_failure_carries_a_reason(outcomes.actions);
	CHECK(failure_code_at(outcomes.actions, 0) == std::string{item_split_failed_code});
	CHECK(outcomes.result.skipped_item_guids.empty());
	CHECK(outcomes.result.splits.empty());
}

TEST_CASE("a split whose right half cannot be read back reports neither half", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.right_halves_vanish = true;

	split_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.position_seconds = 12.0;

	const reported_item_outcome<split_items_result> outcome = apply_split_items(host, request);

	const item_action_outcomes<split_items_result>& outcomes =
		std::get<item_action_outcomes<split_items_result>>(outcome);

	// Both halves or neither: a split reported with one side named would send the agent
	// to address an item it cannot see.
	CHECK(outcomes.result.splits.empty());
	CHECK(outcomes.every_item_landed() == false);
	require_every_failure_carries_a_reason(outcomes.actions);
}

TEST_CASE("split_items returns its splits in timeline order", "[item_tools]")
{
	scripted_item_host host;
	host.items = {
		scripted_item('1', 30.0, 10.0, drums_track(), 0),
		scripted_item('2', 10.0, 10.0, drums_track(), 0)};

	split_items_request request;

	// The later item first, so a handler reporting in call order would be caught.
	request.item_guids = {item_guid_for('1'), item_guid_for('2')};
	request.position_seconds = 15.0;

	const reported_item_outcome<split_items_result> outcome = apply_split_items(host, request);

	const item_action_outcomes<split_items_result>& outcomes =
		std::get<item_action_outcomes<split_items_result>>(outcome);

	REQUIRE(outcomes.result.splits.size() == 1);
	CHECK(outcomes.result.splits[0].original_item_guid == item_guid_for('2'));
	REQUIRE(outcomes.result.skipped_item_guids.size() == 1);
}

// ---------------------------------------------------------------------------
// delete_items
// ---------------------------------------------------------------------------

TEST_CASE("delete_items names the tracks and the length that went", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items.push_back(scripted_item('4', 5.0, 1.5, vocal_track(), 1));

	delete_items_request request;
	request.item_guids = {item_guid_for('4'), item_guid_for('1')};

	reported_item_outcome<delete_items_result> outcome = apply_delete_items(host, request);

	REQUIRE(std::holds_alternative<item_action_outcomes<delete_items_result>>(outcome));

	const item_action_outcomes<delete_items_result>& outcomes =
		std::get<item_action_outcomes<delete_items_result>>(outcome);

	CHECK(outcomes.every_item_landed());
	CHECK(outcomes.result.deleted_item_guids.size() == 2);

	// The track an item sat on cannot be read once the item is gone, so both are read
	// from the snapshot taken first — and reported in track order rather than call order,
	// which is what this assertion pins.
	REQUIRE(outcomes.result.affected_tracks.size() == 2);
	CHECK(outcomes.result.affected_tracks[0].guid == drums_track_guid);
	CHECK(outcomes.result.affected_tracks[1].guid == vocal_track_guid);
	CHECK(outcomes.result.affected_tracks[1].name == vocal_track_name);

	// The figure that distinguishes trimming a stray click from removing a verse.
	CHECK(outcomes.result.deleted_length_seconds == 5.5);
}

TEST_CASE("delete_items names one track once however many of its items went", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	delete_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2'), item_guid_for('3')};

	const reported_item_outcome<delete_items_result> outcome = apply_delete_items(host, request);

	const item_action_outcomes<delete_items_result>& outcomes =
		std::get<item_action_outcomes<delete_items_result>>(outcome);

	CHECK(outcomes.result.deleted_item_guids.size() == 3);
	CHECK(outcomes.result.affected_tracks.size() == 1);
	CHECK(host.items.empty());
}

TEST_CASE("a delete REAPER refuses is reported and its siblings still go", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_delete = {item_guid_for('2')};

	delete_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2'), item_guid_for('3')};

	const reported_item_outcome<delete_items_result> outcome = apply_delete_items(host, request);

	const item_action_outcomes<delete_items_result>& outcomes =
		std::get<item_action_outcomes<delete_items_result>>(outcome);

	require_every_failure_carries_a_reason(outcomes.actions);
	CHECK(failure_code_at(outcomes.actions, 1) == std::string{item_delete_failed_code});

	// Only what actually went is reported as gone, and the length follows it.
	CHECK(outcomes.result.deleted_item_guids.size() == 2);
	CHECK(outcomes.result.deleted_length_seconds == 8.0);
	CHECK(host.items.size() == 1);
}

// ---------------------------------------------------------------------------
// list_selected_items
// ---------------------------------------------------------------------------

TEST_CASE("list_selected_items returns the producer's selection in timeline order", "[item_tools]")
{
	scripted_item_host host;
	host.items = {
		scripted_item('3', 30.0, 4.0, drums_track(), 0),
		scripted_item('1', 10.0, 4.0, vocal_track(), 1),
		scripted_item('2', 20.0, 4.0, drums_track(), 0)};

	list_selected_items_request request;

	item_outcome<list_selected_items_result> outcome = apply_list_selected_items(host, request);

	REQUIRE(std::holds_alternative<list_selected_items_result>(outcome));

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	CHECK(result.scope == item_scope::selected_items);
	REQUIRE(result.items.size() == 3);
	CHECK(result.items[0].guid == item_guid_for('1'));
	CHECK(result.items[1].guid == item_guid_for('2'));
	CHECK(result.items[2].guid == item_guid_for('3'));

	// The track name travels with the GUID, because the producer cannot see a GUID.
	CHECK(result.items[0].track_name == vocal_track_name);

	// Take names by default.
	REQUIRE(result.items[0].take_name.has_value());
	CHECK(result.items[0].take_name->empty() == false);

	CHECK(result.total_in_range == 3);
	CHECK(result.truncated == false);
}

TEST_CASE("list_selected_items leaves take names out when the call says so", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	list_selected_items_request request;
	request.include_take_names = false;

	const item_outcome<list_selected_items_result> outcome =
		apply_list_selected_items(host, request);

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	REQUIRE(result.items.size() == 3);
	CHECK(result.items[0].take_name.has_value() == false);
}

TEST_CASE("the selected_tracks scope returns every item on a selected track", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	// One item on a selected track that the producer has not highlighted, which the
	// schema is explicit is worth reading — they may not expect it to be touched.
	item_state unselected = scripted_item('4', 40.0, 4.0, drums_track(), 0);
	unselected.selected = false;
	host.items.push_back(std::move(unselected));

	list_selected_items_request request;
	request.scope = item_scope::selected_tracks;

	const item_outcome<list_selected_items_result> outcome =
		apply_list_selected_items(host, request);

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	REQUIRE(result.items.size() == 4);
	CHECK(result.items[3].guid == item_guid_for('4'));
	CHECK(result.items[3].selected == false);
	CHECK(result.items[0].selected);
}

TEST_CASE("the time_selection scope covers the items that sound during the range", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.time_selection = item_time_range{12.0, 21.0};

	list_selected_items_request request;
	request.scope = item_scope::time_selection;

	const item_outcome<list_selected_items_result> outcome =
		apply_list_selected_items(host, request);

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	// The first item runs from 10 to 14 and so overlaps the range from one side; the
	// second runs from 20 to 24 and overlaps from the other. The third is outside it.
	REQUIRE(result.items.size() == 2);
	CHECK(result.items[0].guid == item_guid_for('1'));
	CHECK(result.items[1].guid == item_guid_for('2'));
	CHECK(result.scope == item_scope::time_selection);
}

TEST_CASE("the time_selection scope with no time selection is an empty answer", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();

	list_selected_items_request request;
	request.scope = item_scope::time_selection;

	const item_outcome<list_selected_items_result> outcome =
		apply_list_selected_items(host, request);

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	// A complete answer, not a failure — and the scope is echoed so it can be explained.
	CHECK(result.items.empty());
	CHECK(result.total_in_range == 0);
	CHECK(result.truncated == false);
	CHECK(result.scope == item_scope::time_selection);
}

TEST_CASE("list_selected_items caps a long list and says it was capped", "[item_tools]")
{
	scripted_item_host host;

	const std::size_t over_the_cap = maximum_reported_items + 5;

	for (std::size_t index = 0; index < over_the_cap; ++index)
	{
		item_state item = scripted_item('1', static_cast<double>(index), 0.5, drums_track(), 0);
		item.guid = "{00000000-0000-0000-0000-" + std::to_string(100000000000ULL + index) + "}";
		host.items.push_back(std::move(item));
	}

	list_selected_items_request request;

	const item_outcome<list_selected_items_result> outcome =
		apply_list_selected_items(host, request);

	const list_selected_items_result& result = std::get<list_selected_items_result>(outcome);

	// Requirement 23.9. This output schema has `totalInRange` and `truncated`, so the cap
	// is visible rather than a silent truncation the agent reasons over as if it were the
	// whole set — which is the opposite answer from `list_track_fx`, whose schema has
	// neither field and which therefore fails instead.
	CHECK(result.items.size() == maximum_reported_items);
	CHECK(result.total_in_range == over_the_cap);
	CHECK(result.truncated);
}

// ---------------------------------------------------------------------------
// import_audio_file and import_midi_file
// ---------------------------------------------------------------------------

TEST_CASE("import_audio_file reports the item REAPER created and what it read", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.file_access = {{"/music/loop.wav", item_file_access::readable}};
	host.resolved_import_path = "/music/project/loop.wav";

	import_file_request request;
	request.file_path = "/music/loop.wav";
	request.position_seconds = 8.0;

	item_outcome<import_audio_file_result> outcome =
		apply_import_audio_file(host, drums_track(), request);

	REQUIRE(std::holds_alternative<import_audio_file_result>(outcome));

	const import_audio_file_result& result = std::get<import_audio_file_result>(outcome);

	CHECK(result.track.guid == drums_track_guid);
	CHECK(result.item.position_seconds == 8.0);

	// The length is the point: the call supplies a position and no duration, so until the
	// file is opened nobody knows where the item ends.
	CHECK(result.item.length_seconds == 3.5);

	// As REAPER resolved it, which is what the producer would find on disk.
	CHECK(result.file_path == "/music/project/loop.wav");

	REQUIRE(result.sample_rate.has_value());
	CHECK(*result.sample_rate == 48000);
	REQUIRE(result.channel_count.has_value());
	CHECK(*result.channel_count == 2);
}

TEST_CASE("a missing path, an unreadable path, and a file REAPER will not decode are three reasons", "[item_tools]")
{
	import_file_request request;
	request.position_seconds = 0.0;

	SECTION("a path that does not exist")
	{
		scripted_item_host host;
		request.file_path = "/music/gone.wav";

		const item_outcome<import_audio_file_result> outcome =
			apply_import_audio_file(host, drums_track(), request);

		REQUIRE(std::holds_alternative<action_error>(outcome));
		CHECK(std::get<action_error>(outcome).code() == std::string{item_file_not_found_code});
	}

	SECTION("a path that exists and cannot be read")
	{
		scripted_item_host host;
		host.file_access = {{"/music/locked.wav", item_file_access::unreadable}};
		request.file_path = "/music/locked.wav";

		const item_outcome<import_audio_file_result> outcome =
			apply_import_audio_file(host, drums_track(), request);

		REQUIRE(std::holds_alternative<action_error>(outcome));
		CHECK(std::get<action_error>(outcome).code() == std::string{item_file_not_readable_code});
	}

	SECTION("a readable file REAPER has no reader for")
	{
		scripted_item_host host;
		host.file_access = {{"/music/odd.xyz", item_file_access::readable}};
		host.import_lands = false;
		request.file_path = "/music/odd.xyz";

		const item_outcome<import_audio_file_result> outcome =
			apply_import_audio_file(host, drums_track(), request);

		// REAPER decides what it can decode, and this is REAPER's answer rather than
		// anything read out of the file here.
		REQUIRE(std::holds_alternative<action_error>(outcome));
		CHECK(std::get<action_error>(outcome).code() == std::string{item_file_not_importable_code});
	}
}

TEST_CASE("import_midi_file reports how many parts arrived and how many notes", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/parts.mid", item_file_access::readable}};
	host.imported_track_count = 3;
	host.imported_note_count = 412;

	import_file_request request;
	request.file_path = "/music/parts.mid";
	request.position_seconds = 0.0;

	const item_outcome<import_midi_file_result> outcome =
		apply_import_midi_file(host, vocal_track(), request);

	REQUIRE(std::holds_alternative<import_midi_file_result>(outcome));

	const import_midi_file_result& result = std::get<import_midi_file_result>(outcome);

	// More than one means the parts arrived together on one item rather than separated,
	// which is rarely what a producer importing a multi-part file expects.
	CHECK(result.imported_track_count == 3);
	CHECK(result.note_count == 412);
	CHECK(result.track.guid == vocal_track_guid);
}

TEST_CASE("a MIDI import REAPER cannot describe is not reported as one clean part", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/parts.mid", item_file_access::readable}};
	host.imported_track_count = 0;

	import_file_request request;
	request.file_path = "/music/parts.mid";

	const item_outcome<import_midi_file_result> outcome =
		apply_import_midi_file(host, vocal_track(), request);

	// Rounding an unknown part count up to one would say exactly the thing the field
	// exists to prevent.
	REQUIRE(std::holds_alternative<action_error>(outcome));
	CHECK(std::get<action_error>(outcome).code() == std::string{item_import_unreportable_code});
}

TEST_CASE("an import whose item cannot be found afterwards is reported as a failure", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/loop.wav", item_file_access::readable}};
	host.imported_item_is_readable_afterwards = false;

	import_file_request request;
	request.file_path = "/music/loop.wav";

	const item_outcome<import_audio_file_result> outcome =
		apply_import_audio_file(host, drums_track(), request);

	REQUIRE(std::holds_alternative<action_error>(outcome));
}

TEST_CASE("an import reports no sample rate when REAPER did not say", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/loop.wav", item_file_access::readable}};
	host.imported_sample_rate = 0;
	host.imported_channel_count = 0;

	import_file_request request;
	request.file_path = "/music/loop.wav";

	const item_outcome<import_audio_file_result> outcome =
		apply_import_audio_file(host, drums_track(), request);

	const import_audio_file_result& result = std::get<import_audio_file_result>(outcome);

	// Both fields are optional in the schema, so an unknown is omitted rather than
	// reported as a sample rate of nothing.
	CHECK(result.sample_rate.has_value() == false);
	CHECK(result.channel_count.has_value() == false);
}

// ---------------------------------------------------------------------------
// The host being unusable
// ---------------------------------------------------------------------------

TEST_CASE("an unusable host fails every item tool with a reason naming the functions", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.usable = false;
	host.missing_function_names = {"SplitMediaItem", "MoveMediaItemToTrack"};

	const std::optional<action_error> unusable = check_item_host_usable(host);

	REQUIRE(unusable.has_value());
	CHECK(unusable->code() == std::string{item_host_unavailable_code});
	CHECK(unusable->message().find("SplitMediaItem") != std::string::npos);
	CHECK(unusable->message().find("MoveMediaItemToTrack") != std::string::npos);

	// The point of the check: an unusable host reports a project with no items in it, so
	// a handler that proceeded would tell the producer their selection was empty.
	delete_items_request request;
	request.item_guids = {item_guid_for('1')};

	const reported_item_outcome<delete_items_result> outcome = apply_delete_items(host, request);

	REQUIRE(std::holds_alternative<action_error>(outcome));
	CHECK(std::get<action_error>(outcome).code() == std::string{item_host_unavailable_code});
	CHECK(host.deleted_item_guids.empty());
}

TEST_CASE("an offset never moves an item before zero", "[item_tools]")
{
	CHECK(apply_offset(10.0, 4.0).position_seconds == 14.0);
	CHECK(apply_offset(10.0, 4.0).clamped == false);
	CHECK(apply_offset(3.0, -10.0).position_seconds == 0.0);
	CHECK(apply_offset(3.0, -10.0).clamped);
	CHECK(apply_offset(0.0, 0.0).clamped == false);
}

// ---------------------------------------------------------------------------
// Registration and dispatch through the framework
// ---------------------------------------------------------------------------

namespace
{
	// A payload that carries whichever request the test is making, which is what stands in
	// for the codec here. The framework holds the input as an opaque pointer it never
	// dereferences (requirement 4.4), so the suite can drive all eight handlers with no
	// JSON library present.
	struct test_payload
	{
		std::string described_result;

		std::optional<set_item_properties_request> set_item_properties;
		std::optional<move_items_request> move_items;
		std::optional<move_all_items_request> move_all_items;
		std::optional<split_items_request> split_items;
		std::optional<delete_items_request> delete_items;
		std::optional<list_selected_items_request> list_selected_items;
		std::optional<import_file_request> import_audio_file;
		std::optional<import_file_request> import_midi_file;
	};

	struct test_request_reader
	{
		bool operator()(const test_payload& input, set_item_properties_request& request) const
		{
			if (!input.set_item_properties.has_value())
			{
				return false;
			}

			request = *input.set_item_properties;

			return true;
		}

		bool operator()(const test_payload& input, move_items_request& request) const
		{
			if (!input.move_items.has_value())
			{
				return false;
			}

			request = *input.move_items;

			return true;
		}

		bool operator()(const test_payload& input, move_all_items_request& request) const
		{
			if (!input.move_all_items.has_value())
			{
				return false;
			}

			request = *input.move_all_items;

			return true;
		}

		bool operator()(const test_payload& input, split_items_request& request) const
		{
			if (!input.split_items.has_value())
			{
				return false;
			}

			request = *input.split_items;

			return true;
		}

		bool operator()(const test_payload& input, delete_items_request& request) const
		{
			if (!input.delete_items.has_value())
			{
				return false;
			}

			request = *input.delete_items;

			return true;
		}

		bool operator()(const test_payload& input, list_selected_items_request& request) const
		{
			if (!input.list_selected_items.has_value())
			{
				return false;
			}

			request = *input.list_selected_items;

			return true;
		}

		// The two imports read into the same request type, so the tool name is what tells
		// them apart.
		bool operator()(
			const test_payload& input,
			import_file_request& request,
			std::string_view tool_name) const
		{
			const std::optional<import_file_request>& supplied =
				tool_name == import_midi_file_tool_name ? input.import_midi_file : input.import_audio_file;

			if (!supplied.has_value())
			{
				return false;
			}

			request = *supplied;

			return true;
		}
	};

	// Turns a result into a payload. Enough of each result is described that a test can
	// tell which handler produced it and what it said.
	struct test_result_writer
	{
		test_payload operator()(const set_item_properties_result& result) const
		{
			test_payload payload;
			payload.described_result = "set_item_properties " + std::to_string(result.items.size())
				+ " of " + std::to_string(result.actions.size());

			return payload;
		}

		test_payload operator()(const move_items_result& result) const
		{
			test_payload payload;
			payload.described_result = "move_items " + std::to_string(result.items.size()) + " by "
				+ std::to_string(result.applied_offset_seconds) + " clamping "
				+ std::to_string(result.clamped_item_count);

			return payload;
		}

		test_payload operator()(const move_all_items_result& result) const
		{
			test_payload payload;
			payload.described_result = std::string{"move_all_items "}
				+ (result.whole_project ? "whole project " : "named tracks ")
				+ std::to_string(result.moved_item_count);

			return payload;
		}

		test_payload operator()(const split_items_result& result) const
		{
			test_payload payload;
			payload.described_result = "split_items " + std::to_string(result.splits.size())
				+ " skipping " + std::to_string(result.skipped_item_guids.size());

			return payload;
		}

		test_payload operator()(const delete_items_result& result) const
		{
			test_payload payload;
			payload.described_result = "delete_items "
				+ std::to_string(result.deleted_item_guids.size()) + " from "
				+ std::to_string(result.affected_tracks.size());

			return payload;
		}

		test_payload operator()(const list_selected_items_result& result) const
		{
			test_payload payload;
			payload.described_result = "list_selected_items " + std::to_string(result.items.size())
				+ " of " + std::to_string(result.total_in_range);

			return payload;
		}

		test_payload operator()(const import_audio_file_result& result) const
		{
			test_payload payload;
			payload.described_result = "import_audio_file " + result.file_path;

			return payload;
		}

		test_payload operator()(const import_midi_file_result& result) const
		{
			test_payload payload;
			payload.described_result =
				"import_midi_file " + std::to_string(result.imported_track_count) + " parts";

			return payload;
		}
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;

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

			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			++undo_call_count;

			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		std::vector<std::string> end_block_descriptions;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class scripted_track_list final : public TrackListSource
	{
	public:
		explicit scripted_track_list(std::vector<ResolvableTrack> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		std::vector<ResolvableTrack> tracks_in_project_order() const override { return tracks_; }

	private:
		std::vector<ResolvableTrack> tracks_;
	};

	std::vector<ResolvableTrack> two_scripted_tracks()
	{
		ResolvableTrack drums;
		drums.guid = drums_track_guid;
		drums.name = drums_track_name;
		drums.project_index = 0;

		ResolvableTrack vocal;
		vocal.guid = vocal_track_guid;
		vocal.name = vocal_track_name;
		vocal.project_index = 1;

		return {drums, vocal};
	}

	// Everything one dispatch needs, kept together so a test reads as the call it makes.
	struct scripted_executor
	{
		explicit scripted_executor(scripted_item_host& host)
			: tracks{two_scripted_tracks()},
			undo{stack},
			executor{registry, undo, tracks, aliases}
		{
			registrations = register_item_tools<test_payload>(
				registry,
				host,
				test_request_reader{},
				test_result_writer{});
		}

		test_registry registry;
		scripted_undo_stack stack;
		scripted_track_list tracks;
		NoLearnedAliases aliases;
		undo_manager undo;
		test_executor executor;
		std::vector<item_tool_registration> registrations;
	};

	test_call call_for(std::string tool_name, const test_payload& input)
	{
		test_call call;
		call.tool_name = std::move(tool_name);
		call.request_id = "request-1";
		call.validated_input = &input;

		return call;
	}

	test_call call_for(std::string tool_name, const test_payload& input, const std::string& track_guid)
	{
		test_call call = call_for(std::move(tool_name), input);
		call.track_selectors = {TrackSelector{TrackGuidSelector{track_guid}}};

		return call;
	}

	test_result dispatched(const test_outcome& outcome)
	{
		REQUIRE(std::holds_alternative<test_result>(outcome));

		return std::get<test_result>(outcome);
	}

	tool_success<test_payload> success_of(const test_outcome& outcome)
	{
		const test_result result = dispatched(outcome);

		REQUIRE(std::holds_alternative<tool_success<test_payload>>(result));

		return std::get<tool_success<test_payload>>(result);
	}

	tool_partial_outcome<test_payload> partial_of(const test_outcome& outcome)
	{
		const test_result result = dispatched(outcome);

		REQUIRE(std::holds_alternative<tool_partial_outcome<test_payload>>(result));

		return std::get<tool_partial_outcome<test_payload>>(result);
	}
}

TEST_CASE("all eight item tools register, and through the seam each one belongs to", "[item_tools]")
{
	scripted_item_host host;
	scripted_executor scripted{host};

	REQUIRE(scripted.registrations.size() == item_tool_count);
	CHECK(every_item_tool_registered(scripted.registrations));
	CHECK(scripted.registry.registered_tool_count() == item_tool_count);

	const auto* const read = scripted.registry.find_tool(list_selected_items_tool_name);

	REQUIRE(read != nullptr);
	CHECK(read->undo_effect() == tool_undo_effect::none);

	const std::vector<std::string_view> mutations{
		set_item_properties_tool_name,
		move_items_tool_name,
		move_all_items_tool_name,
		split_items_tool_name,
		delete_items_tool_name,
		import_audio_file_tool_name,
		import_midi_file_tool_name};

	for (const std::string_view tool_name : mutations)
	{
		const auto* const registered = scripted.registry.find_tool(tool_name);

		REQUIRE(registered != nullptr);
		CHECK(registered->undo_effect() == tool_undo_effect::undo_block);
	}
}

TEST_CASE("a mutating item tool runs inside exactly one undo block", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	move_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2')};
	request.offset_seconds = 4.0;
	input.move_items = request;

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("move_items", input)));

	CHECK(success.fields.described_result.find("move_items 2") == 0);

	// Requirement 10.8: one block for the whole array, and a marker captured for the turn.
	CHECK(scripted.stack.begin_block_call_count == 1);
	CHECK(scripted.stack.end_block_call_count == 1);
	CHECK(scripted.stack.open_block_depth == 0);
	REQUIRE(success.undo.has_value());
	CHECK(scripted.undo.has_undo_position_marker());

	// And nothing was rewound.
	CHECK(scripted.stack.undo_call_count == 0);
}

TEST_CASE("list_selected_items opens no undo block and records no marker", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	input.list_selected_items = list_selected_items_request{};

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("list_selected_items", input)));

	CHECK(success.fields.described_result == "list_selected_items 3 of 3");
	CHECK(success.undo.has_value() == false);
	CHECK(scripted.stack.begin_block_call_count == 0);

	// Requirement 10.3: a read-only turn records no marker, so it offers no "revert all".
	CHECK(scripted.undo.has_undo_position_marker() == false);
}

TEST_CASE("an item failure is a failed action carrying a reason, never a refusal", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	delete_items_request request;
	request.item_guids = {item_guid_for('9')};
	input.delete_items = request;

	const test_outcome outcome = scripted.executor.execute(call_for("delete_items", input));

	// The refusal schema's `reason` enum has no value for an item GUID that names nothing,
	// and no acknowledgement would make the call valid — so it is a failure. The only
	// refusals an item tool can produce are target resolution's.
	CHECK(std::holds_alternative<tool_refusal>(dispatched(outcome)) == false);

	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	require_every_failure_carries_a_reason(partial.actions);
	CHECK(failure_code_at(partial.actions, 0) == std::string{item_not_found_code});

	// Nothing landed, so there is no position for "revert all" to walk back to.
	CHECK(partial.undo.has_value() == false);
}

TEST_CASE("a partial item outcome keeps the undo report and names each item by GUID", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_delete = {item_guid_for('2')};

	scripted_executor scripted{host};

	test_payload input;
	delete_items_request request;
	request.item_guids = {item_guid_for('1'), item_guid_for('2'), item_guid_for('3')};
	input.delete_items = request;

	const tool_partial_outcome<test_payload>& partial =
		partial_of(scripted.executor.execute(call_for("delete_items", input)));

	CHECK(partial.applied_action_count() == 2);
	CHECK(partial.failed_action_count() == 1);

	// Each outcome carries the item's GUID, which is what makes a partial as informative
	// as the success payload would have been — a partial carries no tool-specific fields.
	CHECK(action_target(partial.actions[0]) == item_guid_for('1'));
	CHECK(action_target(partial.actions[1]) == item_guid_for('2'));
	CHECK(action_target(partial.actions[2]) == item_guid_for('3'));

	// One block for the whole array, and something landed, so the undo report stands.
	CHECK(scripted.stack.begin_block_call_count == 1);
	CHECK(scripted.stack.end_block_call_count == 1);
	REQUIRE(partial.undo.has_value());

	// No rollback: the two that went stay gone, and REAPER's undo is the way back.
	CHECK(host.items.size() == 1);
}

TEST_CASE("set_item_properties returns its own payload even when an entry fails", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_length = {item_guid_for('2')};

	scripted_executor scripted{host};

	test_payload input;
	set_item_properties_request request;

	item_property_change landed;
	landed.item_guid = item_guid_for('1');
	landed.muted = true;
	request.changes.push_back(landed);

	item_property_change refused;
	refused.item_guid = item_guid_for('2');
	refused.length_seconds = 1.0;
	request.changes.push_back(refused);

	input.set_item_properties = request;

	// Its output schema requires `actions` and `items` together, so the payload is always
	// the tool's own — the framework's partial shape would drop `items`.
	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("set_item_properties", input)));

	CHECK(success.fields.described_result == "set_item_properties 1 of 2");

	// One entry landed, so there is a position for "revert all" to walk back to.
	REQUIRE(success.undo.has_value());
}

TEST_CASE("set_item_properties reports no undo marker when every entry failed", "[item_tools]")
{
	// The cost of always returning `handler_success`: the framework attaches the undo
	// report and cannot see inside the payload to know that nothing landed (requirement
	// 4.4). `set-item-properties.schema.json` says the *absence* of `undoPositionBefore` is
	// what tells the producer nothing changed, so a marker here would claim a change
	// nobody made — and a marker sitting above the agent's earlier work shortens the range
	// "revert all" would walk, which is the harm requirement 10.7 names for a refused
	// mutation. The handler says so with `handler_success::nothing_was_applied`.
	scripted_item_host host = host_with_three_items();
	host.items_that_refuse_length = {item_guid_for('1'), item_guid_for('2')};

	scripted_executor scripted{host};

	test_payload input;
	set_item_properties_request request;

	for (const char marker : {'1', '2'})
	{
		item_property_change refused;
		refused.item_guid = item_guid_for(marker);
		refused.length_seconds = 1.0;
		request.changes.push_back(refused);
	}

	input.set_item_properties = request;

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("set_item_properties", input)));

	// Still the tool's own payload, carrying both failures.
	CHECK(success.fields.described_result == "set_item_properties 0 of 2");

	CHECK_FALSE(success.undo.has_value());

	// The block was still opened and closed. What is withheld is the report, not the
	// block — an unbalanced block is worse than a missing marker.
	CHECK(scripted.stack.begin_block_call_count == 1);
	CHECK(scripted.stack.end_block_call_count == 1);
}

TEST_CASE("move_items resolves its destination track through the executor", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	move_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.offset_seconds = 1.0;
	input.move_items = request;

	const tool_success<test_payload>& success = success_of(
		scripted.executor.execute(call_for("move_items", input, vocal_track_guid)));

	CHECK(success.fields.described_result.find("move_items 1") == 0);
	REQUIRE(host.track_moves.size() == 1);
	CHECK(host.track_moves[0].second == vocal_track_guid);
}

TEST_CASE("move_all_items with no track selector shifts the whole project", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	move_all_items_request request;
	request.offset_seconds = 2.0;
	input.move_all_items = request;

	const tool_success<test_payload>& success =
		success_of(scripted.executor.execute(call_for("move_all_items", input)));

	CHECK(success.fields.described_result == "move_all_items whole project 3");
	REQUIRE(success.undo.has_value());
}

TEST_CASE("an unresolvable destination track refuses the call before anything moves", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	test_payload input;
	move_items_request request;
	request.item_guids = {item_guid_for('1')};
	request.offset_seconds = 4.0;
	input.move_items = request;

	const test_outcome outcome = scripted.executor.execute(
		call_for("move_items", input, "{00000000-0000-0000-0000-0000000000ff}"));

	// The one refusal an item tool can produce, and it is target resolution's rather than
	// the handler's: the handler never ran.
	const test_result result = dispatched(outcome);

	REQUIRE(std::holds_alternative<tool_refusal>(result));
	CHECK(host.positions_written.empty());

	// Requirement 9.3 and 10.7: a refused mutation opens no block and records no marker.
	CHECK(scripted.stack.begin_block_call_count == 0);
	CHECK(scripted.undo.has_undo_position_marker() == false);
}

TEST_CASE("an import that addresses no track fails rather than guessing one", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/loop.wav", item_file_access::readable}};

	scripted_executor scripted{host};

	test_payload input;
	import_file_request request;
	request.file_path = "/music/loop.wav";
	input.import_audio_file = request;

	const tool_partial_outcome<test_payload>& partial =
		partial_of(scripted.executor.execute(call_for("import_audio_file", input)));

	REQUIRE(partial.actions.size() == 1);
	require_every_failure_carries_a_reason(partial.actions);
}

TEST_CASE("the two imports are told apart by tool name through one request type", "[item_tools]")
{
	scripted_item_host host;
	host.file_access = {{"/music/parts.mid", item_file_access::readable}};
	host.imported_track_count = 2;

	scripted_executor scripted{host};

	test_payload input;
	import_file_request request;
	request.file_path = "/music/parts.mid";
	input.import_midi_file = request;

	const tool_success<test_payload>& success = success_of(
		scripted.executor.execute(call_for("import_midi_file", input, drums_track_guid)));

	CHECK(success.fields.described_result == "import_midi_file 2 parts");

	// The audio handler reads the audio slot, which this payload does not carry, so it
	// fails rather than importing the MIDI request's path.
	test_payload audio_input;
	audio_input.import_midi_file = request;

	const tool_partial_outcome<test_payload>& partial = partial_of(
		scripted.executor.execute(call_for("import_audio_file", audio_input, drums_track_guid)));

	REQUIRE(partial.actions.size() == 1);
	require_every_failure_carries_a_reason(partial.actions);
}

TEST_CASE("an input the handler cannot read fails the action rather than crashing", "[item_tools]")
{
	scripted_item_host host = host_with_three_items();
	scripted_executor scripted{host};

	// A payload carrying no request for this tool, which is what an upstream mismatch
	// looks like from in here.
	test_payload input;

	const tool_partial_outcome<test_payload>& partial =
		partial_of(scripted.executor.execute(call_for("split_items", input)));

	REQUIRE(partial.actions.size() == 1);
	require_every_failure_carries_a_reason(partial.actions);
	CHECK(host.project_read_count == 0);
}
