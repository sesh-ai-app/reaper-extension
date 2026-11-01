// One writer per tool: the result struct each `src/daw/tools/*.h` handler returns,
// turned into the payload that tool's bundled output schema describes (task 24.2,
// requirements 4.1, 9.2, 9.4, 20.1).
//
// ---------------------------------------------------------------------------
// What this file is the other half of
//
// `tool_request_readers.h` populated the `read_*` member of every per-family codec seam
// and left every `write_*` member empty, so that this task is an addition rather than a
// rewrite. This file is the write side of all ten families. Same shape, same ten seams:
// seven declare a `*_payload_codec` struct of callables, and three — fx, item, and track
// state — take a writer as a template parameter, because each of their writers is an
// overload set across the family's result types and an overload set does not fit in a
// `std::function`.
//
// The naming follows: `bind_*_result_writers` beside each `bind_*_request_readers`, and
// each one populates only `write_*`, so the two halves of a codec are populated by two
// calls that do not touch each other's members.
//
// ---------------------------------------------------------------------------
// Writers are the contract the agent reasons over
//
// The readers could afford to be narrow — requirement 4.4 says the MCP Tool Server
// already validated the input, so a reader's job is to not trust the payload rather than
// to re-check it. The writers have the opposite problem. Nothing validated these payloads
// before, the 42 output schemas are bundled precisely because the extension produces
// them, and requirement 4.1 checks every one on the way out. So a field this file gets
// wrong is either a payload the extension refuses to send — which a producer experiences
// as the tool silently not working — or, worse, a payload that validates while saying
// something untrue.
//
// The second kind is why "emit every required field" is not the bar. From the server's
// side a field the schema permits and the writer never emits is indistinguishable from a
// tool that failed: an agent reading a `list_tracks` result with no `aliases` on any
// track cannot tell whether the tracks hold no aliases or whether this build does not
// report them, and the next thing it does is either invent an alias that already exists
// or refuse to use one that does. So every optional field whose result struct carries
// data for it is emitted, `set_optional_*` decides on the result rather than on the
// writer's convenience, and the suite checks emitted coverage of each schema's optional
// fields rather than only that the required ones validate.
//
// There is exactly one optional field in the 42 that nothing here can fill, and it is
// named where it sits: `create-bus.schema.json`'s `fx[].offline`. `routing_fx_placement`
// carries no offline flag, because the FX in a `create_bus` result were inserted by that
// call and an FX REAPER has just loaded is not offline. A struct field would be a second
// thing to keep true.
//
// ---------------------------------------------------------------------------
// Which fields come from the framework rather than from here
//
// None of these writers emits `undoPositionBefore` or `undoDescription`, and that is not
// an omission. The undo report is `tool_executor.h`'s — the turn's marker lives in
// `undo_manager`, the framework attaches it to a `tool_success` or withholds it, and the
// three rules about when it is withheld (a refusal, a block that failed to close, a
// handler reporting that nothing landed) are all decided there. Each result struct
// mirrors its schema's *tool-specific* fields and holds no undo field for the same
// reason. `tool_outcome_writer.h` is where the two meet, and it is the only place in this
// codebase that writes either field.
//
// Likewise `refused`, `reason`, `blocking`, and `actions`: a refusal and a framework
// partial are shapes the framework fills in, not a tool's own result, so no writer here
// touches them. The two exceptions are the two tools whose *own* output schemas carry
// `actions` — `set_item_properties` and `set_track_state` — and both of those get it from
// their result struct's own `std::vector<action_outcome>`, which is what makes them
// `handler_success` rather than `handler_action_outcomes`.
//
// ---------------------------------------------------------------------------
// Where a schema spelling comes from
//
// Every closed enumeration in these payloads already has a `to_schema_string` beside the
// enum it belongs to — `routing_send_mode` in `routing_tools.h`, `render_source` and its
// two neighbours in `render_coordinator.h`, `StructuralRole` in `structural_role.h`,
// `item_scope`'s `describe_item_scope` in `item_tools.h`. Those are called rather than
// restated, found by ordinary argument-dependent lookup, so a value added to one of those
// enums is spelled in one place. `bus_type` is the one without one and gets a local
// helper, because `cycle_detector.h` declares the enum for a graph search that has no
// schema in view.
//
// Header-only and templated on the document type, like every other file in this seam:
// `tests/CMakeLists.txt` does not compile `src/`, so a writer has to be reachable through
// a header to be tested, and the template is what keeps a JSON library out of the
// `src/daw/` files these writers read.

#ifndef SESH_AI_TRANSPORT_TOOL_RESULT_WRITERS_H
#define SESH_AI_TRANSPORT_TOOL_RESULT_WRITERS_H

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <context/structural_role.h>
#include <daw/cycle_detector.h>
#include <daw/object_resolver.h>
#include <daw/render_coordinator.h>
#include <daw/tool_executor.h>
#include <daw/tools/fx_tools.h>
#include <daw/tools/item_tools.h>
#include <daw/tools/marker_region_tools.h>
#include <daw/tools/render_tool.h>
#include <daw/tools/routing_tools.h>
#include <daw/tools/tempo_tools.h>
#include <daw/tools/track_state_tools.h>
#include <daw/tools/track_structure_tools.h>
#include <daw/tools/undo_tools.h>
#include <daw/undo_manager.h>
#include <transport/tool_payload_writing.h>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// The one enumeration with no spelling beside it
	// ---------------------------------------------------------------------------

	// `type` on `create-bus.schema.json` and `remove-bus.schema.json`.
	//
	// `cycle_detector.h` declares `bus_type` for the routing search, which is a graph
	// question with no schema in it, so the enum has no `to_schema_string` of its own.
	// Spelled here rather than added there, because the only thing that needs the string
	// is a payload and this is the file that builds payloads.
	inline constexpr std::string_view written_bus_type(daw::bus_type type)
	{
		return type == daw::bus_type::aux ? std::string_view{"aux"} : std::string_view{"summing"};
	}

	// `tracks[].target` on `set-parent-send.schema.json`: where the track's output goes
	// now that its parent send was changed.
	inline constexpr std::string_view written_parent_send_target(bool targets_folder_parent)
	{
		return targets_folder_parent ? std::string_view{"folder_parent"} : std::string_view{"master"};
	}

	// ---------------------------------------------------------------------------
	// Shared shapes
	//
	// The definitions `tool-output-defs.schema.json` declares and the bundler hoists into
	// every referring schema. Written once here for the same reason they are declared once
	// there: `trackReference` appears in nineteen of the 42, and nineteen hand-written
	// copies is nineteen chances to spell `guid` differently.
	// ---------------------------------------------------------------------------

	// `trackReference`. Both fields required; the name is what the producer is shown and
	// is allowed to be empty, because an unnamed track in REAPER has no name.
	template <typename JsonValue>
	JsonValue write_track_reference(const daw::TrackReference& track)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string("guid", track.guid, maximum_written_name_length);
		writer.set_string("name", track.name, maximum_written_name_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_track_references(const std::vector<daw::TrackReference>& tracks)
	{
		return write_array_of<JsonValue>(tracks, [](const daw::TrackReference& track) {
			return write_track_reference<JsonValue>(track);
		});
	}

	// `folderDepthRepair`. The Folder Invariant Keeper's own report of what it changed on
	// a track the call did not name, which is requirement 11.2 made visible rather than
	// taken on trust.
	template <typename JsonValue>
	JsonValue write_folder_depth_repairs(
		const std::vector<daw::tools::folder_depth_repair>& repairs)
	{
		return write_array_of<JsonValue>(
			repairs,
			[](const daw::tools::folder_depth_repair& repair) {
				ToolOutputWriter<JsonValue> writer;

				writer.set_string("guid", repair.guid, maximum_written_name_length);
				writer.set_string("name", repair.name, maximum_written_name_length);
				writer.set_integer("previousFolderDepth", repair.previous_folder_depth);
				writer.set_integer("folderDepth", repair.folder_depth);

				return writer.release();
			});
	}

	// `markerDetail`. The index is REAPER's displayed number, reported because the
	// producer sees it on the ruler.
	template <typename JsonValue>
	JsonValue write_marker_detail(const daw::tools::marker_detail& marker)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string("guid", marker.guid, maximum_written_name_length);
		writer.set_integer("index", marker.index);
		writer.set_number("position", marker.position_seconds);
		writer.set_string("name", marker.name, maximum_written_name_length);
		writer.set_optional_color("color", marker.color);

		return writer.release();
	}

	// `regionDetail`.
	template <typename JsonValue>
	JsonValue write_region_detail(const daw::tools::region_detail& region)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string("guid", region.guid, maximum_written_name_length);
		writer.set_integer("index", region.index);
		writer.set_number("start", region.start_seconds);
		writer.set_number("end", region.end_seconds);
		writer.set_string("name", region.name, maximum_written_name_length);
		writer.set_optional_color("color", region.color);

		return writer.release();
	}

	// The `kind` discriminator and the branch it names, for the two tools whose output
	// schema is a `oneOf` over marker and region.
	//
	// `describe_marker_or_region_kind` derives the discriminator from the variant rather
	// than from a field anybody set, which is why a payload carrying `kind: "marker"` and
	// a `region` is not reachable from here.
	template <typename JsonValue>
	void write_marker_or_region_branch(
		ToolOutputWriter<JsonValue>& writer,
		const daw::tools::marker_or_region_detail& detail)
	{
		writer.set_string(
			"kind",
			daw::tools::describe_marker_or_region_kind(detail),
			maximum_written_name_length);

		if (const daw::tools::region_detail* const region =
				std::get_if<daw::tools::region_detail>(&detail)) {
			writer.set_value("region", write_region_detail<JsonValue>(*region));

			return;
		}

		writer.set_value(
			"marker",
			write_marker_detail<JsonValue>(std::get<daw::tools::marker_detail>(detail)));
	}

	// `timeSignature`.
	template <typename JsonValue>
	JsonValue write_time_signature(const daw::tools::tempo_time_signature& signature)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("numerator", signature.numerator);
		writer.set_integer("denominator", signature.denominator);

		return writer.release();
	}

	// `itemPlacement`. `name` is the take name REAPER reports and is optional: an item
	// with no take has none, and inventing one would have the agent quote a name the
	// producer cannot find.
	template <typename JsonValue>
	JsonValue write_item_placement(const daw::tools::item_placement& item)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string("guid", item.guid, maximum_written_name_length);
		writer.set_string("trackGuid", item.track_guid, maximum_written_name_length);
		writer.set_number("position", item.position_seconds);
		writer.set_number("length", item.length_seconds);
		writer.set_optional_string("name", item.name, maximum_written_name_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_item_placements(const std::vector<daw::tools::item_placement>& items)
	{
		return write_array_of<JsonValue>(items, [](const daw::tools::item_placement& item) {
			return write_item_placement<JsonValue>(item);
		});
	}

	// `sendDetail`. One shape for a send and a receive, which is what the schemas do:
	// `trackGuid` is the other end of the connection, whichever direction it points.
	template <typename JsonValue>
	JsonValue write_routing_send(const daw::tools::routing_send& send)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("index", send.index);
		writer.set_string("trackGuid", send.track_guid, maximum_written_name_length);
		writer.set_string("trackName", send.track_name, maximum_written_name_length);
		writer.set_number("volume", send.volume_decibels);
		writer.set_number("pan", send.pan_percent);
		writer.set_boolean("muted", send.muted);
		writer.set_string("mode", to_schema_string(send.mode), maximum_written_name_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_routing_sends(const std::vector<daw::tools::routing_send>& sends)
	{
		return write_array_of<JsonValue>(sends, [](const daw::tools::routing_send& send) {
			return write_routing_send<JsonValue>(send);
		});
	}

	// `fxPlacement`. `offline` is emitted because `fx_chain_entry` carries it: an offline
	// FX is one the producer has to bring back before anything it does is audible, and
	// omitting the flag reads as "online" to an agent that cannot tell the difference.
	template <typename JsonValue>
	JsonValue write_fx_chain_entry(const daw::tools::fx_chain_entry& entry)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("index", entry.index);
		writer.set_named_string(
			"name",
			entry.name,
			unnamed_written_fx,
			maximum_written_name_length);
		writer.set_boolean("bypassed", entry.bypassed);
		writer.set_boolean("offline", entry.offline);

		return writer.release();
	}

	// `actionOutcome`'s two closed shapes, split on `ok` rather than hanging an optional
	// error off one — which is requirement 9.5 the way both the schema and
	// `tool_executor.h`'s `action_outcome` variant already state it.
	//
	// Used by `set_item_properties` and `set_track_state`, whose own output schemas carry
	// `actions`, and by `tool_outcome_writer.h` for a framework partial. One writer, so
	// the two cannot disagree about a shape they share.
	template <typename JsonValue>
	JsonValue write_action_outcome(const daw::action_outcome& outcome)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string(
			"target",
			daw::action_target(outcome),
			daw::maximum_action_target_length);

		if (const daw::action_failed* const failed = std::get_if<daw::action_failed>(&outcome)) {
			writer.set_constant("ok", false);

			ToolOutputWriter<JsonValue> error;
			error.set_string(
				"code",
				failed->error.code(),
				daw::maximum_action_error_code_length);
			error.set_string(
				"message",
				failed->error.message(),
				daw::maximum_action_error_message_length);

			writer.set_value("error", error.release());

			return writer.release();
		}

		writer.set_constant("ok", true);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_action_outcomes(const std::vector<daw::action_outcome>& actions)
	{
		return write_array_of<JsonValue>(actions, [](const daw::action_outcome& outcome) {
			return write_action_outcome<JsonValue>(outcome);
		});
	}

	// ---------------------------------------------------------------------------
	// Track structure
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	JsonValue write_create_track_result(const daw::tools::create_track_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("index", result.index);
		writer.set_integer("accumulatedDepth", result.accumulated_depth);
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_delete_track_result(const daw::tools::delete_track_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("deletedTrack", write_track_reference<JsonValue>(result.deleted_track));
		writer.set_integer("deletedItemCount", result.deleted_item_count);
		writer.set_integer("deletedFxCount", result.deleted_fx_count);
		writer.set_boolean("folderDissolved", result.folder_dissolved);

		// Requirement 11.3's evidence. Empty for a non-folder delete, and emitted as an
		// empty array rather than omitted — the schema requires the field, and "no track
		// rose" is a different answer from "nothing was reported".
		writer.set_value(
			"reparentedTracks",
			write_track_references<JsonValue>(result.reparented_tracks));
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_duplicate_track_result(const daw::tools::duplicate_track_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("sourceTrack", write_track_reference<JsonValue>(result.source_track));
		writer.set_value("newTrack", write_track_reference<JsonValue>(result.new_track));
		writer.set_integer("index", result.index);
		writer.set_integer("copiedItemCount", result.copied_item_count);
		writer.set_integer("copiedFxCount", result.copied_fx_count);
		writer.set_integer("copiedSendCount", result.copied_send_count);
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_folder_structure_result(
		const daw::tools::set_folder_structure_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"tracks",
			write_array_of<JsonValue>(result.tracks, [](const daw::tools::moved_track& moved) {
				ToolOutputWriter<JsonValue> track;

				track.set_string("guid", moved.guid, maximum_written_name_length);
				track.set_string("name", moved.name, maximum_written_name_length);
				track.set_integer("previousAccumulatedDepth", moved.previous_accumulated_depth);
				track.set_integer("accumulatedDepth", moved.accumulated_depth);
				track.set_integer("index", moved.index);

				return track.release();
			}));

		// Optional, and absent is the schema's own way of saying the tracks went to the
		// top level. `set_folder_structure_result` holds the parent as a plain
		// `TrackReference` rather than an optional one, so an empty GUID is the absence —
		// and emitting a reference with no GUID would fail the braced pattern anyway.
		if (!result.folder_parent.guid.empty()) {
			writer.set_value(
				"folderParent",
				write_track_reference<JsonValue>(result.folder_parent));
		}

		writer.set_boolean("movedToTopLevel", result.moved_to_top_level);
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// Markers, regions, the tempo map, the time selection
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	JsonValue write_create_marker_result(const daw::tools::create_marker_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("marker", write_marker_detail<JsonValue>(result.marker));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_create_region_result(const daw::tools::create_region_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("region", write_region_detail<JsonValue>(result.region));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_update_marker_or_region_result(
		const daw::tools::update_marker_or_region_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		write_marker_or_region_branch(writer, result.updated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_delete_marker_or_region_result(
		const daw::tools::delete_marker_or_region_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		// Read before the deletion by the handler, because it cannot be read after — so
		// the producer hears "removed the region Chorus" rather than a GUID.
		write_marker_or_region_branch(writer, result.deleted);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_change_tempo_map_result(const daw::tools::change_tempo_map_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("pointsWritten", result.points_written);
		writer.set_integer("pointsReplaced", result.points_replaced);
		writer.set_boolean("replacedExisting", result.replaced_existing);
		writer.set_integer("tempoChangeCount", result.tempo_change_count);

		// Measured rather than predicted by the handler, and reported because a tempo map
		// change moves every item that was beat-locked and the producer has no other way
		// to be told how much moved.
		writer.set_integer("movedItemCount", result.moved_item_count);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_time_selection_result(
		const daw::tools::set_time_selection_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_number("start", result.start_seconds);
		writer.set_number("end", result.end_seconds);
		writer.set_number("lengthSeconds", result.length_seconds);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_markers_result(const daw::tools::list_markers_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"markers",
			write_array_of<JsonValue>(
				result.markers,
				[](const daw::tools::marker_detail& marker) {
					return write_marker_detail<JsonValue>(marker);
				}));

		// Requirement 23.9: a capped read says how many there were. `truncated` without
		// `totalInRange` would tell the agent something was left out and not how much.
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_regions_result(const daw::tools::list_regions_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"regions",
			write_array_of<JsonValue>(
				result.regions,
				[](const daw::tools::region_detail& region) {
					return write_region_detail<JsonValue>(region);
				}));
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_tempo_changes_result(
		const daw::tools::list_tempo_changes_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"tempoChanges",
			write_array_of<JsonValue>(
				result.tempo_changes,
				[](const daw::tools::tempo_change_entry& entry) {
					ToolOutputWriter<JsonValue> change;

					change.set_number("position", entry.position_seconds);
					change.set_number("tempo", entry.beats_per_minute);
					change.set_value(
						"timeSignature",
						write_time_signature<JsonValue>(entry.time_signature));
					change.set_boolean("linearTransition", entry.linear_transition);

					return change.release();
				}));
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// Routing
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	JsonValue write_create_send_result(const daw::tools::create_send_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("sourceTrack", write_track_reference<JsonValue>(result.source_track));
		writer.set_value(
			"destinationTrack",
			write_track_reference<JsonValue>(result.destination_track));
		writer.set_integer("sendIndex", result.send_index);
		writer.set_number("volume", result.volume_decibels);
		writer.set_number("pan", result.pan_percent);
		writer.set_string("mode", to_schema_string(result.mode), maximum_written_name_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_remove_send_result(const daw::tools::remove_send_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("sourceTrack", write_track_reference<JsonValue>(result.source_track));
		writer.set_value(
			"destinationTrack",
			write_track_reference<JsonValue>(result.destination_track));
		writer.set_integer("sendIndex", result.send_index);

		// The remaining count, because removing a send renumbers the ones above it and an
		// agent holding an index from an earlier call needs to know the chain changed
		// length.
		writer.set_integer("remainingSendCount", result.remaining_send_count);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_send_state_result(const daw::tools::set_send_state_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("sourceTrack", write_track_reference<JsonValue>(result.source_track));
		writer.set_value(
			"destinationTrack",
			write_track_reference<JsonValue>(result.destination_track));
		writer.set_value("send", write_routing_send<JsonValue>(result.send));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_parent_send_result(const daw::tools::set_parent_send_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"tracks",
			write_array_of<JsonValue>(
				result.tracks,
				[](const daw::tools::parent_send_change& change) {
					ToolOutputWriter<JsonValue> track;

					track.set_string("guid", change.track.guid, maximum_written_name_length);
					track.set_string("name", change.track.name, maximum_written_name_length);
					track.set_boolean("previousEnabled", change.previous_enabled);
					track.set_string(
						"target",
						written_parent_send_target(change.targets_folder_parent),
						maximum_written_name_length);
					track.set_string("targetGuid", change.target_guid, maximum_written_name_length);

					return track.release();
				}));
		writer.set_boolean("enabled", result.enabled);

		// The folder parents this change left summing nothing. Required by the schema and
		// emitted empty when there are none, because "no bus went silent" is the answer
		// the producer needs rather than the absence of one — a silent folder parent is
		// the failure requirement 18.1's role derivation exists to make visible.
		writer.set_value(
			"silencedFolderParents",
			write_track_references<JsonValue>(result.silenced_folder_parents));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_create_bus_result(const daw::tools::create_bus_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("busTrack", write_track_reference<JsonValue>(result.bus_track));
		writer.set_string("type", written_bus_type(result.type), maximum_written_name_length);
		writer.set_integer("index", result.index);
		writer.set_value("sourceTracks", write_track_references<JsonValue>(result.source_tracks));

		// `offline` is the one optional field in the 42 nothing here can fill — see the
		// file header. `routing_fx_placement` carries no flag for it, and an FX REAPER has
		// just inserted is not offline.
		writer.set_value(
			"fx",
			write_array_of<JsonValue>(
				result.fx,
				[](const daw::tools::routing_fx_placement& placement) {
					ToolOutputWriter<JsonValue> entry;

					entry.set_integer("index", placement.index);
					entry.set_named_string(
						"name",
						placement.name,
						unnamed_written_fx,
						maximum_written_name_length);
					entry.set_boolean("bypassed", placement.bypassed);

					return entry.release();
				}));

		// Empty for a summing bus, which routes through folder depth rather than through
		// sends. Emitted rather than omitted: the schema requires it, and the difference
		// between an aux bus with no sends and a summing bus is what `type` says.
		writer.set_value(
			"sends",
			write_array_of<JsonValue>(result.sends, [](const daw::tools::created_send& send) {
				ToolOutputWriter<JsonValue> entry;

				entry.set_string(
					"sourceTrackGuid",
					send.source_track_guid,
					maximum_written_name_length);
				entry.set_integer("sendIndex", send.send_index);
				entry.set_number("volume", send.volume_decibels);
				entry.set_string("mode", to_schema_string(send.mode), maximum_written_name_length);

				return entry.release();
			}));
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_remove_bus_result(const daw::tools::remove_bus_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("removedBus", write_track_reference<JsonValue>(result.removed_bus));
		writer.set_string("type", written_bus_type(result.type), maximum_written_name_length);
		writer.set_integer("removedFxCount", result.removed_fx_count);
		writer.set_value(
			"reparentedTracks",
			write_track_references<JsonValue>(result.reparented_tracks));

		// The tracks whose sends to the bus went with it. Named rather than counted,
		// because each one is a track whose signal now goes somewhere else.
		writer.set_value(
			"removedSends",
			write_track_references<JsonValue>(result.removed_sends));
		writer.set_value(
			"folderDepthRepairs",
			write_folder_depth_repairs<JsonValue>(result.folder_depth_repairs));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_get_routing_result(const daw::tools::get_routing_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"tracks",
			write_array_of<JsonValue>(
				result.tracks,
				[](const daw::tools::track_routing_report& report) {
					ToolOutputWriter<JsonValue> track;

					track.set_string("guid", report.track.guid, maximum_written_name_length);
					track.set_string("name", report.track.name, maximum_written_name_length);
					track.set_string(
						"role",
						to_schema_string(report.role),
						maximum_written_name_length);
					track.set_integer("folderDepth", report.folder_depth_delta);
					track.set_integer("accumulatedDepth", report.accumulated_folder_depth);

					// Optional, and absent means top level. Present as an optional on the
					// report, so the writer does not have to decide what an empty GUID
					// meant.
					track.set_optional_string(
						"folderParentGuid",
						reported_string_view(report.folder_parent_guid),
						maximum_written_name_length);

					track.set_boolean("parentSendEnabled", report.parent_send_enabled);
					track.set_value("sends", write_routing_sends<JsonValue>(report.sends));
					track.set_value("receives", write_routing_sends<JsonValue>(report.receives));
					track.set_integer("itemCount", report.item_count);
					track.set_integer("fxCount", report.fx_count);

					return track.release();
				}));

		// Optional, and absent when the snapshot could not read a master track —
		// `master_routing_report::present` is what says so, which is why the flag exists
		// rather than an empty GUID standing in for it.
		if (result.master_track.present) {
			ToolOutputWriter<JsonValue> master;

			master.set_string(
				"guid",
				result.master_track.track.guid,
				maximum_written_name_length);
			master.set_string(
				"name",
				result.master_track.track.name,
				maximum_written_name_length);
			master.set_string(
				"role",
				to_schema_string(result.master_track.role),
				maximum_written_name_length);
			master.set_value(
				"receives",
				write_routing_sends<JsonValue>(result.master_track.receives));
			master.set_integer("fxCount", result.master_track.fx_count);

			writer.set_value("masterTrack", master.release());
		}

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// Render
	// ---------------------------------------------------------------------------

	// The one mutating-classified tool whose output schema carries no undo report, with
	// `additionalProperties: false` — requirement 12.6 enforced by the contract as well as
	// by the registration path. Nothing is added here that could acquire one.
	template <typename JsonValue>
	JsonValue write_render_result(const daw::render_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("queuedJobCount", result.queued_job_count);
		writer.set_string("source", to_schema_string(result.source), maximum_written_name_length);
		writer.set_string("bounds", to_schema_string(result.bounds), maximum_written_name_length);

		// Resolved rather than echoed. `region` and `time_selection` both name a span
		// indirectly, and the producer should be told which seconds it turned out to be.
		writer.set_number("startSeconds", result.start_seconds);
		writer.set_number("endSeconds", result.end_seconds);

		// Echoed when the bounds were a region, and empty otherwise.
		writer.set_optional_string("regionGuid", result.region_guid, maximum_written_name_length);

		// `minItems: 1` on the schema, so an empty list is the absence rather than an
		// empty array — a master mix render names no tracks.
		writer.set_non_empty_array(
			"tracks",
			write_array_of<JsonValue>(result.tracks, [](const daw::render_track& track) {
				ToolOutputWriter<JsonValue> entry;

				entry.set_string("guid", track.guid, maximum_written_name_length);
				entry.set_string("name", track.name, maximum_written_name_length);

				return entry.release();
			}));

		writer.set_named_string(
			"outputDirectory",
			result.output_directory,
			unnamed_written_file_path,
			maximum_written_file_path_length);

		// Enumerated rather than counted, because collision detection ran against exactly
		// these paths before anything was queued (requirement 9.8) and the producer may
		// have to go and look at them.
		writer.set_value(
			"outputPaths",
			write_string_array<JsonValue>(
				result.output_paths,
				unnamed_written_file_path,
				maximum_written_file_path_length));

		writer.set_string(
			"outputFormat",
			to_schema_string(result.output_format),
			maximum_written_name_length);

		// Absent for mp3, which has none, and when the call left it to the project.
		writer.set_optional_integer("bitDepth", result.bit_depth);

		// Optional in the schema and always resolved by the coordinator, so it is emitted
		// — an agent told a render was queued and not told at what rate cannot tell a
		// 48 kHz session from a 44.1 kHz one. Zero is the coordinator having resolved
		// nothing, and the schema's floor is 8000, so that case is an absence rather than
		// a figure nobody computed.
		if (result.sample_rate > 0) {
			writer.set_integer("sampleRate", result.sample_rate);
		}

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// The two rewinding tools
	//
	// Both output schemas omit `undoPositionBefore` and report `undoStackPositionAfter`
	// instead, for the reason `undo_tools.h` gives: a marker captured before a call that
	// moves the stack backwards would point past the producer's own work.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	JsonValue write_undo_last_action_result(const daw::undo_last_action_outcome& outcome)
	{
		ToolOutputWriter<JsonValue> writer;

		// REAPER's own descriptions, which is what the producer sees in their undo
		// history. `undo_manager` already substitutes for an unnamed entry and caps the
		// list; the substitution here is the backstop for an empty string reaching this
		// far, because the schema puts `minLength: 1` on each entry and a dropped entry
		// would be an undo nobody was told about.
		writer.set_value(
			"undoneEntryDescriptions",
			write_string_array<JsonValue>(
				outcome.undone_entry_descriptions,
				unnamed_written_undo_entry,
				maximum_written_name_length));
		writer.set_integer("undoStackPositionAfter", outcome.undo_stack_position_after);

		// Whether the step removed the agent's own entry. Reported because an undo of the
		// producer's own edit is something they should be told happened, even where the
		// call was permitted.
		writer.set_boolean("wasAgentEntry", outcome.was_agent_entry);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_revert_agent_changes_result(const daw::revert_outcome& outcome)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_integer("revertedEntryCount", outcome.reverted_entry_count);
		writer.set_value(
			"undoneEntryDescriptions",
			write_string_array<JsonValue>(
				outcome.undone_entry_descriptions,
				unnamed_written_undo_entry,
				maximum_written_name_length));

		// Both positions, because the schema's own account of a walk that stopped short of
		// the marker is the two disagreeing — and `undo_tools.h` reports that case as a
		// success rather than a failure precisely so the producer is told how far it got.
		writer.set_integer("undoPositionMarker", outcome.undo_position_marker);
		writer.set_integer("undoStackPositionAfter", outcome.undo_stack_position_after);

		// False for every revert the foreign-entry check permitted, which is every revert
		// that was not acknowledged past it (requirement 10.5).
		writer.set_boolean("revertedForeignEntries", outcome.reverted_foreign_entries);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// FX
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	JsonValue write_add_fx_result(const daw::tools::add_fx_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("fxIndex", result.fx_index);
		writer.set_named_string(
			"fxName",
			result.fx_name,
			unnamed_written_fx,
			maximum_written_name_length);

		// The chain's length after the insert, because an FX added at a position
		// renumbers everything above it.
		writer.set_integer("chainLength", result.chain_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_remove_fx_result(const daw::tools::remove_fx_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("fxIndex", result.fx_index);
		writer.set_named_string(
			"fxName",
			result.fx_name,
			unnamed_written_fx,
			maximum_written_name_length);
		writer.set_integer("chainLength", result.chain_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_fx_bypass_result(const daw::tools::set_fx_bypass_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("fxIndex", result.fx_index);
		writer.set_named_string(
			"fxName",
			result.fx_name,
			unnamed_written_fx,
			maximum_written_name_length);
		writer.set_boolean("bypassed", result.bypassed);

		// The previous state, so a call that changed nothing is distinguishable from one
		// that did — which is what the agent needs in order not to report a bypass it did
		// not cause.
		writer.set_boolean("previousBypassed", result.previous_bypassed);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_set_fx_parameter_result(const daw::tools::set_fx_parameter_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("fxIndex", result.fx_index);
		writer.set_named_string(
			"fxName",
			result.fx_name,
			unnamed_written_fx,
			maximum_written_name_length);
		writer.set_named_string(
			"parameterName",
			result.parameter_name,
			unnamed_written_fx_parameter,
			maximum_written_name_length);
		writer.set_number("value", result.value);
		writer.set_number("previousValue", result.previous_value);

		// Whether the plugin's own range moved the value. Reported because a parameter
		// set to the edge of its range reads back as a different number, and an agent
		// that was not told would report a value the producer cannot see on the plugin.
		writer.set_boolean("clamped", result.clamped);

		// The plugin's own rendering — "-12.0 dB" — which is the string to quote to the
		// producer. Optional because not every plugin provides one.
		writer.set_optional_string(
			"formattedValue",
			reported_string_view(result.formatted_value),
			maximum_written_formatted_value_length);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_get_fx_parameters_result(const daw::tools::get_fx_parameters_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_integer("fxIndex", result.fx_index);
		writer.set_named_string(
			"fxName",
			result.fx_name,
			unnamed_written_fx,
			maximum_written_name_length);
		writer.set_value(
			"parameters",
			write_array_of<JsonValue>(
				result.parameters,
				[](const daw::tools::fx_parameter& parameter) {
					ToolOutputWriter<JsonValue> entry;

					entry.set_integer("index", parameter.index);
					entry.set_named_string(
						"name",
						parameter.name,
						unnamed_written_fx_parameter,
						maximum_written_name_length);
					entry.set_number("value", parameter.value);

					// Both bounds, because a value on its own says nothing about where in
					// the parameter's range it sits and the agent's next call is usually a
					// relative one.
					entry.set_number("minimumValue", parameter.minimum_value);
					entry.set_number("maximumValue", parameter.maximum_value);
					entry.set_optional_string(
						"formattedValue",
						reported_string_view(parameter.formatted_value),
						maximum_written_formatted_value_length);

					return entry.release();
				}));
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_track_fx_result(const daw::tools::list_track_fx_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_value(
			"fx",
			write_array_of<JsonValue>(result.fx, [](const daw::tools::fx_chain_entry& entry) {
				return write_fx_chain_entry<JsonValue>(entry);
			}));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_installed_fx_result(const daw::tools::list_installed_fx_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"matches",
			write_array_of<JsonValue>(
				result.matches,
				[](const daw::tools::installed_fx& installed) {
					ToolOutputWriter<JsonValue> entry;

					// The identifier is what `add_fx` takes, so it is the one field of this
					// result the agent acts on rather than reads.
					entry.set_named_string(
						"identifier",
						installed.identifier,
						unnamed_written_fx_identifier,
						maximum_written_name_length);
					entry.set_named_string(
						"name",
						installed.name,
						unnamed_written_fx,
						maximum_written_name_length);

					// Both optional, and emitted whenever the scan carried them. `format`
					// is a closed enumeration the host already maps onto — `unknown` for a
					// prefix it does not recognise — so an empty string here is the scan
					// not having classified the plugin rather than a value to invent.
					entry.set_optional_string(
						"vendor",
						installed.vendor,
						maximum_written_vendor_length);
					entry.set_optional_string(
						"format",
						installed.format,
						maximum_written_name_length);

					// Emitted always. Whether a plugin is an instrument decides which track
					// it belongs on, and an agent that was not told would put a synthesiser
					// at the end of a chain.
					entry.set_boolean("instrument", installed.is_instrument);

					return entry.release();
				}));
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_apply_fx_destructively_result(
		const daw::tools::apply_fx_destructively_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));

		// Named rather than counted, because this is the one FX tool whose effect is not
		// reversible by bypassing anything: each GUID is an item whose audio changed.
		writer.set_value(
			"processedItemGuids",
			write_string_array<JsonValue>(
				result.processed_item_guids,
				no_substitute_for_a_guid,
				maximum_written_name_length));
		writer.set_boolean("replacedExistingTake", result.replaced_existing_take);
		writer.set_integer("appliedFxCount", result.applied_fx_count);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// Items
	// ---------------------------------------------------------------------------

	// One of the two tools whose own output schema carries `actions` alongside a sibling
	// field it also requires, which is why it returns `handler_success` and why
	// `tool_partial_outcome` is never its shape — `handler_action_outcomes` has nowhere to
	// hold `items`.
	template <typename JsonValue>
	JsonValue write_set_item_properties_result(
		const daw::tools::set_item_properties_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("actions", write_action_outcomes<JsonValue>(result.actions));
		writer.set_value("items", write_item_placements<JsonValue>(result.items));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_move_items_result(const daw::tools::move_items_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("items", write_item_placements<JsonValue>(result.items));

		// The offset that was actually applied, which is not the one asked for when an
		// item would have gone before zero.
		writer.set_number("appliedOffsetSeconds", result.applied_offset_seconds);
		writer.set_count("clampedItemCount", result.clamped_item_count);

		// Optional: a move along the timeline names no destination track.
		if (result.to_track.has_value()) {
			writer.set_value("toTrack", write_track_reference<JsonValue>(*result.to_track));
		}

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_move_all_items_result(const daw::tools::move_all_items_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		// `minItems: 1` on the schema, so the whole-project case is the field's absence
		// rather than an empty array — and `wholeProject` below is what says so.
		writer.set_non_empty_array("tracks", write_track_references<JsonValue>(result.tracks));

		writer.set_boolean("wholeProject", result.whole_project);
		writer.set_count("movedItemCount", result.moved_item_count);
		writer.set_number("appliedOffsetSeconds", result.applied_offset_seconds);
		writer.set_count("clampedItemCount", result.clamped_item_count);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_split_items_result(const daw::tools::split_items_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"splits",
			write_array_of<JsonValue>(result.splits, [](const daw::tools::item_split& split) {
				ToolOutputWriter<JsonValue> entry;

				// The original GUID as well as both halves, because REAPER's split
				// replaces one item with two and an agent holding the original needs told
				// which pair it became.
				entry.set_string(
					"originalItemGuid",
					split.original_item_guid,
					maximum_written_name_length);
				entry.set_value("leftItem", write_item_placement<JsonValue>(split.left_item));
				entry.set_value("rightItem", write_item_placement<JsonValue>(split.right_item));

				return entry.release();
			}));

		// The items the position missed. Reported rather than dropped: an item the split
		// did not touch is an item still whole, and silence here reads as a split that
		// happened.
		writer.set_value(
			"skippedItemGuids",
			write_string_array<JsonValue>(
				result.skipped_item_guids,
				no_substitute_for_a_guid,
				maximum_written_name_length));
		writer.set_number("position", result.position_seconds);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_delete_items_result(const daw::tools::delete_items_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"deletedItemGuids",
			write_string_array<JsonValue>(
				result.deleted_item_guids,
				no_substitute_for_a_guid,
				maximum_written_name_length));

		// The tracks the deletion left gaps in, so the agent can look at them rather than
		// re-reading the whole project.
		writer.set_value(
			"affectedTracks",
			write_track_references<JsonValue>(result.affected_tracks));

		// How much material went, which is the figure that tells a producer whether this
		// was the edit they meant.
		writer.set_number("deletedLengthSeconds", result.deleted_length_seconds);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_selected_items_result(
		const daw::tools::list_selected_items_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		// Echoed back, because the same session answers the three scopes very differently
		// and "nothing is selected" needs saying about the scope that was asked.
		writer.set_string(
			"scope",
			describe_item_scope(result.scope),
			maximum_written_name_length);

		writer.set_value(
			"items",
			write_array_of<JsonValue>(
				result.items,
				[](const daw::tools::selected_item& item) {
					ToolOutputWriter<JsonValue> entry;

					entry.set_string("guid", item.guid, maximum_written_name_length);
					entry.set_string("trackGuid", item.track_guid, maximum_written_name_length);

					// The track's name as well as its GUID: this is a read the producer
					// hears read back, and a GUID is not something they can be told.
					entry.set_string("trackName", item.track_name, maximum_written_name_length);
					entry.set_number("position", item.position_seconds);
					entry.set_number("length", item.length_seconds);

					// Optional, and absent for an item with no take.
					entry.set_optional_string(
						"takeName",
						reported_string_view(item.take_name),
						maximum_written_name_length);

					entry.set_boolean("muted", item.muted);
					entry.set_boolean("selected", item.selected);

					return entry.release();
				}));
		writer.set_count("totalInRange", result.total_in_range);
		writer.set_boolean("truncated", result.truncated);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_import_audio_file_result(
		const daw::tools::import_audio_file_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_value("item", write_item_placement<JsonValue>(result.item));

		// The path REAPER resolved, which is not always the one the call named.
		writer.set_named_string(
			"filePath",
			result.file_path,
			unnamed_written_file_path,
			maximum_written_file_path_length);

		// Both optional, and both emitted when the probe read them. A file at a different
		// rate from the project is the usual cause of a producer hearing the wrong pitch,
		// so it is worth the agent being able to say so.
		writer.set_optional_integer("sampleRate", result.sample_rate);
		writer.set_optional_integer("channelCount", result.channel_count);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_import_midi_file_result(const daw::tools::import_midi_file_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("track", write_track_reference<JsonValue>(result.track));
		writer.set_value("item", write_item_placement<JsonValue>(result.item));
		writer.set_named_string(
			"filePath",
			result.file_path,
			unnamed_written_file_path,
			maximum_written_file_path_length);

		// A multi-track MIDI file imports as several tracks, and the count is how the
		// agent knows the project grew by more than the one track it addressed.
		writer.set_integer("importedTrackCount", result.imported_track_count);
		writer.set_integer("noteCount", result.note_count);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// Track state
	// ---------------------------------------------------------------------------

	// The other tool whose own output schema carries `actions`, alongside
	// `aliasesLearned`, which is the sibling `handler_action_outcomes` has nowhere to
	// hold.
	template <typename JsonValue>
	JsonValue write_set_track_state_result(const daw::tools::set_track_state_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value("actions", write_action_outcomes<JsonValue>(result.actions));

		// Required, and emitted empty when the call learned none. Requirement 8.x's own
		// report: an alias the agent believes it taught and the store did not keep is the
		// failure this field exists to make visible.
		writer.set_value(
			"aliasesLearned",
			write_track_references<JsonValue>(result.aliases_learned));

		// Optional, and present only where an alias was learned — requirement 8.10 has
		// the producer told when an alias is learned in an unsaved session, because the
		// aliases as of the last save are what survive.
		writer.set_optional_boolean("projectSaved", result.project_saved);

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_list_tracks_result(const daw::tools::list_tracks_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_value(
			"tracks",
			write_array_of<JsonValue>(
				result.tracks,
				[](const daw::tools::reported_track_state& track) {
					ToolOutputWriter<JsonValue> entry;

					entry.set_string("guid", track.guid, maximum_written_name_length);
					entry.set_integer("index", track.index);
					entry.set_string("name", track.name, maximum_written_name_length);

					// Emitted always, including empty, and this is the clearest case in
					// the 42 of why. `list_tracks` is the one place the agent learns what
					// aliases a track holds; a track reported without the field is
					// indistinguishable from a build that does not report aliases, and the
					// agent's next move is either to teach one that already exists or to
					// refuse to use one that does. An empty array says "this track holds
					// none", which is an answer.
					entry.set_value(
						"aliases",
						write_string_array<JsonValue>(
							track.aliases,
							unnamed_written_alias,
							maximum_written_alias_length));

					entry.set_string(
						"role",
						to_schema_string(track.role),
						maximum_written_name_length);
					entry.set_number("volume", track.volume_decibels);
					entry.set_number("pan", track.pan_percent);
					entry.set_boolean("muted", track.muted);
					entry.set_boolean("soloed", track.soloed);
					entry.set_boolean("armed", track.armed);
					entry.set_integer("itemCount", track.item_count);
					entry.set_optional_color("color", track.color);

					// Optional, and absent when the call asked for the listing without the
					// chain — which is a different answer from a track with an empty
					// chain, and the only reason this is an optional on the report rather
					// than a vector.
					if (track.fx_chain.has_value()) {
						entry.set_value(
							"fxChain",
							write_string_array<JsonValue>(
								*track.fx_chain,
								std::string_view{},
								maximum_written_name_length));
					}

					return entry.release();
				}));

		return writer.release();
	}

	template <typename JsonValue>
	JsonValue write_project_summary_result(const daw::tools::project_summary_result& result)
	{
		ToolOutputWriter<JsonValue> writer;

		writer.set_string("projectName", result.project_name, maximum_written_name_length);
		writer.set_number("tempo", result.tempo);

		daw::tools::tempo_time_signature signature;
		signature.numerator = result.time_signature_numerator;
		signature.denominator = result.time_signature_denominator;

		writer.set_value("timeSignature", write_time_signature<JsonValue>(signature));
		writer.set_integer("sampleRate", result.sample_rate);
		writer.set_integer("trackCount", result.track_count);
		writer.set_number("projectLength", result.project_length);
		writer.set_integer("markerCount", result.marker_count);
		writer.set_integer("regionCount", result.region_count);
		writer.set_integer("tempoChangeCount", result.tempo_change_count);

		return writer.release();
	}

	// ---------------------------------------------------------------------------
	// The writer the three overload-set families take
	//
	// One object with an overload per result type, rather than nineteen functions, so that
	// `compose_tool_handler_registry`'s single `write_result` parameter serves fx, item,
	// and track state at once — their result types are distinct, so overload resolution
	// picks the right member and there is no third place to forget one.
	//
	// The mirror of `ToolRequestReader`: `register_fx_tools` and its two neighbours call
	// `write_result(result)` on a value whose type is the only thing identifying the tool,
	// so a missing overload is a compile failure at the composition root rather than a tool
	// that reports the wrong payload.
	//
	// One asymmetry with the reader, and it is forced rather than chosen. `ToolRequestReader`
	// is a plain class whose `operator()` members are each templated on the document type,
	// because a reader is *handed* a payload and deduces the type from it. A writer has to
	// name its return type before it has seen anything, so the parameter belongs on the
	// class. The composition root therefore spells `ToolResultWriter<nlohmann::json>{}` and
	// `ToolRequestReader{}`.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	class ToolResultWriter {
	public:
		// --- fx ---

		JsonValue operator()(const daw::tools::add_fx_result& result) const
		{
			return write_add_fx_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::remove_fx_result& result) const
		{
			return write_remove_fx_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::set_fx_bypass_result& result) const
		{
			return write_set_fx_bypass_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::set_fx_parameter_result& result) const
		{
			return write_set_fx_parameter_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::get_fx_parameters_result& result) const
		{
			return write_get_fx_parameters_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::list_track_fx_result& result) const
		{
			return write_list_track_fx_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::list_installed_fx_result& result) const
		{
			return write_list_installed_fx_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::apply_fx_destructively_result& result) const
		{
			return write_apply_fx_destructively_result<JsonValue>(result);
		}

		// --- items ---

		JsonValue operator()(const daw::tools::set_item_properties_result& result) const
		{
			return write_set_item_properties_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::move_items_result& result) const
		{
			return write_move_items_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::move_all_items_result& result) const
		{
			return write_move_all_items_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::split_items_result& result) const
		{
			return write_split_items_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::delete_items_result& result) const
		{
			return write_delete_items_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::list_selected_items_result& result) const
		{
			return write_list_selected_items_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::import_audio_file_result& result) const
		{
			return write_import_audio_file_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::import_midi_file_result& result) const
		{
			return write_import_midi_file_result<JsonValue>(result);
		}

		// --- track state ---

		JsonValue operator()(const daw::tools::set_track_state_result& result) const
		{
			return write_set_track_state_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::list_tracks_result& result) const
		{
			return write_list_tracks_result<JsonValue>(result);
		}

		JsonValue operator()(const daw::tools::project_summary_result& result) const
		{
			return write_project_summary_result<JsonValue>(result);
		}
	};

	// ---------------------------------------------------------------------------
	// Binding the write side of the seven per-family codecs
	//
	// Write members only, so a codec whose readers were bound by
	// `tool_request_readers.h` is completed by these rather than rebuilt — which is why
	// each takes the codec by reference and populates it rather than returning one.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	void bind_track_structure_result_writers(
		daw::tools::track_structure_payload_codec<JsonValue>& codec)
	{
		codec.write_create_track_result = [](const daw::tools::create_track_result& result) {
			return write_create_track_result<JsonValue>(result);
		};

		codec.write_delete_track_result = [](const daw::tools::delete_track_result& result) {
			return write_delete_track_result<JsonValue>(result);
		};

		codec.write_duplicate_track_result = [](const daw::tools::duplicate_track_result& result) {
			return write_duplicate_track_result<JsonValue>(result);
		};

		codec.write_set_folder_structure_result =
			[](const daw::tools::set_folder_structure_result& result) {
				return write_set_folder_structure_result<JsonValue>(result);
			};
	}

	template <typename JsonValue>
	void bind_marker_region_result_writers(
		daw::tools::marker_region_payload_codec<JsonValue>& codec)
	{
		codec.write_create_marker_result = [](const daw::tools::create_marker_result& result) {
			return write_create_marker_result<JsonValue>(result);
		};

		codec.write_create_region_result = [](const daw::tools::create_region_result& result) {
			return write_create_region_result<JsonValue>(result);
		};

		codec.write_update_marker_or_region_result =
			[](const daw::tools::update_marker_or_region_result& result) {
				return write_update_marker_or_region_result<JsonValue>(result);
			};

		codec.write_delete_marker_or_region_result =
			[](const daw::tools::delete_marker_or_region_result& result) {
				return write_delete_marker_or_region_result<JsonValue>(result);
			};

		codec.write_change_tempo_map_result =
			[](const daw::tools::change_tempo_map_result& result) {
				return write_change_tempo_map_result<JsonValue>(result);
			};

		codec.write_set_time_selection_result =
			[](const daw::tools::set_time_selection_result& result) {
				return write_set_time_selection_result<JsonValue>(result);
			};
	}

	template <typename JsonValue>
	void bind_marker_region_read_result_writers(
		daw::tools::marker_region_read_payload_codec<JsonValue>& codec)
	{
		codec.write_list_markers_result = [](const daw::tools::list_markers_result& result) {
			return write_list_markers_result<JsonValue>(result);
		};

		codec.write_list_regions_result = [](const daw::tools::list_regions_result& result) {
			return write_list_regions_result<JsonValue>(result);
		};
	}

	template <typename JsonValue>
	void bind_tempo_result_writers(daw::tools::tempo_payload_codec<JsonValue>& codec)
	{
		codec.write_list_tempo_changes_result =
			[](const daw::tools::list_tempo_changes_result& result) {
				return write_list_tempo_changes_result<JsonValue>(result);
			};
	}

	// All seven routing writers, including the two tools whose readers are absent:
	// `remove_bus` and `get_routing` are addressed entirely by target resolution, so they
	// read nothing and still have results to report.
	template <typename JsonValue>
	void bind_routing_result_writers(daw::tools::routing_payload_codec<JsonValue>& codec)
	{
		codec.write_create_send_result = [](const daw::tools::create_send_result& result) {
			return write_create_send_result<JsonValue>(result);
		};

		codec.write_remove_send_result = [](const daw::tools::remove_send_result& result) {
			return write_remove_send_result<JsonValue>(result);
		};

		codec.write_set_send_state_result = [](const daw::tools::set_send_state_result& result) {
			return write_set_send_state_result<JsonValue>(result);
		};

		codec.write_set_parent_send_result =
			[](const daw::tools::set_parent_send_result& result) {
				return write_set_parent_send_result<JsonValue>(result);
			};

		codec.write_create_bus_result = [](const daw::tools::create_bus_result& result) {
			return write_create_bus_result<JsonValue>(result);
		};

		codec.write_remove_bus_result = [](const daw::tools::remove_bus_result& result) {
			return write_remove_bus_result<JsonValue>(result);
		};

		codec.write_get_routing_result = [](const daw::tools::get_routing_result& result) {
			return write_get_routing_result<JsonValue>(result);
		};
	}

	template <typename JsonValue>
	void bind_render_result_writer(daw::tools::render_payload_codec<JsonValue>& codec)
	{
		codec.write_render_result = [](const daw::render_result& result) {
			return write_render_result<JsonValue>(result);
		};
	}

	// The undo codec is writers only — both tools' one input field,
	// `confirmedForeignUndo`, arrives on `daw::tool_call`, so there is no
	// `bind_undo_request_readers` for this to sit beside.
	template <typename JsonValue>
	void bind_undo_result_writers(daw::tools::undo_payload_codec<JsonValue>& codec)
	{
		codec.write_undo_last_action_result = [](const daw::undo_last_action_outcome& outcome) {
			return write_undo_last_action_result<JsonValue>(outcome);
		};

		codec.write_revert_agent_changes_result = [](const daw::revert_outcome& outcome) {
			return write_revert_agent_changes_result<JsonValue>(outcome);
		};
	}

}

#endif
