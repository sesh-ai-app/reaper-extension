// The CEF-backed half of `make_panel_browser_creator`.
//
// One of two definitions; `entry/panel_browser_creator_without_cef.cpp` is the other,
// and `CMakeLists.txt` compiles exactly one — this one when a CEF distribution was
// located. The header explains why that is a CMake decision rather than a preprocessor
// one.
//
// This is the only translation unit outside `src/ui/cef_browser_host.cpp` that names
// `CefPanelBrowserHost`, which is the whole reason it is a file of its own rather than
// part of `plugin_entry.cpp` or `extension_composition.cpp`. Either of those would make
// the extension's library unbuildable without CEF rather than merely CEF-less.
//
// ---------------------------------------------------------------------------
// Not verified by anything on the machine this was written on
//
// There is no CEF distribution here, so this file has been read and syntax-checked and
// nothing more. It has not been linked, because linking needs the distribution, and it
// has not been run, because running needs REAPER. What it does is translation — build a
// host, initialize CEF, bind it, hand `UiHost` a `file://` URL — and every decision it
// could have made instead lives behind `UiHost` and `DeferredBrowserHost`, both of
// which the suite drives against recorders. Treat the ordering below as unverified
// until the packaging work builds against a real distribution.

#include <entry/panel_browser_creator.h>

#include <ui/cef_browser_host.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

#include <memory>
#include <string>
#include <utility>

namespace sesh_ai::entry {

	namespace {

		// The reason line for a CEF that refused to initialize.
		//
		// `CefPanelBrowserHost::failure_reason()` is the only thing that knows which of
		// the several reasons applied — no distribution at runtime, a second
		// initialization in one process, a helper it could not launch — so it is quoted
		// rather than re-described.
		std::string cef_initialization_failure_reason(const ui::CefPanelBrowserHost& host)
		{
			std::string reason{"the CEF browser could not be initialized"};

			if (!host.failure_reason().empty()) {
				reason.append(": ");
				reason.append(host.failure_reason());
			}

			return reason;
		}

	}

	PanelBrowserInstallation make_panel_browser_creator(
		ui::UiHost& ui_host,
		ui::DeferredBrowserHost& deferred_browser_host,
		const PanelBrowserSettings& settings)
	{
		PanelBrowserInstallation installation;

		// The `file://` URL is built before anything is created, because
		// `build_local_asset_url` is the one thing here that can refuse for a reason
		// worth reporting — a relative directory, a path carrying a scheme, a parent
		// directory segment — and it costs nothing to find that out before CEF is
		// initialized in a process that cannot un-initialize it.
		const ui::LocalAssetUrlResult application_entry_point = ui::build_local_asset_url(
			settings.application_asset_directory,
			ui::default_application_entry_point
		);

		if (!application_entry_point.valid) {
			installation.unavailable_reason =
				std::string{"the React application's assets could not be addressed: "}
					+ application_entry_point.rejection_reason;

			return installation;
		}

		installation.creator = [
			&ui_host,
			&deferred_browser_host,
			state = installation.state,
			runtime_layout = settings.runtime_layout,
			application_entry_point_url = application_entry_point.url
		](void* parent_window_handle) -> bool {
			if (parent_window_handle == nullptr) {
				state->last_failure_reason = "REAPER supplied no panel window";

				return false;
			}

			auto host = std::make_unique<ui::CefPanelBrowserHost>(
				parent_window_handle,
				runtime_layout
			);

			// `CefInitialize` is process-wide and cannot be undone, so it happens before
			// anything is bound and a failure leaves the deferred host unbound rather
			// than bound to a host that never came up.
			if (!host->initialize_cef()) {
				state->last_failure_reason = cef_initialization_failure_reason(*host);

				return false;
			}

			// Messages from JavaScript go to `UiHost::receive_from_javascript`, which is
			// what puts them through the dispatcher. Set before the browser is created,
			// so a message from a page that loads immediately has somewhere to go.
			host->set_message_from_javascript_handler(
				[&ui_host](std::string message_name, ui::SerializedBridgePayload payload) {
					ui_host.receive_from_javascript(std::move(message_name), std::move(payload));
				}
			);

			// Bound before the browser is created, because `UiHost::create_browser`
			// reaches the host through this and an unbound one refuses. `bind` returns
			// false for a second call, which is what a second `open_panel` getting past
			// `PluginEntry`'s own guard would look like — reported rather than replacing
			// a running browser nothing could then reach.
			if (!deferred_browser_host.bind(host.get())) {
				state->last_failure_reason = "a panel browser is already bound";

				// Not kept. Letting it go out of scope here destroys a host whose CEF is
				// initialized, which is the lesser of the two problems: the alternative
				// is holding a second host the extension has no route to.
				return false;
			}

			// Ownership moves into the shared state, which the plugin entry holds for as
			// long as the extension is loaded. It has to outlive the bind above, and the
			// bind is why it cannot simply be a local.
			state->browser_host = std::move(host);

			const ui::CreateBrowserOutcome outcome =
				ui_host.create_browser(application_entry_point_url);

			if (!outcome.created) {
				state->last_failure_reason =
					std::string{"the panel browser was refused: "}
						+ std::string{ui::describe(outcome.refusal)};
			}

			return outcome.created;
		};

		return installation;
	}

}
