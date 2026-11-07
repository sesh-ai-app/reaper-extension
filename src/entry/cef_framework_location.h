// Where CEF's library is, found from where the extension binary was loaded: the
// Chromium Embedded Framework on macOS (task 26.2) and `libcef.dll` on Windows (task
// 32.2), for requirements 1.1 and 15.7.
//
// ---------------------------------------------------------------------------
// Why the framework is not linked
//
// A macOS application links CEF's framework by an install name relative to its own
// executable, `@executable_path/../Frameworks/...`. Inside REAPER, `@executable_path` is
// REAPER's executable, so a linked framework would be looked for inside REAPER's bundle,
// and dyld would refuse to load the extension at all. So the extension does not link it.
// CEF's wrapper on macOS resolves every CEF function through `dlsym` once
// `cef_load_library` has opened the framework, which is how cefsimple works too; the
// difference is only where the path comes from. CefScopedLibraryLoader's `LoadInMain`
// looks in the same wrong place as the linker would, so the extension computes the path
// itself and calls `cef_load_library` with it.
//
// ---------------------------------------------------------------------------
// Three paths, all beside the binary
//
// The release archive, unpacked into `UserPlugins`, and a development build's `bin/` are
// laid out the same way:
//
//   reaper_sesh_ai.dylib
//   Chromium Embedded Framework.framework/
//   Sesh AI Helper.app/   and the (Alerts), (GPU), (Plugin), (Renderer) ones
//   sesh_ai_ui/
//
// From that this resolves:
//
//   framework_library_path  the framework's executable, for `cef_load_library`.
//   framework_directory     the framework bundle, for CEF's `framework_dir_path`. CEF
//                           derives the Resources directory, so the `.pak` files and
//                           locales, from it. Left empty, CEF looks for
//                           `Contents/Frameworks/Chromium Embedded Framework.framework`
//                           inside the main bundle, which is REAPER's.
//   main_bundle_path        the plain helper bundle, for CEF's `main_bundle_path`.
//
// The helpers load the framework themselves. CefScopedLibraryLoader's `LoadInHelper`
// looks three directories above the helper executable, which in this layout is the
// directory the framework is in.
//
// ---------------------------------------------------------------------------
// Why the main bundle is set, and why to the helper bundle
//
// CEF reads Chromium's base bundle identifier from the main bundle's Info.plist
// (`OverrideBaseBundleID` in CEF's libcef/common/util_mac.mm). Chromium names the Mach
// service a helper uses to reach the browser process after that identifier and the
// browser's process id, and the helper looks it up under its own identifier and its
// parent's process id (`kBootstrapNameFormat` in base/apple/mach_port_rendezvous_mac.cc,
// and the `MachPortRendezvousServer` rule in sandbox/policy/mac/common.sb, at Chromium
// 154.0.8037.58). So every process has to arrive at the same identifier.
//
// Left empty, they would not. The browser process would take REAPER's bundle, and each
// helper the outermost `.app` in its own path, which here is the helper's own bundle,
// because the helpers are not inside REAPER's. Set, CEF passes the path to every
// helper on its command line (`AppendExtraCommandLineSwitches` in
// libcef/browser/chrome/chrome_content_browser_client_cef.cc), so all of them agree.
//
// The plain helper bundle rather than REAPER's, for three reasons. Its path is known
// from the binary's own directory, where REAPER's would have to be asked of the system.
// Its identifier is this project's. And it ends in `.app`, which Chromium checks before
// it derives the suffixed helpers' paths at all (`base::apple::AmIBundled`). Whether
// anything else in Chromium reads the main bundle in a way this choice breaks is
// unverified until the panel opens in REAPER (task 28).
//
// The bundle's Info.plist is checked rather than only its directory, because Chromium
// loads the bundle with `NSBundle` and stops the process with a `CHECK` when that fails
// (`BundleFromPath` in base/apple/bundle_locations.mm). The process is REAPER.
//
// ---------------------------------------------------------------------------
// Windows: `libcef.dll`, loaded by full path
//
// The same problem in another form. Windows resolves a DLL's imports through the host
// process's search path, and the host is REAPER, whose directory is not the extension's.
// An extension that imports `libcef.dll` directly is refused by the loader before any of
// its code runs, and the producer sees REAPER report a plug-in that failed to load. So
// the extension delay-loads `libcef.dll` (`CMakeLists.txt` says how) and loads it from
// the path resolved here before the first CEF call (`ui/cef_library_loading.h`).
//
// Windows joins this resolver rather than getting its own, because the question and the
// answer's consumers are the same: one library beside the binary, a path handed to the
// browser host as `framework_library_path`, and a missing file reported through
// `install_panel_browser_creator` the way a missing framework is. Only the layout
// differs. There is no bundle, and no main bundle: CEF's `framework_dir_path` and
// `main_bundle_path` are macOS settings, so those two paths stay empty.
//
// The path has to be fully qualified, which is stricter than the absolute-path check
// the macOS paths get. `\Users\...` is absolute to that check but relative to the
// current drive on Windows, and `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` requires a fully
// qualified path. `GetModuleFileNameW` returns one, so this only refuses a path that
// did not come from it.
//
// ---------------------------------------------------------------------------
// Linux
//
// Nothing to load. `libcef.so` is linked, and found beside the extension through the
// `$ORIGIN` run-path `CMakeLists.txt` sets, so the resolution reports that nothing is
// needed and every path stays empty.

#ifndef SESH_AI_ENTRY_CEF_FRAMEWORK_LOCATION_H
#define SESH_AI_ENTRY_CEF_FRAMEWORK_LOCATION_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <entry/panel_asset_directory.h>
#include <ui/ui_host.h>

namespace sesh_ai::entry {

	// The suffix every framework bundle name carries. The executable inside has the
	// bundle's name without it.
	inline constexpr std::string_view framework_bundle_extension{".framework"};

	struct CefFrameworkResolution {
		// True when the platform has nothing for the extension to load, or when it does
		// and every path is one the platform accepts and everything checked is there.
		bool found = false;

		// True on macOS and Windows, where the extension loads CEF's library itself.
		// False on Linux, where `libcef.so` is linked.
		bool extension_loads_cef_library = false;

		// Filled whenever they could be derived, including when nothing was found there,
		// so the log line can say where it looked.
		//
		// The library CEF's functions come from: the framework's executable on macOS,
		// `libcef.dll` on Windows.
		std::string framework_library_path;

		// macOS only, and empty on Windows.
		std::string framework_directory;
		std::string main_bundle_path;

		// Empty when found. Otherwise the line for the log.
		std::string unavailable_reason;
	};

	// The framework bundle's executable name: the bundle's name without `.framework`.
	// Empty when the name does not end in it.
	inline std::string framework_executable_name(std::string_view framework_bundle_name)
	{
		if (framework_bundle_name.size() <= framework_bundle_extension.size()) {
			return {};
		}

		const std::string_view suffix = framework_bundle_name.substr(
			framework_bundle_name.size() - framework_bundle_extension.size()
		);

		if (suffix != framework_bundle_extension) {
			return {};
		}

		return std::string{framework_bundle_name.substr(
			0,
			framework_bundle_name.size() - framework_bundle_extension.size()
		)};
	}

	// Finds the framework named `framework_bundle_name` and the main bundle named
	// `main_bundle_name` beside the binary at `extension_binary_path`. Both names are
	// `CefPlatformConfiguration`'s; an empty framework name means the platform loads
	// none.
	//
	// The same order as `resolve_cef_subprocess_helper`: the names, the binary path, a
	// path CEF would refuse, and only then the filesystem. Both files are asked about
	// even when the first is missing, so one log line names everything to put in place.
	inline CefFrameworkResolution resolve_cef_framework(
		std::string_view extension_binary_path,
		std::string_view framework_bundle_name,
		std::string_view main_bundle_name,
		const RegularFileExistence& regular_file_exists)
	{
		CefFrameworkResolution resolution;

		if (framework_bundle_name.empty()) {
			resolution.found = true;

			return resolution;
		}

		resolution.extension_loads_cef_library = true;

		const std::string executable_name = framework_executable_name(framework_bundle_name);

		if (executable_name.empty()) {
			resolution.unavailable_reason =
				"the CEF framework bundle name '" + std::string{framework_bundle_name}
					+ "' does not end in '.framework', so the executable inside it cannot be named";

			return resolution;
		}

		if (main_bundle_name.empty()) {
			// `cef_platform_configuration_for` never returns this, and the suite holds it
			// to that.
			resolution.unavailable_reason =
				"no main bundle is named for this platform, so the browser process and the CEF "
				"helpers would not agree on the bundle identifier they find each other by";

			return resolution;
		}

		if (extension_binary_path.empty()) {
			resolution.unavailable_reason =
				"the extension could not determine where its own binary was loaded from, so it "
				"cannot find '" + std::string{framework_bundle_name} + "' beside it";

			return resolution;
		}

		const std::string_view binary_directory = directory_containing(extension_binary_path);

		if (binary_directory.empty()) {
			resolution.unavailable_reason =
				"the extension binary's path '" + std::string{extension_binary_path}
					+ "' names no directory, so the CEF framework cannot be found beside it";

			return resolution;
		}

		const char separator = path_separator_used_by(extension_binary_path);

		resolution.framework_directory = std::string{binary_directory};
		resolution.framework_directory.push_back(separator);
		resolution.framework_directory.append(framework_bundle_name);

		resolution.framework_library_path = resolution.framework_directory;
		resolution.framework_library_path.push_back(separator);
		resolution.framework_library_path.append(executable_name);

		resolution.main_bundle_path = std::string{binary_directory};
		resolution.main_bundle_path.push_back(separator);
		resolution.main_bundle_path.append(main_bundle_name);

		// CEF requires `framework_dir_path` and `main_bundle_path` to be absolute, and
		// all three share one directory, so one check covers them.
		if (!ui::detail::is_absolute_path(resolution.framework_directory)) {
			resolution.unavailable_reason =
				"the CEF framework path '" + resolution.framework_directory
					+ "' is not absolute, and CEF requires an absolute framework_dir_path";

			return resolution;
		}

		std::string main_bundle_information_path = resolution.main_bundle_path;
		main_bundle_information_path.push_back(separator);
		main_bundle_information_path.append("Contents");
		main_bundle_information_path.push_back(separator);
		main_bundle_information_path.append("Info.plist");

		std::vector<std::string> missing_files;

		for (const std::string& required_file : {resolution.framework_library_path, main_bundle_information_path}) {
			if (!regular_file_exists || !regular_file_exists(required_file)) {
				missing_files.push_back(required_file);
			}
		}

		if (!missing_files.empty()) {
			resolution.unavailable_reason = "the CEF framework layout is incomplete: ";

			for (std::size_t index = 0; index < missing_files.size(); ++index) {
				if (index > 0) {
					resolution.unavailable_reason.append(" and ");
				}

				resolution.unavailable_reason.append("'" + missing_files[index] + "'");
			}

			resolution.unavailable_reason.append(
				missing_files.size() == 1 ? " does not exist" : " do not exist"
			);
			resolution.unavailable_reason.append(
				". The release archive installs the framework and the helper bundles beside "
				"the extension binary"
			);

			return resolution;
		}

		resolution.found = true;

		return resolution;
	}

	// Whether a Windows path is fully qualified: a drive letter, a colon, and a
	// separator, or two separators, which is a UNC path or one of the `\\?\` forms.
	// Either separator, because Windows accepts both in a path it is handed.
	inline bool is_fully_qualified_windows_path(std::string_view path)
	{
		const auto is_separator = [](char character) {
			return character == '\\' || character == '/';
		};

		if (path.size() > 2 && is_separator(path[0]) && is_separator(path[1])) {
			return true;
		}

		return ui::detail::is_drive_letter_absolute_path(path);
	}

	// Finds the library named `library_file_name`, `CefPlatformConfiguration`'s
	// `loader_file_name`, beside the Windows binary at `extension_binary_path`.
	//
	// The same order as `resolve_cef_framework`: the name, the binary path, a path
	// `LoadLibraryExW` would refuse, and only then the filesystem.
	inline CefFrameworkResolution resolve_cef_windows_library(
		std::string_view extension_binary_path,
		std::string_view library_file_name,
		const RegularFileExistence& regular_file_exists)
	{
		CefFrameworkResolution resolution;
		resolution.extension_loads_cef_library = true;

		if (library_file_name.empty()) {
			// `cef_platform_configuration_for` never returns this, and the suite holds it
			// to that.
			resolution.unavailable_reason =
				"no CEF library file is named for this platform, so there is nothing to load "
				"CEF's functions from";

			return resolution;
		}

		if (extension_binary_path.empty()) {
			resolution.unavailable_reason =
				"the extension could not determine where its own binary was loaded from, so it "
				"cannot find '" + std::string{library_file_name} + "' beside it";

			return resolution;
		}

		const std::string_view binary_directory = directory_containing(extension_binary_path);

		if (binary_directory.empty()) {
			resolution.unavailable_reason =
				"the extension binary's path '" + std::string{extension_binary_path}
					+ "' names no directory, so '" + std::string{library_file_name}
					+ "' cannot be found beside it";

			return resolution;
		}

		// Backslashes throughout, whatever the binary path used. Microsoft's
		// `LoadLibraryExW` reference asks for them in a path, and Windows does not
		// normalise a `\\?\` path, so a forward slash in one is not a separator.
		resolution.framework_library_path = std::string{binary_directory};
		resolution.framework_library_path.push_back('\\');
		resolution.framework_library_path.append(library_file_name);

		for (char& character : resolution.framework_library_path) {
			if (character == '/') {
				character = '\\';
			}
		}

		if (!is_fully_qualified_windows_path(resolution.framework_library_path)) {
			resolution.unavailable_reason =
				"the CEF library path '" + resolution.framework_library_path
					+ "' is not fully qualified, and the library is loaded only by a fully "
					"qualified path so that Windows does not search REAPER's directories for it";

			return resolution;
		}

		if (!regular_file_exists || !regular_file_exists(resolution.framework_library_path)) {
			resolution.unavailable_reason =
				"the CEF library '" + resolution.framework_library_path
					+ "' does not exist. The release archive installs it beside the extension binary";

			return resolution;
		}

		resolution.found = true;

		return resolution;
	}

	// The resolution for the platform `configuration` describes, which is what the
	// plugin entry asks for. The framework on macOS, `libcef.dll` on Windows, and nothing
	// on Linux, chosen by which of the configuration's names are set rather than by the
	// platform this was compiled for, so the suite can ask about all three.
	inline CefFrameworkResolution resolve_cef_library(
		std::string_view extension_binary_path,
		const ui::CefPlatformConfiguration& configuration,
		const RegularFileExistence& regular_file_exists)
	{
		if (!configuration.framework_bundle_name.empty()) {
			return resolve_cef_framework(
				extension_binary_path,
				configuration.framework_bundle_name,
				configuration.main_bundle_name,
				regular_file_exists
			);
		}

		if (configuration.loader_must_sit_beside_extension_binary) {
			return resolve_cef_windows_library(
				extension_binary_path,
				configuration.loader_file_name,
				regular_file_exists
			);
		}

		CefFrameworkResolution nothing_to_load;
		nothing_to_load.found = true;

		return nothing_to_load;
	}

}

#endif
