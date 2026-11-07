// The inbound readers: every tool's payload, turned into the argument its handler takes
// (task 24.1, requirements 4.1, 4.4, 9.1, 23.1).
//
// ---------------------------------------------------------------------------
// Why this file drives the real JSON library rather than a stub
//
// Most of the suite stands a plain struct in for `nlohmann::json`, and that is right
// where the thing under test never reads a field — `tool_executor.h` is templated on the
// payload precisely so it cannot. These readers are the opposite case: reading fields is
// the whole of what they do, and a stub that answered `is_number_integer()` differently
// from the real library would make these cases agree with each other and with nothing
// else.
//
// So the guard is `__has_include`, and the cases skip with a named reason on a machine
// with no vcpkg — the same arrangement `schema_validation_property_test.cpp` uses for
// Properties 12 and 13, and for the same reason. They run in CI, which configures with a
// vcpkg toolchain.

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/schema_validator.h>

#if __has_include(<nlohmann/json.hpp>)
	#define SESH_AI_TESTS_HAVE_JSON 1
#else
	#define SESH_AI_TESTS_HAVE_JSON 0
#endif

#if SESH_AI_TESTS_HAVE_JSON
	#include <nlohmann/json.hpp>

	#include <transport/tool_request_readers.h>
#endif

#if SESH_AI_TESTS_HAVE_JSON

namespace {

	using sesh_ai::daw::ResolvedTrack;
	using sesh_ai::daw::tool_call;
	using sesh_ai::daw::tool_execution_context;
	using sesh_ai::transport::ToolInputNotReadable;
	using sesh_ai::transport::ToolInputProblem;
	using sesh_ai::transport::ToolInputReader;
	using sesh_ai::transport::ToolRequestReader;
	using sesh_ai::transport::check_tool_input_fields;
	using sesh_ai::transport::unreadable_tool_input_identifier;

	using Json = nlohmann::json;

	// A braced GUID of the form the project context reports, distinguished by one digit.
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

	// The smallest payload each of the 42 tools accepts: every required field and nothing
	// else. Kept in one place so the case below can walk the whole protocol rather than
	// the tools somebody remembered.
	Json minimal_payload_for(std::string_view tool_name)
	{
		Json payload = Json::object();

		if (tool_name == "add_fx") {
			payload["track"] = guid_selector(1);
			payload["fxIdentifier"] = "ReaComp";
		} else if (tool_name == "apply_fx_destructively") {
			payload["track"] = guid_selector(1);
		} else if (tool_name == "change_tempo_map") {
			Json point = Json::object();
			point["position"] = 0.0;
			point["tempo"] = 120.0;
			payload["points"] = Json::array({point});
		} else if (tool_name == "create_bus") {
			payload["type"] = "summing";
			payload["name"] = "Drum Bus";
			payload["sourceTracks"] = Json::array({guid_selector(1)});
		} else if (tool_name == "create_marker") {
			payload["position"] = 12.5;
			payload["name"] = "Chorus";
		} else if (tool_name == "create_region") {
			payload["start"] = 1.0;
			payload["end"] = 9.0;
			payload["name"] = "Verse";
		} else if (tool_name == "create_send") {
			payload["sourceTrack"] = guid_selector(1);
			payload["destinationTrack"] = guid_selector(2);
		} else if (tool_name == "create_track") {
			payload["name"] = "Lead Vox";
		} else if (tool_name == "delete_items") {
			payload["itemGuids"] = Json::array({guid_of(3)});
		} else if (tool_name == "delete_marker_or_region") {
			payload["guid"] = guid_of(4);
		} else if (tool_name == "delete_track" || tool_name == "duplicate_track") {
			payload["track"] = guid_selector(1);
		} else if (tool_name == "get_fx_parameters" || tool_name == "remove_fx") {
			payload["track"] = guid_selector(1);
			payload["fxIndex"] = 2;
		} else if (tool_name == "import_audio_file" || tool_name == "import_midi_file") {
			payload["track"] = guid_selector(1);
			payload["filePath"] = "/tmp/take.wav";
			payload["position"] = 4.0;
		} else if (tool_name == "list_installed_fx") {
			payload["searchTerm"] = "comp";
		} else if (tool_name == "list_track_fx") {
			payload["track"] = guid_selector(1);
		} else if (tool_name == "move_all_items") {
			payload["offsetSeconds"] = 2.5;
		} else if (tool_name == "move_items") {
			payload["itemGuids"] = Json::array({guid_of(3)});
		} else if (tool_name == "remove_bus") {
			payload["busTrack"] = guid_selector(1);
		} else if (tool_name == "remove_send" || tool_name == "set_send_state") {
			payload["sourceTrack"] = guid_selector(1);
			payload["sendIndex"] = 0;
		} else if (tool_name == "render") {
			payload["source"] = "master_mix";
			payload["bounds"] = "project";
		} else if (tool_name == "set_folder_structure") {
			payload["tracks"] = Json::array({guid_selector(1)});
		} else if (tool_name == "set_fx_bypass") {
			payload["track"] = guid_selector(1);
			payload["fxIndex"] = 0;
			payload["bypassed"] = true;
		} else if (tool_name == "set_fx_parameter") {
			payload["track"] = guid_selector(1);
			payload["fxIndex"] = 0;
			payload["parameterName"] = "Threshold";
			payload["value"] = -12.0;
		} else if (tool_name == "set_item_properties") {
			Json change = Json::object();
			change["itemGuid"] = guid_of(3);
			payload["changes"] = Json::array({change});
		} else if (tool_name == "set_parent_send") {
			payload["tracks"] = Json::array({guid_selector(1)});
			payload["enabled"] = false;
		} else if (tool_name == "set_time_selection") {
			payload["start"] = 0.0;
			payload["end"] = 8.0;
		} else if (tool_name == "set_track_state") {
			Json change = Json::object();
			change["track"] = guid_selector(1);
			payload["changes"] = Json::array({change});
		} else if (tool_name == "split_items") {
			payload["itemGuids"] = Json::array({guid_of(3)});
			payload["position"] = 6.0;
		} else if (tool_name == "update_marker_or_region") {
			payload["guid"] = guid_of(4);
		}

		// Everything else takes nothing required: `get_project_summary`, `get_routing`,
		// `list_markers`, `list_regions`, `list_selected_items`, `list_tempo_changes`,
		// `list_tracks`, `undo_last_action`, `revert_agent_changes`.
		return payload;
	}

	// Whether the named tool's own fields read out of this payload.
	bool fields_read(std::string_view tool_name, const Json& payload)
	{
		ToolInputProblem problem;
		ToolInputReader<Json> reader{&payload, problem};

		check_tool_input_fields(tool_name, reader);

		return !problem.anything_recorded();
	}

	std::string field_problem(std::string_view tool_name, const Json& payload)
	{
		ToolInputProblem problem;
		ToolInputReader<Json> reader{&payload, problem};

		check_tool_input_fields(tool_name, reader);

		return problem.description();
	}

	// A context over one payload and a scripted set of already-resolved targets, which is
	// what the four context-taking families' readers are handed.
	struct ReaderFixture {
		tool_call<Json> call;
		std::vector<ResolvedTrack> resolved_targets;

		tool_execution_context<Json> context() const
		{
			return tool_execution_context<Json>{call, resolved_targets};
		}

		void resolve(int distinguisher)
		{
			ResolvedTrack resolved;
			resolved.reference.guid = guid_of(distinguisher);
			resolved.reference.name = "Track " + std::to_string(distinguisher);

			resolved_targets.push_back(resolved);
		}
	};

}

// ---------------------------------------------------------------------------
// The whole protocol reads
// ---------------------------------------------------------------------------

TEST_CASE("every constrained tool's minimal payload reads into its handler's argument")
{
	// Walks `tool_output_schema_bindings` rather than a list in this file, so a tool
	// whose field names were never read is a failure here rather than an unknown-tool
	// error in a producer's session.
	for (const sesh_ai::transport::ToolOutputSchemaBinding& binding :
		sesh_ai::transport::tool_output_schema_bindings) {
		const Json payload = minimal_payload_for(binding.tool_name);

		INFO("tool: " << binding.tool_name << " problem: " << field_problem(binding.tool_name, payload));
		CHECK(fields_read(binding.tool_name, payload));
	}
}

TEST_CASE("a required field the call did not carry is not readable")
{
	struct RequiredField {
		std::string_view tool_name;
		const char* field_name;
	};

	const std::vector<RequiredField> required_fields{
		{"add_fx", "fxIdentifier"},
		{"change_tempo_map", "points"},
		{"create_bus", "type"},
		{"create_bus", "name"},
		{"create_marker", "position"},
		{"create_marker", "name"},
		{"create_region", "start"},
		{"create_region", "end"},
		{"create_region", "name"},
		{"create_track", "name"},
		{"delete_items", "itemGuids"},
		{"delete_marker_or_region", "guid"},
		{"get_fx_parameters", "fxIndex"},
		{"import_audio_file", "filePath"},
		{"import_audio_file", "position"},
		{"list_installed_fx", "searchTerm"},
		{"move_all_items", "offsetSeconds"},
		{"move_items", "itemGuids"},
		{"remove_fx", "fxIndex"},
		{"remove_send", "sendIndex"},
		{"render", "source"},
		{"render", "bounds"},
		{"set_folder_structure", "tracks"},
		{"set_fx_bypass", "bypassed"},
		{"set_fx_parameter", "parameterName"},
		{"set_fx_parameter", "value"},
		{"set_item_properties", "changes"},
		{"set_parent_send", "enabled"},
		{"set_send_state", "sendIndex"},
		{"set_time_selection", "start"},
		{"set_time_selection", "end"},
		{"split_items", "position"},
		{"update_marker_or_region", "guid"}
	};

	for (const RequiredField& required : required_fields) {
		Json payload = minimal_payload_for(required.tool_name);
		payload.erase(required.field_name);

		INFO("tool: " << required.tool_name << " field: " << required.field_name);
		CHECK_FALSE(fields_read(required.tool_name, payload));
	}
}

TEST_CASE("a field of the wrong type is not readable")
{
	SECTION("a send index that arrived as a string does not become send zero")
	{
		Json payload = minimal_payload_for("remove_send");
		payload["sendIndex"] = "0";

		CHECK_FALSE(fields_read("remove_send", payload));
	}

	SECTION("a parent send state that arrived as a string does not become enabled")
	{
		Json payload = minimal_payload_for("set_parent_send");
		payload["enabled"] = "yes";

		CHECK_FALSE(fields_read("set_parent_send", payload));
	}

	SECTION("a fractional FX index is not a whole number")
	{
		Json payload = minimal_payload_for("remove_fx");
		payload["fxIndex"] = 1.5;

		CHECK_FALSE(fields_read("remove_fx", payload));
	}

	SECTION("but a whole number stored as a float is one, because the schema says so")
	{
		// JSON Schema's `integer` matches any number with a zero fractional part, so the
		// MCP Tool Server accepted this. Refusing it here would reject a valid call
		// inside a producer's session, which is what requirement 4.4 is about.
		Json payload = minimal_payload_for("remove_fx");
		payload["fxIndex"] = 2.0;

		CHECK(fields_read("remove_fx", payload));

		ToolInputProblem problem;
		ToolInputReader<Json> reader{&payload, problem};

		CHECK(sesh_ai::transport::read_remove_fx_fields(reader).fx_index == 2);
	}

	SECTION("an integer past what the tool layer holds is reported rather than wrapped")
	{
		Json payload = minimal_payload_for("remove_fx");
		payload["fxIndex"] = 4294967296LL;

		CHECK_FALSE(fields_read("remove_fx", payload));
	}

	SECTION("a marker position that is not a number is not read as zero")
	{
		Json payload = minimal_payload_for("create_marker");
		payload["position"] = Json::array();

		CHECK_FALSE(fields_read("create_marker", payload));
	}

	SECTION("an identifier that is present but empty names nothing")
	{
		Json payload = minimal_payload_for("delete_marker_or_region");
		payload["guid"] = "";

		CHECK_FALSE(fields_read("delete_marker_or_region", payload));
	}

	SECTION("a tempo point that is not an object is reported rather than skipped")
	{
		Json payload = minimal_payload_for("change_tempo_map");
		payload["points"] = Json::array({120.0});

		CHECK_FALSE(fields_read("change_tempo_map", payload));
	}

	SECTION("an item GUID array holding a number is reported")
	{
		Json payload = minimal_payload_for("delete_items");
		payload["itemGuids"] = Json::array({3});

		CHECK_FALSE(fields_read("delete_items", payload));
	}
}

TEST_CASE("a number that is not finite is not readable")
{
	// A position that is not a number would be written straight into REAPER, and an item
	// moved to infinity is somewhere the producer cannot see or select.
	Json payload = minimal_payload_for("split_items");
	payload["position"] = std::numeric_limits<double>::quiet_NaN();

	CHECK_FALSE(fields_read("split_items", payload));

	payload["position"] = std::numeric_limits<double>::infinity();

	CHECK_FALSE(fields_read("split_items", payload));
}

TEST_CASE("a value outside a closed enumeration is not readable")
{
	SECTION("a bus type nobody recognises does not build a summing bus")
	{
		Json payload = minimal_payload_for("create_bus");
		payload["type"] = "sumning";

		CHECK_FALSE(fields_read("create_bus", payload));
	}

	SECTION("a render source nobody recognises does not queue a master mix")
	{
		Json payload = minimal_payload_for("render");
		payload["source"] = "stem";

		CHECK_FALSE(fields_read("render", payload));
	}

	SECTION("a send mode nobody recognises does not become post fader")
	{
		Json payload = minimal_payload_for("create_send");
		payload["mode"] = "post-fader";

		CHECK_FALSE(fields_read("create_send", payload));
	}

	SECTION("a bit depth outside 16, 24, 32 is reported")
	{
		Json payload = minimal_payload_for("render");
		payload["bitDepth"] = 20;

		CHECK_FALSE(fields_read("render", payload));
	}

	SECTION("an item scope nobody recognises does not become selected items")
	{
		Json payload = Json::object();
		payload["scope"] = "selected";

		CHECK_FALSE(fields_read("list_selected_items", payload));
	}
}

TEST_CASE("an inverted range is readable, because the tool layer is what reports it")
{
	// Requirement 9.6 is `check_end_after_start`'s, in three writing contexts and the
	// three read windows. A second answer here would report a call that read perfectly
	// well as an unreadable input instead.
	Json region = minimal_payload_for("create_region");
	region["start"] = 9.0;
	region["end"] = 1.0;

	CHECK(fields_read("create_region", region));

	Json selection = minimal_payload_for("set_time_selection");
	selection["start"] = 8.0;
	selection["end"] = 2.0;

	CHECK(fields_read("set_time_selection", selection));
}

TEST_CASE("a numeric bound the input schema carries is the server's and is not restated")
{
	// `volumeDecibels` is bounded at -150 and 12 by `mcp-tool-defs.schema.json`. A bound
	// restated here is one that can drift from the authoritative copy in the direction
	// requirement 4.4 warns about, so a value past it is read and left to the tool layer.
	Json payload = minimal_payload_for("set_send_state");
	payload["volume"] = -400.0;

	CHECK(fields_read("set_send_state", payload));
}

TEST_CASE("a result cap below one is reported rather than converted")
{
	// `timeline_read_window::maximum_results` is unsigned, so a negative count does not
	// clamp — it becomes an enormous one and asks for every marker in the project.
	Json payload = Json::object();
	payload["maximumResults"] = -1;

	CHECK_FALSE(fields_read("list_markers", payload));
}

// ---------------------------------------------------------------------------
// What the readers produce
// ---------------------------------------------------------------------------

TEST_CASE("a colour reads as the value the tool layer holds")
{
	Json payload = minimal_payload_for("create_marker");
	payload["color"] = "#FF8000";

	ToolInputProblem problem;
	ToolInputReader<Json> reader{&payload, problem};

	const sesh_ai::daw::tools::create_marker_request request =
		sesh_ai::transport::read_create_marker_fields(reader);

	CHECK_FALSE(problem.anything_recorded());
	REQUIRE(request.color.has_value());
	CHECK(*request.color == 0xFF8000);

	SECTION("and a colour that is not #RRGGBB is reported rather than part-parsed")
	{
		Json malformed = minimal_payload_for("create_marker");
		malformed["color"] = "FF8000";

		CHECK_FALSE(fields_read("create_marker", malformed));

		malformed["color"] = "#FF80ZZ";

		CHECK_FALSE(fields_read("create_marker", malformed));
	}
}

TEST_CASE("an enumeration reads as the value it names rather than as a position")
{
	Json payload = minimal_payload_for("create_send");
	payload["mode"] = "pre_fx";
	payload["volume"] = -6.0;
	payload["pan"] = 25.0;

	ToolInputProblem problem;
	ToolInputReader<Json> reader{&payload, problem};

	const sesh_ai::daw::tools::create_send_request request =
		sesh_ai::transport::read_create_send_fields(reader);

	CHECK_FALSE(problem.anything_recorded());
	CHECK(request.mode == sesh_ai::daw::tools::routing_send_mode::pre_fx);
	CHECK(request.volume_decibels == -6.0);
	CHECK(request.pan_percent == 25.0);
}

TEST_CASE("a render request reads its source, bounds, and format")
{
	Json payload = minimal_payload_for("render");
	payload["source"] = "stems_via_master";
	payload["bounds"] = "region";
	payload["regionGuid"] = guid_of(4);
	payload["outputFormat"] = "flac";
	payload["bitDepth"] = 24;
	payload["sampleRate"] = 48000;

	ToolInputProblem problem;
	ToolInputReader<Json> reader{&payload, problem};

	const sesh_ai::daw::render_request request = sesh_ai::transport::read_render_fields(reader);

	CHECK_FALSE(problem.anything_recorded());
	CHECK(request.source == sesh_ai::daw::render_source::stems_via_master);
	CHECK(request.bounds == sesh_ai::daw::render_bounds::region);
	CHECK(request.region_guid == guid_of(4));
	CHECK(request.output_format == sesh_ai::daw::render_output_format::flac);
	REQUIRE(request.bit_depth.has_value());
	CHECK(*request.bit_depth == 24);
	REQUIRE(request.sample_rate.has_value());
	CHECK(*request.sample_rate == 48000);

	// Read by the framework rather than by the codec, so that `confirmedOverwrite` has
	// exactly one reader.
	CHECK_FALSE(request.confirmed_overwrite);
}

TEST_CASE("an absent optional field stays absent rather than becoming a default")
{
	Json payload = minimal_payload_for("update_marker_or_region");
	payload["name"] = "Bridge";

	ToolInputProblem problem;
	ToolInputReader<Json> reader{&payload, problem};

	const sesh_ai::daw::tools::update_marker_or_region_request request =
		sesh_ai::transport::read_update_marker_or_region_fields(reader);

	CHECK_FALSE(problem.anything_recorded());
	REQUIRE(request.name.has_value());
	CHECK(*request.name == "Bridge");
	CHECK_FALSE(request.position_seconds.has_value());
	CHECK_FALSE(request.end_seconds.has_value());
	CHECK_FALSE(request.color.has_value());
	CHECK(request.asks_for_any_change());
}

TEST_CASE("a tempo point keeps the preceding time signature when the call named none")
{
	Json carrying = Json::object();
	carrying["position"] = 4.0;
	carrying["tempo"] = 90.0;
	carrying["linearTransition"] = true;

	Json signature = Json::object();
	signature["numerator"] = 6;
	signature["denominator"] = 8;
	carrying["timeSignature"] = signature;

	Json bare = Json::object();
	bare["position"] = 0.0;
	bare["tempo"] = 120.0;

	Json payload = Json::object();
	payload["points"] = Json::array({bare, carrying});
	payload["replaceExisting"] = true;

	ToolInputProblem problem;
	ToolInputReader<Json> reader{&payload, problem};

	const sesh_ai::daw::tools::change_tempo_map_request request =
		sesh_ai::transport::read_change_tempo_map_fields(reader);

	CHECK_FALSE(problem.anything_recorded());
	CHECK(request.replace_existing);
	REQUIRE(request.points.size() == 2);
	CHECK_FALSE(request.points[0].time_signature.has_value());
	CHECK_FALSE(request.points[0].linear_transition);
	REQUIRE(request.points[1].time_signature.has_value());
	CHECK(request.points[1].time_signature->numerator == 6);
	CHECK(request.points[1].time_signature->denominator == 8);
	CHECK(request.points[1].linear_transition);
}

TEST_CASE("the two list defaults match the input schemas' own defaults")
{
	Json items = Json::object();

	ToolInputProblem items_problem;
	ToolInputReader<Json> items_reader{&items, items_problem};

	const sesh_ai::daw::tools::list_selected_items_request item_request =
		sesh_ai::transport::read_list_selected_items_fields(items_reader);

	CHECK(item_request.include_take_names);
	CHECK(item_request.scope == sesh_ai::daw::tools::item_scope::selected_items);

	Json tracks = Json::object();

	ToolInputProblem tracks_problem;
	ToolInputReader<Json> tracks_reader{&tracks, tracks_problem};

	const sesh_ai::daw::tools::list_tracks_request track_request =
		sesh_ai::transport::read_list_tracks_fields(tracks_reader);

	CHECK(track_request.include_fx_chain);
	CHECK(track_request.track_guids.empty());
}

TEST_CASE("a name pattern in the two GUID-keyed track state tools is reported, not guessed")
{
	// Both readers are handed the payload alone, and only the Object Resolver can turn a
	// pattern into a GUID. Dropping it would widen `list_tracks` to the whole project;
	// standing in for it would have `set_track_state` call a track unknown that
	// resolution had just proved exists.
	Json listing = Json::object();
	listing["tracks"] = Json::array({name_pattern_selector("drum bus")});

	CHECK_FALSE(fields_read("list_tracks", listing));
	CHECK(field_problem("list_tracks", listing).find("guid") != std::string::npos);

	Json change = Json::object();
	change["track"] = name_pattern_selector("lead vox");

	Json state = Json::object();
	state["changes"] = Json::array({change});

	CHECK_FALSE(fields_read("set_track_state", state));

	SECTION("and a GUID in the same position reads")
	{
		Json by_guid = Json::object();
		by_guid["tracks"] = Json::array({guid_selector(1), guid_selector(2)});

		ToolInputProblem problem;
		ToolInputReader<Json> reader{&by_guid, problem};

		const sesh_ai::daw::tools::list_tracks_request request =
			sesh_ai::transport::read_list_tracks_fields(reader);

		CHECK_FALSE(problem.anything_recorded());
		REQUIRE(request.track_guids.size() == 2);
		CHECK(request.track_guids[0] == guid_of(1));
		CHECK(request.track_guids[1] == guid_of(2));
	}
}

// ---------------------------------------------------------------------------
// The three seams, each reporting the way its own registration reports
// ---------------------------------------------------------------------------

TEST_CASE("the overload-set reader returns false and leaves the request alone")
{
	const ToolRequestReader read_request;

	Json payload = minimal_payload_for("set_fx_parameter");
	payload["fxIndex"] = "0";

	sesh_ai::daw::tools::set_fx_parameter_request request;
	request.fx_index = 7;
	request.parameter_name = "untouched";

	CHECK_FALSE(read_request(payload, request));

	// The handler checks the boolean and reports `fx_input_unreadable`; a request it was
	// told not to use must not have been written over on the way to being refused.
	CHECK(request.fx_index == 7);
	CHECK(request.parameter_name == "untouched");

	SECTION("and returns true with the request filled when the payload reads")
	{
		CHECK(read_request(minimal_payload_for("set_fx_parameter"), request));
		CHECK(request.fx_index == 0);
		CHECK(request.parameter_name == "Threshold");
		CHECK(request.value == -12.0);
	}
}

TEST_CASE("the overload-set reader has an overload for all nineteen request types")
{
	// `compose_tool_handler_registry` passes one `read_request` to fx, item, and track
	// state, and overload resolution on the request type is what picks the right member.
	// A request type with no overload, or one that resolves to the wrong member, is a
	// compile failure here rather than at the composition root in task 24.3 — and a
	// reader bound to the wrong tool's fields would read true against the wrong payload.
	const ToolRequestReader read_request;

	sesh_ai::daw::tools::add_fx_request add_fx;
	sesh_ai::daw::tools::remove_fx_request remove_fx;
	sesh_ai::daw::tools::set_fx_bypass_request set_fx_bypass;
	sesh_ai::daw::tools::set_fx_parameter_request set_fx_parameter;
	sesh_ai::daw::tools::get_fx_parameters_request get_fx_parameters;
	sesh_ai::daw::tools::list_track_fx_request list_track_fx;
	sesh_ai::daw::tools::list_installed_fx_request list_installed_fx;
	sesh_ai::daw::tools::apply_fx_destructively_request apply_fx_destructively;

	CHECK(read_request(minimal_payload_for("add_fx"), add_fx));
	CHECK(read_request(minimal_payload_for("remove_fx"), remove_fx));
	CHECK(read_request(minimal_payload_for("set_fx_bypass"), set_fx_bypass));
	CHECK(read_request(minimal_payload_for("set_fx_parameter"), set_fx_parameter));
	CHECK(read_request(minimal_payload_for("get_fx_parameters"), get_fx_parameters));
	CHECK(read_request(minimal_payload_for("list_track_fx"), list_track_fx));
	CHECK(read_request(minimal_payload_for("list_installed_fx"), list_installed_fx));
	CHECK(read_request(minimal_payload_for("apply_fx_destructively"), apply_fx_destructively));

	CHECK(add_fx.fx_identifier == "ReaComp");
	CHECK(remove_fx.fx_index == 2);
	CHECK(set_fx_bypass.bypassed);
	CHECK(get_fx_parameters.fx_index == 2);
	CHECK(list_installed_fx.search_term == "comp");
	CHECK_FALSE(apply_fx_destructively.replace_existing_take);

	sesh_ai::daw::tools::set_item_properties_request set_item_properties;
	sesh_ai::daw::tools::move_items_request move_items;
	sesh_ai::daw::tools::move_all_items_request move_all_items;
	sesh_ai::daw::tools::split_items_request split_items;
	sesh_ai::daw::tools::delete_items_request delete_items;
	sesh_ai::daw::tools::list_selected_items_request list_selected_items;
	sesh_ai::daw::tools::import_file_request import_file;

	CHECK(read_request(minimal_payload_for("set_item_properties"), set_item_properties));
	CHECK(read_request(minimal_payload_for("move_items"), move_items));
	CHECK(read_request(minimal_payload_for("move_all_items"), move_all_items));
	CHECK(read_request(minimal_payload_for("split_items"), split_items));
	CHECK(read_request(minimal_payload_for("delete_items"), delete_items));
	CHECK(read_request(minimal_payload_for("list_selected_items"), list_selected_items));
	CHECK(read_request(minimal_payload_for("import_audio_file"), import_file, "import_audio_file"));
	CHECK(read_request(minimal_payload_for("import_midi_file"), import_file, "import_midi_file"));

	REQUIRE(set_item_properties.changes.size() == 1);
	CHECK(set_item_properties.changes[0].item_guid == guid_of(3));
	CHECK_FALSE(set_item_properties.changes[0].names_a_change());
	CHECK_FALSE(move_items.offset_seconds.has_value());
	CHECK(move_all_items.offset_seconds == 2.5);
	CHECK(split_items.position_seconds == 6.0);
	REQUIRE(delete_items.item_guids.size() == 1);
	CHECK(import_file.file_path == "/tmp/take.wav");
	CHECK(import_file.position_seconds == 4.0);

	sesh_ai::daw::tools::set_track_state_request set_track_state;
	sesh_ai::daw::tools::list_tracks_request list_tracks;
	sesh_ai::daw::tools::get_project_summary_request get_project_summary;

	CHECK(read_request(minimal_payload_for("set_track_state"), set_track_state));
	CHECK(read_request(minimal_payload_for("list_tracks"), list_tracks));
	CHECK(read_request(minimal_payload_for("get_project_summary"), get_project_summary));

	REQUIRE(set_track_state.changes.size() == 1);
	CHECK(set_track_state.changes[0].track_guid == guid_of(1));
	CHECK(list_tracks.include_fx_chain);
}

TEST_CASE("the overload-set reader refuses a payload that is not an object")
{
	const ToolRequestReader read_request;

	const Json payload = Json::array();

	sesh_ai::daw::tools::list_track_fx_request track_fx;
	sesh_ai::daw::tools::get_project_summary_request summary;

	CHECK_FALSE(read_request(payload, track_fx));
	CHECK_FALSE(read_request(payload, summary));

	const Json empty_object = Json::object();

	CHECK(read_request(empty_object, track_fx));
	CHECK(read_request(empty_object, summary));
}

TEST_CASE("the routing readers hand back what the call named")
{
	sesh_ai::daw::tools::routing_payload_codec<Json> codec;
	sesh_ai::transport::bind_routing_request_readers(codec);

	REQUIRE(static_cast<bool>(codec.read_create_bus_request));

	Json payload = minimal_payload_for("create_bus");
	payload["type"] = "aux";
	payload["fxIdentifiers"] = Json::array({"ReaVerb", "ReaEQ"});
	payload["sendVolume"] = -3.0;

	const sesh_ai::daw::tools::bus_construction_request request = codec.read_create_bus_request(payload);

	CHECK(request.type == sesh_ai::daw::bus_type::aux);
	CHECK(request.bus_name == "Drum Bus");
	REQUIRE(request.fx_names.size() == 2);
	CHECK(request.fx_names[0] == "ReaVerb");
	CHECK(request.send_volume_decibels == -3.0);
}

TEST_CASE("the render reader binds the read side of the codec and nothing else")
{
	sesh_ai::daw::tools::render_payload_codec<Json> codec;
	sesh_ai::transport::bind_render_request_reader(codec);

	CHECK(static_cast<bool>(codec.read_render_request));

	// The write side is task 24.2's and is bound by `bind_render_result_writer`, which this
	// function deliberately does not reach: the two halves of a codec are populated by two
	// calls so that neither can quietly overwrite the other's members. Until the writer
	// runs, `register_render_tool` declines the codec rather than registering a render
	// that could never report a queued job — which is what keeps a half-built codec honest.
	//
	// That the writer then completes it is `tool_result_writers_test.cpp`'s case; what is
	// checked here is the half this binder owns.
	CHECK_FALSE(static_cast<bool>(codec.write_render_result));
}

TEST_CASE("a track structure reader substitutes an identifier the project cannot hold")
{
	// `register_track_structure_tools` calls its readers with no null-input guard, so the
	// guard is here. The substitution is reported by `apply_delete_track` as
	// `unknown_track` with nothing touched.
	sesh_ai::daw::tools::track_structure_payload_codec<Json> codec;
	sesh_ai::transport::bind_track_structure_request_readers(codec);

	ReaderFixture fixture;

	const sesh_ai::daw::tools::delete_track_request deletion =
		codec.read_delete_track_request(fixture.context());

	CHECK(deletion.track_guid == std::string{unreadable_tool_input_identifier});

	const sesh_ai::daw::tools::duplicate_track_request duplication =
		codec.read_duplicate_track_request(fixture.context());

	CHECK(duplication.track_guid == std::string{unreadable_tool_input_identifier});

	SECTION("and hands over the resolved GUID when the call resolved one")
	{
		fixture.resolve(5);

		CHECK(codec.read_delete_track_request(fixture.context()).track_guid == guid_of(5));
	}
}

TEST_CASE("create_track maps its two optional selectors by the order they were read in")
{
	sesh_ai::daw::tools::track_structure_payload_codec<Json> codec;
	sesh_ai::transport::bind_track_structure_request_readers(codec);

	SECTION("inside a folder only")
	{
		ReaderFixture fixture;
		Json payload = minimal_payload_for("create_track");
		payload["insideFolderOf"] = guid_selector(1);
		fixture.call.validated_input = &payload;
		fixture.resolve(1);

		const sesh_ai::daw::tools::create_track_request request =
			codec.read_create_track_request(fixture.context());

		CHECK(request.name == "Lead Vox");
		CHECK(request.after_track_guid.empty());
		CHECK(request.inside_folder_of_guid == guid_of(1));
	}

	SECTION("both, in the order the selectors were resolved")
	{
		ReaderFixture fixture;
		Json payload = minimal_payload_for("create_track");
		payload["afterTrack"] = guid_selector(1);
		payload["insideFolderOf"] = guid_selector(2);
		payload["color"] = "#102030";
		fixture.call.validated_input = &payload;
		fixture.resolve(1);
		fixture.resolve(2);

		const sesh_ai::daw::tools::create_track_request request =
			codec.read_create_track_request(fixture.context());

		CHECK(request.after_track_guid == guid_of(1));
		CHECK(request.inside_folder_of_guid == guid_of(2));
		REQUIRE(request.color.has_value());
		CHECK(*request.color == 0x102030);
	}

	SECTION("and a call with no readable input is reported rather than creating a track")
	{
		ReaderFixture fixture;

		CHECK_THROWS_AS(codec.read_create_track_request(fixture.context()), ToolInputNotReadable);
	}
}

TEST_CASE("set_folder_structure takes its destination from the selector after the array")
{
	sesh_ai::daw::tools::track_structure_payload_codec<Json> codec;
	sesh_ai::transport::bind_track_structure_request_readers(codec);

	ReaderFixture fixture;
	Json payload = Json::object();
	payload["tracks"] = Json::array({guid_selector(1), guid_selector(2)});
	payload["intoFolderOf"] = guid_selector(3);
	fixture.call.validated_input = &payload;
	fixture.resolve(1);
	fixture.resolve(2);
	fixture.resolve(3);

	const sesh_ai::daw::tools::set_folder_structure_request request =
		codec.read_set_folder_structure_request(fixture.context());

	REQUIRE(request.track_guids.size() == 2);
	CHECK(request.track_guids[0] == guid_of(1));
	CHECK(request.track_guids[1] == guid_of(2));
	CHECK(request.into_folder_of_guid == guid_of(3));
	CHECK_FALSE(request.move_to_top_level);
}

TEST_CASE("a marker, region, or read-window reader reports a call it cannot read")
{
	sesh_ai::daw::tools::marker_region_payload_codec<Json> mutating;
	sesh_ai::transport::bind_marker_region_request_readers(mutating);

	sesh_ai::daw::tools::marker_region_read_payload_codec<Json> reads;
	sesh_ai::transport::bind_marker_region_read_request_readers(reads);

	sesh_ai::daw::tools::tempo_payload_codec<Json> tempo;
	sesh_ai::transport::bind_tempo_request_readers(tempo);

	ReaderFixture fixture;

	// No input at all. `apply_create_marker` has no precondition that could fail, so a
	// default-constructed request would put an unnamed marker at position zero.
	CHECK_THROWS_AS(mutating.read_create_marker_request(fixture.context()), ToolInputNotReadable);
	CHECK_THROWS_AS(mutating.read_change_tempo_map_request(fixture.context()), ToolInputNotReadable);

	// The three read windows have no required field at all, which is exactly why the
	// absence of an input is checked before the fields are.
	CHECK_THROWS_AS(reads.read_list_markers_request(fixture.context()), ToolInputNotReadable);
	CHECK_THROWS_AS(reads.read_list_regions_request(fixture.context()), ToolInputNotReadable);
	CHECK_THROWS_AS(tempo.read_list_tempo_changes_request(fixture.context()), ToolInputNotReadable);

	SECTION("and names the field in the reason it carries")
	{
		Json payload = minimal_payload_for("create_marker");
		payload.erase("position");
		fixture.call.validated_input = &payload;

		try {
			mutating.read_create_marker_request(fixture.context());
			FAIL("the reader accepted a payload with no position");
		} catch (const ToolInputNotReadable& unreadable) {
			const std::string reason{unreadable.what()};

			CHECK(reason.find("create_marker") != std::string::npos);
			CHECK(reason.find("position") != std::string::npos);
		}
	}

	SECTION("and reads an empty object as the whole project")
	{
		Json payload = Json::object();
		fixture.call.validated_input = &payload;

		const sesh_ai::daw::tools::timeline_read_window window =
			reads.read_list_markers_request(fixture.context());

		CHECK_FALSE(window.start_position_seconds.has_value());
		CHECK_FALSE(window.end_position_seconds.has_value());
		CHECK(window.maximum_results == sesh_ai::daw::tools::default_listed_results);
	}
}

#else

TEST_CASE("the inbound tool readers need nlohmann/json")
{
	SKIP(
		"nlohmann/json is not on the include path, so the readers cannot be driven against "
		"the library they bind to. Configure with a vcpkg toolchain."
	);
}

#endif
