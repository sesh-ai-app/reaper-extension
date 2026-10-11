// SHA-256, as specified in FIPS 180-4.
//
// PKCE's `S256` code challenge method is `base64url(SHA256(ascii(code_verifier)))`
// (RFC 7636 §4.2), so the authorization code flow cannot be run without a SHA-256.
// Nothing already in this build provides one: nlohmann/json parses JSON, the schema
// validator validates, and the Socket.IO client's TLS stack — which would have brought
// OpenSSL with it — is declared but not populated, and is in any case not a dependency
// the auth component should acquire a hash from.
//
// So it is implemented here. Writing a primitive by hand deserves a justification, and
// this is it: a hash is the one primitive where doing so carries no key material, no
// nonce reuse hazard, no padding oracle, and no timing channel worth defending —
// SHA-256 over a public value is a fixed sequence of arithmetic with no secret input.
// (The code verifier is a secret, but the challenge derived from it is published in the
// authorization URL, and the computation branches on nothing.) And correctness is
// decidable rather than a matter of judgement: the FIPS 180-4 vectors pin it exactly,
// and tests/auth/sha256_test.cpp runs them, including the multi-block and
// length-extension-boundary cases where an incorrect implementation actually goes
// wrong.
//
// What would *not* be acceptable to hand-roll, and is not hand-rolled: the entropy
// source. See auth/secure_random.h — that goes to the operating system.
//
// Pure arithmetic over fixed-width integers. No allocation, no REAPER, no JSON,
// header-only so the suite reaches it directly.

#ifndef SESH_AI_AUTH_SHA256_H
#define SESH_AI_AUTH_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sesh_ai::auth
{
	inline constexpr std::size_t sha256_digest_length = 32;
	inline constexpr std::size_t sha256_block_length = 64;

	using sha256_digest = std::array<unsigned char, sha256_digest_length>;

	namespace detail
	{
		// The first thirty-two bits of the fractional parts of the cube roots of the
		// first sixty-four primes (FIPS 180-4 §4.2.2).
		inline constexpr std::uint32_t sha256_round_constants[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
			0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
			0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
			0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
			0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
			0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
			0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
			0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
			0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned int distance)
		{
			return (value >> distance) | (value << (32u - distance));
		}
	}

	// Incremental interface, because the single-shot convenience functions below are
	// written in terms of it and because the suite needs to feed a message in pieces to
	// show that the block boundary is handled.
	class sha256_hasher
	{
	public:
		void update(const unsigned char* data, std::size_t length)
		{
			for (std::size_t index = 0; index < length; ++index)
			{
				pending_block_[pending_length_] = data[index];
				++pending_length_;

				if (pending_length_ == sha256_block_length)
				{
					compress_pending_block();
					pending_length_ = 0;
				}
			}

			message_bit_length_ += static_cast<std::uint64_t>(length) * 8u;
		}

		void update(std::string_view data)
		{
			update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
		}

		// Appends the padding FIPS 180-4 §5.1.1 specifies — a single 1 bit, then zeros,
		// then the message length as a big-endian 64-bit count of bits — and returns the
		// digest. Not idempotent; a hasher is used once.
		sha256_digest finish()
		{
			pending_block_[pending_length_] = 0x80u;
			++pending_length_;

			// The length field occupies the last eight bytes of the final block. When
			// the padded message does not leave room for it, the current block is
			// filled with zeros and compressed, and the length goes in the next one.
			// This is the boundary a wrong implementation gets wrong, which is why the
			// suite hashes messages of 55, 56, 57, 63, 64, and 65 bytes.
			if (pending_length_ > sha256_block_length - 8)
			{
				while (pending_length_ < sha256_block_length)
				{
					pending_block_[pending_length_] = 0u;
					++pending_length_;
				}

				compress_pending_block();
				pending_length_ = 0;
			}

			while (pending_length_ < sha256_block_length - 8)
			{
				pending_block_[pending_length_] = 0u;
				++pending_length_;
			}

			for (int shift = 56; shift >= 0; shift -= 8)
			{
				pending_block_[pending_length_] =
					static_cast<unsigned char>((message_bit_length_ >> shift) & 0xffu);
				++pending_length_;
			}

			compress_pending_block();
			pending_length_ = 0;

			sha256_digest digest{};

			for (std::size_t word_index = 0; word_index < 8; ++word_index)
			{
				for (std::size_t byte_index = 0; byte_index < 4; ++byte_index)
				{
					digest[(word_index * 4) + byte_index] = static_cast<unsigned char>(
						(state_[word_index] >> (24u - (8u * byte_index))) & 0xffu);
				}
			}

			return digest;
		}

	private:
		void compress_pending_block()
		{
			std::uint32_t schedule[64] = {};

			for (std::size_t index = 0; index < 16; ++index)
			{
				schedule[index] =
					(static_cast<std::uint32_t>(pending_block_[(index * 4) + 0]) << 24) |
					(static_cast<std::uint32_t>(pending_block_[(index * 4) + 1]) << 16) |
					(static_cast<std::uint32_t>(pending_block_[(index * 4) + 2]) << 8) |
					static_cast<std::uint32_t>(pending_block_[(index * 4) + 3]);
			}

			for (std::size_t index = 16; index < 64; ++index)
			{
				const std::uint32_t previous_fifteen = schedule[index - 15];
				const std::uint32_t previous_two = schedule[index - 2];

				const std::uint32_t sigma_zero =
					detail::rotate_right(previous_fifteen, 7) ^
					detail::rotate_right(previous_fifteen, 18) ^
					(previous_fifteen >> 3);

				const std::uint32_t sigma_one =
					detail::rotate_right(previous_two, 17) ^
					detail::rotate_right(previous_two, 19) ^
					(previous_two >> 10);

				schedule[index] = schedule[index - 16] + sigma_zero + schedule[index - 7] + sigma_one;
			}

			std::uint32_t a = state_[0];
			std::uint32_t b = state_[1];
			std::uint32_t c = state_[2];
			std::uint32_t d = state_[3];
			std::uint32_t e = state_[4];
			std::uint32_t f = state_[5];
			std::uint32_t g = state_[6];
			std::uint32_t h = state_[7];

			for (std::size_t index = 0; index < 64; ++index)
			{
				const std::uint32_t big_sigma_one =
					detail::rotate_right(e, 6) ^ detail::rotate_right(e, 11) ^ detail::rotate_right(e, 25);

				const std::uint32_t choose = (e & f) ^ ((~e) & g);

				const std::uint32_t first_temporary =
					h + big_sigma_one + choose + detail::sha256_round_constants[index] + schedule[index];

				const std::uint32_t big_sigma_zero =
					detail::rotate_right(a, 2) ^ detail::rotate_right(a, 13) ^ detail::rotate_right(a, 22);

				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);

				const std::uint32_t second_temporary = big_sigma_zero + majority;

				h = g;
				g = f;
				f = e;
				e = d + first_temporary;
				d = c;
				c = b;
				b = a;
				a = first_temporary + second_temporary;
			}

			state_[0] += a;
			state_[1] += b;
			state_[2] += c;
			state_[3] += d;
			state_[4] += e;
			state_[5] += f;
			state_[6] += g;
			state_[7] += h;
		}

		// The first thirty-two bits of the fractional parts of the square roots of the
		// first eight primes (FIPS 180-4 §5.3.3).
		std::uint32_t state_[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};

		unsigned char pending_block_[sha256_block_length] = {};
		std::size_t pending_length_ = 0;
		std::uint64_t message_bit_length_ = 0;
	};

	inline sha256_digest sha256(const unsigned char* message, std::size_t length)
	{
		sha256_hasher hasher;

		hasher.update(message, length);

		return hasher.finish();
	}

	inline sha256_digest sha256(std::string_view message)
	{
		sha256_hasher hasher;

		hasher.update(message);

		return hasher.finish();
	}
}

#endif
