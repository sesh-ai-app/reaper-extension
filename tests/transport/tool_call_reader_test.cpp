// `read_tool_call` — the four things the Tool Executor needs to know before any handler
// runs (task 24.1, requirements 4.1, 4.4, 9.1, 23.1).
//
// Driven against the real JSON library for the reason
// `tool_request_readers_test.cpp` gives at its own head: reading fields is the whole of
// what this does, so a stub would have the cases agree with each other and with nothing
// else.

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#if __has_include(<nlohmann/json.hpp>)
	#define SESH_AI_TESTS_HAVE_JSON 1
#else
	#define SESH_AI_TESTS_HAVE_JSON 0
#endif

#if SESH_AI_TESTS_HAVE_JSON
	#include <nlohmann/json.hpp>

	#include <transport/tool_call_reader.h>
#endif

#if SESH_AI_TESTS_HAVE_JSON

namespace {

	using sesh_ai::daw::TrackGuidSelector;
	using sesh_ai::daw::TrackNamePatternSelector;
	using sesh_ai::daw::TrackSelector;
	using sesh_ai::transport::ToolCallReadOutcome;
	using sesh_ai::transport::read_tool_call_reporting;

	using Json = nlohmann::json;

	// Stands in for `transport/envelope.h`'s `InboundEnvelope`.
	//
	// The real one carries an `nlohmann::json` payload and nothing else these readers
	// touch, so this is it field for field — the reader is templated on the envelope type
	// for the same reason the Message Dispatcher is.
	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		Json payload = Json::object();
	};

	std::string guid_of(int distinguisher)
	{
		std::string guid{"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEE"};
		guid += static_cast<char>('0' + distinguisher);
		guid += '}';

		return guid;
	}

	Json guid_selector(int distinguisher)
	{
		Json selector = Json::object();
		selector["guid"] = guid_of(distinguisher);

		return selector;
	}

	Json name_pattern_selector(std::string_view pattern)
	{
		Json selector = Json::object();
		selector["namePattern"] = std::string{pattern};

		return selector;
	}

	// One envelope and the call read out of it, kept together.
	//
	// Together because `tool_call::validated_input` points into the envelope — the
	// executor holds the payload as a pointer it never dereferences, and a `tool_call`
	// has nowhere to own a copy. The Message Dispatcher owns the envelope for the whole
	// dispatch; here, this does. Neither copyable nor movable, so a case cannot hand one
	// back out of the scope that owns the envelope.
	class ReadCall {
	public:
		ReadCall(std::string_view tool_name, Json payload)
		{
			envelope_.type = "request:" + std::string{tool_name};
			envelope_.request_id = "req-7";
			envelope_.payload = std::move(payload);

			outcome_ = read_tool_call_reporting<StubInboundEnvelope, Json>(tool_name, envelope_);
		}

		ReadCall(const ReadCall&) = delete;
		ReadCall& operator=(const ReadCall&) = delete;
		ReadCall(ReadCall&&) = delete;
		ReadCall& operator=(ReadCall&&) = delete;

		const ToolCallReadOutcome<Json>& outcome() const { return outcome_; }

		const StubInboundEnvelope& envelope() const { return envelope_; }

	private:
		StubInboundEnvelope envelope_;
		ToolCallReadOutcome<Json> outcome_;
	};

	// The GUID a selector addresses, or empty when it addresses a pattern.
	std::string selector_guid(const TrackSelector& selector)
	{
		if (const TrackGuidSelector* const by_guid = std::get_if<TrackGuidSelector>(&selector)) {
			return by_guid->guid;
		}

		return std::string{};
	}

	std::string selector_pattern(const TrackSelector& selector)
	{
		if (const TrackNamePatternSelector* const by_pattern =
				std::get_if<TrackNamePatternSelector>(&selector)) {
			return by_pattern->name_pattern;
		}

		return std::string{};
	}

}

TEST_CASE("a tool call carries the tool name, the request identifier, and the payload")
{
	Json payload = Json::object();
	payload["track"] = guid_selector(1);
	payload["fxIdentifier"] = "ReaComp";

	const ReadCall outcome_reading{"add_fx", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	REQUIRE(outcome.payload_was_readable());
	CHECK(outcome.call.tool_name == "add_fx");
	CHECK(outcome.call.request_id == "req-7");

	// Requirement 4.4: carried as a pointer the executor never dereferences.
	REQUIRE(outcome.call.validated_input != nullptr);
	CHECK(outcome.call.validated_input->contains("fxIdentifier"));
}

TEST_CASE("each selector field is read into the order target resolution will see")
{
	SECTION("one track, for the eleven tools that address one")
	{
		Json payload = Json::object();
		payload["track"] = guid_selector(1);

		const ReadCall outcome_reading{"delete_track", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		REQUIRE(outcome.call.track_selectors.size() == 1);
		CHECK(selector_guid(outcome.call.track_selectors[0]) == guid_of(1));
	}

	SECTION("create_send resolves its source before its destination")
	{
		Json payload = Json::object();
		payload["sourceTrack"] = guid_selector(1);
		payload["destinationTrack"] = guid_selector(2);

		const ReadCall outcome_reading{"create_send", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		REQUIRE(outcome.call.track_selectors.size() == 2);
		CHECK(selector_guid(outcome.call.track_selectors[0]) == guid_of(1));
		CHECK(selector_guid(outcome.call.track_selectors[1]) == guid_of(2));
	}

	SECTION("create_track resolves afterTrack before insideFolderOf")
	{
		Json payload = Json::object();
		payload["name"] = "Lead Vox";
		payload["insideFolderOf"] = guid_selector(2);
		payload["afterTrack"] = guid_selector(1);

		const ReadCall outcome_reading{"create_track", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		// The payload's own property order does not decide it: the reader asks for
		// `afterTrack` first, which is the order `bind_track_structure_request_readers`
		// maps them back in.
		REQUIRE(outcome.call.track_selectors.size() == 2);
		CHECK(selector_guid(outcome.call.track_selectors[0]) == guid_of(1));
		CHECK(selector_guid(outcome.call.track_selectors[1]) == guid_of(2));
	}

	SECTION("set_folder_structure resolves the moved tracks in order, then the destination")
	{
		Json payload = Json::object();
		payload["tracks"] = Json::array({guid_selector(1), guid_selector(2)});
		payload["intoFolderOf"] = guid_selector(3);

		const ReadCall outcome_reading{"set_folder_structure", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		REQUIRE(outcome.call.track_selectors.size() == 3);
		CHECK(selector_guid(outcome.call.track_selectors[0]) == guid_of(1));
		CHECK(selector_guid(outcome.call.track_selectors[1]) == guid_of(2));
		CHECK(selector_guid(outcome.call.track_selectors[2]) == guid_of(3));
	}

	SECTION("set_track_state resolves one selector per entry of its changes array")
	{
		Json first = Json::object();
		first["track"] = guid_selector(1);
		first["muted"] = true;

		Json second = Json::object();
		second["track"] = guid_selector(2);
		second["volume"] = -6.0;

		Json payload = Json::object();
		payload["changes"] = Json::array({first, second});

		const ReadCall outcome_reading{"set_track_state", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		REQUIRE(outcome.call.track_selectors.size() == 2);
		CHECK(selector_guid(outcome.call.track_selectors[0]) == guid_of(1));
		CHECK(selector_guid(outcome.call.track_selectors[1]) == guid_of(2));
	}

	SECTION("a tool that addresses no track resolves nothing")
	{
		Json payload = Json::object();
		payload["position"] = 4.0;
		payload["name"] = "Chorus";

		const ReadCall outcome_reading{"create_marker", payload};
		const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

		REQUIRE(outcome.payload_was_readable());
		CHECK(outcome.call.track_selectors.empty());
	}
}

TEST_CASE("an optional selector field the call omitted addresses nothing")
{
	// `move_items` without `toTrack` is a move along the timeline, and `list_tracks`
	// without `tracks` is the whole project. Neither is a missing field.
	Json move = Json::object();
	move["itemGuids"] = Json::array({guid_of(3)});

	const ReadCall moved_reading{"move_items", move};
	const ToolCallReadOutcome<Json>& moved = moved_reading.outcome();

	REQUIRE(moved.payload_was_readable());
	CHECK(moved.call.track_selectors.empty());

	const ReadCall listed_reading{"list_tracks", Json::object()};
	const ToolCallReadOutcome<Json>& listed = listed_reading.outcome();

	REQUIRE(listed.payload_was_readable());
	CHECK(listed.call.track_selectors.empty());
}

TEST_CASE("a required selector field the call omitted makes the payload unreadable")
{
	const ReadCall outcome_reading{"remove_fx", Json::object()};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	CHECK_FALSE(outcome.payload_was_readable());
	CHECK(outcome.call.validated_input == nullptr);
	CHECK(outcome.call.track_selectors.empty());
}

TEST_CASE("a name pattern selector is carried as a pattern for the Object Resolver")
{
	Json payload = Json::object();
	payload["track"] = name_pattern_selector("drum bus");

	const ReadCall outcome_reading{"list_track_fx", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	REQUIRE(outcome.payload_was_readable());
	REQUIRE(outcome.call.track_selectors.size() == 1);
	CHECK(selector_pattern(outcome.call.track_selectors[0]) == "drum bus");
}

TEST_CASE("a selector carrying neither a guid nor a name pattern makes the call unreadable")
{
	Json payload = Json::object();
	payload["track"] = Json::object();

	const ReadCall outcome_reading{"list_track_fx", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	CHECK_FALSE(outcome.payload_was_readable());
	CHECK(outcome.call.validated_input == nullptr);
}

TEST_CASE("the four acknowledgement fields are read for every tool that carries one")
{
	Json payload = Json::object();
	payload["source"] = "master_mix";
	payload["bounds"] = "project";
	payload["confirmedOverwrite"] = true;
	payload["confirmedTrackSelection"] = true;

	const ReadCall outcome_reading{"render", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	REQUIRE(outcome.payload_was_readable());
	CHECK(outcome.call.confirmed_overwrite);
	CHECK(outcome.call.confirmed_track_selection);
	CHECK_FALSE(outcome.call.confirmed_signal_cycle);
	CHECK_FALSE(outcome.call.confirmed_foreign_undo);

	SECTION("and a foreign undo acknowledgement on a tool that takes nothing else")
	{
		Json revert = Json::object();
		revert["confirmedForeignUndo"] = true;

		const ReadCall reverted_reading{"revert_agent_changes", revert};
		const ToolCallReadOutcome<Json>& reverted = reverted_reading.outcome();

		REQUIRE(reverted.payload_was_readable());
		CHECK(reverted.call.confirmed_foreign_undo);
	}

	SECTION("and an acknowledgement of the wrong type is not read as approval")
	{
		Json malformed = Json::object();
		malformed["confirmedForeignUndo"] = "yes";

		const ReadCall reverted_reading{"revert_agent_changes", malformed};
		const ToolCallReadOutcome<Json>& reverted = reverted_reading.outcome();

		CHECK_FALSE(reverted.payload_was_readable());
		CHECK_FALSE(reverted.call.confirmed_foreign_undo);
	}
}

TEST_CASE("an unreadable payload reaches the executor as no payload and addresses nothing")
{
	// The selectors go with the payload rather than surviving it. Keeping them would have
	// the executor resolve targets for a call it is about to fail, and a pattern matching
	// two tracks would turn an unreadable input into an ambiguity refusal — reporting the
	// wrong thing about the wrong problem.
	Json payload = Json::object();
	payload["track"] = name_pattern_selector("drum bus");
	payload["fxIndex"] = "2";

	const ReadCall outcome_reading{"remove_fx", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	CHECK_FALSE(outcome.payload_was_readable());
	CHECK(outcome.call.validated_input == nullptr);
	CHECK(outcome.call.track_selectors.empty());

	// Still named, so a startup or dispatch log line can say which call this was.
	CHECK(outcome.call.tool_name == "remove_fx");
	CHECK(outcome.call.request_id == "req-7");
	CHECK(outcome.payload_problem.find("fxIndex") != std::string::npos);
}

TEST_CASE("a payload that is not an object carries no fields")
{
	StubInboundEnvelope envelope;
	envelope.type = "request:get_project_summary";
	envelope.request_id = "req-7";
	envelope.payload = Json::array();

	const ToolCallReadOutcome<Json> outcome =
		read_tool_call_reporting<StubInboundEnvelope, Json>("get_project_summary", envelope);

	CHECK_FALSE(outcome.payload_was_readable());
	CHECK(outcome.call.validated_input == nullptr);
	CHECK_FALSE(outcome.payload_problem.empty());
}

TEST_CASE("an inverted range is a readable call, left for the tool layer to report")
{
	// The task settled this: an inverted range stays an invalid call reported as a failed
	// action by `check_end_after_start`, not an unreadable input and not a refusal.
	Json payload = Json::object();
	payload["start"] = 9.0;
	payload["end"] = 1.0;

	const ReadCall outcome_reading{"set_time_selection", payload};
	const ToolCallReadOutcome<Json>& outcome = outcome_reading.outcome();

	CHECK(outcome.payload_was_readable());
	CHECK(outcome.call.validated_input != nullptr);
}

TEST_CASE("the codec's own entry point returns just the call")
{
	StubInboundEnvelope envelope;
	envelope.type = "request:get_project_summary";
	envelope.request_id = "req-9";

	const sesh_ai::daw::tool_call<Json> call =
		sesh_ai::transport::read_tool_call<StubInboundEnvelope, Json>(
			"get_project_summary",
			envelope);

	CHECK(call.tool_name == "get_project_summary");
	CHECK(call.request_id == "req-9");
	CHECK(call.validated_input == &envelope.payload);
}

#else

TEST_CASE("reading a tool call needs nlohmann/json")
{
	SKIP(
		"nlohmann/json is not on the include path, so the reader cannot be driven against "
		"the library it binds to. Configure with a vcpkg toolchain."
	);
}

#endif
