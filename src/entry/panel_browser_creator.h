// Where the CEF-backed browser is installed into the startup plan (requirements 1.3,
// 15.1).
//
// `PluginEntry` reaches the browser through `PanelBrowserCreator` — a
// `std::function<bool(void*)>` — and `plugin_entry.cpp` deliberately leaves it unset,
// because naming `CefPanelBrowserHost` from the entry point would turn a CEF-less build
// from "no UI host" into "no library at all". This is the seam that fills it, and the
// two translation units behind it are the only place in the extension that names CEF
// outside `src/ui/cef_browser_host.cpp`.
//
// ---------------------------------------------------------------------------
// One declaration, two definitions, and CMake picks
//
// `make_panel_browser_creator` is declared here and defined twice:
//
//   - `entry/cef_panel_browser_creator.cpp` names `CefPanelBrowserHost`, initializes
//     CEF, and returns a creator that builds the browser in REAPER's panel.
//   - `entry/panel_browser_creator_without_cef.cpp` returns an empty creator and a
//     reason, so `PluginEntry::open_panel` reports `no_browser_creator` and the panel
//     registration is simply not taken.
//
// `CMakeLists.txt` compiles exactly one of them, selected by whether a CEF distribution
// was located — the same switch that already drops `src/ui/cef_browser_host.cpp`. A
// linker error is what would happen if that switch were wrong, which is the failure
// worth having: one definition or the other, never both and never neither.
//
// Note that `ui/cef_browser_host.h` itself includes no CEF — every CEF type is behind an
// incomplete `cef_state`. So the CEF-naming unit here compiles without a distribution
// and only fails to *link* without one, which is precisely why the exclusion has to be
// a CMake decision rather than a preprocessor one. A `#ifdef` would compile happily and
// produce a library with an undefined symbol in it.
//
// ---------------------------------------------------------------------------
// What this does not do
//
// It does not create the panel window. `DockWindowAddEx` takes an `HWND`, and producing
// one needs a dialog resource plus the SWELL resource-generation step on macOS and
// Linux — a `CMakeLists.txt` change and packaging work that belongs with task 21.x.
// `LoadedExtension::register_panel_window` already takes the registration when there is
// a window to register; the creator below is what fills that window once there is one.

#ifndef SESH_AI_ENTRY_PANEL_BROWSER_CREATOR_H
#define SESH_AI_ENTRY_PANEL_BROWSER_CREATOR_H

#include <memory>
#include <string>

#include <entry/plugin_entry.h>

// For `CefRuntimeLayout` and nothing else. That header includes no CEF — every CEF type
// in it is behind an incomplete `cef_state` — so this one stays compilable on a machine
// with no distribution, which is what the suite needs.
#include <ui/cef_browser_host.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

namespace sesh_ai::entry {

	// What a browser host is built over: where the application's assets are, and where
	// the CEF runtime pieces were installed.
	//
	// Plain strings, resolved by the caller from REAPER's resource path. This header
	// reaches no filesystem library, for the same reason `CefRuntimeLayout` does not.
	struct PanelBrowserSettings {
		// The directory holding Vite's output. `UiHost::create_browser` is handed
		// `<this>/index.html` as a `file://` URL, built by `build_local_asset_url`,
		// which refuses a relative path, a path with a scheme, and a path with a parent
		// directory segment.
		std::string application_asset_directory;

		// Where the CEF helper, resource, and locale directories live. Empty members
		// are what a platform that does not need one looks like — macOS keeps resources
		// inside the framework bundle and finds them itself.
		ui::CefRuntimeLayout runtime_layout;
	};

	// What a creator holds between being installed and being called.
	//
	// The browser host cannot be built when the creator is — it needs the panel's
	// native window handle, which arrives as the creator's argument — so this is where
	// it goes when it finally exists, and the creator captures it by `shared_ptr` so
	// that the state survives the creator being copied into `StartupPlan` and the
	// installation being returned by value. A lambda capturing the installation it is a
	// member of would dangle the moment the installation moved.
	//
	// Held as `ui::BrowserHost` rather than as `CefPanelBrowserHost`, so this header
	// does not name the CEF-backed type. That is the point of the whole arrangement:
	// everything except the one translation unit behind it talks to a `BrowserHost`.
	struct PanelBrowserState {
		std::unique_ptr<ui::BrowserHost> browser_host;

		// Why the most recent attempt failed, when one did. Empty otherwise. Written by
		// the creator and read by the plugin entry for the log line — the creator's own
		// return is a bool, which cannot carry a reason.
		std::string last_failure_reason;
	};

	// What installing the creator produced.
	struct PanelBrowserInstallation {
		PanelBrowserCreator creator;

		// Shared with the creator. Never null — a build with no CEF still gets a state,
		// it just never gets a host put in it.
		std::shared_ptr<PanelBrowserState> state{std::make_shared<PanelBrowserState>()};

		// Empty when a creator was installed. Otherwise the line the plugin entry logs
		// — "this build has no CEF distribution" is information, not a fault, and the
		// panel reporting `no_browser_creator` is the honest outcome.
		std::string unavailable_reason;

		bool installed() const { return static_cast<bool>(creator); }
	};

	// Builds the creator that puts a CEF browser in the panel.
	//
	// `ui_host` and `deferred_browser_host` must both outlive the creator, and the
	// second is how the ordering works: the UI Host is constructed at load over a
	// `DeferredBrowserHost`, and the creator binds the real host into it when the panel
	// opens. `ui/deferred_browser_host.h` records why that is the arrangement rather
	// than a settable host or a conditionally-constructed UI Host.
	//
	// The creator calls `UiHost::create_browser` rather than the browser host directly,
	// so the refusals `UiHost` already owns — a second create, a create after CEF shut
	// down, a URL that is not a local asset — apply here rather than being re-decided.
	//
	// Defined in exactly one of the two translation units named in the file comment.
	PanelBrowserInstallation make_panel_browser_creator(
		ui::UiHost& ui_host,
		ui::DeferredBrowserHost& deferred_browser_host,
		const PanelBrowserSettings& settings
	);

}

#endif
