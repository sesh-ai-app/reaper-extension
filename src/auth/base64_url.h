// base64url without padding — RFC 4648 §5, as RFC 7636 §3 requires it for PKCE.
//
// Two differences from ordinary base64, and both matter here. The 62nd and 63rd
// characters are `-` and `_` rather than `+` and `/`, so the output survives a URL
// query string without percent-encoding; and the `=` padding is omitted, which RFC 7636
// states explicitly ("the base64url-encoded value ... with all trailing '=' characters
// omitted"). A code challenge carrying padding is rejected by the authorization server,
// and a code verifier carrying `+` or `/` is outside the `unreserved` character set the
// specification confines it to.
//
// The alphabet is also exactly the set of characters RFC 7636 allows in a code
// verifier (ALPHA / DIGIT / "-" / "." / "_" / "~" — a superset), which is why the
// verifier is produced by encoding random bytes rather than by sampling a custom
// alphabet: the encoding is the thing that guarantees the character set, and there is
// no modulo bias to reason about because every byte of input is used whole.
//
// Encode only. Nothing in the authorization code flow decodes a base64url value: the
// challenge is sent, the verifier is sent, and neither is ever read back.

#ifndef SESH_AI_AUTH_BASE64_URL_H
#define SESH_AI_AUTH_BASE64_URL_H

#include <cstddef>
#include <string>

namespace sesh_ai::auth
{
	namespace detail
	{
		inline constexpr char base64_url_alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz"
			"0123456789"
			"-_";
	}

	// Encoded length for `byte_count` input bytes: four characters per three bytes,
	// with the final partial group contributing two characters for one leftover byte
	// and three for two, rather than being padded out to four.
	inline constexpr std::size_t base64_url_encoded_length(std::size_t byte_count)
	{
		const std::size_t whole_groups = byte_count / 3;
		const std::size_t leftover_bytes = byte_count % 3;

		return (whole_groups * 4) + (leftover_bytes == 0 ? 0 : leftover_bytes + 1);
	}

	inline std::string base64_url_encode(const unsigned char* bytes, std::size_t length)
	{
		std::string encoded;

		encoded.reserve(base64_url_encoded_length(length));

		std::size_t index = 0;

		while (index + 3 <= length)
		{
			const unsigned int group =
				(static_cast<unsigned int>(bytes[index]) << 16) |
				(static_cast<unsigned int>(bytes[index + 1]) << 8) |
				static_cast<unsigned int>(bytes[index + 2]);

			encoded.push_back(detail::base64_url_alphabet[(group >> 18) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[(group >> 12) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[(group >> 6) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[group & 0x3fu]);

			index += 3;
		}

		const std::size_t leftover_bytes = length - index;

		if (leftover_bytes == 1)
		{
			const unsigned int group = static_cast<unsigned int>(bytes[index]) << 16;

			encoded.push_back(detail::base64_url_alphabet[(group >> 18) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[(group >> 12) & 0x3fu]);
		}
		else if (leftover_bytes == 2)
		{
			const unsigned int group =
				(static_cast<unsigned int>(bytes[index]) << 16) |
				(static_cast<unsigned int>(bytes[index + 1]) << 8);

			encoded.push_back(detail::base64_url_alphabet[(group >> 18) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[(group >> 12) & 0x3fu]);
			encoded.push_back(detail::base64_url_alphabet[(group >> 6) & 0x3fu]);
		}

		return encoded;
	}
}

#endif
