// `list_tempo_changes` (task 20.1, requirement 25.1).
//
// Three states these tests exist to try to reach, and all three are ways of reporting a
// tempo map that is not the one the producer is working in:
//
//   - An empty result for a session that never changes tempo.
//     `CountTempoTimeSigMarkers` counts markers and a project's own tempo is not a
//     marker, so REAPER's list really is empty for the overwhelmingly common session —
//     and `list-tempo-changes.schema.json` says that case returns one entry at position
//     zero carrying the project tempo, not nothing. An empty answer there is the tool
//     reporting "the tempo does nothing anywhere" about a session running at 140.
//   - A time signature inherited rather than resolved, or resolved from the wrong side
//     of the window. `tempoChangeEntry` requires `timeSignature` on every entry because
//     an agent reading one entry in isolation cannot walk backwards, and an entry inside
//     the window can inherit from a point outside it — so filtering before resolving
//     reports 4/4 for a passage written in 7/8.
//   - A capped read reported as the whole map. Requirement 23.9's silent truncation,
//     which this output schema carries `totalInRange` and `truncated` to avoid.
//
// The range check is not re-derived here, for the reason `marker_region_tools_test.cpp`
// gives: `check_end_after_start` is the Tool Executor's own and is already swept over
// 1156 pairs of doubles. What is asserted is that this tool routes through it.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/tempo_tools.h>

using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::invalid_time_range_code;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;

using sesh_ai::daw::tools::apply_list_tempo_changes;
using sesh_ai::daw::tools::initial_tempo_and_time_signature;
using sesh_ai::daw::tools::list_tempo_changes_result;
using sesh_ai::daw::tools::list_tempo_changes_tool_name;
using sesh_ai::daw::tools::marker_or_region;
using sesh_ai::daw::tools::marker_or_region_creation;
using sesh_ai::daw::tools::marker_or_region_write;
using sesh_ai::daw::tools::marker_region_outcome;
using sesh_ai::daw::tools::MarkerRegionHost;
using sesh_ai::daw::tools::maximum_listed_tempo_changes;
using sesh_ai::daw::tools::project_tempo_unavailable_code;
using sesh_ai::daw::tools::register_tempo_tools;
using sesh_ai::daw::tools::resolved_tempo_map;
using sesh_ai::daw::tools::tempo_change_entry;
using sesh_ai::daw::tools::tempo_map_point;
using sesh_ai::daw::tools::tempo_payload_codec;
using sesh_ai::daw::tools::tempo_time_signature;
using sesh_ai::daw::tools::the_requested_span_target;
using sesh_ai::daw::tools::timeline_read_window;

namespace
{
	// ---------------------------------------------------------------------------
	// A scripted session, narrowed to what this tool reads
	// ---------------------------------------------------------------------------

	// `list_tempo_changes` uses two of `MarkerRegionHost`'s ten operations — the tempo
	// map and the project's own tempo — so the other eight answer as an empty session
	// rather than being scripted. A tool reaching one of them would be reading REAPER a
	// second way, which is what this seam exists to prevent, and an empty answer is the
	// one that makes that visible rather than plausible.
	class scripted_tempo_session final : public MarkerRegionHost
	{
	public:
		scripted_tempo_session() = default;

		explicit scripted_tempo_session(std::vector<tempo_map_point> map)
			: tempo_map{std::move(map)}
		{
		}

		std::vector<marker_or_region> read_markers_and_regions_in_enumeration_order() override
		{
			return {};
		}

		std::optional<marker_or_region> create_marker_or_region(
			const marker_or_region_creation&) override
		{
			return std::nullopt;
		}

		bool write_marker_or_region(const marker_or_region_write&) override { return false; }
		bool delete_marker_or_region(const std::string&) override { return false; }

		std::vector<tempo_map_point> read_tempo_map() override
		{
			++tempo_read_call_count;

			return tempo_map;
		}

		std::optional<initial_tempo_and_time_signature>
			read_initial_tempo_and_time_signature() override
		{
			++initial_read_call_count;

			if (!project_tempo_readable)
			{
				return std::nullopt;
			}

			return project_initial;
		}

		bool write_tempo_point(std::optional<std::size_t>, const tempo_map_point&) override
		{
			return false;
		}

		bool clear_tempo_map() override { return false; }
		std::vector<double> read_item_positions() override { return {}; }
		bool write_time_selection(double, double) override { return false; }

		std::vector<tempo_map_point> tempo_map;

		// A plain session until a test says otherwise.
		initial_tempo_and_time_signature project_initial{120.0, tempo_time_signature{4, 4}};
		bool project_tempo_readable = true;

		int tempo_read_call_count = 0;
		int initial_read_call_count = 0;
	};

	tempo_map_point point_at(
		double position_seconds,
		double beats_per_minute,
		std::optional<tempo_time_signature> time_signature = std::nullopt,
		bool linear_transition = false)
	{
		tempo_map_point point;
		point.position_seconds = position_seconds;
		point.beats_per_minute = beats_per_minute;
		point.time_signature = time_signature;
		point.linear_transition = linear_transition;

		return point;
	}

	const list_tempo_changes_result& entries_from(
		const marker_region_outcome<list_tempo_changes_result>& outcome)
	{
		REQUIRE(std::holds_alternative<list_tempo_changes_result>(outcome));

		return std::get<list_tempo_changes_result>(outcome);
	}

	// The whole timeline, which is what omitting every parameter means.
	timeline_read_window whole_timeline()
	{
		timeline_read_window window;
		window.maximum_results = maximum_listed_tempo_changes;

		return window;
	}

	std::vector<double> positions_of(const std::vector<tempo_change_entry>& entries)
	{
		std::vector<double> positions;

		for (const tempo_change_entry& entry : entries)
		{
			positions.push_back(entry.position_seconds);
		}

		return positions;
	}
}

// ---------------------------------------------------------------------------
// The project's own tempo is an entry, not an absence
// ---------------------------------------------------------------------------

TEST_CASE("a session that never changes tempo reports one entry carrying the project tempo", "[daw][tools][tempo][reads]")
{
	// `CountTempoTimeSigMarkers` reports zero for this session, because the project's
	// tempo and time signature are not tempo markers. Reporting an empty list would
	// answer "what does the tempo do across this timeline" with nothing, about a session
	// running at a perfectly definite 140 BPM.
	scripted_tempo_session session;
	session.project_initial = initial_tempo_and_time_signature{140.0, tempo_time_signature{3, 4}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(result.tempo_changes.size() == 1);
	REQUIRE(result.tempo_changes.front().position_seconds == 0.0);
	REQUIRE(result.tempo_changes.front().beats_per_minute == 140.0);
	REQUIRE(result.tempo_changes.front().time_signature == tempo_time_signature{3, 4});

	// The project's tempo does not ramp into a first marker that is not there.
	REQUIRE_FALSE(result.tempo_changes.front().linear_transition);

	// A complete answer, so it is not reported as capped.
	REQUIRE(result.total_in_range == 1);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("an empty tempo map is a complete answer and never a failure", "[daw][tools][tempo][reads]")
{
	scripted_tempo_session session;

	marker_region_outcome<list_tempo_changes_result> outcome =
		apply_list_tempo_changes(session, whole_timeline());

	// Not a failed action. "The tempo reported in the project context snapshot holds for
	// the whole timeline" is something this tool can say, and saying it as a failure
	// would have the agent retrying a read that worked.
	REQUIRE_FALSE(std::holds_alternative<handler_action_outcomes>(outcome));
	REQUIRE(std::get<list_tempo_changes_result>(outcome).tempo_changes.size() == 1);
}

TEST_CASE("a map whose first point is already at the start is not given a second entry there", "[daw][tools][tempo][reads]")
{
	// A tempo marker at position zero already describes the start of the timeline.
	// Synthesising beside it would put two entries at one position, which REAPER itself
	// will not hold and which the agent cannot address apart.
	scripted_tempo_session session{{
		point_at(0.0, 95.0, tempo_time_signature{6, 8}),
		point_at(60.0, 120.0),
	}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(result.tempo_changes.size() == 2);
	REQUIRE(positions_of(result.tempo_changes) == std::vector<double>{0.0, 60.0});

	// The marker's tempo, not the project's.
	REQUIRE(result.tempo_changes.front().beats_per_minute == 95.0);
}

TEST_CASE("a point a nanosecond from the start counts as describing the start", "[daw][tools][tempo][reads]")
{
	// The same tolerance `change_tempo_map` uses to decide whether a write replaces a
	// point or inserts beside it. Two answers to "is there a point here" would have a
	// read reporting an entry a write then overwrote.
	scripted_tempo_session session{{point_at(1e-10, 95.0, tempo_time_signature{4, 4})}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(result.tempo_changes.size() == 1);
	REQUIRE(result.tempo_changes.front().beats_per_minute == 95.0);
}

TEST_CASE("the project tempo is prepended to a map that starts later", "[daw][tools][tempo][reads]")
{
	scripted_tempo_session session{{point_at(60.0, 150.0, tempo_time_signature{4, 4})}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(positions_of(result.tempo_changes) == std::vector<double>{0.0, 60.0});
	REQUIRE(result.tempo_changes.front().beats_per_minute == 120.0);
	REQUIRE(result.tempo_changes.back().beats_per_minute == 150.0);
	REQUIRE(result.total_in_range == 2);
}

// ---------------------------------------------------------------------------
// Time signatures are resolved, not inherited
// ---------------------------------------------------------------------------

TEST_CASE("a tempo point carrying no time signature reports the one in force", "[daw][tools][tempo][reads]")
{
	// REAPER lets a tempo marker change the tempo without changing the signature, which
	// is why `tempo_map_point::time_signature` is an optional. `tempoChangeEntry`
	// requires it on every entry, so the walk happens once here rather than in the
	// agent's head.
	scripted_tempo_session session{{
		point_at(30.0, 130.0, tempo_time_signature{7, 8}),
		point_at(60.0, 140.0),
		point_at(90.0, 150.0),
		point_at(120.0, 160.0, tempo_time_signature{4, 4}),
	}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(result.tempo_changes.size() == 5);

	// The synthesised entry at zero carries the project's signature.
	REQUIRE(result.tempo_changes[0].time_signature == tempo_time_signature{4, 4});
	REQUIRE(result.tempo_changes[1].time_signature == tempo_time_signature{7, 8});

	// Two points that change only the tempo. Both report 7/8, which is the bar length a
	// producer is actually playing in at 60 and 90 seconds.
	REQUIRE(result.tempo_changes[2].time_signature == tempo_time_signature{7, 8});
	REQUIRE(result.tempo_changes[3].time_signature == tempo_time_signature{7, 8});
	REQUIRE(result.tempo_changes[4].time_signature == tempo_time_signature{4, 4});
}

TEST_CASE("an entry inherits its time signature from a point outside the window", "[daw][tools][tempo][reads]")
{
	// The resolution runs over the whole map before the window is applied. Filtering
	// first would leave the entry at 90 seconds with nothing earlier to inherit from,
	// and it would report the project's 4/4 for a passage written in 7/8.
	scripted_tempo_session session{{
		point_at(30.0, 130.0, tempo_time_signature{7, 8}),
		point_at(90.0, 150.0),
	}};

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 60.0;

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, window));

	REQUIRE(result.tempo_changes.size() == 1);
	REQUIRE(result.tempo_changes.front().position_seconds == 90.0);
	REQUIRE(result.tempo_changes.front().time_signature == tempo_time_signature{7, 8});
}

TEST_CASE("the linear transition flag travels per entry", "[daw][tools][tempo][reads]")
{
	scripted_tempo_session session{{
		point_at(30.0, 130.0, tempo_time_signature{4, 4}, true),
		point_at(60.0, 140.0, std::nullopt, false),
	}};

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, whole_timeline()));

	REQUIRE(result.tempo_changes.size() == 3);
	REQUIRE_FALSE(result.tempo_changes[0].linear_transition);
	REQUIRE(result.tempo_changes[1].linear_transition);
	REQUIRE_FALSE(result.tempo_changes[2].linear_transition);
}

TEST_CASE("a tempo and a time signature outside the schema's ranges are bounded", "[daw][tools][tempo][reads]")
{
	// `project_context_builder.h`'s own bounding, reused rather than rewritten: the
	// snapshot reports a tempo against the same range, and a second clamp could disagree
	// with the first about one session. A result outside the range is a payload the
	// extension's own outbound validation refuses (requirement 4.1), which the producer
	// experiences as the tool silently not working.
	const std::vector<tempo_change_entry> resolved = resolved_tempo_map(
		{point_at(30.0, 5000.0, tempo_time_signature{200, 5})},
		initial_tempo_and_time_signature{0.0, tempo_time_signature{0, 0}});

	REQUIRE(resolved.size() == 2);

	// A zero tempo falls to the schema minimum of 1 BPM — wrong in a way somebody
	// reports, rather than a plausible 120 that nobody ever finds.
	REQUIRE(resolved.front().beats_per_minute == 1.0);
	REQUIRE(resolved.front().time_signature == tempo_time_signature{1, 1});

	REQUIRE(resolved.back().beats_per_minute == 960.0);
	REQUIRE(resolved.back().time_signature == tempo_time_signature{64, 4});
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

TEST_CASE("a tempo entry on either edge of the window is inside it", "[daw][tools][tempo][reads]")
{
	scripted_tempo_session session{{
		point_at(0.0, 100.0, tempo_time_signature{4, 4}),
		point_at(30.0, 110.0),
		point_at(60.0, 120.0),
		point_at(90.0, 130.0),
	}};

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 30.0;
	window.end_position_seconds = 60.0;

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, window));

	REQUIRE(positions_of(result.tempo_changes) == std::vector<double>{30.0, 60.0});
	REQUIRE(result.total_in_range == 2);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("a window past every entry is an empty result rather than a failure", "[daw][tools][tempo][reads]")
{
	// Different from an empty map: this window genuinely contains no entries, and
	// `totalInRange` of zero says so. The entry at position zero is not in range, and
	// reporting it anyway would answer a question the call did not ask.
	scripted_tempo_session session;

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 100.0;

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, window));

	REQUIRE(result.tempo_changes.empty());
	REQUIRE(result.total_in_range == 0);
	REQUIRE_FALSE(result.truncated);
}

TEST_CASE("an inverted window is a failed action and nothing is read", "[daw][tools][tempo][reads]")
{
	scripted_tempo_session session;

	timeline_read_window window = whole_timeline();
	window.start_position_seconds = 100.0;
	window.end_position_seconds = 10.0;

	marker_region_outcome<list_tempo_changes_result> outcome =
		apply_list_tempo_changes(session, window);

	const auto* const outcomes = std::get_if<handler_action_outcomes>(&outcome);

	REQUIRE(outcomes != nullptr);
	REQUIRE(outcomes->actions.size() == 1);
	REQUIRE_FALSE(action_was_applied(outcomes->actions.front()));

	const auto* const failure = std::get_if<action_failed>(&outcomes->actions.front());

	REQUIRE(failure != nullptr);
	REQUIRE(failure->error.code() == invalid_time_range_code);
	REQUIRE(failure->target == the_requested_span_target);

	// Before anything was read, so an empty map cannot be mistaken for a session that
	// never changes tempo.
	REQUIRE(session.tempo_read_call_count == 0);
	REQUIRE(session.initial_read_call_count == 0);
}

// ---------------------------------------------------------------------------
// Requirement 23.9: cap and report
// ---------------------------------------------------------------------------

TEST_CASE("a capped tempo read reports the total and says it was capped", "[daw][tools][tempo][reads]")
{
	std::vector<tempo_map_point> map;

	for (int number = 1; number <= 40; ++number)
	{
		map.push_back(point_at(static_cast<double>(number), 120.0, tempo_time_signature{4, 4}));
	}

	scripted_tempo_session session{map};

	timeline_read_window window;
	window.maximum_results = 10;

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, window));

	REQUIRE(result.tempo_changes.size() == 10);

	// 41 and not 40: the synthesised entry at position zero is an entry the call would
	// have been given, so it is counted.
	REQUIRE(result.total_in_range == 41);
	REQUIRE(result.truncated);

	// In position order from the start, which is what makes the continuation the input
	// schema describes work.
	REQUIRE(result.tempo_changes.front().position_seconds == 0.0);
	REQUIRE(result.tempo_changes.back().position_seconds == 9.0);
}

TEST_CASE("the output schema's maxItems bounds a tempo read asking for more", "[daw][tools][tempo][reads]")
{
	std::vector<tempo_map_point> map;

	for (int number = 1; number <= 150; ++number)
	{
		map.push_back(point_at(static_cast<double>(number), 120.0, tempo_time_signature{4, 4}));
	}

	scripted_tempo_session session{map};

	timeline_read_window window;
	window.maximum_results = 500;

	const list_tempo_changes_result result =
		entries_from(apply_list_tempo_changes(session, window));

	REQUIRE(result.tempo_changes.size() == maximum_listed_tempo_changes);
	REQUIRE(result.total_in_range == 151);
	REQUIRE(result.truncated);
}

// ---------------------------------------------------------------------------
// The one failure beyond the window
// ---------------------------------------------------------------------------

TEST_CASE("a project tempo REAPER will not report is a failed action naming why", "[daw][tools][tempo][reads]")
{
	// Every entry's time signature is resolved from the project's, and the entry at
	// position zero is composed from the project's tempo. A fabricated 120 in 4/4 would
	// have the agent writing tempo points against a map it was told the wrong shape of —
	// and `change_tempo_map` moves every beat-based item in the session.
	scripted_tempo_session session{{point_at(30.0, 130.0)}};
	session.project_tempo_readable = false;

	marker_region_outcome<list_tempo_changes_result> outcome =
		apply_list_tempo_changes(session, whole_timeline());

	const auto* const outcomes = std::get_if<handler_action_outcomes>(&outcome);

	REQUIRE(outcomes != nullptr);
	REQUIRE(outcomes->actions.size() == 1);

	const auto* const failure = std::get_if<action_failed>(&outcomes->actions.front());

	REQUIRE(failure != nullptr);
	REQUIRE(failure->error.code() == project_tempo_unavailable_code);
	REQUIRE(failure->target == list_tempo_changes_tool_name);

	// The map is not read either, so nothing is reported from a session half of which
	// could not be described.
	REQUIRE(session.tempo_read_call_count == 0);
}

// ---------------------------------------------------------------------------
// Registration through the Tool Executor's seam
// ---------------------------------------------------------------------------

namespace
{
	struct test_payload
	{
		std::string described_result;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_context = tool_execution_context<test_payload>;

	tempo_payload_codec<test_payload> codec_for(timeline_read_window window)
	{
		tempo_payload_codec<test_payload> codec;

		codec.read_list_tempo_changes_request = [window](const test_context&) { return window; };
		codec.write_list_tempo_changes_result = [](const list_tempo_changes_result& result) {
			return test_payload{
				"listed " + std::to_string(result.tempo_changes.size()) + " tempo entries"};
		};

		return codec;
	}

	class scripted_undo_stack final : public undo_stack
	{
	public:
		int current_position() override { return 0; }
		std::string entry_description_at(int) override { return {}; }

		void begin_block() override { ++begin_block_call_count; }
		void end_block(const std::string&, int) override { ++end_block_call_count; }
		bool undo_one_entry() override { return false; }

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
	};

	class empty_track_list final : public TrackListSource
	{
	public:
		std::vector<ResolvableTrack> tracks_in_project_order() const override { return {}; }
	};
}

TEST_CASE("list_tempo_changes registers as a read tool with no undo effect", "[daw][tools][tempo][reads][registration]")
{
	test_registry registry;
	scripted_tempo_session session;

	const auto report = register_tempo_tools(registry, session, codec_for(whole_timeline()));

	REQUIRE(report.every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 1);

	const auto* const tool = registry.find_tool(list_tempo_changes_tool_name);

	REQUIRE(tool != nullptr);
	REQUIRE(tool->undo_effect() == tool_undo_effect::none);
}

TEST_CASE("an incomplete tempo codec registers nothing", "[daw][tools][tempo][reads][registration]")
{
	test_registry registry;
	scripted_tempo_session session;

	tempo_payload_codec<test_payload> codec = codec_for(whole_timeline());
	codec.write_list_tempo_changes_result = {};

	REQUIRE_FALSE(register_tempo_tools(registry, session, codec).every_tool_registered());
	REQUIRE(registry.registered_tool_count() == 0);
}

TEST_CASE("a dispatched list_tempo_changes comes back as a success with no undo report", "[daw][tools][tempo][reads][registration]")
{
	test_registry registry;
	scripted_tempo_session session{{point_at(60.0, 150.0, tempo_time_signature{4, 4})}};

	REQUIRE(register_tempo_tools(registry, session, codec_for(whole_timeline()))
		.every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{list_tempo_changes_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	REQUIRE(success->tool_name == list_tempo_changes_tool_name);

	// The project's own tempo and the one marker.
	REQUIRE(success->fields.described_result == "listed 2 tempo entries");

	// No block opened, so no undo position is recorded for a call that changed nothing.
	REQUIRE_FALSE(success->undo.has_value());
	REQUIRE(stack.begin_block_call_count == 0);
	REQUIRE(stack.end_block_call_count == 0);
}

TEST_CASE("a dispatched tempo read with an inverted window is a partial and not a refusal", "[daw][tools][tempo][reads][registration]")
{
	// `result_schema_path_for` routes a `tool_partial_outcome` to
	// `messages/tool-result-partial.schema.json` rather than to this tool's own output
	// schema, which requires `tempoChanges`, `totalInRange`, and `truncated` and is
	// closed. Without that routing a read tool's only failure shape would be refused by
	// the extension's own outbound validation.
	test_registry registry;
	scripted_tempo_session session;

	timeline_read_window inverted = whole_timeline();
	inverted.start_position_seconds = 100.0;
	inverted.end_position_seconds = 10.0;

	REQUIRE(register_tempo_tools(registry, session, codec_for(inverted)).every_tool_registered());

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	NoLearnedAliases no_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, no_aliases};

	undo.begin_turn();

	tool_call<test_payload> call;
	call.tool_name = std::string{list_tempo_changes_tool_name};

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE_FALSE(std::holds_alternative<tool_refusal>(*result));

	REQUIRE(sesh_ai::daw::result_schema_path_for(outcome)
		== sesh_ai::transport::tool_result_partial_contract_schema_path);

	REQUIRE(stack.begin_block_call_count == 0);
}
