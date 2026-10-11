// PKCE — Proof Key for Code Exchange, RFC 7636.
//
// Why this is not optional. The extension is a public client: it ships as a binary on a
// producer's machine, from a public repository, so it has no client secret to
// authenticate the token exchange with (requirement 26.5 — nothing depends on a secret
// being hidden in a public repository). Without PKCE, the authorization code is the only
// thing standing between an interception and a token, and the code travels back through
// a redirect. RFC 8252 §8.1 — OAuth 2.0 for Native Apps — requires PKCE for exactly this
// case, and Cognito enforces it for app clients without a secret.
//
// The mechanism, in the three lines it actually is:
//
//   1. Before the authorization request, invent a high-entropy secret — the *code
//      verifier* — and send only `SHA256(verifier)` with it, as the *code challenge*.
//   2. The authorization server ties the challenge to the code it issues.
//   3. At the token exchange, present the verifier. The server hashes it and compares.
//
// An attacker who captures the redirect gets the code but not the verifier, and the
// exchange fails. That argument holds only if the verifier is unpredictable, which is
// why `SecureRandomSource` exists and why there is no fallback in it.
//
// ---------------------------------------------------------------------------
// The choices made here, and the specification text behind each
//
// **S256, never `plain`.** RFC 7636 §4.2: a client capable of S256 MUST use it, and
// `plain` is permitted only for clients that cannot. This one can. `plain` sends the
// verifier itself as the challenge, which means the authorization request carries the
// secret — the browser history, the server's access log, and anything watching the
// navigation all get it. There is no configuration switch here to select `plain`,
// because there is no circumstance in which this client should send it.
//
// **32 random bytes, base64url-encoded to 43 characters.** RFC 7636 §4.1 allows 43 to
// 128 characters from the `unreserved` set and recommends 32 bytes of entropy; §7.1
// requires at least 256 bits. 32 bytes is 256 bits, and its base64url encoding is
// exactly 43 characters — the minimum legal length, which is the specification saying
// that 256 bits is the point rather than the character count. Encoding random bytes
// rather than sampling an alphabet also means there is no modulo bias to get wrong:
// every input byte is consumed whole, and the output character set is the encoding's,
// which is a subset of `unreserved`.
//
// **The `state` nonce gets the same treatment.** It is a CSRF defence, so a predictable
// state is a hole; 32 bytes from the same source, encoded the same way.
//
// ---------------------------------------------------------------------------
// The verifier is a secret and the challenge is not
//
// That asymmetry is in the types: `code_verifier` is a `secret_string`, so it cannot be
// streamed, logged, or implicitly converted, and it is zeroised when the pair goes away.
// `code_challenge` is a plain `std::string` because it is published in the authorization
// URL by design. tests/auth/pkce_test.cpp asserts the authorization URL never contains
// the verifier — the failure mode that would quietly turn S256 back into `plain`.

#ifndef SESH_AI_AUTH_PKCE_H
#define SESH_AI_AUTH_PKCE_H

#include <auth/base64_url.h>
#include <auth/secret_string.h>
#include <auth/secure_random.h>
#include <auth/sha256.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace sesh_ai::auth
{
	// 256 bits, per RFC 7636 §7.1.
	inline constexpr std::size_t pkce_verifier_entropy_bytes = 32;

	// base64url of 32 bytes, unpadded. RFC 7636's minimum legal verifier length, and
	// what the byte count above encodes to.
	inline constexpr std::size_t pkce_verifier_length = 43;

	// The same 256 bits for the CSRF nonce.
	inline constexpr std::size_t state_nonce_entropy_bytes = 32;
	inline constexpr std::size_t state_nonce_length = 43;

	// The only challenge method this client sends. See the header comment.
	inline constexpr char pkce_code_challenge_method[] = "S256";

	struct pkce_pair
	{
		// Sent only in the token exchange, over TLS, in the request body.
		secret_string code_verifier;

		// Sent in the authorization URL. Public by design.
		std::string code_challenge;
	};

	// `base64url(SHA256(ascii(verifier)))` — RFC 7636 §4.2.
	//
	// Separate from the generation below so the suite can pin it against the worked
	// example in RFC 7636 Appendix B, which is the one place the whole derivation is
	// checkable against a number somebody else computed.
	inline std::string pkce_code_challenge_from_verifier(std::string_view code_verifier)
	{
		const sha256_digest digest = sha256(code_verifier);

		return base64_url_encode(digest.data(), digest.size());
	}

	// A fresh verifier and its challenge.
	//
	// Empty when the entropy source failed, which the caller must treat as "this
	// sign-in cannot be started" rather than as something to work around. See
	// `SecureRandomSource::fill_random_bytes`.
	inline std::optional<pkce_pair> generate_pkce_pair(SecureRandomSource& random_source)
	{
		std::array<unsigned char, pkce_verifier_entropy_bytes> entropy{};

		if (!random_source.fill_random_bytes(entropy.data(), entropy.size()))
		{
			return std::nullopt;
		}

		std::string verifier = base64_url_encode(entropy.data(), entropy.size());

		// The entropy is as sensitive as the verifier derived from it, and it is about
		// to go out of scope on the stack where nothing would clear it.
		detail::overwrite_with_zeros(reinterpret_cast<char*>(entropy.data()), entropy.size());

		pkce_pair pair;

		pair.code_challenge = pkce_code_challenge_from_verifier(verifier);

		// adopt() copies the characters in and zeroises `verifier`, so the plaintext does
		// not survive in a local nobody is watching.
		pair.code_verifier = secret_string::adopt(verifier);

		return pair;
	}

	// A fresh `state` nonce. Empty when the entropy source failed.
	inline std::optional<std::string> generate_state_nonce(SecureRandomSource& random_source)
	{
		std::array<unsigned char, state_nonce_entropy_bytes> entropy{};

		if (!random_source.fill_random_bytes(entropy.data(), entropy.size()))
		{
			return std::nullopt;
		}

		std::string nonce = base64_url_encode(entropy.data(), entropy.size());

		detail::overwrite_with_zeros(reinterpret_cast<char*>(entropy.data()), entropy.size());

		return nonce;
	}

	// Whether a string is a legal RFC 7636 §4.1 code verifier: 43 to 128 characters,
	// each from `unreserved` = ALPHA / DIGIT / "-" / "." / "_" / "~".
	//
	// Nothing in the flow calls this — the generator cannot produce anything else. It is
	// here so the suite can state the specification's constraint directly rather than
	// restating the generator's implementation.
	inline bool is_legal_pkce_code_verifier(std::string_view candidate)
	{
		if (candidate.size() < 43 || candidate.size() > 128)
		{
			return false;
		}

		for (const char character : candidate)
		{
			const bool is_alphabetic =
				(character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
			const bool is_digit = character >= '0' && character <= '9';
			const bool is_other_unreserved =
				character == '-' || character == '.' || character == '_' || character == '~';

			if (!is_alphabetic && !is_digit && !is_other_unreserved)
			{
				return false;
			}
		}

		return true;
	}
}

#endif
