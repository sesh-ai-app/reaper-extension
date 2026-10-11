// PKCE generation and derivation.
//
// The anchor is RFC 7636 Appendix B, which carries a worked example: a specific verifier and
// the challenge it must produce. That one assertion pins base64url, SHA-256, and the
// derivation order together against a number the specification's authors computed, which is
// worth more than any number of internally consistent checks.
//
// Everything else here is about the constraints RFC 7636 §4.1 and §7.1 put on the verifier —
// length, character set, entropy — and about the two failure modes that would quietly
// weaken the flow: reusing a verifier across sign-ins, and letting the verifier reach the
// authorization URL.

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <auth/pkce.h>

#include "authentication_test_doubles.h"

using sesh_ai::auth::generate_pkce_pair;
using sesh_ai::auth::generate_state_nonce;
using sesh_ai::auth::is_legal_pkce_code_verifier;
using sesh_ai::auth::pkce_code_challenge_from_verifier;
using sesh_ai::auth::pkce_code_challenge_method;
using sesh_ai::auth::pkce_verifier_entropy_bytes;
using sesh_ai::auth::pkce_verifier_length;
using sesh_ai::tests::DeterministicRandomSource;
using sesh_ai::tests::FixedRandomSource;

TEST_CASE("the code challenge matches the RFC 7636 worked example", "[auth][pkce]")
{
	// RFC 7636 Appendix B, verbatim.
	REQUIRE(pkce_code_challenge_from_verifier("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
		"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_CASE("the challenge method is S256 and there is no way to select plain", "[auth][pkce]")
{
	// RFC 7636 §4.2: a client capable of S256 must use it. This one is, so `plain` is not a
	// configurable option anywhere — the constant is the only method sent, and the challenge
	// is always a hash rather than the verifier itself.
	REQUIRE(std::string{pkce_code_challenge_method} == "S256");

	const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";

	REQUIRE(pkce_code_challenge_from_verifier(verifier) != verifier);
}

TEST_CASE("a generated verifier satisfies RFC 7636's constraints", "[auth][pkce]")
{
	DeterministicRandomSource random_source;

	const auto pair = generate_pkce_pair(random_source);

	REQUIRE(pair.has_value());

	// 256 bits of entropy (§7.1), encoding to 43 characters — §4.1's minimum legal length.
	REQUIRE(pkce_verifier_entropy_bytes == 32);
	REQUIRE(pair->code_verifier.size() == pkce_verifier_length);
	REQUIRE(pair->code_verifier.size() == 43);
	REQUIRE(is_legal_pkce_code_verifier(pair->code_verifier.reveal()));

	// And the challenge is the hash of that verifier, not of anything else.
	REQUIRE(pair->code_challenge == pkce_code_challenge_from_verifier(pair->code_verifier.reveal()));
	REQUIRE(pair->code_challenge.size() == 43);
}

TEST_CASE("the verifier is derived from the bytes the entropy source gave", "[auth][pkce]")
{
	// The whole security argument rests on the verifier coming from the random source rather
	// than from anything predictable, so: a known byte sequence must produce the encoding of
	// exactly those bytes. A generator that ignored its source, or mixed in a clock, or hashed
	// the bytes before encoding them, would fail here.
	std::vector<unsigned char> bytes(32);

	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		bytes[index] = static_cast<unsigned char>(index);
	}

	FixedRandomSource random_source{bytes};

	const auto pair = generate_pkce_pair(random_source);

	REQUIRE(pair.has_value());
	REQUIRE(pair->code_verifier.reveal() ==
		sesh_ai::auth::base64_url_encode(bytes.data(), bytes.size()));
	REQUIRE(pair->code_verifier.reveal() == "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");

	// And the challenge is the hash of that verifier.
	REQUIRE(pair->code_challenge ==
		pkce_code_challenge_from_verifier("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"));
}

TEST_CASE("an entropy failure refuses to produce PKCE material", "[auth][pkce]")
{
	// There is no fallback by design. A verifier from a weaker source is worse than no
	// sign-in, because it looks like it worked.
	DeterministicRandomSource random_source;

	random_source.fail_next_fill = true;

	REQUIRE_FALSE(generate_pkce_pair(random_source).has_value());
	REQUIRE_FALSE(generate_state_nonce(random_source).has_value());
}

TEST_CASE("successive sign-ins get different PKCE material", "[auth][pkce]")
{
	// A reused verifier means a captured code from an earlier sign-in is exchangeable. The
	// deterministic source advances, so this checks the generator consumes fresh bytes per
	// call rather than caching.
	DeterministicRandomSource random_source;

	std::set<std::string> verifiers;
	std::set<std::string> challenges;
	std::set<std::string> nonces;

	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const auto pair = generate_pkce_pair(random_source);
		const auto nonce = generate_state_nonce(random_source);

		REQUIRE(pair.has_value());
		REQUIRE(nonce.has_value());

		verifiers.insert(pair->code_verifier.reveal());
		challenges.insert(pair->code_challenge);
		nonces.insert(*nonce);
	}

	REQUIRE(verifiers.size() == 64);
	REQUIRE(challenges.size() == 64);
	REQUIRE(nonces.size() == 64);
}

// The same constraints against the operating system's own CSPRNG rather than a test double
// live in tests/auth/platform_secure_random_test.cpp, which is the only file that includes
// the platform header — on Windows it pulls in <windows.h>, and confining that to one
// translation unit is why the platform source is a separate header.

TEST_CASE("the state nonce carries the same entropy as the verifier", "[auth][pkce]")
{
	// A guessable state re-opens the CSRF hole state exists to close, so it gets 256 bits
	// too rather than being treated as a formality.
	DeterministicRandomSource random_source;

	const auto nonce = generate_state_nonce(random_source);

	REQUIRE(nonce.has_value());
	REQUIRE(nonce->size() == sesh_ai::auth::state_nonce_length);
	REQUIRE(nonce->size() == 43);

	// base64url output is already URL-safe, which is why the nonce needs no encoding
	// gymnastics when it goes into and comes back out of a query string.
	for (const char character : *nonce)
	{
		const bool is_alphabetic =
			(character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
		const bool is_digit = character >= '0' && character <= '9';
		const bool is_url_safe_extra = character == '-' || character == '_';

		REQUIRE((is_alphabetic || is_digit || is_url_safe_extra));
	}
}

TEST_CASE("the legal-verifier check follows RFC 7636 section 4.1", "[auth][pkce]")
{
	REQUIRE(is_legal_pkce_code_verifier(std::string(43, 'a')));
	REQUIRE(is_legal_pkce_code_verifier(std::string(128, 'a')));
	REQUIRE(is_legal_pkce_code_verifier("ABCdef123-._~" + std::string(30, 'x')));

	// Too short, too long, and carrying characters outside `unreserved`.
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(42, 'a')));
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(129, 'a')));
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(42, 'a') + "+"));
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(42, 'a') + "/"));
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(42, 'a') + "="));
	REQUIRE_FALSE(is_legal_pkce_code_verifier(std::string(42, 'a') + " "));
}
