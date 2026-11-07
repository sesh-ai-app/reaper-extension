// Finding CEF's library beside the extension binary: the Chromium Embedded Framework and
// CEF's main bundle on macOS (task 26.2), and `libcef.dll` on Windows (task 32.2), for
// requirements 1.1 and 15.7.
//
// `cef_load_library`, `LoadLibraryExW`, and CEF itself are not reached here. What is
// pinned is where the extension looks, that every way of not finding the library is a
// reason naming the paths, that Linux needs nothing, and that the helper bundle names the
// platform configuration lists are the ones Chromium will derive from the one the
// extension hands it. The Windows paths are plain strings with backslashes, so they are
// checked here on every platform. Whether the creator is then installed is mostly
// `panel_asset_directory_test.cpp`.

#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/cef_framework_location.h>
#include <entry/cef_subprocess_helper_location.h>
#include <entry/panel_asset_directory.h>
#include <entry/panel_browser_creator.h>
#include <ui/ui_host.h>

using sesh_ai::entry::ApplicationAssetDirectoryResolution;
using sesh_ai::entry::CefFrameworkResolution;
using sesh_ai::entry::CefSubprocessHelperResolution;
using sesh_ai::entry::PanelBrowserInstallation;
using sesh_ai::entry::PanelBrowserSettings;
using sesh_ai::entry::RegularFileExistence;
using sesh_ai::entry::directory_containing;
using sesh_ai::entry::framework_executable_name;
using sesh_ai::entry::install_panel_browser_creator;
using sesh_ai::entry::is_fully_qualified_windows_path;
using sesh_ai::entry::resolve_cef_framework;
using sesh_ai::entry::resolve_cef_library;
using sesh_ai::entry::resolve_cef_windows_library;
using sesh_ai::ui::CefPlatformConfiguration;
using sesh_ai::ui::HostPlatform;
using sesh_ai::ui::cef_platform_configuration_for;

namespace {

	// Stands in for the filesystem: the files that exist, and every path asked about.
	struct RecordingFilesystem {
		std::vector<std::string> existing_files;
		std::vector<std::string> paths_asked_about;

		RegularFileExistence seam()
		{
			return [this](const std::string& file_path) {
				paths_asked_about.push_back(file_path);

				for (const std::string& existing : existing_files) {
					if (existing == file_path) {
						return true;
					}
				}

				return false;
			};
		}
	};

	const std::string user_plugins = "/Users/producer/Library/Application Support/REAPER/UserPlugins";

	const std::string installed_binary = user_plugins + "/reaper_sesh_ai.dylib";

	const std::string installed_framework_directory = user_plugins + "/Chromium Embedded Framework.framework";

	const std::string installed_framework_library = installed_framework_directory + "/Chromium Embedded Framework";

	const std::string installed_main_bundle = user_plugins + "/Sesh AI Helper.app";

	const std::string installed_main_bundle_information = installed_main_bundle + "/Contents/Info.plist";

	CefFrameworkResolution resolve_for_macos(std::string_view binary_path, RecordingFilesystem& filesystem)
	{
		const CefPlatformConfiguration configuration = cef_platform_configuration_for(HostPlatform::macos);

		return resolve_cef_framework(
			binary_path,
			configuration.framework_bundle_name,
			configuration.main_bundle_name,
			filesystem.seam()
		);
	}

	// What `GetModuleFileNameW` answers for the extension installed in REAPER's default
	// resource path, as UTF-8.
	const std::string windows_user_plugins = "C:\\Users\\Producer\\AppData\\Roaming\\REAPER\\UserPlugins";

	const std::string windows_installed_binary = windows_user_plugins + "\\reaper_sesh_ai.dll";

	const std::string windows_installed_library = windows_user_plugins + "\\libcef.dll";

	CefFrameworkResolution resolve_for_windows(std::string_view binary_path, RecordingFilesystem& filesystem)
	{
		return resolve_cef_library(
			binary_path,
			cef_platform_configuration_for(HostPlatform::windows),
			filesystem.seam()
		);
	}

}

// ---------------------------------------------------------------------------
// Where the framework and the main bundle are looked for
// ---------------------------------------------------------------------------

TEST_CASE("the framework and the main bundle are beside the extension binary", "[entry][cef_framework]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files = {installed_framework_library, installed_main_bundle_information};

	const CefFrameworkResolution resolution = resolve_for_macos(installed_binary, filesystem);

	CHECK(resolution.found);
	CHECK(resolution.extension_loads_cef_library);
	CHECK(resolution.framework_library_path == installed_framework_library);
	CHECK(resolution.framework_directory == installed_framework_directory);
	CHECK(resolution.main_bundle_path == installed_main_bundle);
	CHECK(resolution.unavailable_reason.empty());

	// The executable `cef_load_library` opens, and the Info.plist `NSBundle` reads: an
	// empty bundle directory would stop REAPER in a Chromium CHECK.
	CHECK(filesystem.paths_asked_about
		== std::vector<std::string>{installed_framework_library, installed_main_bundle_information});
}

TEST_CASE("a missing framework or main bundle is a reason naming every path that is missing", "[entry][cef_framework]")
{
	SECTION("both missing, which is a development build before anything is placed")
	{
		RecordingFilesystem filesystem;

		const CefFrameworkResolution resolution = resolve_for_macos(installed_binary, filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.framework_library_path == installed_framework_library);
		CHECK(resolution.main_bundle_path == installed_main_bundle);
		CHECK(resolution.unavailable_reason.find(installed_framework_library) != std::string::npos);
		CHECK(resolution.unavailable_reason.find(installed_main_bundle_information) != std::string::npos);
		CHECK(resolution.unavailable_reason.find("do not exist") != std::string::npos);

		// Both asked about, so one line names everything to put in place.
		CHECK(filesystem.paths_asked_about.size() == 2);
	}

	SECTION("only the main bundle missing")
	{
		RecordingFilesystem filesystem;
		filesystem.existing_files = {installed_framework_library};

		const CefFrameworkResolution resolution = resolve_for_macos(installed_binary, filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find(installed_framework_library) == std::string::npos);
		CHECK(resolution.unavailable_reason.find("'" + installed_main_bundle_information + "' does not exist")
			!= std::string::npos);
	}
}

TEST_CASE("every way of not having absolute paths is reported, and the filesystem is not asked", "[entry][cef_framework]")
{
	RecordingFilesystem filesystem;
	const CefPlatformConfiguration configuration = cef_platform_configuration_for(HostPlatform::macos);

	SECTION("the platform did not say where the binary is")
	{
		const CefFrameworkResolution resolution = resolve_for_macos("", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.framework_directory.empty());
		CHECK(resolution.unavailable_reason.find("could not determine where its own binary") != std::string::npos);
	}

	SECTION("a bare file name names no directory")
	{
		const CefFrameworkResolution resolution = resolve_for_macos("reaper_sesh_ai.dylib", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("names no directory") != std::string::npos);
	}

	SECTION("a relative path, which CEF refuses for framework_dir_path")
	{
		const CefFrameworkResolution resolution = resolve_for_macos("UserPlugins/reaper_sesh_ai.dylib", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.framework_directory == "UserPlugins/Chromium Embedded Framework.framework");
		CHECK(resolution.unavailable_reason.find("is not absolute") != std::string::npos);
	}

	SECTION("a framework name that does not end in .framework")
	{
		const CefFrameworkResolution resolution = resolve_cef_framework(
			installed_binary,
			"Chromium Embedded Framework",
			configuration.main_bundle_name,
			filesystem.seam()
		);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("does not end in '.framework'") != std::string::npos);
	}

	SECTION("a framework with no main bundle named")
	{
		const CefFrameworkResolution resolution = resolve_cef_framework(
			installed_binary,
			configuration.framework_bundle_name,
			"",
			filesystem.seam()
		);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("no main bundle is named") != std::string::npos);
	}

	CHECK(filesystem.paths_asked_about.empty());
}

TEST_CASE("an unwired filesystem seam finds no framework rather than assuming one", "[entry][cef_framework]")
{
	const CefPlatformConfiguration configuration = cef_platform_configuration_for(HostPlatform::macos);

	const CefFrameworkResolution resolution = resolve_cef_framework(
		installed_binary,
		configuration.framework_bundle_name,
		configuration.main_bundle_name,
		RegularFileExistence{}
	);

	CHECK_FALSE(resolution.found);
	CHECK_FALSE(resolution.unavailable_reason.empty());
}

TEST_CASE("with no framework named there is no framework to find", "[entry][cef_framework]")
{
	// `resolve_cef_framework` is macOS's half. Windows and Linux name no framework, and
	// `resolve_cef_library` sends neither of them here.
	for (const HostPlatform platform : {HostPlatform::windows, HostPlatform::linux}) {
		RecordingFilesystem filesystem;
		const CefPlatformConfiguration configuration = cef_platform_configuration_for(platform);

		const CefFrameworkResolution resolution = resolve_cef_framework(
			"/opt/REAPER/UserPlugins/reaper_sesh_ai.so",
			configuration.framework_bundle_name,
			configuration.main_bundle_name,
			filesystem.seam()
		);

		CHECK(resolution.found);
		CHECK_FALSE(resolution.extension_loads_cef_library);
		CHECK(resolution.framework_library_path.empty());
		CHECK(resolution.framework_directory.empty());
		CHECK(resolution.main_bundle_path.empty());
		CHECK(filesystem.paths_asked_about.empty());
	}
}

TEST_CASE("the framework's executable is its bundle name without .framework", "[entry][cef_framework]")
{
	CHECK(framework_executable_name("Chromium Embedded Framework.framework") == "Chromium Embedded Framework");
	CHECK(framework_executable_name("Chromium Embedded Framework").empty());
	CHECK(framework_executable_name(".framework").empty());
	CHECK(framework_executable_name("").empty());
}

// ---------------------------------------------------------------------------
// The bundle names agree with each other and with Chromium
// ---------------------------------------------------------------------------

TEST_CASE("the main bundle is the plain helper bundle the subprocess path is inside", "[entry][cef_framework]")
{
	const CefPlatformConfiguration configuration = cef_platform_configuration_for(HostPlatform::macos);

	REQUIRE_FALSE(configuration.helper_process_bundle_names.empty());
	CHECK(configuration.main_bundle_name == configuration.helper_process_bundle_names.front());
	CHECK(configuration.subprocess_executable_name.find(configuration.main_bundle_name + "/Contents/MacOS/") == 0);

	// Chromium only derives the suffixed helpers' paths when the main bundle's path ends
	// in `.app` (`base::apple::AmIBundled`).
	const std::string application_extension = ".app";
	CHECK(configuration.main_bundle_name.size() > application_extension.size());
	CHECK(configuration.main_bundle_name.substr(configuration.main_bundle_name.size() - application_extension.size())
		== application_extension);
}

TEST_CASE("every suffixed helper Chromium derives from the subprocess path is a bundle the platform lists", "[entry][cef_framework]")
{
	// What `ChildProcessHost::GetChildPath` does (content/browser/child_process_host_impl.cc
	// at Chromium 154.0.8037.58): take the subprocess path's base name, go four
	// directories up, and append "<base><suffix>.app/Contents/MacOS/<base><suffix>". The
	// suffixes are CEF_HELPER_APP_SUFFIXES' name suffixes in the distribution's
	// cmake/cef_variables.cmake, which CMakeLists.txt builds one bundle per.
	const CefPlatformConfiguration configuration = cef_platform_configuration_for(HostPlatform::macos);
	const std::string subprocess_path = user_plugins + "/" + configuration.subprocess_executable_name;

	const std::string base_name = subprocess_path.substr(subprocess_path.find_last_of('/') + 1);

	std::string four_directories_up = subprocess_path;

	for (int level = 0; level < 4; ++level) {
		four_directories_up = std::string{directory_containing(four_directories_up)};
	}

	CHECK(four_directories_up == user_plugins);

	for (const std::string suffix : {" (Alerts)", " (GPU)", " (Plugin)", " (Renderer)"}) {
		const std::string derived_bundle = base_name + suffix + ".app";
		const std::string derived_path =
			four_directories_up + "/" + derived_bundle + "/Contents/MacOS/" + base_name + suffix;

		CAPTURE(derived_path);

		bool listed = false;

		for (const std::string& bundle_name : configuration.helper_process_bundle_names) {
			listed = listed || bundle_name == derived_bundle;
		}

		CHECK(listed);

		// In the same directory as the plain helper. LoadInHelper looks for the framework
		// at `<executable directory>/../../..`, so each helper finds it there too.
		std::string helper_directory = derived_path;

		for (int level = 0; level < 4; ++level) {
			helper_directory = std::string{directory_containing(helper_directory)};
		}

		CHECK(helper_directory == user_plugins);
	}
}

TEST_CASE("only macOS names a framework and a main bundle", "[entry][cef_framework]")
{
	CHECK(cef_platform_configuration_for(HostPlatform::macos).framework_bundle_name
		== "Chromium Embedded Framework.framework");
	CHECK(cef_platform_configuration_for(HostPlatform::macos).main_bundle_name == "Sesh AI Helper.app");

	for (const HostPlatform platform : {HostPlatform::windows, HostPlatform::linux}) {
		CHECK(cef_platform_configuration_for(platform).framework_bundle_name.empty());
		CHECK(cef_platform_configuration_for(platform).main_bundle_name.empty());
	}
}

// ---------------------------------------------------------------------------
// Windows: libcef.dll beside the extension binary
// ---------------------------------------------------------------------------

TEST_CASE("on Windows libcef.dll is beside the extension binary", "[entry][cef_framework][windows]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files = {windows_installed_library};

	const CefFrameworkResolution resolution = resolve_for_windows(windows_installed_binary, filesystem);

	CHECK(resolution.found);
	CHECK(resolution.extension_loads_cef_library);
	CHECK(resolution.framework_library_path == windows_installed_library);
	CHECK(resolution.unavailable_reason.empty());

	// CEF's `framework_dir_path` and `main_bundle_path` are macOS settings.
	CHECK(resolution.framework_directory.empty());
	CHECK(resolution.main_bundle_path.empty());

	CHECK(filesystem.paths_asked_about == std::vector<std::string>{windows_installed_library});
}

TEST_CASE("a missing libcef.dll is reported and drops the creator, rather than failing the extension's load", "[entry][cef_framework][windows]")
{
	RecordingFilesystem filesystem;

	const CefFrameworkResolution resolution = resolve_for_windows(windows_installed_binary, filesystem);

	CHECK_FALSE(resolution.found);
	CHECK(resolution.framework_library_path == windows_installed_library);
	CHECK(resolution.unavailable_reason.find("'" + windows_installed_library + "' does not exist")
		!= std::string::npos);

	// What the plugin entry does with it: the creator is dropped, so CEF is never
	// initialized and nothing calls into the missing library, and the reason is the line
	// the panel's empty state is logged with.
	ApplicationAssetDirectoryResolution application_assets;
	application_assets.found = true;
	application_assets.asset_directory = windows_user_plugins + "\\sesh_ai_ui";

	CefSubprocessHelperResolution subprocess_helper;
	subprocess_helper.found = true;
	subprocess_helper.subprocess_path = windows_user_plugins + "\\sesh_ai_cef_helper.exe";

	std::vector<PanelBrowserSettings> settings_given;

	const PanelBrowserInstallation installation = install_panel_browser_creator(
		application_assets,
		subprocess_helper,
		resolution,
		[&settings_given](const PanelBrowserSettings& settings) {
			settings_given.push_back(settings);

			PanelBrowserInstallation installed;
			installed.creator = [](void*) { return true; };

			return installed;
		}
	);

	CHECK_FALSE(installation.installed());
	CHECK(installation.unavailable_reason == resolution.unavailable_reason);

	REQUIRE(settings_given.size() == 1);
	CHECK(settings_given.front().runtime_layout.framework_library_path == windows_installed_library);
	CHECK(settings_given.front().runtime_layout.framework_directory.empty());
	CHECK(settings_given.front().runtime_layout.main_bundle_path.empty());
}

TEST_CASE("a non-ASCII Windows path keeps its UTF-8 bytes", "[entry][cef_framework][windows]")
{
	// "Zoë" in UTF-8. The resolver joins bytes and never re-encodes, so the path the
	// loader converts to UTF-16 is the one `GetModuleFileNameW` gave.
	const std::string user_plugins = "C:\\Users\\Zo\xC3\xAB\\AppData\\Roaming\\REAPER\\UserPlugins";

	RecordingFilesystem filesystem;
	filesystem.existing_files = {user_plugins + "\\libcef.dll"};

	const CefFrameworkResolution resolution = resolve_for_windows(user_plugins + "\\reaper_sesh_ai.dll", filesystem);

	CHECK(resolution.found);
	CHECK(resolution.framework_library_path == user_plugins + "\\libcef.dll");
}

TEST_CASE("a Windows library path uses backslashes whatever the binary path used", "[entry][cef_framework][windows]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files = {windows_installed_library};

	SECTION("forward slashes throughout")
	{
		const CefFrameworkResolution resolution = resolve_for_windows(
			"C:/Users/Producer/AppData/Roaming/REAPER/UserPlugins/reaper_sesh_ai.dll",
			filesystem
		);

		CHECK(resolution.found);
		CHECK(resolution.framework_library_path == windows_installed_library);
	}

	SECTION("mixed")
	{
		const CefFrameworkResolution resolution = resolve_for_windows(
			"C:\\Users\\Producer/AppData\\Roaming/REAPER\\UserPlugins/reaper_sesh_ai.dll",
			filesystem
		);

		CHECK(resolution.found);
		CHECK(resolution.framework_library_path == windows_installed_library);
	}
}

TEST_CASE("a UNC install is fully qualified", "[entry][cef_framework][windows]")
{
	const std::string user_plugins = "\\\\studio-server\\plugins\\REAPER\\UserPlugins";

	RecordingFilesystem filesystem;
	filesystem.existing_files = {user_plugins + "\\libcef.dll"};

	const CefFrameworkResolution resolution = resolve_for_windows(user_plugins + "\\reaper_sesh_ai.dll", filesystem);

	CHECK(resolution.found);
	CHECK(resolution.framework_library_path == user_plugins + "\\libcef.dll");
}

TEST_CASE("every way of not having a fully qualified Windows path is reported, and the filesystem is not asked", "[entry][cef_framework][windows]")
{
	RecordingFilesystem filesystem;

	SECTION("the platform did not say where the binary is")
	{
		const CefFrameworkResolution resolution = resolve_for_windows("", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.extension_loads_cef_library);
		CHECK(resolution.framework_library_path.empty());
		CHECK(resolution.unavailable_reason.find("could not determine where its own binary") != std::string::npos);
	}

	SECTION("a bare file name names no directory")
	{
		const CefFrameworkResolution resolution = resolve_for_windows("reaper_sesh_ai.dll", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("names no directory") != std::string::npos);
	}

	SECTION("relative to the current drive, which the URL rules would call absolute")
	{
		const CefFrameworkResolution resolution =
			resolve_for_windows("\\Users\\Producer\\UserPlugins\\reaper_sesh_ai.dll", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.framework_library_path == "\\Users\\Producer\\UserPlugins\\libcef.dll");
		CHECK(resolution.unavailable_reason.find("is not fully qualified") != std::string::npos);
	}

	SECTION("relative to a drive's current directory")
	{
		const CefFrameworkResolution resolution = resolve_for_windows("C:UserPlugins\\reaper_sesh_ai.dll", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("is not fully qualified") != std::string::npos);
	}

	SECTION("relative to the current directory")
	{
		const CefFrameworkResolution resolution = resolve_for_windows("UserPlugins\\reaper_sesh_ai.dll", filesystem);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("is not fully qualified") != std::string::npos);
	}

	SECTION("no library named")
	{
		const CefFrameworkResolution resolution =
			resolve_cef_windows_library(windows_installed_binary, "", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("no CEF library file is named") != std::string::npos);
	}

	CHECK(filesystem.paths_asked_about.empty());
}

TEST_CASE("an unwired filesystem seam finds no libcef.dll rather than assuming one", "[entry][cef_framework][windows]")
{
	const CefFrameworkResolution resolution = resolve_cef_library(
		windows_installed_binary,
		cef_platform_configuration_for(HostPlatform::windows),
		RegularFileExistence{}
	);

	CHECK_FALSE(resolution.found);
	CHECK(resolution.framework_library_path == windows_installed_library);
	CHECK_FALSE(resolution.unavailable_reason.empty());
}

TEST_CASE("a fully qualified Windows path has a drive and a separator, or starts with two separators", "[entry][cef_framework][windows]")
{
	CHECK(is_fully_qualified_windows_path("C:\\REAPER\\libcef.dll"));
	CHECK(is_fully_qualified_windows_path("c:/REAPER/libcef.dll"));
	CHECK(is_fully_qualified_windows_path("\\\\server\\share\\libcef.dll"));
	CHECK(is_fully_qualified_windows_path("\\\\?\\C:\\REAPER\\libcef.dll"));

	CHECK_FALSE(is_fully_qualified_windows_path(""));
	CHECK_FALSE(is_fully_qualified_windows_path("\\\\"));
	CHECK_FALSE(is_fully_qualified_windows_path("\\REAPER\\libcef.dll"));
	CHECK_FALSE(is_fully_qualified_windows_path("C:REAPER\\libcef.dll"));
	CHECK_FALSE(is_fully_qualified_windows_path("REAPER\\libcef.dll"));
	CHECK_FALSE(is_fully_qualified_windows_path("1:\\REAPER\\libcef.dll"));
}

// ---------------------------------------------------------------------------
// Which resolution each platform gets
// ---------------------------------------------------------------------------

TEST_CASE("each platform's configuration chooses its own resolution", "[entry][cef_framework]")
{
	SECTION("macOS resolves the framework and the main bundle")
	{
		RecordingFilesystem through_dispatch;
		through_dispatch.existing_files = {installed_framework_library, installed_main_bundle_information};

		RecordingFilesystem directly;
		directly.existing_files = through_dispatch.existing_files;

		const CefFrameworkResolution dispatched = resolve_cef_library(
			installed_binary,
			cef_platform_configuration_for(HostPlatform::macos),
			through_dispatch.seam()
		);
		const CefFrameworkResolution direct = resolve_for_macos(installed_binary, directly);

		CHECK(dispatched.found == direct.found);
		CHECK(dispatched.framework_library_path == direct.framework_library_path);
		CHECK(dispatched.framework_directory == direct.framework_directory);
		CHECK(dispatched.main_bundle_path == direct.main_bundle_path);
		CHECK(through_dispatch.paths_asked_about == directly.paths_asked_about);
	}

	SECTION("Linux links libcef.so, so there is nothing to find")
	{
		RecordingFilesystem filesystem;

		const CefFrameworkResolution resolution = resolve_cef_library(
			"/opt/REAPER/UserPlugins/reaper_sesh_ai.so",
			cef_platform_configuration_for(HostPlatform::linux),
			filesystem.seam()
		);

		CHECK(resolution.found);
		CHECK_FALSE(resolution.extension_loads_cef_library);
		CHECK(resolution.framework_library_path.empty());
		CHECK(filesystem.paths_asked_about.empty());
	}
}
