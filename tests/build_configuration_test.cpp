// Build configuration smoke test.
//
// The suite proper arrives with the components it covers. Until then this file
// keeps the test target honest: it proves the target compiles, links Catch2, and
// gives ctest something to run, so a broken build system surfaces here rather
// than in the first task that writes a real test.
//
// The assertion is not decorative. std::string_view and constexpr comparison on
// it only compile under C++17, which is checked here rather than through
// __cplusplus — MSVC reports 199711L for that macro unless /Zc:__cplusplus is
// passed, and the Windows build leg would fail on an otherwise correct
// configuration.

#include <string_view>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the build provides the C++17 standard library", "[build]")
{
	constexpr std::string_view language_standard{"C++17"};

	STATIC_REQUIRE(language_standard.size() == 5);
	REQUIRE(language_standard == "C++17");
}
