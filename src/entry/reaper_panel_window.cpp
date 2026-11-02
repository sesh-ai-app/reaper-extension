// The one translation unit that creates the panel window from its dialog resource
// (task 25.1, requirements 1.1, 1.3).
//
// `CreateDialogParam` and `DestroyWindow` are Win32 on Windows and SWELL on macOS and
// Linux, and the same two lines serve all three because SWELL defines
// `CreateDialogParam` as a macro over `SWELL_CreateDialog`
// (`WDL/swell/swell-functions.h`). What differs is where the dialog comes from:
//
//   - On Windows the resource compiler has already put `entry/panel_window.rc` into this
//     DLL, and `CreateDialogParam` finds it through the module handle.
//   - On macOS and Linux nothing compiled the resource script. WDL's generator turned it
//     into `entry/panel_window.rc_mac_dlg` at build time, and the bottom of this file
//     expands it. The expansion is a static object that adds the dialog to
//     `SWELL_curmodule_dialogresource_head`, the per-module list `CreateDialogParam`
//     searches — which is why it must be in this module and not anywhere else.
//
// Both platform calls go through function pointers on macOS and Linux. The build
// defines `SWELL_PROVIDED_BY_APP`, and WDL's module stub fills the pointers from REAPER
// when the module loads; `CMakeLists.txt` explains why the extension cannot simply link
// SWELL itself.
//
// Requirement 22.3 holds without anything here enforcing it. Neither call is in the
// REAPER C API, and the only callers are REAPER's own UI callbacks — the action hook
// that opens the panel, and the unload call that tears it down — which run on the main
// thread.

#include <entry/reaper_panel_window.h>

// Project headers before the SDK, for the reason `entry/plugin_entry.cpp` gives: on
// macOS and Linux the SDK reaches for swell, whose `min` and `max` macros eat any
// `std::min` parsed after it. `reaper_sdk.h` is the only route to the SDK in this tree.
#include <entry/panel_window.h>
#include <entry/panel_window_resource.h>

#include <reaper_sdk.h>

namespace {

	// The panel's dialog procedure. Handles nothing yet, and returns FALSE for every
	// message so the dialog manager's default handling applies.
	//
	// FALSE for `WM_INITDIALOG` too, which is a choice rather than an omission: TRUE
	// would ask the dialog manager to move keyboard focus into the panel the moment it
	// is created, taking it from REAPER's arrange view. The dialog has no controls to
	// focus anyway.
	//
	// Task 25.2 is where this grows. Resizing the browser with the panel and answering
	// the close box are both messages that arrive here.
	INT_PTR CALLBACK handle_panel_window_message(
		HWND /*window*/,
		UINT /*message*/,
		WPARAM /*first_message_parameter*/,
		LPARAM /*second_message_parameter*/)
	{
		return FALSE;
	}

	void* create_panel_window(void* module_instance, void* parent_window)
	{
		// SWELL's `CreateDialogParam` discards its first argument, so on macOS and Linux
		// this is the only use of `module_instance`. Written unconditionally rather than
		// behind `#ifndef _WIN32`, because it costs nothing on Windows.
		static_cast<void>(module_instance);

		return CreateDialogParam(
			static_cast<HINSTANCE>(module_instance),
			MAKEINTRESOURCE(SESH_AI_PANEL_DIALOG_RESOURCE_IDENTIFIER),
			static_cast<HWND>(parent_window),
			&handle_panel_window_message,
			0
		);
	}

	void destroy_panel_window(void* window)
	{
		DestroyWindow(static_cast<HWND>(window));
	}

}

namespace sesh_ai::entry {

	PanelWindowPlatform make_reaper_panel_window_platform(void* module_instance)
	{
		PanelWindowPlatform platform;

		platform.create_window = [module_instance](void* parent_window) {
			return create_panel_window(module_instance, parent_window);
		};

		platform.destroy_window = &destroy_panel_window;

		return platform;
	}

}

#ifndef _WIN32

// The generated dialog, expanded last and on macOS and Linux only.
//
// Last, because `swell-dlggen.h` defines `BEGIN`, `END`, `CONTROL`, `NOT`, and a page of
// Win32 control names as macros so that the resource script's own text compiles as an
// initializer list. Nothing may be parsed after them that uses those words, so they are
// undefined again straight after — the same arrangement as the SDK's own
// `reaper-plugins/reaper_csurf/csurf_main.cpp` and WDL's
// `swell/sample_project/myapp.cpp`.
//
// `swell-dlggen.h` is reached through a source-file include directory that
// `CMakeLists.txt` sets on this file alone, so the rest of WDL stays off the library's
// include path. The generated file is under the build tree's `generated/` directory,
// which is already on it.
//
// The generated file is not a system header, so its warnings reach this build. The one
// it produces is `-Wmissing-field-initializers`: an empty dialog expands to a single
// `{NULL, }` sentinel entry, leaving the rest of the entry zeroed, which is what SWELL
// expects. Silenced for the expansion only, because the file is regenerated from the
// .rc on every edit and is not something to fix by hand. `#pragma GCC` is understood by
// both compilers that reach this branch, Clang and GCC.
#include <swell-dlggen.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <entry/panel_window.rc_mac_dlg>
#pragma GCC diagnostic pop

#undef BEGIN
#undef END

#endif
