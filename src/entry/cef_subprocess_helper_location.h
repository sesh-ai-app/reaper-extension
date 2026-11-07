// Where the CEF subprocess helper is, found from where the extension binary was loaded
// (task 26.1, requirement 15.7).
//
// CEF starts the renderer, GPU, and utility processes from `browser_subprocess_path`.
// Left empty, CEF relaunches the host process's own executable, which is REAPER, so
// `CefPanelBrowserHost::initialize_cef` refuses an empty path. This is what fills it.
// CEF also requires the path to be absolute.
//
// ---------------------------------------------------------------------------
// Beside the binary, for the reasons the panel assets are
//
// `entry/panel_asset_directory.h` explains why the chat panel's files are looked for
// beside the extension binary rather than under REAPER's resource path. The same
// reasoning holds for the helper, and the release archive is laid out the same way: the
// binary and everything CEF needs side by side, unpacked into `UserPlugins`. The name
// joined onto that directory is `CefPlatformConfiguration::subprocess_executable_name`
// (`ui/ui_host.h`):
//
//   Windows  sesh_ai_cef_helper.exe
//   Linux    sesh_ai_cef_helper
//   macOS    Sesh AI Helper.app/Contents/MacOS/Sesh AI Helper
//
// `CMakeLists.txt` builds an executable of that name into `bin/`, beside the library, so
// a development build finds it. On macOS that is the plain helper bundle, one of five
// (task 26.2); Chromium derives the other four's paths from this one. The framework the
// helpers load is beside them, which `entry/cef_framework_location.h` resolves.
//
// ---------------------------------------------------------------------------
// Missing is a reported condition
//
// The same rule as the assets. A missing helper is neither a crash nor a CEF that
// relaunches REAPER: the resolution names the path it looked at, and
// `install_panel_browser_creator` drops the creator before CEF is ever initialized.

#ifndef SESH_AI_ENTRY_CEF_SUBPROCESS_HELPER_LOCATION_H
#define SESH_AI_ENTRY_CEF_SUBPROCESS_HELPER_LOCATION_H

#include <string>
#include <string_view>

#include <entry/panel_asset_directory.h>
#include <ui/ui_host.h>

namespace sesh_ai::entry {

	struct CefSubprocessHelperResolution {
		// True only when the path is absolute and an executable file is there.
		bool found = false;

		// Where the helper is, or would be. Filled whenever a path could be derived,
		// including when nothing was found there, so the log line can say where it
		// looked.
		std::string subprocess_path;

		// Empty when found. Otherwise the line for the log.
		std::string unavailable_reason;
	};

	// Finds the helper named `subprocess_executable_name` beside the binary at
	// `extension_binary_path`.
	//
	// The same order as `resolve_application_asset_directory`: no binary path, a path
	// that names no directory, a path CEF would refuse, and only then the filesystem.
	inline CefSubprocessHelperResolution resolve_cef_subprocess_helper(
		std::string_view extension_binary_path,
		std::string_view subprocess_executable_name,
		const RegularFileExistence& regular_file_exists)
	{
		CefSubprocessHelperResolution resolution;

		if (subprocess_executable_name.empty()) {
			// `cef_platform_configuration_for` never returns this, and the suite holds it
			// to that. Reported rather than asserted, so a future platform added without
			// a helper name fails with a sentence.
			resolution.unavailable_reason =
				"no CEF subprocess helper is named for this platform, so the browser cannot "
				"be started without CEF relaunching REAPER as its renderer";

			return resolution;
		}

		if (extension_binary_path.empty()) {
			resolution.unavailable_reason =
				"the extension could not determine where its own binary was loaded from, so it "
				"cannot find the CEF subprocess helper '" + std::string{subprocess_executable_name}
					+ "' beside it";

			return resolution;
		}

		const std::string_view binary_directory = directory_containing(extension_binary_path);

		if (binary_directory.empty()) {
			resolution.unavailable_reason =
				"the extension binary's path '" + std::string{extension_binary_path}
					+ "' names no directory, so the CEF subprocess helper cannot be found beside it";

			return resolution;
		}

		resolution.subprocess_path = std::string{binary_directory};
		resolution.subprocess_path.push_back(path_separator_used_by(extension_binary_path));
		resolution.subprocess_path.append(subprocess_executable_name);

		if (!ui::detail::is_absolute_path(resolution.subprocess_path)) {
			resolution.unavailable_reason =
				"the CEF subprocess helper path '" + resolution.subprocess_path
					+ "' is not absolute, and CEF requires an absolute browser_subprocess_path";

			return resolution;
		}

		if (!regular_file_exists || !regular_file_exists(resolution.subprocess_path)) {
			resolution.unavailable_reason =
				"the CEF subprocess helper was not found: '" + resolution.subprocess_path
					+ "' does not exist. CEF starts its renderer and GPU processes from it, and the "
					"release archive installs it beside the extension binary";

			return resolution;
		}

		resolution.found = true;

		return resolution;
	}

}

#endif
