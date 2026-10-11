// The `render` tool handler — the wiring between the Tool Executor and the Render
// Coordinator (task 10.9, requirements 9.1, 12.1, 12.6).
//
// The Render Coordinator is already tested next door, exhaustively: settings
// composition, path resolution, collision detection, and the solo-queue-unsolo loop
// all have their own suites. None of that is re-tested here. What this file checks is
// the part that only exists once the component is registered as a tool, and it is
// where the risk in task 10.9 actually lives:
//
//   - `render` is registered through the path that opens no undo block, so its result
//     carries no undo report and no marker is captured (requirement 12.6). Asserted as
//     an absence, because absence is what `outputs/render.schema.json` enforces with
//     `additionalProperties: false`.
//   - A collision refusal reaches the caller with its reason and its acknowledgement
//     field intact, in both forms, and nothing was queued.
//   - `confirmedOverwrite` travels from the framework's own extraction into the
//     coordinator's request, so a retry after an approved overwrite queues.
//   - A coordinator failure is a failed action carrying a reason rather than a
//     refusal — the two are different answers to the producer.
//   - An unknown tool name still produces requirement 23.6's error rather than
//     reaching the handler at all.
//
// The scripted host is this file's own. `render_coordinator_test.cpp` has one too, in
// an anonymous namespace, and reaching into it would couple two suites that test
// different things.
#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/render_tool.h>

using sesh_ai::daw::action_failed;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::output_file_collision_refusal_reason;
using sesh_ai::daw::overwrite_acknowledgement_field;
using sesh_ai::daw::render_bounds;
using sesh_ai::daw::render_coordinator;
using sesh_ai::daw::render_format_request;
using sesh_ai::daw::render_output_format;
using sesh_ai::daw::render_project_state;
using sesh_ai::daw::render_request;
using sesh_ai::daw::render_result;
using sesh_ai::daw::render_settings_word;
using sesh_ai::daw::render_source;
using sesh_ai::daw::render_track;
using sesh_ai::daw::RenderHost;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::time_span;
using sesh_ai::daw::tool_call;
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
using sesh_ai::daw::undo_report_of;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::unknown_tool_error;
using sesh_ai::daw::tools::register_render_tool;
using sesh_ai::daw::tools::render_payload_codec;
using sesh_ai::daw::tools::render_tool_name;

namespace
{
	// The payload type the framework is templated on. A plain struct, which is the
	// point of the template parameter — no JSON library is needed to drive any of this.
	struct test_payload
	{
		int queued_job_count = 0;
		std::vector<std::string> output_paths;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;

	// The already-validated tool input, as far as this suite is concerned. The
	// framework never reads it; the codec below does.
	struct scripted_render_input
	{
		render_source source = render_source::master_mix;
		render_bounds bounds = render_bounds::project;
		std::string region_guid;
		render_output_format output_format = render_output_format::wav;

		// Deliberately not carrying `confirmedOverwrite`. The framework extracts it
		// onto the call, and the handler reads it from there — if it ever read it from
		// here instead, this struct would have to grow the field and the test below
		// asserting the retry queues would fail.
	};

	// A scripted undo stack that counts everything a tool could do to it. `render`
	// should do none of it.
	class counting_undo_stack final : public undo_stack
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

		void begin_block() override { ++begin_block_call_count; }

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
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

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class empty_track_list final : public TrackListSource
	{
	public:
		std::vector<ResolvableTrack> tracks_in_project_order() const override { return {}; }
	};

	// Answers from data and counts what was queued. Only the parts of `RenderHost`
	// these tests depend on carry state; the rest is present because the interface
	// requires it.
	class scripted_render_host final : public RenderHost
	{
	public:
		render_project_state project_state{};
		std::vector<render_track> selected_tracks{};
		std::optional<time_span> region_span{};
		std::string render_directory{"/Users/producer/Music/Session/renders"};
		std::string render_targets{};
		std::vector<std::string> existing_files{};

		int queue_additions = 0;
		std::vector<std::string> soloed_track_guids{};

		render_project_state read_project_state() override { return project_state; }

		std::vector<render_track> read_selected_tracks() override { return selected_tracks; }

		std::optional<time_span> find_region_span(const std::string&) override { return region_span; }

		std::string read_render_directory() override { return render_directory; }

		void write_render_settings(render_settings_word) override {}

		void write_render_bounds(int, const time_span&) override {}

		void write_render_sample_rate(int) override {}

		void write_render_format(const render_format_request&) override {}

		void write_render_output(const std::string&, const std::string&) override {}

		std::string read_render_targets() override { return render_targets; }

		bool output_file_exists(const std::string& resolved_path) override
		{
			return std::find(existing_files.begin(), existing_files.end(), resolved_path)
				!= existing_files.end();
		}

		void set_track_solo(const std::string& track_guid, bool soloed) override
		{
			if (soloed)
			{
				soloed_track_guids.push_back(track_guid);
			}
		}

		void add_project_to_render_queue() override { ++queue_additions; }
	};

	// A project with something to render, so the interesting refusals are the ones the
	// test arranges rather than an empty session.
	scripted_render_host host_with_a_renderable_session()
	{
		scripted_render_host host;
		host.project_state.project_length_seconds = 180.0;
		host.project_state.sample_rate = 48000;
		host.render_targets = "/Users/producer/Music/Session/renders/Session.wav";

		return host;
	}

	// The codec the real build binds to `nlohmann::json`, bound here to the scripted
	// structs.
	render_payload_codec<test_payload> scripted_codec()
	{
		render_payload_codec<test_payload> codec;

		codec.read_render_request = [](const test_payload&) {
			// The suite drives the request through `scripted_render_input` below rather
			// than through the payload, so this branch is only reached by the tests that
			// do not care what was requested.
			render_request request;
			request.source = render_source::master_mix;
			request.bounds = render_bounds::project;

			return request;
		};

		codec.write_render_result = [](const render_result& result) {
			test_payload payload;
			payload.queued_job_count = result.queued_job_count;
			payload.output_paths = result.output_paths;

			return payload;
		};

		return codec;
	}

	// A codec whose reader answers from a scripted input, which is what lets one test
	// ask for a region render and another for a master mix.
	render_payload_codec<test_payload> codec_reading(const scripted_render_input& input)
	{
		render_payload_codec<test_payload> codec = scripted_codec();

		codec.read_render_request = [&input](const test_payload&) {
			render_request request;
			request.source = input.source;
			request.bounds = input.bounds;
			request.region_guid = input.region_guid;
			request.output_format = input.output_format;

			return request;
		};

		return codec;
	}

	const test_result& result_of(const test_outcome& outcome)
	{
		REQUIRE(std::holds_alternative<test_result>(outcome));

		return std::get<test_result>(outcome);
	}

	const tool_success<test_payload>& success_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);
		REQUIRE(std::holds_alternative<tool_success<test_payload>>(result));

		return std::get<tool_success<test_payload>>(result);
	}

	const tool_refusal& refusal_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);
		REQUIRE(std::holds_alternative<tool_refusal>(result));

		return std::get<tool_refusal>(result);
	}

	const tool_partial_outcome<test_payload>& partial_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);
		REQUIRE(std::holds_alternative<tool_partial_outcome<test_payload>>(result));

		return std::get<tool_partial_outcome<test_payload>>(result);
	}

	// Everything one dispatch needs, assembled so each test says only what it is
	// about.
	struct render_tool_fixture
	{
		test_registry registry;
		counting_undo_stack stack;
		undo_manager undo{stack};
		empty_track_list tracks;
		NoLearnedAliases aliases;
		test_executor executor{registry, undo, tracks, aliases};
		test_payload validated_input{};

		test_outcome dispatch(bool confirmed_overwrite = false)
		{
			test_call call;
			call.tool_name = std::string{render_tool_name};
			call.request_id = "request-render";
			call.validated_input = &validated_input;
			call.confirmed_overwrite = confirmed_overwrite;

			return executor.execute(call);
		}
	};
}

// ---------------------------------------------------------------------------
// Requirement 12.6 — the registration path that opens nothing
// ---------------------------------------------------------------------------

TEST_CASE("render registers with an undo effect of none", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, scripted_codec())
		== tool_registration_outcome::registered);

	// The one assertion `tool_executor_test.cpp` already makes about `render`, restated
	// here because this file is what would break it: registering through the mutating
	// path is the plausible mistake, and it would change this value.
	const auto* const registered = fixture.registry.find_tool(render_tool_name);
	REQUIRE(registered != nullptr);
	CHECK(registered->undo_effect() == tool_undo_effect::none);
}

TEST_CASE("a queued render reports what was queued and carries no undo report", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, scripted_codec())
		== tool_registration_outcome::registered);

	const test_outcome outcome = fixture.dispatch();

	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.tool_name == std::string{render_tool_name});
	CHECK(success.fields.queued_job_count == 1);
	CHECK(host.queue_additions == 1);

	// Requirement 12.6, asserted as the absence the output schema enforces. A result
	// carrying an undo report would fail the extension's own outbound validation,
	// because `outputs/render.schema.json` has no such property and forbids
	// additional ones.
	CHECK_FALSE(success.undo.has_value());
	CHECK_FALSE(undo_report_of(result_of(outcome)).has_value());

	// No block was opened and no marker was captured, so a turn whose only action is
	// this render has no range for "revert all" to walk.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.stack.end_block_call_count == 0);
	CHECK_FALSE(fixture.undo.has_undo_position_marker());
}

// ---------------------------------------------------------------------------
// Requirement 12.4 and 9.8 — the collision refusal, in both forms
// ---------------------------------------------------------------------------

TEST_CASE(
	"an existing output file refuses the render with confirmedOverwrite named",
	"[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	host.existing_files.push_back("/Users/producer/Music/Session/renders/Session.wav");

	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, scripted_codec())
		== tool_registration_outcome::registered);

	const test_outcome outcome = fixture.dispatch();

	const tool_refusal& refusal = refusal_of(outcome);

	// The reason travels unchanged from the coordinator, and so does the
	// acknowledgement field — rewriting either would have the agent offering the
	// producer something else to confirm.
	CHECK(refusal.reason == std::string{output_file_collision_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{overwrite_acknowledgement_field});

	// The specific file, because a producer approving an overwrite needs to see which.
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].kind == "file_path");
	CHECK(refusal.blocking[0].description == "/Users/producer/Music/Session/renders/Session.wav");

	// Nothing was queued, and nothing was opened.
	CHECK(host.queue_additions == 0);
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK_FALSE(fixture.undo.has_undo_position_marker());
}

TEST_CASE("a self-collision refuses the render and names no acknowledgement", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	host.selected_tracks = {
		render_track{"{00000000-0000-0000-0000-0000000000AA}", "Kick"},
		render_track{"{00000000-0000-0000-0000-0000000000BB}", "Snare"},
	};

	// REAPER resolving two of this render's own jobs to one path. The pattern that
	// produced it is not this suite's business — what matters here is that the refusal
	// arrives with no field to set, because approving it would mean approving that one
	// of the two files is silently not written.
	host.render_targets =
		"/Users/producer/Music/Session/renders/Session.wav;"
		"/Users/producer/Music/Session/renders/Session.wav";

	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	const scripted_render_input stems_request{render_source::stems, render_bounds::project, {},
		render_output_format::wav};

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, codec_reading(stems_request))
		== tool_registration_outcome::registered);

	const test_outcome outcome = fixture.dispatch();

	const tool_refusal& refusal = refusal_of(outcome);
	CHECK(refusal.reason == std::string{output_file_collision_refusal_reason});
	CHECK(refusal.acknowledgement_field.empty());
	CHECK_FALSE(refusal.blocking.empty());
	CHECK(host.queue_additions == 0);
}

TEST_CASE(
	"an approved overwrite reaches the coordinator and the retry queues",
	"[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	host.existing_files.push_back("/Users/producer/Music/Session/renders/Session.wav");

	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, scripted_codec())
		== tool_registration_outcome::registered);

	// The same call that refused above, retried with the acknowledgement the refusal
	// named. It arrives on the call rather than in the payload, which is the wiring
	// this checks: the handler reads `tool_call::confirmed_overwrite` and puts it on
	// the request.
	const test_outcome outcome = fixture.dispatch(true);

	CHECK(success_of(outcome).fields.queued_job_count == 1);
	CHECK(host.queue_additions == 1);
	CHECK_FALSE(undo_report_of(result_of(outcome)).has_value());
}

// ---------------------------------------------------------------------------
// Requirement 9.5 — a failure is not a refusal
// ---------------------------------------------------------------------------

TEST_CASE("a call the coordinator cannot make sense of fails with a reason", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();

	// No region carries the GUID, so there is no span to render. Nothing the producer
	// could acknowledge, so it is a failed action rather than a refusal.
	host.region_span.reset();

	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	const scripted_render_input region_request{render_source::master_mix, render_bounds::region,
		"{00000000-0000-0000-0000-0000000000CC}", render_output_format::wav};

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, codec_reading(region_request))
		== tool_registration_outcome::registered);

	const test_outcome outcome = fixture.dispatch();

	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);
	REQUIRE(partial.actions.size() == 1);
	CHECK(partial.failed_action_count() == 1);

	// Requirement 9.5: the reason is not optional, and it is the coordinator's own
	// words rather than a generic one invented here.
	const action_failed& failure = std::get<action_failed>(partial.actions[0]);
	CHECK(failure.error.message().find("{00000000-0000-0000-0000-0000000000CC}") != std::string::npos);

	CHECK(host.queue_additions == 0);
	CHECK_FALSE(partial.undo.has_value());
}

TEST_CASE("a render with no result writer fails rather than reporting a queued job", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	// The half of the codec the real build binds to `nlohmann::json`, left empty. A
	// success nobody can serialise has to be a failure carrying a reason, not a
	// default-constructed payload claiming a render was queued.
	render_payload_codec<test_payload> codec = scripted_codec();
	codec.write_render_result = {};

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, codec)
		== tool_registration_outcome::registered);

	const test_outcome outcome = fixture.dispatch();

	CHECK(partial_of(outcome).failed_action_count() == 1);
	CHECK(host.queue_additions == 0);
}

// ---------------------------------------------------------------------------
// Requirement 23.6 — a name this build does not implement
// ---------------------------------------------------------------------------

TEST_CASE("an unknown tool name never reaches the render handler", "[render_tool]")
{
	scripted_render_host host = host_with_a_renderable_session();
	render_coordinator coordinator{host};
	render_tool_fixture fixture;

	REQUIRE(
		register_render_tool(fixture.registry, coordinator, scripted_codec())
		== tool_registration_outcome::registered);

	test_call call;
	call.tool_name = "render_the_whole_album";
	call.validated_input = &fixture.validated_input;

	const test_outcome outcome = fixture.executor.execute(call);

	REQUIRE(std::holds_alternative<unknown_tool_error>(outcome));
	CHECK(std::get<unknown_tool_error>(outcome).tool_name == "render_the_whole_album");

	// Dispatch stopped at the name, so nothing was queued and nothing was resolved.
	CHECK(host.queue_additions == 0);
}
