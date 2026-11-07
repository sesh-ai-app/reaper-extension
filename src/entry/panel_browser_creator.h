// Where the CEF-backed browser is installed into the startup plan (requirements 1.3,
// 15.1).
//
// `PluginEntry` reaches the browser through `PanelBrowserCreator` — a
// `std::function<bool(void*)>` — and `plugin_entry.h` never names what fills it,
// because the Catch2 suite compiles that header and has no CEF distribution to link
// against. This is the seam that fills it. `make_panel_browser_creator` is declared
// here and defined in `entry/cef_panel_browser_creator.cpp`, which with
// `src/ui/cef_browser_host.cpp` is the only place in the extension that names the
// CEF-backed host. The library always compiles both; the suite compiles neither, and
// drives `install_panel_browser_creator` below through a `PanelBrowserCreatorFactory`
// instead.
//
// Note that `ui/cef_browser_host.h` itself includes no CEF — every CEF type is behind an
// incomplete `cef_state` — which is what lets this header, and the suite, include it.
//
// ---------------------------------------------------------------------------
// What this does not do
//
// It does not create the panel window. That is `entry/panel_window.h`, over the dialog
// resource and the SWELL resource-generation step task 25.1 added. Creating the window
// when the panel action fires, docking it, and handing it to the creator is
// `entry/dockable_panel.h` (task 25.2), and `install_panel_browser_creator` below is
// where the plugin entry decides whether there is a creator to hand it to.

#ifndef SESH_AI_ENTRY_PANEL_BROWSER_CREATOR_H
#define SESH_AI_ENTRY_PANEL_BROWSER_CREATOR_H

#include <functional>
#include <memory>
#include <string>

#include <entry/cef_framework_location.h>
#include <entry/cef_subprocess_helper_location.h>
#include <entry/panel_asset_directory.h>
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
	// Plain strings, resolved by the caller. The asset directory is found beside the
	// extension binary — `entry/panel_asset_directory.h` says why there rather than
	// under REAPER's resource path. This header reaches no filesystem library, for the
	// same reason `CefRuntimeLayout` does not.
	struct PanelBrowserSettings {
		// The directory holding Vite's output. `UiHost::create_browser` is handed
		// `<this>/index.html` as a `file://` URL, built by `build_local_asset_url`,
		// which refuses a relative path, a path with a scheme, and a path with a parent
		// directory segment.
		std::string application_asset_directory;

		// Where the CEF helper, resource, and locale directories live. Empty members
		// are what a platform that does not need one looks like — macOS keeps resources
		// inside the framework bundle and finds them itself. The subprocess path is
		// found beside the binary by `entry/cef_subprocess_helper_location.h`.
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

		// `CefPanelBrowserHost::pump_message_loop`, for the timer tick to call. Empty until
		// a host exists, so for good when no creator was installed.
		//
		// A callable rather than a method on `ui::BrowserHost`, because pumping is CEF's
		// concern and not part of what the UI Host asks of a browser. It is the step that
		// hands messages from JavaScript to `UiHost::receive_from_javascript` on REAPER's
		// main thread, and on macOS and Linux the step that runs CEF's own loop at all, so
		// without it nothing the producer sends from the panel is ever read.
		//
		// Captures the host by raw pointer. The tick stops at teardown step one and the
		// host is destroyed at step three, and the plugin entry clears this before it
		// resets `browser_host` regardless.
		std::function<void()> pump_browser_message_loop;

		// Why the most recent attempt failed, when one did. Empty otherwise. Written by
		// the creator and read by the plugin entry for the log line — the creator's own
		// return is a bool, which cannot carry a reason.
		std::string last_failure_reason;
	};

	// What installing the creator produced.
	struct PanelBrowserInstallation {
		PanelBrowserCreator creator;

		// Shared with the creator. Never null — an installation without a creator still
		// gets a state, it just never gets a host put in it.
		std::shared_ptr<PanelBrowserState> state{std::make_shared<PanelBrowserState>()};

		// Empty when a creator was installed. Otherwise the line the plugin entry logs:
		// what was missing, with the path it was looked for at where there is one, so
		// the panel reporting `no_browser_creator` comes with what to put in place.
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
	// Defined in `entry/cef_panel_browser_creator.cpp`, which the suite does not compile.
	PanelBrowserInstallation make_panel_browser_creator(
		ui::UiHost& ui_host,
		ui::DeferredBrowserHost& deferred_browser_host,
		const PanelBrowserSettings& settings
	);

	// `make_panel_browser_creator` with its two references already bound, so the
	// decision below can be driven by the suite without the CEF-backed definition.
	using PanelBrowserCreatorFactory =
		std::function<PanelBrowserInstallation(const PanelBrowserSettings& settings)>;

	// Installs the creator only when there is something for the browser to load (task
	// 25.2, requirement 15.1) and a helper for CEF to start it with (task 26.1,
	// requirement 15.7).
	//
	// The factory is called even when the assets were not found, so that its own
	// refusal is not hidden behind theirs. The CEF-backed factory declines when the
	// asset directory cannot be made into a `file://` URL — empty, relative, or holding a
	// control character or a `..` segment — and that reason leads, with missing assets
	// added to it rather than replacing it. A developer reading the log then learns both
	// things in one line instead of fixing one and meeting the other.
	//
	// A missing helper or framework is not added to that line. All three paths are
	// derived from the same extension binary location, so a directory that cannot be
	// addressed is the cause and their absence most likely a consequence of it.
	//
	// When the factory installs a creator and the assets, the helper, or CEF's library
	// are not there, the creator is dropped rather than installed. It would initialize
	// CEF — which a process cannot undo — and then show a Chromium error page in the
	// panel, or have no executable to start the renderer from. Dropping it costs
	// nothing: the CEF-backed creator does no work until it is called.
	//
	// CEF's library is the third thing checked, the same way: the framework on macOS
	// (task 26.2) and `libcef.dll` on Windows (task 32.2). Without the framework
	// `cef_load_library` would fail, which is recoverable, but a main bundle with no
	// Info.plist would stop REAPER in a Chromium `CHECK`, which is not. Without
	// `libcef.dll` the browser host's full-path load would fail and be reported too, so
	// this only moves that report from the panel opening to the extension loading, where
	// it joins the others. On Linux the resolution reports that nothing is needed, so it
	// never drops a creator there.
	//
	// The CEF runtime layout is built here from the resolutions rather than passed in,
	// so a path and the decision about whether it exists cannot come from two places.
	// The resource, locale, and cache paths stay empty: CEF's defaults find the first
	// two in this layout (`ui/cef_browser_host.h` says how), and the cache path is
	// task 27.1's.
	inline PanelBrowserInstallation install_panel_browser_creator(
		const ApplicationAssetDirectoryResolution& application_assets,
		const CefSubprocessHelperResolution& subprocess_helper,
		const CefFrameworkResolution& framework,
		const PanelBrowserCreatorFactory& make_creator)
	{
		if (!make_creator) {
			PanelBrowserInstallation installation;
			installation.unavailable_reason =
				"the extension's object graph was not composed, so there is no UI Host to put "
				"a browser behind";

			return installation;
		}

		PanelBrowserSettings settings;

		// The derived path even when nothing was found there. An empty one would make
		// the CEF-backed factory refuse with "the asset directory is empty", which is
		// true and says nothing about where it looked.
		settings.application_asset_directory = application_assets.asset_directory;

		// The derived paths even when nothing was found there, for the same reason.
		settings.runtime_layout.subprocess_path = subprocess_helper.subprocess_path;
		settings.runtime_layout.framework_library_path = framework.framework_library_path;
		settings.runtime_layout.framework_directory = framework.framework_directory;
		settings.runtime_layout.main_bundle_path = framework.main_bundle_path;

		PanelBrowserInstallation installation = make_creator(settings);

		if (!installation.installed()) {
			if (!application_assets.found) {
				installation.unavailable_reason.append(" Separately, ");
				installation.unavailable_reason.append(application_assets.unavailable_reason);
			}

			return installation;
		}

		if (application_assets.found && subprocess_helper.found && framework.found) {
			return installation;
		}

		// Every reason that applies, assets first, so one log line names every file a
		// developer has to put in place.
		installation.creator = nullptr;
		installation.unavailable_reason.clear();

		const auto append_reason = [&installation](const std::string& reason) {
			if (!installation.unavailable_reason.empty()) {
				installation.unavailable_reason.append(" Separately, ");
			}

			installation.unavailable_reason.append(reason);
		};

		if (!application_assets.found) {
			append_reason(application_assets.unavailable_reason);
		}

		if (!subprocess_helper.found) {
			append_reason(subprocess_helper.unavailable_reason);
		}

		if (!framework.found) {
			append_reason(framework.unavailable_reason);
		}

		return installation;
	}

}

#endif
