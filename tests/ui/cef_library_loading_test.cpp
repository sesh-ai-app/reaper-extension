// What the extension reports when `libcef.dll` is there and will not load, and the
// module name the delay-load hook answers for (task 32.2, requirements 1.1 and 15.7).
//
// `ui/cef_library_loading_windows.cpp` is the `LoadLibraryExW` call and the hook, and
// only the Windows build compiles it. What is pinned here is the part that does not
// need Windows: the line a failed load is logged with, and that the three places naming
// the library agree with each other.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ui/cef_library_loading.h>
#include <ui/ui_host.h>

using sesh_ai::ui::HostPlatform;
using sesh_ai::ui::cef_library_file_name_on_windows;
using sesh_ai::ui::cef_platform_configuration_for;
using sesh_ai::ui::describe_cef_library_load_failure;
using sesh_ai::ui::windows_error_bad_executable_format;
using sesh_ai::ui::windows_error_module_not_found;

namespace {

	const std::string installed_library = "C:\\Users\\Producer\\AppData\\Roaming\\REAPER\\UserPlugins\\libcef.dll";

}

TEST_CASE("a library that is there and will not load names a missing dependency", "[ui][cef_library_loading]")
{
	// The resolver found the file, so the module Windows could not find is one of its
	// imports. A line saying only "not found" would send a producer looking for the file
	// that is there.
	const std::string reason = describe_cef_library_load_failure(installed_library, windows_error_module_not_found);

	CHECK(reason.find("'" + installed_library + "'") != std::string::npos);
	CHECK(reason.find("a library it depends on was not found") != std::string::npos);
	CHECK(reason.find("chrome_elf.dll") != std::string::npos);
	CHECK(reason.find("(Windows error 126)") != std::string::npos);
}

TEST_CASE("a library for another architecture says so", "[ui][cef_library_loading]")
{
	const std::string reason =
		describe_cef_library_load_failure(installed_library, windows_error_bad_executable_format);

	CHECK(reason.find("another architecture") != std::string::npos);
	CHECK(reason.find("(Windows error 193)") != std::string::npos);
}

TEST_CASE("any other load failure names the path and the error number and guesses nothing", "[ui][cef_library_loading]")
{
	// 5 is ERROR_ACCESS_DENIED.
	const std::string reason = describe_cef_library_load_failure(installed_library, 5);

	CHECK(reason == "the CEF library could not be loaded from '" + installed_library + "' (Windows error 5)");
}

TEST_CASE("the library the hook answers for is the one the Windows configuration names", "[ui][cef_library_loading]")
{
	// The third place is `/DELAYLOAD:libcef.dll` in CMakeLists.txt, which no test reads.
	CHECK(cef_platform_configuration_for(HostPlatform::windows).loader_file_name
		== cef_library_file_name_on_windows);
	CHECK(cef_platform_configuration_for(HostPlatform::windows).loader_must_sit_beside_extension_binary);
}
