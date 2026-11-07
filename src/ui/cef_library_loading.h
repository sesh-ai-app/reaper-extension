// Loading `libcef.dll` by its full path on Windows (task 32.2, requirements 1.1 and
// 15.7).
//
// `entry/cef_framework_location.h` says why the extension cannot import `libcef.dll`:
// the import would be looked for in REAPER's search path, and the extension would fail
// to load before any of its code ran. So `CMakeLists.txt` delay-loads it, and
// `CefPanelBrowserHost::initialize_cef` calls `load_cef_library_by_full_path` with the
// path resolved beside the binary before it touches anything of CEF's. This header is
// the declaration and the part of the decision the suite can reach; the Windows calls
// are in `ui/cef_library_loading_windows.cpp`, which only the Windows build compiles.
//
// ---------------------------------------------------------------------------
// Why not CEF's own loader
//
// CEF ships one for Windows, `CefScopedLibraryLoader::LoadInMainAssert` in
// `include/wrapper/cef_library_loader.h`, and its failures are `LOG(FATAL)`
// (`libcef_dll/wrapper/cef_scoped_library_loader_win.cc`). The process it would
// terminate is REAPER's, over a file the producer can put back. Its destructor also
// frees the library, which is wrong for an object that does not live as long as CEF.
// What it does is otherwise small, so it is done here with a failure that is reported.
//
// ---------------------------------------------------------------------------
// How the library is loaded
//
// `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
// LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`. `libcef.dll` imports `chrome_elf.dll`, which ships
// beside it, and Windows resolves a DLL's own imports by module name even when the DLL
// was loaded by full path. Microsoft's `LoadLibraryExW` reference says
// `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` puts the loaded DLL's directory first in the search
// for its dependencies and requires a fully qualified path, and that combining it with
// `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` (the application directory, `AddDllDirectory`
// directories, and System32) keeps the system libraries reachable. Neither searches the
// current directory or `PATH`.
//
// CEF's loader uses `LOAD_WITH_ALTERED_SEARCH_PATH` instead, which also starts from the
// DLL's directory. It then falls back to the Windows directory, the current directory,
// and `PATH`. REAPER's current directory is usually a project's, and a project folder
// can come from anyone, so this loader does not fall back there. The reference says the
// two kinds of flag cannot be combined.
//
// The same reference asks for backslashes in a path, which the resolver guarantees.
//
// ---------------------------------------------------------------------------
// How the delay-loaded imports find it
//
// The delay-load helper in `delayimp.lib` resolves an import on its first call, by
// `LoadLibrary` on the bare name `libcef.dll`. A bare name finds a module that is
// already loaded, from whatever directory, so after the load here that would usually be
// this one. But Microsoft documents that when more than one loaded module has the same
// base name, a bare-name load returns the one loaded first, and another REAPER extension
// may have brought its own CEF. So `ui/cef_library_loading_windows.cpp` defines the
// delay-load notification hook, and on `dliNotePreLoadLibrary` for `libcef.dll` hands the
// helper the module loaded here. Which library the extension's CEF calls reach is then
// a fact rather than a lookup.
//
// ---------------------------------------------------------------------------
// Nothing may call into `libcef.dll` before this
//
// A delay-load that fails raises a structured exception, which inside REAPER is a crash.
// So the load has to precede every call that goes through a delay-load thunk, and those
// are not only `CefInitialize`: `CefString` and `CefSettings` call `libcef.dll`'s string
// functions as they assign and as they are destroyed. `initialize_cef` therefore loads
// before it declares either. What was checked that nothing runs earlier:
//
//   - Only `ui/cef_browser_host.cpp` and the helper's own entry point include CEF, and
//     the helper is a separate executable. `CefPanelBrowserHost`'s constructor and
//     destructor make no CEF call on Windows; every other member returns before reaching
//     CEF until `initialize_cef` has succeeded.
//   - Static initialisers. The macOS build of `libcef_dll_wrapper` and of the two CEF
//     translation units has no initialiser section at all (`otool -l`, no
//     `__mod_init_func` or `__init_offsets`). The wrapper's Windows-only sources
//     (`cef_scoped_library_loader_win.cc`, `cef_util_win.cc`,
//     `cef_certificate_util_win.cc`) are not referenced by the extension, so the linker
//     takes nothing from them. That the Windows build agrees is reasoned, not checked.
//
// ---------------------------------------------------------------------------
// Never unloaded
//
// The delay-load helper writes each resolved address into the extension's import table,
// so freeing the library would leave those entries pointing at unmapped code, and CEF
// cannot be initialized a second time in one process anyway. The library stays loaded
// until REAPER exits, as it would have if it were imported.

#ifndef SESH_AI_UI_CEF_LIBRARY_LOADING_H
#define SESH_AI_UI_CEF_LIBRARY_LOADING_H

#include <string>
#include <string_view>

namespace sesh_ai::ui {

	// The module name the extension delay-loads and the notification hook answers for.
	// The same name as `CefPlatformConfiguration::loader_file_name` on Windows, which the
	// suite checks, and as `/DELAYLOAD:libcef.dll` in `CMakeLists.txt`.
	inline constexpr char cef_library_file_name_on_windows[] = "libcef.dll";

	// The `GetLastError` values the failure line explains. Spelled out rather than taken
	// from `windows.h`, so the suite can reach the explanation on every platform.
	inline constexpr unsigned long windows_error_module_not_found = 126;
	inline constexpr unsigned long windows_error_bad_executable_format = 193;

	struct CefLibraryLoadOutcome {
		bool loaded = false;

		// Empty when loaded. Otherwise the line for the log.
		std::string failure_reason;
	};

	// Loads `libcef.dll` from `library_path`, a fully qualified UTF-8 path, and makes it
	// the module the delay-loaded CEF imports bind to. A second call after a success
	// returns loaded without loading again.
	//
	// Windows only: defined in `ui/cef_library_loading_windows.cpp`, which no other
	// platform compiles. Call it on REAPER's main thread, before any CEF call.
	CefLibraryLoadOutcome load_cef_library_by_full_path(const std::string& library_path);

	// The line for a `LoadLibraryExW` that returned no module, from the path it was
	// handed and the `GetLastError` value it left.
	//
	// The resolver has already found a regular file at the path, so "the module was not
	// found" here almost always means one of its own dependencies was not, and saying
	// only the error number would send a producer looking for the file that is there.
	inline std::string describe_cef_library_load_failure(
		std::string_view library_path,
		unsigned long windows_error_code)
	{
		std::string reason{"the CEF library could not be loaded from '"};
		reason.append(library_path);
		reason.append("'");

		if (windows_error_code == windows_error_module_not_found) {
			reason.append(
				": a library it depends on was not found beside it or in System32. "
				"chrome_elf.dll ships beside it in the release archive"
			);
		}
		else if (windows_error_code == windows_error_bad_executable_format) {
			reason.append(
				": it is not a library this process can load, which is what a libcef.dll "
				"built for another architecture looks like"
			);
		}

		reason.append(" (Windows error ");
		reason.append(std::to_string(windows_error_code));
		reason.append(")");

		return reason;
	}

}

#endif
