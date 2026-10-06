// The two rewinding tool handlers — the wiring between the Tool Executor and the Undo
// Manager (task 10.9, requirements 9.1, 10.5).
//
// The Undo Manager is already tested next door: the range walk, the classification of
// each entry, interleaving detected anywhere in the range, the marker's lifetime. None
// of that is re-tested here. What this file checks is what only exists once the
// manager is registered as two tools, and the one guarantee registering them could
// take away:
//
//   - Requirement 10.5 and property 16: `revert_agent_changes` refusing performs zero
//     undo actions. Counted on a scripted stack rather than inferred from the refusal
//     payload, because "nothing was undone" is a claim about the session, not about the
//     result describing it.
//   - Both tools register with `tool_undo_effect::rewinds_undo_stack` and open no undo
//     block — a block would record an entry describing the removal of entries, and a
//     marker would point past the producer's own work. Neither result carries an undo
//     report, which is the absence both output schemas enforce.
//   - `undo_last_action` is one step and `revert_agent_changes` is the whole turn. The
//     two are dispatched separately and reported separately.
//   - An unknown tool name still produces requirement 23.6's error rather than reaching
//     either handler.
//
// The scripted stack is this file's own, for the reason `undo_manager_test.cpp`'s lives
// in an anonymous namespace: the two suites are testing different things and should not
// share a fixture that one of them can change.
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/undo_tools.h>

using sesh_ai::daw::agent_undo_description_prefix;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::foreign_undo_acknowledgement_field;
using sesh_ai::daw::foreign_undo_refusal_reason;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::revert_outcome;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_partial_outcome;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::undo_block_guard;
using sesh_ai::daw::undo_last_action_outcome;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_report_of;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::unknown_tool_error;
using sesh_ai::daw::tools::register_undo_tools;
using sesh_ai::daw::tools::revert_agent_changes_tool_name;
using sesh_ai::daw::tools::undo_last_action_tool_name;
using sesh_ai::daw::tools::undo_payload_codec;
using sesh_ai::daw::tools::undo_tool_registration;

namespace
{
	// What the handlers write their result into. A plain struct — the framework never
	// reads a field of it, and neither does this suite beyond what it asserts.
	struct test_payload
	{
		int reverted_entry_count = 0;
		std::vector<std::string> undone_entry_descriptions;
		int undo_stack_position_after = 0;
		bool was_agent_entry = false;
	};

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;

	// A scripted REAPER undo stack that counts every undo, every block open, and every
	// block close. The undo count is what makes requirement 10.5 an assertion.
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

		// A producer's own edit landing on the stack. The extension cannot cause one,
		// which is exactly why interleaving has to be staged rather than provoked.
		void append_producer_edit(const std::string& description)
		{
			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
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

	// The codec the real build binds to `nlohmann::json`. Both writers take the
	// manager's own outcome types, which already match their output schemas field for
	// field.
	undo_payload_codec<test_payload> scripted_codec()
	{
		undo_payload_codec<test_payload> codec;

		codec.write_undo_last_action_result = [](const undo_last_action_outcome& outcome) {
			test_payload payload;
			payload.undone_entry_descriptions = outcome.undone_entry_descriptions;
			payload.undo_stack_position_after = outcome.undo_stack_position_after;
			payload.was_agent_entry = outcome.was_agent_entry;

			return payload;
		};

		codec.write_revert_agent_changes_result = [](const revert_outcome& outcome) {
			test_payload payload;
			payload.reverted_entry_count = outcome.reverted_entry_count;
			payload.undone_entry_descriptions = outcome.undone_entry_descriptions;
			payload.undo_stack_position_after = outcome.undo_stack_position_after;

			return payload;
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

	// Everything one dispatch needs, with both tools registered.
	struct undo_tools_fixture
	{
		test_registry registry;
		counting_undo_stack stack;
		undo_manager undo{stack};
		empty_track_list tracks;
		NoLearnedAliases aliases;
		test_executor executor{registry, undo, tracks, aliases};
		test_payload validated_input{};
		undo_tool_registration registration{};

		undo_tools_fixture()
		{
			registration = register_undo_tools(registry, undo, scripted_codec());
		}

		// One mutating tool's worth of agent work, which is what gives the turn a
		// marker and the stack an agent-created entry. Produced through the manager
		// rather than by seeding a description, so the entry carries whatever prefix
		// the manager actually writes.
		void agent_changed_something(const std::string& tool_name)
		{
			undo_block_guard block = undo.open_undo_block(tool_name);
			block.close();
		}

		// The counters, cleared so a later assertion is about the dispatch and not
		// about the setup that arranged it.
		void forget_setup_calls()
		{
			stack.begin_block_call_count = 0;
			stack.end_block_call_count = 0;
			stack.undo_call_count = 0;
		}

		test_outcome dispatch(std::string_view tool_name, bool confirmed_foreign_undo = false)
		{
			test_call call;
			call.tool_name = std::string{tool_name};
			call.request_id = "request-undo";
			call.validated_input = &validated_input;
			call.confirmed_foreign_undo = confirmed_foreign_undo;

			return executor.execute(call);
		}
	};
}

// ---------------------------------------------------------------------------
// The registration path — no block, no marker
// ---------------------------------------------------------------------------

TEST_CASE("both rewinding tools register as rewinding the undo stack", "[undo_tools]")
{
	undo_tools_fixture fixture;

	CHECK(fixture.registration.every_tool_registered());
	REQUIRE(fixture.registration.registered_tool_names.size() == 2);

	for (const std::string_view& tool_name : {undo_last_action_tool_name, revert_agent_changes_tool_name})
	{
		const auto* const registered = fixture.registry.find_tool(tool_name);
		REQUIRE(registered != nullptr);

		// Not `undo_block`: wrapping an undo in a block would record an entry
		// describing the removal of entries, and capturing a marker before one would
		// point past the producer's own work.
		CHECK(registered->undo_effect() == tool_undo_effect::rewinds_undo_stack);
	}
}

TEST_CASE("a rewinding tool opens no undo block and reports no undo position", "[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();
	fixture.agent_changed_something("create_track");
	fixture.forget_setup_calls();

	const test_outcome outcome = fixture.dispatch(undo_last_action_tool_name);

	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.tool_name == std::string{undo_last_action_tool_name});

	// The absence both output schemas enforce: `undoPositionBefore` is the marker
	// "revert all" walks back to, and a rewinding tool reporting one would have the
	// producer redoing what was just undone.
	CHECK_FALSE(success.undo.has_value());
	CHECK_FALSE(undo_report_of(result_of(outcome)).has_value());

	// And no block was opened for the dispatch itself.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.stack.end_block_call_count == 0);
}

// ---------------------------------------------------------------------------
// Requirement 10.5 and property 16 — a refused revert undoes nothing
// ---------------------------------------------------------------------------

TEST_CASE(
	"revert_agent_changes refusing performs zero undo actions",
	"[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();

	// The turn: the agent changed something, then the producer edited the session
	// while it was working, then the agent changed something else. The foreign entry is
	// in the middle of the range rather than on top, which is requirement 10.6's case.
	fixture.agent_changed_something("create_track");
	fixture.stack.append_producer_edit("Move media items");
	fixture.agent_changed_something("set_track_state");

	const int position_before_request = fixture.stack.current_position();
	fixture.forget_setup_calls();

	const test_outcome outcome = fixture.dispatch(revert_agent_changes_tool_name);

	const tool_refusal& refusal = refusal_of(outcome);
	CHECK(refusal.reason == std::string{foreign_undo_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{foreign_undo_acknowledgement_field});

	// Only the producer's own entry is named, in REAPER's own words, so what they are
	// shown matches their undo history.
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].kind == "undo_entry");
	CHECK(refusal.blocking[0].description == "Move media items");

	// The assertion this whole file exists for. Not "the stack looks unchanged" — no
	// undo was attempted at all.
	CHECK(fixture.stack.undo_call_count == 0);
	CHECK(fixture.stack.current_position() == position_before_request);

	// And a refusal records nothing either way: the marker the turn already had is
	// still the turn's, and no block was opened to shorten it.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.undo.has_undo_position_marker());
}

TEST_CASE("revert_agent_changes walks a clean range back to the marker", "[undo_tools]")
{
	undo_tools_fixture fixture;

	// One pre-existing entry from before the turn, which the revert must not touch.
	fixture.stack.append_producer_edit("Insert new track");
	fixture.undo.begin_turn();

	fixture.agent_changed_something("create_track");
	fixture.agent_changed_something("set_track_state");
	fixture.forget_setup_calls();

	const test_outcome outcome = fixture.dispatch(revert_agent_changes_tool_name);

	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.fields.reverted_entry_count == 2);
	CHECK(success.fields.undone_entry_descriptions.size() == 2);

	// Two entries in the range, two undo calls, and the stack stopped at the marker
	// rather than walking into the producer's earlier work.
	CHECK(fixture.stack.undo_call_count == 2);
	CHECK(fixture.stack.current_position() == 1);
	CHECK(success.fields.undo_stack_position_after == 1);
	CHECK_FALSE(success.undo.has_value());
}

TEST_CASE(
	"an acknowledged foreign entry lets the revert proceed",
	"[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();

	fixture.agent_changed_something("create_track");
	fixture.stack.append_producer_edit("Move media items");
	fixture.forget_setup_calls();

	// The retry after the refusal named the producer's edit and they said it should go
	// too. It permits rather than overrides — the entries are still classified.
	const test_outcome outcome = fixture.dispatch(revert_agent_changes_tool_name, true);

	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.fields.reverted_entry_count == 2);
	CHECK(fixture.stack.undo_call_count == 2);
	CHECK(fixture.stack.current_position() == 0);
}

// ---------------------------------------------------------------------------
// undo_last_action — one step, and a different range
// ---------------------------------------------------------------------------

TEST_CASE("undo_last_action undoes exactly one agent entry", "[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();

	fixture.agent_changed_something("create_track");
	fixture.agent_changed_something("set_track_state");
	fixture.forget_setup_calls();

	const test_outcome outcome = fixture.dispatch(undo_last_action_tool_name);

	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.fields.was_agent_entry);
	REQUIRE(success.fields.undone_entry_descriptions.size() == 1);

	// REAPER's own description of the entry, which for the agent's own work is the
	// description the manager wrote when it closed the block.
	CHECK(
		success.fields.undone_entry_descriptions[0].find(std::string{agent_undo_description_prefix})
		== 0u);

	// One step, not the turn. The earlier block is still there.
	CHECK(fixture.stack.undo_call_count == 1);
	CHECK(fixture.stack.current_position() == 1);
}

TEST_CASE("undo_last_action refuses the producer's own top entry", "[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();

	fixture.agent_changed_something("create_track");
	fixture.stack.append_producer_edit("Adjust track volume");
	fixture.forget_setup_calls();

	const test_outcome outcome = fixture.dispatch(undo_last_action_tool_name);

	const tool_refusal& refusal = refusal_of(outcome);
	CHECK(refusal.reason == std::string{foreign_undo_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{foreign_undo_acknowledgement_field});
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].description == "Adjust track volume");

	CHECK(fixture.stack.undo_call_count == 0);
	CHECK(fixture.stack.current_position() == 2);
}

TEST_CASE("undo_last_action on an empty undo history is a complete answer", "[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();

	const test_outcome outcome = fixture.dispatch(undo_last_action_tool_name);

	// Nothing to undo is not a failure: the schema can say it with no descriptions and
	// a position of zero, and calling it a failure would have the agent apologising for
	// a session that was already where the producer wanted it.
	const tool_success<test_payload>& success = success_of(outcome);
	CHECK(success.fields.undone_entry_descriptions.empty());
	CHECK(success.fields.undo_stack_position_after == 0);
	CHECK_FALSE(success.fields.was_agent_entry);
}

// ---------------------------------------------------------------------------
// Requirement 23.6 — a name this build does not implement
// ---------------------------------------------------------------------------

TEST_CASE("an unknown tool name never reaches either undo handler", "[undo_tools]")
{
	undo_tools_fixture fixture;
	fixture.undo.begin_turn();
	fixture.agent_changed_something("create_track");
	fixture.forget_setup_calls();

	test_call call;
	call.tool_name = "undo_everything_forever";
	call.validated_input = &fixture.validated_input;

	const test_outcome outcome = fixture.executor.execute(call);

	REQUIRE(std::holds_alternative<unknown_tool_error>(outcome));
	CHECK(std::get<unknown_tool_error>(outcome).tool_name == "undo_everything_forever");

	// Dispatch stopped at the name, so the stack was not touched.
	CHECK(fixture.stack.undo_call_count == 0);
	CHECK(fixture.stack.begin_block_call_count == 0);
}
