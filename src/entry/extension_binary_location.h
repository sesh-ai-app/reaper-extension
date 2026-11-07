// Where the extension binary was loaded from, and whether a file exists (task 25.2,
// requirement 15.1).
//
// The two platform calls `entry/panel_asset_directory.h` needs to find the chat panel's
// application files beside the binary. Declared without any platform header and defined
// in `entry/extension_binary_location.cpp`, the same split as
// `entry/reaper_panel_window.h`. What the path means is decided in
// `panel_asset_directory.h`, where the suite can reach it; nothing here branches on
// anything but "did the platform answer".

#ifndef SESH_AI_ENTRY_EXTENSION_BINARY_LOCATION_H
#define SESH_AI_ENTRY_EXTENSION_BINARY_LOCATION_H

#include <string>

namespace sesh_ai::entry {

	// The absolute path of the loaded extension binary, UTF-8 encoded, or empty when the
	// platform would not say.
	//
	// `module_instance` is the `REAPER_PLUGIN_HINSTANCE` REAPER passes to
	// `ReaperPluginEntry`. On Windows it is this DLL's `HINSTANCE` and is what
	// `GetModuleFileNameW` is asked about. On macOS and Linux it is not used: `dladdr`
	// on an address inside this library answers the same question without depending on
	// what REAPER chose to pass.
	std::string locate_extension_binary(void* module_instance);

	// Whether a regular file exists at a UTF-8 path. False for a directory, and false
	// for anything the platform could not inspect.
	bool regular_file_exists(const std::string& file_path);

}

#endif
