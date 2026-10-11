// base64url without padding, RFC 4648 §5.
//
// The vectors are RFC 4648 §10's own test strings, transposed to the URL alphabet and with
// the padding removed — which is exactly the transformation RFC 7636 §3 asks for, so
// getting them from the specification that defines the encoding rather than from the
// implementation is the point.

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <auth/base64_url.h>

using sesh_ai::auth::base64_url_encode;
using sesh_ai::auth::base64_url_encoded_length;

namespace
{
	std::string encode(std::string_view text)
	{
		return base64_url_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
	}
}

TEST_CASE("base64url encodes the RFC 4648 vectors without padding", "[auth][base64url]")
{
	// Each of these exercises a different leftover: "f" has one byte over, "fo" two, "foo"
	// none, and so on around the cycle.
	REQUIRE(encode("") == "");
	REQUIRE(encode("f") == "Zg");
	REQUIRE(encode("fo") == "Zm8");
	REQUIRE(encode("foo") == "Zm9v");
	REQUIRE(encode("foob") == "Zm9vYg");
	REQUIRE(encode("fooba") == "Zm9vYmE");
	REQUIRE(encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64url uses the URL alphabet rather than the standard one", "[auth][base64url]")
{
	// Bytes chosen so that indexes 62 and 63 both appear. Standard base64 would render
	// these as '+' and '/', which would need percent-encoding in a query string and which
	// RFC 7636 excludes from a code verifier.
	const unsigned char bytes[] = {0xffu, 0xefu, 0xbeu};

	const std::string encoded = base64_url_encode(bytes, sizeof(bytes));

	REQUIRE(encoded == "_---");
	REQUIRE(encoded.find('+') == std::string::npos);
	REQUIRE(encoded.find('/') == std::string::npos);
	REQUIRE(encoded.find('=') == std::string::npos);
}

TEST_CASE("base64url output never carries a character outside the alphabet", "[auth][base64url]")
{
	// Every single byte value, and every pair of a byte with 0x00 and 0xff, so all four
	// six-bit windows of the encoder are driven across their full range.
	const auto assert_alphabet = [](const unsigned char* bytes, std::size_t length) {
		const std::string encoded = base64_url_encode(bytes, length);

		REQUIRE(encoded.size() == base64_url_encoded_length(length));

		for (const char character : encoded)
		{
			const bool is_alphabetic =
				(character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
			const bool is_digit = character >= '0' && character <= '9';
			const bool is_url_safe_extra = character == '-' || character == '_';

			REQUIRE((is_alphabetic || is_digit || is_url_safe_extra));
		}
	};

	for (int value = 0; value <= 255; ++value)
	{
		const unsigned char single[] = {static_cast<unsigned char>(value)};

		assert_alphabet(single, 1);

		const unsigned char with_low[] = {static_cast<unsigned char>(value), 0x00u};
		const unsigned char with_high[] = {static_cast<unsigned char>(value), 0xffu};
		const unsigned char triple[] = {static_cast<unsigned char>(value), 0x00u, 0xffu};

		assert_alphabet(with_low, 2);
		assert_alphabet(with_high, 2);
		assert_alphabet(triple, 3);
	}
}

TEST_CASE("base64url encoded length matches what the encoder produces", "[auth][base64url]")
{
	// The reserve() in the encoder is sized from this, and a 32-byte input encoding to 43
	// characters is what makes the generated code verifier RFC 7636's minimum legal length.
	REQUIRE(base64_url_encoded_length(0) == 0);
	REQUIRE(base64_url_encoded_length(1) == 2);
	REQUIRE(base64_url_encoded_length(2) == 3);
	REQUIRE(base64_url_encoded_length(3) == 4);
	REQUIRE(base64_url_encoded_length(32) == 43);

	for (std::size_t length = 0; length <= 130; ++length)
	{
		const std::string bytes(length, '\x5a');

		REQUIRE(base64_url_encode(reinterpret_cast<const unsigned char*>(bytes.data()), length).size() ==
			base64_url_encoded_length(length));
	}
}
