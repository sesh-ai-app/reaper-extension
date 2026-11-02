// The identifier of the panel's dialog resource (task 25.1, requirements 1.1, 1.3).
//
// Read by two tools that are not both C++ compilers, so this header holds a `#define`
// and nothing else:
//
//   - On Windows, `entry/panel_window.rc` includes it and the resource compiler binds
//     the dialog to this number. The resource compiler runs the C preprocessor and
//     nothing more, so a `constexpr`, a namespace, or an include of a C++ header here
//     would stop the Windows build at the resource step.
//   - On macOS and Linux, WDL's generator copies the dialog's *name* — this macro's
//     name, not its value — into `entry/panel_window.rc_mac_dlg`, and the expansion of
//     that file in `entry/reaper_panel_window.cpp` is where the name becomes a number.
//
// So the macro name is spelled in three places that have to agree: here, on the
// `DIALOGEX` line of the resource script, and in `CMakeLists.txt`, which passes it to
// `cmake/GenerateSwellDialogResource.cmake` so a generator that silently skipped the
// dialog fails the build rather than a producer's first click on the panel.
//
// The value is private to this module. Windows looks resources up in the module handle
// passed to `CreateDialogParam`, and SWELL in this module's own resource list, so the
// number cannot collide with REAPER's or another extension's.

#ifndef SESH_AI_ENTRY_PANEL_WINDOW_RESOURCE_H
#define SESH_AI_ENTRY_PANEL_WINDOW_RESOURCE_H

#define SESH_AI_PANEL_DIALOG_RESOURCE_IDENTIFIER 101

#endif
