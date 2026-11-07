// Finding the CEF subprocess helper beside the extension binary (task 26.1, requirement
// 15.7).
//
// The helper itself links CEF and is not compiled here. What is pinned is where the
// extension looks for it, that every way of not finding it is a reason naming the path,
// and that the name it looks for is the name CMakeLists.txt builds. Whether the creator
// is then installed is `panel_asset_directory_test.cpp`, beside the same decision for the
// chat panel's files.

#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/cef_subprocess_helper_location.h>
#include <entry/panel_asset_directory.h>
#include <ui/ui_host.h>

using sesh_ai::entry::CefSubprocessHelperResolution;
using sesh_ai::entry::RegularFileExistence;
using sesh_ai::entry::resolve_cef_subprocess_helper;
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

	const std::string macos_installed_binary =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/reaper_sesh_ai.dylib";

	const std::string macos_installed_helper =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/Sesh AI Helper.app/Contents/MacOS/Sesh AI Helper";

	std::string macos_helper_name()
	{
		return cef_platform_configuration_for(HostPlatform::macos).subprocess_executable_name;
	}

	// The last path segment, which is the executable's own file name.
	std::string final_path_segment(const std::string& path)
	{
		const std::size_t last_separator = path.find_last_of("/\\");

		return last_separator == std::string::npos ? path : path.substr(last_separator + 1);
	}

}

// ---------------------------------------------------------------------------
// Where the helper is looked for
// ---------------------------------------------------------------------------

TEST_CASE("the helper is the platform's helper name beside the extension binary", "[entry][cef_subprocess_helper]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files.push_back(macos_installed_helper);

	const CefSubprocessHelperResolution resolution =
		resolve_cef_subprocess_helper(macos_installed_binary, macos_helper_name(), filesystem.seam());

	CHECK(resolution.found);
	CHECK(resolution.subprocess_path == macos_installed_helper);
	CHECK(resolution.unavailable_reason.empty());

	// The executable inside the bundle, not the bundle directory: that is the file CEF
	// starts, and a bundle with no executable in it is as missing as no bundle.
	CHECK(filesystem.paths_asked_about == std::vector<std::string>{macos_installed_helper});
}

TEST_CASE("a Windows binary path keeps its own separator for the helper", "[entry][cef_subprocess_helper]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files.push_back(
		"C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\sesh_ai_cef_helper.exe"
	);

	const CefSubprocessHelperResolution resolution = resolve_cef_subprocess_helper(
		"C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\reaper_sesh_ai.dll",
		cef_platform_configuration_for(HostPlatform::windows).subprocess_executable_name,
		filesystem.seam()
	);

	CHECK(resolution.found);
	CHECK(resolution.subprocess_path
		== "C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\sesh_ai_cef_helper.exe");
}

TEST_CASE("a helper that is not beside the binary is a reason naming where it was looked for", "[entry][cef_subprocess_helper]")
{
	// Every macOS development build until the helper bundle exists.
	RecordingFilesystem filesystem;

	const CefSubprocessHelperResolution resolution =
		resolve_cef_subprocess_helper(macos_installed_binary, macos_helper_name(), filesystem.seam());

	CHECK_FALSE(resolution.found);

	// Still filled, so the creator can be handed the path that was looked at.
	CHECK(resolution.subprocess_path == macos_installed_helper);
	CHECK(resolution.unavailable_reason.find("CEF subprocess helper was not found") != std::string::npos);
	CHECK(resolution.unavailable_reason.find(macos_installed_helper) != std::string::npos);
}

TEST_CASE("every way of not having an absolute helper path is reported, and the filesystem is not asked", "[entry][cef_subprocess_helper]")
{
	RecordingFilesystem filesystem;

	SECTION("the platform did not say where the binary is")
	{
		const CefSubprocessHelperResolution resolution =
			resolve_cef_subprocess_helper("", macos_helper_name(), filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.subprocess_path.empty());
		CHECK(resolution.unavailable_reason.find("could not determine where its own binary") != std::string::npos);
		CHECK(resolution.unavailable_reason.find(macos_helper_name()) != std::string::npos);
	}

	SECTION("a bare file name names no directory")
	{
		const CefSubprocessHelperResolution resolution =
			resolve_cef_subprocess_helper("reaper_sesh_ai.so", "sesh_ai_cef_helper", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("names no directory") != std::string::npos);
	}

	SECTION("a relative path, which CEF refuses for browser_subprocess_path")
	{
		const CefSubprocessHelperResolution resolution = resolve_cef_subprocess_helper(
			"UserPlugins/reaper_sesh_ai.so",
			"sesh_ai_cef_helper",
			filesystem.seam()
		);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.subprocess_path == "UserPlugins/sesh_ai_cef_helper");
		CHECK(resolution.unavailable_reason.find("is not absolute") != std::string::npos);
	}

	SECTION("a platform with no helper name")
	{
		const CefSubprocessHelperResolution resolution =
			resolve_cef_subprocess_helper(macos_installed_binary, "", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("no CEF subprocess helper is named") != std::string::npos);
	}

	CHECK(filesystem.paths_asked_about.empty());
}

TEST_CASE("an unwired filesystem seam finds no helper rather than assuming one", "[entry][cef_subprocess_helper]")
{
	const CefSubprocessHelperResolution resolution =
		resolve_cef_subprocess_helper(macos_installed_binary, macos_helper_name(), RegularFileExistence{});

	CHECK_FALSE(resolution.found);
	CHECK_FALSE(resolution.unavailable_reason.empty());
}

// ---------------------------------------------------------------------------
// The name looked for is the name built
// ---------------------------------------------------------------------------

TEST_CASE("the helper the extension looks for is the executable CMakeLists.txt builds", "[entry][cef_subprocess_helper]")
{
	// SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME is the target's OUTPUT_NAME, handed to
	// the suite by tests/CMakeLists.txt. The platform configuration is written separately
	// in src/ui/ui_host.h, so this is the one place the two are compared.
	std::string built_executable_name{SESH_AI_CEF_SUBPROCESS_HELPER_OUTPUT_NAME};

	if (sesh_ai::ui::current_host_platform() == HostPlatform::windows) {
		built_executable_name.append(".exe");
	}

	const std::string looked_for =
		cef_platform_configuration_for(sesh_ai::ui::current_host_platform()).subprocess_executable_name;

	CHECK(final_path_segment(looked_for) == built_executable_name);
}

TEST_CASE("every platform names a helper", "[entry][cef_subprocess_helper]")
{
	// An empty name is what would let CEF fall back to relaunching REAPER.
	for (const HostPlatform platform : {HostPlatform::macos, HostPlatform::windows, HostPlatform::linux}) {
		CHECK_FALSE(cef_platform_configuration_for(platform).subprocess_executable_name.empty());
	}
}
