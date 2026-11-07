// Where the chat panel's application files are, found from where the extension binary
// was loaded (task 25.2, requirement 15.1).
//
// The React application is Vite's output, `ui/dist` in this repository. CEF loads it
// over `file://`, so the extension needs an absolute path to it at runtime. The installed
// layout is CMakeLists.txt's install rules (task 27.1): the binary and a `sesh_ai_ui/`
// directory side by side, which the producer unpacks into REAPER's `UserPlugins`.
// `tests/ui/ui_host_test.cpp` builds its URLs over the same `UserPlugins/sesh_ai_ui`.
//
// ---------------------------------------------------------------------------
// Beside the binary, not under REAPER's resource path
//
// `GetResourcePath()` + `/UserPlugins/sesh_ai_ui` would find the same directory for the
// usual installation and the wrong one for every other. REAPER also loads extensions
// from its program directory's `Plugins` folder, and a portable installation has its
// resource path somewhere else entirely. The archive puts the two side by side, so the
// binary's own directory is the one answer that is right wherever the producer put it.
// It also costs no REAPER C API call, so requirement 22.3 has nothing to say about when
// it runs. Finding the binary is `entry/extension_binary_location.cpp`'s job, because it
// is a platform call; deciding what that path means is here, because it has branches.
//
// ---------------------------------------------------------------------------
// Missing is a reported condition
//
// A development build has no `sesh_ai_ui/` beside `build-vcpkg/bin/reaper_sesh_ai.dylib`,
// and an archive unpacked carelessly may have lost it. Neither is a crash and neither
// is a silent blank panel: the resolution says what it looked for and where, and the
// plugin entry writes that line when the panel opens with no browser in it.

#ifndef SESH_AI_ENTRY_PANEL_ASSET_DIRECTORY_H
#define SESH_AI_ENTRY_PANEL_ASSET_DIRECTORY_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <ui/ui_host.h>

namespace sesh_ai::entry {

	// The directory name the release archive gives Vite's output, beside the binary.
	inline constexpr std::string_view application_asset_directory_name{"sesh_ai_ui"};

	// Whether a regular file exists at a UTF-8 path. A seam, because the suite has no
	// installed layout to look at; `entry/extension_binary_location.cpp` is the real one.
	using RegularFileExistence = std::function<bool(const std::string& file_path)>;

	struct ApplicationAssetDirectoryResolution {
		// True only when the directory's path is one CEF can be handed and its entry
		// point is actually there.
		bool found = false;

		// Where the directory is, or would be. Filled whenever a path could be derived,
		// including when nothing was found there, so the log line can say where it
		// looked rather than only that it failed.
		std::string asset_directory;

		// Empty when found. Otherwise the line for the log.
		std::string unavailable_reason;
	};

	// Everything before the last path separator, or empty when there is none. Both
	// separators, because a Windows path from `GetModuleFileNameW` uses backslashes and
	// nothing promises it uses only them.
	inline std::string_view directory_containing(std::string_view file_path)
	{
		const std::size_t last_separator = file_path.find_last_of("/\\");

		if (last_separator == std::string_view::npos) {
			return {};
		}

		return file_path.substr(0, last_separator);
	}

	// The separator a path already uses, so a joined path does not mix the two on
	// Windows. `build_local_asset_url` normalises either way; the log line is what reads
	// better for it.
	inline char path_separator_used_by(std::string_view path)
	{
		return path.find('\\') != std::string_view::npos && path.find('/') == std::string_view::npos
			? '\\'
			: '/';
	}

	// Finds `sesh_ai_ui/` beside the binary at `extension_binary_path`.
	//
	// The checks run cheapest and most fundamental first: no binary path at all, a
	// path that names no directory, a directory CEF could not be handed as a `file://`
	// URL — the same validation `make_panel_browser_creator` applies, run here so the
	// reason names the directory — and only then the filesystem.
	inline ApplicationAssetDirectoryResolution resolve_application_asset_directory(
		std::string_view extension_binary_path,
		const RegularFileExistence& regular_file_exists)
	{
		ApplicationAssetDirectoryResolution resolution;

		if (extension_binary_path.empty()) {
			resolution.unavailable_reason =
				"the extension could not determine where its own binary was loaded from, so it "
				"cannot find the chat panel's application files beside it";

			return resolution;
		}

		const std::string_view binary_directory = directory_containing(extension_binary_path);

		if (binary_directory.empty()) {
			resolution.unavailable_reason =
				"the extension binary's path '" + std::string{extension_binary_path}
					+ "' names no directory, so the chat panel's application files cannot be "
					"found beside it";

			return resolution;
		}

		const char separator = path_separator_used_by(extension_binary_path);

		resolution.asset_directory = std::string{binary_directory};
		resolution.asset_directory.push_back(separator);
		resolution.asset_directory.append(application_asset_directory_name);

		const ui::LocalAssetUrlResult entry_point_url = ui::build_local_asset_url(
			resolution.asset_directory,
			ui::default_application_entry_point
		);

		if (!entry_point_url.valid) {
			resolution.unavailable_reason =
				"the chat panel's application directory '" + resolution.asset_directory
					+ "' cannot be loaded over file://: " + entry_point_url.rejection_reason;

			return resolution;
		}

		std::string entry_point_path = resolution.asset_directory;
		entry_point_path.push_back(separator);
		entry_point_path.append(ui::default_application_entry_point);

		if (!regular_file_exists || !regular_file_exists(entry_point_path)) {
			resolution.unavailable_reason =
				"the chat panel's application files were not found: '" + entry_point_path
					+ "' does not exist. The release archive installs them as "
					+ std::string{application_asset_directory_name}
					+ "/ beside the extension binary; a development build does not put them there";

			return resolution;
		}

		resolution.found = true;

		return resolution;
	}

}

#endif
