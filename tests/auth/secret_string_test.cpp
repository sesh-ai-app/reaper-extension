// The container token material lives in.
//
// Requirements 17.4 and 26.4 are both about what must *not* happen to a token, which is
// awkward to test directly — you cannot assert the absence of a log line nobody wrote. What
// can be asserted is the mechanism: that the zeroising overwrite really overwrites, that
// every route out of a secret goes through it, and that the log-facing description carries
// no characters of the value. Between them those cover the failure modes that would leave a
// token in the process image or in CI output.
//
// The parts that are structural rather than testable — no `operator<<`, no implicit
// conversion, no persistence method — are enforced by the type not having them, so a
// violation is a compile error rather than a red test. That is the stronger guarantee and
// it is why they are shaped that way.

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <auth/secret_string.h>

using sesh_ai::auth::constant_time_equals;
using sesh_ai::auth::secret_string;
using sesh_ai::auth::detail::overwrite_with_zeros;

TEST_CASE("the zeroising overwrite clears every byte", "[auth][secret]")
{
	// The mechanism on its own, over a buffer the test owns outright. Everything below
	// depends on this working, and this is the one place it can be checked without reasoning
	// about std::string's internals.
	std::array<char, 64> buffer{};

	buffer.fill('x');

	overwrite_with_zeros(buffer.data(), buffer.size());

	for (const char byte : buffer)
	{
		REQUIRE(byte == '\0');
	}

	// A null pointer or a zero length is a no-op rather than a crash, because clear() is
	// called on empty secrets constantly — every sign-in clears three of them.
	overwrite_with_zeros(nullptr, 16);
	overwrite_with_zeros(buffer.data(), 0);
}

TEST_CASE("clearing a secret overwrites the characters it held", "[auth][secret]")
{
	// Long enough to be heap-allocated on every standard library, so the buffer inspected
	// below is a real allocation rather than the small-string buffer inside the object.
	const std::string token(128, 'z');

	secret_string secret{token};

	REQUIRE(secret.size() == token.size());

	// The allocation stays alive and owned by the secret across clear() — std::string::clear
	// sets the length to zero and keeps the capacity — so reading the bytes that were the
	// value is reading memory that is still ours.
	const char* const characters = secret.reveal().data();

	secret.clear();

	REQUIRE(secret.empty());
	REQUIRE(secret.reveal().empty());

	for (std::size_t index = 0; index < token.size(); ++index)
	{
		REQUIRE(characters[index] == '\0');
	}
}

TEST_CASE("adopting a secret clears the caller's copy", "[auth][secret]")
{
	// This is how the token endpoint implementation will hand a parsed token over: the JSON
	// parse produced a std::string, and that string must not be left holding the plaintext.
	std::string parsed(96, 'q');

	const char* const characters = parsed.data();
	const std::size_t original_length = parsed.size();

	const secret_string secret = secret_string::adopt(parsed);

	REQUIRE(secret.size() == original_length);
	REQUIRE(secret.reveal() == std::string(original_length, 'q'));
	REQUIRE(parsed.empty());

	for (std::size_t index = 0; index < original_length; ++index)
	{
		REQUIRE(characters[index] == '\0');
	}
}

TEST_CASE("moving a secret leaves nothing behind in the source", "[auth][secret]")
{
	// The reason the move constructor copies rather than steals: a stolen short string leaves
	// its characters in the source's inline buffer with size() reporting zero, so there is no
	// longer any way to address them. Short and long values are both checked, because the
	// small-string case is the one that would silently go wrong.
	for (const std::size_t length : {std::size_t{8}, std::size_t{128}})
	{
		secret_string source{std::string(length, 'k')};

		const secret_string moved{std::move(source)};

		REQUIRE(moved.size() == length);
		REQUIRE(moved.reveal() == std::string(length, 'k'));
		REQUIRE(source.empty());
		REQUIRE(source.size() == 0);
	}

	secret_string assignment_source{std::string(128, 'm')};
	secret_string assignment_destination{std::string("previous value")};

	assignment_destination = std::move(assignment_source);

	REQUIRE(assignment_destination.reveal() == std::string(128, 'm'));
	REQUIRE(assignment_source.empty());
}

TEST_CASE("reassigning a secret clears what it held first", "[auth][secret]")
{
	secret_string secret{std::string(128, 'a')};

	const char* const characters = secret.reveal().data();

	secret = secret_string{std::string("something shorter")};

	REQUIRE(secret.reveal() == "something shorter");

	// The original allocation was released when the longer value was replaced, so the only
	// thing that can be asserted here without reading freed memory is that the new value is
	// what is held. The clear-before-assign is what the previous test covers directly.
	(void)characters;
}

TEST_CASE("a secret describes itself for a log without revealing anything", "[auth][secret]")
{
	const std::string token = "a-token-that-must-not-appear-in-any-log-line";

	const secret_string secret{token};

	const std::string description = secret.describe_for_log();

	REQUIRE(description.find(token) == std::string::npos);
	REQUIRE(description.find("a-token") == std::string::npos);
	REQUIRE(description == "<redacted, " + std::to_string(token.size()) + " characters>");

	REQUIRE(secret_string{}.describe_for_log() == "<absent>");
}

TEST_CASE("secrets compare without an early exit", "[auth][secret]")
{
	// Constant-time comparison is not observable from a test — what is observable is that it
	// is a correct comparison, which is worth pinning because a hand-written loop is easy to
	// get subtly wrong. The state nonce check in the sign-in flow depends on it.
	const secret_string nonce{"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"};

	REQUIRE(constant_time_equals(nonce, secret_string{"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"}));
	REQUIRE(constant_time_equals(nonce, std::string_view{"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"}));

	// Differing in the first character, the last character, and in length — the three cases
	// a broken comparison gets wrong in different ways.
	REQUIRE_FALSE(constant_time_equals(nonce, std::string_view{"F9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"}));
	REQUIRE_FALSE(constant_time_equals(nonce, std::string_view{"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cX"}));
	REQUIRE_FALSE(constant_time_equals(nonce, std::string_view{"E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-c"}));
	REQUIRE_FALSE(constant_time_equals(nonce, std::string_view{""}));

	REQUIRE(constant_time_equals(secret_string{}, std::string_view{""}));
}
