// The CEF side of the UI seam.
//
// This header deliberately does not include CEF. Every CEF type is held inside
// `cef_state`, which is declared here and defined in the translation unit, so
// everything except `cef_browser_host.cpp` can talk about a CEF-backed browser host
// without a CEF distribution on its include path — and the Catch2 suite, which has
// none, still compiles.
//
// Same shape as `daw/reaper_render_host.h` and `entry/reaper_timer_registrar.h`: the
// interface and this header stay dependency-free, and one `*.cpp` is where the library
// lives. The interface itself, and all of this component's logic, is in `ui/ui_host.h`.
//
// ---------------------------------------------------------------------------
// The extension is a shared library, which decides most of what is below
//
// REAPER loads `reaper_sesh_ai.dylib` / `.dll` / `.so` into its own process, and that
// single fact drives the awkward parts:
//
//   - **The subprocess executable is always ours.** CEF's default way of starting a
//     renderer is to re-execute the host binary with process-type arguments. The host
//     binary is REAPER. So `browser_subprocess_path` is set on all three platforms, to
//     a small helper the extension ships — see `CefPlatformConfiguration`.
//   - **CEF is initialized once per process and never again.** `CefInitialize` and
//     `CefShutdown` are process-wide, and REAPER may load and unload an extension
//     during one run. So this object refuses a second initialization rather than
//     attempting one, and `shut_down_cef` is idempotent.
//   - **The Windows sandbox cannot be enabled from here.** It has to be set up in the
//     host executable's entry point, which belongs to REAPER.
//   - **The message loop differs per platform.** Windows supports CEF's
//     multi-threaded message loop, which is what keeps the panel responsive
//     independently of REAPER's main thread (requirements 15.5, 22.4). macOS and Linux
//     do not, so they use the external message pump and `CefDoMessageLoopWork` driven
//     from the extension's timer. Both arrangements leave the browser in its own
//     process; the difference is only which thread runs CEF's own loop.
//
// ---------------------------------------------------------------------------
// Not verified by anything
//
// There is no CEF distribution on the development machine and none in the test target,
// so nothing here is compiled or run by the suite. What is asserted about it lives in
// `ui/ui_host.h` — the platform configuration, the URL rules, the teardown order, and
// the escaping of a payload on its way into the page. This file is the translation
// layer, and it is deliberately dull, because it is the one part of this component the
// suite cannot reach. Treat every claim in it as unverified until the packaging work
// builds against a real distribution.

#ifndef SESH_AI_UI_CEF_BROWSER_HOST_H
#define SESH_AI_UI_CEF_BROWSER_HOST_H

#include <ui/ui_host.h>

#include <functional>
#include <memory>
#include <string>

namespace sesh_ai::ui {

	// Where the runtime pieces of a CEF distribution were installed, alongside the
	// extension binary. Filled in by the plugin entry from REAPER's resource path; kept
	// as plain strings so this header stays free of any filesystem library.
	struct CefRuntimeLayout {
		// The helper the renderer, GPU, and utility processes run as. Joined from the
		// installation directory and `CefPlatformConfiguration::subprocess_executable_name`.
		std::string subprocess_path;

		// CEF's `Resources` directory — `.pak` files and `icudtl.dat`. Left empty on
		// macOS, where they are inside the framework bundle and CEF finds them itself.
		std::string resource_directory;

		// CEF's `locales` directory. Empty on macOS for the same reason.
		std::string locales_directory;

		// Where Chromium may write its cache. Under REAPER's own resource path, so an
		// uninstall takes it with it.
		std::string cache_path;

		// Linux only: the setuid sandbox helper.
		std::string sandbox_helper_path;
	};

	// The `BrowserHost` CEF actually backs.
	//
	// Named for the panel rather than for CEF, because CEF's own API already has a
	// `CefBrowserHost` — the type `CefBrowser::GetHost()` returns, and the one that
	// carries `CreateBrowser` and `CloseBrowser`. Two types of one name inside one
	// translation unit is a trap, and the trap springs as a confusing overload error
	// rather than as a name clash.
	class CefPanelBrowserHost final : public BrowserHost {
	public:
		// `parent_window_handle` is the native handle of REAPER's dockable panel — an
		// `HWND` on Windows and a SWELL `HWND` on macOS and Linux. Held as `void*` so
		// this header needs neither `windows.h` nor swell.
		CefPanelBrowserHost(
			void* parent_window_handle,
			CefRuntimeLayout runtime_layout,
			CefPlatformConfiguration platform_configuration =
				cef_platform_configuration_for(current_host_platform())
		);

		// Declared here and defined in the translation unit, because `cef_state` is
		// incomplete at this point and `std::unique_ptr` needs it complete to destroy.
		~CefPanelBrowserHost() override;

		CefPanelBrowserHost(const CefPanelBrowserHost&) = delete;
		CefPanelBrowserHost& operator=(const CefPanelBrowserHost&) = delete;
		CefPanelBrowserHost(CefPanelBrowserHost&&) = delete;
		CefPanelBrowserHost& operator=(CefPanelBrowserHost&&) = delete;

		// Calls `CefInitialize`. False when it failed or when CEF is already initialized
		// in this process; `failure_reason()` says which. Must be called before
		// `create_browser`.
		bool initialize_cef();

		bool cef_initialized() const;

		// Empty until something failed. The plugin entry logs it and shows the panel an
		// error rather than an empty browser.
		const std::string& failure_reason() const;

		// Call from the main-thread timer tick, on every platform.
		//
		// Two jobs. It hands any messages that arrived from JavaScript to the handler,
		// which is what keeps them on REAPER's main thread instead of a CEF thread. And
		// on macOS and Linux it pumps CEF's own loop, which those platforms need because
		// they do not support the multi-threaded message loop Windows uses.
		void pump_message_loop();

		// Where a message from JavaScript goes. Set by the plugin entry to
		// `UiHost::receive_from_javascript`. The handler is invoked on the main thread,
		// never on a CEF thread.
		//
		// The signature is the reason requirement 15.4 holds in this direction: the only
		// thing this object can hand over is a name and a `SerializedBridgePayload`.
		using MessageFromJavascriptHandler =
			std::function<void(std::string message_name, SerializedBridgePayload payload)>;

		void set_message_from_javascript_handler(MessageFromJavascriptHandler handler);

		// The function the React application installs on `window` and the extension
		// calls into. One name, stated once, so the C++ and TypeScript sides cannot
		// disagree about it.
		static const char* javascript_bridge_receive_function();

		// The function the React application calls to send a message back. Registered as
		// a CEF JavaScript binding on the renderer side.
		static const char* javascript_bridge_send_function();

		// ------------------------------------------------------------------
		// BrowserHost
		// ------------------------------------------------------------------

		bool create_browser(const std::string& local_asset_url) override;
		bool navigate(const std::string& absolute_url) override;

		bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json
		) override;

		void close_browser() override;
		void shut_down_cef() override;

		// The CEF objects. Declared but not defined here, so the types its members are
		// made of never reach this header. Public only because the translation unit's own
		// helpers name it; nothing outside can do anything with an incomplete type.
		struct cef_state;

	private:
		std::unique_ptr<cef_state> state_;
		void* parent_window_handle_;
		CefRuntimeLayout runtime_layout_;
		CefPlatformConfiguration platform_configuration_;
		MessageFromJavascriptHandler message_from_javascript_handler_;
		std::string failure_reason_;
		bool cef_initialized_ = false;
		bool cef_shut_down_ = false;
	};

}

#endif
