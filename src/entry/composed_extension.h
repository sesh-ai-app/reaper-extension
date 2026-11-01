// What the plugin entry holds once the graph is real (task 24.3, requirements 1.1, 2.4,
// 5.1, 5.2, 9.1, 15.4, 22.1).
//
// `entry/extension_composition.h` is the composition root's *logic*: which object goes
// in which slot, and what is reported when one is absent. It is a header templated on
// the payload and both envelope types precisely so that `tests/CMakeLists.txt`, which
// does not compile `src/`, can drive the whole graph against stubs.
//
// This header is the other end of that split. It names one instantiation —
// `nlohmann::json` with the two real envelopes — and declares the handle
// `plugin_entry.cpp` reaches it through. `extension_composition.cpp` is the one
// translation unit that constructs it, which is also the one place the 42 tools' readers
// and writers are bound to the JSON library and the nine REAPER-backed adapters are
// constructed.
//
// ---------------------------------------------------------------------------
// Why a handle rather than the graph itself
//
// `ExtensionObjectGraph` is a class template with twenty-odd members, each of which
// pulls its own component's header. Naming it in `plugin_entry.cpp` would put every one
// of those in the one translation unit that also includes the REAPER SDK — where WDL's
// swell defines `min` and `max` as function-like macros and any header parsed after it
// that calls `std::min` is mis-parsed. The ordering that avoids that is already
// load-bearing in `plugin_entry.cpp` and documented there; an abstract handle keeps the
// surface it has to get right to five functions instead of a graph.
//
// It also keeps the construction order in one place. The graph's reference topology has
// a documented backward edge in it and a documented lifetime constraint against the
// queues; both belong beside the thing they constrain, not in the entry point.
//
// ---------------------------------------------------------------------------
// What this does not own
//
// The queue pair, the timer registration, the log, and REAPER's registrations all stay
// the plugin entry's: they are what requirement 1.2's teardown order is *about*, and the
// order is `plugin_entry.h`'s subject. The graph is handed the outbound queue by
// reference and never the pair.
//
// Nor the panel window or the browser. `make_panel_browser_creator`
// (`entry/panel_browser_creator.h`) takes the UI Host and the deferred browser host,
// both of which this exposes — but it also needs the panel's `HWND` and the resolved
// asset directory, which are task 25's. So the creator is installed by whoever has
// those, over the two references below.

#ifndef SESH_AI_ENTRY_COMPOSED_EXTENSION_H
#define SESH_AI_ENTRY_COMPOSED_EXTENSION_H

#include <functional>
#include <memory>

#include <context/project_context_builder.h>
#include <entry/extension_composition.h>
#include <transport/envelope.h>
#include <transport/message_dispatcher.h>
#include <transport/queue_pair.h>
#include <ui/bridge_publishers.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`, so naming the struct here is a forward declaration of the same
// type. Held and passed on, never dereferenced here — the same arrangement every
// `reaper_*_host.h` in this tree uses, and what keeps this header out of the SDK's way.
struct reaper_plugin_info_t;

namespace sesh_ai::entry {

	// The one instantiation the extension actually runs: nlohmann/json, and the two
	// envelopes `transport/envelope.h` declares.
	using ReaperExtensionObjectGraph = ExtensionObjectGraph<
		nlohmann::json,
		transport::InboundEnvelope,
		transport::OutboundEnvelope
	>;

	using ReaperMessageDispatcher =
		transport::MessageDispatcher<transport::InboundEnvelope, nlohmann::json>;

	using ReaperProjectContextBuilder =
		context::ProjectContextBuilder<nlohmann::json, transport::OutboundEnvelope>;

	// The composed graph, behind the five things the plugin entry does with it.
	//
	// Abstract with one implementation, rather than the graph returned by value, for the
	// reasons in the file comment. Each accessor is one the entry point calls exactly
	// once, at load or at teardown.
	class ComposedExtension {
	public:
		virtual ~ComposedExtension() = default;

		ComposedExtension(const ComposedExtension&) = delete;
		ComposedExtension& operator=(const ComposedExtension&) = delete;
		ComposedExtension(ComposedExtension&&) = delete;
		ComposedExtension& operator=(ComposedExtension&&) = delete;

		// Requirement 2.4's slot, for `bind_envelope_router`. The binding happens in
		// `plugin_entry.cpp` because that is where the log is, and the log line is
		// written inside the binding — see `entry/extension_log.h`.
		virtual ReaperMessageDispatcher& message_dispatcher() = 0;

		// `MainThreadDispatcher::UiStatePublisher`'s slot (requirement 2.3). Everything
		// the presenters accumulated, published to CEF inside the tick and nowhere else.
		virtual std::function<void()> ui_state_publisher() = 0;

		// The two references `make_panel_browser_creator` takes.
		virtual ui::UiHost& ui_host() = 0;
		virtual ui::DeferredBrowserHost& deferred_browser_host() = 0;

		// The Transport Client's seam, presented into from the network thread.
		virtual ui::DeferredConnectionStatePresenter& connection_state_presenter() = 0;

		// The Context Builder, for the two publications that are not answers to a
		// request: on connect, and on the tick's debounced project-change poll.
		virtual ReaperProjectContextBuilder& project_context_builder() = 0;

		// The 42 minus what registered, and anything that was refused. Read once at
		// load and written down — a known gap beats a producer discovering it one
		// unknown-tool error at a time.
		virtual const ToolRegistryCompositionReport& tool_registry_report() const = 0;

		// Calls that could not be dispatched because this build had no codec. Zero in
		// any build that configured one, which `tests/entry/tool_payload_codecs_test.cpp`
		// is what holds true.
		virtual std::size_t unavailable_tool_call_count() const = 0;

	protected:
		ComposedExtension() = default;
	};

	// What the graph is built over, from the plugin entry's side.
	//
	// Three things, because everything else the graph needs is a REAPER-backed adapter it
	// constructs itself from `plugin_info`.
	struct ComposedExtensionDependencies {
		// The pointer REAPER passed to `ReaperPluginEntry`. Valid for the lifetime of
		// the loaded extension, which outlives the graph. Null is accepted and produces
		// a graph whose hosts resolve nothing — which is a graph that reports every
		// REAPER call as unavailable rather than one that cannot be built.
		reaper_plugin_info_t* plugin_info = nullptr;

		// The network thread's half of the `QueuePair`, owned by the plugin entry.
		//
		// Must outlive every use of the graph, which is a narrower claim than "must
		// outlive the graph": requirement 1.2 destroys the queues at teardown step two
		// and shuts down CEF at step three, so the graph's reference is dead for the
		// width of one step. Nothing reads it there — the timer was unregistered at step
		// one, so the only writer is gone — and `plugin_entry.cpp` records that at the
		// step itself.
		transport::ConcurrentQueue<transport::OutboundEnvelope>* outbound_queue = nullptr;

		// `MainThreadDispatcher::request_ui_state_publication`, which is documented safe
		// from any thread. The Transport Client presents a connection status from the
		// network thread and this is how the publish reaches the tick.
		ui::UiStatePublicationRequest request_ui_state_publication;
	};

	// Builds the whole graph, bound to nlohmann/json and the REAPER C API.
	//
	// Null for `outbound_queue` is the one refusal: every one of the seven destinations
	// either pushes onto it or holds it, so a graph without it could be constructed and
	// could not answer anything. A null `plugin_info` is not a refusal — see the field's
	// comment.
	//
	// Defined in `entry/extension_composition.cpp`.
	std::unique_ptr<ComposedExtension> compose_extension(
		ComposedExtensionDependencies dependencies
	);

}

#endif
