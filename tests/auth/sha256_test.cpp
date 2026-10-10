// SHA-256 against published vectors.
//
// The hash is implemented in this repository (see src/auth/sha256.h for why), so it is
// only as trustworthy as the vectors it is pinned against. These are the FIPS 180-4
// examples plus the block-boundary lengths, which is where an incorrect implementation
// actually goes wrong: padding that does not leave room for the length field, a length
// field written in the wrong byte order, or a final block that is compressed twice.
//
// Nothing here is a property test in the design's sense — the design's numbered properties
// do not cover authentication — so these are examples, and the examples are the ones
// somebody else computed.

#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <auth/sha256.h>

using sesh_ai::auth::sha256;
using sesh_ai::auth::sha256_digest;
using sesh_ai::auth::sha256_hasher;

namespace
{
	std::string to_hexadecimal(const sha256_digest& digest)
	{
		static constexpr char digits[] = "0123456789abcdef";

		std::string hexadecimal;

		hexadecimal.reserve(digest.size() * 2);

		for (const unsigned char byte : digest)
		{
			hexadecimal.push_back(digits[(byte >> 4) & 0x0fu]);
			hexadecimal.push_back(digits[byte & 0x0fu]);
		}

		return hexadecimal;
	}

	std::string repeated(char character, std::size_t count)
	{
		return std::string(count, character);
	}
}

TEST_CASE("SHA-256 matches the FIPS 180-4 examples", "[auth][sha256]")
{
	// FIPS 180-4, Appendix B.1 — one block, and the empty message, which is the padding
	// path with no message bytes at all.
	REQUIRE(to_hexadecimal(sha256(std::string_view{""})) ==
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	REQUIRE(to_hexadecimal(sha256(std::string_view{"abc"})) ==
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	// 56 bytes. The padded message needs 57 bytes before the length field, which does not
	// fit in the 56 available — so this is the case that must compress an extra block.
	REQUIRE(to_hexadecimal(sha256(std::string_view{
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})) ==
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	// 112 bytes — two full blocks of message, so the chaining between compressions is
	// exercised rather than assumed.
	REQUIRE(to_hexadecimal(sha256(std::string_view{
		"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
		"ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"})) ==
		"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("SHA-256 matches reference digests across the block boundary", "[auth][sha256]")
{
	// Every length where the padding decision changes, with digests taken from an
	// independent implementation. 55 fits the length field, 56 through 63 do not, 64 is a
	// whole block with the padding entirely in the next one, 65 starts over.
	struct boundary_case
	{
		std::size_t length;
		const char* digest;
	};

	static constexpr boundary_case cases[] = {
		{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
		{56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
		{57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
		{63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
		{64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
		{65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
		{119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
		{120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"}
	};

	for (const boundary_case& boundary : cases)
	{
		const std::string message = repeated('a', boundary.length);

		REQUIRE(to_hexadecimal(sha256(message)) == std::string{boundary.digest});
	}
}

TEST_CASE("SHA-256 is insensitive to how the message is fed in", "[auth][sha256]")
{
	// A code verifier is 43 bytes and always arrives whole, so this is not covering a case
	// the flow has. It covers the incremental interface the single-shot functions are built
	// on: if update() mishandled a block boundary mid-message, hashing in pieces would
	// disagree with hashing at once.
	const std::string message = repeated('a', 200);

	const sha256_digest at_once = sha256(message);

	for (const std::size_t chunk_length : {std::size_t{1}, std::size_t{7}, std::size_t{63},
		std::size_t{64}, std::size_t{65}, std::size_t{199}})
	{
		sha256_hasher hasher;

		std::size_t offset = 0;

		while (offset < message.size())
		{
			const std::size_t remaining = message.size() - offset;
			const std::size_t length = remaining < chunk_length ? remaining : chunk_length;

			hasher.update(std::string_view{message}.substr(offset, length));

			offset += length;
		}

		REQUIRE(hasher.finish() == at_once);
	}
}

TEST_CASE("SHA-256 matches the FIPS 180-4 one-million-character example", "[auth][sha256]")
{
	// A megabyte of 'a'. The one vector that exercises the 64-bit length field beyond a
	// single byte, which is where a big-endian mistake in the padding first shows up.
	sha256_hasher hasher;

	const std::string block = repeated('a', 1000);

	for (int repetition = 0; repetition < 1000; ++repetition)
	{
		hasher.update(block);
	}

	REQUIRE(to_hexadecimal(hasher.finish()) ==
		"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
