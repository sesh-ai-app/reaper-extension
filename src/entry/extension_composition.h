// The composition root's logic: what is wired to what, and what it reports when a
// piece is missing (requirements 1.1, 1.3, 2.4, 5.1, 5.2, 5.5, 5.7, 9.1, 13.1, 15.4).
//
// Every component in this extension is built against a narrow seam and tested against a
// recorder. That is what makes the suite possible without REAPER, CEF, or a JSON
// library, and it is also why the graph can only be assembled once the pieces exist —
// which is why task 22.1 is deliberately last among the implementation tasks.
//
// ---------------------------------------------------------------------------
// Where the line is drawn, and why it is drawn here rather than in a `.cpp`
//
// There are two different jobs in a composition root and conflating them produces
// something nothing can check.
//
// **Which object goes in which slot, and what to say when one is absent.** That is
// logic. It decides whether a tool call reaches the Tool Executor, whether the Tool
// Executor's schema answer survives the handoff to the network thread, and whether 42
// tools or 11 are registered. It is also the part most likely to be wrong, because the
// seven destinations are seven distinct interfaces and nothing outside this file ever
// sees them all at once. So it is here, in a header, templated on the payload and
// envelope types — which is what lets `tests/entry/extension_composition_test.cpp`
// drive the whole graph against stubs.
//
// **Binding those slots to nlohmann/json, the REAPER C API, and CEF.** That is
// translation. It lives in `extension_composition.cpp` and in the two panel-browser
// translation units, it cannot be compiled on a machine with no vcpkg, and it is
// deliberately dull for that reason — the same arrangement `ui/cef_browser_host.cpp`
// and `daw/reaper_render_host.cpp` already use.
//
// The practical test of the split: `tests/CMakeLists.txt` does not compile `src/`, and
// the shared-library target is not created without nlohmann/json. A composition root
// that is one `.cpp` is therefore a composition root nothing in this repository can
// check on a developer machine.
//
// ---------------------------------------------------------------------------
// The seven destinations
//
// `transport/message_dispatcher.h` declares seven seams and names none of the
// components behind them. The seven adapters below are the other side of each, and
// they are thin on purpose — an adapter that decides anything is a decision made
// somewhere the component that owns it cannot see.
//
// Two of them carry a decision anyway, and both are about information that would
// otherwise be lost at a boundary:
//
//   - `ExecutorToolCallRouter` reads `daw::result_schema_path_for` once and hands the
//     answer across. Nothing downstream can re-derive it: a framework
//     `tool_partial_outcome` and the named tool's own success are indistinguishable
//     from a payload plus a tool name.
//   - `QueuedToolResultSink` copies that answer onto
//     `OutboundEnvelope::payload_schema_path_override`, which is the field task 22.2
//     added so the answer could cross the `QueuePair` with the envelope rather than
//     beside it.
//
// ---------------------------------------------------------------------------
// The registry is composed over nine REAPER-backed adapters, and reports its gaps
//
// `compose_tool_handler_registry` makes ten registration calls over the nine adapters
// under `src/daw/` — the seven `reaper_*_host` units plus `reaper_alias_storage.cpp`
// behind `AliasStore` and `reaper_undo_stack.cpp` behind `undo_manager`. Ten rather
// than nine because `MarkerRegionHost` backs three families: the six mutating marker
// and region tools, the two reads, and `list_tempo_changes`.
//
// Note what is *not* here: a `ReaperApi`. Each family reaches REAPER through its own
// narrow host interface, and that is enforced structurally rather than by convention —
// `CMakeLists.txt` puts the SDK include path on the library target alone, so a header
// that leaks the SDK is a build failure in the suite rather than a test nobody can run.
// This file names seven host interfaces and no REAPER type at all.
//
// The report is the point of the function as much as the registration is.
// `tool_handler_registry::unregistered_constrained_tool_names` answers "the 42 minus
// what is registered", and surfacing that at startup is the difference between a known
// gap and a producer discovering it one unknown-tool error at a time.

#ifndef SESH_AI_ENTRY_EXTENSION_COMPOSITION_H
#define SESH_AI_ENTRY_EXTENSION_COMPOSITION_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/alias_store.h>
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
#include <transport/message_dispatcher.h>
#include <ui/bridge_publishers.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

namespace sesh_ai::entry {

	// ---------------------------------------------------------------------------
	// The Tool Executor's two JSON seams
	// ---------------------------------------------------------------------------

	// How a tool call and its result cross the JSON boundary.
	//
	// Supplied by the caller for the same reason every `*_payload_codec` in
	// `src/daw/tools/` is: these two functions are the only part of the tool path that
	// needs a JSON library, and parameterising them is what keeps the routing
	// exercisable without one.
	//
	// `read_tool_call` is the Envelope Codec's side of requirement 9.1 — the track
	// selectors the executor resolves before any handler runs are read out of the
	// payload here, and so are the four acknowledgement flags the refusal schema names.
	// `write_dispatch_outcome` is the inverse, and it is a single function over all four
	// shapes rather than four, because the shape is a variant the caller can visit and
	// splitting it here would put the same `std::visit` in two places.
	template <typename InboundEnvelopeType, typename JsonValue>
	struct ToolCallPayloadCodec {
		std::function<daw::tool_call<JsonValue>(
			std::string_view tool_name,
			const InboundEnvelopeType& inbound_envelope
		)> read_tool_call;

		std::function<JsonValue(const daw::dispatch_outcome<JsonValue>&)> write_dispatch_outcome;

		bool is_complete() const
		{
			return static_cast<bool>(read_tool_call)
				&& static_cast<bool>(write_dispatch_outcome);
		}
	};

	// `MessageDispatcher`'s Tool Executor seam (requirement 5.1).
	//
	// Three things happen here and nothing else: the call is read out of the envelope,
	// the executor dispatches it, and the outcome is written back out with the
	// executor's own schema answer attached.
	//
	// `tool_is_known` is false for requirement 23.6, and the response still goes out —
	// `build_unknown_tool_error` has already put the tool's name, the `unknown_tool`
	// code, and a sentence a producer reads inside the payload. A name the server sends
	// that this build does not implement is a visible gap, not silence.
	template <typename InboundEnvelopeType, typename JsonValue>
	class ExecutorToolCallRouter final
		: public transport::ToolCallRouter<InboundEnvelopeType, JsonValue> {
	public:
		ExecutorToolCallRouter(
			daw::tool_executor_of<JsonValue>& executor,
			ToolCallPayloadCodec<InboundEnvelopeType, JsonValue> codec)
			: executor_{executor},
			codec_{std::move(codec)}
		{
		}

		ExecutorToolCallRouter(const ExecutorToolCallRouter&) = delete;
		ExecutorToolCallRouter& operator=(const ExecutorToolCallRouter&) = delete;
		ExecutorToolCallRouter(ExecutorToolCallRouter&&) = delete;
		ExecutorToolCallRouter& operator=(ExecutorToolCallRouter&&) = delete;

		transport::ToolCallResponse<JsonValue> execute_tool_call(
			std::string_view tool_name,
			const InboundEnvelopeType& inbound_envelope) override
		{
			transport::ToolCallResponse<JsonValue> response;

			// An incomplete codec is a composition fault rather than a protocol one, so
			// it is reported as the tool being unavailable rather than by throwing into
			// the tick. `build_unknown_tool_error`'s payload is the honest shape for it:
			// no tool ran, and the response names which one.
			if (!codec_.is_complete()) {
				response.tool_is_known = false;
				++unavailable_call_count_;

				return response;
			}

			const daw::dispatch_outcome<JsonValue> outcome =
				executor_.execute(codec_.read_tool_call(tool_name, inbound_envelope));

			response.payload = codec_.write_dispatch_outcome(outcome);
			response.tool_is_known = !daw::is_unknown_tool(outcome);

			// The executor's answer, read once and carried. See the file comment.
			const std::optional<std::string_view> result_schema_path =
				daw::result_schema_path_for(outcome);

			if (result_schema_path.has_value()) {
				response.result_schema_path.assign(*result_schema_path);
			}

			++executed_call_count_;

			return response;
		}

		std::size_t executed_call_count() const noexcept { return executed_call_count_; }

		// Calls that could not be dispatched because this build has no codec. Zero in
		// any build that configured one, and the value a startup log line reports when
		// it is not.
		std::size_t unavailable_call_count() const noexcept { return unavailable_call_count_; }

	private:
		daw::tool_executor_of<JsonValue>& executor_;
		ToolCallPayloadCodec<InboundEnvelopeType, JsonValue> codec_;

		std::size_t executed_call_count_ = 0;
		std::size_t unavailable_call_count_ = 0;
	};

	// `MessageDispatcher`'s tool result seam.
	//
	// One assignment is the whole reason this adapter is worth reading:
	// `payload_schema_path_override` takes `result_schema_path()` so the Tool Executor's
	// answer reaches the codec on the network thread. Without it the codec re-derives,
	// and `outbound_schema_path_for` cannot be right for a framework partial — it would
	// route a `tool_partial_outcome` to the named tool's own output schema, which
	// refuses it on `items` the framework cannot supply, and a producer sees the tool
	// silently not working.
	//
	// Empty is a legitimate answer and means "derive it as usual": an unknown tool, or
	// a success naming something outside the 42. Assigned unconditionally, because
	// empty is also what the field's own documented default means.
	template <typename OutboundEnvelopeType, typename JsonValue>
	class QueuedToolResultSink final : public transport::ToolResultSink<JsonValue> {
	public:
		using OutboundQueue = transport::ConcurrentQueue<OutboundEnvelopeType>;

		explicit QueuedToolResultSink(OutboundQueue& outbound_queue)
			: outbound_queue_{outbound_queue}
		{
		}

		QueuedToolResultSink(const QueuedToolResultSink&) = delete;
		QueuedToolResultSink& operator=(const QueuedToolResultSink&) = delete;
		QueuedToolResultSink(QueuedToolResultSink&&) = delete;
		QueuedToolResultSink& operator=(QueuedToolResultSink&&) = delete;

		bool send_tool_result(const transport::ToolResultResponse<JsonValue>& response) override
		{
			OutboundEnvelopeType outbound_envelope;

			outbound_envelope.type = response.type();

			// Requirement 5.9. `ToolResultResponse` cannot be built without the call it
			// answers, so this copy cannot be of an identifier nobody supplied.
			outbound_envelope.request_id = response.request_id();
			outbound_envelope.payload = response.payload();
			outbound_envelope.payload_schema_path_override = response.result_schema_path();

			outbound_queue_.push(std::move(outbound_envelope));
			++queued_result_count_;

			// True because the envelope is now the queue's. Validation happens on the
			// network thread (requirement 22.2), so a payload that fails its own schema
			// is refused there and reported there — this is the handoff, not the verdict.
			return true;
		}

		std::size_t queued_result_count() const noexcept { return queued_result_count_; }

	private:
		OutboundQueue& outbound_queue_;
		std::size_t queued_result_count_ = 0;
	};

	// ---------------------------------------------------------------------------
	// The other five destinations
	// ---------------------------------------------------------------------------

	// `confirm:request` and `confirm:resolved` (requirements 5.2, 5.6).
	//
	// Both of the coordinator's envelope-level entry points, under the names the seam
	// gives them. The coordinator also has `present_confirmation_request` and
	// `resolve_confirmation`, which take the payload already taken apart — reaching for
	// those here would mean decoding the payload twice.
	template <typename InboundEnvelopeType, typename OutboundEnvelopeType>
	class CoordinatorConfirmationRouter final
		: public transport::ConfirmationRouter<InboundEnvelopeType> {
	public:
		explicit CoordinatorConfirmationRouter(
			transport::ConfirmationCoordinator<OutboundEnvelopeType>& coordinator)
			: coordinator_{coordinator}
		{
		}

		CoordinatorConfirmationRouter(const CoordinatorConfirmationRouter&) = delete;
		CoordinatorConfirmationRouter& operator=(const CoordinatorConfirmationRouter&) = delete;
		CoordinatorConfirmationRouter(CoordinatorConfirmationRouter&&) = delete;
		CoordinatorConfirmationRouter& operator=(CoordinatorConfirmationRouter&&) = delete;

		void handle_confirmation_request(const InboundEnvelopeType& inbound_envelope) override
		{
			last_request_outcome_ = coordinator_.handle_confirmation_request(inbound_envelope);
		}

		void handle_confirmation_resolved(const InboundEnvelopeType& inbound_envelope) override
		{
			last_resolved_outcome_ = coordinator_.handle_confirmation_resolved(inbound_envelope);
		}

		// The coordinator returns an outcome and the seam returns nothing, so the most
		// recent one is held for the log. `ConfirmationRequestDisposition::unaddressable`
		// is the one worth a line: a `confirm:request` with no identifier cannot be
		// answered, so no prompt was raised at all.
		const std::optional<transport::ConfirmationRequestOutcome>& last_request_outcome() const
		{
			return last_request_outcome_;
		}

		const std::optional<transport::ConfirmationResolvedOutcome>& last_resolved_outcome() const
		{
			return last_resolved_outcome_;
		}

	private:
		transport::ConfirmationCoordinator<OutboundEnvelopeType>& coordinator_;

		std::optional<transport::ConfirmationRequestOutcome> last_request_outcome_;
		std::optional<transport::ConfirmationResolvedOutcome> last_resolved_outcome_;
	};

	// `request:project_context` (requirements 5.3, 6.9).
	//
	// The identifier and nothing else, because the request has an empty payload. The
	// builder needs the outbound queue to publish into, and it is held here rather than
	// passed per call for the same reason the coordinator holds its own: the queue
	// outlives every request.
	template <typename JsonValue, typename OutboundEnvelopeType>
	class BuilderProjectContextRouter final : public transport::ProjectContextRouter {
	public:
		using OutboundQueue = transport::ConcurrentQueue<OutboundEnvelopeType>;

		BuilderProjectContextRouter(
			context::ProjectContextBuilder<JsonValue, OutboundEnvelopeType>& builder,
			OutboundQueue& outbound_queue)
			: builder_{builder},
			outbound_queue_{outbound_queue}
		{
		}

		BuilderProjectContextRouter(const BuilderProjectContextRouter&) = delete;
		BuilderProjectContextRouter& operator=(const BuilderProjectContextRouter&) = delete;
		BuilderProjectContextRouter(BuilderProjectContextRouter&&) = delete;
		BuilderProjectContextRouter& operator=(BuilderProjectContextRouter&&) = delete;

		void answer_project_context_request(std::string_view request_id) override
		{
			last_publication_ = builder_.publish_on_request(outbound_queue_, request_id);
		}

		const std::optional<context::SnapshotPublication>& last_publication() const
		{
			return last_publication_;
		}

	private:
		context::ProjectContextBuilder<JsonValue, OutboundEnvelopeType>& builder_;
		OutboundQueue& outbound_queue_;

		std::optional<context::SnapshotPublication> last_publication_;
	};

	// `request:transport` (requirement 5.4).
	template <typename InboundEnvelopeType, typename OutboundEnvelopeType>
	class HandlerTransportCommandRouter final
		: public transport::TransportCommandRouter<InboundEnvelopeType> {
	public:
		explicit HandlerTransportCommandRouter(
			transport::TransportHandler<OutboundEnvelopeType>& handler)
			: handler_{handler}
		{
		}

		HandlerTransportCommandRouter(const HandlerTransportCommandRouter&) = delete;
		HandlerTransportCommandRouter& operator=(const HandlerTransportCommandRouter&) = delete;
		HandlerTransportCommandRouter(HandlerTransportCommandRouter&&) = delete;
		HandlerTransportCommandRouter& operator=(HandlerTransportCommandRouter&&) = delete;

		void execute_transport_command(const InboundEnvelopeType& inbound_envelope) override
		{
			last_outcome_ = handler_.handle(inbound_envelope);
		}

		const std::optional<transport::TransportRequestOutcome>& last_outcome() const
		{
			return last_outcome_;
		}

	private:
		transport::TransportHandler<OutboundEnvelopeType>& handler_;
		std::optional<transport::TransportRequestOutcome> last_outcome_;
	};

	// `stream:*` and `error:*` (requirements 5.5, 23.2).
	//
	// One seam for twelve envelope types: the Stream Presenter's five, and the seven
	// `error:*` types that have no bundled payload schema and fold into
	// `StreamErrorView` alongside `stream:error`.
	//
	// Nothing is published from here. The presenter accumulates and the tick publishes,
	// which is requirement 2.3 — and it is also what makes a turn's forty deltas one
	// repaint rather than forty.
	template <typename InboundEnvelopeType>
	class PresenterStreamRouter final : public transport::StreamRouter<InboundEnvelopeType> {
	public:
		explicit PresenterStreamRouter(ui::StreamPresenter& stream_presenter)
			: stream_presenter_{stream_presenter}
		{
		}

		PresenterStreamRouter(const PresenterStreamRouter&) = delete;
		PresenterStreamRouter& operator=(const PresenterStreamRouter&) = delete;
		PresenterStreamRouter(PresenterStreamRouter&&) = delete;
		PresenterStreamRouter& operator=(PresenterStreamRouter&&) = delete;

		void present_stream_envelope(const InboundEnvelopeType& inbound_envelope) override
		{
			last_outcome_ = stream_presenter_.handle(inbound_envelope);
		}

		const std::optional<ui::StreamEnvelopeOutcome>& last_outcome() const
		{
			return last_outcome_;
		}

	private:
		ui::StreamPresenter& stream_presenter_;
		std::optional<ui::StreamEnvelopeOutcome> last_outcome_;
	};

	// `script:download` (requirement 5.7, ADR 0011).
	//
	// The one inbound route whose payload crosses to JavaScript untouched, because
	// there is nothing for the C++ side to decide — so the message name *is* the
	// envelope type, read from `ui_host.h` rather than spelled here.
	//
	// A download and never an execution. The only operation is a publish, and there is
	// deliberately nothing here that could hand the script to REAPER.
	//
	// `serialise_payload` is the one JSON-touching step: the payload arrived parsed and
	// the bridge carries text. It is a seam for the usual reason, and a payload it
	// renders as empty is refused by `UiHost::publish_to_javascript` rather than
	// published — an empty string is not a JSON document, and `{}` is.
	template <typename InboundEnvelopeType, typename JsonValue>
	class UiHostScriptDownloadRouter final
		: public transport::ScriptDownloadRouter<InboundEnvelopeType> {
	public:
		using PayloadSerialiser = std::function<std::string(const JsonValue&)>;

		UiHostScriptDownloadRouter(ui::UiHost& ui_host, PayloadSerialiser serialise_payload)
			: ui_host_{ui_host},
			serialise_payload_{std::move(serialise_payload)}
		{
		}

		UiHostScriptDownloadRouter(const UiHostScriptDownloadRouter&) = delete;
		UiHostScriptDownloadRouter& operator=(const UiHostScriptDownloadRouter&) = delete;
		UiHostScriptDownloadRouter(UiHostScriptDownloadRouter&&) = delete;
		UiHostScriptDownloadRouter& operator=(UiHostScriptDownloadRouter&&) = delete;

		void present_script_download(const InboundEnvelopeType& inbound_envelope) override
		{
			if (!serialise_payload_) {
				++unserialisable_download_count_;
				return;
			}

			tally_.note(ui_host_.publish_to_javascript(ui::bridge_message(
				std::string{ui::script_download_bridge_message_name},
				serialise_payload_(inbound_envelope.payload)
			)));
		}

		const ui::BridgePublicationTally& tally() const noexcept { return tally_; }

		std::size_t unserialisable_download_count() const noexcept
		{
			return unserialisable_download_count_;
		}

	private:
		ui::UiHost& ui_host_;
		PayloadSerialiser serialise_payload_;

		ui::BridgePublicationTally tally_;
		std::size_t unserialisable_download_count_ = 0;
	};

	// The spelling the dispatcher routes on and the spelling the bridge publishes under
	// are the same constant, which is what makes forwarding the payload untouched
	// correct. Checked here rather than trusted, because the two are read by different
	// halves of the system and a drift between them is a download the panel never
	// hears about.
	static_assert(
		transport::script_download_envelope_type == ui::script_download_bridge_message_name,
		"the script download route and its bridge message must be one spelling"
	);

	// ---------------------------------------------------------------------------
	// The Tool Executor's handler registry
	// ---------------------------------------------------------------------------

	// The nine REAPER-backed adapters, as the seven host interfaces and two stores the
	// tool families actually take.
	//
	// References, not values. A host copied into a closure is a second view of one
	// project, which every one of the `register_*_tools` functions says in its own
	// words; holding them by reference here keeps that true one level up. Everything
	// named must outlive the registry, which outlives one dispatch — in the extension
	// they are all members of the object that owns the timer registration.
	//
	// No `JsonValue` parameter: not one of these depends on the payload type. The REAPER
	// side of the extension does not know what a JSON document is, which is the whole
	// reason the codecs below are separate.
	struct ToolHostReferences {
		// `reaper_fx_host.cpp`
		daw::tools::fx_host& fx;

		// `tools/reaper_item_host.cpp`
		daw::tools::item_host& item;

		// `tools/reaper_marker_region_host.cpp`. Backs three families — the six
		// mutating marker and region tools, the two reads, and `list_tempo_changes`.
		daw::tools::MarkerRegionHost& marker_region;

		// `tools/reaper_routing_host.cpp`
		daw::tools::routing_host& routing;

		// `tools/reaper_track_state_host.cpp`
		daw::tools::track_state_host& track_state;

		// `tools/reaper_track_structure_host.cpp`
		daw::tools::TrackStructureHost& track_structure;

		// `reaper_render_host.cpp`, behind the Render Coordinator. The coordinator
		// rather than the host, because `render` is the one tool whose logic is a
		// component of its own.
		daw::render_coordinator& render;

		// `reaper_undo_stack.cpp`, behind the Undo Manager (ADR 0015).
		daw::undo_manager& undo;

		// `reaper_alias_storage.cpp`, behind the Alias Store. Both faces of it are
		// needed: `learn_track_alias` and `forget_track_alias` write through the store,
		// and `list_tracks` reads through the lookup.
		daw::AliasStore& alias_store;
		const daw::LearnedAliasLookup& learned_aliases;
	};

	// The per-family codecs, for the seven families whose registration takes one.
	//
	// fx, item, and track_state are not here: those three take a reader and a writer
	// directly, because each of their readers is an overload set across the family's
	// request types rather than one function, and an overload set does not fit in a
	// `std::function`. They are passed to `compose_tool_handler_registry` as template
	// parameters for that reason.
	template <typename JsonValue>
	struct ToolPayloadCodecs {
		daw::tools::marker_region_payload_codec<JsonValue> marker_region;
		daw::tools::marker_region_read_payload_codec<JsonValue> marker_region_read;
		daw::tools::render_payload_codec<JsonValue> render;
		daw::tools::routing_payload_codec<JsonValue> routing;
		daw::tools::tempo_payload_codec<JsonValue> tempo;
		daw::tools::track_structure_payload_codec<JsonValue> track_structure;
		daw::tools::undo_payload_codec<JsonValue> undo;
	};

	// One tool that did not register, and why.
	struct ToolRegistrationFailure {
		std::string tool_name;
		daw::tool_registration_outcome outcome = daw::tool_registration_outcome::no_handler_supplied;
	};

	// What composing the registry achieved, and what it did not.
	//
	// Returned rather than logged, like every other outcome in this codebase. The
	// plugin entry writes the line, because it is the component that knows how this
	// build reports things.
	struct ToolRegistryCompositionReport {
		// In registration order, which `tool_handler_registry` preserves.
		std::vector<std::string> registered_tool_names;

		// A name that was offered and refused: not one of the 42, already registered, or
		// no handler supplied.
		std::vector<ToolRegistrationFailure> failures;

		// The 42 minus what is registered. Distinct from `failures`, and the distinction
		// is the useful one: a failure is a name this composition tried and could not
		// place, while this is a name nothing even offered — which is what an absent
		// codec looks like, because every `register_*_tools` returns early on an
		// incomplete one rather than registering a handler that would throw.
		std::vector<std::string> unregistered_constrained_tool_names;

		bool every_constrained_tool_registered() const
		{
			return unregistered_constrained_tool_names.empty();
		}

		bool anything_was_refused() const { return !failures.empty(); }
	};

	namespace detail {

		inline void note_registration(
			ToolRegistryCompositionReport& report,
			std::string_view tool_name,
			daw::tool_registration_outcome outcome)
		{
			if (outcome == daw::tool_registration_outcome::registered) {
				report.registered_tool_names.emplace_back(tool_name);
				return;
			}

			report.failures.push_back(ToolRegistrationFailure{std::string{tool_name}, outcome});
		}

		// The three families that report a vector of name-and-outcome pairs.
		template <typename RegistrationVector>
		void note_registration_vector(
			ToolRegistryCompositionReport& report,
			const RegistrationVector& registrations)
		{
			for (const auto& registration : registrations) {
				note_registration(report, registration.tool_name, registration.outcome);
			}
		}

		// The two families that report two name lists instead. The outcome is not
		// carried on those, so a rejection is recorded under the one value that is
		// certainly true of it — the name was offered and is not in the registry.
		inline void note_registration_name_lists(
			ToolRegistryCompositionReport& report,
			const std::vector<std::string>& registered_tool_names,
			const std::vector<std::string>& rejected_tool_names)
		{
			for (const std::string& tool_name : registered_tool_names) {
				report.registered_tool_names.push_back(tool_name);
			}

			for (const std::string& tool_name : rejected_tool_names) {
				report.failures.push_back(
					ToolRegistrationFailure{tool_name, daw::tool_registration_outcome::no_handler_supplied}
				);
			}
		}

	}

	// Registers all 42 tools over the nine adapters (requirement 9.1, design "Tool
	// Executor").
	//
	// Ten calls, one per family, in a fixed order — which is the order
	// `registered_tool_names` reports, because the registry keeps insertion order and
	// that is what makes a startup log line reproducible across machines.
	//
	// Nothing here decides anything about a tool. Each family's registration call owns
	// which seam each of its tools goes through, which is the choice of whether a
	// refusal is possible and whether an undo block is opened, and those are decisions
	// that belong beside the handlers. What belongs here is that all ten are called,
	// with the right adapter, and that the gap is reported.
	//
	// `read_request` and `write_result` are the overload sets fx, item, and track_state
	// take. One pair serves all three: their request and result types are distinct, so
	// overload resolution picks the right member, and three separate pairs would be
	// three places to forget one.
	template <typename JsonValue, typename RequestReader, typename ResultWriter>
	ToolRegistryCompositionReport compose_tool_handler_registry(
		daw::tool_handler_registry<JsonValue>& registry,
		const ToolHostReferences& hosts,
		ToolPayloadCodecs<JsonValue> codecs,
		RequestReader read_request,
		ResultWriter write_result)
	{
		ToolRegistryCompositionReport report;

		// --- track structure: create, delete, duplicate, set_folder_structure ---
		const daw::tools::track_structure_registration_report track_structure =
			daw::tools::register_track_structure_tools<JsonValue>(
				registry,
				hosts.track_structure,
				std::move(codecs.track_structure)
			);

		detail::note_registration(
			report, daw::tools::create_track_tool_name, track_structure.create_track);
		detail::note_registration(
			report, daw::tools::delete_track_tool_name, track_structure.delete_track);
		detail::note_registration(
			report, daw::tools::duplicate_track_tool_name, track_structure.duplicate_track);
		detail::note_registration(
			report,
			daw::tools::set_folder_structure_tool_name,
			track_structure.set_folder_structure
		);

		// --- track state: set_track_properties, list_tracks, and the alias tools ---
		detail::note_registration_vector(
			report,
			daw::tools::register_track_state_tools<JsonValue>(
				registry,
				hosts.track_state,
				hosts.alias_store,
				hosts.learned_aliases,
				read_request,
				write_result
			)
		);

		// --- routing: the six mutating tools plus `get_routing` ---
		const daw::tools::routing_tool_registration routing =
			daw::tools::register_routing_tools<JsonValue>(
				registry,
				hosts.routing,
				std::move(codecs.routing)
			);

		detail::note_registration_name_lists(
			report, routing.registered_tool_names, routing.rejected_tool_names);

		// --- fx ---
		detail::note_registration_vector(
			report,
			daw::tools::register_fx_tools<JsonValue>(
				registry,
				hosts.fx,
				read_request,
				write_result
			)
		);

		// --- items ---
		detail::note_registration_vector(
			report,
			daw::tools::register_item_tools<JsonValue>(
				registry,
				hosts.item,
				read_request,
				write_result
			)
		);

		// --- markers and regions, mutating ---
		const daw::tools::marker_region_registration_report marker_region =
			daw::tools::register_marker_region_tools<JsonValue>(
				registry,
				hosts.marker_region,
				std::move(codecs.marker_region)
			);

		detail::note_registration(
			report, daw::tools::create_marker_tool_name, marker_region.create_marker);
		detail::note_registration(
			report, daw::tools::create_region_tool_name, marker_region.create_region);
		detail::note_registration(
			report,
			daw::tools::update_marker_or_region_tool_name,
			marker_region.update_marker_or_region
		);
		detail::note_registration(
			report,
			daw::tools::delete_marker_or_region_tool_name,
			marker_region.delete_marker_or_region
		);
		detail::note_registration(
			report, daw::tools::change_tempo_map_tool_name, marker_region.change_tempo_map);
		detail::note_registration(
			report, daw::tools::set_time_selection_tool_name, marker_region.set_time_selection);

		// --- markers and regions, reads ---
		const daw::tools::marker_region_read_registration_report marker_region_reads =
			daw::tools::register_marker_region_read_tools<JsonValue>(
				registry,
				hosts.marker_region,
				std::move(codecs.marker_region_read)
			);

		detail::note_registration(
			report, daw::tools::list_markers_tool_name, marker_region_reads.list_markers);
		detail::note_registration(
			report, daw::tools::list_regions_tool_name, marker_region_reads.list_regions);

		// --- the tempo map read, over the same host ---
		const daw::tools::tempo_registration_report tempo =
			daw::tools::register_tempo_tools<JsonValue>(
				registry,
				hosts.marker_region,
				std::move(codecs.tempo)
			);

		detail::note_registration(
			report, daw::tools::list_tempo_changes_tool_name, tempo.list_tempo_changes);

		// --- undo: the two rewinding tools ---
		const daw::tools::undo_tool_registration undo =
			daw::tools::register_undo_tools<JsonValue>(
				registry,
				hosts.undo,
				std::move(codecs.undo)
			);

		detail::note_registration_name_lists(
			report, undo.registered_tool_names, undo.rejected_tool_names);

		// --- render, which opens no undo block and records no marker (requirement
		// 12.7) ---
		detail::note_registration(
			report,
			daw::tools::render_tool_name,
			daw::tools::register_render_tool<JsonValue>(
				registry,
				hosts.render,
				std::move(codecs.render)
			)
		);

		report.unregistered_constrained_tool_names = registry.unregistered_constrained_tool_names();

		return report;
	}

	// ---------------------------------------------------------------------------
	// JavaScript to C++
	// ---------------------------------------------------------------------------

	// `UiHost`'s `DispatcherSink`: the three messages the UI originates.
	//
	// `ui_host.h` names them and the design document's bridge contract says why all
	// three are protocol payloads carried verbatim rather than view models. The prompt
	// in particular is the UI's to build and not bare text handed to C++ for framing,
	// because the standalone Bedrock Guardrail's `dataPath` resolves to a named field
	// inside that payload — framing it here would put the guarded text somewhere the
	// guardrail does not look.
	//
	// So two of the three are a push onto the outbound queue with no decoding at all,
	// and the third is the one that is answered locally: a confirmation decision goes
	// to the Confirmation Coordinator, which already holds the pending table and
	// refuses an answer for a prompt that resolved or expired while the producer read
	// it. Sending it straight to the server instead would have the extension
	// authorising a destructive operation whose window has closed.
	//
	// A name outside the three is refused rather than forwarded. `UiHost` reports that
	// as `dispatcher_refused`, and it is the only answer: the bridge is both halves'
	// own contract, so a name neither side declared is a drift worth surfacing rather
	// than a message to pass along and hope about.
	template <typename JsonValue, typename OutboundEnvelopeType>
	class BridgeMessageDispatcherSink final : public ui::DispatcherSink {
	public:
		using OutboundQueue = transport::ConcurrentQueue<OutboundEnvelopeType>;

		// Parsing is a seam for the usual reason: the bridge carries text, the outbound
		// queue carries a parsed payload, and a JSON library is what sits between them.
		// Empty is "this was not a JSON document", which is refused rather than queued.
		using PayloadParser = std::function<std::optional<JsonValue>(const std::string&)>;

		// The `requestId` out of a parsed `confirm-decision` payload. Empty when the
		// payload carried none, which the coordinator then refuses as untracked —
		// `confirm-decision.schema.json` requires one, so an empty answer means the
		// payload would not have been sendable either.
		using RequestIdentifierReader = std::function<std::string(const JsonValue&)>;

		BridgeMessageDispatcherSink(
			transport::ConfirmationCoordinator<OutboundEnvelopeType>& confirmation_coordinator,
			OutboundQueue& outbound_queue,
			PayloadParser parse_payload,
			RequestIdentifierReader read_request_identifier)
			: confirmation_coordinator_{confirmation_coordinator},
			outbound_queue_{outbound_queue},
			parse_payload_{std::move(parse_payload)},
			read_request_identifier_{std::move(read_request_identifier)}
		{
		}

		BridgeMessageDispatcherSink(const BridgeMessageDispatcherSink&) = delete;
		BridgeMessageDispatcherSink& operator=(const BridgeMessageDispatcherSink&) = delete;
		BridgeMessageDispatcherSink(BridgeMessageDispatcherSink&&) = delete;
		BridgeMessageDispatcherSink& operator=(BridgeMessageDispatcherSink&&) = delete;

		bool accept_message_from_javascript(const ui::BridgeMessage& message) override
		{
			if (!parse_payload_ || !read_request_identifier_) {
				++unparseable_message_count_;
				return false;
			}

			const std::string& message_name = message.message_name();

			// Only the three the UI originates. A `view:` name arriving from JavaScript
			// is the bridge pointed the wrong way round and is refused on the strength
			// of the direction `ui_host.h` declares, not on a guess.
			if (ui::bridge_message_direction_for(message_name)
				!= ui::BridgeMessageDirection::from_javascript) {
				++refused_message_count_;
				return false;
			}

			std::optional<JsonValue> payload =
				parse_payload_(message.payload().serialized_json());

			if (!payload.has_value()) {
				++unparseable_message_count_;
				return false;
			}

			if (message_name == ui::producer_prompt_bridge_message_name) {
				return queue_outbound(message_name, std::move(*payload));
			}

			// The two decisions. `decision` must agree with the name, which is the rule
			// the server applies to the envelope too — so the verdict is taken from the
			// name rather than from the payload's field, and the payload's own field is
			// the UI's copy of the same answer.
			const transport::ProducerVerdict verdict =
				message_name == ui::confirmation_approval_bridge_message_name
					? transport::ProducerVerdict::approved
					: transport::ProducerVerdict::rejected;

			const std::string request_identifier = read_request_identifier_(*payload);

			const transport::ConfirmationDecisionOutcome outcome =
				confirmation_coordinator_.answer(request_identifier, verdict);

			last_decision_outcome_ = outcome;

			// The coordinator queued the decision envelope itself, so there is nothing
			// to push here. False for a prompt it is not tracking, which is the refusal
			// worth propagating: the producer tapped a button on a prompt that is no
			// longer live.
			return outcome.decision_queued;
		}

		std::size_t queued_message_count() const noexcept { return queued_message_count_; }
		std::size_t refused_message_count() const noexcept { return refused_message_count_; }

		std::size_t unparseable_message_count() const noexcept
		{
			return unparseable_message_count_;
		}

		const std::optional<transport::ConfirmationDecisionOutcome>& last_decision_outcome() const
		{
			return last_decision_outcome_;
		}

	private:
		bool queue_outbound(const std::string& envelope_type, JsonValue payload)
		{
			OutboundEnvelopeType outbound_envelope;

			outbound_envelope.type = envelope_type;
			outbound_envelope.payload = std::move(payload);

			// `payload_schema_path_override` is deliberately left empty. The schema
			// follows from the type for every one of these, so the codec derives it as
			// it always has — the override exists for the one case it cannot, which is a
			// tool result.
			outbound_queue_.push(std::move(outbound_envelope));
			++queued_message_count_;

			return true;
		}

		transport::ConfirmationCoordinator<OutboundEnvelopeType>& confirmation_coordinator_;
		OutboundQueue& outbound_queue_;
		PayloadParser parse_payload_;
		RequestIdentifierReader read_request_identifier_;

		std::size_t queued_message_count_ = 0;
		std::size_t refused_message_count_ = 0;
		std::size_t unparseable_message_count_ = 0;

		std::optional<transport::ConfirmationDecisionOutcome> last_decision_outcome_;
	};

	// ---------------------------------------------------------------------------
	// The graph
	// ---------------------------------------------------------------------------

	// Everything the extension owns on the main thread, wired together.
	//
	// What this class is for is the construction order and the reference topology, both
	// of which are the kind of thing that is obvious while writing it and impossible to
	// reconstruct afterwards. Member declaration order below *is* the construction
	// order, and one edge in it runs backwards on purpose — see `dispatcher_sink_`.
	//
	// Not a singleton and not static. One per REAPER process is the actual cardinality,
	// and `plugin_entry.cpp` already holds the one instance; making that a property of
	// this type would also make it untestable, and the suite constructs several.
	//
	// Templated on the payload type and both envelope types, which is what lets the
	// whole graph be assembled and driven in `tests/entry/extension_composition_test.cpp`
	// with no JSON library, no REAPER, and no CEF. `extension_composition.cpp` is the
	// one place the three parameters become `nlohmann::json`, `InboundEnvelope`, and
	// `OutboundEnvelope`.
	template <typename JsonValue, typename InboundEnvelopeType, typename OutboundEnvelopeType>
	class ExtensionObjectGraph {
	public:
		using OutboundQueue = transport::ConcurrentQueue<OutboundEnvelopeType>;
		using Sink = BridgeMessageDispatcherSink<JsonValue, OutboundEnvelopeType>;
		using ToolCallCodec = ToolCallPayloadCodec<InboundEnvelopeType, JsonValue>;

		// What the graph is built over: the queue it answers into, the REAPER-backed
		// adapters, and the JSON seams.
		//
		// References for everything whose lifetime is the plugin entry's, values for
		// the callables. Nothing here is owned by the graph, which is what keeps the
		// teardown order `plugin_entry.h` specifies in the plugin entry's hands.
		struct Dependencies {
			OutboundQueue& outbound_queue;

			// The nine REAPER-backed adapters.
			ToolHostReferences hosts;

			// Requirement 9.1's input, read once per call by the executor.
			const daw::TrackListSource& track_list;

			// The Context Builder's reading of the project.
			context::ProjectContextSource& project_context_source;

			// The seven transport commands' one REAPER call apiece.
			transport::TransportActionInvoker& transport_actions;

			// `MainThreadDispatcher::request_ui_state_publication`, which is documented
			// safe from any thread — the Transport Client presents a connection status
			// from the network thread and this is how the publish reaches the tick.
			ui::UiStatePublicationRequest request_ui_state_publication;

			// The JSON seams. Every one of them is a `std::function` so that the graph
			// is assemblable without a JSON library; `extension_composition.cpp` fills
			// them in with nlohmann and the suite with stubs.
			ToolCallCodec tool_call_codec;
			ToolPayloadCodecs<JsonValue> tool_payload_codecs;
			typename Sink::PayloadParser parse_bridge_payload;
			typename Sink::RequestIdentifierReader read_bridge_request_identifier;
			typename UiHostScriptDownloadRouter<InboundEnvelopeType, JsonValue>::PayloadSerialiser
				serialise_script_download_payload;
		};

		// `read_request` and `write_result` are fx, item, and track_state's overload
		// sets. Taken here rather than on `Dependencies` because they cannot be
		// `std::function` members — each is an overload set across its family's request
		// types, and one `std::function` holds one signature.
		template <typename RequestReader, typename ResultWriter>
		ExtensionObjectGraph(
			Dependencies dependencies,
			RequestReader read_request,
			ResultWriter write_result)
			: outbound_queue_{dependencies.outbound_queue},
			dispatcher_sink_{
				confirmation_coordinator_,
				dependencies.outbound_queue,
				std::move(dependencies.parse_bridge_payload),
				std::move(dependencies.read_bridge_request_identifier)
			},
			ui_host_{deferred_browser_host_, dispatcher_sink_},
			confirmation_presenter_{ui_host_},
			confirmation_coordinator_{confirmation_presenter_, dependencies.outbound_queue},
			connection_state_presenter_{std::move(dependencies.request_ui_state_publication)},
			ui_state_publisher_{ui_host_, connection_state_presenter_, stream_presenter_},
			project_context_builder_{dependencies.project_context_source},
			transport_handler_{dependencies.transport_actions, dependencies.outbound_queue},
			tool_executor_{
				tool_handler_registry_,
				dependencies.hosts.undo,
				dependencies.track_list,
				dependencies.hosts.learned_aliases
			},
			tool_calls_{tool_executor_, std::move(dependencies.tool_call_codec)},
			tool_results_{dependencies.outbound_queue},
			confirmations_{confirmation_coordinator_},
			project_context_{project_context_builder_, dependencies.outbound_queue},
			transport_commands_{transport_handler_},
			streams_{stream_presenter_},
			script_downloads_{
				ui_host_,
				std::move(dependencies.serialise_script_download_payload)
			},
			message_dispatcher_{
				tool_calls_,
				tool_results_,
				confirmations_,
				project_context_,
				transport_commands_,
				streams_,
				script_downloads_
			}
		{
			// Registration happens in the body rather than in the initialiser list,
			// because it needs the registry *and* the hosts, and because the report is
			// the one thing about the graph a caller has to read. The executor already
			// holds the registry by const reference, so filling it afterwards is
			// correct — and is the only order available, since a registry handed to the
			// executor before the handlers exist is the same object either way.
			tool_registry_report_ = compose_tool_handler_registry<JsonValue>(
				tool_handler_registry_,
				dependencies.hosts,
				std::move(dependencies.tool_payload_codecs),
				read_request,
				write_result
			);
		}

		ExtensionObjectGraph(const ExtensionObjectGraph&) = delete;
		ExtensionObjectGraph& operator=(const ExtensionObjectGraph&) = delete;
		ExtensionObjectGraph(ExtensionObjectGraph&&) = delete;
		ExtensionObjectGraph& operator=(ExtensionObjectGraph&&) = delete;

		// The Message Dispatcher, for `bind_envelope_router`. This is requirement 2.4's
		// slot, and handing it over is the whole point of the class.
		transport::MessageDispatcher<InboundEnvelopeType, JsonValue>& message_dispatcher()
		{
			return message_dispatcher_;
		}

		// `MainThreadDispatcher::UiStatePublisher`'s slot (requirement 2.3).
		std::function<void()> ui_state_publisher()
		{
			return [this] { ui_state_publisher_.publish(); };
		}

		// What `make_panel_browser_creator` binds the CEF-backed host into, and what
		// the UI Host publishes through until it does.
		ui::DeferredBrowserHost& deferred_browser_host() { return deferred_browser_host_; }

		ui::UiHost& ui_host() { return ui_host_; }

		// The Transport Client's seam, which the network thread presents into.
		ui::DeferredConnectionStatePresenter& connection_state_presenter()
		{
			return connection_state_presenter_;
		}

		// The Context Builder, for the two publications that are not answers to a
		// request: on connect, and on the timer's debounced project-change poll.
		context::ProjectContextBuilder<JsonValue, OutboundEnvelopeType>& project_context_builder()
		{
			return project_context_builder_;
		}

		ui::StreamPresenter& stream_presenter() { return stream_presenter_; }

		// The 42 minus what registered, and anything that was refused. Read once at
		// load and written down — see `compose_tool_handler_registry`.
		const ToolRegistryCompositionReport& tool_registry_report() const
		{
			return tool_registry_report_;
		}

		const Sink& dispatcher_sink() const { return dispatcher_sink_; }

		const ExecutorToolCallRouter<InboundEnvelopeType, JsonValue>& tool_call_router() const
		{
			return tool_calls_;
		}

		const QueuedToolResultSink<OutboundEnvelopeType, JsonValue>& tool_result_sink() const
		{
			return tool_results_;
		}

		const ui::BridgeConfirmationPresenter& confirmation_presenter() const
		{
			return confirmation_presenter_;
		}

		const ui::BridgeUiStatePublisher& ui_state_publication_tally() const
		{
			return ui_state_publisher_;
		}

	private:
		OutboundQueue& outbound_queue_;

		// ---- the UI half ----
		//
		// First, because the UI Host is what three of the seven destinations terminate
		// in and because the browser arrives later than everything else here.
		ui::DeferredBrowserHost deferred_browser_host_;

		// One reference in this class runs backwards: the sink holds
		// `confirmation_coordinator_`, declared below it.
		//
		// The cycle is real and is not an accident of ordering — the UI Host holds the
		// sink, the sink answers a confirmation decision locally, the coordinator
		// presents through the UI Host. Something has to be bound before it is
		// constructed, and a reference bound to a member's storage is well-defined
		// provided nothing reads through it until construction finishes. Nothing does:
		// the only use is inside `accept_message_from_javascript`, which cannot run
		// before the graph exists, because the only thing that calls it is a CEF browser
		// that is created later still.
		//
		// The alternative was a settable coordinator on the sink, which trades a
		// documented one-line ordering constraint for a slot that is empty at runtime
		// and has to be checked on every message.
		Sink dispatcher_sink_;

		ui::UiHost ui_host_;

		ui::StreamPresenter stream_presenter_;
		ui::BridgeConfirmationPresenter confirmation_presenter_;
		transport::ConfirmationCoordinator<OutboundEnvelopeType> confirmation_coordinator_;
		ui::DeferredConnectionStatePresenter connection_state_presenter_;
		ui::BridgeUiStatePublisher ui_state_publisher_;

		// ---- the DAW half ----
		context::ProjectContextBuilder<JsonValue, OutboundEnvelopeType> project_context_builder_;
		transport::TransportHandler<OutboundEnvelopeType> transport_handler_;

		daw::tool_handler_registry<JsonValue> tool_handler_registry_;
		daw::tool_executor_of<JsonValue> tool_executor_;

		// ---- the seven destinations ----
		ExecutorToolCallRouter<InboundEnvelopeType, JsonValue> tool_calls_;
		QueuedToolResultSink<OutboundEnvelopeType, JsonValue> tool_results_;
		CoordinatorConfirmationRouter<InboundEnvelopeType, OutboundEnvelopeType> confirmations_;
		BuilderProjectContextRouter<JsonValue, OutboundEnvelopeType> project_context_;
		HandlerTransportCommandRouter<InboundEnvelopeType, OutboundEnvelopeType> transport_commands_;
		PresenterStreamRouter<InboundEnvelopeType> streams_;
		UiHostScriptDownloadRouter<InboundEnvelopeType, JsonValue> script_downloads_;

		transport::MessageDispatcher<InboundEnvelopeType, JsonValue> message_dispatcher_;

		ToolRegistryCompositionReport tool_registry_report_;
	};

}

#endif
