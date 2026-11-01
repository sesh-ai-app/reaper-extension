// One reader per tool: the already-validated input, turned into the argument struct
// each `src/daw/tools/*.h` handler already takes (task 24.1, requirements 4.1, 4.4,
// 9.1, 23.1).
//
// ---------------------------------------------------------------------------
// What this file is the other half of
//
// Ten tool families declare a payload seam and none of them implements one. Seven do it
// as a `*_payload_codec` struct of callables, and three — fx, item, and track state — do
// it as a reader and a writer passed as template parameters, because each of their
// readers is an overload set across the family's request types and an overload set does
// not fit in a `std::function`. `extension_composition.h:566` gathers the seven;
// `compose_tool_handler_registry` takes the pair.
//
// Every one of those seams is empty in this build, every registration call returns early
// on an incomplete codec, and so none of the 42 tools is registered. This file is the
// read side of all ten. The write side is task 24.2 and lands beside it — nothing here
// touches a `write_*` member, and the binders below populate only `read_*` so that
// adding the writers is an addition rather than a rewrite.
//
// ---------------------------------------------------------------------------
// Three ways to report an input that cannot be read, because there are three seams
//
// A reader returns the request type its handler takes. Those signatures belong to tasks
// 10.3 through 10.9 and this task does not get to change them, so how a reader says
// "there is no request here" is decided by the seam rather than chosen:
//
//   - **fx, item, and track state** take `bool read_request(payload, request)`. False is
//     the designed answer and each family already reports it: `fx_input_unreadable`,
//     `item_input_unreadable`, `track_state_input_unreadable`. Nineteen tools.
//
//   - **routing and render** take `Request(const payload&)` and their registration
//     checks `validated_input == nullptr` before calling it. So an unreadable payload is
//     reported by not being a payload: `tool_call_reader.h` hands the executor a call
//     with no input, and both families answer with `missing_tool_input` and a sentence
//     naming what could not be read. Eight tools.
//
//   - **track structure, markers and regions, the two region reads, and the tempo map
//     read** take the whole execution context and their registration calls the reader
//     unguarded. A null input reaches the reader, and there is nothing it can return
//     that is safe: `apply_create_marker` has no precondition to fail, so a
//     default-constructed request creates a marker at position zero with no name from a
//     payload nobody could read. These readers therefore throw
//     `ToolInputNotReadable`, which `tool_executor.h` catches on both the mutating and
//     the non-mutating path and reports as a failed action carrying the reason — with
//     the undo block closed by `undo_manager`'s own guard and no undo position reported,
//     because nothing was applied. Thirteen tools.
//
// The remaining two are `undo_last_action` and `revert_agent_changes`, which read nothing
// out of a payload at all: `undo_payload_codec` holds writers only, and the one field
// either tool carries is `confirmedForeignUndo`, which arrives on `daw::tool_call`.
//
// `delete_track` and `duplicate_track` need no throw and have none. Neither reads a field
// out of the payload — their target comes entirely from resolution — so the reader
// substitutes `unreadable_tool_input_identifier` for a GUID resolution did not produce,
// and `apply_delete_track` reports `unknown_track` having touched nothing. The same
// substitution covers the other two structural tools in the case where resolution came
// back with fewer targets than the payload named.
//
// What is the same in all three: a failed action carrying a reason, nothing applied, and
// no undefined behaviour. What never happens: a request made up out of defaults.
// `routing_tools.h` names that failure exactly — a `create_bus` whose type nobody
// extracted would build a folder because `bus_type` happens to default to summing — and
// `render_tool.h` names its own, one file where the producer asked for eight.
//
// ---------------------------------------------------------------------------
// Where a GUID comes from, and the one place a reader cannot produce one
//
// Target resolution runs before any handler (requirement 9.1), so a tool addressing a
// track gets it from `tool_execution_context::resolved_targets` rather than from the
// payload — which is why the four track structure readers take the context and the
// routing readers do not need to. The order is the order `tool_call_reader.h` put the
// selectors in, and the two sides agree because both read the same payload to decide it;
// `read_track_selectors` is the one statement of that order.
//
// `set_track_state` and `list_tracks` are the exception and it is worth being plain
// about. Their requests hold GUIDs, their readers are handed the payload alone, and
// `trackSelector` admits a name pattern that only the Object Resolver can turn into a
// GUID. So a pattern in those two tools is reported as an input this build cannot read,
// with a message telling the agent to address the track by the GUID the project context
// already gave it. The alternatives were worse: dropping the selector would widen
// `list_tracks` to the whole project, which answers a different question, and
// substituting a placeholder would have `set_track_state` report a track as unknown when
// resolution had just proved it exists.

#ifndef SESH_AI_TRANSPORT_TOOL_REQUEST_READERS_H
#define SESH_AI_TRANSPORT_TOOL_REQUEST_READERS_H

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include <transport/tool_payload_reading.h>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// Reporting an input that cannot be read
	// ---------------------------------------------------------------------------

	// The reader's answer for the four families whose registration calls it with no
	// null-input guard. See the file comment.
	//
	// Derived from `std::runtime_error` because `tool_executor.h` catches
	// `std::exception` and reports `what()` as the failed action's reason, so the message
	// is the whole payload of this type.
	class ToolInputNotReadable : public std::runtime_error {
	public:
		ToolInputNotReadable(std::string_view tool_name, std::string_view problem)
			: std::runtime_error(
				std::string{tool_name} + " was given an input this build could not read ("
					+ std::string{problem} + "), so nothing was attempted")
		{
		}
	};

	// The identifier a reader substitutes when the call carried no readable input and the
	// handler it feeds checks its target against the project.
	//
	// Not a GUID, deliberately: it has to be an identifier the project cannot contain, so
	// that `apply_delete_track` and its three neighbours report `unknown_track` and change
	// nothing. Phrased as a sentence because it travels to the agent as the failed
	// action's `actionTarget`.
	inline constexpr std::string_view unreadable_tool_input_identifier{
		"(this call carried no input this build could read)"};

	// ---------------------------------------------------------------------------
	// The closed enumerations, spelled once each
	//
	// Mapped by name rather than by declaration order. An enumeration read as a position
	// into the C++ enum would silently change meaning the day somebody inserts a value,
	// and every one of these decides something audible.
	// ---------------------------------------------------------------------------

	inline constexpr std::string_view send_mode_post_fader{"post_fader"};
	inline constexpr std::string_view send_mode_pre_fx{"pre_fx"};
	inline constexpr std::string_view send_mode_post_fx{"post_fx"};

	inline daw::tools::routing_send_mode send_mode_at(std::size_t position)
	{
		switch (position) {
			case 1:
				return daw::tools::routing_send_mode::pre_fx;
			case 2:
				return daw::tools::routing_send_mode::post_fx;
			default:
				return daw::tools::routing_send_mode::post_fader;
		}
	}

	inline daw::bus_type bus_type_at(std::size_t position)
	{
		return position == 1 ? daw::bus_type::aux : daw::bus_type::summing;
	}

	inline daw::tools::item_scope item_scope_at(std::size_t position)
	{
		switch (position) {
			case 1:
				return daw::tools::item_scope::selected_tracks;
			case 2:
				return daw::tools::item_scope::time_selection;
			default:
				return daw::tools::item_scope::selected_items;
		}
	}

	inline daw::render_source render_source_at(std::size_t position)
	{
		switch (position) {
			case 1:
				return daw::render_source::stems;
			case 2:
				return daw::render_source::stems_via_master;
			default:
				return daw::render_source::master_mix;
		}
	}

	inline daw::render_bounds render_bounds_at(std::size_t position)
	{
		switch (position) {
			case 0:
				return daw::render_bounds::region;
			case 1:
				return daw::render_bounds::time_selection;
			default:
				return daw::render_bounds::project;
		}
	}

	inline daw::render_output_format render_output_format_at(std::size_t position)
	{
		switch (position) {
			case 1:
				return daw::render_output_format::mp3;
			case 2:
				return daw::render_output_format::flac;
			default:
				return daw::render_output_format::wav;
		}
	}

	// ---------------------------------------------------------------------------
	// The acknowledgement fields
	// ---------------------------------------------------------------------------

	// The four flags the refusal schema names, as `daw::tool_call` holds them.
	//
	// Read here rather than by each tool's own reader because the Tool Executor carries
	// them for every tool: a retry after a refusal sets one, and the component that acts
	// on it is a precondition check or a handler rather than this reader. `render_tool.h`
	// is explicit that `confirmedOverwrite` has exactly one reader for this reason.
	struct AcknowledgementFlags {
		bool confirmed_track_selection = false;
		bool confirmed_signal_cycle = false;
		bool confirmed_overwrite = false;
		bool confirmed_foreign_undo = false;
	};

	template <typename JsonValue>
	AcknowledgementFlags read_acknowledgement_flags(ToolInputReader<JsonValue>& reader)
	{
		AcknowledgementFlags flags;

		flags.confirmed_track_selection =
			reader.optional_boolean("confirmedTrackSelection").value_or(false);
		flags.confirmed_signal_cycle = reader.optional_boolean("confirmedSignalCycle").value_or(false);
		flags.confirmed_overwrite = reader.optional_boolean("confirmedOverwrite").value_or(false);
		flags.confirmed_foreign_undo = reader.optional_boolean("confirmedForeignUndo").value_or(false);

		return flags;
	}

	// ---------------------------------------------------------------------------
	// Shared field shapes
	// ---------------------------------------------------------------------------

	// The GUID a `trackSelector` carries, for the two tools whose per-entry request holds
	// a GUID and whose reader never sees the resolved targets. See the file comment on why
	// a name pattern is reported rather than dropped or stood in for.
	template <typename JsonValue>
	std::string read_track_selector_identifier(
		ToolInputReader<JsonValue>& parent,
		const char* field_name)
	{
		const JsonValue* const selector = parent.optional_object(field_name);

		if (selector == nullptr) {
			if (!parent.has(field_name)) {
				parent.record_problem(
					std::string{"\""} + field_name + "\" is required and the call did not carry it");
			}

			return std::string{};
		}

		ToolInputReader<JsonValue> selector_reader = parent.reader_for(*selector);

		if (selector_reader.has("guid")) {
			return selector_reader.required_identifier("guid");
		}

		if (selector_reader.has("namePattern")) {
			parent.record_problem(
				std::string{"\""} + field_name
					+ "\" addresses a track by name pattern, and this tool reads its input without "
					  "the resolution a pattern needs — address the track by the guid the project "
					  "context reported");

			return std::string{};
		}

		parent.record_problem(
			std::string{"\""} + field_name + "\" carries neither a guid nor a namePattern");

		return std::string{};
	}

	// `timeSignature` on a tempo point. Absent keeps the preceding signature, which is
	// why the absence is carried rather than defaulted.
	template <typename JsonValue>
	std::optional<daw::tools::tempo_time_signature> read_time_signature(
		ToolInputReader<JsonValue>& parent)
	{
		const JsonValue* const signature = parent.optional_object("timeSignature");

		if (signature == nullptr) {
			return std::nullopt;
		}

		ToolInputReader<JsonValue> signature_reader = parent.reader_for(*signature);

		daw::tools::tempo_time_signature read;
		read.numerator = signature_reader.required_integer("numerator");
		read.denominator = signature_reader.required_integer("denominator");

		return read;
	}

	// The three read tools' shared window. One reader because
	// `list-markers.schema.json`, `list-regions.schema.json`, and
	// `list-tempo-changes.schema.json` carry the same three optional inputs with the same
	// meanings, which is the same reason `timeline_read_window` is one struct.
	//
	// The result cap is the one numeric bound this file checks, and not as a schema bound:
	// `maximum_results` is a `std::size_t`, so a negative count does not clamp, it becomes
	// an enormous one and asks for every marker in the project.
	template <typename JsonValue>
	daw::tools::timeline_read_window read_timeline_read_window(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::timeline_read_window window;

		window.start_position_seconds = reader.optional_number("startPosition");
		window.end_position_seconds = reader.optional_number("endPosition");

		const std::optional<int> maximum_results = reader.optional_integer("maximumResults");

		if (maximum_results.has_value()) {
			if (*maximum_results < 1) {
				reader.record_problem(
					"\"maximumResults\" asks for fewer than one result, which is not a number of "
					"results this read can return");
			} else {
				window.maximum_results = static_cast<std::size_t>(*maximum_results);
			}
		}

		return window;
	}

	// ---------------------------------------------------------------------------
	// Per-tool field readers
	//
	// Each one reads the fields that come out of the payload and nothing that comes out
	// of target resolution, so the same function serves two callers: the reader the codec
	// holds, and `tool_call_reader.h`'s check of whether the payload is readable at all.
	// One definition of what each tool's input has to look like, rather than two that can
	// disagree.
	// ---------------------------------------------------------------------------

	// --- track structure ---

	template <typename JsonValue>
	daw::tools::create_track_request read_create_track_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::create_track_request request;

		request.name = reader.required_string("name");
		request.color = reader.optional_color("color");

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_folder_structure_request read_set_folder_structure_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_folder_structure_request request;

		// Read for its presence and its length; the GUIDs themselves come from the
		// resolved targets.
		reader.required_array("tracks");

		request.move_to_top_level = reader.optional_boolean("moveToTopLevel").value_or(false);

		return request;
	}

	// --- markers, regions, the tempo map ---

	template <typename JsonValue>
	daw::tools::create_marker_request read_create_marker_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::create_marker_request request;

		request.position_seconds = reader.required_number("position");
		request.name = reader.required_string("name");
		request.color = reader.optional_color("color");

		return request;
	}

	// No range check here, and that is requirement 9.6 landing where the tool layer
	// already answers it: `apply_create_region` runs `check_end_after_start` before it
	// writes anything, so an inverted range is a failed action carrying
	// `invalid_time_range` rather than an input this reader refuses. One answer to one
	// question, which is what the task settled.
	template <typename JsonValue>
	daw::tools::create_region_request read_create_region_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::create_region_request request;

		request.start_seconds = reader.required_number("start");
		request.end_seconds = reader.required_number("end");
		request.name = reader.required_string("name");
		request.color = reader.optional_color("color");

		return request;
	}

	template <typename JsonValue>
	daw::tools::update_marker_or_region_request read_update_marker_or_region_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::update_marker_or_region_request request;

		request.guid = reader.required_identifier("guid");
		request.name = reader.optional_string("name");
		request.position_seconds = reader.optional_number("position");
		request.end_seconds = reader.optional_number("end");
		request.color = reader.optional_color("color");

		return request;
	}

	template <typename JsonValue>
	daw::tools::delete_marker_or_region_request read_delete_marker_or_region_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::delete_marker_or_region_request request;

		request.guid = reader.required_identifier("guid");

		return request;
	}

	template <typename JsonValue>
	daw::tools::change_tempo_map_request read_change_tempo_map_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::change_tempo_map_request request;

		request.replace_existing = reader.optional_boolean("replaceExisting").value_or(false);

		const JsonValue* const points = reader.required_array("points");

		if (points == nullptr) {
			return request;
		}

		for (const JsonValue& element : *points) {
			if (!element.is_object()) {
				reader.record_problem("\"points\" holds an entry that is not a tempo point");

				return request;
			}

			ToolInputReader<JsonValue> point_reader = reader.reader_for(element);

			daw::tools::tempo_map_point point;
			point.position_seconds = point_reader.required_number("position");
			point.beats_per_minute = point_reader.required_number("tempo");
			point.time_signature = read_time_signature(point_reader);
			point.linear_transition =
				point_reader.optional_boolean("linearTransition").value_or(false);

			request.points.push_back(std::move(point));
		}

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_time_selection_request read_set_time_selection_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_time_selection_request request;

		request.start_seconds = reader.required_number("start");
		request.end_seconds = reader.required_number("end");

		return request;
	}

	// --- routing ---

	template <typename JsonValue>
	daw::tools::create_send_request read_create_send_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::create_send_request request;

		request.volume_decibels = reader.optional_number("volume").value_or(0.0);
		request.pan_percent = reader.optional_number("pan").value_or(0.0);

		const std::optional<std::size_t> mode = reader.optional_enumeration(
			"mode",
			{send_mode_post_fader, send_mode_pre_fx, send_mode_post_fx});

		if (mode.has_value()) {
			request.mode = send_mode_at(*mode);
		}

		return request;
	}

	template <typename JsonValue>
	daw::tools::remove_send_request read_remove_send_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::remove_send_request request;

		request.send_index = reader.required_integer("sendIndex");

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_send_state_request read_set_send_state_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_send_state_request request;

		request.send_index = reader.required_integer("sendIndex");
		request.change.volume_decibels = reader.optional_number("volume");
		request.change.pan_percent = reader.optional_number("pan");
		request.change.muted = reader.optional_boolean("muted");

		const std::optional<std::size_t> mode = reader.optional_enumeration(
			"mode",
			{send_mode_post_fader, send_mode_pre_fx, send_mode_post_fx});

		if (mode.has_value()) {
			request.change.mode = send_mode_at(*mode);
		}

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_parent_send_request read_set_parent_send_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_parent_send_request request;

		request.enabled = reader.required_boolean("enabled");

		return request;
	}

	template <typename JsonValue>
	daw::tools::bus_construction_request read_create_bus_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::bus_construction_request request;

		request.type = bus_type_at(reader.required_enumeration("type", {"summing", "aux"}));
		request.bus_name = reader.required_string("name");
		request.send_volume_decibels = reader.optional_number("sendVolume").value_or(0.0);

		// `create-bus.schema.json` names no send mode, so the request's own default
		// stands. Left unread rather than invented, because the field the producer would
		// have set does not exist.
		request.fx_names = read_identifier_array(
			reader.optional_array("fxIdentifiers"),
			"fxIdentifiers",
			reader);

		return request;
	}

	// --- render ---

	template <typename JsonValue>
	daw::render_request read_render_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::render_request request;

		request.source = render_source_at(
			reader.required_enumeration("source", {"master_mix", "stems", "stems_via_master"}));
		request.bounds = render_bounds_at(
			reader.required_enumeration("bounds", {"region", "time_selection", "project"}));

		// Required when `bounds` is `region` and meaningless otherwise, and the
		// coordinator is what reports a bound of `region` with no region to render —
		// which is why this is read as optional rather than conditionally required here.
		if (reader.has("regionGuid")) {
			request.region_guid = reader.required_identifier("regionGuid");
		}

		const std::optional<std::size_t> output_format =
			reader.optional_enumeration("outputFormat", {"wav", "mp3", "flac"});

		if (output_format.has_value()) {
			request.output_format = render_output_format_at(*output_format);
		}

		request.bit_depth = reader.optional_integer("bitDepth");

		if (request.bit_depth.has_value()
			&& *request.bit_depth != 16
			&& *request.bit_depth != 24
			&& *request.bit_depth != 32) {
			reader.record_problem(
				"\"bitDepth\" is not one of 16, 24, 32, and a render written at a depth nobody "
				"asked for is a file the producer has to find and delete");
		}

		request.sample_rate = reader.optional_integer("sampleRate");

		// `confirmed_overwrite` is deliberately not read. `render_tool.h` copies it from
		// the framework's own extraction so that there is one reader of
		// `confirmedOverwrite`; two is how a refusal comes to be retried against a
		// coordinator that never heard about the approval.

		return request;
	}

	// --- fx ---

	template <typename JsonValue>
	daw::tools::add_fx_request read_add_fx_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::add_fx_request request;

		request.fx_identifier = reader.required_identifier("fxIdentifier");
		request.chain_position = reader.optional_integer("chainPosition");

		return request;
	}

	template <typename JsonValue>
	daw::tools::remove_fx_request read_remove_fx_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::remove_fx_request request;

		request.fx_index = reader.required_integer("fxIndex");

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_fx_bypass_request read_set_fx_bypass_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_fx_bypass_request request;

		request.fx_index = reader.required_integer("fxIndex");
		request.bypassed = reader.required_boolean("bypassed");

		return request;
	}

	template <typename JsonValue>
	daw::tools::set_fx_parameter_request read_set_fx_parameter_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_fx_parameter_request request;

		request.fx_index = reader.required_integer("fxIndex");
		request.parameter_name = reader.required_non_empty_string("parameterName");
		request.value = reader.required_number("value");

		return request;
	}

	template <typename JsonValue>
	daw::tools::get_fx_parameters_request read_get_fx_parameters_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::get_fx_parameters_request request;

		request.fx_index = reader.required_integer("fxIndex");

		return request;
	}

	template <typename JsonValue>
	daw::tools::list_installed_fx_request read_list_installed_fx_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::list_installed_fx_request request;

		// Required, and allowed to be empty: an empty search term is every installed
		// plugin up to the cap, which `apply_list_installed_fx` already bounds.
		request.search_term = reader.required_string("searchTerm");
		request.maximum_results = reader.optional_integer("maximumResults");

		return request;
	}

	template <typename JsonValue>
	daw::tools::apply_fx_destructively_request read_apply_fx_destructively_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::apply_fx_destructively_request request;

		request.replace_existing_take =
			reader.optional_boolean("replaceExistingTake").value_or(false);
		request.item_guids = read_identifier_array(
			reader.optional_array("itemGuids"),
			"itemGuids",
			reader);

		return request;
	}

	// --- items ---

	template <typename JsonValue>
	daw::tools::set_item_properties_request read_set_item_properties_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_item_properties_request request;

		const JsonValue* const changes = reader.required_array("changes");

		if (changes == nullptr) {
			return request;
		}

		for (const JsonValue& element : *changes) {
			if (!element.is_object()) {
				reader.record_problem("\"changes\" holds an entry that is not an object");

				return request;
			}

			ToolInputReader<JsonValue> change_reader = reader.reader_for(element);

			daw::tools::item_property_change change;
			change.item_guid = change_reader.required_identifier("itemGuid");
			change.position_seconds = change_reader.optional_number("position");
			change.length_seconds = change_reader.optional_number("length");
			change.fade_in_seconds = change_reader.optional_number("fadeInSeconds");
			change.fade_out_seconds = change_reader.optional_number("fadeOutSeconds");
			change.volume_decibels = change_reader.optional_number("volume");
			change.muted = change_reader.optional_boolean("muted");

			request.changes.push_back(std::move(change));
		}

		return request;
	}

	template <typename JsonValue>
	daw::tools::move_items_request read_move_items_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::move_items_request request;

		request.item_guids =
			read_identifier_array(reader.required_array("itemGuids"), "itemGuids", reader);
		request.offset_seconds = reader.optional_number("offsetSeconds");

		return request;
	}

	template <typename JsonValue>
	daw::tools::move_all_items_request read_move_all_items_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::move_all_items_request request;

		request.offset_seconds = reader.required_number("offsetSeconds");

		return request;
	}

	template <typename JsonValue>
	daw::tools::split_items_request read_split_items_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::split_items_request request;

		request.item_guids =
			read_identifier_array(reader.required_array("itemGuids"), "itemGuids", reader);
		request.position_seconds = reader.required_number("position");

		return request;
	}

	template <typename JsonValue>
	daw::tools::delete_items_request read_delete_items_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::delete_items_request request;

		request.item_guids =
			read_identifier_array(reader.required_array("itemGuids"), "itemGuids", reader);

		return request;
	}

	template <typename JsonValue>
	daw::tools::list_selected_items_request read_list_selected_items_fields(
		ToolInputReader<JsonValue>& reader)
	{
		daw::tools::list_selected_items_request request;

		const std::optional<std::size_t> scope = reader.optional_enumeration(
			"scope",
			{"selected_items", "selected_tracks", "time_selection"});

		if (scope.has_value()) {
			request.scope = item_scope_at(*scope);
		}

		request.include_take_names = reader.optional_boolean("includeTakeNames").value_or(true);

		return request;
	}

	template <typename JsonValue>
	daw::tools::import_file_request read_import_file_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::import_file_request request;

		request.file_path = reader.required_non_empty_string("filePath");
		request.position_seconds = reader.required_number("position");

		return request;
	}

	// --- track state ---

	template <typename JsonValue>
	daw::tools::set_track_state_request read_set_track_state_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::set_track_state_request request;

		const JsonValue* const changes = reader.required_array("changes");

		if (changes == nullptr) {
			return request;
		}

		for (const JsonValue& element : *changes) {
			if (!element.is_object()) {
				reader.record_problem("\"changes\" holds an entry that is not an object");

				return request;
			}

			ToolInputReader<JsonValue> change_reader = reader.reader_for(element);

			daw::tools::track_state_change change;
			change.track_guid = read_track_selector_identifier(change_reader, "track");
			change.name = change_reader.optional_string("name");
			change.alias = change_reader.optional_string("alias");
			change.volume_decibels = change_reader.optional_number("volume");
			change.pan_percent = change_reader.optional_number("pan");
			change.muted = change_reader.optional_boolean("muted");
			change.soloed = change_reader.optional_boolean("soloed");
			change.armed = change_reader.optional_boolean("armed");
			change.color = change_reader.optional_color("color");

			request.changes.push_back(std::move(change));
		}

		return request;
	}

	template <typename JsonValue>
	daw::tools::list_tracks_request read_list_tracks_fields(ToolInputReader<JsonValue>& reader)
	{
		daw::tools::list_tracks_request request;

		request.include_fx_chain = reader.optional_boolean("includeFxChain").value_or(true);

		const JsonValue* const tracks = reader.optional_array("tracks");

		if (tracks == nullptr) {
			return request;
		}

		for (const JsonValue& element : *tracks) {
			if (!element.is_object()) {
				reader.record_problem("\"tracks\" holds an entry that is not a track selector");

				return request;
			}

			ToolInputReader<JsonValue> selector_reader = reader.reader_for(element);

			if (selector_reader.has("namePattern")) {
				reader.record_problem(
					"\"tracks\" addresses a track by name pattern, and this tool reads its input "
					"without the resolution a pattern needs — address the track by the guid the "
					"project context reported");

				return request;
			}

			if (!selector_reader.has("guid")) {
				reader.record_problem(
					"\"tracks\" holds a selector carrying neither a guid nor a namePattern");

				return request;
			}

			request.track_guids.push_back(selector_reader.required_identifier("guid"));
		}

		return request;
	}

	// ---------------------------------------------------------------------------
	// Whether a tool's payload reads into its handler's argument at all
	//
	// The one table of which tool reads which fields, consulted by
	// `tool_call_reader.h`. A tool whose request is built entirely out of target
	// resolution has no fields to check, and the shape of the payload is then the whole
	// question — a payload that arrived as an array carries no `confirmedTrackSelection`
	// either.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	void check_tool_input_fields(
		std::string_view tool_name,
		ToolInputReader<JsonValue>& reader)
	{
		// --- track structure ---
		if (tool_name == daw::tools::create_track_tool_name) {
			read_create_track_fields(reader);
		} else if (tool_name == daw::tools::set_folder_structure_tool_name) {
			read_set_folder_structure_fields(reader);

			// --- markers, regions, the tempo map ---
		} else if (tool_name == daw::tools::create_marker_tool_name) {
			read_create_marker_fields(reader);
		} else if (tool_name == daw::tools::create_region_tool_name) {
			read_create_region_fields(reader);
		} else if (tool_name == daw::tools::update_marker_or_region_tool_name) {
			read_update_marker_or_region_fields(reader);
		} else if (tool_name == daw::tools::delete_marker_or_region_tool_name) {
			read_delete_marker_or_region_fields(reader);
		} else if (tool_name == daw::tools::change_tempo_map_tool_name) {
			read_change_tempo_map_fields(reader);
		} else if (tool_name == daw::tools::set_time_selection_tool_name) {
			read_set_time_selection_fields(reader);
		} else if (tool_name == daw::tools::list_markers_tool_name
			|| tool_name == daw::tools::list_regions_tool_name
			|| tool_name == daw::tools::list_tempo_changes_tool_name) {
			read_timeline_read_window(reader);

			// --- routing ---
		} else if (tool_name == "create_send") {
			read_create_send_fields(reader);
		} else if (tool_name == "remove_send") {
			read_remove_send_fields(reader);
		} else if (tool_name == "set_send_state") {
			read_set_send_state_fields(reader);
		} else if (tool_name == "set_parent_send") {
			read_set_parent_send_fields(reader);
		} else if (tool_name == "create_bus") {
			read_create_bus_fields(reader);

			// --- render ---
		} else if (tool_name == daw::tools::render_tool_name) {
			read_render_fields(reader);

			// --- fx ---
		} else if (tool_name == daw::tools::add_fx_tool_name) {
			read_add_fx_fields(reader);
		} else if (tool_name == daw::tools::remove_fx_tool_name) {
			read_remove_fx_fields(reader);
		} else if (tool_name == daw::tools::set_fx_bypass_tool_name) {
			read_set_fx_bypass_fields(reader);
		} else if (tool_name == daw::tools::set_fx_parameter_tool_name) {
			read_set_fx_parameter_fields(reader);
		} else if (tool_name == daw::tools::get_fx_parameters_tool_name) {
			read_get_fx_parameters_fields(reader);
		} else if (tool_name == daw::tools::list_installed_fx_tool_name) {
			read_list_installed_fx_fields(reader);
		} else if (tool_name == daw::tools::apply_fx_destructively_tool_name) {
			read_apply_fx_destructively_fields(reader);

			// --- items ---
		} else if (tool_name == daw::tools::set_item_properties_tool_name) {
			read_set_item_properties_fields(reader);
		} else if (tool_name == daw::tools::move_items_tool_name) {
			read_move_items_fields(reader);
		} else if (tool_name == daw::tools::move_all_items_tool_name) {
			read_move_all_items_fields(reader);
		} else if (tool_name == daw::tools::split_items_tool_name) {
			read_split_items_fields(reader);
		} else if (tool_name == daw::tools::delete_items_tool_name) {
			read_delete_items_fields(reader);
		} else if (tool_name == daw::tools::list_selected_items_tool_name) {
			read_list_selected_items_fields(reader);
		} else if (tool_name == daw::tools::import_audio_file_tool_name
			|| tool_name == daw::tools::import_midi_file_tool_name) {
			read_import_file_fields(reader);

			// --- track state ---
		} else if (tool_name == daw::tools::set_track_state_tool_name) {
			read_set_track_state_fields(reader);
		} else if (tool_name == daw::tools::list_tracks_tool_name) {
			read_list_tracks_fields(reader);
		}

		// Everything else reads no field out of the payload: `delete_track`,
		// `duplicate_track`, `remove_bus`, `get_routing`, `list_track_fx`,
		// `get_project_summary`, `undo_last_action`, `revert_agent_changes`. Each one is
		// either addressed entirely by target resolution or takes nothing but an
		// acknowledgement, both of which are read elsewhere.
	}

	// ---------------------------------------------------------------------------
	// The reader the three overload-set families take
	//
	// One object with an overload per request type, rather than nineteen functions, so
	// that `compose_tool_handler_registry`'s single `read_request` parameter serves fx,
	// item, and track state at once — their request types are distinct, so overload
	// resolution picks the right member and there is no third place to forget one.
	//
	// Every overload answers the same question: did the whole request read, or not. False
	// reaches the producer as the family's own `*_input_unreadable` failed action.
	// ---------------------------------------------------------------------------

	class ToolRequestReader {
	public:
		// --- fx ---

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::add_fx_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_add_fx_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::remove_fx_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_remove_fx_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::set_fx_bypass_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_set_fx_bypass_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::set_fx_parameter_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_set_fx_parameter_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::get_fx_parameters_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_get_fx_parameters_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::list_installed_fx_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_list_installed_fx_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(
			const JsonValue& payload,
			daw::tools::apply_fx_destructively_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_apply_fx_destructively_fields(reader);
			});
		}

		// `list_track_fx` takes nothing but the track, which target resolution carries.
		// The payload still has to be an object: one that is not carries no
		// `confirmedTrackSelection` either, and a call whose input is the wrong shape is
		// not a call this build should act on.
		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::list_track_fx_request& request) const
		{
			static_cast<void>(request);

			return payload.is_object();
		}

		// --- items ---

		template <typename JsonValue>
		bool operator()(
			const JsonValue& payload,
			daw::tools::set_item_properties_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_set_item_properties_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::move_items_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_move_items_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::move_all_items_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_move_all_items_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::split_items_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_split_items_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::delete_items_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_delete_items_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(
			const JsonValue& payload,
			daw::tools::list_selected_items_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_list_selected_items_fields(reader);
			});
		}

		// Both imports, which take the same three things and differ only in what REAPER
		// does with the file. `item_tools.h` passes the tool name so a reader could name
		// the call; the fields are identical, so nothing here needs it.
		template <typename JsonValue>
		bool operator()(
			const JsonValue& payload,
			daw::tools::import_file_request& request,
			std::string_view tool_name) const
		{
			static_cast<void>(tool_name);

			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_import_file_fields(reader);
			});
		}

		// --- track state ---

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::set_track_state_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_set_track_state_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(const JsonValue& payload, daw::tools::list_tracks_request& request) const
		{
			return read_into(payload, request, [](ToolInputReader<JsonValue>& reader) {
				return read_list_tracks_fields(reader);
			});
		}

		template <typename JsonValue>
		bool operator()(
			const JsonValue& payload,
			daw::tools::get_project_summary_request& request) const
		{
			static_cast<void>(request);

			return payload.is_object();
		}

	private:
		// Reads, and assigns only when the whole read was clean. A half-filled request is
		// the thing this seam exists to prevent: the handler checks the boolean, and a
		// request it was told not to use should not have been written over.
		template <typename JsonValue, typename Request, typename FieldReader>
		static bool read_into(
			const JsonValue& payload,
			Request& request,
			const FieldReader& read_fields)
		{
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			Request read = read_fields(reader);

			if (problem.anything_recorded()) {
				return false;
			}

			request = std::move(read);

			return true;
		}
	};

	// ---------------------------------------------------------------------------
	// Binding the read side of the seven per-family codecs
	//
	// Read members only. Task 24.2 adds `bind_*_result_writers` beside each of these and
	// nothing here moves — which is why these take the codec by reference and populate it
	// rather than returning a complete one.
	// ---------------------------------------------------------------------------

	namespace tool_reader_detail {

		// The GUID of one resolved target, or the unreadable-input identifier when there
		// is none. See the file comment: this is the substitution the four unguarded
		// families use where the handler already checks its target against the project.
		inline std::string resolved_identifier_at(
			const std::vector<daw::ResolvedTrack>& targets,
			std::size_t position)
		{
			if (position >= targets.size()) {
				return std::string{unreadable_tool_input_identifier};
			}

			return targets[position].reference.guid;
		}

		// Reads a tool's fields and throws when they do not read.
		//
		// The fallback the file comment describes, for the readers whose registration
		// calls them with no null-input guard. The message is the problem the reader
		// recorded, so an agent is told which field rather than that something was wrong.
		//
		// No input at all is its own answer and is checked first, because the three read
		// windows have no required field — so a payload nobody could read would otherwise
		// slide through as the default window and answer a question the call did not ask.
		template <typename JsonValue, typename FieldReader>
		auto read_or_throw(
			const daw::tool_execution_context<JsonValue>& context,
			std::string_view tool_name,
			const FieldReader& read_fields)
		{
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{context.call.validated_input, problem};

			if (!reader.is_readable_object()) {
				throw ToolInputNotReadable{tool_name, "it carried no readable input object"};
			}

			auto request = read_fields(reader);

			if (problem.anything_recorded()) {
				throw ToolInputNotReadable{tool_name, problem.description()};
			}

			return request;
		}

	}

	// The four structural tools. Three of them are addressed entirely by target
	// resolution, so their readers never touch the payload beyond asking which optional
	// selectors it carried — and a call with no readable input substitutes
	// `unreadable_tool_input_identifier`, which the handlers report as `unknown_track`
	// having changed nothing.
	template <typename JsonValue>
	void bind_track_structure_request_readers(
		daw::tools::track_structure_payload_codec<JsonValue>& codec)
	{
		codec.read_create_track_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				daw::tools::create_track_request request = tool_reader_detail::read_or_throw(
					context,
					daw::tools::create_track_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_create_track_fields(reader);
					});

				// The selector order `read_track_selectors` used: `afterTrack` first, then
				// `insideFolderOf`, each only when the payload carried it. Both sides
				// decide it by reading the same payload, which is what keeps them in step
				// without a second structure to carry the mapping.
				ToolInputProblem problem;
				ToolInputReader<JsonValue> reader{context.call.validated_input, problem};

				std::size_t position = 0;

				if (reader.has("afterTrack")) {
					request.after_track_guid =
						tool_reader_detail::resolved_identifier_at(context.resolved_targets, position);
					++position;
				}

				if (reader.has("insideFolderOf")) {
					request.inside_folder_of_guid =
						tool_reader_detail::resolved_identifier_at(context.resolved_targets, position);
				}

				return request;
			};

		codec.read_delete_track_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				daw::tools::delete_track_request request;
				request.track_guid =
					tool_reader_detail::resolved_identifier_at(context.resolved_targets, 0);

				return request;
			};

		codec.read_duplicate_track_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				daw::tools::duplicate_track_request request;
				request.track_guid =
					tool_reader_detail::resolved_identifier_at(context.resolved_targets, 0);

				return request;
			};

		codec.read_set_folder_structure_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				daw::tools::set_folder_structure_request request =
					tool_reader_detail::read_or_throw(
						context,
						daw::tools::set_folder_structure_tool_name,
						[](ToolInputReader<JsonValue>& reader) {
							return read_set_folder_structure_fields(reader);
						});

				ToolInputProblem problem;
				ToolInputReader<JsonValue> reader{context.call.validated_input, problem};

				const JsonValue* const tracks = reader.optional_array("tracks");
				const std::size_t named_track_count = tracks == nullptr ? 0 : tracks->size();

				for (std::size_t position = 0; position < named_track_count; ++position) {
					request.track_guids.push_back(
						tool_reader_detail::resolved_identifier_at(context.resolved_targets, position));
				}

				if (reader.has("intoFolderOf")) {
					request.into_folder_of_guid = tool_reader_detail::resolved_identifier_at(
						context.resolved_targets,
						named_track_count);
				}

				return request;
			};
	}

	// The six mutating marker and region tools. None of them addresses a track, so each
	// reader is its field read and the throw that stands in for the null-input guard the
	// registration does not have.
	template <typename JsonValue>
	void bind_marker_region_request_readers(
		daw::tools::marker_region_payload_codec<JsonValue>& codec)
	{
		codec.read_create_marker_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::create_marker_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_create_marker_fields(reader);
					});
			};

		codec.read_create_region_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::create_region_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_create_region_fields(reader);
					});
			};

		codec.read_update_marker_or_region_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::update_marker_or_region_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_update_marker_or_region_fields(reader);
					});
			};

		codec.read_delete_marker_or_region_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::delete_marker_or_region_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_delete_marker_or_region_fields(reader);
					});
			};

		codec.read_change_tempo_map_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::change_tempo_map_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_change_tempo_map_fields(reader);
					});
			};

		codec.read_set_time_selection_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::set_time_selection_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_set_time_selection_fields(reader);
					});
			};
	}

	template <typename JsonValue>
	void bind_marker_region_read_request_readers(
		daw::tools::marker_region_read_payload_codec<JsonValue>& codec)
	{
		codec.read_list_markers_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::list_markers_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_timeline_read_window(reader);
					});
			};

		codec.read_list_regions_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::list_regions_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_timeline_read_window(reader);
					});
			};
	}

	template <typename JsonValue>
	void bind_tempo_request_readers(daw::tools::tempo_payload_codec<JsonValue>& codec)
	{
		codec.read_list_tempo_changes_request =
			[](const daw::tool_execution_context<JsonValue>& context) {
				return tool_reader_detail::read_or_throw(
					context,
					daw::tools::list_tempo_changes_tool_name,
					[](ToolInputReader<JsonValue>& reader) {
						return read_timeline_read_window(reader);
					});
			};
	}

	// The five routing readers. `remove_bus` and `get_routing` have none: both are
	// addressed entirely by target resolution.
	//
	// These take the payload alone and their registration checks for a null one first, so
	// there is nothing to throw about here — an unreadable payload reaches the executor as
	// no payload and routing reports `missing_tool_input`.
	template <typename JsonValue>
	void bind_routing_request_readers(daw::tools::routing_payload_codec<JsonValue>& codec)
	{
		codec.read_create_send_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_create_send_fields(reader);
		};

		codec.read_remove_send_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_remove_send_fields(reader);
		};

		codec.read_set_send_state_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_set_send_state_fields(reader);
		};

		codec.read_set_parent_send_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_set_parent_send_fields(reader);
		};

		codec.read_create_bus_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_create_bus_fields(reader);
		};
	}

	template <typename JsonValue>
	void bind_render_request_reader(daw::tools::render_payload_codec<JsonValue>& codec)
	{
		codec.read_render_request = [](const JsonValue& payload) {
			ToolInputProblem problem;
			ToolInputReader<JsonValue> reader{&payload, problem};

			return read_render_fields(reader);
		};
	}

}

#endif
