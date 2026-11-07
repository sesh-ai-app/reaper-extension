// The one translation unit that includes CEF.
//
// Keeping CEF here is what makes the UI Host's logic testable without a CEF
// distribution: the suite substitutes a `BrowserHost` and never links this file. The
// same arrangement as `entry/reaper_timer_registrar.cpp` for the REAPER SDK.
//
// ---------------------------------------------------------------------------
// Compiled, not exercised
//
// The shared library always compiles this against the distribution `CMakeLists.txt`
// pins (154.0.28+g564dd6c+chromium-154.0.8037.58), so the compiler holds it to CEF's
// API at that version. Nothing in the suite runs it, and nothing will notice if what it
// does at run time is wrong until checkpoint 28 opens the panel in REAPER.
//
// Everything about this component that *can* be held to a contract was put in
// `ui/ui_host.h` for exactly that reason.
//
// ---------------------------------------------------------------------------
// Thread discipline
//
// Three threads touch this object and each one is allowed a different set of calls.
//
//   REAPER's main thread — the constructor, `initialize_cef`, `pump_message_loop`, and
//   every `BrowserHost` operation. These are the calls the UI Host makes, and the UI
//   Host only ever runs inside a timer tick (requirement 22.3).
//
//   CEF's UI thread — the callbacks. On Windows this is a thread CEF owns, because the
//   multi-threaded message loop is what keeps the panel repainting while REAPER's main
//   thread is busy (requirements 15.5, 22.4). On macOS and Linux it is the main thread,
//   pumped from the tick. Either way the browser reference is only ever read under the
//   lock.
//
//   CEF's IO and renderer threads — nothing here. `OnProcessMessageReceived` arrives on
//   the UI thread, and what it carries is put on a queue that `pump_message_loop` drains
//   on the main thread. A message from JavaScript must not reach the dispatcher on a CEF
//   thread: the dispatcher calls the REAPER C API.
//
// So every operation that CEF requires on its UI thread is posted there, and everything
// handed back to the extension is queued to the main thread. The lock covers only the
// browser reference and the inbound queue, and is never held across a CEF call.

#include <ui/cef_browser_host.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ui/renderer_bridge_binding.h>

#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_process_message.h>
#include <include/cef_task.h>
#include <include/cef_values.h>

#if defined(__APPLE__)
#include <include/wrapper/cef_library_loader.h>
#endif

#if defined(_WIN32)
#include <windows.h>

#include <ui/cef_library_loading.h>
#endif

namespace sesh_ai::ui {

	namespace {

		// The arguments of one process message from the renderer, in the shape
		// `read_message_from_javascript` reads. The renderer half that writes them is the
		// helper's (`ui/cef_subprocess_helper_main.cpp`), and the positions both halves use
		// are `ui/renderer_bridge_binding.h`'s.
		class process_message_argument_list_view {
		public:
			explicit process_message_argument_list_view(CefRefPtr<CefListValue> arguments)
				: arguments_{std::move(arguments)}
			{
			}

			std::size_t size() const
			{
				return arguments_.get() == nullptr ? 0 : arguments_->GetSize();
			}

			bool is_string(std::size_t index) const
			{
				return arguments_->GetType(index) == VTYPE_STRING;
			}

			std::string string_at(std::size_t index) const
			{
				// UTF-16 in CEF, UTF-8 from here on, which is what the JSON parser and the
				// outbound queue expect.
				return arguments_->GetString(index).ToString();
			}

		private:
			CefRefPtr<CefListValue> arguments_;
		};

		// A task that runs an arbitrary callable on a CEF thread.
		//
		// Written out rather than using `CefCreateClosureTask`, because a hand-rolled
		// `CefTask` is the one spelling that has been stable across every CEF version this
		// project might end up on.
		template <typename CallableType>
		class posted_task final : public CefTask {
		public:
			explicit posted_task(CallableType callable)
				: callable_{std::move(callable)}
			{
			}

			void Execute() override
			{
				callable_();
			}

		private:
			CallableType callable_;

			IMPLEMENT_REFCOUNTING(posted_task);
		};

		// Runs `callable` on CEF's UI thread — immediately when already there, so that a
		// synchronous path stays synchronous.
		template <typename CallableType>
		void run_on_cef_ui_thread(CallableType callable)
		{
			if (CefCurrentlyOn(TID_UI)) {
				callable();
				return;
			}

			CefPostTask(TID_UI, new posted_task<CallableType>{std::move(callable)});
		}

		// One message from JavaScript, waiting for the main thread.
		struct queued_message_from_javascript {
			std::string message_name;
			std::string serialized_json;
		};

		// The panel's native handle, in whatever `CefWindowHandle` is on this platform.
		//
		// Three different things, which is why it is a function rather than a cast at the
		// call site. An `HWND` on Windows and a `void*`-or-`NSView*` on macOS are both
		// pointers; an X11 `Window` on Linux is an integer, and a pointer does not convert
		// to one without going through `std::uintptr_t`.
		//
		// Two open items, both needing a running REAPER to settle, and neither of them
		// resolvable here:
		//
		//   - On macOS, REAPER's dock panel handle is a SWELL `HWND`, which is an
		//     Objective-C object rather than an `NSView*`. Whether CEF accepts it directly,
		//     or whether the view has to be pulled out of it first, is a question for the
		//     SDK validation step. If CEF's `cef_window_handle_t` turns out to be declared
		//     as `NSView*` rather than `void*` on this platform, this translation unit has
		//     to become Objective-C++ as well.
		//   - On Linux, REAPER under SWELL is GDK-backed, so the handle has to be resolved
		//     to an X11 window before CEF sees it.
		inline CefWindowHandle native_window_handle(void* parent_window_handle)
		{
#if defined(__linux__)
			return static_cast<CefWindowHandle>(
				reinterpret_cast<std::uintptr_t>(parent_window_handle)
			);
#else
			return static_cast<CefWindowHandle>(parent_window_handle);
#endif
		}

	}

	// The CEF objects, and the client that owns the callbacks.
	//
	// Defined here so that `cef_browser_host.h` names none of these types.
	struct CefPanelBrowserHost::cef_state {
		// The browser-process client. Holds the life-span handler that captures the
		// browser and the IPC entry point for messages from JavaScript.
		class client final : public CefClient, public CefLifeSpanHandler {
		public:
			explicit client(cef_state& state)
				: state_{state}
			{
			}

			CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override
			{
				return this;
			}

			// ------------------------------------------------------------------
			// CefLifeSpanHandler — CEF's UI thread
			// ------------------------------------------------------------------

			void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
			{
				const std::lock_guard<std::mutex> lock{state_.mutex};
				state_.browser = browser;
			}

			void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
			{
				const std::lock_guard<std::mutex> lock{state_.mutex};

				if (state_.browser.get() != nullptr
					&& state_.browser->IsSame(browser)) {
					state_.browser = nullptr;
				}
			}

			// ------------------------------------------------------------------
			// CefClient — CEF's UI thread
			// ------------------------------------------------------------------

			bool OnProcessMessageReceived(
				CefRefPtr<CefBrowser> browser,
				CefRefPtr<CefFrame> frame,
				CefProcessId source_process,
				CefRefPtr<CefProcessMessage> message) override
			{
				if (source_process != PID_RENDERER) {
					return false;
				}

				// The frame is judged again here, from the browser's own view of it,
				// rather than trusting that the renderer only bound the function where
				// `ui/renderer_bridge_binding.h` says it should. The renderer is the less
				// trusted of the two processes.
				const std::string frame_url =
					frame.get() == nullptr ? std::string{} : frame->GetURL().ToString();

				BridgeFrameDescription frame_description;
				frame_description.is_main_frame = frame.get() != nullptr && frame->IsMain();
				frame_description.url = frame_url;

				{
					const std::lock_guard<std::mutex> lock{state_.mutex};
					frame_description.belongs_to_panel_browser =
						state_.browser.get() != nullptr && state_.browser->IsSame(browser);
				}

				// Converted rather than compared directly: `CefString`'s comparison
				// overloads have moved around between CEF versions, and `ToString` has
				// not.
				MessageFromJavascriptRead read = read_message_from_javascript(
					message->GetName().ToString(),
					frame_description,
					process_message_argument_list_view{message->GetArgumentList()}
				);

				if (!read.consumed()) {
					return false;
				}

				// Ours and refused. Nothing is queued: the renderer code this extension
				// ships never sends one of these, so whatever did is not owed an answer.
				if (!read.readable()) {
					return true;
				}

				// Queued rather than handled. The handler reaches the Main-Thread
				// Dispatcher, and on Windows this is CEF's own thread rather than
				// REAPER's main one (requirement 22.3). `pump_message_loop`, called from
				// the timer tick, is what hands it over.
				queued_message_from_javascript queued;
				queued.message_name = std::move(read.message_name);
				queued.serialized_json = std::move(read.serialized_json);

				const std::lock_guard<std::mutex> lock{state_.mutex};
				state_.messages_from_javascript.push_back(std::move(queued));

				return true;
			}

		private:
			cef_state& state_;

			IMPLEMENT_REFCOUNTING(client);
		};

		// Guards `browser` and `messages_from_javascript`, which are the only members two
		// threads touch. Never held across a CEF call.
		std::mutex mutex;

		CefRefPtr<CefBrowser> browser;
		CefRefPtr<client> browser_client;
		std::vector<queued_message_from_javascript> messages_from_javascript;

#if defined(__APPLE__)
		// Whether `cef_load_library` succeeded, so the destructor knows whether there is
		// anything to unload, and whether `CefInitialize` was ever reached, so it knows
		// whether unloading is safe.
		bool framework_loaded = false;
		bool cef_initialize_called = false;
#endif
	};

	CefPanelBrowserHost::CefPanelBrowserHost(
		void* parent_window_handle,
		CefRuntimeLayout runtime_layout,
		CefPlatformConfiguration platform_configuration)
		: state_{std::make_unique<cef_state>()},
		parent_window_handle_{parent_window_handle},
		runtime_layout_{std::move(runtime_layout)},
		platform_configuration_{std::move(platform_configuration)}
	{
	}

	CefPanelBrowserHost::~CefPanelBrowserHost()
	{
		// Deliberately not a shutdown. `CefShutdown` is process-wide and ordered against
		// the timer and the queues (requirement 1.2), so it belongs to
		// `ShutdownSequence` and to the plugin entry that drives it — not to whenever this
		// object happens to be destroyed. A destructor that shut CEF down would make the
		// ordering depend on declaration order in a file this component cannot see.

#if defined(__APPLE__)
		// The framework is unloaded only when CEF never started or was shut down
		// cleanly. Unloading it under a running CEF, or after a `CefInitialize` that
		// failed partway, could leave its threads executing unmapped code inside REAPER,
		// so then it stays loaded until the process exits.
		if (state_->framework_loaded && (!state_->cef_initialize_called || cef_shut_down_)) {
			cef_unload_library();
		}
#endif
	}

	const char* CefPanelBrowserHost::javascript_bridge_receive_function()
	{
		return "window.seshAiBridge.receive";
	}

	const char* CefPanelBrowserHost::javascript_bridge_send_function()
	{
		// The renderer installs the function under this name, so it is read from the
		// header both processes compile. `data()` is safe as a C string because the
		// view was made from a literal.
		return javascript_bridge_send_function_name.data();
	}

	bool CefPanelBrowserHost::cef_initialized() const
	{
		return cef_initialized_;
	}

	const std::string& CefPanelBrowserHost::failure_reason() const
	{
		return failure_reason_;
	}

	void CefPanelBrowserHost::set_message_from_javascript_handler(MessageFromJavascriptHandler handler)
	{
		message_from_javascript_handler_ = std::move(handler);
	}

	bool CefPanelBrowserHost::initialize_cef()
	{
		if (cef_initialized_) {
			failure_reason_ = "cef is already initialized in this process";
			return false;
		}

		if (cef_shut_down_) {
			// `CefInitialize` after `CefShutdown` is not supported in the same process.
			failure_reason_ = "cef has been shut down and cannot be initialized again";
			return false;
		}

		if (runtime_layout_.subprocess_path.empty()) {
			// Not a detail that can be defaulted. Without it CEF re-executes REAPER with
			// renderer arguments, and REAPER does not know what to do with them.
			failure_reason_ = "no cef subprocess executable path was supplied";
			return false;
		}

#if defined(__APPLE__)
		// The framework is not linked, because a linked framework is looked for
		// relative to REAPER's executable, and so is CefScopedLibraryLoader's
		// `LoadInMain`. The path is resolved beside this binary instead
		// (`entry/cef_framework_location.h`), and every CEF call below goes through the
		// function pointers `cef_load_library` fills.
		if (runtime_layout_.framework_library_path.empty()
			|| runtime_layout_.framework_directory.empty()
			|| runtime_layout_.main_bundle_path.empty()) {
			failure_reason_ = "the cef framework and main bundle paths were not supplied";
			return false;
		}

		if (!state_->framework_loaded) {
			if (cef_load_library(runtime_layout_.framework_library_path.c_str()) == 0) {
				failure_reason_ =
					"the chromium embedded framework could not be loaded from '"
					+ runtime_layout_.framework_library_path + "'";
				return false;
			}

			state_->framework_loaded = true;
		}
#endif

#if defined(_WIN32)
		// `libcef.dll` is delay-loaded, because an import would be looked for in REAPER's
		// search path. Loaded here by the path resolved beside this binary, before
		// anything below: `CefSettings` and `CefString` call into it as well as
		// `CefInitialize`, and a delay-load that cannot find it crashes REAPER rather than
		// failing. `ui/cef_library_loading.h` has the rest.
		if (runtime_layout_.framework_library_path.empty()) {
			failure_reason_ = "the path to libcef.dll was not supplied";
			return false;
		}

		const CefLibraryLoadOutcome cef_library_load =
			load_cef_library_by_full_path(runtime_layout_.framework_library_path);

		if (!cef_library_load.loaded) {
			failure_reason_ = cef_library_load.failure_reason;
			return false;
		}
#endif

#if defined(_WIN32)
		CefMainArgs main_args{::GetModuleHandleW(nullptr)};
#else
		CefMainArgs main_args{0, nullptr};
#endif

		CefSettings settings;

		// The browser gets its own process on every platform (requirements 15.5, 22.4).
		// There is no path here that sets `single_process`: that mode shares a process
		// with REAPER, which is the arrangement those two requirements exist to rule out.
		CefString(&settings.browser_subprocess_path).FromString(runtime_layout_.subprocess_path);

#if defined(__APPLE__)
		// Both deliberately. Without the first CEF looks for the framework, and so for
		// its resources and locales, inside REAPER's bundle. Without the second the
		// browser process and the helpers take different bundle identifiers and cannot
		// find each other's Mach services. `entry/cef_framework_location.h` has the
		// sources for both.
		CefString(&settings.framework_dir_path).FromString(runtime_layout_.framework_directory);
		CefString(&settings.main_bundle_path).FromString(runtime_layout_.main_bundle_path);
#endif

		if (!runtime_layout_.resource_directory.empty()) {
			CefString(&settings.resources_dir_path).FromString(runtime_layout_.resource_directory);
		}

		if (!runtime_layout_.locales_directory.empty()) {
			CefString(&settings.locales_dir_path).FromString(runtime_layout_.locales_directory);
		}

		if (!runtime_layout_.cache_path.empty()) {
			CefString(&settings.root_cache_path).FromString(runtime_layout_.cache_path);
		}

		settings.no_sandbox = platform_configuration_.process_sandbox_enabled ? 0 : 1;

#if defined(_WIN32)
		// CEF runs its own loop on its own thread, so the panel keeps repainting while
		// REAPER's main thread is inside a tool call.
		settings.multi_threaded_message_loop = 1;
		settings.external_message_pump = 0;
#else
		// Neither macOS nor Linux supports the multi-threaded message loop, so CEF's loop
		// is driven from the extension's timer instead. The browser is still a separate
		// process; only CEF's own bookkeeping shares this thread.
		settings.multi_threaded_message_loop = 0;

		// Deliberately off, even though pumping is what happens below. CEF's external
		// message pump is the *optimised* form of this arrangement and it requires a
		// `CefBrowserProcessHandler::OnScheduleMessagePumpWork` to tell the host when work
		// is waiting — which means a browser-process `CefApp` passed to `CefInitialize`
		// below, and none is. The helper's `CefApp` is the renderer's and cannot do this
		// job. Turning the flag on without the handler asks CEF to wait for a callback
		// nothing will make. With it off, `CefDoMessageLoopWork` on a timer is
		// CEF's documented way to integrate with a loop the host already owns.
		settings.external_message_pump = 0;
#endif

#if defined(__APPLE__)
		state_->cef_initialize_called = true;
#endif

		if (!CefInitialize(main_args, settings, nullptr, nullptr)) {
			failure_reason_ = "cef initialization failed";
			return false;
		}

		state_->browser_client = new cef_state::client{*state_};
		cef_initialized_ = true;
		failure_reason_.clear();

		return true;
	}

	bool CefPanelBrowserHost::create_browser(const std::string& local_asset_url)
	{
		if (!cef_initialized_ || cef_shut_down_) {
			return false;
		}

		if (parent_window_handle_ == nullptr) {
			// No dockable panel yet. Creating a windowless browser instead would show the
			// producer nothing and report success.
			return false;
		}

		{
			const std::lock_guard<std::mutex> lock{state_->mutex};

			if (state_->browser.get() != nullptr) {
				return false;
			}
		}

		const std::string url = local_asset_url;
		CefRefPtr<cef_state::client> browser_client = state_->browser_client;
		void* const parent_window_handle = parent_window_handle_;

		// `CreateBrowser` is asynchronous: `OnAfterCreated` is what actually records the
		// browser. Returning true here means the request was made, which is all the UI
		// Host's `BrowserHost` contract claims.
		run_on_cef_ui_thread([url, browser_client, parent_window_handle] {
			CefWindowInfo window_info;

			// The panel owns its own layout; REAPER resizes the child window. A zero rect
			// would leave the browser invisible until the first resize, so it is created
			// at a size the dock will immediately override.
			window_info.SetAsChild(
				native_window_handle(parent_window_handle),
				CefRect{0, 0, 480, 640}
			);

			CefBrowserSettings browser_settings;

			// The marker the renderer reads in `OnBrowserCreated`, and the first of the
			// three things that decide whether a frame gets `seshAiBridgeSend`
			// (`ui/renderer_bridge_binding.h`). A popup is created without it.
			CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
			extra_info->SetBool(CefString{std::string{panel_browser_marker_key}}, true);

			::CefBrowserHost::CreateBrowser(
				window_info,
				browser_client,
				CefString{url},
				browser_settings,
				extra_info,
				nullptr
			);
		});

		return true;
	}

	bool CefPanelBrowserHost::navigate(const std::string& absolute_url)
	{
		if (!cef_initialized_ || cef_shut_down_) {
			return false;
		}

		CefRefPtr<CefBrowser> browser;

		{
			const std::lock_guard<std::mutex> lock{state_->mutex};
			browser = state_->browser;
		}

		if (browser.get() == nullptr) {
			return false;
		}

		const std::string url = absolute_url;

		run_on_cef_ui_thread([browser, url] {
			const CefRefPtr<CefFrame> frame = browser->GetMainFrame();

			if (frame.get() != nullptr) {
				frame->LoadURL(CefString{url});
			}
		});

		return true;
	}

	bool CefPanelBrowserHost::post_message_to_javascript(
		const std::string& message_name,
		const std::string& serialized_json)
	{
		if (!cef_initialized_ || cef_shut_down_) {
			return false;
		}

		CefRefPtr<CefBrowser> browser;

		{
			const std::lock_guard<std::mutex> lock{state_->mutex};
			browser = state_->browser;
		}

		if (browser.get() == nullptr) {
			return false;
		}

		// Both halves go through `to_javascript_string_literal`, which is where the
		// escaping is defined and where the suite holds it to its invariants. A payload
		// carries agent-authored conversation text, so this is an injection boundary
		// rather than a formatting one.
		std::string script{javascript_bridge_receive_function()};
		script.append("(")
			.append(to_javascript_string_literal(message_name))
			.append(", ")
			.append(to_javascript_string_literal(serialized_json))
			.append(");");

		run_on_cef_ui_thread([browser, script] {
			const CefRefPtr<CefFrame> frame = browser->GetMainFrame();

			if (frame.get() != nullptr) {
				// The URL and line number are for the page's own error reporting, and an
				// empty URL is what CEF documents for a script with no source file.
				frame->ExecuteJavaScript(CefString{script}, CefString{}, 0);
			}
		});

		return true;
	}

	void CefPanelBrowserHost::pump_message_loop()
	{
		std::vector<queued_message_from_javascript> messages;

		{
			const std::lock_guard<std::mutex> lock{state_->mutex};
			messages.swap(state_->messages_from_javascript);
		}

		// Drained and then handled with no lock held: the handler reaches the dispatcher,
		// which may publish back across the bridge.
		if (message_from_javascript_handler_) {
			for (queued_message_from_javascript& message : messages) {
				message_from_javascript_handler_(
					std::move(message.message_name),
					SerializedBridgePayload{std::move(message.serialized_json)}
				);
			}
		}

#if !defined(_WIN32)
		if (cef_initialized_ && !cef_shut_down_) {
			CefDoMessageLoopWork();
		}
#endif
	}

	void CefPanelBrowserHost::close_browser()
	{
		if (!cef_initialized_ || cef_shut_down_) {
			return;
		}

		CefRefPtr<CefBrowser> browser;

		{
			const std::lock_guard<std::mutex> lock{state_->mutex};
			browser = state_->browser;
		}

		if (browser.get() == nullptr) {
			return;
		}

		// `true` forces the close: a `beforeunload` handler in the page must not be able
		// to keep REAPER from quitting.
		run_on_cef_ui_thread([browser] {
			browser->GetHost()->CloseBrowser(true);
		});

		// The browser is not cleared here — `OnBeforeClose` does that, and clearing it
		// early would leave CEF holding a browser this object no longer knows about.
		// `shut_down_cef` below is what gives CEF the chance to finish.
	}

	void CefPanelBrowserHost::shut_down_cef()
	{
		if (!cef_initialized_ || cef_shut_down_) {
			return;
		}

#if !defined(_WIN32)
		// The close requested above completes on CEF's UI thread, and `CefShutdown`
		// requires it to have finished. On macOS and Linux that thread is this one, so the
		// loop has to be pumped until the browser is gone.
		//
		// Bounded rather than unbounded. A page that will not close must not hang REAPER's
		// quit — the producer would report Sesh as having frozen their DAW, which is worse
		// than a helper process that outlives the shutdown by a moment. The bound is
		// generous relative to a close that is going to succeed.
		//
		// Not a loop on Windows: CEF's own thread runs the close there, and `CefShutdown`
		// blocks until that thread has finished. Spinning this one would be a busy-wait
		// that accomplishes nothing.
		constexpr int maximum_close_pump_iterations = 1000;

		for (int iteration = 0; iteration < maximum_close_pump_iterations; ++iteration) {
			{
				const std::lock_guard<std::mutex> lock{state_->mutex};

				if (state_->browser.get() == nullptr) {
					break;
				}
			}

			CefDoMessageLoopWork();
		}
#endif

		state_->browser_client = nullptr;

		CefShutdown();

		cef_shut_down_ = true;
		cef_initialized_ = false;
	}

}
