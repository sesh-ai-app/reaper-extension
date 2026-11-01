// The outbound writers: every tool's result, turned into the payload its bundled output
// schema describes (task 24.2, requirements 4.1, 9.2, 9.4, 20.1).
//
// ---------------------------------------------------------------------------
// Why this file drives the real validator rather than reading the schemas by eye
//
// The readers' tests could be written against the JSON library alone, because a reader's
// contract is "did this field read". A writer's contract is "does this payload satisfy a
// bundled schema", and the only thing that can answer is the schema validator the
// extension itself uses. So every writer here is driven end to end: the result struct, the
// per-tool writer, `write_dispatch_outcome`, and then `BundledSchemaValidator` against the
// path `written_outcome_schema_path` names — which is also the path
// `OutboundEnvelope::payload_schema_path_override` will carry.
//
// `BundledSchemaValidator`'s definitions live in `src/transport/schema_validator.cpp`,
// which `tests/CMakeLists.txt` does not compile. `schema_validation_property_test.cpp`
// includes that translation unit for Properties 12 and 13 and this file links against what
// it compiled — including it a second time would be a duplicate definition. Both files gate
// on the same two `__has_include` checks, so the symbols exist in exactly the builds where
// these cases compile.
//
// ---------------------------------------------------------------------------
// Validating is not the whole bar, and the second case below is the reason
//
// "Every written payload validates" is satisfied by a writer that emits only the required
// fields, and the task names why that is not good enough: from the server's side, a field
// the schema permits and the writer never emits is indistinguishable from a tool that
// failed. An agent reading a `list_tracks` result with no `aliases` cannot tell whether the
// tracks hold none or whether this build does not report them.
//
// So `the writers emit every field the output schemas declare` walks each schema for every
// property path it declares — through `$ref`, through `oneOf`, through array items — and
// checks it against the paths the written payload actually carries. The fixtures populate
// every optional field for that reason: a fixture that left one empty would make the
// coverage check pass by asking for less.
//
// One path is allowed to be missing and is named at the allowance, which is the whole point
// of listing it rather than relaxing the check.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/schema_validator.h>

#if __has_include(<nlohmann/json.hpp>)
	#define SESH_AI_TESTS_HAVE_JSON 1
#else
	#define SESH_AI_TESTS_HAVE_JSON 0
#endif

#if __has_include(<nlohmann/json.hpp>) && __has_include(<nlohmann/json-schema.hpp>)
	#define SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION 1
#else
	#define SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION 0
#endif

#if SESH_AI_TESTS_HAVE_JSON
	#include <nlohmann/json.hpp>

	#include <transport/envelope.h>
	#include <transport/message_dispatcher.h>
	#include <transport/queue_pair.h>
	#include <transport/tool_outcome_writer.h>
	#include <transport/tool_request_readers.h>
	#include <transport/tool_result_writers.h>

	#include <entry/extension_composition.h>
#endif

#if SESH_AI_TESTS_HAVE_JSON

namespace {

	using Json = nlohmann::json;

	using sesh_ai::daw::BlockingEntity;
	using sesh_ai::daw::TrackReference;
	using sesh_ai::daw::dispatch_outcome;
	using sesh_ai::daw::failed_action;
	using sesh_ai::daw::succeeded_action;
	using sesh_ai::daw::tool_partial_outcome;
	using sesh_ai::daw::tool_refusal;
	using sesh_ai::daw::tool_result;
	using sesh_ai::daw::tool_success;
	using sesh_ai::daw::undo_report;
	using sesh_ai::transport::ToolResultWriter;
	using sesh_ai::transport::write_dispatch_outcome;
	using sesh_ai::transport::written_color_string;
	using sesh_ai::transport::written_outcome_schema_path;

	// ---------------------------------------------------------------------------
	// Values the schemas constrain
	// ---------------------------------------------------------------------------

	// A braced GUID of the form `guidToString` produces and the output schemas'
	// `^\{[0-9A-Fa-f-]{36}\}$` pattern accepts, distinguished by one digit.
	std::string guid_of(int distinguisher)
	{
		std::string guid{"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEE"};
		guid += static_cast<char>('0' + distinguisher);
		guid += '}';

		return guid;
	}

	TrackReference sample_track_reference(int distinguisher, std::string name)
	{
		return TrackReference{guid_of(distinguisher), std::move(name)};
	}

	sesh_ai::daw::tools::folder_depth_repair sample_folder_repair(int distinguisher)
	{
		sesh_ai::daw::tools::folder_depth_repair repair;
		repair.guid = guid_of(distinguisher);
		repair.name = "Repaired";
		repair.previous_folder_depth = 1;
		repair.folder_depth = 0;

		return repair;
	}

	sesh_ai::daw::tools::item_placement sample_item_placement(int distinguisher)
	{
		sesh_ai::daw::tools::item_placement placement;
		placement.guid = guid_of(distinguisher);
		placement.track_guid = guid_of(1);
		placement.position_seconds = 4.0;
		placement.length_seconds = 2.5;
		placement.name = "Take 3";

		return placement;
	}

	sesh_ai::daw::tools::routing_send sample_routing_send(int distinguisher)
	{
		sesh_ai::daw::tools::routing_send send;
		send.index = 0;
		send.track_guid = guid_of(distinguisher);
		send.track_name = "Reverb";
		send.volume_decibels = -6.0;
		send.pan_percent = 25.0;
		send.muted = false;
		send.mode = sesh_ai::daw::tools::routing_send_mode::pre_fx;

		return send;
	}

	sesh_ai::daw::tools::marker_detail sample_marker_detail()
	{
		sesh_ai::daw::tools::marker_detail marker;
		marker.guid = guid_of(4);
		marker.index = 2;
		marker.position_seconds = 12.5;
		marker.name = "Chorus";
		marker.color = 0x102030;

		return marker;
	}

	sesh_ai::daw::tools::region_detail sample_region_detail()
	{
		sesh_ai::daw::tools::region_detail region;
		region.guid = guid_of(5);
		region.index = 1;
		region.start_seconds = 8.0;
		region.end_seconds = 24.0;
		region.name = "Verse";
		region.color = 0xA0B0C0;

		return region;
	}

	sesh_ai::daw::tools::fx_chain_entry sample_fx_chain_entry()
	{
		sesh_ai::daw::tools::fx_chain_entry entry;
		entry.index = 1;
		entry.name = "ReaComp";
		entry.bypassed = true;
		entry.offline = true;

		return entry;
	}

	sesh_ai::daw::tools::fx_parameter sample_fx_parameter()
	{
		sesh_ai::daw::tools::fx_parameter parameter;
		parameter.index = 3;
		parameter.name = "Threshold";
		parameter.value = -18.0;
		parameter.minimum_value = -60.0;
		parameter.maximum_value = 0.0;
		parameter.formatted_value = "-18.0 dB";

		return parameter;
	}

	// ---------------------------------------------------------------------------
	// The bundle, and the validator the extension itself uses
	// ---------------------------------------------------------------------------

	// The same walk `schema_validator_test.cpp` and `schema_validation_property_test.cpp`
	// do, identifying the bundle by `MANIFEST.json` rather than by the directory name —
	// see the comment on that file's copy for the hand-compiled case this guards against.
	std::filesystem::path locate_schema_directory()
	{
		const std::filesystem::path schema_directory =
			std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "schemas";

		if (!std::filesystem::is_regular_file(schema_directory / "MANIFEST.json")) {
			return {};
		}

		return schema_directory;
	}

	// ---------------------------------------------------------------------------
	// Property paths, for the coverage check
	// ---------------------------------------------------------------------------

	// Follows a local `#/definitions/...` fragment. Every reference in the bundle is local
	// — the bundler's resolver rewrites cross-document ones — so there is nothing to fetch
	// and nothing to fail on.
	const Json& resolve_local_reference(const Json& node, const Json& root, int depth)
	{
		if (depth > 16 || !node.is_object() || !node.contains("$ref")) {
			return node;
		}

		const std::string reference = node.at("$ref").get<std::string>();

		if (reference.rfind("#/", 0) != 0) {
			return node;
		}

		const Json& target = root.at(Json::json_pointer{reference.substr(1)});

		return resolve_local_reference(target, root, depth + 1);
	}

	// Every property path a schema declares, with array positions written as `[]`.
	//
	// Descends `oneOf`, `anyOf`, and `allOf` branches as well as `properties` and `items`,
	// so a `oneOf` schema declares the union of its branches — which is what the two
	// marker-or-region tools need, since no single payload can satisfy both.
	void collect_declared_paths(
		const Json& node,
		const Json& root,
		const std::string& prefix,
		std::set<std::string>& paths,
		int depth)
	{
		if (depth > 16) {
			return;
		}

		const Json& resolved = resolve_local_reference(node, root, 0);

		if (!resolved.is_object()) {
			return;
		}

		for (const char* combinator : {"oneOf", "anyOf", "allOf"}) {
			if (!resolved.contains(combinator)) {
				continue;
			}

			for (const Json& branch : resolved.at(combinator)) {
				collect_declared_paths(branch, root, prefix, paths, depth + 1);
			}
		}

		if (resolved.contains("properties")) {
			for (const auto& property : resolved.at("properties").items()) {
				const std::string path = prefix + "/" + property.key();

				paths.insert(path);
				collect_declared_paths(property.value(), root, path, paths, depth + 1);
			}
		}

		if (resolved.contains("items")) {
			collect_declared_paths(resolved.at("items"), root, prefix + "/[]", paths, depth + 1);
		}
	}

	// Every property path a payload carries, written the same way.
	void collect_present_paths(
		const Json& node,
		const std::string& prefix,
		std::set<std::string>& paths)
	{
		if (node.is_object()) {
			for (const auto& property : node.items()) {
				const std::string path = prefix + "/" + property.key();

				paths.insert(path);
				collect_present_paths(property.value(), path, paths);
			}

			return;
		}

		if (node.is_array()) {
			for (const Json& element : node) {
				collect_present_paths(element, prefix + "/[]", paths);
			}
		}
	}

	// ---------------------------------------------------------------------------
	// One tool's written payloads
	// ---------------------------------------------------------------------------

	// Every payload the named tool can produce as a success, and whether the framework
	// attaches an undo report to it.
	//
	// Several tools need more than one payload to cover their schema: `move_all_items`
	// reports either a track list or the whole project, and the two marker-or-region tools
	// are a `oneOf` whose branches no single payload satisfies.
	struct WrittenToolResult {
		std::string tool_name;
		std::vector<Json> payloads;
		bool attaches_undo_report = false;
	};

	undo_report mutating_undo_report()
	{
		return undo_report{7, "Sesh AI: a tool"};
	}

	// Writes one success through `write_dispatch_outcome`, which is the function the
	// composition root holds and the only thing that attaches the undo report.
	Json written_success(std::string_view tool_name, Json fields, bool attaches_undo_report)
	{
		tool_success<Json> success;
		success.tool_name = std::string{tool_name};
		success.fields = std::move(fields);

		if (attaches_undo_report) {
			success.undo = mutating_undo_report();
		}

		const dispatch_outcome<Json> outcome{tool_result<Json>{std::move(success)}};

		return write_dispatch_outcome<Json>(outcome);
	}

	// The schema path a success for this tool is validated against, which is also the
	// override the outbound envelope carries.
	std::string success_schema_path(std::string_view tool_name)
	{
		tool_success<Json> success;
		success.tool_name = std::string{tool_name};

		const dispatch_outcome<Json> outcome{tool_result<Json>{std::move(success)}};

		return written_outcome_schema_path<Json>(outcome);
	}

	// --- track structure ---

	Json written_create_track()
	{
		sesh_ai::daw::tools::create_track_result result;
		result.track = sample_track_reference(1, "Lead Vox");
		result.index = 3;
		result.accumulated_depth = 1;
		result.folder_depth_repairs.push_back(sample_folder_repair(2));

		return sesh_ai::transport::write_create_track_result<Json>(result);
	}

	Json written_delete_track()
	{
		sesh_ai::daw::tools::delete_track_result result;
		result.deleted_track = sample_track_reference(1, "Drum Bus");
		result.deleted_item_count = 4;
		result.deleted_fx_count = 2;
		result.folder_dissolved = true;
		result.reparented_tracks.push_back(sample_track_reference(2, "Kick"));
		result.folder_depth_repairs.push_back(sample_folder_repair(3));

		return sesh_ai::transport::write_delete_track_result<Json>(result);
	}

	Json written_duplicate_track()
	{
		sesh_ai::daw::tools::duplicate_track_result result;
		result.source_track = sample_track_reference(1, "Snare");
		result.new_track = sample_track_reference(2, "Snare copy");
		result.index = 4;
		result.copied_item_count = 3;
		result.copied_fx_count = 1;
		result.copied_send_count = 2;
		result.folder_depth_repairs.push_back(sample_folder_repair(3));

		return sesh_ai::transport::write_duplicate_track_result<Json>(result);
	}

	Json written_set_folder_structure()
	{
		sesh_ai::daw::tools::moved_track moved;
		moved.guid = guid_of(1);
		moved.name = "Kick";
		moved.previous_accumulated_depth = 0;
		moved.accumulated_depth = 1;
		moved.index = 2;

		sesh_ai::daw::tools::set_folder_structure_result result;
		result.tracks.push_back(moved);
		result.folder_parent = sample_track_reference(3, "Drums");
		result.moved_to_top_level = false;
		result.folder_depth_repairs.push_back(sample_folder_repair(4));

		return sesh_ai::transport::write_set_folder_structure_result<Json>(result);
	}

	// --- markers, regions, tempo, time selection ---

	Json written_create_marker()
	{
		sesh_ai::daw::tools::create_marker_result result;
		result.marker = sample_marker_detail();

		return sesh_ai::transport::write_create_marker_result<Json>(result);
	}

	Json written_create_region()
	{
		sesh_ai::daw::tools::create_region_result result;
		result.region = sample_region_detail();

		return sesh_ai::transport::write_create_region_result<Json>(result);
	}

	Json written_update_marker_or_region(bool as_region)
	{
		sesh_ai::daw::tools::update_marker_or_region_result result;

		if (as_region) {
			result.updated = sample_region_detail();
		} else {
			result.updated = sample_marker_detail();
		}

		return sesh_ai::transport::write_update_marker_or_region_result<Json>(result);
	}

	Json written_delete_marker_or_region(bool as_region)
	{
		sesh_ai::daw::tools::delete_marker_or_region_result result;

		if (as_region) {
			result.deleted = sample_region_detail();
		} else {
			result.deleted = sample_marker_detail();
		}

		return sesh_ai::transport::write_delete_marker_or_region_result<Json>(result);
	}

	Json written_change_tempo_map()
	{
		sesh_ai::daw::tools::change_tempo_map_result result;
		result.points_written = 8;
		result.points_replaced = 3;
		result.replaced_existing = true;
		result.tempo_change_count = 8;
		result.moved_item_count = 12;

		return sesh_ai::transport::write_change_tempo_map_result<Json>(result);
	}

	Json written_set_time_selection()
	{
		sesh_ai::daw::tools::set_time_selection_result result;
		result.start_seconds = 4.0;
		result.end_seconds = 20.0;
		result.length_seconds = 16.0;

		return sesh_ai::transport::write_set_time_selection_result<Json>(result);
	}

	Json written_list_markers()
	{
		sesh_ai::daw::tools::list_markers_result result;
		result.markers.push_back(sample_marker_detail());
		result.total_in_range = 3;
		result.truncated = true;

		return sesh_ai::transport::write_list_markers_result<Json>(result);
	}

	Json written_list_regions()
	{
		sesh_ai::daw::tools::list_regions_result result;
		result.regions.push_back(sample_region_detail());
		result.total_in_range = 2;
		result.truncated = false;

		return sesh_ai::transport::write_list_regions_result<Json>(result);
	}

	Json written_list_tempo_changes()
	{
		sesh_ai::daw::tools::tempo_change_entry entry;
		entry.position_seconds = 0.0;
		entry.beats_per_minute = 128.0;
		entry.time_signature.numerator = 7;
		entry.time_signature.denominator = 8;
		entry.linear_transition = true;

		sesh_ai::daw::tools::list_tempo_changes_result result;
		result.tempo_changes.push_back(entry);
		result.total_in_range = 1;
		result.truncated = false;

		return sesh_ai::transport::write_list_tempo_changes_result<Json>(result);
	}

	// --- routing ---

	Json written_create_send()
	{
		sesh_ai::daw::tools::create_send_result result;
		result.source_track = sample_track_reference(1, "Snare");
		result.destination_track = sample_track_reference(2, "Reverb");
		result.send_index = 0;
		result.volume_decibels = -12.0;
		result.pan_percent = -25.0;
		result.mode = sesh_ai::daw::tools::routing_send_mode::post_fx;

		return sesh_ai::transport::write_create_send_result<Json>(result);
	}

	Json written_remove_send()
	{
		sesh_ai::daw::tools::remove_send_result result;
		result.source_track = sample_track_reference(1, "Snare");
		result.destination_track = sample_track_reference(2, "Reverb");
		result.send_index = 1;
		result.remaining_send_count = 2;

		return sesh_ai::transport::write_remove_send_result<Json>(result);
	}

	Json written_set_send_state()
	{
		sesh_ai::daw::tools::set_send_state_result result;
		result.source_track = sample_track_reference(1, "Snare");
		result.destination_track = sample_track_reference(2, "Reverb");
		result.send = sample_routing_send(2);

		return sesh_ai::transport::write_set_send_state_result<Json>(result);
	}

	Json written_set_parent_send()
	{
		sesh_ai::daw::tools::parent_send_change change;
		change.track = sample_track_reference(1, "Kick");
		change.previous_enabled = true;
		change.targets_folder_parent = true;
		change.target_guid = guid_of(2);

		sesh_ai::daw::tools::set_parent_send_result result;
		result.tracks.push_back(change);
		result.enabled = false;
		result.silenced_folder_parents.push_back(sample_track_reference(2, "Drums"));

		return sesh_ai::transport::write_set_parent_send_result<Json>(result);
	}

	Json written_create_bus()
	{
		sesh_ai::daw::tools::routing_fx_placement placement;
		placement.index = 0;
		placement.name = "ReaVerb";
		placement.bypassed = false;

		sesh_ai::daw::tools::created_send send;
		send.source_track_guid = guid_of(1);
		send.send_index = 0;
		send.volume_decibels = -3.0;
		send.mode = sesh_ai::daw::tools::routing_send_mode::post_fader;

		sesh_ai::daw::tools::create_bus_result result;
		result.bus_track = sample_track_reference(3, "Reverb Bus");
		result.type = sesh_ai::daw::bus_type::aux;
		result.index = 5;
		result.source_tracks.push_back(sample_track_reference(1, "Snare"));
		result.fx.push_back(placement);
		result.sends.push_back(send);
		result.folder_depth_repairs.push_back(sample_folder_repair(4));

		return sesh_ai::transport::write_create_bus_result<Json>(result);
	}

	Json written_remove_bus()
	{
		sesh_ai::daw::tools::remove_bus_result result;
		result.removed_bus = sample_track_reference(3, "Reverb Bus");
		result.type = sesh_ai::daw::bus_type::summing;
		result.removed_fx_count = 2;
		result.reparented_tracks.push_back(sample_track_reference(1, "Snare"));
		result.removed_sends.push_back(sample_track_reference(2, "Kick"));
		result.folder_depth_repairs.push_back(sample_folder_repair(4));

		return sesh_ai::transport::write_remove_bus_result<Json>(result);
	}

	Json written_get_routing()
	{
		sesh_ai::daw::tools::track_routing_report report;
		report.track = sample_track_reference(1, "Snare");
		report.role = sesh_ai::context::StructuralRole::aux_return;
		report.folder_depth_delta = 1;
		report.accumulated_folder_depth = 0;
		report.folder_parent_guid = guid_of(2);
		report.parent_send_enabled = false;
		report.sends.push_back(sample_routing_send(2));
		report.receives.push_back(sample_routing_send(3));
		report.item_count = 4;
		report.fx_count = 2;

		sesh_ai::daw::tools::get_routing_result result;
		result.tracks.push_back(report);
		result.master_track.present = true;
		result.master_track.track = sample_track_reference(9, "MASTER");
		result.master_track.role = sesh_ai::context::StructuralRole::master;
		result.master_track.receives.push_back(sample_routing_send(1));
		result.master_track.fx_count = 1;

		return sesh_ai::transport::write_get_routing_result<Json>(result);
	}

	// --- render ---

	Json written_render()
	{
		sesh_ai::daw::render_result result;
		result.queued_job_count = 2;
		result.source = sesh_ai::daw::render_source::stems;
		result.bounds = sesh_ai::daw::render_bounds::region;
		result.start_seconds = 8.0;
		result.end_seconds = 24.0;
		result.region_guid = guid_of(5);
		result.tracks.push_back(sesh_ai::daw::render_track{guid_of(1), "Snare"});
		result.output_directory = "/Users/producer/Renders";
		result.output_paths.push_back("/Users/producer/Renders/Snare.wav");
		result.output_format = sesh_ai::daw::render_output_format::wav;
		result.bit_depth = 24;
		result.sample_rate = 48000;

		return sesh_ai::transport::write_render_result<Json>(result);
	}

	// --- the two rewinding tools ---

	Json written_undo_last_action()
	{
		sesh_ai::daw::undo_last_action_outcome outcome;
		outcome.performed = true;
		outcome.undone_entry_descriptions.push_back("Sesh AI: create_track");
		outcome.undo_stack_position_after = 6;
		outcome.was_agent_entry = true;

		return sesh_ai::transport::write_undo_last_action_result<Json>(outcome);
	}

	Json written_revert_agent_changes()
	{
		sesh_ai::daw::revert_outcome outcome;
		outcome.performed = true;
		outcome.reverted_entry_count = 3;
		outcome.undone_entry_descriptions.push_back("Sesh AI: add_fx");
		outcome.undo_position_marker = 4;
		outcome.undo_stack_position_after = 4;
		outcome.reverted_foreign_entries = false;

		return sesh_ai::transport::write_revert_agent_changes_result<Json>(outcome);
	}

	// --- fx, through the overload set the three families share ---

	Json written_add_fx()
	{
		sesh_ai::daw::tools::add_fx_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx_index = 2;
		result.fx_name = "ReaComp";
		result.chain_length = 3;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_remove_fx()
	{
		sesh_ai::daw::tools::remove_fx_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx_index = 1;
		result.fx_name = "ReaEQ";
		result.chain_length = 2;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_set_fx_bypass()
	{
		sesh_ai::daw::tools::set_fx_bypass_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx_index = 0;
		result.fx_name = "ReaComp";
		result.bypassed = true;
		result.previous_bypassed = false;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_set_fx_parameter()
	{
		sesh_ai::daw::tools::set_fx_parameter_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx_index = 0;
		result.fx_name = "ReaComp";
		result.parameter_name = "Threshold";
		result.value = -18.0;
		result.previous_value = -12.0;
		result.clamped = true;
		result.formatted_value = "-18.0 dB";

		return ToolResultWriter<Json>{}(result);
	}

	Json written_get_fx_parameters()
	{
		sesh_ai::daw::tools::get_fx_parameters_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx_index = 0;
		result.fx_name = "ReaComp";
		result.parameters.push_back(sample_fx_parameter());
		result.total_in_range = 12;
		result.truncated = true;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_list_track_fx()
	{
		sesh_ai::daw::tools::list_track_fx_result result;
		result.track = sample_track_reference(1, "Snare");
		result.fx.push_back(sample_fx_chain_entry());

		return ToolResultWriter<Json>{}(result);
	}

	Json written_list_installed_fx()
	{
		sesh_ai::daw::tools::installed_fx installed;
		installed.identifier = "VST3:ReaComp";
		installed.name = "ReaComp";
		installed.vendor = "Cockos";
		installed.format = "vst3";
		installed.is_instrument = false;

		sesh_ai::daw::tools::list_installed_fx_result result;
		result.matches.push_back(installed);
		result.total_in_range = 40;
		result.truncated = true;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_apply_fx_destructively()
	{
		sesh_ai::daw::tools::apply_fx_destructively_result result;
		result.track = sample_track_reference(1, "Snare");
		result.processed_item_guids.push_back(guid_of(6));
		result.replaced_existing_take = true;
		result.applied_fx_count = 2;

		return ToolResultWriter<Json>{}(result);
	}

	// --- items ---

	Json written_set_item_properties()
	{
		sesh_ai::daw::tools::set_item_properties_result result;
		result.actions.push_back(succeeded_action(guid_of(6)));
		result.actions.push_back(
			failed_action(guid_of(7), "unknown_item", "no item in the project holds that guid"));
		result.items.push_back(sample_item_placement(6));

		return ToolResultWriter<Json>{}(result);
	}

	Json written_move_items()
	{
		sesh_ai::daw::tools::move_items_result result;
		result.items.push_back(sample_item_placement(6));
		result.applied_offset_seconds = 1.5;
		result.clamped_item_count = 1;
		result.to_track = sample_track_reference(2, "Snare Double");

		return ToolResultWriter<Json>{}(result);
	}

	Json written_move_all_items(bool whole_project)
	{
		sesh_ai::daw::tools::move_all_items_result result;
		result.whole_project = whole_project;
		result.moved_item_count = 9;
		result.applied_offset_seconds = -2.0;
		result.clamped_item_count = 2;

		if (!whole_project) {
			result.tracks.push_back(sample_track_reference(1, "Snare"));
		}

		return ToolResultWriter<Json>{}(result);
	}

	Json written_split_items()
	{
		sesh_ai::daw::tools::item_split split;
		split.original_item_guid = guid_of(6);
		split.left_item = sample_item_placement(7);
		split.right_item = sample_item_placement(8);

		sesh_ai::daw::tools::split_items_result result;
		result.splits.push_back(split);
		result.skipped_item_guids.push_back(guid_of(9));
		result.position_seconds = 6.0;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_delete_items()
	{
		sesh_ai::daw::tools::delete_items_result result;
		result.deleted_item_guids.push_back(guid_of(6));
		result.affected_tracks.push_back(sample_track_reference(1, "Snare"));
		result.deleted_length_seconds = 12.5;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_list_selected_items()
	{
		sesh_ai::daw::tools::selected_item item;
		item.guid = guid_of(6);
		item.track_guid = guid_of(1);
		item.track_name = "Snare";
		item.position_seconds = 4.0;
		item.length_seconds = 2.0;
		item.take_name = "Snare 03";
		item.muted = true;
		item.selected = true;

		sesh_ai::daw::tools::list_selected_items_result result;
		result.scope = sesh_ai::daw::tools::item_scope::time_selection;
		result.items.push_back(item);
		result.total_in_range = 8;
		result.truncated = true;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_import_audio_file()
	{
		sesh_ai::daw::tools::import_audio_file_result result;
		result.track = sample_track_reference(1, "Snare");
		result.item = sample_item_placement(6);
		result.file_path = "/Users/producer/takes/snare.wav";
		result.sample_rate = 44100;
		result.channel_count = 2;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_import_midi_file()
	{
		sesh_ai::daw::tools::import_midi_file_result result;
		result.track = sample_track_reference(1, "Keys");
		result.item = sample_item_placement(6);
		result.file_path = "/Users/producer/takes/keys.mid";
		result.imported_track_count = 2;
		result.note_count = 96;

		return ToolResultWriter<Json>{}(result);
	}

	// --- track state ---

	Json written_set_track_state()
	{
		sesh_ai::daw::tools::set_track_state_result result;
		result.actions.push_back(succeeded_action(guid_of(1)));

		// Both action shapes, because `actionOutcome` is a `oneOf` and a fixture carrying
		// only the succeeded branch would leave the error branch's three fields unchecked
		// by the coverage case below.
		result.actions.push_back(
			failed_action(guid_of(2), "unknown_track", "no track in the project holds that guid"));
		result.aliases_learned.push_back(sample_track_reference(1, "Snare"));
		result.project_saved = false;

		return ToolResultWriter<Json>{}(result);
	}

	Json written_list_tracks()
	{
		sesh_ai::daw::tools::reported_track_state track;
		track.guid = guid_of(1);
		track.index = 0;
		track.name = "Snare";
		track.aliases.push_back("the crack");
		track.role = sesh_ai::context::StructuralRole::normal;
		track.volume_decibels = -3.0;
		track.pan_percent = 15.0;
		track.muted = false;
		track.soloed = true;
		track.armed = true;
		track.item_count = 6;
		track.color = 0x112233;
		track.fx_chain = std::vector<std::string>{"ReaComp", "ReaEQ"};

		sesh_ai::daw::tools::list_tracks_result result;
		result.tracks.push_back(track);

		return ToolResultWriter<Json>{}(result);
	}

	Json written_get_project_summary()
	{
		sesh_ai::daw::tools::project_summary_result result;
		result.project_name = "Session 12";
		result.tempo = 128.0;
		result.time_signature_numerator = 7;
		result.time_signature_denominator = 8;
		result.sample_rate = 48000;
		result.track_count = 12;
		result.project_length = 240.5;
		result.marker_count = 4;
		result.region_count = 3;
		result.tempo_change_count = 2;

		return sesh_ai::transport::write_project_summary_result<Json>(result);
	}

	// ---------------------------------------------------------------------------
	// The table
	//
	// All 42, in the order `schema_validator.h`'s binding table spells them, so a tool
	// missing from here is visible against that list rather than against memory. The
	// `attaches_undo_report` column is the undo-effect classification restated as data, and
	// the case below cross-checks it against whether each schema declares
	// `undoPositionBefore` — which is what catches a tool registered through the wrong path.
	// ---------------------------------------------------------------------------

	std::vector<WrittenToolResult> every_written_tool_result()
	{
		std::vector<WrittenToolResult> results;

		const auto add = [&results](
			std::string_view tool_name,
			std::vector<Json> fields,
			bool attaches_undo_report) {
			WrittenToolResult written;
			written.tool_name = std::string{tool_name};
			written.attaches_undo_report = attaches_undo_report;

			for (Json& payload : fields) {
				written.payloads.push_back(
					written_success(tool_name, std::move(payload), attaches_undo_report));
			}

			results.push_back(std::move(written));
		};

		add("add_fx", {written_add_fx()}, true);
		add("apply_fx_destructively", {written_apply_fx_destructively()}, true);
		add("change_tempo_map", {written_change_tempo_map()}, true);
		add("create_bus", {written_create_bus()}, true);
		add("create_marker", {written_create_marker()}, true);
		add("create_region", {written_create_region()}, true);
		add("create_send", {written_create_send()}, true);
		add("create_track", {written_create_track()}, true);
		add("delete_items", {written_delete_items()}, true);
		add(
			"delete_marker_or_region",
			{written_delete_marker_or_region(false), written_delete_marker_or_region(true)},
			true);
		add("delete_track", {written_delete_track()}, true);
		add("duplicate_track", {written_duplicate_track()}, true);
		add("get_fx_parameters", {written_get_fx_parameters()}, false);
		add("get_project_summary", {written_get_project_summary()}, false);
		add("get_routing", {written_get_routing()}, false);
		add("import_audio_file", {written_import_audio_file()}, true);
		add("import_midi_file", {written_import_midi_file()}, true);
		add("list_installed_fx", {written_list_installed_fx()}, false);
		add("list_markers", {written_list_markers()}, false);
		add("list_regions", {written_list_regions()}, false);
		add("list_selected_items", {written_list_selected_items()}, false);
		add("list_tempo_changes", {written_list_tempo_changes()}, false);
		add("list_track_fx", {written_list_track_fx()}, false);
		add("list_tracks", {written_list_tracks()}, false);
		add(
			"move_all_items",
			{written_move_all_items(false), written_move_all_items(true)},
			true);
		add("move_items", {written_move_items()}, true);
		add("remove_bus", {written_remove_bus()}, true);
		add("remove_fx", {written_remove_fx()}, true);
		add("remove_send", {written_remove_send()}, true);

		// `render`'s undo effect is `none` (requirement 12.6) even though the tool is
		// classified as mutating, and `outputs/render.schema.json` is closed with no undo
		// report — so a report attached here would fail the validation below.
		add("render", {written_render()}, false);

		add("revert_agent_changes", {written_revert_agent_changes()}, false);
		add("set_folder_structure", {written_set_folder_structure()}, true);
		add("set_fx_bypass", {written_set_fx_bypass()}, true);
		add("set_fx_parameter", {written_set_fx_parameter()}, true);
		add("set_item_properties", {written_set_item_properties()}, true);
		add("set_parent_send", {written_set_parent_send()}, true);
		add("set_send_state", {written_set_send_state()}, true);
		add("set_time_selection", {written_set_time_selection()}, true);
		add("set_track_state", {written_set_track_state()}, true);
		add("split_items", {written_split_items()}, true);
		add("undo_last_action", {written_undo_last_action()}, false);
		add(
			"update_marker_or_region",
			{written_update_marker_or_region(false), written_update_marker_or_region(true)},
			true);

		return results;
	}

	// The one declared path in the 42 that no writer fills, and why.
	//
	// Listed rather than relaxed, so adding a second one is a deliberate edit to this table
	// with a sentence attached rather than a coverage check that quietly asks for less.
	const std::map<std::string, std::set<std::string>>& paths_no_writer_fills()
	{
		// `create-bus.schema.json` permits `offline` on each FX the bus was built with.
		// `routing_fx_placement` carries no offline flag, because the FX in a `create_bus`
		// result were inserted by that same call and an FX REAPER has just loaded is not
		// offline — a struct field would be a second thing to keep true.
		static const std::map<std::string, std::set<std::string>> allowed{
			{"create_bus", {"/fx/[]/offline"}}
		};

		return allowed;
	}

}

// ---------------------------------------------------------------------------
// The writers, against the real validator
// ---------------------------------------------------------------------------

#if SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION

TEST_CASE("every tool's written payload validates against its bundled output schema")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	sesh_ai::transport::BundledSchemaValidator validator{schema_directory.string()};

	const sesh_ai::transport::SchemaBundleLoadOutcome loaded = validator.load();

	REQUIRE(loaded.errors.empty());
	REQUIRE(loaded.loaded);

	const std::vector<WrittenToolResult> written = every_written_tool_result();

	// All 42, which is also `bundled_tool_output_schema_count` — so a tool added to the
	// protocol without a writer fails here rather than at the composition root.
	REQUIRE(written.size() == sesh_ai::transport::bundled_tool_output_schema_count);

	for (const WrittenToolResult& result : written) {
		const std::string schema_path = success_schema_path(result.tool_name);

		REQUIRE_FALSE(schema_path.empty());
		REQUIRE_FALSE(result.payloads.empty());

		for (const Json& payload : result.payloads) {
			const sesh_ai::transport::SchemaValidationOutcome outcome =
				validator.validate_against<Json>(schema_path, payload);

			// The payload and the schema are both named in the failure, because a bare
			// "did not validate" over 42 tools is not a diagnosis.
			CHECKED_ELSE(outcome.valid) {
				FAIL_CHECK(
					result.tool_name + " wrote a payload that " + schema_path
						+ " refuses: " + sesh_ai::transport::render_validation_errors(outcome.errors, 512)
						+ " — payload was " + payload.dump());
			}
		}
	}
}

TEST_CASE("the writers emit every field the output schemas declare")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	// The check the task's own warning asks for. Validation alone is satisfied by a writer
	// that emits the required fields and nothing else, and from the server's side an
	// optional field the writer never emits is indistinguishable from a tool that failed.
	for (const WrittenToolResult& result : every_written_tool_result()) {
		const std::string schema_path = success_schema_path(result.tool_name);
		const std::filesystem::path schema_file = schema_directory / schema_path;

		REQUIRE(std::filesystem::is_regular_file(schema_file));

		std::ifstream schema_stream{schema_file};
		const Json schema = Json::parse(schema_stream);

		std::set<std::string> declared;
		collect_declared_paths(schema, schema, std::string{}, declared, 0);

		std::set<std::string> present;

		for (const Json& payload : result.payloads) {
			collect_present_paths(payload, std::string{}, present);
		}

		// Every schema declares something, so an empty set would mean the walk above
		// stopped at a `$ref` it could not follow rather than that the schema is open.
		REQUIRE_FALSE(declared.empty());

		// And one nested path per shape the bundler hoists, because "declared is not
		// empty" is also true of a walk that read the root properties and resolved none of
		// the references — which would make this whole case pass by asking for the four
		// fields every schema spells inline.
		if (result.tool_name == "create_track") {
			CHECK(declared.count("/folderDepthRepairs/[]/previousFolderDepth") == 1);
			CHECK(declared.count("/track/guid") == 1);
		}

		if (result.tool_name == "set_item_properties") {
			CHECK(declared.count("/actions/[]/error/message") == 1);
		}

		if (result.tool_name == "update_marker_or_region") {
			// Both `oneOf` branches, which no single payload can satisfy.
			CHECK(declared.count("/marker/position") == 1);
			CHECK(declared.count("/region/end") == 1);
		}

		const auto allowed = paths_no_writer_fills().find(result.tool_name);

		for (const std::string& path : declared) {
			if (allowed != paths_no_writer_fills().end() && allowed->second.count(path) > 0) {
				continue;
			}

			CHECKED_ELSE(present.count(path) > 0) {
				FAIL_CHECK(
					result.tool_name + " never emits \"" + path
						+ "\", which " + schema_path
						+ " declares — from the server's side that is indistinguishable from the "
						  "tool having failed");
			}
		}
	}
}

TEST_CASE("the undo report is attached exactly where a tool's output schema carries one")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	// The cross-check between the registration path a tool took and the contract it
	// answers to. `render`, the two rewinding tools, and the ten read tools open no undo
	// block, and each of their schemas is closed with no undo report — so a tool moved to
	// the wrong registration call fails here rather than at runtime.
	std::size_t schemas_carrying_an_undo_report = 0;

	for (const WrittenToolResult& result : every_written_tool_result()) {
		const std::filesystem::path schema_file =
			schema_directory / success_schema_path(result.tool_name);

		std::ifstream schema_stream{schema_file};
		const Json schema = Json::parse(schema_stream);

		std::set<std::string> declared;
		collect_declared_paths(schema, schema, std::string{}, declared, 0);

		const bool schema_carries_undo = declared.count("/undoPositionBefore") > 0;

		CHECK(schema_carries_undo == result.attaches_undo_report);

		if (schema_carries_undo) {
			++schemas_carrying_an_undo_report;
		}
	}

	// Twenty-nine of the 42. Asserted so that a schema losing or gaining an undo report in
	// a future bundle is a visible change rather than one absorbed by the loop above.
	CHECK(schemas_carrying_an_undo_report == 29);
}

TEST_CASE("the undo report is written in each of the three states a success can hold")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	sesh_ai::transport::BundledSchemaValidator validator{schema_directory.string()};

	REQUIRE(validator.load().loaded);

	SECTION("a report with no description omits the optional field rather than emitting it")
	{
		// `undoDescription` is `minLength: 1`. The description is `"Sesh AI: <tool_name>"`
		// composed by `undo_manager`, which substitutes for an unnamed tool, so an empty
		// one should not arrive — and if it does, an absent optional field is a payload the
		// server accepts while an empty string is not.
		tool_success<Json> success;
		success.tool_name = "create_marker";
		success.fields = written_create_marker();
		success.undo = undo_report{0, std::string{}};

		const dispatch_outcome<Json> outcome{tool_result<Json>{std::move(success)}};
		const Json payload = write_dispatch_outcome<Json>(outcome);

		CHECK(payload.at("undoPositionBefore").get<int>() == 0);
		CHECK_FALSE(payload.contains("undoDescription"));

		CHECK(
			validator
				.validate_against<Json>("mcp-tools/outputs/create-marker.schema.json", payload)
				.valid);
	}

	SECTION("a position below zero is written as the start of the session")
	{
		// `undo_manager` already clamps at every place it reads the stack position, and
		// this is the backstop. The schema's `minimum: 0` means a negative one would make a
		// mutation that happened unreportable, which is worse than reporting the position
		// the manager itself substitutes.
		tool_success<Json> success;
		success.tool_name = "create_marker";
		success.fields = written_create_marker();
		success.undo = undo_report{-3, "Sesh AI: create_marker"};

		const dispatch_outcome<Json> outcome{tool_result<Json>{std::move(success)}};
		const Json payload = write_dispatch_outcome<Json>(outcome);

		CHECK(payload.at("undoPositionBefore").get<int>() == 0);
		CHECK(
			validator
				.validate_against<Json>("mcp-tools/outputs/create-marker.schema.json", payload)
				.valid);
	}

	SECTION("a success in which nothing landed carries no report, which is the signal")
	{
		// `handler_success::nothing_was_applied` reaches here as an absent optional, and
		// both action-array schemas say in their own descriptions that the absence of
		// `undoPositionBefore` is what tells the producer nothing changed. Attaching one
		// anyway would offer a revert that walks back over the producer's earlier work.
		tool_success<Json> success;
		success.tool_name = "set_track_state";
		success.fields = written_set_track_state();

		const dispatch_outcome<Json> outcome{tool_result<Json>{std::move(success)}};
		const Json payload = write_dispatch_outcome<Json>(outcome);

		CHECK_FALSE(payload.contains("undoPositionBefore"));
		CHECK_FALSE(payload.contains("undoDescription"));

		// Still a valid result: both fields are optional on this schema, which is what
		// makes the absence sayable rather than a gap.
		CHECK(
			validator
				.validate_against<Json>("mcp-tools/outputs/set-track-state.schema.json", payload)
				.valid);
	}

	SECTION("a read tool's schema has nowhere to put one, and none is attached")
	{
		// `outputs/list-tracks.schema.json` is closed and names neither field, so a report
		// attached to a read would fail the extension's own outbound validation — which is
		// the check this section is really making about the two together.
		tool_success<Json> success;
		success.tool_name = "list_tracks";
		success.fields = written_list_tracks();

		const dispatch_outcome<Json> read_outcome{tool_result<Json>{success}};

		CHECK_FALSE(write_dispatch_outcome<Json>(read_outcome).contains("undoPositionBefore"));

		success.undo = mutating_undo_report();

		const dispatch_outcome<Json> wrongly_reported{tool_result<Json>{std::move(success)}};

		CHECK_FALSE(
			validator
				.validate_against<Json>(
					"mcp-tools/outputs/list-tracks.schema.json",
					write_dispatch_outcome<Json>(wrongly_reported))
				.valid);
	}
}

TEST_CASE("a refusal validates against the refusal contract for every reason it can carry")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	sesh_ai::transport::BundledSchemaValidator validator{schema_directory.string()};

	REQUIRE(validator.load().loaded);

	// The five values `tool-result-refusal.schema.json` closes `reason` at, each taken from
	// the constant the component that produces it already spells — rather than restated
	// here, where a typo would agree with itself.
	const std::vector<std::pair<std::string, std::string>> reasons_and_acknowledgements{
		{std::string{sesh_ai::daw::unresolved_track_selector_refusal_reason}, std::string{}},
		{std::string{sesh_ai::daw::ambiguous_track_selector_refusal_reason},
			std::string{sesh_ai::daw::ambiguous_track_selector_acknowledgement_field}},
		{std::string{sesh_ai::daw::output_file_collision_refusal_reason},
			std::string{sesh_ai::daw::overwrite_acknowledgement_field}},
		{std::string{sesh_ai::daw::foreign_undo_refusal_reason},
			std::string{sesh_ai::daw::foreign_undo_acknowledgement_field}},
		{std::string{sesh_ai::daw::signal_cycle_refusal_reason},
			std::string{sesh_ai::daw::signal_cycle_acknowledgement_field}}
	};

	REQUIRE(reasons_and_acknowledgements.size() == 5);

	// The four `blocking[].kind` values, one of which carries a GUID and three of which do
	// not — a file path and an undo entry have none, and the schema's `guid` pattern would
	// reject an empty string, which is why the writer omits the property rather than
	// emitting it.
	const std::vector<BlockingEntity> blocking_entities{
		BlockingEntity{std::string{sesh_ai::daw::track_blocking_entity_kind}, "Snare", guid_of(1)},
		BlockingEntity{std::string{sesh_ai::daw::file_path_blocking_kind},
			"/Users/producer/Renders/Snare.wav", std::string{}},
		BlockingEntity{std::string{sesh_ai::daw::undo_entry_blocking_kind},
			"Move media item", std::string{}},
		BlockingEntity{std::string{sesh_ai::daw::send_blocking_entity_kind},
			"Snare -> Reverb -> Snare", std::string{}}
	};

	for (const auto& reason_and_acknowledgement : reasons_and_acknowledgements) {
		for (const BlockingEntity& blocking : blocking_entities) {
			tool_refusal refusal;
			refusal.reason = reason_and_acknowledgement.first;
			refusal.acknowledgement_field = reason_and_acknowledgement.second;
			refusal.blocking.push_back(blocking);

			const dispatch_outcome<Json> outcome{tool_result<Json>{refusal}};
			const Json payload = write_dispatch_outcome<Json>(outcome);
			const std::string schema_path = written_outcome_schema_path<Json>(outcome);

			CHECK(schema_path == sesh_ai::transport::tool_result_refusal_schema_path);

			const sesh_ai::transport::SchemaValidationOutcome validated =
				validator.validate_against<Json>(schema_path, payload);

			CHECKED_ELSE(validated.valid) {
				FAIL_CHECK(
					"a refusal carrying " + refusal.reason + " was refused by its own contract: "
						+ sesh_ai::transport::render_validation_errors(validated.errors, 512));
			}

			// A refusal has nowhere to put an undo report, and the schema is closed — so
			// the absence is checked rather than assumed (requirements 9.3, 10.7).
			CHECK_FALSE(payload.contains("undoPositionBefore"));
			CHECK(payload.at("refused").get<bool>());

			// Present exactly when the reason names one. A pattern matching no track names
			// no acknowledgement, because acknowledging it would not make the track exist.
			CHECK(
				payload.contains("acknowledgementField")
					== !reason_and_acknowledgement.second.empty());

			// Omitted rather than emitted empty for the three kinds with no GUID.
			CHECK(payload.at("blocking").at(0).contains("guid") == !blocking.guid.empty());
		}
	}
}

TEST_CASE("a framework partial validates against the partial contract and not the tool's")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not reachable from this file's path.");
	}

	sesh_ai::transport::BundledSchemaValidator validator{schema_directory.string()};

	REQUIRE(validator.load().loaded);

	// Requirement 9.4: an action array reports per action and does not roll back the
	// siblings that landed. Both outcomes are present here for that reason — the succeeded
	// action is still reported as succeeded.
	tool_partial_outcome<Json> partial;
	partial.tool_name = "set_item_properties";
	partial.actions.push_back(succeeded_action(guid_of(6)));
	partial.actions.push_back(
		failed_action(guid_of(7), "unknown_item", "no item in the project holds that guid"));
	partial.undo = mutating_undo_report();

	const dispatch_outcome<Json> outcome{tool_result<Json>{partial}};
	const Json payload = write_dispatch_outcome<Json>(outcome);
	const std::string schema_path = written_outcome_schema_path<Json>(outcome);

	// The whole reason `payload_schema_path_override` exists. `set_item_properties`' own
	// output schema also carries `actions`, so from a payload and a tool name these two are
	// indistinguishable — and routing this one to that schema refuses it on `items`, which
	// the framework cannot supply.
	CHECK(schema_path == sesh_ai::transport::tool_result_partial_contract_schema_path);

	const sesh_ai::transport::SchemaValidationOutcome validated =
		validator.validate_against<Json>(schema_path, payload);

	CHECKED_ELSE(validated.valid) {
		FAIL_CHECK(
			"a framework partial was refused by its own contract: "
				+ sesh_ai::transport::render_validation_errors(validated.errors, 512));
	}

	CHECK(payload.at("actions").size() == 2);
	CHECK(payload.at("actions").at(0).at("ok").get<bool>());
	CHECK_FALSE(payload.at("actions").at(1).at("ok").get<bool>());
	CHECK(payload.at("actions").at(1).at("error").at("code").get<std::string>() == "unknown_item");
	CHECK(payload.at("undoPositionBefore").get<int>() == 7);
	CHECK(payload.at("undoDescription").get<std::string>() == "Sesh AI: a tool");

	// And the same payload against the named tool's own schema, which is the failure the
	// override prevents. Checked rather than described, because "it would be refused" is
	// the claim the whole mechanism rests on.
	const sesh_ai::transport::SchemaValidationOutcome misrouted =
		validator.validate_against<Json>("mcp-tools/outputs/set-item-properties.schema.json", payload);

	CHECK_FALSE(misrouted.valid);

	SECTION("and carries no undo report when every action failed")
	{
		tool_partial_outcome<Json> all_failed;
		all_failed.tool_name = "delete_items";
		all_failed.actions.push_back(
			failed_action(guid_of(7), "unknown_item", "no item in the project holds that guid"));

		const dispatch_outcome<Json> failed_outcome{tool_result<Json>{all_failed}};
		const Json failed_payload = write_dispatch_outcome<Json>(failed_outcome);

		CHECK_FALSE(failed_payload.contains("undoPositionBefore"));
		CHECK_FALSE(failed_payload.contains("undoDescription"));

		CHECK(
			validator
				.validate_against<Json>(
					sesh_ai::transport::tool_result_partial_contract_schema_path,
					failed_payload)
				.valid);
	}
}

#else

TEST_CASE("the outbound tool writers need the schema validator")
{
	SKIP(
		"nlohmann/json-schema-validator is not on the include path, so a written payload "
		"cannot be checked against its bundled schema. Configure with a vcpkg toolchain."
	);
}

#endif

// ---------------------------------------------------------------------------
// The parts that need no validator
// ---------------------------------------------------------------------------

TEST_CASE("an unknown tool names the tool and answers to no schema")
{
	// Requirement 23.6. Not a tool result — there is no tool, so there are no
	// tool-specific fields and nothing to validate against, which is the correct answer
	// rather than a failure.
	const dispatch_outcome<Json> outcome{
		sesh_ai::daw::build_unknown_tool_error("summon_bassist")};

	const Json payload = write_dispatch_outcome<Json>(outcome);

	CHECK(written_outcome_schema_path<Json>(outcome).empty());
	CHECK(payload.at("tool").get<std::string>() == "summon_bassist");
	CHECK(
		payload.at("code").get<std::string>()
			== std::string{sesh_ai::daw::unknown_tool_error_code});

	// The name is inside the message as well as in its own field, because the server reads
	// one and the producer reads the other.
	CHECK(
		payload.at("message").get<std::string>().find("summon_bassist") != std::string::npos);
}

TEST_CASE("a colour is written as the inverse of the reader's parse")
{
	// `ToolInputReader::optional_color` parses `#RRGGBB` into the 24-bit value the tool
	// layer holds, and this is the other direction. The pair is checked rather than each
	// half separately: a writer that reordered the channels would disagree with the reader
	// about what a colour is, and both would still look right on their own.
	sesh_ai::transport::ToolInputProblem problem;

	const Json payload = Json{{"color", "#1A2B3C"}};
	sesh_ai::transport::ToolInputReader<Json> reader{&payload, problem};

	const std::optional<int> parsed = reader.optional_color("color");

	REQUIRE(parsed.has_value());
	CHECK_FALSE(problem.anything_recorded());
	CHECK(written_color_string(*parsed) == "#1A2B3C");

	// REAPER's enabled flag is masked off by `color_from_reaper_custom_color` before a
	// colour reaches a result, and the mask here is the backstop for one that did not.
	CHECK(written_color_string(0x01000000 | 0x1A2B3C) == "#1A2B3C");
}

TEST_CASE("a minLength field substitutes rather than emitting a string the schema refuses")
{
	// Sixteen string fields across the 42 carry `minLength: 1`, and a handler can hand back
	// an empty one — an FX REAPER would not name, an undo entry with no description. The
	// substitution follows `tool_executor.h`'s `unnamed_action_target` and
	// `undo_manager.h`'s `unnamed_undo_entry_description`: an object nobody can name is
	// still an object the agent needs told about, and dropping it loses the only evidence
	// it exists.
	sesh_ai::daw::tools::add_fx_result result;
	result.track = sample_track_reference(1, std::string{});
	result.fx_index = 0;
	result.fx_name = std::string{};
	result.chain_length = 1;

	const Json payload = sesh_ai::transport::write_add_fx_result<Json>(result);

	CHECK(
		payload.at("fxName").get<std::string>()
			== std::string{sesh_ai::transport::unnamed_written_fx});

	// The track name is `maxLength` only, and an unnamed track in REAPER really has no
	// name — so that one is emitted empty rather than given a placeholder the producer
	// never typed.
	CHECK(payload.at("track").at("name").get<std::string>().empty());

	SECTION("and a GUID array is not substituted for, because no placeholder satisfies it")
	{
		// `deletedItemGuids` constrains each entry by the braced pattern rather than by
		// `minLength`, so a substituted sentence would fail it exactly as an empty string
		// does while reading as though it were acceptable. The empty string goes out and
		// the validation names the index, which is the honest report for an identifier
		// only REAPER can mint.
		sesh_ai::daw::tools::delete_items_result deletion;
		deletion.deleted_item_guids.push_back(std::string{});
		deletion.affected_tracks.push_back(sample_track_reference(1, "Snare"));
		deletion.deleted_length_seconds = 1.0;

		const Json deleted =
			sesh_ai::transport::write_delete_items_result<Json>(deletion);

		CHECK(deleted.at("deletedItemGuids").at(0).get<std::string>().empty());
	}
}

TEST_CASE("a value longer than the schema allows is shortened rather than refused")
{
	sesh_ai::daw::tools::add_fx_result result;
	result.track = sample_track_reference(1, std::string(900, 'x'));
	result.fx_index = 0;
	result.fx_name = std::string(900, 'y');
	result.chain_length = 1;

	const Json payload = sesh_ai::transport::write_add_fx_result<Json>(result);

	CHECK(
		payload.at("track").at("name").get<std::string>().size()
			== sesh_ai::transport::maximum_written_name_length);
	CHECK(
		payload.at("fxName").get<std::string>().size()
			== sesh_ai::transport::maximum_written_name_length);
}

TEST_CASE("binding the writers completes every codec the readers left half built")
{
	// `tool_request_readers.h` populated the read members and nothing else, so a codec is
	// incomplete until these run — and `register_*_tools` returns early on an incomplete
	// one, which is why all 42 tools are unregistered in the build before this task.
	sesh_ai::daw::tools::track_structure_payload_codec<Json> track_structure;
	sesh_ai::transport::bind_track_structure_request_readers(track_structure);

	CHECK_FALSE(track_structure.is_complete());

	sesh_ai::transport::bind_track_structure_result_writers(track_structure);

	CHECK(track_structure.is_complete());

	sesh_ai::daw::tools::marker_region_payload_codec<Json> marker_region;
	sesh_ai::transport::bind_marker_region_request_readers(marker_region);

	CHECK_FALSE(marker_region.is_complete());

	sesh_ai::transport::bind_marker_region_result_writers(marker_region);

	CHECK(marker_region.is_complete());

	sesh_ai::daw::tools::marker_region_read_payload_codec<Json> marker_region_reads;
	sesh_ai::transport::bind_marker_region_read_request_readers(marker_region_reads);
	sesh_ai::transport::bind_marker_region_read_result_writers(marker_region_reads);

	CHECK(marker_region_reads.is_complete());

	sesh_ai::daw::tools::tempo_payload_codec<Json> tempo;
	sesh_ai::transport::bind_tempo_request_readers(tempo);
	sesh_ai::transport::bind_tempo_result_writers(tempo);

	CHECK(tempo.is_complete());

	// Routing and undo declare no `is_complete`, and each registration checks the callables
	// it needs one at a time — so the check here is that every member is bound.
	sesh_ai::daw::tools::routing_payload_codec<Json> routing;
	sesh_ai::transport::bind_routing_request_readers(routing);
	sesh_ai::transport::bind_routing_result_writers(routing);

	CHECK(static_cast<bool>(routing.write_create_send_result));
	CHECK(static_cast<bool>(routing.write_remove_send_result));
	CHECK(static_cast<bool>(routing.write_set_send_state_result));
	CHECK(static_cast<bool>(routing.write_set_parent_send_result));
	CHECK(static_cast<bool>(routing.write_create_bus_result));
	CHECK(static_cast<bool>(routing.write_remove_bus_result));
	CHECK(static_cast<bool>(routing.write_get_routing_result));

	sesh_ai::daw::tools::undo_payload_codec<Json> undo;
	sesh_ai::transport::bind_undo_result_writers(undo);

	CHECK(static_cast<bool>(undo.write_undo_last_action_result));
	CHECK(static_cast<bool>(undo.write_revert_agent_changes_result));

	sesh_ai::daw::tools::render_payload_codec<Json> render;
	sesh_ai::transport::bind_render_request_reader(render);
	sesh_ai::transport::bind_render_result_writer(render);

	CHECK(static_cast<bool>(render.read_render_request));
	CHECK(static_cast<bool>(render.write_render_result));
}

TEST_CASE("the schema the writer answers to is the override the outbound envelope carries")
{
	// The one assignment task 22.2 added the field for, exercised through the real sink
	// rather than described. `EnvelopeCodec::encode_outbound` runs on the network thread and
	// derives the schema from the type and the payload; for a framework partial that
	// derivation cannot be right, so the answer has to travel with the envelope.
	using OutboundEnvelope = sesh_ai::transport::OutboundEnvelope;

	sesh_ai::transport::ConcurrentQueue<OutboundEnvelope> outbound_queue;
	sesh_ai::entry::QueuedToolResultSink<OutboundEnvelope, Json> sink{outbound_queue};

	sesh_ai::transport::InboundEnvelope answered_call;
	answered_call.type = "request:set_item_properties";
	answered_call.request_id = "call-17";

	tool_partial_outcome<Json> partial;
	partial.tool_name = "set_item_properties";
	partial.actions.push_back(succeeded_action(guid_of(6)));
	partial.undo = mutating_undo_report();

	const dispatch_outcome<Json> outcome{tool_result<Json>{partial}};

	const sesh_ai::transport::ToolResultResponse<Json> response{
		answered_call,
		partial.tool_name,
		write_dispatch_outcome<Json>(outcome),
		written_outcome_schema_path<Json>(outcome)
	};

	REQUIRE(sink.send_tool_result(response));

	const std::optional<OutboundEnvelope> queued = outbound_queue.try_pop();

	REQUIRE(queued.has_value());
	CHECK(queued->type == "response:set_item_properties");

	// Requirement 5.9, and the reason `ToolResultResponse` cannot be built without the call
	// it answers.
	CHECK(queued->request_id == "call-17");

	// Not `mcp-tools/outputs/set-item-properties.schema.json`, which is what the codec
	// would derive from this type and payload.
	CHECK(
		queued->payload_schema_path_override
			== sesh_ai::transport::tool_result_partial_contract_schema_path);
	CHECK(queued->payload.at("actions").size() == 1);
}

#else

TEST_CASE("the outbound tool writers need nlohmann/json")
{
	SKIP(
		"nlohmann/json is not on the include path, so the writers cannot build the documents "
		"they produce. Configure with a vcpkg toolchain."
	);
}

#endif
