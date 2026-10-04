// Tool Executor framework — dispatch, target resolution, undo block wrapping, and the
// three result shapes.
//
// The framework's job is to make four things impossible rather than merely unlikely,
// and the suite is built around trying to reach them:
//
//   - A refusal that recorded an undo block (requirements 9.3, 10.7).
//   - A failed action with no reason (requirement 9.5).
//   - More than one undo block for one call, or a block left open (requirements 9.4,
//     9.9, 10.8, 23.8).
//   - A successful action rolled back because a sibling failed (requirement 9.4).
//
// Two of those are enforced by the types and so are checked here by a static assertion
// or by the absence of a constructor rather than by a runtime expectation. The other
// two are control flow, and the scripted undo stack below is what turns them into
// assertions: it counts `Undo_BeginBlock2` and `Undo_EndBlock2`, tracks open depth, and
// can be told to throw from the close — none of which can be staged inside REAPER.
//
// The scripted DAW state is what makes "successes survive a sibling failure" a real
// check rather than a restatement of the result payload: the handler writes to it, the
// framework never touches it, and the test reads it after a partial outcome.
//
// Task 10.2 layers the formal property tests (21, 22, 28) on top. These are the
// examples and the exhaustive sweeps underneath them.

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tool_executor.h>

using sesh_ai::daw::action_error;
using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_succeeded;
using sesh_ai::daw::action_target;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::ambiguous_track_selector_acknowledgement_field;
using sesh_ai::daw::ambiguous_track_selector_refusal_reason;
using sesh_ai::daw::any_action_was_applied;
using sesh_ai::daw::bus_construction_plan;
using sesh_ai::daw::bus_type;
using sesh_ai::daw::check_end_after_start;
using sesh_ai::daw::count_applied_actions;
using sesh_ai::daw::count_failed_actions;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::evaluate_bus_construction;
using sesh_ai::daw::evaluate_send_creation;
using sesh_ai::daw::failed_action;
using sesh_ai::daw::foreign_undo_refusal;
using sesh_ai::daw::foreign_undo_refusal_reason;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::handler_success;
using sesh_ai::daw::invalid_time_range_code;
using sesh_ai::daw::LearnedAliasLookup;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::output_file_collision_refusal_reason;
using sesh_ai::daw::reaper_api_failure_code;
using sesh_ai::daw::refusal_from_foreign_undo;
using sesh_ai::daw::refusal_from_render_refusal;
using sesh_ai::daw::refusal_from_routing_decision;
using sesh_ai::daw::render_blocking_entity;
using sesh_ai::daw::render_refusal;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::ResolvedTrack;
using sesh_ai::daw::resolve_call_targets;
using sesh_ai::daw::result_schema_path_for;
using sesh_ai::daw::routing_graph;
using sesh_ai::daw::send_blocking_entity_kind;
using sesh_ai::daw::signal_cycle_acknowledgement_field;
using sesh_ai::daw::signal_cycle_refusal_reason;
using sesh_ai::daw::succeeded_action;
using sesh_ai::daw::time_range_is_well_formed;
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
using sesh_ai::daw::TrackGuidSelector;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::TrackNamePatternSelector;
using sesh_ai::daw::TrackSelector;
using sesh_ai::daw::track_node;
using sesh_ai::daw::undo_block_report;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_report;
using sesh_ai::daw::undo_report_of;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::unknown_tool_error;
using sesh_ai::daw::unknown_tool_error_code;
using sesh_ai::daw::unnamed_failure_code;
using sesh_ai::daw::unnamed_failure_message;
using sesh_ai::daw::unresolved_track_selector_refusal_reason;

namespace
{
	// The payload type the framework is templated on.
	//
	// A plain struct rather than nlohmann::json, which is the point of the template
	// parameter: the framework never reads a field, so the suite does not need a JSON
	// library to drive it, and an attempt to add input validation to the framework
	// would stop compiling here.
	struct test_payload
	{
		std::string described_result;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_context = tool_execution_context<test_payload>;
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

			if (open_block_depth > deepest_open_block_depth)
			{
				deepest_open_block_depth = open_block_depth;
			}
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;
			end_block_descriptions.push_back(description);

			if (throw_from_end_block)
			{
				throw std::runtime_error("REAPER refused to close the block");
			}

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

		void seed_entries(std::vector<std::string> descriptions)
		{
			entries_ = std::move(descriptions);
			position_ = static_cast<int>(entries_.size());
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		int deepest_open_block_depth = 0;
		bool throw_from_end_block = false;
		std::vector<std::string> end_block_descriptions;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	// A synthetic track list.
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

	// REAPER's braced GUID form, which the refusal schema's `guid` pattern expects.
	std::string guid_for(char distinguishing_character)
	{
		std::string guid{"{00000000-0000-0000-0000-0000000000"};
		guid.push_back(distinguishing_character);
		guid.push_back(distinguishing_character);
		guid.push_back('}');

		return guid;
	}

	ResolvableTrack track_named(const std::string& name, char distinguishing_character)
	{
		ResolvableTrack track;
		track.guid = guid_for(distinguishing_character);
		track.name = name;
		track.project_index = 0;

		return track;
	}

	TrackSelector by_name(std::string name_pattern)
	{
		return TrackNamePatternSelector{std::move(name_pattern)};
	}

	TrackSelector by_guid(std::string guid)
	{
		return TrackGuidSelector{std::move(guid)};
	}

	// What the handlers write to, and what the framework must never touch. This is how
	// "a successful action is not rolled back when a sibling fails" becomes an
	// assertion about the session rather than about the payload describing it.
	struct scripted_project_state
	{
		std::vector<std::string> applied_changes;
	};

	const test_result& result_of(const test_outcome& outcome)
	{
		REQUIRE(std::holds_alternative<test_result>(outcome));

		return std::get<test_result>(outcome);
	}

	const tool_refusal& refusal_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_refusal>(result));

		return std::get<tool_refusal>(result);
	}

	const tool_success<test_payload>& success_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_success<test_payload>>(result));

		return std::get<tool_success<test_payload>>(result);
	}

	const tool_partial_outcome<test_payload>& partial_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_partial_outcome<test_payload>>(result));

		return std::get<tool_partial_outcome<test_payload>>(result);
	}

	// One tool the framework can dispatch to, with somewhere to record that it ran.
	struct handler_call_record
	{
		int call_count = 0;
		std::size_t resolved_target_count = 0;
	};
}

// ---------------------------------------------------------------------------
// Requirement 23.6 — an unknown tool name
// ---------------------------------------------------------------------------

TEST_CASE("an unknown tool name returns an error result naming the tool", "[tool_executor]")
{
	test_registry registry;
	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "quantise_the_vibe";

	const test_outcome outcome = executor.execute(call);

	REQUIRE(std::holds_alternative<unknown_tool_error>(outcome));

	const unknown_tool_error& error = std::get<unknown_tool_error>(outcome);

	CHECK(error.tool_name == "quantise_the_vibe");
	CHECK(error.code == std::string{unknown_tool_error_code});

	// Naming it in the message is the requirement: a missing implementation has to be
	// visible rather than silent.
	CHECK(error.message.find("quantise_the_vibe") != std::string::npos);

	// And nothing was opened for a tool that does not exist.
	CHECK(stack.begin_block_call_count == 0);
	CHECK(undo.has_undo_position_marker() == false);
}

TEST_CASE("an unknown tool has no result schema to validate against", "[tool_executor]")
{
	test_call call;
	call.tool_name = "quantise_the_vibe";

	test_registry registry;
	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	const test_outcome outcome = executor.execute(call);

	// Empty rather than a derived path: there is no payload, so there is nothing to
	// validate, and a guessed path would turn requirement 23.6's case into a validator
	// that cannot find a file.
	CHECK(result_schema_path_for(outcome).has_value() == false);
}

// ---------------------------------------------------------------------------
// The registration seam
// ---------------------------------------------------------------------------

TEST_CASE("registration rejects a name that is not one of the 42 constrained tools", "[tool_executor]")
{
	test_registry registry;

	const tool_registration_outcome outcome = registry.register_read_tool(
		"invent_a_tool",
		[](const test_context&) { return handler_success<test_payload>{}; });

	CHECK(outcome == tool_registration_outcome::not_a_constrained_tool);
	CHECK(registry.registered_tool_count() == 0);
}

TEST_CASE("registration rejects a second handler for one tool", "[tool_executor]")
{
	test_registry registry;

	CHECK(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	CHECK(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::already_registered);

	CHECK(registry.registered_tool_count() == 1);
}

TEST_CASE("registration rejects an empty handler", "[tool_executor]")
{
	test_registry registry;

	CHECK(registry.register_read_tool("list_tracks", {})
		== tool_registration_outcome::no_handler_supplied);
	CHECK(registry.register_mutating_tool("set_track_state", {})
		== tool_registration_outcome::no_handler_supplied);
	CHECK(registry.registered_tool_count() == 0);
}

TEST_CASE("the registry reports which of the 42 tools this build has no handler for", "[tool_executor]")
{
	test_registry registry;

	// Before any handler lands, every one of the 42 is missing — which is what task
	// 10.1 leaves behind on purpose, and what the extension can report at startup
	// rather than discovering one unknown-tool error at a time.
	CHECK(registry.unregistered_constrained_tool_names().size() == 42);

	REQUIRE(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	const std::vector<std::string> missing = registry.unregistered_constrained_tool_names();

	CHECK(missing.size() == 41);

	for (const std::string& name : missing)
	{
		CHECK(name != "list_tracks");
	}
}

TEST_CASE("the registration call decides the undo effect", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	REQUIRE(registry.register_rewinding_tool(
		"revert_agent_changes",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	REQUIRE(registry.register_read_tool(
		"render",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	CHECK(registry.find_tool("list_tracks")->undo_effect() == tool_undo_effect::none);
	CHECK(registry.find_tool("set_track_state")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(registry.find_tool("revert_agent_changes")->undo_effect()
		== tool_undo_effect::rewinds_undo_stack);

	// Requirement 12.7: a queued render records no block and no marker, so it is
	// registered through the path that opens neither.
	CHECK(registry.find_tool("render")->undo_effect() == tool_undo_effect::none);
}

// ---------------------------------------------------------------------------
// Requirement 9.1 — resolve targets, then apply inside a block
// ---------------------------------------------------------------------------

TEST_CASE("a mutating tool succeeds inside exactly one identifiable undo block", "[tool_executor]")
{
	test_registry registry;
	handler_call_record record;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[&record](const test_context& context) {
			++record.call_count;
			record.resolved_target_count = context.resolved_targets.size();

			return handler_success<test_payload>{test_payload{"volume set"}};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";
	call.track_selectors.push_back(by_name("Lead Vox"));

	const test_outcome outcome = executor.execute(call);
	const tool_success<test_payload>& success = success_of(outcome);

	CHECK(record.call_count == 1);

	// Requirement 9.1: the target was resolved before the handler ran, and the handler
	// received it rather than a selector.
	CHECK(record.resolved_target_count == 1);

	CHECK(success.tool_name == "set_track_state");
	CHECK(success.fields.described_result == "volume set");

	// Requirement 9.2: tool-specific fields plus the undo report.
	REQUIRE(success.undo.has_value());
	CHECK(success.undo->undo_description == "Sesh AI: set_track_state");
	CHECK(success.undo->undo_position_before == 0);

	// One block, opened once, closed once, and not left open.
	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
	CHECK(stack.open_block_depth == 0);
	CHECK(stack.deepest_open_block_depth == 1);

	// And nothing was undone.
	CHECK(stack.undo_call_count == 0);
}

TEST_CASE("a read tool opens no block and reports no undo position", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) {
			return handler_success<test_payload>{test_payload{"two tracks"}};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "list_tracks";

	const test_outcome outcome = executor.execute(call);
	const tool_success<test_payload>& success = success_of(outcome);

	// Requirement 10.3: a read-only turn acquires no marker.
	CHECK(success.undo.has_value() == false);
	CHECK(undo.has_undo_position_marker() == false);
	CHECK(stack.begin_block_call_count == 0);
	CHECK(stack.end_block_call_count == 0);
}

TEST_CASE("the turn's marker is reported unchanged on every mutating result", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context&) { return handler_success<test_payload>{}; })
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;

	// The producer's session already has history, so the marker is not zero and a
	// default-constructed one would be visible.
	stack.seed_entries({"Move media items", "Change track volume"});

	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";

	const test_outcome first = executor.execute(call);
	const test_outcome second = executor.execute(call);

	// Requirements 10.1 and 10.2: captured before the first mutation, unchanged after.
	REQUIRE(success_of(first).undo.has_value());
	REQUIRE(success_of(second).undo.has_value());
	CHECK(success_of(first).undo->undo_position_before == 2);
	CHECK(success_of(second).undo->undo_position_before == 2);

	// Two calls, two blocks — one per call, not one per turn.
	CHECK(stack.begin_block_call_count == 2);
	CHECK(stack.end_block_call_count == 2);
}

// ---------------------------------------------------------------------------
// Requirements 9.3 and 10.7 — a refusal records nothing
// ---------------------------------------------------------------------------

TEST_CASE("an unresolved track selector refuses before the handler runs", "[tool_executor]")
{
	test_registry registry;
	handler_call_record record;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[&record](const test_context&) {
			++record.call_count;

			return handler_success<test_payload>{};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";
	call.track_selectors.push_back(by_name("Tuba"));

	const test_outcome outcome = executor.execute(call);
	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{unresolved_track_selector_refusal_reason});
	CHECK(refusal.refused == true);

	// Requirement 9.3 and requirement 10.7. No handler ran, no block was opened, no
	// marker was recorded — so a refusal mid-turn cannot shorten the range "revert
	// all" would walk.
	CHECK(record.call_count == 0);
	CHECK(stack.begin_block_call_count == 0);
	CHECK(stack.end_block_call_count == 0);
	CHECK(undo.has_undo_position_marker() == false);

	// And the type has nowhere to put an undo report even if something wanted to.
	CHECK(undo_report_of(result_of(outcome)).has_value() == false);
}

TEST_CASE("an ambiguous track selector names every candidate with its GUID", "[tool_executor]")
{
	test_registry registry;
	handler_call_record record;

	REQUIRE(registry.register_mutating_tool(
		"delete_track",
		[&record](const test_context&) {
			++record.call_count;

			return handler_success<test_payload>{};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Drums", 'a'), track_named("Drums", 'b')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "delete_track";
	call.track_selectors.push_back(by_name("Drums"));

	const test_outcome outcome = executor.execute(call);
	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{ambiguous_track_selector_refusal_reason});
	CHECK(refusal.acknowledgement_field
		== std::string{ambiguous_track_selector_acknowledgement_field});
	REQUIRE(refusal.blocking.size() == 2);
	CHECK(refusal.blocking[0].guid == guid_for('a'));
	CHECK(refusal.blocking[1].guid == guid_for('b'));

	CHECK(record.call_count == 0);
	CHECK(stack.begin_block_call_count == 0);
}

TEST_CASE("resolution stops at the first selector that does not resolve", "[tool_executor]")
{
	const std::vector<ResolvableTrack> tracks{track_named("Lead Vox", 'a')};
	NoLearnedAliases aliases;

	std::vector<TrackSelector> selectors;
	selectors.push_back(by_guid(guid_for('a')));
	selectors.push_back(by_name("Tuba"));
	selectors.push_back(by_guid(guid_for('a')));

	const sesh_ai::daw::target_resolution resolution =
		resolve_call_targets(tracks, aliases, selectors);

	// A tool that resolved two of three targets is not a tool that can run, so the
	// refusal is about the call rather than about one action.
	REQUIRE(std::holds_alternative<tool_refusal>(resolution));
	CHECK(std::get<tool_refusal>(resolution).reason
		== std::string{unresolved_track_selector_refusal_reason});
}

TEST_CASE("every selector resolves into the order the call named them", "[tool_executor]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named("Lead Vox", 'a'),
		track_named("Bass", 'b')
	};
	NoLearnedAliases aliases;

	std::vector<TrackSelector> selectors;
	selectors.push_back(by_name("Bass"));
	selectors.push_back(by_name("Lead Vox"));

	const sesh_ai::daw::target_resolution resolution =
		resolve_call_targets(tracks, aliases, selectors);

	REQUIRE(std::holds_alternative<std::vector<ResolvedTrack>>(resolution));

	const std::vector<ResolvedTrack>& resolved = std::get<std::vector<ResolvedTrack>>(resolution);

	REQUIRE(resolved.size() == 2);
	CHECK(resolved[0].reference.name == "Bass");
	CHECK(resolved[1].reference.name == "Lead Vox");
}

TEST_CASE("a mutating tool's precondition refusal opens no undo block", "[tool_executor]")
{
	test_registry registry;
	handler_call_record record;

	REQUIRE(registry.register_mutating_tool(
		"create_send",
		[&record](const test_context&) {
			++record.call_count;

			return handler_success<test_payload>{};
		},
		[](const test_context&) -> std::optional<tool_refusal> {
			// What `routing_tools.h` will do in task 10.5: build the projected graph
			// and ask the detector.
			routing_graph graph;
			graph.tracks_in_project_order.push_back(track_node{"a", 0, false});
			graph.tracks_in_project_order.push_back(track_node{"b", 0, false});
			graph.explicit_sends.push_back(sesh_ai::daw::send_edge{"b", "a"});

			return refusal_from_routing_decision(evaluate_send_creation(graph, "a", "b"));
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "create_send";

	const test_outcome outcome = executor.execute(call);
	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{signal_cycle_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{signal_cycle_acknowledgement_field});
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].kind == std::string{send_blocking_entity_kind});
	CHECK(refusal.blocking[0].description.empty() == false);

	// The precondition runs before the block exists, which is what makes requirement
	// 9.3 control flow rather than a rule to remember.
	CHECK(record.call_count == 0);
	CHECK(stack.begin_block_call_count == 0);
	CHECK(undo.has_undo_position_marker() == false);
}

TEST_CASE("an acyclic routing change is not refused", "[tool_executor]")
{
	routing_graph graph;
	graph.tracks_in_project_order.push_back(track_node{"a", 0, false});
	graph.tracks_in_project_order.push_back(track_node{"b", 0, false});

	CHECK(refusal_from_routing_decision(evaluate_send_creation(graph, "a", "b")).has_value() == false);

	bus_construction_plan plan;
	plan.type = bus_type::aux;
	plan.bus_track = "bus";
	plan.source_tracks = {"a", "b"};
	plan.bus_parent_send_enabled = false;

	CHECK(refusal_from_routing_decision(evaluate_bus_construction(graph, plan)).has_value() == false);
}

// ---------------------------------------------------------------------------
// Requirement 9.4 — one block per action array, and no rollback
// ---------------------------------------------------------------------------

TEST_CASE("an action array runs in one undo block and reports per-action outcomes", "[tool_executor]")
{
	test_registry registry;
	scripted_project_state project;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[&project](const test_context& context) {
			handler_action_outcomes produced;

			for (const ResolvedTrack& target : context.resolved_targets)
			{
				// The third track is the one REAPER will not accept, and the loop keeps
				// going past it — which is what requirement 9.4 asks for.
				if (target.reference.name == "Reverb Return")
				{
					produced.actions.push_back(failed_action(
						target.reference.guid,
						"reaper_rejected_value",
						"SetMediaTrackInfo_Value returned false for D_VOL"));

					continue;
				}

				project.applied_changes.push_back(target.reference.name);
				produced.actions.push_back(succeeded_action(target.reference.guid));
			}

			return produced;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{
		track_named("Lead Vox", 'a'),
		track_named("Bass", 'b'),
		track_named("Reverb Return", 'c'),
		track_named("Drums", 'd')
	}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";
	call.track_selectors.push_back(by_name("Lead Vox"));
	call.track_selectors.push_back(by_name("Bass"));
	call.track_selectors.push_back(by_name("Reverb Return"));
	call.track_selectors.push_back(by_name("Drums"));

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 4);
	CHECK(partial.applied_action_count() == 3);
	CHECK(partial.failed_action_count() == 1);

	// Requirement 9.4: the outcomes are in the order the actions were supplied.
	CHECK(action_was_applied(partial.actions[0]));
	CHECK(action_was_applied(partial.actions[1]));
	CHECK(action_was_applied(partial.actions[2]) == false);
	CHECK(action_was_applied(partial.actions[3]));

	// Requirement 10.8: one block covering whatever landed, not one per action.
	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
	CHECK(stack.open_block_depth == 0);

	// Requirement 9.4, the half that matters: nothing was rolled back. The project
	// still carries all three changes that landed, and the framework issued no undo.
	CHECK(project.applied_changes == std::vector<std::string>{"Lead Vox", "Bass", "Drums"});
	CHECK(stack.undo_call_count == 0);

	// Requirement 9.4 again: the partial carries the undo report, because something
	// landed and "revert all" needs a position.
	REQUIRE(partial.undo.has_value());
	CHECK(partial.undo->undo_description == "Sesh AI: set_track_state");
}

TEST_CASE("an action array where nothing landed reports no undo position", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context& context) {
			handler_action_outcomes produced;

			for (const ResolvedTrack& target : context.resolved_targets)
			{
				produced.actions.push_back(failed_action(
					target.reference.guid,
					"reaper_rejected_value",
					"SetMediaTrackInfo_Value returned false"));
			}

			return produced;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";
	call.track_selectors.push_back(by_name("Lead Vox"));

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	CHECK(partial.applied_action_count() == 0);

	// Nothing landed, so there is nothing for "revert all" to walk back to, and
	// offering a position anyway would take the producer past the turn's start into
	// their own work.
	CHECK(partial.undo.has_value() == false);
}

// ---------------------------------------------------------------------------
// Requirement 9.5 — every failure carries a reason
// ---------------------------------------------------------------------------

TEST_CASE("a failed action cannot be constructed without a reason", "[tool_executor]")
{
	// The mechanism, stated as a compile-time fact rather than as a runtime check: a
	// reasonless failure is not a value this program can hold.
	STATIC_REQUIRE(std::is_default_constructible_v<action_succeeded>);
	STATIC_REQUIRE(std::is_default_constructible_v<action_failed> == false);
	STATIC_REQUIRE(std::is_default_constructible_v<action_error> == false);

	// And the variant's default alternative is the success, so a default-constructed
	// outcome is not a failure with an empty reason either.
	STATIC_REQUIRE(std::is_default_constructible_v<action_outcome>);
	CHECK(action_was_applied(action_outcome{}));
}

TEST_CASE("an empty failure reason is substituted rather than sent as empty", "[tool_executor]")
{
	// Both fields have minLength 1 in the partial schema, so an empty one would fail
	// outbound validation. Substituted rather than dropped: a failure nobody can name
	// is still a failure the agent needs told about.
	const action_outcome outcome = failed_action("", "", "");

	REQUIRE(action_was_applied(outcome) == false);

	const action_failed& failed = std::get<action_failed>(outcome);

	CHECK(failed.target.empty() == false);
	CHECK(failed.error.code() == std::string{unnamed_failure_code});
	CHECK(failed.error.message() == std::string{unnamed_failure_message});
}

TEST_CASE("an over-long failure reason is clamped to the schema's bound", "[tool_executor]")
{
	const action_outcome outcome = failed_action(
		std::string(2048, 'g'),
		std::string(256, 'c'),
		std::string(4096, 'm'));

	const action_failed& failed = std::get<action_failed>(outcome);

	CHECK(failed.target.size() == 1024);
	CHECK(failed.error.code().size() == 128);
	CHECK(failed.error.message().size() == 1024);
}

TEST_CASE("the action tallies agree with the outcomes", "[tool_executor]")
{
	std::vector<action_outcome> actions;
	actions.push_back(succeeded_action("one"));
	actions.push_back(failed_action("two", "code", "message"));
	actions.push_back(succeeded_action("three"));

	CHECK(count_applied_actions(actions) == 2);
	CHECK(count_failed_actions(actions) == 1);
	CHECK(any_action_was_applied(actions));
	CHECK(action_target(actions[1]) == "two");

	CHECK(any_action_was_applied({}) == false);
}

// ---------------------------------------------------------------------------
// Requirement 9.6 — end strictly after start
// ---------------------------------------------------------------------------

TEST_CASE("a time range is accepted only when the end strictly follows the start", "[tool_executor]")
{
	CHECK(time_range_is_well_formed(0.0, 1.0));
	CHECK(time_range_is_well_formed(-4.5, -4.4));

	// Equal is not a range: a zero-length region is not a region.
	CHECK(time_range_is_well_formed(2.0, 2.0) == false);
	CHECK(time_range_is_well_formed(2.0, 1.0) == false);

	const std::optional<action_error> failure = check_end_after_start(2.0, 2.0);

	REQUIRE(failure.has_value());
	CHECK(failure->code() == std::string{invalid_time_range_code});
	CHECK(failure->message().empty() == false);
}

TEST_CASE("a non-finite time range is rejected", "[tool_executor]")
{
	const double not_a_number = std::numeric_limits<double>::quiet_NaN();

	// Every comparison against NaN is false, so this falls out on the refusing side,
	// which is the direction worth failing in.
	CHECK(time_range_is_well_formed(0.0, not_a_number) == false);
	CHECK(time_range_is_well_formed(not_a_number, 1.0) == false);
	CHECK(time_range_is_well_formed(not_a_number, not_a_number) == false);
}

TEST_CASE("a tool whose time range is inverted does not mutate the project", "[tool_executor]")
{
	test_registry registry;
	scripted_project_state project;

	REQUIRE(registry.register_mutating_tool(
		"set_time_selection",
		[&project](const test_context&) {
			// What `marker_region_tools.h` will do in task 10.8: check the span before
			// writing anything.
			const double start_seconds = 8.0;
			const double end_seconds = 4.0;

			const std::optional<action_error> invalid_range =
				check_end_after_start(start_seconds, end_seconds);

			handler_action_outcomes produced;

			if (invalid_range.has_value())
			{
				produced.actions.push_back(failed_action(
					"time selection",
					invalid_range->code(),
					invalid_range->message()));

				return produced;
			}

			project.applied_changes.push_back("time selection");
			produced.actions.push_back(succeeded_action("time selection"));

			return produced;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_time_selection";

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(std::get<action_failed>(partial.actions[0]).error.code()
		== std::string{invalid_time_range_code});
	CHECK(project.applied_changes.empty());
}

// ---------------------------------------------------------------------------
// Requirements 9.9 and 23.8 — a REAPER call failing mid-block
// ---------------------------------------------------------------------------

TEST_CASE("a handler that throws closes the block and reports the failure with a reason", "[tool_executor]")
{
	test_registry registry;
	scripted_project_state project;

	REQUIRE(registry.register_mutating_tool(
		"create_track",
		[&project](const test_context&) -> sesh_ai::daw::mutating_handler_result<test_payload> {
			project.applied_changes.push_back("first track");

			throw std::runtime_error("InsertTrackAtIndex returned no track");
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "create_track";

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// Requirement 9.9: the action is reported failed, with a reason.
	REQUIRE(partial.actions.size() == 1);
	REQUIRE(action_was_applied(partial.actions[0]) == false);

	const action_failed& failed = std::get<action_failed>(partial.actions[0]);

	CHECK(failed.error.code() == std::string{reaper_api_failure_code});
	CHECK(failed.error.message() == "InsertTrackAtIndex returned no track");

	// Requirement 23.8: the block is closed, and no block is left open.
	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
	CHECK(stack.open_block_depth == 0);

	// And what landed before the throw is still there. There is no undo call on this
	// path, so there is nothing that could have rolled it back.
	CHECK(project.applied_changes == std::vector<std::string>{"first track"});
	CHECK(stack.undo_call_count == 0);
}

TEST_CASE("a handler that throws a non-standard exception still closes the block", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"create_track",
		[](const test_context&) -> sesh_ai::daw::mutating_handler_result<test_payload> {
			throw 17;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "create_track";

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(std::get<action_failed>(partial.actions[0]).error.message().empty() == false);
	CHECK(stack.end_block_call_count == 1);
	CHECK(stack.open_block_depth == 0);
}

TEST_CASE("an undo block that will not close reports no undo position", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context&) {
			return handler_success<test_payload>{test_payload{"volume set"}};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	stack.throw_from_end_block = true;

	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";

	const test_outcome outcome = executor.execute(call);

	// The work stands, but the record of it does not, so the result does not claim a
	// clean block: it becomes a partial naming the close failure.
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	CHECK(partial.failed_action_count() == 1);
	CHECK(std::get<action_failed>(partial.actions.back()).error.code()
		== std::string{sesh_ai::daw::undo_block_close_failure_code});

	// No `undoPositionBefore`. An unbalanced `Undo_EndBlock2` means nobody knows what
	// walking back to that position would undo, and it might be the producer's work.
	CHECK(partial.undo.has_value() == false);

	// Exactly one close attempt per open, even though the close threw — a second would
	// unbalance REAPER's own refcount.
	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
}

TEST_CASE("a precondition check that throws opens no block", "[tool_executor]")
{
	test_registry registry;
	handler_call_record record;

	REQUIRE(registry.register_mutating_tool(
		"create_send",
		[&record](const test_context&) {
			++record.call_count;

			return handler_success<test_payload>{};
		},
		[](const test_context&) -> std::optional<tool_refusal> {
			throw std::runtime_error("GetTrackNumSends returned a negative count");
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "create_send";

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// A check that could not run is not a check that passed.
	CHECK(record.call_count == 0);
	CHECK(partial.failed_action_count() == 1);
	CHECK(stack.begin_block_call_count == 0);
	CHECK(undo.has_undo_position_marker() == false);
}

TEST_CASE("a read tool that throws opens no block and reports a reason", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_read_tool(
		"list_installed_fx",
		[](const test_context&) -> sesh_ai::daw::non_mutating_handler_result<test_payload> {
			throw std::runtime_error("EnumInstalledFX returned nothing");
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "list_installed_fx";

	const test_outcome outcome = executor.execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	CHECK(partial.failed_action_count() == 1);
	CHECK(partial.undo.has_value() == false);
	CHECK(stack.begin_block_call_count == 0);
}

// ---------------------------------------------------------------------------
// The rewinding tools
// ---------------------------------------------------------------------------

TEST_CASE("a rewinding tool may refuse and opens no block", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_rewinding_tool(
		"revert_agent_changes",
		[](const test_context&) -> sesh_ai::daw::non_mutating_handler_result<test_payload> {
			// What `render_tool.h`'s sibling will do in task 10.9: hand the whole
			// question to the Undo Manager, which classifies the range before undoing
			// anything.
			foreign_undo_refusal foreign;
			foreign.refused = true;
			foreign.reason = std::string{foreign_undo_refusal_reason};
			foreign.acknowledgement_field = "confirmedForeignUndo";
			foreign.blocking.push_back(sesh_ai::daw::blocking_undo_entry{
				std::string{sesh_ai::daw::undo_entry_blocking_kind},
				"Change track volume"
			});

			return *refusal_from_foreign_undo(foreign);
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "revert_agent_changes";

	const test_outcome outcome = executor.execute(call);
	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{foreign_undo_refusal_reason});
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].description == "Change track volume");

	// A rewinding tool records no block and no marker: wrapping it would record an
	// entry describing the removal of entries.
	CHECK(stack.begin_block_call_count == 0);
	CHECK(undo.has_undo_position_marker() == false);
}

TEST_CASE("a clean undo range maps to no refusal", "[tool_executor]")
{
	foreign_undo_refusal foreign;

	CHECK(refusal_from_foreign_undo(foreign).has_value() == false);
}

// ---------------------------------------------------------------------------
// Requirement 9.8 and the render refusals
// ---------------------------------------------------------------------------

TEST_CASE("an output path collision refusal enumerates the colliding paths", "[tool_executor]")
{
	render_refusal render;
	render.reason = std::string{output_file_collision_refusal_reason};
	render.blocking.push_back(render_blocking_entity{"file_path", "/renders/Mix.wav", ""});
	render.blocking.push_back(render_blocking_entity{"file_path", "/renders/Mix.wav", ""});

	const tool_refusal refusal = refusal_from_render_refusal(render);

	CHECK(refusal.reason == std::string{output_file_collision_refusal_reason});
	REQUIRE(refusal.blocking.size() == 2);
	CHECK(refusal.blocking[0].kind == "file_path");
	CHECK(refusal.blocking[0].description == "/renders/Mix.wav");

	// A path has no GUID, and the serialiser omits the property rather than sending an
	// empty string the schema's pattern would reject.
	CHECK(refusal.blocking[0].guid.empty());
}

// ---------------------------------------------------------------------------
// Which schema each shape validates against
// ---------------------------------------------------------------------------

TEST_CASE("each result shape names the vendored schema it validates against", "[tool_executor]")
{
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context&) {
			return handler_success<test_payload>{test_payload{"volume set"}};
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call successful_call;
	successful_call.tool_name = "set_track_state";
	successful_call.track_selectors.push_back(by_name("Lead Vox"));

	const std::optional<std::string_view> success_path =
		result_schema_path_for(executor.execute(successful_call));

	REQUIRE(success_path.has_value());
	CHECK(*success_path == "mcp-tools/outputs/set-track-state.schema.json");

	test_call refused_call;
	refused_call.tool_name = "set_track_state";
	refused_call.track_selectors.push_back(by_name("Tuba"));

	const std::optional<std::string_view> refusal_path =
		result_schema_path_for(executor.execute(refused_call));

	REQUIRE(refusal_path.has_value());
	CHECK(*refusal_path == "messages/tool-result-refusal.schema.json");
}

TEST_CASE("a partial outcome validates against the framework's partial contract", "[tool_executor]")
{
	// This case used to assert the opposite, on the reasoning that `schema_validator.h`
	// declines to treat `actions` as a discriminator because `set_track_state` and
	// `set_item_properties` both return one as their ordinary result. That reasoning is
	// sound about the Envelope Codec's problem and wrong about this one. The codec sees a
	// payload and a tool name and cannot tell the two apart; this framework built the
	// payload and knows which it is.
	//
	// And the two are never the same object. `set-item-properties.schema.json` requires
	// `actions` and `items` together under `additionalProperties: false`, so that tool
	// builds its whole payload and returns `handler_success` — a `tool_partial_outcome`
	// naming it is always this framework's shape, carrying `actions` and nothing else.
	// Routed to the tool's own schema it would be refused on the missing `items`, which
	// requirement 4.1's outbound validation turns into the tool silently not working.
	tool_partial_outcome<test_payload> partial;
	partial.tool_name = "set_item_properties";
	partial.actions.push_back(succeeded_action("one"));

	const test_outcome outcome{test_result{partial}};
	const std::optional<std::string_view> path = result_schema_path_for(outcome);

	REQUIRE(path.has_value());
	CHECK(*path == "messages/tool-result-partial.schema.json");
}

TEST_CASE("a read tool's failure does not validate against its own output schema", "[tool_executor]")
{
	// The live consequence of the case above, and the reason it is worth a test of its
	// own. A read tool's only shape for a reason is `handler_action_outcomes`, so every
	// failure of `list_tracks`, `list_track_fx` and `list_selected_items` arrives as a
	// framework partial — and all three of those output schemas require fields the
	// framework has no way to supply.
	test_registry registry;

	REQUIRE(registry.register_read_tool(
		"list_tracks",
		[](const test_context&) -> sesh_ai::daw::non_mutating_handler_result<test_payload> {
			handler_action_outcomes produced;
			produced.actions.push_back(failed_action(
				"list_tracks",
				"track_list_too_large",
				"this session holds more tracks than one result can report"));

			return produced;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	test_call call;
	call.tool_name = "list_tracks";

	const std::optional<std::string_view> path = result_schema_path_for(executor.execute(call));

	REQUIRE(path.has_value());
	CHECK(*path == "messages/tool-result-partial.schema.json");
	CHECK(*path != "mcp-tools/outputs/list-tracks.schema.json");
}

// ---------------------------------------------------------------------------
// The undo report, read uniformly across the three shapes
// ---------------------------------------------------------------------------

TEST_CASE("only the two mutating shapes can carry an undo report", "[tool_executor]")
{
	tool_success<test_payload> success;
	success.tool_name = "create_track";
	success.undo = undo_report{4, "Sesh AI: create_track"};

	CHECK(undo_report_of(test_result{success}).has_value());

	tool_partial_outcome<test_payload> partial;
	partial.tool_name = "set_track_state";
	partial.actions.push_back(succeeded_action("one"));
	partial.undo = undo_report{4, "Sesh AI: set_track_state"};

	CHECK(undo_report_of(test_result{partial}).has_value());

	CHECK(undo_report_of(test_result{sesh_ai::daw::build_unresolved_track_selector_refusal()})
		.has_value() == false);
}

TEST_CASE("the undo report mirrors the undo manager's block report", "[tool_executor]")
{
	undo_block_report report;
	report.turn_undo_position_marker = 9;
	report.undo_description = "Sesh AI: duplicate_track";
	report.block_position_before = 12;

	const undo_report mirrored = sesh_ai::daw::undo_report_from(report);

	// The turn's marker, not this block's own position — requirement 10.2 asks for the
	// marker reported unchanged on every mutating result.
	CHECK(mirrored.undo_position_before == 9);
	CHECK(mirrored.undo_description == "Sesh AI: duplicate_track");
}

TEST_CASE("a success whose handler says nothing landed reports no undo marker", "[tool_executor]")
{
	// `tool_partial_outcome` gets this right on its own, because the framework can count
	// its applied actions. A `tool_success` cannot be read that way — requirement 4.4
	// keeps the payload opaque and this framework never dereferences it — so the two tools
	// whose own output schema carries `actions` have to say so. Both
	// `set-item-properties.schema.json` and `set-track-state.schema.json` describe the
	// *absence* of `undoPositionBefore` as what tells the producer nothing changed, so a
	// marker reported for a call that moved nothing shortens the range "revert all" walks.
	auto marker_for = [](bool nothing_was_applied) {
		test_registry registry;

		REQUIRE(registry.register_mutating_tool(
			"set_item_properties",
			[nothing_was_applied](const test_context&) {
				handler_success<test_payload> produced;
				produced.fields = test_payload{"every entry reported"};
				produced.nothing_was_applied = nothing_was_applied;

				return produced;
			})
			== tool_registration_outcome::registered);

		scripted_undo_stack stack;
		undo_manager undo{stack};
		scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
		NoLearnedAliases aliases;
		test_executor executor{registry, undo, tracks, aliases};

		undo.begin_turn();

		test_call call;
		call.tool_name = "set_item_properties";

		return undo_report_of(std::get<test_result>(executor.execute(call)));
	};

	// The default, and the answer for the thirty-odd mutating tools whose success means
	// the mutation happened.
	CHECK(marker_for(false).has_value());

	// An array in which every entry failed left nothing behind.
	CHECK_FALSE(marker_for(true).has_value());
}

TEST_CASE("a handler that vetoes the undo marker still runs inside one closed block", "[tool_executor]")
{
	// The flag withholds the *report*, not the block. `undo_manager` opens one and closes
	// it either way, because the handler's writes went somewhere even when none of them
	// landed as the producer asked — and an unbalanced block is the one failure mode worse
	// than a missing marker.
	test_registry registry;

	REQUIRE(registry.register_mutating_tool(
		"set_track_state",
		[](const test_context&) {
			handler_success<test_payload> produced;
			produced.nothing_was_applied = true;

			return produced;
		})
		== tool_registration_outcome::registered);

	scripted_undo_stack stack;
	undo_manager undo{stack};
	scripted_track_list tracks{{track_named("Lead Vox", 'a')}};
	NoLearnedAliases aliases;
	test_executor executor{registry, undo, tracks, aliases};

	undo.begin_turn();

	test_call call;
	call.tool_name = "set_track_state";

	const test_outcome outcome = executor.execute(call);
	const auto* const success = std::get_if<tool_success<test_payload>>(&std::get<test_result>(outcome));

	REQUIRE(success != nullptr);
	CHECK_FALSE(success->undo.has_value());

	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
}
