// `read_tool_call` — one inbound `request:<tool_name>` envelope, as the Tool Executor
// sees it (task 24.1, requirements 4.1, 4.4, 9.1, 23.1).
//
// `ToolCallPayloadCodec::read_tool_call` (`src/entry/extension_composition.h:125`) is
// the function this file implements. The executor's `tool_call` holds five things and
// this is where four of them come from:
//
//   - the tool name and the request identifier, copied off the envelope;
//   - every track the call addresses, as selectors, in a fixed per-tool order;
//   - the four acknowledgement fields the refusal schema names;
//   - the payload itself, which the executor carries as a pointer it never dereferences.
//
// The typed argument each handler takes is the other half and lives in
// `tool_request_readers.h`. This file reads what the *framework* needs, which is
// everything that has to be known before any handler runs.
//
// ---------------------------------------------------------------------------
// Target resolution happens before the handler, so the selectors are read here
//
// Requirement 9.1 has the executor resolve every target before a handler runs, and
// `tool_executor.h` cannot read a selector out of a payload it is forbidden to look
// inside. So the selector order is this file's decision, and it is the decision the
// context-taking readers in `tool_request_readers.h` depend on: `create_send` resolves
// its source before its destination, `create_track` resolves `afterTrack` before
// `insideFolderOf`, `set_folder_structure` resolves its `tracks` array in order and
// `intoFolderOf` last.
//
// Both sides work it out by reading the same payload rather than by passing a mapping
// across, which is what keeps them in step: a reader asks "did the call carry
// `insideFolderOf`" and gets the same answer this file got.
//
// ---------------------------------------------------------------------------
// A payload this build cannot read is not a payload
//
// One rule, stated here because this is the only place that sees the whole call.
// `check_tool_input_fields` reads the named tool's fields; a missing one, one of the
// wrong type, or an enumeration value outside the closed set is recorded, and when
// anything was recorded the executor is handed a call with **no input and no
// selectors**.
//
// That is not a refusal and must not become one. A refusal names a field the producer
// can set to proceed, and `tool-result-refusal.schema.json` closes `reason` at five
// values — none of which describes a payload that does not parse into a request. What
// the ten tool families do with a call carrying no input is report a failed action with
// a reason, which is what requirement 9.5 asks of every failure and what the task
// settled for inverted ranges as well.
//
// The selectors go with the input rather than surviving it, and that is the part worth
// reading twice. Keeping them would have the executor resolve targets for a call it is
// about to fail, and a pattern that resolved to two tracks would turn an unreadable
// payload into an ambiguity refusal — reporting the wrong thing about the wrong problem.
// A payload this build cannot read addresses nothing.
//
// Inverted ranges are deliberately *not* checked here. `check_end_after_start` already
// answers that question in the tool layer, for regions, marker and region updates, time
// selections, and the three read windows, and it reports `invalid_time_range` on a
// failed action. A second answer here would classify the same call as an unreadable
// input instead, which is a worse report of a call that was read perfectly well.

#ifndef SESH_AI_TRANSPORT_TOOL_CALL_READER_H
#define SESH_AI_TRANSPORT_TOOL_CALL_READER_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/object_resolver.h>
#include <daw/tool_executor.h>
#include <transport/tool_payload_reading.h>
#include <transport/tool_request_readers.h>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// Selectors
	// ---------------------------------------------------------------------------

	// One `trackSelector`, as `daw::TrackSelector` holds it: exactly one of a GUID and a
	// name pattern, which is what the schema's `oneOf` with `additionalProperties: false`
	// on each branch says and what the variant makes unrepresentable otherwise.
	//
	// Empty for a selector that is neither, with the problem recorded. A selector
	// carrying both is read as the GUID, which is the branch `trackSelector`'s own
	// description tells the agent to prefer — and the one that survives the producer
	// renaming a track between the snapshot and the call.
	template <typename JsonValue>
	std::optional<daw::TrackSelector> read_track_selector(
		ToolInputReader<JsonValue>& parent,
		const JsonValue& selector)
	{
		if (!selector.is_object()) {
			parent.record_problem("a track selector arrived as something other than an object");

			return std::nullopt;
		}

		ToolInputReader<JsonValue> selector_reader = parent.reader_for(selector);

		if (selector_reader.has("guid")) {
			const std::string guid = selector_reader.required_identifier("guid");

			if (guid.empty()) {
				return std::nullopt;
			}

			return daw::TrackSelector{daw::TrackGuidSelector{guid}};
		}

		if (selector_reader.has("namePattern")) {
			const std::string pattern = selector_reader.required_identifier("namePattern");

			if (pattern.empty()) {
				return std::nullopt;
			}

			return daw::TrackSelector{daw::TrackNamePatternSelector{pattern}};
		}

		parent.record_problem("a track selector carries neither a guid nor a namePattern");

		return std::nullopt;
	}

	// A named selector field. Absent is recorded when the tool requires one and ignored
	// when it does not — `move_items` without `toTrack` is a move along the timeline, and
	// `list_tracks` without `tracks` is the whole project.
	template <typename JsonValue>
	void append_track_selector(
		ToolInputReader<JsonValue>& reader,
		const char* field_name,
		bool is_required,
		std::vector<daw::TrackSelector>& selectors)
	{
		if (!reader.has(field_name)) {
			if (is_required) {
				reader.record_problem(
					std::string{"\""} + field_name + "\" is required and the call did not carry it");
			}

			return;
		}

		const JsonValue* const selector = reader.optional_object(field_name);

		if (selector == nullptr) {
			return;
		}

		std::optional<daw::TrackSelector> read = read_track_selector(reader, *selector);

		if (read.has_value()) {
			selectors.push_back(std::move(*read));
		}
	}

	// A `trackSelectors` array, in the order the call named them — which is the order the
	// resolved targets arrive in and therefore the order every handler reports.
	template <typename JsonValue>
	void append_track_selector_array(
		ToolInputReader<JsonValue>& reader,
		const char* field_name,
		bool is_required,
		std::vector<daw::TrackSelector>& selectors)
	{
		const JsonValue* const array =
			is_required ? reader.required_array(field_name) : reader.optional_array(field_name);

		if (array == nullptr) {
			return;
		}

		for (const JsonValue& element : *array) {
			std::optional<daw::TrackSelector> read = read_track_selector(reader, element);

			if (!read.has_value()) {
				return;
			}

			selectors.push_back(std::move(*read));
		}
	}

	// Every track the named tool addresses, in the order this file fixes.
	//
	// The eleven single-`track` tools are grouped because they are genuinely the same
	// case: an FX or import tool addresses one track through one required selector, and
	// listing them separately would be eleven chances to spell the field differently.
	template <typename JsonValue>
	std::vector<daw::TrackSelector> read_track_selectors(
		std::string_view tool_name,
		ToolInputReader<JsonValue>& reader)
	{
		std::vector<daw::TrackSelector> selectors;

		if (tool_name == daw::tools::add_fx_tool_name
			|| tool_name == daw::tools::remove_fx_tool_name
			|| tool_name == daw::tools::set_fx_bypass_tool_name
			|| tool_name == daw::tools::set_fx_parameter_tool_name
			|| tool_name == daw::tools::get_fx_parameters_tool_name
			|| tool_name == daw::tools::list_track_fx_tool_name
			|| tool_name == daw::tools::apply_fx_destructively_tool_name
			|| tool_name == daw::tools::import_audio_file_tool_name
			|| tool_name == daw::tools::import_midi_file_tool_name
			|| tool_name == daw::tools::delete_track_tool_name
			|| tool_name == daw::tools::duplicate_track_tool_name) {
			append_track_selector(reader, "track", true, selectors);

			return selectors;
		}

		// --- track structure ---
		if (tool_name == daw::tools::create_track_tool_name) {
			// `afterTrack` first, then `insideFolderOf`. Both optional, and
			// `bind_track_structure_request_readers` maps them back by asking the payload
			// the same two questions in the same order.
			append_track_selector(reader, "afterTrack", false, selectors);
			append_track_selector(reader, "insideFolderOf", false, selectors);

			return selectors;
		}

		if (tool_name == daw::tools::set_folder_structure_tool_name) {
			// The moved tracks in order, then the destination folder. The array's length
			// is what tells the reader where the destination sits.
			append_track_selector_array(reader, "tracks", true, selectors);
			append_track_selector(reader, "intoFolderOf", false, selectors);

			return selectors;
		}

		// --- routing ---
		if (tool_name == "create_send") {
			// Source before destination, which is the order `create_send_result` reports
			// them and the order the cycle precondition reads them in.
			append_track_selector(reader, "sourceTrack", true, selectors);
			append_track_selector(reader, "destinationTrack", true, selectors);

			return selectors;
		}

		if (tool_name == "remove_send" || tool_name == "set_send_state") {
			append_track_selector(reader, "sourceTrack", true, selectors);

			return selectors;
		}

		if (tool_name == "remove_bus") {
			append_track_selector(reader, "busTrack", true, selectors);

			return selectors;
		}

		if (tool_name == "create_bus") {
			append_track_selector_array(reader, "sourceTracks", true, selectors);

			return selectors;
		}

		if (tool_name == "set_parent_send") {
			append_track_selector_array(reader, "tracks", true, selectors);

			return selectors;
		}

		if (tool_name == "get_routing") {
			append_track_selector_array(reader, "tracks", false, selectors);

			return selectors;
		}

		// --- items ---
		if (tool_name == daw::tools::move_items_tool_name) {
			append_track_selector(reader, "toTrack", false, selectors);

			return selectors;
		}

		if (tool_name == daw::tools::move_all_items_tool_name) {
			append_track_selector_array(reader, "tracks", false, selectors);

			return selectors;
		}

		// --- track state ---
		if (tool_name == daw::tools::list_tracks_tool_name) {
			append_track_selector_array(reader, "tracks", false, selectors);

			return selectors;
		}

		if (tool_name == daw::tools::set_track_state_tool_name) {
			// One selector per entry of `changes`, in the order the entries were
			// supplied. The handler's own request holds GUIDs and never sees these — see
			// `tool_request_readers.h` on why — but resolution still runs, which is what
			// refuses an entry naming a pattern that matches no track or several.
			const JsonValue* const changes = reader.optional_array("changes");

			if (changes == nullptr) {
				return selectors;
			}

			for (const JsonValue& element : *changes) {
				if (!element.is_object()) {
					return selectors;
				}

				ToolInputReader<JsonValue> change_reader = reader.reader_for(element);

				append_track_selector(change_reader, "track", true, selectors);
			}

			return selectors;
		}

		// --- render ---
		if (tool_name == daw::tools::render_tool_name) {
			append_track_selector_array(reader, "tracks", false, selectors);

			return selectors;
		}

		// Everything else addresses no track: the marker, region, and tempo tools, the
		// three item tools that work from item GUIDs, `list_installed_fx`,
		// `get_project_summary`, and the two rewinding tools.
		return selectors;
	}

	// ---------------------------------------------------------------------------
	// The call
	// ---------------------------------------------------------------------------

	// What reading one envelope produced, and what was wrong with it if anything.
	//
	// The description is not needed to dispatch — the executor already reports the
	// failure through each family's own path — but it is the only account of *why* a
	// payload was rejected, and the composition root is where a line naming it belongs.
	// `read_tool_call` below is the thin adapter `ToolCallPayloadCodec` takes.
	template <typename JsonValue>
	struct ToolCallReadOutcome {
		daw::tool_call<JsonValue> call;

		// Empty when the payload read cleanly.
		std::string payload_problem;

		bool payload_was_readable() const { return payload_problem.empty(); }
	};

	// Reads one inbound tool call envelope.
	//
	// `validated_input` points into the envelope, which the caller owns: the Message
	// Dispatcher holds the envelope for the whole dispatch, and `tool_call` has nowhere
	// to own a copy. A `tool_call` therefore does not outlive the envelope it was read
	// from, which is how `ExecutorToolCallRouter` already uses it.
	template <typename InboundEnvelopeType, typename JsonValue>
	ToolCallReadOutcome<JsonValue> read_tool_call_reporting(
		std::string_view tool_name,
		const InboundEnvelopeType& inbound_envelope)
	{
		ToolCallReadOutcome<JsonValue> outcome;

		outcome.call.tool_name = std::string{tool_name};
		outcome.call.request_id = inbound_envelope.request_id;

		const JsonValue& payload = inbound_envelope.payload;

		ToolInputProblem problem;
		ToolInputReader<JsonValue> reader{&payload, problem};

		if (!reader.is_readable_object()) {
			outcome.payload_problem = "this call's input is not an object, so it carries no fields";

			return outcome;
		}

		const AcknowledgementFlags flags = read_acknowledgement_flags(reader);

		check_tool_input_fields(tool_name, reader);

		std::vector<daw::TrackSelector> selectors = read_track_selectors(tool_name, reader);

		if (problem.anything_recorded()) {
			// No input and no selectors. See the file comment: an unreadable payload
			// addresses nothing, and the executor reports it through the path each tool
			// family already has for a call that carried no input.
			outcome.payload_problem = problem.description();

			return outcome;
		}

		outcome.call.validated_input = &payload;
		outcome.call.track_selectors = std::move(selectors);
		outcome.call.confirmed_track_selection = flags.confirmed_track_selection;
		outcome.call.confirmed_signal_cycle = flags.confirmed_signal_cycle;
		outcome.call.confirmed_overwrite = flags.confirmed_overwrite;
		outcome.call.confirmed_foreign_undo = flags.confirmed_foreign_undo;

		return outcome;
	}

	// The signature `ToolCallPayloadCodec::read_tool_call` holds.
	template <typename InboundEnvelopeType, typename JsonValue>
	daw::tool_call<JsonValue> read_tool_call(
		std::string_view tool_name,
		const InboundEnvelopeType& inbound_envelope)
	{
		return read_tool_call_reporting<InboundEnvelopeType, JsonValue>(
			tool_name,
			inbound_envelope).call;
	}

}

#endif
