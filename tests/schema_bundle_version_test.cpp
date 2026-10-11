// The generated protocolVersion header.
//
// The value comes out of MANIFEST.json at configure time, so what can go wrong is
// the extraction rather than the value: a renamed manifest field, a JSON read that
// silently produced nothing, or a template substitution that left the placeholder
// in place. Each of those yields a header that compiles and a handshake the server
// rejects for a reason nobody would guess from the error. Checking the shape here
// turns all three into a failed test on this machine.
//
// This does not re-check the digests. That is a build step and has already run by
// the time this binary exists — see cmake/VerifySchemaBundle.cmake.

#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <transport/schema_bundle_version.h>

TEST_CASE("the vendored bundle's protocolVersion reaches the C++ side", "[schemas]")
{
	constexpr std::string_view protocol_version{sesh_ai::schema_bundle::protocol_version};

	SECTION("it is populated")
	{
		STATIC_REQUIRE(!protocol_version.empty());
		REQUIRE(protocol_version.find('@') == std::string_view::npos);
	}

	SECTION("it has the shape the bundler produces")
	{
		// Twelve lowercase hexadecimal characters, derived from the file digests.
		// Anything else means the manifest was hand-edited into a semver or the
		// wrong field was read.
		REQUIRE(protocol_version.size() == 12);

		const bool every_character_is_lowercase_hexadecimal = std::all_of(
			protocol_version.begin(),
			protocol_version.end(),
			[](char character) {
				return (character >= '0' && character <= '9')
					|| (character >= 'a' && character <= 'f');
			}
		);

		REQUIRE(every_character_is_lowercase_hexadecimal);
	}
}
