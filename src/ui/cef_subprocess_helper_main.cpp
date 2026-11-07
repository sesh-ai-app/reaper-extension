// The entry point of the CEF subprocess helper (task 26.1, requirements 15.7 and 21.1).
//
// CEF runs the renderer, GPU, and utility processes by launching an executable with a
// `--type=` switch on its command line. Its default is to relaunch the host process's own
// executable, and the host process here is REAPER, which does not know what to do with
// that switch. So `CefPanelBrowserHost::initialize_cef` always sets
// `browser_subprocess_path`, and this is the executable it points at. It is a separate
// CMake target, `sesh_ai_cef_subprocess_helper`, linked against the same
// libcef_dll_wrapper and the same CEF runtime as the extension. `CMakeLists.txt` says
// which of CEF's compiler flags it takes and why.
//
// Its main job is `CefExecuteProcess`. That call blocks for the life of the subprocess
// and returns its exit code. Started without a `--type` switch, which only happens when a
// person runs it by hand, CEF returns -1 straight away, and so does this.
//
// Its other job is the renderer's half of the bridge (task 31.1). The `CefApp` below
// gives CEF a render process handler, which installs `seshAiBridgeSend` in the panel's
// page and turns each call into one process message to the browser process. Every
// decision in that — which frames, which arguments, what the message carries — is in
// `ui/renderer_bridge_binding.h`, where the suite reaches it; the file comment there
// says why each one was made. What is here is translation into CEF's types. The same
// `CefApp` is handed to the GPU and utility processes, which never call the handler.
//
// This file takes CEF's flags, `-fno-exceptions` and `-fno-rtti` among them, and so does
// the header it includes. Nothing here may throw or ask for a dynamic type.
//
// ---------------------------------------------------------------------------
// Followed from CEF's own sample
//
// The minimal distribution `CMakeLists.txt` downloads carries no samples, so this follows
// `tests/cefsimple/` in the CEF repository at the commit the pin names (g564dd6c). On
// macOS that is `process_helper_mac.cc`: enter the sandbox, load the framework at run time,
// then execute. On Windows and Linux cefsimple's own executable doubles as its helper, so
// what is taken from it is the entry point and the `CefApp`.
//
// ---------------------------------------------------------------------------
// Threads
//
// Every callback below runs on the renderer's main thread (CEF's `TID_RENDERER`): the
// browser notifications, the context notification, and the V8 function itself. So the
// set of panel browsers needs no lock. This is a separate process from REAPER, so none
// of it is one of the extension's two queues (requirement 22.1) either.

#include <cstddef>
#include <set>
#include <string>
#include <utility>

#include <include/cef_app.h>
#include <include/cef_process_message.h>
#include <include/cef_render_process_handler.h>
#include <include/cef_v8.h>
#include <include/cef_values.h>

#include <ui/renderer_bridge_binding.h>

#if defined(__APPLE__)
#include <include/wrapper/cef_library_loader.h>

// CEF's CMake support defines this when its USE_SANDBOX option is on, which it is by
// default. The extension asks for the sandbox on macOS (`process_sandbox_enabled` in
// `ui/ui_host.h`), and a renderer that was asked to be sandboxed has to enter the sandbox
// here, before the framework is loaded.
#if defined(CEF_USE_SANDBOX)
#include <include/cef_sandbox_mac.h>
#endif
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

	// The exit code when the helper fails before it reaches CEF. Both failures below are
	// layout problems, not crashes. CEF's wrapper has already written the reason to stderr.
	constexpr int helper_could_not_reach_cef_exit_code = 1;

	// The arguments of one V8 call, in the shape `check_bridge_send_arguments` reads.
	class v8_argument_list_view {
	public:
		explicit v8_argument_list_view(const CefV8ValueList& arguments)
			: arguments_{arguments}
		{
		}

		std::size_t size() const
		{
			return arguments_.size();
		}

		bool is_string(std::size_t index) const
		{
			return arguments_[index].get() != nullptr && arguments_[index]->IsString();
		}

	private:
		const CefV8ValueList& arguments_;
	};

	// `seshAiBridgeSend` itself. One per bound context, holding the frame it was bound
	// in, so that a call always leaves through the frame that passed the eligibility
	// check rather than through whichever context happens to be current.
	//
	// It holds nothing else, which is requirement 15.4's half here: the page can reach a
	// function that takes two strings and returns `undefined`.
	class bridge_send_function final : public CefV8Handler {
	public:
		explicit bridge_send_function(CefRefPtr<CefFrame> frame)
			: frame_{std::move(frame)}
		{
		}

		bool Execute(
			const CefString& name,
			CefRefPtr<CefV8Value> object,
			const CefV8ValueList& arguments,
			CefRefPtr<CefV8Value>& return_value,
			CefString& exception) override
		{
			(void)name;
			(void)object;

			// A refusal is a thrown JavaScript error, never a crash and never a silent
			// drop: `sendRawBridgeMessage` reports it as `transport_threw`.
			const sesh_ai::ui::BridgeSendArgumentProblem problem =
				sesh_ai::ui::check_bridge_send_arguments(v8_argument_list_view{arguments});

			if (problem != sesh_ai::ui::BridgeSendArgumentProblem::none) {
				exception = std::string{sesh_ai::ui::describe_bridge_send_argument_problem(problem)};
				return true;
			}

			if (frame_.get() == nullptr || !frame_->IsValid()) {
				exception = std::string{sesh_ai::ui::bridge_send_frame_gone_message};
				return true;
			}

			CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(
				std::string{sesh_ai::ui::message_from_javascript_process_message_name}
			);

			CefRefPtr<CefListValue> message_arguments = message->GetArgumentList();
			message_arguments->SetString(
				sesh_ai::ui::message_name_argument_index,
				arguments[sesh_ai::ui::message_name_argument_index]->GetStringValue()
			);
			message_arguments->SetString(
				sesh_ai::ui::serialized_json_argument_index,
				arguments[sesh_ai::ui::serialized_json_argument_index]->GetStringValue()
			);

			frame_->SendProcessMessage(PID_BROWSER, message);

			return_value = CefV8Value::CreateUndefined();
			return true;
		}

	private:
		CefRefPtr<CefFrame> frame_;

		IMPLEMENT_REFCOUNTING(bridge_send_function);
	};

	// Installs `seshAiBridgeSend` in the main frame of the panel's page and nowhere else.
	class panel_render_process_handler final : public CefRenderProcessHandler {
	public:
		void OnBrowserCreated(
			CefRefPtr<CefBrowser> browser,
			CefRefPtr<CefDictionaryValue> extra_info) override
		{
			const CefString marker_key{std::string{sesh_ai::ui::panel_browser_marker_key}};

			if (extra_info.get() != nullptr
				&& extra_info->HasKey(marker_key)
				&& extra_info->GetType(marker_key) == VTYPE_BOOL
				&& extra_info->GetBool(marker_key)) {
				panel_browser_identifiers_.insert(browser->GetIdentifier());
			}
		}

		void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override
		{
			panel_browser_identifiers_.erase(browser->GetIdentifier());
		}

		// Called for every new V8 context, which includes every navigation of the main
		// frame. So the sign-in page, loaded into the same frame, is checked afresh and
		// refused, and the application gets the binding back when it is loaded again.
		void OnContextCreated(
			CefRefPtr<CefBrowser> browser,
			CefRefPtr<CefFrame> frame,
			CefRefPtr<CefV8Context> context) override
		{
			const std::string frame_url = frame->GetURL().ToString();

			sesh_ai::ui::BridgeFrameDescription description;
			description.belongs_to_panel_browser =
				panel_browser_identifiers_.count(browser->GetIdentifier()) != 0;
			description.is_main_frame = frame->IsMain();
			description.url = frame_url;

			if (sesh_ai::ui::bridge_frame_eligibility(description)
				!= sesh_ai::ui::BridgeFrameEligibility::eligible) {
				return;
			}

			const CefString function_name{
				std::string{sesh_ai::ui::javascript_bridge_send_function_name}
			};

			CefRefPtr<CefV8Value> function =
				CefV8Value::CreateFunction(function_name, new bridge_send_function{frame});

			// Read-only and undeletable, so a script that found its way into the page
			// cannot put a function of its own in this one's place and read what the
			// producer types.
			context->GetGlobal()->SetValue(
				function_name,
				function,
				static_cast<CefV8Value::PropertyAttribute>(
					V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE
				)
			);
		}

	private:
		std::set<int> panel_browser_identifiers_;

		IMPLEMENT_REFCOUNTING(panel_render_process_handler);
	};

	class helper_application final : public CefApp {
	public:
		helper_application()
			: render_process_handler_{new panel_render_process_handler}
		{
		}

		CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
		{
			return render_process_handler_;
		}

	private:
		CefRefPtr<CefRenderProcessHandler> render_process_handler_;

		IMPLEMENT_REFCOUNTING(helper_application);
	};

}

#if defined(_WIN32)

// `wWinMain` rather than `main`, because the target is built for the Windows GUI
// subsystem. A console-subsystem helper would open a console window for every renderer
// and GPU process CEF starts. cefsimple makes the same choice for the same reason.
//
// `libcef.dll` is delay-loaded (CEF's `/DELAYLOAD:libcef.dll`, applied in CMakeLists.txt),
// so it is resolved on the first CEF call below from this executable's own directory.
// That is why the helper has to sit beside `libcef.dll`.
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
	const CefMainArgs main_arguments{instance};

	CefRefPtr<CefApp> application{new helper_application};

	// No sandbox information. The Windows sandbox can only be set up from the host
	// executable's entry point, and that belongs to REAPER (`ui/cef_browser_host.h`).
	return CefExecuteProcess(main_arguments, application, nullptr);
}

#else

int main(int argument_count, char* argument_values[])
{
#if defined(__APPLE__)
#if defined(CEF_USE_SANDBOX)
	// Before the framework is loaded, as cefsimple does it. The sandbox library is found
	// inside the framework, at a path relative to this executable.
	CefScopedSandboxContext sandbox_context;

	if (!sandbox_context.Initialize(argument_count, argument_values)) {
		return helper_could_not_reach_cef_exit_code;
	}
#endif

	// On macOS the framework has to be loaded at run time rather than linked, which is why
	// the target does not link it. `LoadInHelper` looks for it three directories above this
	// executable, which is where it is when this executable is
	// `<directory>/Sesh AI Helper.app/Contents/MacOS/Sesh AI Helper` and the framework is
	// `<directory>/Chromium Embedded Framework.framework`. The build places the five
	// helper bundles and the framework side by side in that directory, `bin/` or
	// `UserPlugins`, which is the layout CMakeLists.txt describes under "The CEF runtime
	// beside the binary". A bare executable outside that layout stops here.
	CefScopedLibraryLoader library_loader;

	if (!library_loader.LoadInHelper()) {
		return helper_could_not_reach_cef_exit_code;
	}
#endif

	const CefMainArgs main_arguments{argument_count, argument_values};

	// After the framework is loaded on macOS, and declared after the loader, so it is
	// released before the framework is unloaded.
	CefRefPtr<CefApp> application{new helper_application};

	return CefExecuteProcess(main_arguments, application, nullptr);
}

#endif
