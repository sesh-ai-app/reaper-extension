// The platform half of the panel window (task 25.1, requirements 1.1, 1.3).
//
// Declared here without the SDK, defined in `entry/reaper_panel_window.cpp`, which is
// the one translation unit that creates a window from `entry/panel_window.rc`. The same
// split as `entry/reaper_timer_registrar.h`: this header can be included anywhere,
// because it names nothing from `reaper_plugin.h`, `windows.h`, or swell.

#ifndef SESH_AI_ENTRY_REAPER_PANEL_WINDOW_H
#define SESH_AI_ENTRY_REAPER_PANEL_WINDOW_H

#include <entry/panel_window.h>

namespace sesh_ai::entry {

	// Builds the two seams a `PanelWindow` needs over `CreateDialogParam` and
	// `DestroyWindow`.
	//
	// `module_instance` is the `REAPER_PLUGIN_HINSTANCE` REAPER passes to
	// `ReaperPluginEntry`. On Windows it is this DLL's `HINSTANCE`, and it is where the
	// resource compiler put the dialog — a null or foreign handle makes
	// `CreateDialogParam` look in the wrong module and the creation is reported as
	// `platform_refused`. SWELL ignores it: the generated resource registers itself in
	// this module's own list, which `SWELL_CreateDialog` is handed directly.
	//
	// The parent passed to `create_window` is REAPER's main window, `hwnd_main` in the
	// registration table. Supplying it is the caller's job, task 25.2's.
	PanelWindowPlatform make_reaper_panel_window_platform(void* module_instance);

}

#endif
