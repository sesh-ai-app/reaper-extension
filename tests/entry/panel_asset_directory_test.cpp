// Finding the chat panel's application files, and deciding whether a browser creator is
// installed over them (task 25.2, requirement 15.1).
//
// `entry/extension_binary_location.cpp` is the two platform calls and is not compiled
// here. What is pinned is what their answers mean: where the directory is looked for,
// that every way of not finding it is a reported reason naming the path, and that a
// creator is only installed when there is something for the browser to load and a CEF
// subprocess helper to start it with (task 26.1) — while a factory that declines still
// says why first. Finding the helper is `cef_subprocess_helper_location_test.cpp`.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/cef_framework_location.h>
#include <entry/panel_asset_directory.h>
#include <entry/panel_browser_creator.h>

using sesh_ai::entry::ApplicationAssetDirectoryResolution;
using sesh_ai::entry::CefFrameworkResolution;
using sesh_ai::entry::CefSubprocessHelperResolution;
using sesh_ai::entry::PanelBrowserCreatorFactory;
using sesh_ai::entry::PanelBrowserInstallation;
using sesh_ai::entry::PanelBrowserSettings;
using sesh_ai::entry::RegularFileExistence;
using sesh_ai::entry::directory_containing;
using sesh_ai::entry::install_panel_browser_creator;
using sesh_ai::entry::resolve_application_asset_directory;

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

	const std::string installed_binary =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/reaper_sesh_ai.dylib";

	const std::string installed_asset_directory =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/sesh_ai_ui";

	ApplicationAssetDirectoryResolution found_assets()
	{
		ApplicationAssetDirectoryResolution assets;
		assets.found = true;
		assets.asset_directory = installed_asset_directory;

		return assets;
	}

	ApplicationAssetDirectoryResolution missing_assets()
	{
		ApplicationAssetDirectoryResolution assets;
		assets.asset_directory = installed_asset_directory;
		assets.unavailable_reason = "the chat panel's application files were not found";

		return assets;
	}

	const std::string installed_helper =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/Sesh AI Helper.app/Contents/MacOS/Sesh AI Helper";

	CefSubprocessHelperResolution found_helper()
	{
		CefSubprocessHelperResolution helper;
		helper.found = true;
		helper.subprocess_path = installed_helper;

		return helper;
	}

	CefSubprocessHelperResolution missing_helper()
	{
		CefSubprocessHelperResolution helper;
		helper.subprocess_path = installed_helper;
		helper.unavailable_reason = "the CEF subprocess helper was not found";

		return helper;
	}

	const std::string installed_framework_directory =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/Chromium Embedded Framework.framework";

	const std::string installed_framework_library =
		installed_framework_directory + "/Chromium Embedded Framework";

	const std::string installed_main_bundle =
		"/Users/producer/Library/Application Support/REAPER/UserPlugins/Sesh AI Helper.app";

	CefFrameworkResolution found_framework()
	{
		CefFrameworkResolution framework;
		framework.found = true;
		framework.extension_loads_cef_library = true;
		framework.framework_library_path = installed_framework_library;
		framework.framework_directory = installed_framework_directory;
		framework.main_bundle_path = installed_main_bundle;

		return framework;
	}

	CefFrameworkResolution missing_framework()
	{
		CefFrameworkResolution framework = found_framework();
		framework.found = false;
		framework.unavailable_reason = "the CEF framework layout is incomplete";

		return framework;
	}

	// A factory shaped like the CEF-backed `make_panel_browser_creator`: installs a
	// creator, and records the settings it was given.
	PanelBrowserCreatorFactory installing_factory(std::vector<PanelBrowserSettings>& settings_given)
	{
		return [&settings_given](const PanelBrowserSettings& settings) {
			settings_given.push_back(settings);

			PanelBrowserInstallation installation;
			installation.creator = [](void*) { return true; };

			return installation;
		};
	}

	// Shaped like the CEF-backed `make_panel_browser_creator` refusing a directory
	// `build_local_asset_url` cannot address: no creator, and the reason why.
	const std::string asset_directory_not_addressable_reason =
		"the React application's assets could not be addressed: the asset directory must be an absolute path";

	PanelBrowserCreatorFactory factory_refusing_the_asset_directory()
	{
		return [](const PanelBrowserSettings&) {
			PanelBrowserInstallation installation;
			installation.unavailable_reason = asset_directory_not_addressable_reason;

			return installation;
		};
	}

}

// ---------------------------------------------------------------------------
// Resolving the directory
// ---------------------------------------------------------------------------

TEST_CASE("the application directory is sesh_ai_ui beside the extension binary", "[entry][panel_assets]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files.push_back(installed_asset_directory + "/index.html");

	const ApplicationAssetDirectoryResolution resolution =
		resolve_application_asset_directory(installed_binary, filesystem.seam());

	CHECK(resolution.found);
	CHECK(resolution.asset_directory == installed_asset_directory);
	CHECK(resolution.unavailable_reason.empty());

	// The entry point, not the directory, is what is checked: an empty directory is as
	// blank a panel as a missing one.
	CHECK(filesystem.paths_asked_about == std::vector<std::string>{installed_asset_directory + "/index.html"});
}

TEST_CASE("a Windows binary path keeps its own separator", "[entry][panel_assets]")
{
	RecordingFilesystem filesystem;
	filesystem.existing_files.push_back(
		"C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\sesh_ai_ui\\index.html"
	);

	const ApplicationAssetDirectoryResolution resolution = resolve_application_asset_directory(
		"C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\reaper_sesh_ai.dll",
		filesystem.seam()
	);

	CHECK(resolution.found);
	CHECK(resolution.asset_directory == "C:\\Users\\producer\\AppData\\Roaming\\REAPER\\UserPlugins\\sesh_ai_ui");
}

TEST_CASE("assets that are not beside the binary are a reason naming where they were looked for", "[entry][panel_assets]")
{
	// Every development build: nothing puts `ui/dist` beside `build-vcpkg/bin`.
	RecordingFilesystem filesystem;

	const ApplicationAssetDirectoryResolution resolution =
		resolve_application_asset_directory(installed_binary, filesystem.seam());

	CHECK_FALSE(resolution.found);

	// Still filled, so the creator can be handed the path that was looked at.
	CHECK(resolution.asset_directory == installed_asset_directory);
	CHECK(resolution.unavailable_reason.find(installed_asset_directory + "/index.html") != std::string::npos);
	CHECK(resolution.unavailable_reason.find("sesh_ai_ui/") != std::string::npos);
}

TEST_CASE("every way of not knowing where the binary is is reported, and the filesystem is not asked", "[entry][panel_assets]")
{
	RecordingFilesystem filesystem;

	SECTION("the platform did not say where the binary is")
	{
		const ApplicationAssetDirectoryResolution resolution =
			resolve_application_asset_directory("", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.asset_directory.empty());
		CHECK(resolution.unavailable_reason.find("could not determine where its own binary") != std::string::npos);
	}

	SECTION("a bare file name names no directory")
	{
		const ApplicationAssetDirectoryResolution resolution =
			resolve_application_asset_directory("reaper_sesh_ai.so", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("names no directory") != std::string::npos);
	}

	SECTION("a relative path cannot become a file:// URL")
	{
		const ApplicationAssetDirectoryResolution resolution =
			resolve_application_asset_directory("UserPlugins/reaper_sesh_ai.so", filesystem.seam());

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("the asset directory must be an absolute path") != std::string::npos);
	}

	SECTION("a path with a parent directory segment is refused, as the URL builder refuses it")
	{
		const ApplicationAssetDirectoryResolution resolution = resolve_application_asset_directory(
			"/opt/REAPER/../UserPlugins/reaper_sesh_ai.so",
			filesystem.seam()
		);

		CHECK_FALSE(resolution.found);
		CHECK(resolution.unavailable_reason.find("'..'") != std::string::npos);
	}

	CHECK(filesystem.paths_asked_about.empty());
}

TEST_CASE("an unwired filesystem seam finds nothing rather than assuming", "[entry][panel_assets]")
{
	const ApplicationAssetDirectoryResolution resolution =
		resolve_application_asset_directory(installed_binary, RegularFileExistence{});

	CHECK_FALSE(resolution.found);
	CHECK_FALSE(resolution.unavailable_reason.empty());
}

TEST_CASE("the directory containing a path takes either separator", "[entry][panel_assets]")
{
	CHECK(directory_containing("/a/b/c.so") == "/a/b");
	CHECK(directory_containing("C:\\a\\b.dll") == "C:\\a");
	CHECK(directory_containing("C:\\a/b\\c.dll") == "C:\\a/b");
	CHECK(directory_containing("c.so").empty());
}

TEST_CASE("the directory the extension looks for is the one CMakeLists.txt installs ui/dist as", "[entry][panel_assets]")
{
	// SESH_AI_APPLICATION_ASSET_DIRECTORY_NAME is the install rule's destination, handed
	// to the suite by tests/CMakeLists.txt. If the two drifted apart, the install would
	// succeed and the panel would never find its files.
	CHECK(sesh_ai::entry::application_asset_directory_name == SESH_AI_APPLICATION_ASSET_DIRECTORY_NAME);
}

// ---------------------------------------------------------------------------
// Installing the creator over them
// ---------------------------------------------------------------------------

TEST_CASE("the creator is installed over the found directory", "[entry][panel_assets][panel_browser]")
{
	std::vector<PanelBrowserSettings> settings_given;

	const PanelBrowserInstallation installation =
		install_panel_browser_creator(found_assets(), found_helper(), found_framework(), installing_factory(settings_given));

	CHECK(installation.installed());
	CHECK(installation.unavailable_reason.empty());

	REQUIRE(settings_given.size() == 1);
	CHECK(settings_given.front().application_asset_directory == installed_asset_directory);
	CHECK(settings_given.front().runtime_layout.subprocess_path == installed_helper);
	CHECK(settings_given.front().runtime_layout.framework_library_path == installed_framework_library);
	CHECK(settings_given.front().runtime_layout.framework_directory == installed_framework_directory);
	CHECK(settings_given.front().runtime_layout.main_bundle_path == installed_main_bundle);
}

TEST_CASE("a creator with no framework beside the binary is dropped, and every missing piece is named", "[entry][panel_assets][panel_browser]")
{
	// CEF would initialize with nothing to load, or stop REAPER on a main bundle it
	// cannot open.
	std::vector<PanelBrowserSettings> settings_given;

	SECTION("only the framework missing")
	{
		const PanelBrowserInstallation installation = install_panel_browser_creator(
			found_assets(),
			found_helper(),
			missing_framework(),
			installing_factory(settings_given)
		);

		CHECK_FALSE(installation.installed());
		CHECK(installation.unavailable_reason == missing_framework().unavailable_reason);

		// Asked with the paths that were looked at.
		REQUIRE(settings_given.size() == 1);
		CHECK(settings_given.front().runtime_layout.framework_library_path == installed_framework_library);
	}

	SECTION("everything missing, in the order assets, helper, framework")
	{
		const PanelBrowserInstallation installation = install_panel_browser_creator(
			missing_assets(),
			missing_helper(),
			missing_framework(),
			installing_factory(settings_given)
		);

		CHECK_FALSE(installation.installed());
		CHECK(installation.unavailable_reason
			== missing_assets().unavailable_reason
				+ " Separately, " + missing_helper().unavailable_reason
				+ " Separately, " + missing_framework().unavailable_reason);
	}
}

TEST_CASE("a creator with no helper beside the binary is dropped, and the reason is the helper's", "[entry][panel_assets][panel_browser]")
{
	// CEF would initialize and then have no executable to start the renderer from.
	std::vector<PanelBrowserSettings> settings_given;

	const PanelBrowserInstallation installation =
		install_panel_browser_creator(found_assets(), missing_helper(), found_framework(), installing_factory(settings_given));

	CHECK_FALSE(installation.installed());
	CHECK(installation.unavailable_reason == missing_helper().unavailable_reason);

	// Asked with the path that was looked at, so a refusal from the factory would name it.
	REQUIRE(settings_given.size() == 1);
	CHECK(settings_given.front().runtime_layout.subprocess_path == installed_helper);
}

TEST_CASE("missing assets and a missing helper are both reported, assets first", "[entry][panel_assets][panel_browser]")
{
	std::vector<PanelBrowserSettings> settings_given;

	const PanelBrowserInstallation installation =
		install_panel_browser_creator(missing_assets(), missing_helper(), found_framework(), installing_factory(settings_given));

	CHECK_FALSE(installation.installed());
	CHECK(installation.unavailable_reason.find(missing_assets().unavailable_reason) == 0);
	CHECK(installation.unavailable_reason.find(" Separately, " + missing_helper().unavailable_reason)
		!= std::string::npos);
}

TEST_CASE("a creator over missing assets is dropped, and the reason is the assets'", "[entry][panel_assets][panel_browser]")
{
	// CEF is present and would initialize — irreversibly — to show an error page.
	std::vector<PanelBrowserSettings> settings_given;

	const PanelBrowserInstallation installation = install_panel_browser_creator(
		missing_assets(),
		found_helper(),
		found_framework(),
		installing_factory(settings_given)
	);

	CHECK_FALSE(installation.installed());
	CHECK(installation.unavailable_reason == missing_assets().unavailable_reason);

	// Still asked, with the path that was looked at rather than an empty one.
	REQUIRE(settings_given.size() == 1);
	CHECK(settings_given.front().application_asset_directory == installed_asset_directory);
}

TEST_CASE("a factory that declines says so first, and adds the missing assets rather than hiding them", "[entry][panel_assets][panel_browser]")
{
	// The helper and the framework are missing too, and neither is added to the reason:
	// all three are found beside the same binary, so a directory that cannot be
	// addressed is the cause of their absence rather than a second problem.
	SECTION("assets missing too")
	{
		const PanelBrowserInstallation installation = install_panel_browser_creator(
			missing_assets(),
			missing_helper(),
			missing_framework(),
			factory_refusing_the_asset_directory()
		);

		CHECK_FALSE(installation.installed());
		CHECK(installation.unavailable_reason.find(asset_directory_not_addressable_reason) == 0);
		CHECK(installation.unavailable_reason.find(missing_assets().unavailable_reason) != std::string::npos);
		CHECK(installation.unavailable_reason.find(missing_helper().unavailable_reason) == std::string::npos);
		CHECK(installation.unavailable_reason.find(missing_framework().unavailable_reason) == std::string::npos);
	}

	SECTION("assets present")
	{
		const PanelBrowserInstallation installation = install_panel_browser_creator(
			found_assets(),
			missing_helper(),
			missing_framework(),
			factory_refusing_the_asset_directory()
		);

		CHECK_FALSE(installation.installed());
		CHECK(installation.unavailable_reason == asset_directory_not_addressable_reason);
	}
}

TEST_CASE("no factory means no graph, and that is the reason", "[entry][panel_assets][panel_browser]")
{
	const PanelBrowserInstallation installation = install_panel_browser_creator(
		found_assets(),
		found_helper(),
		found_framework(),
		PanelBrowserCreatorFactory{}
	);

	CHECK_FALSE(installation.installed());
	CHECK(installation.unavailable_reason.find("object graph was not composed") != std::string::npos);
	CHECK(installation.state != nullptr);
}
