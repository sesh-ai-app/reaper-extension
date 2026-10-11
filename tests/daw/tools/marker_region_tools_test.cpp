// Marker, region, tempo map, and time selection tools — `create_marker`,
// `create_region`, `update_marker_or_region`, `delete_marker_or_region`,
// `change_tempo_map`, and `set_time_selection`.
//
// Four states these tests exist to try to reach:
//
//   - A marker rejected for having an end that equals its start (requirement 9.6
//     applied where it must not be). REAPER stores `D_ENDPOS` equal to `D_STARTPOS` for
//     a marker, so a range check on the marker path rejects every marker there is, and
//     nothing about the code looks wrong while it happens.
//   - A zero-length or inverted span accepted (requirement 9.6 not applied where it
//     must be), or applied and reported as a refusal rather than as a failed action.
//   - A region reported without its span, or with its end read off the wrong field —
//     the one difference between the two branches of the `oneOf`, and the difference
//     that turns "render the chorus" into a render of nothing.
//   - A tempo map write reported as clean when a point did not land, or a failure
//     reported with no reason (requirements 9.4, 9.5).
//
// The range check is not re-derived here. `check_end_after_start` is the Tool Executor's
// own, swept over 1156 pairs of doubles by `tool_executor_property_test.cpp`, and the
// assertions below check that each handler routes through it and reports its code —
// never that a hand-written comparison in a test agrees with a hand-written comparison
// in the code it was written beside.
//
// The scripted session below is what makes all of this checkable without REAPER. It
// keeps markers and regions in one list the way REAPER does, distinguished by a flag,
// and it renumbers the displayed numbers by position on every change — which REAPER
// does, and which is why an update reads the object back rather than composing a result
// from what was asked for.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/marker_region_tools.h>

using sesh_ai::daw::action_error;
using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_succeeded;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::check_end_after_start;
using sesh_ai::daw::count_applied_actions;
using sesh_ai::daw::count_failed_actions;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::invalid_time_range_code;
using sesh_ai::daw::maximum_action_outcomes;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::outcome_report_target;
using sesh_ai::daw::outcome_report_truncated_code;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_partial_outcome;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_registration_outcome;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;

using sesh_ai::daw::tools::ambiguous_marker_or_region_code;
using sesh_ai::daw::tools::apply_change_tempo_map;
using sesh_ai::daw::tools::apply_create_marker;
using sesh_ai::daw::tools::apply_create_region;
using sesh_ai::daw::tools::apply_delete_marker_or_region;
using sesh_ai::daw::tools::apply_list_markers;
using sesh_ai::daw::tools::apply_list_regions;
using sesh_ai::daw::tools::apply_set_time_selection;
using sesh_ai::daw::tools::apply_update_marker_or_region;
using sesh_ai::daw::tools::bounded_result_cap;
using sesh_ai::daw::tools::check_read_window;
using sesh_ai::daw::tools::change_tempo_map_request;
using sesh_ai::daw::tools::change_tempo_map_result;
using sesh_ai::daw::tools::change_tempo_map_tool_name;
using sesh_ai::daw::tools::color_from_reaper_custom_color;
using sesh_ai::daw::tools::create_marker_request;
using sesh_ai::daw::tools::create_marker_result;
using sesh_ai::daw::tools::create_marker_tool_name;
using sesh_ai::daw::tools::create_region_request;
using sesh_ai::daw::tools::create_region_result;
using sesh_ai::daw::tools::create_region_tool_name;
using sesh_ai::daw::tools::delete_marker_or_region_request;
using sesh_ai::daw::tools::delete_marker_or_region_result;
using sesh_ai::daw::tools::delete_marker_or_region_tool_name;
using sesh_ai::daw::tools::describe_marker_or_region_kind;
using sesh_ai::daw::tools::end_position_on_a_marker_code;
using sesh_ai::daw::tools::initial_tempo_and_time_signature;
using sesh_ai::daw::tools::list_markers_result;
using sesh_ai::daw::tools::list_markers_tool_name;
using sesh_ai::daw::tools::list_regions_result;
using sesh_ai::daw::tools::list_regions_tool_name;
using sesh_ai::daw::tools::marker_create_failed_code;
using sesh_ai::daw::tools::marker_detail;
using sesh_ai::daw::tools::marker_detail_kind;
using sesh_ai::daw::tools::marker_or_region;
using sesh_ai::daw::tools::marker_or_region_creation;
using sesh_ai::daw::tools::marker_or_region_delete_failed_code;
using sesh_ai::daw::tools::marker_or_region_update_failed_code;
using sesh_ai::daw::tools::marker_or_region_write;
using sesh_ai::daw::tools::marker_region_outcome;
using sesh_ai::daw::tools::marker_region_payload_codec;
using sesh_ai::daw::tools::marker_region_read_payload_codec;
using sesh_ai::daw::tools::MarkerRegionHost;
using sesh_ai::daw::tools::maximum_listed_markers_or_regions;
using sesh_ai::daw::tools::no_marker_or_region_change_requested_code;
using sesh_ai::daw::tools::no_tempo_points_supplied_code;
using sesh_ai::daw::tools::reaper_custom_color_enabled_flag;
using sesh_ai::daw::tools::reaper_custom_color_from;
using sesh_ai::daw::tools::region_create_failed_code;
using sesh_ai::daw::tools::region_detail;
using sesh_ai::daw::tools::region_detail_kind;
using sesh_ai::daw::tools::register_marker_region_read_tools;
using sesh_ai::daw::tools::register_marker_region_tools;
using sesh_ai::daw::tools::set_time_selection_request;
using sesh_ai::daw::tools::set_time_selection_result;
using sesh_ai::daw::tools::set_time_selection_tool_name;
using sesh_ai::daw::tools::tempo_map_clear_failed_code;
using sesh_ai::daw::tools::tempo_map_point;
using sesh_ai::daw::tools::tempo_point_index_at;
using sesh_ai::daw::tools::tempo_point_insertion_index;
using sesh_ai::daw::tools::tempo_point_write_failed_code;
using sesh_ai::daw::tools::tempo_time_signature;
using sesh_ai::daw::tools::the_requested_span_target;
using sesh_ai::daw::tools::time_selection_write_failed_code;
using sesh_ai::daw::tools::timeline_read_window;
using sesh_ai::daw::tools::unknown_marker_or_region_code;
using sesh_ai::daw::tools::update_marker_or_region_request;
using sesh_ai::daw::tools::update_marker_or_region_result;
using sesh_ai::daw::tools::update_marker_or_region_tool_name;

namespace
{
	// ---------------------------------------------------------------------------
	// Building a session
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

	marker_or_region marker_at(double position_seconds, std::string name, int distinguishing_number)
	{
		marker_or_region entry;
		entry.object.guid = guid_for(distinguishing_number);
		entry.object.name = std::move(name);
		entry.object.is_region = false;
		entry.object.start_seconds = position_seconds;

		// What REAPER reports for a marker: the end is the start.
		entry.object.end_seconds = position_seconds;

		return entry;
	}

	marker_or_region region_spanning(
		double start_seconds,
		double end_seconds,
		std::string name,
		int distinguishing_number)
	{
		marker_or_region entry;
		entry.object.guid = guid_for(distinguishing_number);
		entry.object.name = std::move(name);
		entry.object.is_region = true;
		entry.object.start_seconds = start_seconds;
		entry.object.end_seconds = end_seconds;

		return entry;
	}

	// A scripted REAPER session.
	//
	// Applies the same changes REAPER would, including the parts that matter: markers and
	// regions share one list distinguished by a flag, the displayed numbers are assigned
	// by position and reassigned whenever anything moves, and a tempo map write moves the
	// items that are positioned in beats and leaves the ones positioned in time.
	class scripted_session final : public MarkerRegionHost
	{
	public:
		scripted_session() = default;

		explicit scripted_session(std::vector<marker_or_region> markers_and_regions)
			: markers_and_regions_{std::move(markers_and_regions)}
		{
			renumber();
		}

		std::vector<marker_or_region> read_markers_and_regions_in_enumeration_order() override
		{
			++read_call_count;

			if (forget_everything)
			{
				return {};
			}

			return markers_and_regions_;
		}

		std::optional<marker_or_region> create_marker_or_region(
			const marker_or_region_creation& creation) override
		{
			++create_call_count;
			creations.push_back(creation);

			if (refuse_creation)
			{
				return std::nullopt;
			}

			marker_or_region created;
			created.object.guid = guid_for(next_guid_number_++);
			created.object.name = creation.name;
			created.object.is_region = creation.is_region;
			created.object.start_seconds = creation.start_seconds;
			created.object.end_seconds = creation.is_region
				? creation.end_seconds
				: creation.start_seconds;
			created.color = creation.color;

			markers_and_regions_.push_back(created);
			renumber();

			return *find(created.object.guid);
		}

		bool write_marker_or_region(const marker_or_region_write& write) override
		{
			++write_call_count;
			writes.push_back(write);

			if (refuse_writes)
			{
				return false;
			}

			marker_or_region* const entry = find(write.guid);

			if (entry == nullptr)
			{
				return false;
			}

			if (write.name.has_value())
			{
				entry->object.name = *write.name;
			}

			if (write.start_seconds.has_value())
			{
				entry->object.start_seconds = *write.start_seconds;
			}

			if (write.end_seconds.has_value())
			{
				entry->object.end_seconds = *write.end_seconds;
			}

			if (write.color.has_value())
			{
				entry->color = write.color;
			}

			renumber();

			// The one case that makes reading back rather than composing observable in
			// the other direction: REAPER accepted the write and then does not report the
			// object.
			if (forget_after_write)
			{
				forget_everything = true;
			}

			return true;
		}

		bool delete_marker_or_region(const std::string& guid) override
		{
			++delete_call_count;
			deleted_guids.push_back(guid);

			if (refuse_deletes)
			{
				return false;
			}

			const std::size_t before = markers_and_regions_.size();

			markers_and_regions_.erase(
				std::remove_if(
					markers_and_regions_.begin(),
					markers_and_regions_.end(),
					[&guid](const marker_or_region& entry) { return entry.object.guid == guid; }),
				markers_and_regions_.end());

			renumber();

			return markers_and_regions_.size() < before;
		}

		std::vector<tempo_map_point> read_tempo_map() override
		{
			++tempo_read_call_count;

			return tempo_map;
		}

		// The seam task 20.1 widened. None of the six tools in this file reads it —
		// `list_tempo_changes` does, and `tempo_tools_test.cpp` drives it — so this
		// scripted session answers with a plain session at 120 BPM in 4/4.
		std::optional<initial_tempo_and_time_signature>
			read_initial_tempo_and_time_signature() override
		{
			if (!project_tempo_readable)
			{
				return std::nullopt;
			}

			return initial_tempo_and_time_signature{120.0, tempo_time_signature{4, 4}};
		}

		bool write_tempo_point(
			std::optional<std::size_t> replacing_index,
			const tempo_map_point& point) override
		{
			tempo_writes.push_back(std::make_pair(replacing_index, point));

			if (refuse_tempo_point_at.has_value()
				&& point.position_seconds == *refuse_tempo_point_at)
			{
				return false;
			}

			if (replacing_index.has_value())
			{
				if (*replacing_index >= tempo_map.size())
				{
					// REAPER would be addressing a point that is not there. Reported as a
					// refusal to write so that an index the tools computed wrongly shows
					// up as a failure rather than as a silent no-op.
					return false;
				}

				tempo_map[*replacing_index] = point;
			}
			else
			{
				const std::size_t insertion_index =
					tempo_point_insertion_index(positions_of(tempo_map), point.position_seconds);

				tempo_map.insert(
					tempo_map.begin() + static_cast<std::ptrdiff_t>(insertion_index),
					point);
			}

			move_beat_based_items();

			return true;
		}

		bool clear_tempo_map() override
		{
			++clear_call_count;

			if (refuse_clear)
			{
				return false;
			}

			tempo_map.clear();
			move_beat_based_items();

			return true;
		}

		std::vector<double> read_item_positions() override
		{
			++item_read_call_count;

			return item_positions;
		}

		bool write_time_selection(double start_seconds, double end_seconds) override
		{
			++time_selection_call_count;

			if (refuse_time_selection)
			{
				return false;
			}

			time_selection = std::make_pair(start_seconds, end_seconds);

			return true;
		}

		const std::vector<marker_or_region>& markers_and_regions() const
		{
			return markers_and_regions_;
		}

		marker_or_region* find(const std::string& guid)
		{
			for (marker_or_region& entry : markers_and_regions_)
			{
				if (entry.object.guid == guid)
				{
					return &entry;
				}
			}

			return nullptr;
		}

		std::string guid_of(std::string_view name) const
		{
			for (const marker_or_region& entry : markers_and_regions_)
			{
				if (entry.object.name == name)
				{
					return entry.object.guid;
				}
			}

			return {};
		}

		// What the session holds.
		std::vector<tempo_map_point> tempo_map;
		std::vector<double> item_positions;
		std::vector<std::size_t> beat_based_item_indices;
		std::optional<std::pair<double, double>> time_selection;

		// What it refuses.
		bool refuse_creation = false;
		bool refuse_writes = false;
		bool refuse_deletes = false;
		bool refuse_clear = false;
		bool refuse_time_selection = false;
		bool forget_after_write = false;
		bool forget_everything = false;
		bool project_tempo_readable = true;
		std::optional<double> refuse_tempo_point_at;

		// What it was asked to do.
		std::vector<marker_or_region_creation> creations;
		std::vector<marker_or_region_write> writes;
		std::vector<std::string> deleted_guids;
		std::vector<std::pair<std::optional<std::size_t>, tempo_map_point>> tempo_writes;

		int read_call_count = 0;
		int create_call_count = 0;
		int write_call_count = 0;
		int delete_call_count = 0;
		int tempo_read_call_count = 0;
		int item_read_call_count = 0;
		int clear_call_count = 0;
		int time_selection_call_count = 0;

	private:
		static std::vector<double> positions_of(const std::vector<tempo_map_point>& points)
		{
			std::vector<double> positions;
			positions.reserve(points.size());

			for (const tempo_map_point& point : points)
			{
				positions.push_back(point.position_seconds);
			}

			return positions;
		}

		// REAPER's own bookkeeping: the list is enumerated in position order, and the
		// displayed numbers are assigned within each kind. Modelled because it is the
		// reason an update has to read the object back — moving a marker can change the
		// number the producer sees for it without anything else about it changing.
		void renumber()
		{
			std::stable_sort(
				markers_and_regions_.begin(),
				markers_and_regions_.end(),
				[](const marker_or_region& left, const marker_or_region& right) {
					return left.object.start_seconds < right.object.start_seconds;
				});

			int next_marker_number = 1;
			int next_region_number = 1;

			for (std::size_t index = 0; index < markers_and_regions_.size(); ++index)
			{
				markers_and_regions_[index].object.internal_index = static_cast<int>(index);

				if (markers_and_regions_[index].object.is_region)
				{
					markers_and_regions_[index].object.displayed_number = next_region_number;
					++next_region_number;

					continue;
				}

				markers_and_regions_[index].object.displayed_number = next_marker_number;
				++next_marker_number;
			}
		}

		// A tempo change moves everything positioned in beats and nothing positioned in
		// time. The distance does not matter to any assertion here; that some items move
		// and others do not is the whole of what `movedItemCount` reports.
		void move_beat_based_items()
		{
			for (const std::size_t index : beat_based_item_indices)
			{
				if (index < item_positions.size())
				{
					item_positions[index] += 1.5;
				}
			}
		}

		std::vector<marker_or_region> markers_and_regions_;
		int next_guid_number_ = 900;
	};

	// ---------------------------------------------------------------------------
	// Reading an outcome
	// ---------------------------------------------------------------------------

	template <typename ResultType>
	const ResultType& result_of(const marker_region_outcome<ResultType>& outcome)
	{
		REQUIRE(std::holds_alternative<ResultType>(outcome));

		return std::get<ResultType>(outcome);
	}

	template <typename ResultType>
	const handler_action_outcomes& outcomes_of(const marker_region_outcome<ResultType>& outcome)
	{
		REQUIRE(std::holds_alternative<handler_action_outcomes>(outcome));

		return std::get<handler_action_outcomes>(outcome);
	}

	const action_failed& failure_at(const handler_action_outcomes& outcomes, std::size_t index)
	{
		REQUIRE(index < outcomes.actions.size());
		REQUIRE(std::holds_alternative<action_failed>(outcomes.actions[index]));

		return std::get<action_failed>(outcomes.actions[index]);
	}

	// One failed action carrying the expected code, and a reason that is not empty.
	//
	// Requirement 9.5 checked at every rejection site rather than only in the property
	// test, because a reasonless failure is what the type system already prevents and a
	// *useless* reason is what it cannot.
	template <typename ResultType>
	void require_single_failure(
		const marker_region_outcome<ResultType>& outcome,
		std::string_view expected_code)
	{
		const handler_action_outcomes& outcomes = outcomes_of(outcome);

		REQUIRE(outcomes.actions.size() == 1);
		REQUIRE_FALSE(action_was_applied(outcomes.actions[0]));

		const action_failed& failed = failure_at(outcomes, 0);

		REQUIRE(failed.error.code() == std::string{expected_code});
		REQUIRE_FALSE(failed.error.message().empty());
		REQUIRE_FALSE(failed.target.empty());
	}
}

// ---------------------------------------------------------------------------
// The shape of a rejection
// ---------------------------------------------------------------------------

TEST_CASE("no handler in this component can produce a refusal", "[daw][tools][markers]")
{
	// Requirement 9.6 as amended, and the file's claim that none of the six has a
	// refusal to give, stated as a type rather than as a comment. A `tool_refusal`
	// alternative on any of these six outcome types would not survive this.
	STATIC_REQUIRE(
		std::is_same_v<
			marker_region_outcome<create_region_result>,
			std::variant<create_region_result, handler_action_outcomes>>);

	STATIC_REQUIRE_FALSE(
		std::is_same_v<
			marker_region_outcome<create_region_result>,
			std::variant<create_region_result, handler_action_outcomes, tool_refusal>>);

	// The check every one of the three contexts routes through returns a reason, not a
	// refusal. Asserted here as well as in the property test because this is the file
	// whose handlers could quietly stop calling it.
	STATIC_REQUIRE(
		std::is_same_v<decltype(check_end_after_start(0.0, 1.0)), std::optional<action_error>>);
}

// ---------------------------------------------------------------------------
// create_marker — a point, and never range-checked
// ---------------------------------------------------------------------------

TEST_CASE("create_marker places a point and reports the GUID and number REAPER assigned", "[daw][tools][markers]")
{
	scripted_session session;

	create_marker_request request;
	request.position_seconds = 12.5;
	request.name = "Verse";
	request.color = 0x00FF00;

	const auto outcome = apply_create_marker(session, request);
	const create_marker_result& result = result_of(outcome);

	REQUIRE_FALSE(result.marker.guid.empty());
	REQUIRE(result.marker.position_seconds == 12.5);
	REQUIRE(result.marker.name == "Verse");
	REQUIRE(result.marker.color == std::optional<int>{0x00FF00});

	// The number REAPER displays, which is what the producer sees on the ruler.
	REQUIRE(result.marker.index == 1);

	// A marker is stored as a point: REAPER reports its end equal to its start, and the
	// creation says so rather than leaving the end at zero.
	REQUIRE(session.creations.size() == 1);
	REQUIRE_FALSE(session.creations[0].is_region);
	REQUIRE(session.creations[0].start_seconds == 12.5);
	REQUIRE(session.creations[0].end_seconds == 12.5);
}

TEST_CASE("create_marker is not subject to the end-after-start check", "[daw][tools][markers]")
{
	// The mutation this test exists to catch. A marker's end equals its start, so
	// `check_end_after_start` applied on this path rejects every marker that can be
	// created — including the first one in a session, at position zero.
	scripted_session session;

	for (const double position_seconds : {0.0, 1.0, 480.0})
	{
		INFO("position " << position_seconds);

		// The same pair the check would be given, and it rejects it.
		REQUIRE(check_end_after_start(position_seconds, position_seconds).has_value());

		create_marker_request request;
		request.position_seconds = position_seconds;
		request.name = "Marker";

		const auto outcome = apply_create_marker(session, request);

		// And the marker is created anyway, because the check does not apply to a point.
		REQUIRE(std::holds_alternative<create_marker_result>(outcome));
	}

	REQUIRE(session.markers_and_regions().size() == 3);
}

TEST_CASE("create_marker reports a failed action when REAPER does not create it", "[daw][tools][markers]")
{
	scripted_session session;
	session.refuse_creation = true;

	create_marker_request request;
	request.position_seconds = 4.0;
	request.name = "Drop";

	require_single_failure(apply_create_marker(session, request), marker_create_failed_code);
	REQUIRE(session.markers_and_regions().empty());
}

// ---------------------------------------------------------------------------
// create_region — a span, and the first of requirement 9.6's three contexts
// ---------------------------------------------------------------------------

TEST_CASE("create_region writes the span and reports both bounds", "[daw][tools][markers]")
{
	scripted_session session;

	create_region_request request;
	request.start_seconds = 64.0;
	request.end_seconds = 96.0;
	request.name = "Chorus";

	const auto outcome = apply_create_region(session, request);
	const create_region_result& result = result_of(outcome);

	REQUIRE(result.region.start_seconds == 64.0);

	// The end is the end. Reporting the start here would turn "render the chorus" into a
	// render of nothing, and it is the one field the two branches of the `oneOf` differ
	// by.
	REQUIRE(result.region.end_seconds == 96.0);
	REQUIRE(result.region.end_seconds > result.region.start_seconds);
	REQUIRE(result.region.name == "Chorus");

	REQUIRE(session.creations.size() == 1);
	REQUIRE(session.creations[0].is_region);
	REQUIRE(session.creations[0].end_seconds == 96.0);
}

TEST_CASE("create_region rejects an inverted span as a failed action before writing anything", "[daw][tools][markers]")
{
	scripted_session session;

	create_region_request request;
	request.start_seconds = 96.0;
	request.end_seconds = 64.0;
	request.name = "Backwards";

	require_single_failure(apply_create_region(session, request), invalid_time_range_code);

	// The operation did not proceed, which is the other half of requirement 9.6.
	REQUIRE(session.create_call_count == 0);
	REQUIRE(session.markers_and_regions().empty());
}

TEST_CASE("create_region rejects an equal start and end", "[daw][tools][markers]")
{
	// The second mutation this suite exists to catch. `>` rather than `>=` is what makes
	// a zero-length region impossible, and a region of no length is not a region — it is
	// a marker drawn in the region lane, which nothing can render and nothing can select.
	scripted_session session;

	create_region_request request;
	request.start_seconds = 32.0;
	request.end_seconds = 32.0;
	request.name = "Nothing";

	require_single_failure(apply_create_region(session, request), invalid_time_range_code);
	REQUIRE(session.create_call_count == 0);
}

TEST_CASE("create_region rejects a span with a NaN bound", "[daw][tools][markers]")
{
	scripted_session session;

	create_region_request request;
	request.start_seconds = std::numeric_limits<double>::quiet_NaN();
	request.end_seconds = 10.0;
	request.name = "Unknowable";

	require_single_failure(apply_create_region(session, request), invalid_time_range_code);
	REQUIRE(session.create_call_count == 0);
}

TEST_CASE("create_region reports a failed action when REAPER does not create it", "[daw][tools][markers]")
{
	scripted_session session;
	session.refuse_creation = true;

	create_region_request request;
	request.start_seconds = 1.0;
	request.end_seconds = 2.0;
	request.name = "Bridge";

	require_single_failure(apply_create_region(session, request), region_create_failed_code);
}

// ---------------------------------------------------------------------------
// update_marker_or_region — the tool that has to read `B_ISREGION` first
// ---------------------------------------------------------------------------

TEST_CASE("updating a marker renames it without touching its position", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Verse 1";

	const auto outcome = apply_update_marker_or_region(session, request);
	const update_marker_or_region_result& result = result_of(outcome);

	REQUIRE(describe_marker_or_region_kind(result.updated) == marker_detail_kind);

	const marker_detail& marker = std::get<marker_detail>(result.updated);

	REQUIRE(marker.name == "Verse 1");
	REQUIRE(marker.position_seconds == 8.0);

	// No bound was named, so no bound was written — and no range check was run on a pair
	// that would fail one.
	REQUIRE(session.writes.size() == 1);
	REQUIRE_FALSE(session.writes[0].start_seconds.has_value());
	REQUIRE_FALSE(session.writes[0].end_seconds.has_value());
}

TEST_CASE("a marker rename is not rejected for the marker's own degenerate span", "[daw][tools][markers]")
{
	// The third form of the marker mutation. A marker's start and end are equal, so a
	// range check on the update path — even one that only looks at what the object
	// already holds — makes renaming any marker impossible.
	scripted_session session{{marker_at(0.0, "Start", 1)}};

	REQUIRE(check_end_after_start(0.0, 0.0).has_value());

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Top";

	REQUIRE(std::holds_alternative<update_marker_or_region_result>(
		apply_update_marker_or_region(session, request)));
}

TEST_CASE("moving a marker writes both bounds and reports the number REAPER now displays", "[daw][tools][markers]")
{
	scripted_session session{{
		marker_at(10.0, "Verse", 1),
		marker_at(20.0, "Chorus", 2),
	}};

	// Second on the ruler to begin with.
	REQUIRE(session.find(guid_for(2))->object.displayed_number == 2);

	update_marker_or_region_request request;
	request.guid = guid_for(2);
	request.position_seconds = 5.0;

	const auto outcome = apply_update_marker_or_region(session, request);
	const marker_detail& marker = std::get<marker_detail>(result_of(outcome).updated);

	REQUIRE(marker.position_seconds == 5.0);

	// REAPER renumbers markers by position, so the object is read back rather than
	// composed: a composed result would still claim the number it had before the move.
	REQUIRE(marker.index == 1);

	// Both fields, because they are one value for a marker. Writing only the start would
	// leave a record describing a span from the new position to the old one.
	REQUIRE(session.writes.size() == 1);
	REQUIRE(session.writes[0].start_seconds == std::optional<double>{5.0});
	REQUIRE(session.writes[0].end_seconds == std::optional<double>{5.0});
}

TEST_CASE("an end position on a marker is a failed action, not a silent no-op", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.end_seconds = 16.0;

	require_single_failure(
		apply_update_marker_or_region(session, request),
		end_position_on_a_marker_code);

	// Nothing was written. A marker that silently grew an end, or a success reported for
	// a field the object does not have, are both worse than being told.
	REQUIRE(session.write_call_count == 0);
	REQUIRE(session.find(guid_for(1))->object.end_seconds == 8.0);
}

TEST_CASE("updating a region moves the span it names and leaves the other bound alone", "[daw][tools][markers]")
{
	scripted_session session{{region_spanning(64.0, 96.0, "Chorus", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.end_seconds = 128.0;

	const auto outcome = apply_update_marker_or_region(session, request);

	REQUIRE(describe_marker_or_region_kind(result_of(outcome).updated) == region_detail_kind);

	const region_detail& region = std::get<region_detail>(result_of(outcome).updated);

	REQUIRE(region.start_seconds == 64.0);
	REQUIRE(region.end_seconds == 128.0);

	REQUIRE(session.writes.size() == 1);
	REQUIRE_FALSE(session.writes[0].start_seconds.has_value());
	REQUIRE(session.writes[0].end_seconds == std::optional<double>{128.0});
}

TEST_CASE("a region update that would invert the span is a failed action", "[daw][tools][markers]")
{
	// The second of requirement 9.6's three contexts, and the case the check has to see
	// the *effective* span for: only one bound is supplied, so the rejection depends on
	// the bound the region already holds.
	scripted_session session{{region_spanning(64.0, 96.0, "Chorus", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.end_seconds = 32.0;

	require_single_failure(
		apply_update_marker_or_region(session, request),
		invalid_time_range_code);

	REQUIRE(session.write_call_count == 0);
	REQUIRE(session.find(guid_for(1))->object.end_seconds == 96.0);
}

TEST_CASE("a region update to an equal start and end is a failed action", "[daw][tools][markers]")
{
	scripted_session session{{region_spanning(64.0, 96.0, "Chorus", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.position_seconds = 96.0;

	require_single_failure(
		apply_update_marker_or_region(session, request),
		invalid_time_range_code);

	REQUIRE(session.write_call_count == 0);
}

TEST_CASE("renaming a region does not run the range check on a span the call never touched", "[daw][tools][markers]")
{
	// A region REAPER already holds with its end at its start was not written by this
	// extension — nothing here can produce one. Refusing to rename it would leave the
	// producer unable to fix it through this tool at all, which is a false rejection
	// rather than a safety check.
	scripted_session session{{region_spanning(64.0, 64.0, "Broken", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Chorus";

	const auto outcome = apply_update_marker_or_region(session, request);

	REQUIRE(std::holds_alternative<update_marker_or_region_result>(outcome));
	REQUIRE(session.find(guid_for(1))->object.name == "Chorus");
}

TEST_CASE("an unknown GUID is a failed action rather than a refusal", "[daw][tools][markers]")
{
	// `object_resolver.h`'s own instruction: the refusal schema's `reason` names an
	// unresolved *track* selector and has no value for a marker or region, so this comes
	// back as a failure carrying why.
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(404);
	request.name = "Nothing";

	require_single_failure(
		apply_update_marker_or_region(session, request),
		unknown_marker_or_region_code);

	REQUIRE(session.write_call_count == 0);
}

TEST_CASE("an empty GUID is a failed action", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	update_marker_or_region_request request;
	request.name = "Nothing";

	require_single_failure(
		apply_update_marker_or_region(session, request),
		unknown_marker_or_region_code);
}

TEST_CASE("a GUID two objects share is a failed action naming the ambiguity", "[daw][tools][markers]")
{
	// Not something REAPER should produce, which is exactly why it is reported rather
	// than resolved by picking one: guessing between two objects that claim one identity
	// would edit whichever happened to be enumerated first.
	scripted_session session{{
		marker_at(8.0, "Verse", 1),
		marker_at(16.0, "Verse again", 1),
	}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Which one";

	require_single_failure(
		apply_update_marker_or_region(session, request),
		ambiguous_marker_or_region_code);

	REQUIRE(session.write_call_count == 0);
}

TEST_CASE("an update asking for no change is a failed action", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	update_marker_or_region_request request;
	request.guid = guid_for(1);

	require_single_failure(
		apply_update_marker_or_region(session, request),
		no_marker_or_region_change_requested_code);

	REQUIRE(session.write_call_count == 0);
}

TEST_CASE("an update REAPER refuses is a failed action naming the kind", "[daw][tools][markers]")
{
	scripted_session session{{region_spanning(1.0, 2.0, "Intro", 1)}};
	session.refuse_writes = true;

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Intro 2";

	const auto outcome = apply_update_marker_or_region(session, request);

	require_single_failure(outcome, marker_or_region_update_failed_code);

	const std::string message = failure_at(outcomes_of(outcome), 0).error.message();

	REQUIRE(message.find("region") != std::string::npos);
}

TEST_CASE("an update REAPER accepts but will not read back still reports what landed", "[daw][tools][markers]")
{
	// The write succeeded, so a failure here would be a lie. The index is the one field
	// that can be stale, and the schema already says it must not be used for addressing.
	scripted_session session{{region_spanning(4.0, 8.0, "Intro", 1)}};
	session.forget_after_write = true;

	update_marker_or_region_request request;
	request.guid = guid_for(1);
	request.name = "Intro 2";
	request.end_seconds = 12.0;

	const auto outcome = apply_update_marker_or_region(session, request);
	const region_detail& region = std::get<region_detail>(result_of(outcome).updated);

	REQUIRE(region.guid == guid_for(1));
	REQUIRE(region.name == "Intro 2");
	REQUIRE(region.end_seconds == 12.0);
	REQUIRE(region.start_seconds == 4.0);
}

// ---------------------------------------------------------------------------
// delete_marker_or_region
// ---------------------------------------------------------------------------

TEST_CASE("deleting a marker reports the marker branch read before the deletion", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	delete_marker_or_region_request request;
	request.guid = guid_for(1);

	const auto outcome = apply_delete_marker_or_region(session, request);
	const delete_marker_or_region_result& result = result_of(outcome);

	REQUIRE(describe_marker_or_region_kind(result.deleted) == marker_detail_kind);

	const marker_detail& marker = std::get<marker_detail>(result.deleted);

	REQUIRE(marker.name == "Verse");
	REQUIRE(marker.position_seconds == 8.0);

	// And it is gone, which is why the detail had to be read first.
	REQUIRE(session.markers_and_regions().empty());
}

TEST_CASE("deleting a region reports the span that is gone", "[daw][tools][markers]")
{
	scripted_session session{{region_spanning(64.0, 96.0, "Chorus", 1)}};

	delete_marker_or_region_request request;
	request.guid = guid_for(1);

	const auto outcome = apply_delete_marker_or_region(session, request);
	const delete_marker_or_region_result& result = result_of(outcome);

	REQUIRE(describe_marker_or_region_kind(result.deleted) == region_detail_kind);

	const region_detail& region = std::get<region_detail>(result.deleted);

	REQUIRE(region.name == "Chorus");
	REQUIRE(region.start_seconds == 64.0);

	// The end, read off the end. The producer needs to hear "removed the region Chorus,
	// 64 to 96 seconds" — reporting the start twice would describe a region that never
	// existed.
	REQUIRE(region.end_seconds == 96.0);
	REQUIRE(region.end_seconds != region.start_seconds);
}

TEST_CASE("deleting a marker is not blocked by its end equalling its start", "[daw][tools][markers]")
{
	// The last form of the marker mutation. Applying the range check on the delete path
	// would make every marker in every session undeletable.
	scripted_session session{{marker_at(30.0, "Cue", 1)}};

	REQUIRE(check_end_after_start(30.0, 30.0).has_value());

	delete_marker_or_region_request request;
	request.guid = guid_for(1);

	REQUIRE(std::holds_alternative<delete_marker_or_region_result>(
		apply_delete_marker_or_region(session, request)));
	REQUIRE(session.markers_and_regions().empty());
}

TEST_CASE("deleting an unknown GUID is a failed action", "[daw][tools][markers]")
{
	scripted_session session{{marker_at(8.0, "Verse", 1)}};

	delete_marker_or_region_request request;
	request.guid = guid_for(404);

	require_single_failure(
		apply_delete_marker_or_region(session, request),
		unknown_marker_or_region_code);

	REQUIRE(session.delete_call_count == 0);
	REQUIRE(session.markers_and_regions().size() == 1);
}

TEST_CASE("a deletion REAPER refuses is a failed action and leaves the object", "[daw][tools][markers]")
{
	scripted_session session{{region_spanning(1.0, 2.0, "Intro", 1)}};
	session.refuse_deletes = true;

	delete_marker_or_region_request request;
	request.guid = guid_for(1);

	require_single_failure(
		apply_delete_marker_or_region(session, request),
		marker_or_region_delete_failed_code);

	REQUIRE(session.markers_and_regions().size() == 1);
}

// ---------------------------------------------------------------------------
// change_tempo_map
// ---------------------------------------------------------------------------

namespace
{
	tempo_map_point tempo_point(double position_seconds, double beats_per_minute)
	{
		tempo_map_point point;
		point.position_seconds = position_seconds;
		point.beats_per_minute = beats_per_minute;

		return point;
	}

	scripted_session session_with_items()
	{
		scripted_session session;
		session.item_positions = {0.0, 4.0, 8.0, 12.0};

		// Two of the four are positioned in beats, so a tempo change moves those two and
		// leaves the others where they are.
		session.beat_based_item_indices = {1, 3};

		return session;
	}
}

TEST_CASE("a tempo point at a new position is written and counted as written", "[daw][tools][markers][tempo]")
{
	scripted_session session = session_with_items();
	session.tempo_map = {tempo_point(0.0, 120.0)};

	change_tempo_map_request request;
	request.points = {tempo_point(64.0, 140.0)};

	const auto outcome = apply_change_tempo_map(session, request);
	const change_tempo_map_result& result = result_of(outcome);

	REQUIRE(result.points_written == 1);
	REQUIRE(result.points_replaced == 0);
	REQUIRE_FALSE(result.replaced_existing);

	// The map as it now stands, which is the same count the snapshot reports.
	REQUIRE(result.tempo_change_count == 2);

	// Measured rather than predicted: two of the four items are positioned in beats.
	REQUIRE(result.moved_item_count == 2);

	REQUIRE(session.tempo_writes.size() == 1);
	REQUIRE_FALSE(session.tempo_writes[0].first.has_value());
}

TEST_CASE("a tempo point at a position that already has one replaces it", "[daw][tools][markers][tempo]")
{
	// `pointsWritten` and `pointsReplaced` are separate because "add a tempo change" and
	// "change the tempo change that was there" are different operations, and the producer
	// asked for one of them.
	scripted_session session = session_with_items();
	session.tempo_map = {tempo_point(0.0, 120.0), tempo_point(64.0, 140.0)};

	change_tempo_map_request request;
	request.points = {tempo_point(64.0, 150.0)};

	const auto outcome = apply_change_tempo_map(session, request);
	const change_tempo_map_result& result = result_of(outcome);

	REQUIRE(result.points_written == 0);
	REQUIRE(result.points_replaced == 1);
	REQUIRE(result.tempo_change_count == 2);

	REQUIRE(session.tempo_writes.size() == 1);
	REQUIRE(session.tempo_writes[0].first == std::optional<std::size_t>{1});
	REQUIRE(session.tempo_map[1].beats_per_minute == 150.0);
}

TEST_CASE("a point a nanosecond from an existing one is a replacement rather than a second point", "[daw][tools][markers][tempo]")
{
	// A position that made a round trip through JSON comes back a few bits away from
	// where it started. Two tempo markers a nanosecond apart are drawn on top of each
	// other and neither can be selected, so the near match is treated as the same
	// position.
	scripted_session session;
	session.tempo_map = {tempo_point(64.0, 140.0)};

	change_tempo_map_request request;
	request.points = {tempo_point(64.0 + 1e-12, 150.0)};

	const auto outcome = apply_change_tempo_map(session, request);

	REQUIRE(result_of(outcome).points_replaced == 1);
	REQUIRE(result_of(outcome).points_written == 0);
	REQUIRE(session.tempo_map.size() == 1);
}

TEST_CASE("indices account for points inserted earlier in the same call", "[daw][tools][markers][tempo]")
{
	// A tempo marker is addressed by its index in position order, so inserting one shifts
	// every later index. Re-deriving the index from the map as this call has left it —
	// rather than as it was when the call began — is what keeps the second point from
	// overwriting the wrong one.
	scripted_session session;
	session.tempo_map = {tempo_point(0.0, 120.0), tempo_point(96.0, 160.0)};

	change_tempo_map_request request;
	request.points = {
		tempo_point(32.0, 130.0),
		tempo_point(96.0, 170.0),
	};

	const auto outcome = apply_change_tempo_map(session, request);
	const change_tempo_map_result& result = result_of(outcome);

	REQUIRE(result.points_written == 1);
	REQUIRE(result.points_replaced == 1);
	REQUIRE(result.tempo_change_count == 3);

	// The insert landed at index 1, so the point that was at index 1 is now at index 2 —
	// and that is the index the replacement addressed.
	REQUIRE(session.tempo_writes.size() == 2);
	REQUIRE(session.tempo_writes[1].first == std::optional<std::size_t>{2});

	REQUIRE(session.tempo_map.size() == 3);
	REQUIRE(session.tempo_map[2].position_seconds == 96.0);
	REQUIRE(session.tempo_map[2].beats_per_minute == 170.0);
}

TEST_CASE("replaceExisting clears the map before writing and says so", "[daw][tools][markers][tempo]")
{
	scripted_session session = session_with_items();
	session.tempo_map = {tempo_point(0.0, 120.0), tempo_point(64.0, 140.0)};

	change_tempo_map_request request;
	request.points = {tempo_point(0.0, 90.0)};
	request.replace_existing = true;

	const auto outcome = apply_change_tempo_map(session, request);
	const change_tempo_map_result& result = result_of(outcome);

	REQUIRE(session.clear_call_count == 1);
	REQUIRE(result.replaced_existing);

	// Nothing was there to replace once the map was cleared, so every supplied point is
	// a write. The producer losing their whole map is a different conversation from
	// merging points in, and the result has to make the two distinguishable.
	REQUIRE(result.points_written == 1);
	REQUIRE(result.points_replaced == 0);
	REQUIRE(result.tempo_change_count == 1);
}

TEST_CASE("a clear REAPER refuses writes no points", "[daw][tools][markers][tempo]")
{
	scripted_session session;
	session.tempo_map = {tempo_point(0.0, 120.0)};
	session.refuse_clear = true;

	change_tempo_map_request request;
	request.points = {tempo_point(32.0, 140.0)};
	request.replace_existing = true;

	require_single_failure(
		apply_change_tempo_map(session, request),
		tempo_map_clear_failed_code);

	REQUIRE(session.tempo_writes.empty());
	REQUIRE(session.tempo_map.size() == 1);
}

TEST_CASE("a point REAPER refuses is reported per action and its siblings stay written", "[daw][tools][markers][tempo]")
{
	// Requirements 9.4 and 9.5: one undo block for the whole array, no rollback of what
	// landed, and a reason on the failure.
	scripted_session session;
	session.tempo_map = {tempo_point(0.0, 120.0)};
	session.refuse_tempo_point_at = 64.0;

	change_tempo_map_request request;
	request.points = {
		tempo_point(32.0, 130.0),
		tempo_point(64.0, 140.0),
		tempo_point(96.0, 150.0),
	};

	const auto outcome = apply_change_tempo_map(session, request);
	const handler_action_outcomes& outcomes = outcomes_of(outcome);

	REQUIRE(outcomes.actions.size() == 3);
	REQUIRE(action_was_applied(outcomes.actions[0]));
	REQUIRE_FALSE(action_was_applied(outcomes.actions[1]));
	REQUIRE(action_was_applied(outcomes.actions[2]));

	REQUIRE(failure_at(outcomes, 1).error.code() == std::string{tempo_point_write_failed_code});
	REQUIRE_FALSE(failure_at(outcomes, 1).error.message().empty());

	// The two that landed stay landed.
	REQUIRE(session.tempo_map.size() == 3);
}

TEST_CASE("a tempo map call with no points is a failed action", "[daw][tools][markers][tempo]")
{
	scripted_session session;

	require_single_failure(
		apply_change_tempo_map(session, change_tempo_map_request{}),
		no_tempo_points_supplied_code);

	REQUIRE(session.tempo_writes.empty());
}

TEST_CASE("a time signature change travels as an absence when it is not asked for", "[daw][tools][markers][tempo]")
{
	// "Keep the preceding time signature" and "change to 0/0" are not the same request,
	// so the absence is carried as one rather than as a pair of zeroes.
	scripted_session session;

	tempo_map_point with_signature = tempo_point(0.0, 120.0);
	with_signature.time_signature = tempo_time_signature{6, 8};

	change_tempo_map_request request;
	request.points = {with_signature, tempo_point(32.0, 140.0)};

	REQUIRE(std::holds_alternative<change_tempo_map_result>(
		apply_change_tempo_map(session, request)));

	REQUIRE(session.tempo_writes.size() == 2);
	REQUIRE(session.tempo_writes[0].second.time_signature
		== std::optional<tempo_time_signature>{tempo_time_signature{6, 8}});
	REQUIRE_FALSE(session.tempo_writes[1].second.time_signature.has_value());
}

TEST_CASE("a point-by-point report reports every point and leaves the cap to the framework", "[daw][tools][markers][tempo]")
{
	// `points` allows 1024 and `actions` allows 512, so a long map with a failure cannot be
	// reported point by point. This handler used to cut the list itself and append a
	// `tempo_outcome_report_truncated` code of its own; the cap belongs to
	// `tool-result-partial.schema.json` rather than to tempo maps, so
	// `tool_executor.h`'s `build_partial_outcome` applies it for every handler that can
	// overflow it and names the gap one way. What this handler owes is one outcome per
	// point, in order — see the dispatched case below for what the framework then does.
	scripted_session session;

	change_tempo_map_request request;

	for (std::size_t index = 0; index < 600; ++index)
	{
		request.points.push_back(tempo_point(static_cast<double>(index) + 1.0, 120.0));
	}

	// One of them fails, which is what puts the call on the per-action path at all.
	session.refuse_tempo_point_at = 5.0;

	const auto outcome = apply_change_tempo_map(session, request);
	const handler_action_outcomes& outcomes = outcomes_of(outcome);

	REQUIRE(outcomes.actions.size() == 600);
	REQUIRE(outcomes.actions.size() > maximum_action_outcomes);
	REQUIRE(count_failed_actions(outcomes.actions) == 1);
}

TEST_CASE("tempo point index arithmetic is exact at the edges", "[daw][tools][markers][tempo]")
{
	const std::vector<double> positions{0.0, 32.0, 96.0};

	REQUIRE(tempo_point_index_at(positions, 0.0) == std::optional<std::size_t>{0});
	REQUIRE(tempo_point_index_at(positions, 96.0) == std::optional<std::size_t>{2});
	REQUIRE_FALSE(tempo_point_index_at(positions, 64.0).has_value());

	REQUIRE(tempo_point_insertion_index(positions, -1.0) == 0);
	REQUIRE(tempo_point_insertion_index(positions, 64.0) == 2);
	REQUIRE(tempo_point_insertion_index(positions, 1000.0) == 3);
}

// ---------------------------------------------------------------------------
// set_time_selection — the third of requirement 9.6's three contexts
// ---------------------------------------------------------------------------

TEST_CASE("set_time_selection writes the span and reports its length", "[daw][tools][markers]")
{
	scripted_session session;

	set_time_selection_request request;
	request.start_seconds = 30.0;
	request.end_seconds = 60.0;

	const auto outcome = apply_set_time_selection(session, request);
	const set_time_selection_result& result = result_of(outcome);

	REQUIRE(result.start_seconds == 30.0);
	REQUIRE(result.end_seconds == 60.0);
	REQUIRE(result.length_seconds == 30.0);

	REQUIRE(session.time_selection == std::make_pair(30.0, 60.0));
}

TEST_CASE("set_time_selection rejects an inverted span with the invalid time range code", "[daw][tools][markers]")
{
	scripted_session session;

	set_time_selection_request request;
	request.start_seconds = 60.0;
	request.end_seconds = 30.0;

	require_single_failure(apply_set_time_selection(session, request), invalid_time_range_code);

	// Nothing was selected, which is requirement 9.6's "the operation does not proceed".
	REQUIRE(session.time_selection_call_count == 0);
	REQUIRE_FALSE(session.time_selection.has_value());
}

TEST_CASE("set_time_selection rejects an empty span", "[daw][tools][markers]")
{
	scripted_session session;

	set_time_selection_request request;
	request.start_seconds = 30.0;
	request.end_seconds = 30.0;

	require_single_failure(apply_set_time_selection(session, request), invalid_time_range_code);
	REQUIRE(session.time_selection_call_count == 0);
}

TEST_CASE("a time selection REAPER refuses is a failed action", "[daw][tools][markers]")
{
	scripted_session session;
	session.refuse_time_selection = true;

	set_time_selection_request request;
	request.start_seconds = 0.0;
	request.end_seconds = 8.0;

	require_single_failure(
		apply_set_time_selection(session, request),
		time_selection_write_failed_code);
}

// ---------------------------------------------------------------------------
// Colour, which crosses the seam as REAPER stores it
// ---------------------------------------------------------------------------

TEST_CASE("a colour without REAPER's enabled flag is reported as no colour", "[daw][tools][markers]")
{
	// REAPER stores a colour it was told to ignore. Reporting that value would show the
	// producer a colour they cannot see on their own ruler.
	REQUIRE_FALSE(color_from_reaper_custom_color(0x00FF00).has_value());
	REQUIRE(color_from_reaper_custom_color(0x00FF00 | reaper_custom_color_enabled_flag)
		== std::optional<int>{0x00FF00});

	REQUIRE(color_from_reaper_custom_color(reaper_custom_color_from(0x123456))
		== std::optional<int>{0x123456});
}

// ---------------------------------------------------------------------------
// list_markers and list_regions (task 20.1, requirements 25.2 and 25.3)
//
// Two tools over one REAPER list, so the states these try to reach are the ones the
// shared list makes possible:
//
//   - A region in `list_markers`' result, or a marker in `list_regions`'. The agent
//     would be handed a named span as a point, or a point as a span of zero length,
//     and neither reads as wrong at the call site.
//   - A region reported with its start where its end belongs. `D_ENDPOS` equals
//     `D_STARTPOS` for a marker, so the bug is invisible in any session the test author
//     happens to build out of markers — and in a real session it turns "render the
//     chorus" into a render of nothing.
//   - A capped result reported as if it were the whole set. Requirement 23.9's silent
//     truncation, which both output schemas give the fields to avoid.
//
// A session with both kinds in it, so that every filtering assertion has something to
// get wrong.
// ---------------------------------------------------------------------------

namespace
{
	// Markers and regions interleaved, and deliberately not supplied in position order:
	// the output schemas ask for position order, so handing the handler REAPER's
	// enumeration order already sorted would hide a missing sort.
	scripted_session labelled_arrangement()
	{
		return scripted_session{{
			region_spanning(30.0, 90.0, "chorus", 4),
			marker_at(10.0, "intro", 1),
			region_spanning(90.0, 150.0, "bridge", 5),
			marker_at(200.0, "outro", 2),
			marker_at(90.0, "drop", 3),
		}};
	}

	const list_markers_result& markers_from(
		const marker_region_outcome<list_markers_result>& outcome)
	{
		REQUIRE(std::holds_alternative<list_markers_result>(outcome));

		return std::get<list_markers_result>(outcome);
	}

	const list_regions_result& regions_from(
		const marker_region_outcome<list_regions_result>& outcome)
	{
		REQUIRE(std::holds_alternative<list_regions_result>(outcome));

		return std::get<list_regions_result>(outcome);
	}

	std::vector<std::string> names_of(const std::vector<marker_detail>& markers)
	{
		std::vector<std::string> names;

		for (const marker_detail& marker : markers)
		{
			names.push_back(marker.name);
		}

		return names;
	}

	std::vector<std::string> names_of(const std::vector<region_detail>& regions)
	{
		std::vector<std::string> names;

		for (const region_detail& region : regions)
		{
			names.push_back(region.name);
		}

		return names;
	}

	// A window reading the whole timeline, which is what omitting every parameter means.
	timeline_read_window whole_timeline()
	{
		timeline_read_window window;
		window.maximum_results = maximum_listed_markers_or_regions;

		return window;
	}
}

TEST_CASE("list_markers returns the markers and not the regions sharing REAPER's list", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	const list_markers_result result =
		markers_from(apply_list_markers(session, whole_timeline()));

	// Three markers in a session of five objects. A region arriving here would be a
	// named span handed over as a point, with the end that made it a region nowhere in
	// `markerDetail` to report.
	REQUIRE(names_of(result.markers) == std::vector<std::string>{"intro", "drop", "outro"});
	REQUIRE(result.total_in_range == 3);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("list_regions returns the regions and not the markers sharing REAPER's list", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	const list_regions_result result =
		regions_from(apply_list_regions(session, whole_timeline()));

	// A marker arriving here would be a region of zero length, because REAPER reports
	// `D_ENDPOS` equal to `D_STARTPOS` for one. The marker at 90 seconds is in this
	// session precisely so that a `B_ISREGION` test replaced by a position test cannot
	// pass: it sits exactly on the "bridge" region's start.
	REQUIRE(names_of(result.regions) == std::vector<std::string>{"chorus", "bridge"});
	REQUIRE(result.total_in_range == 2);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("list_regions reports each region's own end and never its start", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	const list_regions_result result =
		regions_from(apply_list_regions(session, whole_timeline()));

	REQUIRE(result.regions.size() == 2);

	// The span, not the point. These two assertions are the ones that fail when a
	// region's end is read off the start field — and `set_time_selection` fed from a
	// region whose end equals its start selects nothing, which is what the producer
	// experiences as "render the chorus" rendering silence.
	REQUIRE(result.regions.front().start_seconds == 30.0);
	REQUIRE(result.regions.front().end_seconds == 90.0);
	REQUIRE(result.regions.front().end_seconds > result.regions.front().start_seconds);

	REQUIRE(result.regions.back().start_seconds == 90.0);
	REQUIRE(result.regions.back().end_seconds == 150.0);
}

TEST_CASE("a marker's position is reported and the GUID is what addresses it", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	const list_markers_result result =
		markers_from(apply_list_markers(session, whole_timeline()));

	REQUIRE(result.markers.front().name == "intro");
	REQUIRE(result.markers.front().position_seconds == 10.0);
	REQUIRE(result.markers.front().guid == session.guid_of("intro"));

	// REAPER's displayed number, which is renumbered as markers come and go and is
	// reported rather than used for addressing. Assigned within the kind, so the first
	// marker is marker 1 even though two regions sit before it in the list.
	REQUIRE(result.markers.front().index == 1);
}

TEST_CASE("a colour REAPER was told to ignore is reported as no colour on a listed marker", "[daw][tools][markers][reads]")
{
	std::vector<marker_or_region> objects{marker_at(10.0, "intro", 1), marker_at(20.0, "verse", 2)};
	objects[0].color = color_from_reaper_custom_color(reaper_custom_color_from(0xAABBCC));
	objects[1].color = color_from_reaper_custom_color(0xAABBCC);

	scripted_session session{objects};

	const list_markers_result result =
		markers_from(apply_list_markers(session, whole_timeline()));

	REQUIRE(result.markers.size() == 2);
	REQUIRE(result.markers.front().color == std::optional<int>{0xAABBCC});
	REQUIRE_FALSE(result.markers.back().color.has_value());
}

TEST_CASE("markers come back in position order whatever order REAPER enumerates them in", "[daw][tools][markers][reads]")
{
	// Supplied latest-first. Both output schemas ask for position order, and a producer
	// hearing their arrangement described backwards would not recognise it.
	scripted_session session{{
		marker_at(200.0, "outro", 1),
		marker_at(90.0, "drop", 2),
		marker_at(10.0, "intro", 3),
	}};

	const list_markers_result result =
		markers_from(apply_list_markers(session, whole_timeline()));

	REQUIRE(names_of(result.markers) == std::vector<std::string>{"intro", "drop", "outro"});
}

TEST_CASE("an empty session is a complete answer and not a failure", "[daw][tools][markers][reads]")
{
	scripted_session session;

	const list_markers_result markers =
		markers_from(apply_list_markers(session, whole_timeline()));

	REQUIRE(markers.markers.empty());
	REQUIRE(markers.total_in_range == 0);
	REQUIRE_FALSE(markers.truncated);

	const list_regions_result regions =
		regions_from(apply_list_regions(session, whole_timeline()));

	REQUIRE(regions.regions.empty());
	REQUIRE(regions.total_in_range == 0);
	REQUIRE_FALSE(regions.truncated);
}

TEST_CASE("a marker on either edge of the window is inside it", "[daw][tools][markers][reads]")
{
	// "At or after" and "at or before", which is what both input schemas say.
	scripted_session session{{
		marker_at(9.0, "before", 1),
		marker_at(10.0, "at the start", 2),
		marker_at(50.0, "inside", 3),
		marker_at(100.0, "at the end", 4),
		marker_at(101.0, "after", 5),
	}};

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 10.0;
	window.end_position_seconds = 100.0;

	const list_markers_result result = markers_from(apply_list_markers(session, window));

	REQUIRE(names_of(result.markers)
		== std::vector<std::string>{"at the start", "inside", "at the end"});

	// The ones outside are not in the count either. `totalInRange` is the number that
	// matched the filters, not the number in the session.
	REQUIRE(result.total_in_range == 3);
}

TEST_CASE("a region straddling the window edge is returned in full rather than clipped", "[daw][tools][markers][reads]")
{
	// `list-regions.schema.json`: "A region overlapping the window is returned in full
	// rather than clipped to it, so its end may lie outside the window the call asked
	// for." Clipping it would hand the agent a span the producer cannot see on their
	// ruler.
	scripted_session session{{
		region_spanning(0.0, 20.0, "ends inside", 1),
		region_spanning(80.0, 300.0, "starts inside", 2),
		region_spanning(400.0, 500.0, "wholly after", 3),
	}};

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 10.0;
	window.end_position_seconds = 100.0;

	const list_regions_result result = regions_from(apply_list_regions(session, window));

	REQUIRE(names_of(result.regions) == std::vector<std::string>{"ends inside", "starts inside"});
	REQUIRE(result.total_in_range == 2);

	// Unclipped at both ends.
	REQUIRE(result.regions.front().start_seconds == 0.0);
	REQUIRE(result.regions.back().end_seconds == 300.0);
}

TEST_CASE("an inverted read window is a failed action and nothing is read", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 100.0;
	window.end_position_seconds = 10.0;

	marker_region_outcome<list_markers_result> outcome = apply_list_markers(session, window);

	const auto* const outcomes = std::get_if<handler_action_outcomes>(&outcome);

	REQUIRE(outcomes != nullptr);
	REQUIRE(outcomes->actions.size() == 1);
	REQUIRE_FALSE(action_was_applied(outcomes->actions.front()));

	// `check_end_after_start`'s own code, the same one the three writing contexts
	// report. Not a refusal: `reason` is closed at five values and none of them names
	// an inverted time range.
	const auto* const failure = std::get_if<action_failed>(&outcomes->actions.front());

	REQUIRE(failure != nullptr);
	REQUIRE(failure->error.code() == invalid_time_range_code);
	REQUIRE(failure->target == the_requested_span_target);

	// Before anything was read, so an empty result cannot be mistaken for a session
	// with no markers in it.
	REQUIRE(session.read_call_count == 0);
}

TEST_CASE("a window with only one bound is not compared against a defaulted other", "[daw][tools][markers][reads]")
{
	// Omitting `endPosition` means "to the end of the project". Compared against a
	// defaulted zero it would invert for every call that named a start.
	scripted_session session = labelled_arrangement();

	timeline_read_window from_the_middle = whole_timeline();
	from_the_middle.start_position_seconds = 100.0;

	REQUIRE_FALSE(check_read_window(from_the_middle).has_value());
	REQUIRE(names_of(markers_from(apply_list_markers(session, from_the_middle)).markers)
		== std::vector<std::string>{"outro"});

	timeline_read_window up_to_the_middle = whole_timeline();
	up_to_the_middle.end_position_seconds = 100.0;

	REQUIRE_FALSE(check_read_window(up_to_the_middle).has_value());
	REQUIRE(names_of(markers_from(apply_list_markers(session, up_to_the_middle)).markers)
		== std::vector<std::string>{"intro", "drop"});
}

TEST_CASE("an equal start and end is rejected, as both input schemas say", "[daw][tools][markers][reads]")
{
	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 42.0;
	window.end_position_seconds = 42.0;

	// "Must be greater than startPosition". Strictly, which is `check_end_after_start`.
	REQUIRE(check_read_window(window).has_value());
}

TEST_CASE("a capped read reports the total and says it was capped", "[daw][tools][markers][reads]")
{
	// Requirement 23.9's capping branch, which both these schemas can express and
	// `list_tracks` and `list_track_fx` cannot — so those two fail a result set over
	// their cap and these two cap and report.
	std::vector<marker_or_region> objects;

	for (int number = 1; number <= 25; ++number)
	{
		objects.push_back(marker_at(static_cast<double>(number), "marker", number));
	}

	scripted_session session{objects};

	timeline_read_window window;
	window.maximum_results = 10;

	const list_markers_result result = markers_from(apply_list_markers(session, window));

	REQUIRE(result.markers.size() == 10);

	// The two fields that make the cap visible. Without them the agent reads ten
	// markers as the whole arrangement and tells the producer so.
	REQUIRE(result.total_in_range == 25);
	REQUIRE(result.truncated);

	// In position order from the window's start, which is what makes the continuation
	// the input schema describes work: pass the last position back as `startPosition`.
	REQUIRE(result.markers.front().position_seconds == 1.0);
	REQUIRE(result.markers.back().position_seconds == 10.0);
}

TEST_CASE("a read inside the cap is not reported as capped", "[daw][tools][markers][reads]")
{
	scripted_session session = labelled_arrangement();

	timeline_read_window window;
	window.maximum_results = 10;

	const list_regions_result result = regions_from(apply_list_regions(session, window));

	REQUIRE(result.regions.size() == 2);
	REQUIRE(result.total_in_range == 2);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("the output schema's maxItems bounds a call asking for more than it can carry", "[daw][tools][markers][reads]")
{
	// `maximumResults` is capped at 100 on the input schema too, so this is unreachable
	// from a validated call — and `maxItems` is 100 on the output, so a result longer
	// than that is a payload the extension's own outbound validation refuses.
	REQUIRE(bounded_result_cap(500, maximum_listed_markers_or_regions)
		== maximum_listed_markers_or_regions);
	REQUIRE(bounded_result_cap(10, maximum_listed_markers_or_regions) == 10);

	// A zero is raised to one rather than to the default: the input schema's minimum is
	// 1, and raising it to 20 would read more than was asked for.
	REQUIRE(bounded_result_cap(0, maximum_listed_markers_or_regions) == 1);

	std::vector<marker_or_region> objects;

	for (int number = 1; number <= 150; ++number)
	{
		objects.push_back(marker_at(static_cast<double>(number), "marker", number));
	}

	scripted_session session{objects};

	timeline_read_window window;
	window.maximum_results = 500;

	const list_markers_result result = markers_from(apply_list_markers(session, window));

	REQUIRE(result.markers.size() == maximum_listed_markers_or_regions);
	REQUIRE(result.total_in_range == 150);
	REQUIRE(result.truncated);
}

// ---------------------------------------------------------------------------
// Registration through the Tool Executor's seam
// ---------------------------------------------------------------------------

namespace
{
	// The payload type the framework is templated on. A plain struct, for the reason
	// `tool_executor_test.cpp` gives: the framework never reads a field, so the suite does
	// not need a JSON library to drive it.
	struct test_payload
	{
		std::string described_result;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_context = tool_execution_context<test_payload>;

	struct scripted_requests
	{
		create_marker_request create_marker;
		create_region_request create_region;
		update_marker_or_region_request update;
		delete_marker_or_region_request remove;
		change_tempo_map_request tempo;
		set_time_selection_request time_selection;
	};

	// A codec that reads its request from a scripted value rather than from JSON. Reading
	// a tool input is the Envelope Codec's job; what is under test here is that the six
	// tools reach the registry and come back out as results.
	marker_region_payload_codec<test_payload> codec_for(scripted_requests requests)
	{
		marker_region_payload_codec<test_payload> codec;

		codec.read_create_marker_request =
			[requests](const test_context&) { return requests.create_marker; };
		codec.read_create_region_request =
			[requests](const test_context&) { return requests.create_region; };
		codec.read_update_marker_or_region_request =
			[requests](const test_context&) { return requests.update; };
		codec.read_delete_marker_or_region_request =
			[requests](const test_context&) { return requests.remove; };
		codec.read_change_tempo_map_request =
			[requests](const test_context&) { return requests.tempo; };
		codec.read_set_time_selection_request =
			[requests](const test_context&) { return requests.time_selection; };

		codec.write_create_marker_result = [](const create_marker_result& result) {
			return test_payload{"created marker " + result.marker.guid};
		};
		codec.write_create_region_result = [](const create_region_result& result) {
			return test_payload{"created region " + result.region.guid};
		};
		codec.write_update_marker_or_region_result = [](const update_marker_or_region_result& result) {
			return test_payload{
				"updated " + std::string{describe_marker_or_region_kind(result.updated)}};
		};
		codec.write_delete_marker_or_region_result = [](const delete_marker_or_region_result& result) {
			return test_payload{
				"deleted " + std::string{describe_marker_or_region_kind(result.deleted)}};
		};
		codec.write_change_tempo_map_result = [](const change_tempo_map_result& result) {
			return test_payload{"wrote " + std::to_string(result.points_written) + " tempo points"};
		};
		codec.write_set_time_selection_result = [](const set_time_selection_result& result) {
			return test_payload{"selected " + std::to_string(result.length_seconds) + " seconds"};
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

TEST_CASE("all six tools register as mutating tools with no precondition", "[daw][tools][markers][registration]")
{
	test_registry registry;
	scripted_session session;

	const auto report = register_marker_region_tools(registry, session, codec_for({}));

	REQUIRE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 6);

	// Registered through the mutating seam, so a block is opened for each and the turn's
	// marker is captured before the first. None of the six is a read tool, and none has a
	// precondition: the refusal schema's five reasons describe nothing any of them
	// decides.
	for (const std::string_view& tool_name : {
			create_marker_tool_name,
			create_region_tool_name,
			update_marker_or_region_tool_name,
			delete_marker_or_region_tool_name,
			change_tempo_map_tool_name,
			set_time_selection_tool_name,
		})
	{
		INFO("tool " << tool_name);
		const auto* const tool = registry.find_tool(tool_name);

		REQUIRE(tool != nullptr);
		REQUIRE(tool->undo_effect() == tool_undo_effect::undo_block);
	}
}

TEST_CASE("an incomplete codec registers nothing", "[daw][tools][markers][registration]")
{
	// An empty `std::function` would throw `std::bad_function_call` from inside the undo
	// block, which is the one place a throw costs the producer their undo point.
	test_registry registry;
	scripted_session session;

	marker_region_payload_codec<test_payload> codec = codec_for({});
	codec.write_change_tempo_map_result = {};

	const auto report = register_marker_region_tools(registry, session, codec);

	REQUIRE_FALSE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 0);
}

TEST_CASE("a second registration of the same tool is rejected", "[daw][tools][markers][registration]")
{
	test_registry registry;
	scripted_session session;

	REQUIRE(register_marker_region_tools(registry, session, codec_for({})).every_tool_registered());

	const auto second = register_marker_region_tools(registry, session, codec_for({}));

	REQUIRE(second.create_marker == tool_registration_outcome::already_registered);
	REQUIRE(second.set_time_selection == tool_registration_outcome::already_registered);
	REQUIRE(registry.registered_tool_count() == 6);
}

TEST_CASE("a dispatched set_time_selection produces a success carrying one undo block", "[daw][tools][markers][registration]")
{
	test_registry registry;
	scripted_session session;

	scripted_requests requests;
	requests.time_selection.start_seconds = 10.0;
	requests.time_selection.end_seconds = 40.0;

	REQUIRE(register_marker_region_tools(registry, session, codec_for(requests))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{set_time_selection_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	REQUIRE(success->tool_name == set_time_selection_tool_name);
	REQUIRE(success->undo.has_value());
	REQUIRE(success->undo->undo_description == "Sesh AI: set_time_selection");

	// Exactly one block, opened and closed.
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);
	REQUIRE(stack.open_block_depth == 0);

	REQUIRE(session.time_selection == std::make_pair(10.0, 40.0));
}

TEST_CASE("a dispatched inverted time selection comes back as a partial and not a refusal", "[daw][tools][markers][registration]")
{
	// Requirement 9.6 through the whole framework rather than against the handler alone:
	// a failed action, an undo report withheld because nothing landed, and no refusal.
	test_registry registry;
	scripted_session session;

	scripted_requests requests;
	requests.time_selection.start_seconds = 40.0;
	requests.time_selection.end_seconds = 10.0;

	REQUIRE(register_marker_region_tools(registry, session, codec_for(requests))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{set_time_selection_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE(std::get_if<tool_refusal>(result) == nullptr);

	const auto* const partial = std::get_if<tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);
	REQUIRE(partial->actions.size() == 1);
	REQUIRE(partial->failed_action_count() == 1);

	// Nothing landed, so there is no position for "revert all" to walk back to and none
	// is offered.
	REQUIRE_FALSE(partial->undo.has_value());
	REQUIRE_FALSE(session.time_selection.has_value());
}

TEST_CASE("a dispatched change_tempo_map that partly fails reports per action and records the block", "[daw][tools][markers][registration]")
{
	test_registry registry;
	scripted_session session;
	session.refuse_tempo_point_at = 64.0;

	scripted_requests requests;
	requests.tempo.points = {tempo_point(32.0, 130.0), tempo_point(64.0, 140.0)};

	REQUIRE(register_marker_region_tools(registry, session, codec_for(requests))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{change_tempo_map_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const partial = std::get_if<tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);
	REQUIRE(partial->applied_action_count() == 1);
	REQUIRE(partial->failed_action_count() == 1);

	// Something landed, so the undo report is present — and it is one block for the whole
	// array.
	REQUIRE(partial->undo.has_value());
	REQUIRE(stack.begin_block_call_count == 1);
	REQUIRE(stack.end_block_call_count == 1);

	// The point that landed stays landed.
	REQUIRE(session.tempo_map.size() == 1);
}

TEST_CASE("a dispatched tempo report over the cap names how many outcomes are missing", "[daw][tools][markers][registration]")
{
	// The backstop half of `tool_executor.h`'s outcome-cap rule, through the whole
	// framework. `change_tempo_map` accepts 1024 points and reports success by aggregate
	// count, so it does not refuse a long call up front — the cap only binds once a point
	// has failed and the result falls back to per-action outcomes, by which time the
	// writing has happened and there is nothing left to refuse.
	//
	// What must not happen is the array quietly ending at 512, which is the silent
	// truncation requirements 9.5 and 23.9 both forbid: an agent reading a full array of
	// 512 clean outcomes would conclude 512 is all there was.
	test_registry registry;
	scripted_session session;
	session.refuse_tempo_point_at = 5.0;

	scripted_requests requests;

	for (std::size_t index = 0; index < 600; ++index)
	{
		requests.tempo.points.push_back(tempo_point(static_cast<double>(index) + 1.0, 120.0));
	}

	REQUIRE(register_marker_region_tools(registry, session, codec_for(requests))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{change_tempo_map_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const partial = std::get_if<tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);

	// Bounded to what `tool-result-partial.schema.json` accepts, with the last slot spent
	// saying what is not there rather than on a 512th outcome.
	REQUIRE(partial->actions.size() == maximum_action_outcomes);

	const auto& last = partial->actions.back();
	const auto* const gap = std::get_if<action_failed>(&last);

	REQUIRE(gap != nullptr);
	REQUIRE(gap->error.code() == std::string{outcome_report_truncated_code});
	REQUIRE(gap->target == std::string{outcome_report_target});

	// It names the count, because "some outcomes are missing" does not tell the agent how
	// far short of 600 the report falls.
	REQUIRE(gap->error.message().find("89") != std::string::npos);

	// The order the schema promises is preserved: the outcomes that are reported are the
	// first 511 the handler produced, not a reshuffle that put the failures first.
	REQUIRE(count_failed_actions(partial->actions) == 2);
	REQUIRE(count_applied_actions(partial->actions) == maximum_action_outcomes - 2);
	REQUIRE_FALSE(action_was_applied(partial->actions[4]));

	// Points landed, so the undo report stands.
	REQUIRE(partial->undo.has_value());
}

// ---------------------------------------------------------------------------
// The two read tools register through the other seam
// ---------------------------------------------------------------------------

namespace
{
	marker_region_read_payload_codec<test_payload> read_codec_for(timeline_read_window window)
	{
		marker_region_read_payload_codec<test_payload> codec;

		codec.read_list_markers_request = [window](const test_context&) { return window; };
		codec.read_list_regions_request = [window](const test_context&) { return window; };

		codec.write_list_markers_result = [](const list_markers_result& result) {
			return test_payload{"listed " + std::to_string(result.markers.size()) + " markers"};
		};
		codec.write_list_regions_result = [](const list_regions_result& result) {
			return test_payload{"listed " + std::to_string(result.regions.size()) + " regions"};
		};

		return codec;
	}
}

TEST_CASE("the two read tools register with no undo effect", "[daw][tools][markers][reads][registration]")
{
	test_registry registry;
	scripted_session session;

	const auto report =
		register_marker_region_read_tools(registry, session, read_codec_for(whole_timeline()));

	REQUIRE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 2);

	// `register_read_tool`, so no block is opened and no marker is captured. A read tool
	// that registered as mutating would record an undo position for a call that changed
	// nothing, which sits above the agent's earlier work and shortens what "revert all"
	// walks back (requirements 9.3 and 10.7).
	for (const std::string_view& tool_name : {list_markers_tool_name, list_regions_tool_name})
	{
		INFO("tool " << tool_name);
		const auto* const tool = registry.find_tool(tool_name);

		REQUIRE(tool != nullptr);
		REQUIRE(tool->undo_effect() == tool_undo_effect::none);
	}
}

TEST_CASE("the six mutating tools and the two reads register alongside each other", "[daw][tools][markers][reads][registration]")
{
	test_registry registry;
	scripted_session session;

	REQUIRE(register_marker_region_tools(registry, session, codec_for({})).every_tool_registered());
	REQUIRE(register_marker_region_read_tools(registry, session, read_codec_for(whole_timeline()))
		.every_tool_registered());

	REQUIRE(registry.registered_tool_count() == 8);
}

TEST_CASE("an incomplete read codec registers nothing", "[daw][tools][markers][reads][registration]")
{
	test_registry registry;
	scripted_session session;

	marker_region_read_payload_codec<test_payload> codec = read_codec_for(whole_timeline());
	codec.write_list_regions_result = {};

	const auto report = register_marker_region_read_tools(registry, session, codec);

	REQUIRE_FALSE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 0);
}

TEST_CASE("a dispatched list_markers comes back as a success with no undo report", "[daw][tools][markers][reads][registration]")
{
	test_registry registry;
	scripted_session session = labelled_arrangement();

	REQUIRE(register_marker_region_read_tools(registry, session, read_codec_for(whole_timeline()))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{list_markers_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	REQUIRE(success->tool_name == list_markers_tool_name);
	REQUIRE(success->fields.described_result == "listed 3 markers");

	// Nothing was opened, nothing was closed, and no undo position is offered for a
	// call that changed nothing.
	REQUIRE_FALSE(success->undo.has_value());
	REQUIRE(stack.begin_block_call_count == 0);
	REQUIRE(stack.end_block_call_count == 0);
}

TEST_CASE("a dispatched read with an inverted window is a partial and not a refusal", "[daw][tools][markers][reads][registration]")
{
	// The framework fix this depends on: `result_schema_path_for` routes a
	// `tool_partial_outcome` to `messages/tool-result-partial.schema.json` rather than
	// to the tool's own output schema. `list-regions.schema.json` requires `regions`,
	// `totalInRange`, and `truncated` and is closed, so a framework partial validated
	// against it would be refused on all three — and the producer would experience a
	// read tool that silently does not work.
	test_registry registry;
	scripted_session session = labelled_arrangement();

	timeline_read_window inverted = whole_timeline();
	inverted.start_position_seconds = 100.0;
	inverted.end_position_seconds = 10.0;

	REQUIRE(register_marker_region_read_tools(registry, session, read_codec_for(inverted))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{list_regions_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE_FALSE(std::holds_alternative<tool_refusal>(*result));

	const auto* const partial = std::get_if<tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);
	REQUIRE(partial->failed_action_count() == 1);

	// The contract that describes the shape the framework built, not the one that
	// describes what `list_regions` returns when it works.
	REQUIRE(sesh_ai::daw::result_schema_path_for(outcome)
		== sesh_ai::transport::tool_result_partial_contract_schema_path);

	// No block for a read tool, whatever it returns.
	REQUIRE_FALSE(partial->undo.has_value());
	REQUIRE(stack.begin_block_call_count == 0);
}
