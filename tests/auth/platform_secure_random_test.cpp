// The operating system's CSPRNG, exercised for real.
//
// This is the one dependency in the authentication component that is not substituted away in
// the suite, and it is the one that matters most: every guarantee PKCE makes rests on the
// verifier being unpredictable, so "we call the platform CSPRNG" should not be a claim
// nobody has run. The source is header-only rather than a translation unit in the extension
// library precisely so that this file can reach it.
//
// A statistical test cannot prove randomness, and this does not attempt to. What it can
// catch is the failure modes that actually happen to code like this: a call that returns
// success without writing anything, a source that returns the same bytes every time, a
// chunking loop that only fills the first 256 bytes of a longer request, and an off-by-one
// at the end of the buffer.
//
// The only file that includes auth/platform_secure_random.h. On Windows that header pulls in
// <windows.h>, and keeping it to one translation unit is the reason it is separate from
// auth/secure_random.h.

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <auth/pkce.h>
#include <auth/platform_secure_random.h>

using sesh_ai::auth::generate_pkce_pair;
using sesh_ai::auth::is_legal_pkce_code_verifier;
using sesh_ai::auth::pkce_code_challenge_from_verifier;
using sesh_ai::auth::PlatformSecureRandomSource;

TEST_CASE("the platform entropy source fills the buffer it was given", "[auth][random]")
{
	PlatformSecureRandomSource random_source;

	// A sentinel either side of the requested range, so a write that overruns or stops one
	// byte short is caught rather than inferred.
	std::array<unsigned char, 66> buffer{};

	buffer.fill(0xa5u);

	REQUIRE(random_source.fill_random_bytes(buffer.data() + 1, 64));

	REQUIRE(buffer.front() == 0xa5u);
	REQUIRE(buffer.back() == 0xa5u);

	// Every byte of a 64-byte request being untouched would be astronomically unlikely from a
	// working source and is exactly what a silently failing one looks like.
	bool any_byte_changed = false;

	for (std::size_t index = 1; index <= 64; ++index)
	{
		if (buffer[index] != 0xa5u)
		{
			any_byte_changed = true;

			break;
		}
	}

	REQUIRE(any_byte_changed);
}

TEST_CASE("the platform entropy source handles the degenerate requests", "[auth][random]")
{
	PlatformSecureRandomSource random_source;

	unsigned char single_byte = 0;

	// Zero length is a success that writes nothing — callers ask for zero bytes only by
	// accident, and failing would turn an accident into a refused sign-in.
	REQUIRE(random_source.fill_random_bytes(&single_byte, 0));

	// A null destination is a caller error and is reported rather than dereferenced.
	REQUIRE_FALSE(random_source.fill_random_bytes(nullptr, 32));

	REQUIRE(random_source.fill_random_bytes(&single_byte, 1));
}

TEST_CASE("the platform entropy source does not repeat itself", "[auth][random]")
{
	// Two hundred independent 32-byte draws — the size the PKCE verifier and the state nonce
	// each use. A duplicate would mean the source is not what it claims to be; a source stuck
	// on one value, or seeded once from a clock, fails here immediately.
	PlatformSecureRandomSource random_source;

	std::set<std::string> draws;

	for (int attempt = 0; attempt < 200; ++attempt)
	{
		std::array<unsigned char, 32> bytes{};

		REQUIRE(random_source.fill_random_bytes(bytes.data(), bytes.size()));

		draws.insert(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	}

	REQUIRE(draws.size() == 200);
}

TEST_CASE("the platform entropy source fills a request larger than one call allows", "[auth][random]")
{
	// getentropy refuses more than 256 bytes at a time, so a longer request goes round the
	// chunking loop. Nothing in the flow asks for more than 32 bytes, but a loop that only
	// filled its first chunk would leave the tail of a longer buffer zeroed, and that is worth
	// catching here rather than the first time something needs a larger draw.
	PlatformSecureRandomSource random_source;

	std::vector<unsigned char> buffer(1000, 0u);

	REQUIRE(random_source.fill_random_bytes(buffer.data(), buffer.size()));

	// A zero byte is ordinary; a run of hundreds of them is a chunking bug. Checked over the
	// final chunk specifically, which is where the failure would land.
	bool any_byte_set_in_final_chunk = false;

	for (std::size_t index = 768; index < buffer.size(); ++index)
	{
		if (buffer[index] != 0u)
		{
			any_byte_set_in_final_chunk = true;

			break;
		}
	}

	REQUIRE(any_byte_set_in_final_chunk);
}

TEST_CASE("PKCE material generated from the platform source is legal and unique", "[auth][random][pkce]")
{
	// The production path end to end: operating system CSPRNG through to a verifier and its
	// challenge. Everything else about PKCE is pinned against the RFC with a deterministic
	// source; this is the assertion that the real source satisfies the same constraints.
	PlatformSecureRandomSource random_source;

	std::set<std::string> verifiers;

	for (int attempt = 0; attempt < 32; ++attempt)
	{
		const auto pair = generate_pkce_pair(random_source);

		REQUIRE(pair.has_value());
		REQUIRE(is_legal_pkce_code_verifier(pair->code_verifier.reveal()));
		REQUIRE(pair->code_challenge == pkce_code_challenge_from_verifier(pair->code_verifier.reveal()));

		verifiers.insert(pair->code_verifier.reveal());
	}

	REQUIRE(verifiers.size() == 32);
}
