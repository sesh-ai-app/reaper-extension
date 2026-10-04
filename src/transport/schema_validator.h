// The Schema Validator: which bundled schema governs which payload, and the seam
// that checks one against the other.
//
// `schemas/` holds 65 files. Which of them applies to a given payload is not a
// detail of the validation library — it is the protocol contract, and getting it
// wrong is the failure mode requirement 4.4 exists to prevent. So the mapping is
// the substance of this header and the validating is a seam behind it.
//
// Four rules decide the mapping, and they come from
// `.kiro/steering/schema-bundles.md` by way of requirement 4:
//
// 1. **Outbound payloads are validated** (requirement 4.1). A malformed project
//    context snapshot should fail here, locally, with a diagnosable error, rather
//    than arriving at the server as a validation rejection in the middle of a
//    producer's turn.
// 2. **Inbound payloads are validated when their envelope type has a bundled
//    schema** (requirement 4.2). Of the 65 files, 17 are envelope and message
//    payload schemas; nine of those are bound to an inbound envelope type below. An
//    inbound type with no bundled schema is dispatched on its type and read
//    defensively — see the Stream Presenter and the Transport Handler for what
//    reading defensively looks like.
// 3. **An inbound payload that fails validation is refused** (requirement 4.3). The
//    codec answers with an error and the envelope never reaches a handler. That
//    happens in envelope_codec.h; what happens here is only deciding that a schema
//    applies.
// 4. **Tool inputs are never validated** (requirement 4.4). The 42 tool input
//    schemas are deliberately absent from the bundle. The MCP Tool Server has
//    already checked tool input against the authoritative copy, so the only thing a
//    vendored re-check could do that the server did not is reject something valid
//    inside a producer's session. There is deliberately no `request:<tool_name>`
//    entry in any table here, and adding one "for safety" is the mistake the
//    steering names. The 42 tool *output* schemas are bundled and are validated,
//    because the extension produces those payloads and nobody checked them earlier.
//
// The remaining six files are the CEF bridge view models, which are not envelope
// types at all and so bind to nothing here. They are listed, below the tool output
// bindings, only so that this file remains the complete account of the bundle — see
// `bridge_message_schema_paths`.
//
// Three things about the shape of this file.
//
// **The mapping is data.** Every binding is a constexpr table, so the suite can
// assert over the whole of it — every schema path spelled here exists in the bundle,
// no envelope type is bound twice, no constrained tool name collides with a bound
// `request:*` type — without a validation library present. A path that names a file
// the bundler does not produce is then a test failure rather than a validator that
// fails closed on a schema it could not find.
//
// **The 42 tool output paths are listed rather than derived.** `set_track_state`
// becomes `mcp-tools/outputs/set-track-state.schema.json` under one mechanical
// substitution, so a loop would be shorter. But the list is the bundle's own file
// list, and spelling it out means a tool renamed on the server is caught by the
// test that checks every path against the manifest, instead of producing a
// plausible path to a file that is not there.
//
// **Validation is behind a templated seam.** PayloadSchemaValidator is what
// envelope_codec.h depends on, so the codec's decisions are testable with a
// substituted validator and no JSON dependency. BundledSchemaValidator is the real
// one; it keeps nlohmann/json and json-schema-validator inside
// schema_validator.cpp, which is the arrangement transport_handler.h uses for the
// REAPER SDK and for the same structural reason.

#ifndef SESH_AI_TRANSPORT_SCHEMA_VALIDATOR_H
#define SESH_AI_TRANSPORT_SCHEMA_VALIDATOR_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sesh_ai::transport {

	// How many files the bundle holds, and how they divide. Spelled here so the
	// suite can check the tables below against the bundle's own totals rather than
	// against a number in a comment.
	inline constexpr std::size_t bundled_schema_file_count = 65;
	inline constexpr std::size_t bundled_envelope_and_message_schema_count = 17;
	inline constexpr std::size_t bundled_tool_output_schema_count = 42;
	inline constexpr std::size_t bundled_bridge_message_schema_count = 6;

	// The envelope wrapper itself. Validated in both directions: it is what enforces
	// the `namespace:action` type pattern, that `payload` is an object, and that no
	// fourth property rode along — so an envelope that fails it is refused before
	// anything reads a field out of it.
	inline constexpr std::string_view envelope_schema_path{"envelope.schema.json"};

	// The project context snapshot. Outbound only, under two envelope types: the
	// unprompted `state:project_context` and the answer to a `request:project_context`.
	inline constexpr std::string_view project_context_schema_path{"project-context.schema.json"};

	// A tool result that is a precondition refusal rather than an outcome. Selected
	// by the `refused` discriminator on the payload rather than by envelope type,
	// because a refusal travels in the same `response:<tool_name>` envelope a result
	// would have — see tool_result_schema_path below.
	inline constexpr std::string_view tool_result_refusal_schema_path{"messages/tool-result-refusal.schema.json"};

	// Two contracts the 42 tool output schemas are written against rather than
	// schemas a payload is validated against directly.
	//
	// The undo report fields and the per-action outcome array are declared once and
	// referenced from each tool output schema; the bundler's resolver hoists those
	// definitions into every referrer, so a mutating tool's own output schema already
	// carries `undoPositionBefore`, and the two action-array tools already carry
	// `actions` with the extra properties they each require. Validating
	// `set_track_state`'s result against the partial contract would fail on
	// `aliasesLearned`, which the contract's `additionalProperties: false` does not
	// allow — so the tool's own schema is the one that applies, and these two are
	// here to be named in that explanation rather than to be selected.
	inline constexpr std::string_view tool_result_partial_contract_schema_path{"messages/tool-result-partial.schema.json"};
	inline constexpr std::string_view tool_result_undo_contract_schema_path{"messages/tool-result-undo.schema.json"};

	// An envelope type and the bundled schema that governs its payload.
	struct EnvelopeSchemaBinding {
		std::string_view envelope_type;
		std::string_view schema_path;
	};

	// The inbound bindings: every envelope type the extension receives that has a
	// bundled schema (requirement 4.2).
	//
	// Nine entries, and the list is closed by what the bundle contains rather than by
	// what the server can send. The extension also receives `request:project_context`
	// (an empty payload, no bundled schema), the seven `error:*` types the server
	// emits, and `request:<tool_name>` — none of which appears here, the first two
	// because the bundle has no schema for them and the third because of rule 4.
	inline constexpr std::array<EnvelopeSchemaBinding, 9> inbound_envelope_schema_bindings{{
		{"request:transport", "messages/transport-command.schema.json"},
		{"confirm:request", "messages/confirmation-request.schema.json"},
		{"confirm:resolved", "messages/confirm-resolved.schema.json"},
		{"script:download", "messages/script-download.schema.json"},
		{"stream:agent_response_start", "messages/agent-response-start.schema.json"},
		{"stream:agent_response_delta", "messages/agent-response-delta.schema.json"},
		{"stream:agent_response_stop", "messages/agent-response-stop.schema.json"},
		{"stream:rtmps_url", "messages/stream-rtmps-url.schema.json"},
		{"stream:error", "messages/stream-error.schema.json"}
	}};

	// The outbound bindings: every envelope type the extension sends whose payload has
	// a bundled schema (requirement 4.1), excluding tool results, which are chosen
	// per tool by tool_result_schema_path.
	//
	// Two pairs of entries share a schema, and both repetitions are deliberate rather
	// than something to factor out. The snapshot is the same payload whether it was
	// asked for or not, and a confirmation decision is the same shape either way —
	// confirm-decision.schema.json says so about itself, since the envelope type
	// already carries the verdict. Keeping one row per envelope type means the lookup
	// is one loop with no special case, and "no envelope type is bound twice" stays a
	// property the suite can check over the table.
	//
	// `stream:request_audio` is sent and is deliberately absent: the bundle has no
	// schema for its payload, so there is nothing to validate it against.
	inline constexpr std::string_view confirm_decision_schema_path{"messages/confirm-decision.schema.json"};

	inline constexpr std::array<EnvelopeSchemaBinding, 6> outbound_envelope_schema_bindings{{
		{"state:prompt", "messages/state-prompt.schema.json"},
		{"state:project_context", project_context_schema_path},
		{"response:project_context", project_context_schema_path},
		{"response:transport", "messages/response-transport.schema.json"},
		{"confirm:approve", confirm_decision_schema_path},
		{"confirm:reject", confirm_decision_schema_path}
	}};

	// The six view models that cross the CEF bridge (design "The CEF bridge
	// contract").
	//
	// Not envelope types, so they are not bindings and nothing here selects one by
	// type — `ui/ui_host.h` owns the name-to-schema mapping, because it owns the
	// names. They are listed here for the one thing this file is responsible for:
	// being the full account of what the bundle holds, so that
	// `all_bundled_schema_paths` loads every file and the suite's manifest check stays
	// an equality rather than a subset. A bundled schema no table named would be a
	// payload the extension ships and never validates.
	//
	// The four protocol payloads the bridge carries verbatim are absent on purpose:
	// each one is already in the inbound or outbound bindings above under its own
	// envelope type, and that is the copy the wire validates against. One schema, one
	// row, whichever side of the bridge the payload happens to be on.
	inline constexpr std::array<std::string_view, bundled_bridge_message_schema_count>
		bridge_message_schema_paths{
			"bridge-messages/agent-response.schema.json",
			"bridge-messages/confirmation-pending.schema.json",
			"bridge-messages/confirmation-resolved.schema.json",
			"bridge-messages/connection-state.schema.json",
			"bridge-messages/stream-error.schema.json",
			"bridge-messages/stream-ingest.schema.json"
		};
	// A constrained tool and the bundled schema for the payload the extension returns
	// for it.
	struct ToolOutputSchemaBinding {
		std::string_view tool_name;
		std::string_view schema_path;
	};

	// All 42. The extension produces every one of these payloads, which is exactly
	// why they are bundled and validated while the 42 input schemas are neither.
	inline constexpr std::array<ToolOutputSchemaBinding, bundled_tool_output_schema_count>
		tool_output_schema_bindings{{
			{"add_fx", "mcp-tools/outputs/add-fx.schema.json"},
			{"apply_fx_destructively", "mcp-tools/outputs/apply-fx-destructively.schema.json"},
			{"change_tempo_map", "mcp-tools/outputs/change-tempo-map.schema.json"},
			{"create_bus", "mcp-tools/outputs/create-bus.schema.json"},
			{"create_marker", "mcp-tools/outputs/create-marker.schema.json"},
			{"create_region", "mcp-tools/outputs/create-region.schema.json"},
			{"create_send", "mcp-tools/outputs/create-send.schema.json"},
			{"create_track", "mcp-tools/outputs/create-track.schema.json"},
			{"delete_items", "mcp-tools/outputs/delete-items.schema.json"},
			{"delete_marker_or_region", "mcp-tools/outputs/delete-marker-or-region.schema.json"},
			{"delete_track", "mcp-tools/outputs/delete-track.schema.json"},
			{"duplicate_track", "mcp-tools/outputs/duplicate-track.schema.json"},
			{"get_fx_parameters", "mcp-tools/outputs/get-fx-parameters.schema.json"},
			{"get_project_summary", "mcp-tools/outputs/get-project-summary.schema.json"},
			{"get_routing", "mcp-tools/outputs/get-routing.schema.json"},
			{"import_audio_file", "mcp-tools/outputs/import-audio-file.schema.json"},
			{"import_midi_file", "mcp-tools/outputs/import-midi-file.schema.json"},
			{"list_installed_fx", "mcp-tools/outputs/list-installed-fx.schema.json"},
			{"list_markers", "mcp-tools/outputs/list-markers.schema.json"},
			{"list_regions", "mcp-tools/outputs/list-regions.schema.json"},
			{"list_selected_items", "mcp-tools/outputs/list-selected-items.schema.json"},
			{"list_tempo_changes", "mcp-tools/outputs/list-tempo-changes.schema.json"},
			{"list_track_fx", "mcp-tools/outputs/list-track-fx.schema.json"},
			{"list_tracks", "mcp-tools/outputs/list-tracks.schema.json"},
			{"move_all_items", "mcp-tools/outputs/move-all-items.schema.json"},
			{"move_items", "mcp-tools/outputs/move-items.schema.json"},
			{"remove_bus", "mcp-tools/outputs/remove-bus.schema.json"},
			{"remove_fx", "mcp-tools/outputs/remove-fx.schema.json"},
			{"remove_send", "mcp-tools/outputs/remove-send.schema.json"},
			{"render", "mcp-tools/outputs/render.schema.json"},
			{"revert_agent_changes", "mcp-tools/outputs/revert-agent-changes.schema.json"},
			{"set_folder_structure", "mcp-tools/outputs/set-folder-structure.schema.json"},
			{"set_fx_bypass", "mcp-tools/outputs/set-fx-bypass.schema.json"},
			{"set_fx_parameter", "mcp-tools/outputs/set-fx-parameter.schema.json"},
			{"set_item_properties", "mcp-tools/outputs/set-item-properties.schema.json"},
			{"set_parent_send", "mcp-tools/outputs/set-parent-send.schema.json"},
			{"set_send_state", "mcp-tools/outputs/set-send-state.schema.json"},
			{"set_time_selection", "mcp-tools/outputs/set-time-selection.schema.json"},
			{"set_track_state", "mcp-tools/outputs/set-track-state.schema.json"},
			{"split_items", "mcp-tools/outputs/split-items.schema.json"},
			{"undo_last_action", "mcp-tools/outputs/undo-last-action.schema.json"},
			{"update_marker_or_region", "mcp-tools/outputs/update-marker-or-region.schema.json"}
		}};

	// The two halves of a tool envelope type. A tool call arrives as
	// `request:<tool_name>` and is answered with `response:<tool_name>`.
	inline constexpr std::string_view tool_request_envelope_type_prefix{"request:"};
	inline constexpr std::string_view tool_response_envelope_type_prefix{"response:"};

	// The payload schema for an inbound envelope type, or empty when the bundle has
	// none for it.
	//
	// Empty is not an error and not a reason to refuse. It means the envelope is
	// dispatched on its type and its payload read defensively, which is requirement
	// 4.2 read the way round it is written: validate the ones that have a schema.
	constexpr std::optional<std::string_view> inbound_payload_schema_path(std::string_view envelope_type)
	{
		for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
			if (binding.envelope_type == envelope_type) {
				return binding.schema_path;
			}
		}

		return std::nullopt;
	}

	// The payload schema for an outbound envelope type other than a tool result, or
	// empty when the bundle has none.
	constexpr std::optional<std::string_view> outbound_payload_schema_path(std::string_view envelope_type)
	{
		for (const EnvelopeSchemaBinding& binding : outbound_envelope_schema_bindings) {
			if (binding.envelope_type == envelope_type) {
				return binding.schema_path;
			}
		}

		return std::nullopt;
	}

	// True for one of the 42 constrained tool names.
	constexpr bool is_constrained_tool_name(std::string_view tool_name)
	{
		for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
			if (binding.tool_name == tool_name) {
				return true;
			}
		}

		return false;
	}

	// The output schema for a constrained tool, or empty for a name that is not one of
	// the 42.
	//
	// Empty for an unknown tool rather than a derived path: a tool the server knows
	// about and this build does not is requirement 23.6's case, answered with an error
	// result naming the tool, and a guessed path would turn it into a validator
	// failing to find a file.
	constexpr std::optional<std::string_view> tool_output_schema_path(std::string_view tool_name)
	{
		for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
			if (binding.tool_name == tool_name) {
				return binding.schema_path;
			}
		}

		return std::nullopt;
	}

	// The tool name inside a `request:<tool_name>` envelope type, or empty when the
	// type is not a call to one of the 42.
	//
	// Provided so that rule 4 can be stated as code: this is the one lookup that
	// identifies a tool input, and there is no function anywhere that maps its result
	// to a schema. `request:transport` and `request:project_context` are not tool
	// names, so they fall out here as well as being bound above.
	constexpr std::optional<std::string_view> tool_name_from_request_envelope_type(std::string_view envelope_type)
	{
		if (envelope_type.size() <= tool_request_envelope_type_prefix.size()) {
			return std::nullopt;
		}

		if (envelope_type.substr(0, tool_request_envelope_type_prefix.size()) != tool_request_envelope_type_prefix) {
			return std::nullopt;
		}

		const std::string_view tool_name = envelope_type.substr(tool_request_envelope_type_prefix.size());

		return is_constrained_tool_name(tool_name) ? std::optional<std::string_view>{tool_name} : std::nullopt;
	}

	// The tool name inside a `response:<tool_name>` envelope type, or empty when the
	// type is not a result for one of the 42.
	constexpr std::optional<std::string_view> tool_name_from_response_envelope_type(std::string_view envelope_type)
	{
		if (envelope_type.size() <= tool_response_envelope_type_prefix.size()) {
			return std::nullopt;
		}

		if (envelope_type.substr(0, tool_response_envelope_type_prefix.size()) != tool_response_envelope_type_prefix) {
			return std::nullopt;
		}

		const std::string_view tool_name = envelope_type.substr(tool_response_envelope_type_prefix.size());

		return is_constrained_tool_name(tool_name) ? std::optional<std::string_view>{tool_name} : std::nullopt;
	}

	// The schema for a tool result payload: the refusal contract when the payload
	// carries the refusal discriminator, otherwise the tool's own output schema.
	//
	// Two candidates rather than three. `refused` is declared by
	// tool-result-refusal.schema.json as the discriminator that marks a payload a
	// refusal rather than a result, and no tool output schema has a `refused`
	// property — so the test is sound. There is deliberately no branch on `actions`,
	// which looks like the partial-outcome discriminator and is not one:
	// `set_track_state` and `set_item_properties` both return an `actions` array as
	// their ordinary result, and routing those to the partial contract would refuse
	// two tools' valid output on the extra properties each requires.
	constexpr std::optional<std::string_view> tool_result_schema_path(
		std::string_view tool_name,
		bool payload_is_refusal
	)
	{
		if (payload_is_refusal) {
			return tool_result_refusal_schema_path;
		}

		return tool_output_schema_path(tool_name);
	}

	// The property name that marks a tool result payload a refusal.
	inline constexpr std::string_view tool_result_refusal_discriminator_property{"refused"};

	// Every distinct schema path the tables above name, sorted, with the two shared
	// bindings collapsed.
	//
	// This is what the real validator loads and what the suite checks against
	// MANIFEST.json. One list serves both, so a path that names a file the bundler
	// does not produce is a test failure rather than a validator that fails closed at
	// runtime on a schema it could not find.
	//
	// Should come to bundled_schema_file_count entries. Two pairs of envelope types
	// share a schema and the two tool result contracts are named directly, so the
	// arithmetic is not obvious from the table sizes — which is why the suite checks
	// the number rather than this comment asserting it.
	inline std::vector<std::string> all_bundled_schema_paths()
	{
		std::vector<std::string> schema_paths;

		schema_paths.reserve(bundled_schema_file_count);

		schema_paths.emplace_back(envelope_schema_path);
		schema_paths.emplace_back(project_context_schema_path);
		schema_paths.emplace_back(tool_result_refusal_schema_path);
		schema_paths.emplace_back(tool_result_partial_contract_schema_path);
		schema_paths.emplace_back(tool_result_undo_contract_schema_path);

		for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
			schema_paths.emplace_back(binding.schema_path);
		}

		for (const EnvelopeSchemaBinding& binding : outbound_envelope_schema_bindings) {
			schema_paths.emplace_back(binding.schema_path);
		}

		for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
			schema_paths.emplace_back(binding.schema_path);
		}

		for (const std::string_view schema_path : bridge_message_schema_paths) {
			schema_paths.emplace_back(schema_path);
		}

		std::sort(schema_paths.begin(), schema_paths.end());
		schema_paths.erase(std::unique(schema_paths.begin(), schema_paths.end()), schema_paths.end());

		return schema_paths;
	}

	// What validating one payload against one schema produced.
	//
	// `valid` false with an empty error list is possible and means the validator had
	// nothing to validate against — an unknown schema path. It fails closed, for the
	// reason the server's validator does: an unregistered boundary should refuse
	// rather than let unvalidated data through.
	struct SchemaValidationOutcome {
		bool valid = false;

		// The schema the payload was checked against. Empty when the path was not one
		// the validator holds.
		std::string schema_path;

		// One rendered failure per entry, in the order the validator reported them.
		// Rendered rather than structured because the only consumers are a log line
		// and the `validationErrors` string in the error payload, and
		// stream-error.schema.json declares that field a string.
		std::vector<std::string> errors;
	};

	// What the extension needs in order to validate a payload: check one payload
	// against one bundled schema, by path.
	//
	// Templated on the payload type rather than fixed to nlohmann::json, which is the
	// same choice TransportHandler makes: the Envelope Codec's decisions — which
	// schema applies, what happens when it fails — are the part worth testing, and
	// they are testable against a substituted validator on a machine with no JSON
	// dependency installed.
	template <typename PayloadType>
	class PayloadSchemaValidator {
	public:
		virtual ~PayloadSchemaValidator() = default;

		// Total: every input produces an outcome, never an exception. The codec runs on
		// the network thread inside a Socket.IO callback, and a validation library
		// throwing across that boundary is a dropped connection rather than a refused
		// envelope.
		virtual SchemaValidationOutcome validate_against(
			std::string_view schema_path,
			const PayloadType& payload
		) = 0;
	};

	// Renders validation failures as one line, joined with "; ", truncated to
	// maximum_length characters.
	//
	// The same rendering the server does, and the truncation is not cosmetic:
	// stream-error.schema.json caps `validationErrors` at 4096 characters, so an
	// untruncated render of a snapshot with a hundred failures would produce an error
	// payload that fails its own schema. An error that cannot be sent because it is
	// too long to describe is the worst available outcome, so the length is enforced
	// here rather than hoped for.
	//
	// Defined here rather than in schema_validator.cpp because that translation unit
	// is only compiled where the JSON dependency is installed, and this function does
	// not need it. The suite drives it directly.
	inline std::string render_validation_errors(
		const std::vector<std::string>& validation_errors,
		std::size_t maximum_length
	)
	{
		static constexpr std::string_view separator{"; "};

		std::string rendered;

		for (const std::string& validation_error : validation_errors) {
			if (!rendered.empty()) {
				rendered.append(separator);
			}

			rendered.append(validation_error);

			// Stopping as soon as the budget is met keeps the work proportional to what
			// is going to be sent rather than to how badly the payload failed.
			if (rendered.size() >= maximum_length) {
				break;
			}
		}

		if (rendered.size() > maximum_length) {
			rendered.resize(maximum_length);
		}

		return rendered;
	}

	// Shortens a string to fit a schema's maxLength.
	//
	// Truncation rather than refusal, for every field except the failure list: a
	// project name long enough to overflow the error payload's `envelopeType` is not a
	// reason to withhold the error.
	inline std::string truncate_to_length(std::string_view value, std::size_t maximum_length)
	{
		return std::string{value.substr(0, value.size() < maximum_length ? value.size() : maximum_length)};
	}

	// The character budget stream-error.schema.json allows for a rendered failure
	// list, and for the two other string fields the error payload carries.
	inline constexpr std::size_t maximum_rendered_validation_errors_length = 4096;
	inline constexpr std::size_t maximum_error_message_length = 1024;
	inline constexpr std::size_t maximum_error_envelope_type_length = 64;

	// What loading the bundle produced.
	struct SchemaBundleLoadOutcome {
		// True when every file named in the tables above compiled.
		bool loaded = false;

		std::size_t compiled_schema_count = 0;

		// One entry per file that could not be read or compiled, naming it. A build
		// with a verified bundle should never produce any: the digests were checked
		// at build time, so a failure here means the directory the extension was
		// pointed at is not the one that was verified.
		std::vector<std::string> errors;
	};

	// The real validator: the 65 vendored files, compiled once, validated against by
	// path.
	//
	// Loads eagerly. Every file in the bundle stands alone — the bundler resolves
	// cross-document `$ref`s into local `#/definitions/...` fragments, so there are
	// 309 references in the bundle and not one of them points outside its own file.
	// That is what makes a per-file loader correct here and a multi-document resolver
	// unnecessary, and it is why loading is one pass rather than the two the server's
	// validator needs against the unresolved source schemas.
	//
	// Eager rather than on first use because requirement 4.1 is about where a problem
	// surfaces: a bundle the extension cannot read should say so at startup, not
	// halfway through a producer's first turn.
	//
	// No digest verification. `cmake/VerifySchemaBundle.cmake` hashes all 65 files on
	// every build and fails the build on a mismatch, and requirement 4.6 is explicit
	// that this is a build step and never a runtime condition. Re-hashing here would
	// add a startup cost for a check that has already passed, and would make a
	// producer's session the place a corrupt install is discovered.
	//
	// Pimpl, so nlohmann/json and json-schema-validator stay inside
	// schema_validator.cpp. The test target links them only when they are installed,
	// which is what keeps this header includable by the suite.
	class BundledSchemaValidator {
	public:
		// The directory holding the bundle — `schemas/` beside the extension binary in
		// a real install.
		explicit BundledSchemaValidator(std::string schema_directory_path);

		~BundledSchemaValidator();

		// Holds compiled schemas the codec borrows by reference; copying would
		// duplicate the compilation and moving would invalidate the borrow.
		BundledSchemaValidator(const BundledSchemaValidator&) = delete;
		BundledSchemaValidator& operator=(const BundledSchemaValidator&) = delete;
		BundledSchemaValidator(BundledSchemaValidator&&) = delete;
		BundledSchemaValidator& operator=(BundledSchemaValidator&&) = delete;

		// Reads and compiles every schema named in the tables above. Safe to call
		// twice; the second call replaces what the first loaded.
		SchemaBundleLoadOutcome load();

		bool has_schema(std::string_view schema_path) const;

		std::size_t compiled_schema_count() const;

		const std::string& schema_directory_path() const { return schema_directory_path_; }

		// Declared here and defined in schema_validator.cpp, explicitly instantiated
		// for nlohmann::json. A member template rather than a plain member so the
		// header does not have to name the JSON type.
		template <typename PayloadType>
		SchemaValidationOutcome validate_against(std::string_view schema_path, const PayloadType& payload);

	private:
		struct CompiledBundle;

		std::string schema_directory_path_;
		std::unique_ptr<CompiledBundle> compiled_bundle_;
	};

	// Adapts BundledSchemaValidator to the seam the codec depends on.
	//
	// A separate type rather than BundledSchemaValidator implementing the interface
	// directly, because the interface is templated on the payload type and a virtual
	// override would force this header to name nlohmann::json. Instantiated for
	// nlohmann::json in envelope_codec.cpp, which is where the production wiring is.
	template <typename PayloadType>
	class BundledPayloadSchemaValidator final : public PayloadSchemaValidator<PayloadType> {
	public:
		explicit BundledPayloadSchemaValidator(BundledSchemaValidator& bundled_validator)
			: bundled_validator_{bundled_validator}
		{
		}

		SchemaValidationOutcome validate_against(
			std::string_view schema_path,
			const PayloadType& payload
		) override
		{
			return bundled_validator_.template validate_against<PayloadType>(schema_path, payload);
		}

	private:
		BundledSchemaValidator& bundled_validator_;
	};

}

#endif
