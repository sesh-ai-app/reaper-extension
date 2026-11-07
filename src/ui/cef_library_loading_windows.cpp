// The Windows half of `ui/cef_library_loading.h` (task 32.2, requirements 1.1 and 15.7).
//
// Compiled only by the Windows build with CEF (`CMakeLists.txt`). Two things: the
// full-path load, and the delay-load notification hook that binds the extension's CEF
// imports to the module loaded here. The header says why each is the way it is.
//
// Includes no CEF, and calls nothing in `libcef.dll`. This is the code that runs before
// calling into it is safe.

#include <ui/cef_library_loading.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>

// After windows.h, which declares the types it uses.
#include <delayimp.h>

namespace {

	// Written once, on REAPER's main thread, before `CefInitialize` starts CEF's threads.
	// Read by the hook on whichever thread first calls each CEF import, which can be
	// CEF's own UI thread, so it is atomic.
	std::atomic<HMODULE> loaded_cef_library{nullptr};

	// The path arrives as UTF-8 because `entry/extension_binary_location.cpp` converted
	// `GetModuleFileNameW`'s answer with `CP_UTF8`, and converting back the same way gives
	// the same wide path. The narrow `LoadLibraryExA` would read it in the ANSI code page,
	// and a producer whose user name is not ASCII would get a path that names nothing.
	std::wstring wide_from_utf8(const std::string& utf8)
	{
		if (utf8.empty()) {
			return {};
		}

		const int utf8_length = static_cast<int>(utf8.size());
		const int wide_length =
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), utf8_length, nullptr, 0);

		if (wide_length <= 0) {
			return {};
		}

		std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), utf8_length, &wide[0], wide_length);

		return wide;
	}

	// Called by `delayimp.lib`'s helper at each step of resolving a delay-loaded import.
	// Only the step before its own `LoadLibrary`, and only for `libcef.dll`, gets an
	// answer; every other null hands the step back to the helper.
	FARPROC WINAPI bind_delay_loaded_cef_library(unsigned notification, PDelayLoadInfo delay_load_information)
	{
		if (notification != static_cast<unsigned>(dliNotePreLoadLibrary)
			|| delay_load_information == nullptr
			|| delay_load_information->szDll == nullptr
			|| _stricmp(delay_load_information->szDll, sesh_ai::ui::cef_library_file_name_on_windows) != 0) {
			return nullptr;
		}

		// Null before the load, which leaves the helper to its own bare-name
		// `LoadLibrary` and, failing that, a structured exception. `initialize_cef`
		// loading first is what keeps that from being reached.
		return reinterpret_cast<FARPROC>(loaded_cef_library.load(std::memory_order_acquire));
	}

}

// The hook `delayimp.lib`'s helper reads. At global scope, with C linkage and `const`,
// which is how `delayimp.h` declares it and how Microsoft's delay-load documentation
// defines one.
extern "C" const PfnDliHook __pfnDliNotifyHook2 = bind_delay_loaded_cef_library;

namespace sesh_ai::ui {

	CefLibraryLoadOutcome load_cef_library_by_full_path(const std::string& library_path)
	{
		CefLibraryLoadOutcome outcome;

		if (loaded_cef_library.load(std::memory_order_acquire) != nullptr) {
			outcome.loaded = true;

			return outcome;
		}

		const std::wstring wide_library_path = wide_from_utf8(library_path);

		if (wide_library_path.empty()) {
			outcome.failure_reason =
				"the CEF library path '" + library_path
					+ "' is empty or not valid UTF-8, so it cannot be handed to Windows";

			return outcome;
		}

		const HMODULE cef_library = LoadLibraryExW(
			wide_library_path.c_str(),
			nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
		);

		if (cef_library == nullptr) {
			// Read first, before anything else can overwrite it.
			const DWORD error_code = GetLastError();

			outcome.failure_reason = describe_cef_library_load_failure(library_path, error_code);

			return outcome;
		}

		loaded_cef_library.store(cef_library, std::memory_order_release);
		outcome.loaded = true;

		return outcome;
	}

}
