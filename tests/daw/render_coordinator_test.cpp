// Unit tests for the Render Coordinator (requirement 12).
//
// The formal properties 23, 24, and 25 are task 11.2 and sit on top of these rather
// than replacing them. A property that settings composition round-trips does not tell
// you that a stem render composes to `&2` in particular, and a property that
// collisions are always caught does not tell you that nothing was queued when one
// was.
//
// Three of these cases exist because of a specific way to get this component wrong:
//
//   - `RENDER_SETTINGS is written exactly once per render` fails if anyone ever
//     writes a second flag, which in REAPER overwrites rather than accumulates and
//     silently turns a stem render into a master mix. The fake counts the writes.
//   - `nothing is queued when the resolved paths collide` fails if collision detection
//     moves after the queue call. The fake records the whole call sequence, so the
//     ordering is asserted rather than assumed.
//   - `stems_via_master queues one job per selected track, including ineligible ones`
//     fails if anyone adds the filter that looks obviously right. Requirement 12.6
//     says unconditionally, and a summing folder parent in the selection is what
//     catches a filter.
//
// The fake host is a fake, not a mock: it answers from plain data and records what was
// asked of it. Nothing here needs REAPER, which is the point of the seam.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <daw/render_coordinator.h>

namespace
{
	using namespace sesh_ai::daw;

	// -------------------------------------------------------------------
	// The fake host
	// -------------------------------------------------------------------

	// Answers from data and records the sequence of calls.
	//
	// The recorded log is what makes the ordering requirements testable: "collisions
	// are detected before any file is written" is a statement about the order of
	// operations, and an outcome value alone cannot express it.
	class recording_render_host final : public RenderHost
	{
	public:
		// --- what it answers with ---

		render_project_state project_state{};
		std::vector<render_track> selected_tracks{};
		std::optional<time_span> region_span{};
		std::string render_directory{"/Users/producer/Music/Session/renders"};

		// The `RENDER_TARGETS` answer. When `render_targets_per_read` is non-empty it
		// is consumed one entry per read, which is how a `stems_via_master` resolution
		// pass gets a different answer per soloed track.
		std::string render_targets{};
		std::vector<std::string> render_targets_per_read{};

		// Paths that already exist on disk, compared verbatim.
		std::vector<std::string> existing_files{};

		// --- what it recorded ---

		std::vector<std::string> call_log{};
		std::vector<int> written_settings_values{};
		std::vector<int> written_bounds_flags{};
		std::vector<time_span> written_bounds_spans{};
		std::vector<int> written_sample_rates{};
		std::vector<std::string> written_sink_codes{};
		std::vector<std::optional<int>> written_bit_depths{};
		std::vector<std::string> written_directories{};
		std::vector<std::string> written_patterns{};
		std::vector<std::string> soloed_track_guids{};
		std::vector<std::string> unsoloed_track_guids{};
		int queue_additions = 0;

		// --- reads ---

		render_project_state read_project_state() override
		{
			call_log.emplace_back("read_project_state");
			return project_state;
		}

		std::vector<render_track> read_selected_tracks() override
		{
			call_log.emplace_back("read_selected_tracks");
			return selected_tracks;
		}

		std::optional<time_span> find_region_span(const std::string& region_guid) override
		{
			call_log.push_back("find_region_span:" + region_guid);
			return region_span;
		}

		std::string read_render_directory() override
		{
			call_log.emplace_back("read_render_directory");
			return render_directory;
		}

		// --- writes ---

		void write_render_settings(render_settings_word settings) override
		{
			call_log.emplace_back("write_render_settings");
			written_settings_values.push_back(settings.value());
		}

		void write_render_bounds(int bounds_flag, const time_span& span) override
		{
			call_log.emplace_back("write_render_bounds");
			written_bounds_flags.push_back(bounds_flag);
			written_bounds_spans.push_back(span);
		}

		void write_render_sample_rate(int sample_rate) override
		{
			call_log.emplace_back("write_render_sample_rate");
			written_sample_rates.push_back(sample_rate);
		}

		void write_render_format(const render_format_request& format) override
		{
			call_log.emplace_back("write_render_format");
			written_sink_codes.emplace_back(format.sink_four_character_code);
			written_bit_depths.push_back(format.bit_depth);
		}

		void write_render_output(
			const std::string& directory,
			const std::string& file_name_pattern) override
		{
			call_log.emplace_back("write_render_output");
			written_directories.push_back(directory);
			written_patterns.push_back(file_name_pattern);
		}

		std::string read_render_targets() override
		{
			call_log.emplace_back("read_render_targets");

			if (!render_targets_per_read.empty())
			{
				const std::string answer = render_targets_per_read[next_targets_read_];

				if (next_targets_read_ + 1 < render_targets_per_read.size())
				{
					++next_targets_read_;
				}

				return answer;
			}

			return render_targets;
		}

		bool output_file_exists(const std::string& resolved_path) override
		{
			call_log.push_back("output_file_exists:" + resolved_path);

			return std::find(existing_files.begin(), existing_files.end(), resolved_path)
				!= existing_files.end();
		}

		void set_track_solo(const std::string& track_guid, bool soloed) override
		{
			call_log.push_back(std::string{soloed ? "solo_on:" : "solo_off:"} + track_guid);

			if (soloed)
			{
				soloed_track_guids.push_back(track_guid);
			}
			else
			{
				unsoloed_track_guids.push_back(track_guid);
			}
		}

		void add_project_to_render_queue() override
		{
			call_log.emplace_back("add_project_to_render_queue");
			++queue_additions;
		}

	private:
		std::size_t next_targets_read_ = 0;
	};

	// -------------------------------------------------------------------
	// Fixtures
	// -------------------------------------------------------------------

	render_track make_track(std::string guid, std::string name)
	{
		render_track track;
		track.guid = std::move(guid);
		track.name = std::move(name);

		return track;
	}

	// A project long enough to render, with a usable time selection and a 48 kHz rate.
	render_project_state make_project_state()
	{
		render_project_state state;
		state.project_length_seconds = 214.5;
		state.time_selection = time_span{32.0, 64.0};
		state.sample_rate = 48000;

		return state;
	}

	// One master mix file. The common master-mix answer from `RENDER_TARGETS`.
	const std::string one_master_mix_target{"/Users/producer/Music/Session/renders/Session.wav"};

	// Three distinct stem files.
	const std::string three_stem_targets{
		"/Users/producer/Music/Session/renders/Session - Kick.wav;"
		"/Users/producer/Music/Session/renders/Session - Snare.wav;"
		"/Users/producer/Music/Session/renders/Session - Bass.wav"
	};

	std::vector<render_track> three_normal_tracks()
	{
		return {
			make_track("{11111111-1111-1111-1111-111111111111}", "Kick"),
			make_track("{22222222-2222-2222-2222-222222222222}", "Snare"),
			make_track("{33333333-3333-3333-3333-333333333333}", "Bass"),
		};
	}

	// How many times a name appears in the call log.
	int count_calls(const std::vector<std::string>& call_log, const std::string& name)
	{
		return static_cast<int>(std::count(call_log.begin(), call_log.end(), name));
	}

	// The position of the first call with this name, or -1.
	int first_call_index(const std::vector<std::string>& call_log, const std::string& name)
	{
		const auto found = std::find(call_log.begin(), call_log.end(), name);

		if (found == call_log.end())
		{
			return -1;
		}

		return static_cast<int>(std::distance(call_log.begin(), found));
	}
}

// -----------------------------------------------------------------------
// Settings composition: one integer, OR-ed (requirement 12.2)
// -----------------------------------------------------------------------

TEST_CASE("a master mix is the absence of the stem bits, not a bit of its own")
{
	render_settings settings;
	settings.source = render_settings_source::master_mix;

	const render_settings_word word = compose_render_settings(settings);

	CHECK(word.value() == 0);
	CHECK((word.value() & render_settings_flag::source_axis_mask) == 0);
}

TEST_CASE("each source occupies exactly one bit of the source axis")
{
	const auto word_for = [](render_settings_source source) {
		render_settings settings;
		settings.source = source;

		return compose_render_settings(settings).value();
	};

	CHECK(word_for(render_settings_source::stems_and_master_mix) == 1);
	CHECK(word_for(render_settings_source::stems_only) == 2);
	CHECK(word_for(render_settings_source::selected_tracks_via_master) == 128);
}

TEST_CASE("flags are OR-ed into one integer rather than replacing each other")
{
	// The specific mistake this guards: writing `&2` and then `&8` leaves `8`, not
	// `10`. Composition has to produce the union.
	render_settings settings;
	settings.source = render_settings_source::stems_only;
	settings.use_render_matrix = true;
	settings.mono_media_to_mono_files = true;
	settings.embed_metadata = true;

	const int expected = render_settings_flag::stems_only
		| render_settings_flag::use_render_matrix
		| render_settings_flag::mono_media_to_mono_files
		| render_settings_flag::embed_metadata;

	CHECK(compose_render_settings(settings).value() == expected);
	CHECK(compose_render_settings(settings).value() == (2 | 8 | 16 | 512));
}

TEST_CASE("composition round-trips for every combination of settings")
{
	// Exhaustive rather than sampled: ten independent flags and four sources is 4096
	// combinations, which is cheaper to enumerate than to argue about. Property 23
	// (task 11.2) generalises this to arbitrary words.
	const std::vector<render_settings_source> sources{
		render_settings_source::master_mix,
		render_settings_source::stems_and_master_mix,
		render_settings_source::stems_only,
		render_settings_source::selected_tracks_via_master,
	};

	constexpr int flag_count = 10;

	for (const render_settings_source source : sources)
	{
		for (int flag_bits = 0; flag_bits < (1 << flag_count); ++flag_bits)
		{
			render_settings settings;
			settings.source = source;
			settings.multichannel_tracks_to_multichannel_files = (flag_bits & (1 << 0)) != 0;
			settings.use_render_matrix = (flag_bits & (1 << 1)) != 0;
			settings.mono_media_to_mono_files = (flag_bits & (1 << 2)) != 0;
			settings.embed_transients = (flag_bits & (1 << 3)) != 0;
			settings.embed_metadata = (flag_bits & (1 << 4)) != 0;
			settings.embed_take_markers = (flag_bits & (1 << 5)) != 0;
			settings.second_pass_render = (flag_bits & (1 << 6)) != 0;
			settings.render_razor_edits = (flag_bits & (1 << 7)) != 0;
			settings.pre_fader_stems = (flag_bits & (1 << 8)) != 0;
			settings.only_stem_channels_sent_to_parent = (flag_bits & (1 << 9)) != 0;

			const render_settings_word word = compose_render_settings(settings);

			if (decompose_render_settings(word) != settings)
			{
				FAIL("settings did not round-trip through word " << word.value());
			}
		}
	}
}

TEST_CASE("a settings value read back from REAPER decomposes to the flags it carries")
{
	// A stem render with the render matrix and mono folding on, as REAPER would report
	// it.
	const render_settings settings =
		decompose_render_settings(render_settings_word_as_read_from_reaper(2 | 8 | 16));

	CHECK(settings.source == render_settings_source::stems_only);
	CHECK(settings.use_render_matrix);
	CHECK(settings.mono_media_to_mono_files);
	CHECK_FALSE(settings.embed_metadata);
	CHECK_FALSE(settings.pre_fader_stems);
}

TEST_CASE("the tool's sources map onto REAPER's Source list")
{
	CHECK(settings_source_for(render_source::master_mix) == render_settings_source::master_mix);
	CHECK(settings_source_for(render_source::stems) == render_settings_source::stems_only);

	// Requirement 12.6: each job in the solo loop is a master mix render. The native
	// `&128` is available but deliberately unused — see render_coordinator.h.
	CHECK(settings_source_for(render_source::stems_via_master) == render_settings_source::master_mix);
}

TEST_CASE("a stem render asks for stems only, not stems plus a master mix")
{
	// ADR 0018 rejected a combined value: a producer asking for stems should receive
	// stems, not stems and an extra file they did not mention.
	const render_settings settings = render_settings_for(render_source::stems);

	CHECK(settings.source == render_settings_source::stems_only);
	CHECK((compose_render_settings(settings).value()
		& render_settings_flag::stems_and_master_mix) == 0);

	// Plain stems are already pre-master; taking the fader out as well is a different
	// request the tool does not expose.
	CHECK_FALSE(settings.pre_fader_stems);
}

// -----------------------------------------------------------------------
// The single write (requirement 12.2)
// -----------------------------------------------------------------------

TEST_CASE("the settings write gate permits one write and refuses the rest")
{
	recording_render_host host;
	single_render_settings_write gate{host};

	CHECK(gate.write(compose_render_settings(render_settings_for(render_source::stems))));
	CHECK_FALSE(gate.write(compose_render_settings(render_settings_for(render_source::master_mix))));
	CHECK_FALSE(gate.write(compose_render_settings(render_settings_for(render_source::master_mix))));

	CHECK(gate.writes_performed() == 1);
	REQUIRE(host.written_settings_values.size() == 1);

	// The refused writes did not reach the host, so the first value stands rather than
	// being overwritten by the master mix word.
	CHECK(host.written_settings_values.front() == render_settings_flag::stems_only);
}

TEST_CASE("RENDER_SETTINGS is written exactly once per render")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};

	SECTION("for a master mix")
	{
		const render_request request{};

		const render_outcome outcome = coordinator.queue_render(request);

		REQUIRE(outcome.status == render_outcome_status::queued);
		CHECK(count_calls(host.call_log, "write_render_settings") == 1);
	}

	SECTION("for a stem set")
	{
		host.selected_tracks = three_normal_tracks();
		host.render_targets = three_stem_targets;

		render_request request;
		request.source = render_source::stems;

		const render_outcome outcome = coordinator.queue_render(request);

		REQUIRE(outcome.status == render_outcome_status::queued);
		CHECK(count_calls(host.call_log, "write_render_settings") == 1);
		CHECK(host.written_settings_values.front() == render_settings_flag::stems_only);
	}

	SECTION("and once for the whole stems_via_master loop, not once per job")
	{
		host.selected_tracks = three_normal_tracks();
		host.render_targets_per_read = {
			"/renders/Session - Kick.wav",
			"/renders/Session - Snare.wav",
			"/renders/Session - Bass.wav",
		};

		render_request request;
		request.source = render_source::stems_via_master;

		const render_outcome outcome = coordinator.queue_render(request);

		REQUIRE(outcome.status == render_outcome_status::queued);
		CHECK(outcome.result.queued_job_count == 3);
		CHECK(count_calls(host.call_log, "write_render_settings") == 1);
	}
}

// -----------------------------------------------------------------------
// Bounds (requirement 12.3, and 9.6's end-greater-than-start)
// -----------------------------------------------------------------------

TEST_CASE("each bounds value selects REAPER's own bounds flag")
{
	CHECK(bounds_flag_for(render_bounds::project) == render_bounds_flag::entire_project);
	CHECK(bounds_flag_for(render_bounds::time_selection) == render_bounds_flag::time_selection);

	// A region renders as custom bounds rather than flag 5, so that expressing "this
	// region" does not mutate the producer's region selection — a side effect a tool
	// with a `none` undo effect has no way to put back.
	CHECK(bounds_flag_for(render_bounds::region) == render_bounds_flag::custom_time_bounds);
}

TEST_CASE("project bounds resolve to the whole timeline and are reported in seconds")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};

	const render_outcome outcome = coordinator.queue_render(render_request{});

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK_THAT(outcome.result.start_seconds, Catch::Matchers::WithinAbs(0.0, 1e-9));
	CHECK_THAT(outcome.result.end_seconds, Catch::Matchers::WithinAbs(214.5, 1e-9));
	CHECK(outcome.result.region_guid.empty());
	REQUIRE(host.written_bounds_flags.size() == 1);
	CHECK(host.written_bounds_flags.front() == render_bounds_flag::entire_project);
}

TEST_CASE("time selection bounds resolve to the selection rather than echoing the word")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_request request;
	request.bounds = render_bounds::time_selection;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK_THAT(outcome.result.start_seconds, Catch::Matchers::WithinAbs(32.0, 1e-9));
	CHECK_THAT(outcome.result.end_seconds, Catch::Matchers::WithinAbs(64.0, 1e-9));
	CHECK(outcome.result.bounds == render_bounds::time_selection);
}

TEST_CASE("region bounds are resolved through the host and echoed back with the GUID")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;
	host.region_span = time_span{96.25, 128.75};

	render_request request;
	request.bounds = render_bounds::region;
	request.region_guid = "{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}";

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK_THAT(outcome.result.start_seconds, Catch::Matchers::WithinAbs(96.25, 1e-9));
	CHECK_THAT(outcome.result.end_seconds, Catch::Matchers::WithinAbs(128.75, 1e-9));
	CHECK(outcome.result.region_guid == request.region_guid);
	REQUIRE(host.written_bounds_spans.size() == 1);
	CHECK_THAT(host.written_bounds_spans.front().start_seconds, Catch::Matchers::WithinAbs(96.25, 1e-9));
}

TEST_CASE("bounds that cannot be resolved are a failure, not a refusal")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};

	SECTION("region bounds with no GUID")
	{
		render_request request;
		request.bounds = render_bounds::region;

		const render_outcome outcome = coordinator.queue_render(request);

		CHECK(outcome.status == render_outcome_status::failed);
		CHECK_FALSE(outcome.failure_reason.empty());
		CHECK(host.queue_additions == 0);
	}

	SECTION("a GUID no region carries")
	{
		render_request request;
		request.bounds = render_bounds::region;
		request.region_guid = "{BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB}";
		host.region_span = std::nullopt;

		const render_outcome outcome = coordinator.queue_render(request);

		CHECK(outcome.status == render_outcome_status::failed);
		CHECK(outcome.failure_reason.find(request.region_guid) != std::string::npos);
		CHECK(host.queue_additions == 0);
	}

	SECTION("a zero-length span")
	{
		// An empty time selection. The output schema's endSeconds has an exclusive
		// minimum of zero, so this could not be reported as a result even if it were
		// queued.
		host.project_state.time_selection = time_span{48.0, 48.0};

		render_request request;
		request.bounds = render_bounds::time_selection;

		const render_outcome outcome = coordinator.queue_render(request);

		CHECK(outcome.status == render_outcome_status::failed);
		CHECK(host.queue_additions == 0);
		CHECK(count_calls(host.call_log, "write_render_settings") == 0);
	}
}

// -----------------------------------------------------------------------
// Path resolution
// -----------------------------------------------------------------------

TEST_CASE("RENDER_TARGETS splits into the paths REAPER would write")
{
	SECTION("one path")
	{
		const std::vector<std::string> paths = split_render_targets("/renders/Session.wav");

		REQUIRE(paths.size() == 1);
		CHECK(paths.front() == "/renders/Session.wav");
	}

	SECTION("several paths")
	{
		const std::vector<std::string> paths = split_render_targets(three_stem_targets);

		REQUIRE(paths.size() == 3);
		CHECK(paths[0] == "/Users/producer/Music/Session/renders/Session - Kick.wav");
		CHECK(paths[2] == "/Users/producer/Music/Session/renders/Session - Bass.wav");
	}

	SECTION("a trailing separator is not a file")
	{
		const std::vector<std::string> paths = split_render_targets("/renders/a.wav;/renders/b.wav;");

		CHECK(paths.size() == 2);
	}

	SECTION("padding is not part of the path")
	{
		const std::vector<std::string> paths = split_render_targets(" /renders/a.wav ;\t/renders/b.wav");

		REQUIRE(paths.size() == 2);
		CHECK(paths[0] == "/renders/a.wav");
		CHECK(paths[1] == "/renders/b.wav");
	}

	SECTION("nothing at all")
	{
		CHECK(split_render_targets("").empty());
		CHECK(split_render_targets(";;").empty());
	}

	SECTION("a Windows path is one path, separators and all")
	{
		const std::vector<std::string> paths =
			split_render_targets("C:\\Users\\producer\\renders\\Session.wav");

		REQUIRE(paths.size() == 1);
		CHECK(paths.front() == "C:\\Users\\producer\\renders\\Session.wav");
	}
}

TEST_CASE("paths are compared in a form that matches how the filesystem sees them")
{
	CHECK(normalize_output_path_for_comparison("C:\\renders\\Kick.wav") == "c:/renders/kick.wav");
	CHECK(normalize_output_path_for_comparison("/renders/Kick.wav")
		== normalize_output_path_for_comparison("/renders/kick.WAV"));

	// Non-ASCII is left alone: a half-correct Unicode fold is worse than a declared
	// ASCII one.
	CHECK(normalize_output_path_for_comparison("/renders/Café.wav")
		== "/renders/caf\xc3\xa9.wav");
}

TEST_CASE("self-colliding output paths are found and reported once each")
{
	SECTION("a distinct set collides with nothing")
	{
		CHECK(find_self_colliding_output_paths({"/a.wav", "/b.wav", "/c.wav"}).empty());
	}

	SECTION("a repeat is reported once, in resolved form")
	{
		const std::vector<std::string> colliding =
			find_self_colliding_output_paths({"/a.wav", "/b.wav", "/a.wav", "/a.wav"});

		REQUIRE(colliding.size() == 1);
		CHECK(colliding.front() == "/a.wav");
	}

	SECTION("several collisions come back in first-appearance order")
	{
		const std::vector<std::string> colliding =
			find_self_colliding_output_paths({"/b.wav", "/a.wav", "/b.wav", "/a.wav"});

		REQUIRE(colliding.size() == 2);
		CHECK(colliding[0] == "/b.wav");
		CHECK(colliding[1] == "/a.wav");
	}

	SECTION("differing only in case is one file on macOS and Windows")
	{
		const std::vector<std::string> colliding =
			find_self_colliding_output_paths({"/renders/Kick.wav", "/renders/kick.wav"});

		REQUIRE(colliding.size() == 1);
		CHECK(colliding.front() == "/renders/kick.wav");
	}

	SECTION("differing only in separator is one file on Windows")
	{
		const std::vector<std::string> colliding =
			find_self_colliding_output_paths({"C:\\renders\\a.wav", "C:/renders/a.wav"});

		CHECK(colliding.size() == 1);
	}

	SECTION("an empty set collides with nothing")
	{
		CHECK(find_self_colliding_output_paths({}).empty());
	}
}

// -----------------------------------------------------------------------
// Collision refusals (requirements 12.4, 9.8)
// -----------------------------------------------------------------------

TEST_CASE("nothing is queued when the resolved paths collide with each other")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();

	// A pattern without $track would resolve every stem to the same name. This is
	// what that looks like coming back from REAPER.
	host.render_targets = "/renders/Session.wav;/renders/Session.wav;/renders/Session.wav";

	render_request request;
	request.source = render_source::stems;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::refused);
	CHECK(outcome.refusal.reason == "output_file_collision");

	// Enumerated, because the producer needs to see which files.
	REQUIRE(outcome.refusal.blocking.size() == 1);
	CHECK(outcome.refusal.blocking.front().kind == "file_path");
	CHECK(outcome.refusal.blocking.front().description == "/renders/Session.wav");

	// Unacknowledgeable: no approval makes two jobs writing one file the intent.
	CHECK(outcome.refusal.acknowledgement_field.empty());

	// The whole point of requirement 12.4 — detected before anything was written.
	CHECK(host.queue_additions == 0);
	CHECK(count_calls(host.call_log, "add_project_to_render_queue") == 0);
}

TEST_CASE("collision detection runs after the paths are resolved and before the queue")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;
	host.existing_files = {one_master_mix_target};

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(render_request{});

	REQUIRE(outcome.status == render_outcome_status::refused);

	const int targets_read = first_call_index(host.call_log, "read_render_targets");
	const int existence_check =
		first_call_index(host.call_log, "output_file_exists:" + one_master_mix_target);

	REQUIRE(targets_read >= 0);
	REQUIRE(existence_check >= 0);
	CHECK(targets_read < existence_check);
	CHECK(first_call_index(host.call_log, "add_project_to_render_queue") == -1);
}

TEST_CASE("an existing output file is refused with the overwrite acknowledgement named")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();
	host.render_targets = three_stem_targets;
	host.existing_files = {
		"/Users/producer/Music/Session/renders/Session - Kick.wav",
		"/Users/producer/Music/Session/renders/Session - Bass.wav",
	};

	render_request request;
	request.source = render_source::stems;

	render_coordinator coordinator{host};

	SECTION("a first call is refused, listing only the files that exist")
	{
		const render_outcome outcome = coordinator.queue_render(request);

		REQUIRE(outcome.status == render_outcome_status::refused);
		CHECK(outcome.refusal.reason == "output_file_collision");
		CHECK(outcome.refusal.acknowledgement_field == "confirmedOverwrite");

		REQUIRE(outcome.refusal.blocking.size() == 2);
		CHECK(outcome.refusal.blocking[0].description
			== "/Users/producer/Music/Session/renders/Session - Kick.wav");
		CHECK(outcome.refusal.blocking[1].description
			== "/Users/producer/Music/Session/renders/Session - Bass.wav");

		CHECK(host.queue_additions == 0);
	}

	SECTION("the acknowledged retry proceeds")
	{
		request.confirmed_overwrite = true;

		const render_outcome outcome = coordinator.queue_render(request);

		CHECK(outcome.status == render_outcome_status::queued);
		CHECK(host.queue_additions == 1);
	}
}

TEST_CASE("a self-collision is refused even when the overwrite was acknowledged")
{
	// The two collisions are not the same refusal. Approving an overwrite says
	// something about files that already exist; it says nothing about two of this
	// render's own jobs writing to one path, where one of the files simply would not
	// be produced.
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();
	host.render_targets = "/renders/Session.wav;/renders/Session.wav";

	render_request request;
	request.source = render_source::stems;
	request.confirmed_overwrite = true;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	CHECK(outcome.status == render_outcome_status::refused);
	CHECK(outcome.refusal.acknowledgement_field.empty());
	CHECK(host.queue_additions == 0);
}

// -----------------------------------------------------------------------
// stems_via_master: the solo-queue-unsolo loop (requirement 12.6)
// -----------------------------------------------------------------------

TEST_CASE("stems_via_master queues one job per selected track and reports the count")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();
	host.render_targets_per_read = {
		"/renders/Session - Kick.wav",
		"/renders/Session - Snare.wav",
		"/renders/Session - Bass.wav",
	};

	render_request request;
	request.source = render_source::stems_via_master;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK(outcome.result.queued_job_count == 3);
	CHECK(host.queue_additions == 3);

	// All three resolved paths are reported, because collision detection ran against
	// exactly these.
	CHECK(outcome.result.output_paths.size() == 3);
}

TEST_CASE("the loop runs over every selected track unconditionally, including buses")
{
	// Requirement 12.6 is explicit that the loop does not filter. A bus and an
	// effect return in the selection are what catch the filter that looks obviously
	// right — the coordinator queues a job for each of them like any other track.
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = {
		make_track("{11111111-1111-1111-1111-111111111111}", "Kick"),
		make_track("{55555555-5555-5555-5555-555555555555}", "Drum Bus"),
		make_track("{66666666-6666-6666-6666-666666666666}", "Reverb"),
	};
	host.render_targets_per_read = {
		"/renders/Session - Kick.wav",
		"/renders/Session - Drum Bus.wav",
		"/renders/Session - Reverb.wav",
	};

	render_request request;
	request.source = render_source::stems_via_master;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK(outcome.result.queued_job_count == 3);
	CHECK(outcome.result.queued_job_count == static_cast<int>(host.selected_tracks.size()));
}

TEST_CASE("each job in the loop is queued with its own track soloed, and solo is restored")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();
	host.render_targets_per_read = {
		"/renders/Session - Kick.wav",
		"/renders/Session - Snare.wav",
		"/renders/Session - Bass.wav",
	};

	render_request request;
	request.source = render_source::stems_via_master;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);

	// Every track that was soloed was unsoloed, so the producer's solo state is where
	// they left it.
	CHECK(host.soloed_track_guids == host.unsoloed_track_guids);

	// The queueing half of the log, in order: solo, queue, unsolo, per track.
	std::vector<std::string> queueing_calls;

	for (const std::string& call : host.call_log)
	{
		const bool is_queueing_call = call == "add_project_to_render_queue"
			|| (call.rfind("solo_on:", 0) == 0)
			|| (call.rfind("solo_off:", 0) == 0);

		if (is_queueing_call)
		{
			queueing_calls.push_back(call);
		}
	}

	// The resolution pass solos and unsolos each track too, so the queueing triples
	// come after six solo calls with no queue addition between them.
	const std::vector<std::string> expected_tail{
		"solo_on:{11111111-1111-1111-1111-111111111111}",
		"add_project_to_render_queue",
		"solo_off:{11111111-1111-1111-1111-111111111111}",
		"solo_on:{22222222-2222-2222-2222-222222222222}",
		"add_project_to_render_queue",
		"solo_off:{22222222-2222-2222-2222-222222222222}",
		"solo_on:{33333333-3333-3333-3333-333333333333}",
		"add_project_to_render_queue",
		"solo_off:{33333333-3333-3333-3333-333333333333}",
	};

	REQUIRE(queueing_calls.size() >= expected_tail.size());

	const std::vector<std::string> actual_tail(
		queueing_calls.end() - static_cast<long>(expected_tail.size()),
		queueing_calls.end());

	CHECK(actual_tail == expected_tail);
}

TEST_CASE("the loop resolves every job's paths before queueing any of them")
{
	// Otherwise a collision in the third job would be found after the first two were
	// already on the queue, which is a partially queued render nobody asked for.
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();

	// Every job resolving to one path is what a pattern missing $track produces.
	host.render_targets = "/renders/Session.wav";

	render_request request;
	request.source = render_source::stems_via_master;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	CHECK(outcome.status == render_outcome_status::refused);
	CHECK(outcome.refusal.reason == "output_file_collision");
	CHECK(host.queue_additions == 0);

	// Solo was still restored on the way out of the resolution pass.
	CHECK(host.soloed_track_guids == host.unsoloed_track_guids);
}

TEST_CASE("the per-track pattern carries the track wildcard, which is what keeps stems distinct")
{
	CHECK(default_render_pattern_for(render_source::master_mix) == "$project");
	CHECK(default_render_pattern_for(render_source::stems).find("$track") != std::string_view::npos);
	CHECK(default_render_pattern_for(render_source::stems_via_master).find("$track")
		!= std::string_view::npos);
}

// -----------------------------------------------------------------------
// Format and sample rate
// -----------------------------------------------------------------------

TEST_CASE("each output format maps to REAPER's four-character sink code")
{
	CHECK(sink_four_character_code_for(render_output_format::wav) == "evaw");
	CHECK(sink_four_character_code_for(render_output_format::mp3) == "l3pm");
	CHECK(sink_four_character_code_for(render_output_format::flac) == "calf");
}

TEST_CASE("mp3 has no bit depth, and the result omits it rather than inventing one")
{
	CHECK(bit_depth_applies_to(render_output_format::wav));
	CHECK(bit_depth_applies_to(render_output_format::flac));
	CHECK_FALSE(bit_depth_applies_to(render_output_format::mp3));

	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = "/renders/Session.mp3";

	render_request request;
	request.output_format = render_output_format::mp3;
	request.bit_depth = 24;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);
	CHECK_FALSE(outcome.result.bit_depth.has_value());
	REQUIRE(host.written_bit_depths.size() == 1);
	CHECK_FALSE(host.written_bit_depths.front().has_value());
}

TEST_CASE("an omitted sample rate resolves to the project's, and the result reports it")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};

	SECTION("omitted")
	{
		const render_outcome outcome = coordinator.queue_render(render_request{});

		REQUIRE(outcome.status == render_outcome_status::queued);
		CHECK(outcome.result.sample_rate == 48000);
		REQUIRE(host.written_sample_rates.size() == 1);
		CHECK(host.written_sample_rates.front() == 48000);
	}

	SECTION("named")
	{
		render_request request;
		request.sample_rate = 96000;

		const render_outcome outcome = coordinator.queue_render(request);

		REQUIRE(outcome.status == render_outcome_status::queued);
		CHECK(outcome.result.sample_rate == 96000);
		CHECK(host.written_sample_rates.front() == 96000);
	}
}

// -----------------------------------------------------------------------
// The result (requirement 12.3), and what it does not carry (requirement 12.7)
// -----------------------------------------------------------------------

TEST_CASE("a queued render reports what was queued")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.selected_tracks = three_normal_tracks();
	host.render_targets = three_stem_targets;
	host.render_directory = "/Users/producer/Music/Session/renders";

	render_request request;
	request.source = render_source::stems;
	request.bounds = render_bounds::time_selection;
	request.bit_depth = 24;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	REQUIRE(outcome.status == render_outcome_status::queued);

	const render_result& result = outcome.result;

	// Requirement 12.3: source, bounds, selected tracks, and the output directory.
	CHECK(result.source == render_source::stems);
	CHECK(result.bounds == render_bounds::time_selection);
	CHECK(result.tracks.size() == 3);
	CHECK(result.tracks.front().name == "Kick");
	CHECK(result.output_directory == "/Users/producer/Music/Session/renders");

	// And the bounds the schema requires resolved rather than echoed.
	CHECK_THAT(result.start_seconds, Catch::Matchers::WithinAbs(32.0, 1e-9));
	CHECK_THAT(result.end_seconds, Catch::Matchers::WithinAbs(64.0, 1e-9));

	// Every path enumerated, because collision detection ran against exactly these.
	REQUIRE(result.output_paths.size() == 3);
	CHECK(result.output_paths[1] == "/Users/producer/Music/Session/renders/Session - Snare.wav");

	CHECK(result.output_format == render_output_format::wav);
	REQUIRE(result.bit_depth.has_value());
	CHECK(*result.bit_depth == 24);
}

TEST_CASE("the result satisfies the bounds the output schema puts on it")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(render_request{});

	REQUIRE(outcome.status == render_outcome_status::queued);

	const render_result& result = outcome.result;

	CHECK(result.queued_job_count >= 1);
	CHECK(result.start_seconds >= 0.0);
	CHECK(result.end_seconds > 0.0);
	CHECK(result.end_seconds > result.start_seconds);
	CHECK_FALSE(result.output_directory.empty());
	CHECK_FALSE(result.output_paths.empty());
	CHECK(result.output_paths.size() <= 512);
	CHECK(result.sample_rate >= 8000);
}

TEST_CASE("a queued render touches REAPER only for the operations rendering needs")
{
	// Requirement 12.7 is visible here as an absence: there is no undo block, no undo
	// position marker, and nothing in the call log that is not a read, a settings
	// write, a path resolution, or the queue addition. `RenderHost` has no undo
	// operation to call, so this case is really asserting that nothing else crept in
	// alongside.
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(render_request{});

	REQUIRE(outcome.status == render_outcome_status::queued);

	const std::vector<std::string> expected_call_log{
		"read_project_state",
		"read_selected_tracks",
		"write_render_settings",
		"write_render_bounds",
		"write_render_sample_rate",
		"write_render_format",
		"read_render_directory",
		"write_render_output",
		"read_render_targets",
		"output_file_exists:" + one_master_mix_target,
		"add_project_to_render_queue",
	};

	CHECK(host.call_log == expected_call_log);
}

TEST_CASE("the schema spellings of the tool's enums are what the schemas spell")
{
	CHECK(to_schema_string(render_source::master_mix) == "master_mix");
	CHECK(to_schema_string(render_source::stems) == "stems");
	CHECK(to_schema_string(render_source::stems_via_master) == "stems_via_master");

	CHECK(to_schema_string(render_bounds::region) == "region");
	CHECK(to_schema_string(render_bounds::time_selection) == "time_selection");
	CHECK(to_schema_string(render_bounds::project) == "project");

	CHECK(to_schema_string(render_output_format::wav) == "wav");
	CHECK(to_schema_string(render_output_format::mp3) == "mp3");
	CHECK(to_schema_string(render_output_format::flac) == "flac");
}

// -----------------------------------------------------------------------
// Failures
// -----------------------------------------------------------------------

TEST_CASE("a per-track render with nothing selected fails rather than queueing silence")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = one_master_mix_target;

	render_request request;
	request.source = render_source::stems;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(request);

	CHECK(outcome.status == render_outcome_status::failed);
	CHECK_FALSE(outcome.failure_reason.empty());
	CHECK(host.queue_additions == 0);
}

TEST_CASE("resolving no output paths fails, since the result must enumerate at least one")
{
	recording_render_host host;
	host.project_state = make_project_state();
	host.render_targets = "";

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(render_request{});

	CHECK(outcome.status == render_outcome_status::failed);
	CHECK_FALSE(outcome.failure_reason.empty());
	CHECK(host.queue_additions == 0);
}

TEST_CASE("a failure always carries a reason")
{
	// A reasonless failure is not a representable result — the same rule requirement
	// 9.5 puts on an action array's failures.
	recording_render_host host;
	host.project_state.project_length_seconds = 0.0;

	render_coordinator coordinator{host};
	const render_outcome outcome = coordinator.queue_render(render_request{});

	REQUIRE(outcome.status == render_outcome_status::failed);
	CHECK_FALSE(outcome.failure_reason.empty());
}
