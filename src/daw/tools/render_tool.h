// The `render` tool handler (task 10.9, requirements 9.1, 12.1).
//
// One tool, and almost nothing in this file decides anything. `render_coordinator.h`
// already composes REAPER's render settings, resolves the output paths, detects the
// collisions, and queues the jobs; `tool_executor.h` already maps a `render_refusal`
// to a `tool_refusal` and already owns the three result shapes. What is left is the
// mapping between them, and the registration call — which is the part worth reading,
// because the registration call is where requirement 12.6 is either kept or lost.
//
// ---------------------------------------------------------------------------
// Registered through the path that opens no undo block
//
// `render`'s undo effect is `none` (requirement 12.6): no undo block is recorded, no
// undo position marker is captured, and a turn whose only action is a render does not
// offer "revert all". `register_read_tool` is the registration call that opens
// neither, so that is the one used here — even though the tool is classified as
// mutating, and even though `render_coordinator` is the only component in this
// codebase that touches REAPER's render queue.
//
// The tool schema's classification and the undo effect are separate axes, and this is
// the one tool where they disagree. `outputs/render.schema.json` is the other half of
// the enforcement: it is the one mutating-classified tool whose output schema carries
// no undo report, with `additionalProperties: false`, so a result that somehow
// acquired one would fail the extension's own outbound validation (requirement 4.1)
// rather than quietly offering the producer a revert that reverts nothing they can
// see.
//
// `register_mutating_tool` would be wrong twice over. It opens a block — which is the
// requirement violated directly — and its body type has no refusal alternative, so
// the collision refusal would have to be re-expressed as a failed action, which names
// no `confirmedOverwrite` for the agent to set on the retry.
//
// ---------------------------------------------------------------------------
// The collision refusal comes out of the component as one step
//
// `tool_executor.h`'s header explains why a refusing body is permitted here rather
// than split into a precondition plus a mutating body, as every other mutating tool
// is: `render_coordinator::queue_render` composes the settings, asks REAPER which
// files it would write, and checks those paths against each other and against disk in
// one indivisible pass, because the paths only exist once the settings are written.
// Splitting that into a check and an apply would mean writing the settings twice and
// resolving the paths twice — a reimplementation of the component, and one where the
// second resolution could disagree with the first.
//
// Nothing is queued when it refuses. The coordinator's order of operations is what
// guarantees that, and this handler adds no queue call of its own.
//
// Both forms of the refusal travel unchanged through
// `refusal_from_render_refusal`, including the difference between them:
//
//   - two of this render's own paths being equal names no acknowledgement field,
//     because approving it would mean approving that one of the files is silently not
//     written;
//   - a path that already exists names `confirmedOverwrite`, so the agent can ask the
//     producer about those specific files and retry.
//
// Rewriting either would have the agent offering the producer the wrong
// acknowledgement, which is why this file contains no refusal-building code at all.
//
// ---------------------------------------------------------------------------
// Refusal, failure, and the difference the producer feels
//
// `render_outcome` already draws the line: a refusal is a precondition the producer
// can resolve and names the field to set on the retry, and a failure is a call that
// does not make sense — a region GUID resolving to nothing, an empty time selection,
// a per-track render with no selected tracks. The three statuses map one to one onto
// the three shapes the framework accepts, so this handler chooses nothing:
// `queued` is a success, `refused` is a refusal, `failed` is a failed action carrying
// the coordinator's own reason.
//
// The third of those validates against `messages/tool-result-partial.schema.json` rather
// than against `outputs/render.schema.json`, and that matters here given what the section
// above says about this schema's `additionalProperties: false`. The same closure cuts the
// other way on a failure: `render.schema.json` requires eight fields — `queuedJobCount`,
// `outputPaths` and `outputFormat` among them — and a call that did not make sense has
// none of them to report. `tool_executor.h`'s `result_schema_path_for` routes on the shape
// rather than on the tool, so a failed action carrying the coordinator's reason is checked
// against the contract that describes an action array and passes.
//
// ---------------------------------------------------------------------------
// Where the JSON is, and is not
//
// Same split as `routing_tools.h`: the framework never reads a field of the payload
// (requirement 4.4 — the MCP Tool Server already validated the input against the
// authoritative schema), and `render_payload_codec` is a pair of callables that
// extract the request from the already-validated input and serialise the result into
// the payload type. Extraction, not validation. The real build binds them to
// `nlohmann::json`; the suite binds them to plain structs, which is what lets this
// file be driven without a JSON library present.
//
// Header-only, following `render_coordinator.h` and `tool_executor.h`: the Catch2
// target compiles what it finds under `tests/` and does not compile `src/`, so logic
// the suite exercises has to be reachable through the header. There is no REAPER
// adapter here and there should not be one — `RenderHost` is the Render
// Coordinator's own seam and is implemented once in `reaper_render_host.cpp`. A
// second adapter at this level would be a second copy of the render settings calls.

#ifndef SESH_AI_DAW_TOOLS_RENDER_TOOL_H
#define SESH_AI_DAW_TOOLS_RENDER_TOOL_H

#include <functional>
#include <string_view>
#include <utility>

#include <daw/render_coordinator.h>
#include <daw/tool_executor.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// The tool name, as `schema_validator.h`'s binding table spells it. Registration
	// rejects a name that is not one of the 42, so a typo here is a rejected
	// registration the extension reports at startup rather than an unknown-tool error
	// mid-session.
	inline constexpr std::string_view render_tool_name{"render"};

	// `code` values this handler produces on a failed action. Neither is a refusal
	// reason: a call whose request could not be read and a call the coordinator
	// rejected as not making sense both name nothing the producer could acknowledge
	// their way past.
	inline constexpr std::string_view missing_render_input_code{"missing_tool_input"};
	inline constexpr std::string_view render_call_failed_code{"render_call_failed"};

	// ---------------------------------------------------------------------------
	// The payload seam
	// ---------------------------------------------------------------------------

	// How the request is read out of an already-validated input, and how the result is
	// written into the payload type.
	//
	// Both members may be empty, and an empty one is a failed action carrying a reason
	// rather than a default-constructed request. A `render` call whose source nobody
	// extracted would otherwise queue a master mix, because `render_source` happens to
	// default to `master_mix` — one file where the producer asked for eight.
	template <typename JsonValue>
	struct render_payload_codec
	{
		std::function<render_request(const JsonValue&)> read_render_request;
		std::function<JsonValue(const render_result&)> write_render_result;
	};

	// ---------------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------------

	// Registers `render` through `tool_executor.h`'s existing seam.
	//
	// `register_read_tool` rather than `register_mutating_tool`, and the file header is
	// where the reasoning lives: it is the call that opens no undo block and captures
	// no marker, which is requirement 12.6, and it is the only one whose body may
	// return the collision refusal the coordinator produces.
	//
	// `coordinator` and the codec's callables must outlive the registry.
	template <typename JsonValue>
	tool_registration_outcome register_render_tool(
		tool_handler_registry<JsonValue>& registry,
		render_coordinator& coordinator,
		render_payload_codec<JsonValue> codec)
	{
		render_coordinator* const render = &coordinator;

		return registry.register_read_tool(
			render_tool_name,
			[render, read_request = std::move(codec.read_render_request),
				write_result = std::move(codec.write_render_result)](
				const tool_execution_context<JsonValue>& context)
				-> non_mutating_handler_result<JsonValue> {
				if (!read_request || !write_result || context.call.validated_input == nullptr)
				{
					handler_action_outcomes outcomes;
					outcomes.actions.push_back(failed_action(
						render_tool_name,
						missing_render_input_code,
						"the render's source, bounds, and output format could not be read from "
						"the call"));

					return outcomes;
				}

				render_request request = read_request(*context.call.validated_input);

				// The acknowledgement comes from the framework's own extraction rather
				// than from the codec, so there is one place `confirmedOverwrite` is
				// read and one answer to whether the producer approved an overwrite.
				// Two readers of one field is how a refusal comes to be retried
				// against a coordinator that never heard about the approval.
				request.confirmed_overwrite = context.call.confirmed_overwrite;

				const render_outcome outcome = render->queue_render(request);

				switch (outcome.status)
				{
					case render_outcome_status::queued:
						// No undo report is attached here, and there is nowhere to put
						// one: the framework attaches it, and the registration path
						// above opened no block for it to report. Requirement 12.6 as
						// an absence in three places at once — the coordinator's
						// `render_result`, the output schema, and this.
						return handler_success<JsonValue>{write_result(outcome.result)};

					case render_outcome_status::refused:
					{
						// Unchanged, deliberately. The coordinator already chose the
						// reason the refusal schema names and already decided whether
						// an acknowledgement could change the answer.
						return refusal_from_render_refusal(outcome.refusal);
					}

					case render_outcome_status::failed:
						break;
				}

				handler_action_outcomes outcomes;
				outcomes.actions.push_back(failed_action(
					render_tool_name,
					render_call_failed_code,
					outcome.failure_reason));

				return outcomes;
			});
	}
}

#endif
