// The project context snapshot: what goes in it, what is kept out of it, and what
// happens to a value the schema cannot represent.
//
// The order below is roughly order of consequence.
//
// **What is kept out.** Requirement 6.2 is the point of this component, and the way
// it fails is not dramatic: somebody adds a field because it is useful and the schema
// allows it, the payload grows, and one day the AI system truncates it before the
// model sees it and accuracy degrades with nothing in any log. So the omission is
// checked against the serialised document rather than against the snapshot struct —
// a struct that happens not to carry a field proves nothing about a serialiser — and
// it is checked as an exact key set in both directions: every allowed key present,
// and not one key from `track_properties_absent_from_the_snapshot`.
//
// **What is in it.** Requirement 6.1's list, field by field, including the three
// counts that stand in for the marker, region, and tempo-map detail.
//
// **The schema's bounds.** Every number the builder emits has to be finite and in
// range, because the codec refuses a payload that is not and the cost of a refusal is
// the whole turn's session context, not one field. So the boundary cases are swept —
// NaN, both infinities, past each end of each range — rather than sampled, and the
// non-representable denominator and the over-long name are in there too.
//
// **A GUID it cannot use.** `guid` is required on a track, so a track whose GUID does
// not match the schema's pattern cannot be represented. Dropping that track and
// sending the rest is the decision under test, along with the two things that make it
// safe: the surviving tracks keep REAPER's own track numbers, and the drop is counted
// rather than silent.
//
// **The three derivations.** Folder parentage accumulated across track order,
// structural role per track with the signals reported alongside it, and unit
// conversion — which is checked for existing and for *not* reaching the snapshot,
// since requirement 6.2 excludes volume and pan.
//
// No JSON library and no REAPER here. The document type is substituted, which is what
// the serialiser's template parameter is for; the project is a value a case writes by
// hand. The limit of that is worth stating: these cases settle the shape and the
// bounds of what the builder produces, not that a validator accepts it. That needs
// the real validator against the vendored schema and is Property 12's, in task 4.3.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/project_context_builder.h>
#include <context/structural_role.h>
#include <daw/unit_conversion.h>

#include "project_context_test_doubles.h"

using sesh_ai::context::PlayState;
using sesh_ai::context::ProjectContextReading;
using sesh_ai::context::ProjectContextSnapshot;
using sesh_ai::context::StructuralRole;
using sesh_ai::context::TrackReading;
using sesh_ai::context::all_play_states;
using sesh_ai::context::all_structural_roles;
using sesh_ai::context::bounded_tempo;
using sesh_ai::context::bounded_timeline_seconds;
using sesh_ai::context::build_project_context_snapshot;
using sesh_ai::context::converted_track_levels;
using sesh_ai::context::describe_project_context_snapshot;
using sesh_ai::context::describe_snapshot_role_derivations;
using sesh_ai::context::is_schema_shaped_guid;
using sesh_ai::context::master_track_properties_absent_from_the_snapshot;
using sesh_ai::context::maximum_name_length;
using sesh_ai::context::maximum_sample_rate;
using sesh_ai::context::maximum_selected_track_guids;
using sesh_ai::context::maximum_tempo;
using sesh_ai::context::maximum_time_signature_numerator;
using sesh_ai::context::maximum_timeline_seconds;
using sesh_ai::context::minimum_sample_rate;
using sesh_ai::context::minimum_tempo;
using sesh_ai::context::minimum_time_signature_numerator;
using sesh_ai::context::minimum_timeline_seconds;
using sesh_ai::context::nearest_representable_time_signature_denominator;
using sesh_ai::context::play_state_from_reaper_play_state;
using sesh_ai::context::representable_time_signature_denominators;
using sesh_ai::context::serialize_project_context;
using sesh_ai::context::snapshot_master_track_property;
using sesh_ai::context::snapshot_master_track_property_names;
using sesh_ai::context::snapshot_properties_replaced_by_counts;
using sesh_ai::context::snapshot_property_names;
using sesh_ai::context::snapshot_time_selection_end_property;
using sesh_ai::context::snapshot_time_selection_start_property;
using sesh_ai::context::snapshot_track_property_names;
using sesh_ai::context::to_schema_string;
using sesh_ai::context::track_properties_absent_from_the_snapshot;

using sesh_ai_tests::StubDocument;
using sesh_ai_tests::plain_globals;
using sesh_ai_tests::plain_reading;
using sesh_ai_tests::plain_track;
using sesh_ai_tests::test_guid;

namespace
{
	StubDocument serialize(const ProjectContextSnapshot& snapshot)
	{
		return serialize_project_context<StubDocument>(snapshot);
	}

	StubDocument serialize_reading(const ProjectContextReading& reading)
	{
		return serialize(build_project_context_snapshot(reading));
	}

	std::vector<std::string> sorted_property_names(const StubDocument& document)
	{
		std::vector<std::string> names = document.property_names();
		std::sort(names.begin(), names.end());

		return names;
	}

	std::vector<std::string> sorted_expected_names(const std::vector<std::string_view>& names)
	{
		std::vector<std::string> expected;
		expected.reserve(names.size());

		for (const std::string_view& name : names)
		{
			expected.emplace_back(name);
		}

		std::sort(expected.begin(), expected.end());

		return expected;
	}

	template <std::size_t Count>
	std::vector<std::string> sorted_expected_names(const std::array<std::string_view, Count>& names)
	{
		return sorted_expected_names(std::vector<std::string_view>{names.begin(), names.end()});
	}

	// A track with a folder delta, so the folder cases read as structure rather than as
	// a list of field assignments.
	TrackReading folder_track(int ordinal, std::string name, int folder_depth_delta)
	{
		TrackReading track = plain_track(test_guid(ordinal), std::move(name));
		track.folder_depth_delta = folder_depth_delta;

		return track;
	}
}

TEST_CASE("the snapshot carries everything requirement 6.1 lists", "[context][snapshot]")
{
	ProjectContextReading reading = plain_reading();

	reading.globals.project_name = "Midnight Sessions";
	reading.globals.tempo = 92.5;
	reading.globals.time_signature_numerator = 7;
	reading.globals.time_signature_denominator = 8;
	reading.globals.sample_rate = 96000;
	reading.globals.project_length = 240.75;
	reading.globals.cursor_position = 64.5;
	reading.globals.reaper_play_state = 1;
	reading.globals.loop_start = 32.0;
	reading.globals.loop_end = 48.0;
	reading.globals.loop_enabled = true;
	reading.globals.time_selection_present = true;
	reading.globals.time_selection_start = 8.0;
	reading.globals.time_selection_end = 24.0;
	reading.globals.tempo_change_count = 3;
	reading.globals.marker_count = 11;
	reading.globals.region_count = 4;

	reading.tracks_in_project_order.front().selected = true;

	const StubDocument document = serialize_reading(reading);

	SECTION("project globals")
	{
		REQUIRE(document.at("projectName").string_value() == "Midnight Sessions");
		REQUIRE(document.at("tempo").number_value() == 92.5);
		REQUIRE(document.at("timeSignature").at("numerator").integer_value() == 7);
		REQUIRE(document.at("timeSignature").at("denominator").integer_value() == 8);
		REQUIRE(document.at("sampleRate").integer_value() == 96000);
		REQUIRE(document.at("projectLength").number_value() == 240.75);
		REQUIRE(document.at("cursorPosition").number_value() == 64.5);
		REQUIRE(document.at("playState").string_value() == "playing");
		REQUIRE(document.at("loopStart").number_value() == 32.0);
		REQUIRE(document.at("loopEnd").number_value() == 48.0);
		REQUIRE(document.at("loopEnabled").boolean_value() == true);
		REQUIRE(document.at("timeSelectionStart").number_value() == 8.0);
		REQUIRE(document.at("timeSelectionEnd").number_value() == 24.0);
		REQUIRE(document.at("tempoChangeCount").integer_value() == 3);
	}

	SECTION("the counts that stand in for the detail")
	{
		REQUIRE(document.at("markerCount").integer_value() == 11);
		REQUIRE(document.at("regionCount").integer_value() == 4);
		REQUIRE(document.at("tempoChangeCount").integer_value() == 3);
	}

	SECTION("per-track guid, index, name, and role")
	{
		REQUIRE(document.at("tracks").elements().size() == 1);

		const StubDocument& track = document.at("tracks").elements().front();

		REQUIRE(track.at("guid").string_value() == test_guid(1));
		REQUIRE(track.at("index").integer_value() == 0);
		REQUIRE(track.at("name").string_value() == "Kick");
		REQUIRE(track.at("role").string_value() == "normal");
	}

	SECTION("selectedTrackGuids")
	{
		REQUIRE(document.at("selectedTrackGuids").elements().size() == 1);
		REQUIRE(document.at("selectedTrackGuids").elements().front().string_value() == test_guid(1));
	}

	SECTION("the master track's guid, name, and role")
	{
		const StubDocument& master_track = document.at(std::string{snapshot_master_track_property});

		REQUIRE(master_track.at("guid").string_value() == test_guid(0));
		REQUIRE(master_track.at("name").string_value() == "MASTER");
		REQUIRE(master_track.at("role").string_value() == "master");
	}
}

TEST_CASE("the snapshot omits every field requirement 6.2 excludes", "[context][snapshot]")
{
	// A project with something to say about every excluded field, so nothing is absent
	// merely because it was never set: the track has items, FX, a receive, a folder
	// delta, and a parent send flag, and all five are role signals the derivation uses.
	ProjectContextReading reading = plain_reading();

	TrackReading& track = reading.tracks_in_project_order.front();
	track.item_count = 12;
	track.has_fx = true;
	track.receive_count = 3;
	track.folder_depth_delta = 1;
	track.parent_send_enabled = false;
	track.selected = true;

	reading.tracks_in_project_order.push_back(folder_track(2, "Snare", -1));
	reading.master_track.has_fx = true;
	reading.master_track.receive_count = 2;

	const StubDocument document = serialize_reading(reading);

	SECTION("a track carries exactly guid, index, name, and role")
	{
		for (const StubDocument& track_document : document.at("tracks").elements())
		{
			REQUIRE(
				sorted_property_names(track_document)
					== sorted_expected_names({
						snapshot_track_property_names.begin(),
						snapshot_track_property_names.end()
					})
			);
		}
	}

	SECTION("and none of the fourteen excluded per-track properties")
	{
		for (const StubDocument& track_document : document.at("tracks").elements())
		{
			for (const std::string_view& excluded : track_properties_absent_from_the_snapshot)
			{
				// Named in the assertion, so a failure says which field crept in rather
				// than only that the key set differed.
				INFO("excluded per-track property: " << excluded);
				REQUIRE_FALSE(track_document.contains(std::string{excluded}));
			}
		}
	}

	SECTION("the role signals are reported but not serialised")
	{
		const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

		// Requirement 6.6: the evidence exists and is reachable.
		REQUIRE(snapshot.tracks.front().role_signals.item_count == 12);
		REQUIRE(snapshot.tracks.front().role_signals.has_fx);
		REQUIRE(snapshot.tracks.front().role_signals.receive_count == 3);
		REQUIRE_FALSE(snapshot.tracks.front().role_signals.parent_send_enabled);

		// Requirement 6.2: and it travels in a log line, not in the payload. `itemCount`
		// and `parentSendEnabled` are the same signals under the schema's names.
		const StubDocument& track_document = document.at("tracks").elements().front();

		REQUIRE_FALSE(track_document.contains("itemCount"));
		REQUIRE_FALSE(track_document.contains("parentSendEnabled"));
		REQUIRE_FALSE(track_document.contains("folderDepth"));

		const std::vector<std::string> role_lines = describe_snapshot_role_derivations(snapshot);

		REQUIRE(role_lines.size() == snapshot.tracks.size() + 1);
		REQUIRE(role_lines.front().find("itemCount=12") != std::string::npos);
		REQUIRE(role_lines.front().find("receiveCount=3") != std::string::npos);
	}

	SECTION("the master track carries exactly guid, name, and role")
	{
		const StubDocument& master_track = document.at(std::string{snapshot_master_track_property});

		REQUIRE(
			sorted_property_names(master_track)
				== sorted_expected_names({
					snapshot_master_track_property_names.begin(),
					snapshot_master_track_property_names.end()
				})
		);

		for (const std::string_view& excluded : master_track_properties_absent_from_the_snapshot)
		{
			INFO("excluded master track property: " << excluded);
			REQUIRE_FALSE(master_track.contains(std::string{excluded}));
		}
	}

	SECTION("the marker and region arrays are replaced by counts, not carried")
	{
		for (const std::string_view& replaced : snapshot_properties_replaced_by_counts)
		{
			INFO("property replaced by a count: " << replaced);
			REQUIRE_FALSE(document.contains(std::string{replaced}));
		}

		REQUIRE(document.contains("markerCount"));
		REQUIRE(document.contains("regionCount"));
	}

	SECTION("the top-level key set is the fifteen always-present properties plus masterTrack")
	{
		std::vector<std::string_view> expected{
			snapshot_property_names.begin(),
			snapshot_property_names.end()
		};

		expected.push_back(snapshot_master_track_property);

		REQUIRE(sorted_property_names(document) == sorted_expected_names(expected));
	}
}

TEST_CASE("unit conversion is the builder's, and is not in the snapshot", "[context][snapshot]")
{
	// Requirement 6.7 asks the builder to convert; requirement 6.2 keeps the result out
	// of the snapshot. Both, rather than either.
	SECTION("the conversion delegates to the one implementation")
	{
		const auto levels = converted_track_levels(1.0, 0.0);

		REQUIRE(levels.volume_decibels == sesh_ai::daw::unity_volume_decibels);
		REQUIRE(levels.pan_percent == sesh_ai::daw::centre_pan_percent);

		const auto hard_left_and_silent = converted_track_levels(0.0, -1.0);

		REQUIRE(hard_left_and_silent.volume_decibels == sesh_ai::daw::minimum_volume_decibels);
		REQUIRE(hard_left_and_silent.pan_percent == sesh_ai::daw::minimum_pan_percent);
	}

	SECTION("and no converted level appears in the payload")
	{
		const StubDocument document = serialize_reading(plain_reading());
		const StubDocument& track = document.at("tracks").elements().front();

		REQUIRE_FALSE(track.contains("volume"));
		REQUIRE_FALSE(track.contains("pan"));
	}
}

TEST_CASE("every number the snapshot emits is inside the schema's range", "[context][snapshot]")
{
	constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
	constexpr double positive_infinity = std::numeric_limits<double>::infinity();
	constexpr double negative_infinity = -std::numeric_limits<double>::infinity();

	SECTION("tempo clamps to both bounds")
	{
		REQUIRE(bounded_tempo(minimum_tempo) == minimum_tempo);
		REQUIRE(bounded_tempo(maximum_tempo) == maximum_tempo);
		REQUIRE(bounded_tempo(0.0) == minimum_tempo);
		REQUIRE(bounded_tempo(-40.0) == minimum_tempo);
		REQUIRE(bounded_tempo(100000.0) == maximum_tempo);
		REQUIRE(bounded_tempo(positive_infinity) == maximum_tempo);
		REQUIRE(bounded_tempo(negative_infinity) == minimum_tempo);
		REQUIRE(bounded_tempo(120.0) == 120.0);
	}

	SECTION("a NaN tempo reads as the schema minimum rather than a plausible 120")
	{
		// The choice is deliberate and the reason is in the header: a read that failed
		// should produce a number somebody notices.
		REQUIRE(bounded_tempo(not_a_number) == minimum_tempo);
	}

	SECTION("positions and lengths are finite and non-negative")
	{
		REQUIRE(bounded_timeline_seconds(-1.0) == minimum_timeline_seconds);
		REQUIRE(bounded_timeline_seconds(negative_infinity) == minimum_timeline_seconds);
		REQUIRE(bounded_timeline_seconds(not_a_number) == minimum_timeline_seconds);
		REQUIRE(bounded_timeline_seconds(positive_infinity) == maximum_timeline_seconds);
		REQUIRE(bounded_timeline_seconds(12.5) == 12.5);
		REQUIRE(std::isfinite(bounded_timeline_seconds(positive_infinity)));
	}

	SECTION("a reading full of non-finite values still produces a sendable snapshot")
	{
		ProjectContextReading reading = plain_reading();

		reading.globals.tempo = not_a_number;
		reading.globals.project_length = positive_infinity;
		reading.globals.cursor_position = not_a_number;
		reading.globals.loop_start = negative_infinity;
		reading.globals.loop_end = positive_infinity;
		reading.globals.time_selection_present = true;
		reading.globals.time_selection_start = not_a_number;
		reading.globals.time_selection_end = positive_infinity;

		const StubDocument document = serialize_reading(reading);

		for (const std::string& property_name : document.property_names())
		{
			const StubDocument& value = document.at(property_name);

			if (!value.is_number())
			{
				continue;
			}

			INFO("property: " << property_name);
			REQUIRE(std::isfinite(value.number_value()));
			REQUIRE(value.number_value() >= minimum_timeline_seconds);
		}

		REQUIRE(document.at("tempo").number_value() == minimum_tempo);
		REQUIRE(document.at("projectLength").number_value() == maximum_timeline_seconds);
	}

	SECTION("the sample rate clamps to both bounds")
	{
		ProjectContextReading reading = plain_reading();

		reading.globals.sample_rate = 0;
		REQUIRE(serialize_reading(reading).at("sampleRate").integer_value() == minimum_sample_rate);

		reading.globals.sample_rate = -44100;
		REQUIRE(serialize_reading(reading).at("sampleRate").integer_value() == minimum_sample_rate);

		reading.globals.sample_rate = 9999999;
		REQUIRE(serialize_reading(reading).at("sampleRate").integer_value() == maximum_sample_rate);

		reading.globals.sample_rate = 44100;
		REQUIRE(serialize_reading(reading).at("sampleRate").integer_value() == 44100);
	}

	SECTION("the time signature numerator clamps and the denominator snaps to a note value")
	{
		ProjectContextReading reading = plain_reading();

		reading.globals.time_signature_numerator = 0;
		reading.globals.time_signature_denominator = 4;

		REQUIRE(
			serialize_reading(reading).at("timeSignature").at("numerator").integer_value()
				== minimum_time_signature_numerator
		);

		reading.globals.time_signature_numerator = 4096;

		REQUIRE(
			serialize_reading(reading).at("timeSignature").at("numerator").integer_value()
				== maximum_time_signature_numerator
		);

		// Every representable denominator survives unchanged, which is the half that
		// matters: a snapping rule that moved a legitimate 8 would be worse than one
		// that mishandled a 5.
		for (const int denominator : representable_time_signature_denominators)
		{
			INFO("denominator: " << denominator);
			REQUIRE(nearest_representable_time_signature_denominator(denominator) == denominator);
		}

		// And anything else lands on a value the schema's enum holds.
		for (int denominator = -8; denominator <= 128; ++denominator)
		{
			const int snapped = nearest_representable_time_signature_denominator(denominator);

			INFO("denominator: " << denominator << " snapped to " << snapped);
			REQUIRE(
				std::find(
					representable_time_signature_denominators.begin(),
					representable_time_signature_denominators.end(),
					snapped
				) != representable_time_signature_denominators.end()
			);
		}

		REQUIRE(nearest_representable_time_signature_denominator(5) == 4);
		REQUIRE(nearest_representable_time_signature_denominator(0) == 1);
		REQUIRE(nearest_representable_time_signature_denominator(-3) == 1);
		REQUIRE(nearest_representable_time_signature_denominator(1000) == 64);
	}

	SECTION("the counts are non-negative")
	{
		ProjectContextReading reading = plain_reading();

		reading.globals.marker_count = -1;
		reading.globals.region_count = -7;
		reading.globals.tempo_change_count = -3;

		const StubDocument document = serialize_reading(reading);

		REQUIRE(document.at("markerCount").integer_value() == 0);
		REQUIRE(document.at("regionCount").integer_value() == 0);
		REQUIRE(document.at("tempoChangeCount").integer_value() == 0);
	}

	SECTION("names are truncated to the schema's maxLength")
	{
		ProjectContextReading reading = plain_reading();

		reading.globals.project_name = std::string(maximum_name_length + 64, 'p');
		reading.tracks_in_project_order.front().name = std::string(maximum_name_length + 1, 't');
		reading.master_track.name = std::string(maximum_name_length * 2, 'm');

		const StubDocument document = serialize_reading(reading);

		REQUIRE(document.at("projectName").string_value().size() == maximum_name_length);
		REQUIRE(
			document.at("tracks").elements().front().at("name").string_value().size()
				== maximum_name_length
		);
		REQUIRE(
			document.at(std::string{snapshot_master_track_property}).at("name").string_value().size()
				== maximum_name_length
		);
	}

	SECTION("a name already inside the bound is untouched")
	{
		ProjectContextReading reading = plain_reading();
		reading.tracks_in_project_order.front().name = "Drum Bus";

		REQUIRE(
			serialize_reading(reading).at("tracks").elements().front().at("name").string_value()
				== "Drum Bus"
		);
	}

	SECTION("the fields the schema declares integer are emitted as integers")
	{
		// A double where an integer is wanted passes every value assertion and fails
		// validation, so the document remembers which it was told.
		const StubDocument document = serialize_reading(plain_reading());

		REQUIRE(document.at("sampleRate").is_integer());
		REQUIRE(document.at("markerCount").is_integer());
		REQUIRE(document.at("regionCount").is_integer());
		REQUIRE(document.at("tempoChangeCount").is_integer());
		REQUIRE(document.at("timeSignature").at("numerator").is_integer());
		REQUIRE(document.at("timeSignature").at("denominator").is_integer());
		REQUIRE(document.at("tracks").elements().front().at("index").is_integer());

		REQUIRE(document.at("tempo").is_number());
		REQUIRE(document.at("projectLength").is_number());
		REQUIRE(document.at("cursorPosition").is_number());
		REQUIRE(document.at("loopStart").is_number());
		REQUIRE(document.at("loopEnd").is_number());
		REQUIRE(document.at("loopEnabled").is_boolean());
		REQUIRE(document.at("playState").is_string());
	}
}

TEST_CASE("the time selection is sent as a pair or not at all", "[context][snapshot]")
{
	ProjectContextReading reading = plain_reading();

	SECTION("absent when nothing is selected")
	{
		reading.globals.time_selection_present = false;
		reading.globals.time_selection_start = 4.0;
		reading.globals.time_selection_end = 8.0;

		const StubDocument document = serialize_reading(reading);

		REQUIRE_FALSE(document.contains(std::string{snapshot_time_selection_start_property}));
		REQUIRE_FALSE(document.contains(std::string{snapshot_time_selection_end_property}));
	}

	SECTION("both present when there is one")
	{
		reading.globals.time_selection_present = true;
		reading.globals.time_selection_start = 4.0;
		reading.globals.time_selection_end = 8.0;

		const StubDocument document = serialize_reading(reading);

		REQUIRE(document.at(std::string{snapshot_time_selection_start_property}).number_value() == 4.0);
		REQUIRE(document.at(std::string{snapshot_time_selection_end_property}).number_value() == 8.0);
	}
}

TEST_CASE("REAPER's play state bitfield maps to the schema's enum", "[context][snapshot]")
{
	SECTION("the four states the schema names")
	{
		REQUIRE(play_state_from_reaper_play_state(0) == PlayState::stopped);
		REQUIRE(play_state_from_reaper_play_state(1) == PlayState::playing);
		REQUIRE(play_state_from_reaper_play_state(2) == PlayState::paused);
		REQUIRE(play_state_from_reaper_play_state(4) == PlayState::recording);
	}

	SECTION("recording wins over paused, and paused over playing")
	{
		// REAPER sets more than one bit routinely: recording is reported as playing and
		// recording together, and pausing mid-take leaves the recording bit set. The
		// asymmetry is deliberate — see the header. An agent told "paused" during a take
		// might edit a track and cost the producer the take; one told "recording" while
		// the transport is paused only waits.
		REQUIRE(play_state_from_reaper_play_state(1 | 4) == PlayState::recording);
		REQUIRE(play_state_from_reaper_play_state(2 | 4) == PlayState::recording);
		REQUIRE(play_state_from_reaper_play_state(1 | 2 | 4) == PlayState::recording);
		REQUIRE(play_state_from_reaper_play_state(1 | 2) == PlayState::paused);
	}

	SECTION("every bit combination, plus unknown high bits, yields one of the four")
	{
		for (int reaper_play_state = 0; reaper_play_state < 64; ++reaper_play_state)
		{
			const PlayState play_state = play_state_from_reaper_play_state(reaper_play_state);

			INFO("REAPER play state: " << reaper_play_state);
			REQUIRE(
				std::find(all_play_states.begin(), all_play_states.end(), play_state)
					!= all_play_states.end()
			);
			REQUIRE_FALSE(to_schema_string(play_state).empty());
		}
	}
}

TEST_CASE("a track whose GUID the schema cannot carry is dropped, not sent malformed", "[context][snapshot]")
{
	SECTION("the pattern is as loose as the schema's and no looser")
	{
		REQUIRE(is_schema_shaped_guid(test_guid(1)));
		REQUIRE(is_schema_shaped_guid("{00000000-0000-0000-0000-000000000000}"));
		REQUIRE(is_schema_shaped_guid("{aAbBcCdDeEfF0123456789----0123456789}"));

		REQUIRE_FALSE(is_schema_shaped_guid(""));
		REQUIRE_FALSE(is_schema_shaped_guid("00000000-0000-0000-0000-000000000000"));
		REQUIRE_FALSE(is_schema_shaped_guid("{00000000-0000-0000-0000-00000000000}"));
		REQUIRE_FALSE(is_schema_shaped_guid("{00000000-0000-0000-0000-0000000000000}"));
		REQUIRE_FALSE(is_schema_shaped_guid("{0000000g-0000-0000-0000-000000000000}"));
		REQUIRE_FALSE(is_schema_shaped_guid("(00000000-0000-0000-0000-000000000000)"));
	}

	SECTION("the unusable track is left out and the rest are sent")
	{
		ProjectContextReading reading;
		reading.readable = true;
		reading.globals = plain_globals();

		reading.tracks_in_project_order.push_back(plain_track(test_guid(1), "Kick"));
		reading.tracks_in_project_order.push_back(plain_track("", "Unreadable"));
		reading.tracks_in_project_order.push_back(plain_track(test_guid(3), "Bass"));

		const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

		REQUIRE(snapshot.tracks.size() == 2);
		REQUIRE(snapshot.tracks_omitted_for_unusable_guid == 1);
		REQUIRE(snapshot.tracks.front().name == "Kick");
		REQUIRE(snapshot.tracks.back().name == "Bass");
	}

	SECTION("surviving tracks keep REAPER's own track numbers")
	{
		// The index is the number the producer sees in REAPER, so the array is not
		// renumbered around the gap. Renumbering would have the agent say "track 2" for
		// a track REAPER calls 3.
		ProjectContextReading reading;
		reading.readable = true;
		reading.globals = plain_globals();

		reading.tracks_in_project_order.push_back(plain_track(test_guid(1), "Kick"));
		reading.tracks_in_project_order.push_back(plain_track("not-a-guid", "Unreadable"));
		reading.tracks_in_project_order.push_back(plain_track(test_guid(3), "Bass"));

		const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

		REQUIRE(snapshot.tracks.front().index == 0);
		REQUIRE(snapshot.tracks.back().index == 2);
	}

	SECTION("a selected track with an unusable GUID is counted out of the selection")
	{
		ProjectContextReading reading;
		reading.readable = true;
		reading.globals = plain_globals();

		TrackReading unreadable = plain_track("{}", "Unreadable");
		unreadable.selected = true;

		reading.tracks_in_project_order.push_back(std::move(unreadable));

		const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

		REQUIRE(snapshot.selected_track_guids.empty());
		REQUIRE(snapshot.selected_track_guids_omitted == 1);
	}

	SECTION("the master track is omitted entirely when its GUID is unusable")
	{
		ProjectContextReading reading = plain_reading();
		reading.master_track.guid = "MASTER";

		const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

		REQUIRE_FALSE(snapshot.master_track.present);
		REQUIRE_FALSE(serialize(snapshot).contains(std::string{snapshot_master_track_property}));
	}

	SECTION("and when REAPER handed back no master track at all")
	{
		ProjectContextReading reading = plain_reading();
		reading.master_track.present = false;

		REQUIRE_FALSE(
			serialize_reading(reading).contains(std::string{snapshot_master_track_property})
		);
	}
}

TEST_CASE("the selection is capped at the schema's maxItems", "[context][snapshot]")
{
	ProjectContextReading reading;
	reading.readable = true;
	reading.globals = plain_globals();

	const std::size_t track_count = maximum_selected_track_guids + 3;

	for (std::size_t ordinal = 0; ordinal < track_count; ++ordinal)
	{
		// The GUID generator gives 256 distinct values, which is fine here: the cap is
		// about count, and a repeated GUID is still a string the schema accepts.
		TrackReading track = plain_track(test_guid(static_cast<int>(ordinal % 256)), "Track");
		track.selected = true;

		reading.tracks_in_project_order.push_back(std::move(track));
	}

	const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

	REQUIRE(snapshot.selected_track_guids.size() == maximum_selected_track_guids);
	REQUIRE(snapshot.selected_track_guids_omitted == 3);
	REQUIRE(snapshot.tracks.size() == track_count);
}

TEST_CASE("folder parentage is accumulated across track order", "[context][snapshot]")
{
	// Drums opens a folder holding Kick and Snare; Snare closes it. Bass is top level.
	// The shape is what requirement 6.5 is about — there is no parent pointer to read,
	// only the deltas and the order.
	ProjectContextReading reading;
	reading.readable = true;
	reading.globals = plain_globals();

	reading.tracks_in_project_order.push_back(folder_track(1, "Drums", 1));
	reading.tracks_in_project_order.push_back(folder_track(2, "Kick", 0));
	reading.tracks_in_project_order.push_back(folder_track(3, "Snare", -1));
	reading.tracks_in_project_order.push_back(folder_track(4, "Bass", 0));

	const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

	REQUIRE(snapshot.tracks.size() == 4);

	SECTION("nesting level comes out of the accumulation")
	{
		REQUIRE(snapshot.tracks[0].role_signals.accumulated_folder_depth == 0);
		REQUIRE(snapshot.tracks[1].role_signals.accumulated_folder_depth == 1);
		REQUIRE(snapshot.tracks[2].role_signals.accumulated_folder_depth == 1);
		REQUIRE(snapshot.tracks[3].role_signals.accumulated_folder_depth == 0);
	}

	SECTION("so do the child counts, which is what decides whether a parent sums")
	{
		REQUIRE(snapshot.tracks[0].role_signals.child_track_count == 2);
		REQUIRE(snapshot.tracks[0].role_signals.children_with_parent_send_enabled == 2);
		REQUIRE(snapshot.tracks[1].role_signals.child_track_count == 0);
		REQUIRE(snapshot.tracks[3].role_signals.child_track_count == 0);
	}

	SECTION("and the roles that follow from them")
	{
		REQUIRE(snapshot.tracks[0].role == StructuralRole::summing_folder_parent);
		REQUIRE(snapshot.tracks[1].role == StructuralRole::normal);
		REQUIRE(snapshot.tracks[2].role == StructuralRole::normal);
		REQUIRE(snapshot.tracks[3].role == StructuralRole::normal);
	}

	SECTION("nested folders accumulate rather than reset")
	{
		ProjectContextReading nested;
		nested.readable = true;
		nested.globals = plain_globals();

		nested.tracks_in_project_order.push_back(folder_track(1, "Band", 1));
		nested.tracks_in_project_order.push_back(folder_track(2, "Drums", 1));
		nested.tracks_in_project_order.push_back(folder_track(3, "Kick", 0));
		nested.tracks_in_project_order.push_back(folder_track(4, "Snare", -2));

		const ProjectContextSnapshot nested_snapshot = build_project_context_snapshot(nested);

		REQUIRE(nested_snapshot.tracks[0].role_signals.accumulated_folder_depth == 0);
		REQUIRE(nested_snapshot.tracks[1].role_signals.accumulated_folder_depth == 1);
		REQUIRE(nested_snapshot.tracks[2].role_signals.accumulated_folder_depth == 2);
		REQUIRE(nested_snapshot.tracks[3].role_signals.accumulated_folder_depth == 2);

		REQUIRE(nested_snapshot.tracks[0].role_signals.child_track_count == 1);
		REQUIRE(nested_snapshot.tracks[1].role_signals.child_track_count == 2);
	}

	SECTION("a delta list REAPER should never produce is absorbed, not repaired")
	{
		// A close with nothing open. `reconstruct_folder_paths` closes only what is
		// actually open and `normalize_structural_role_signals` clamps what reaches the
		// derivation, so the snapshot still describes something. Repairing the deltas is
		// requirement 11's job and belongs to an edit, not to a read.
		ProjectContextReading malformed;
		malformed.readable = true;
		malformed.globals = plain_globals();

		malformed.tracks_in_project_order.push_back(folder_track(1, "Orphan", -4));
		malformed.tracks_in_project_order.push_back(folder_track(2, "Next", 0));

		const ProjectContextSnapshot malformed_snapshot = build_project_context_snapshot(malformed);

		REQUIRE(malformed_snapshot.tracks.size() == 2);

		for (const auto& track : malformed_snapshot.tracks)
		{
			REQUIRE(track.role_signals.accumulated_folder_depth >= 0);
		}
	}
}

TEST_CASE("a folder parent nothing feeds is reported silent, and the role reaches the payload", "[context][snapshot]")
{
	// The summing-versus-silent distinction the agent relays to the producer, checked
	// here because the role in the snapshot is where it comes from.
	ProjectContextReading reading;
	reading.readable = true;
	reading.globals = plain_globals();

	TrackReading bus = folder_track(1, "Drum Bus", 1);
	TrackReading kick = folder_track(2, "Kick", 0);
	TrackReading snare = folder_track(3, "Snare", -1);

	kick.parent_send_enabled = false;
	snare.parent_send_enabled = false;

	reading.tracks_in_project_order.push_back(std::move(bus));
	reading.tracks_in_project_order.push_back(std::move(kick));
	reading.tracks_in_project_order.push_back(std::move(snare));

	const ProjectContextSnapshot snapshot = build_project_context_snapshot(reading);

	REQUIRE(snapshot.tracks.front().role == StructuralRole::silent_folder_parent);

	const StubDocument document = serialize(snapshot);

	REQUIRE(
		document.at("tracks").elements().front().at("role").string_value()
			== "silent_folder_parent"
	);
}

TEST_CASE("every structural role spells the same string in the snapshot as in the schema", "[context][snapshot]")
{
	// The enumeration and the schema's `structuralRole` enum have to agree exactly: a
	// spelling drift here is a snapshot the server refuses for a reason nobody would
	// guess from the error.
	for (const StructuralRole role : all_structural_roles)
	{
		ProjectContextSnapshot snapshot;

		sesh_ai::context::SnapshotTrack track;
		track.guid = test_guid(1);
		track.index = 0;
		track.name = "Track";
		track.role = role;

		snapshot.tracks.push_back(track);

		const StubDocument document = serialize(snapshot);

		INFO("role: " << to_schema_string(role));
		REQUIRE(
			document.at("tracks").elements().front().at("role").string_value()
				== std::string{to_schema_string(role)}
		);
	}
}

TEST_CASE("an empty project is a valid snapshot", "[context][snapshot]")
{
	// Worth its own case: a producer opening REAPER and connecting before adding a
	// track is the first snapshot the server ever sees, and `tracks` is required.
	ProjectContextReading reading;
	reading.readable = true;
	reading.globals = plain_globals();
	reading.globals.project_name.clear();

	const StubDocument document = serialize_reading(reading);

	REQUIRE(document.at("projectName").string_value().empty());
	REQUIRE(document.at("tracks").is_array());
	REQUIRE(document.at("tracks").elements().empty());
	REQUIRE(document.at("selectedTrackGuids").is_array());
	REQUIRE(document.at("selectedTrackGuids").elements().empty());
	REQUIRE_FALSE(document.contains(std::string{snapshot_master_track_property}));
}

TEST_CASE("the snapshot's log line names its size and what it dropped", "[context][snapshot]")
{
	ProjectContextReading reading = plain_reading();
	reading.tracks_in_project_order.push_back(plain_track("bad", "Unreadable"));
	reading.globals.marker_count = 5;

	const std::string description =
		describe_project_context_snapshot(build_project_context_snapshot(reading));

	REQUIRE(description.find("tracks=1") != std::string::npos);
	REQUIRE(description.find("tracksOmitted=1") != std::string::npos);
	REQUIRE(description.find("markerCount=5") != std::string::npos);
	REQUIRE(description.find("masterTrack=present") != std::string::npos);

	// The log says how big the snapshot was, not what was in it. Track names are the
	// producer's work and do not belong in a log line.
	REQUIRE(description.find("Kick") == std::string::npos);
}
