// The container every piece of token material lives in (requirements 17.4, 26.1,
// 26.4, 26.5).
//
// Requirement 17.4 says tokens are held in memory only — no persistent storage, no
// platform keychain, no secret material on disk — and requirement 26.4 says no
// credential is ever logged. Both are the kind of rule that a comment cannot enforce,
// so this type is shaped to make the wrong thing hard to write rather than merely
// discouraged:
//
//   - There is no `operator<<`, no `operator std::string`, and no implicit conversion
//     of any kind. `std::cout << token` does not compile, and neither does passing a
//     secret where a string is expected. The only way to reach the characters is
//     `reveal()`, which is named so that it shows up in a review and in a grep.
//   - There is no `save`, `write`, `serialise`, or `to_json`. Persistence is not an
//     expressible operation on a secret, here or anywhere else in src/auth.
//   - Catch2 has no stringifier for it, so a failing assertion prints `{?}` rather
//     than the token — which matters because CI logs are public for this repository.
//   - `describe_for_log()` is the thing to reach for when a log line needs to say
//     something about a token. It reports the length and nothing else.
//
// Zeroisation on destruction is the other half. std::string does not clear its buffer
// when it goes away, so a token that merely fell out of scope is still sitting in the
// process image where a crash dump or a core file would pick it up.
//
// ---------------------------------------------------------------------------
// Why the move constructor copies
//
// Moving a std::string that is short enough for the small-string optimisation copies
// the characters into the destination and leaves the source's inline buffer holding
// them — and after the move the source reports size() == 0, so there is no longer any
// way to address the bytes that need clearing. A moved-from secret would therefore be
// a secret nobody can zeroise.
//
// So a move here copies the characters and then clears the source through its
// still-valid size. That is one memcpy of a few hundred bytes on an operation that
// happens a handful of times per sign-in, against the guarantee that no code path
// leaves token bytes somewhere unreachable. The trade is not close.

#ifndef SESH_AI_AUTH_SECRET_STRING_H
#define SESH_AI_AUTH_SECRET_STRING_H

#include <cstddef>
#include <string>
#include <string_view>

namespace sesh_ai::auth
{
	namespace detail
	{
		// Overwrites `length` bytes at `bytes` with zeros in a way the optimiser is
		// not allowed to remove.
		//
		// A plain memset or std::fill over a buffer that is about to be freed is a
		// dead store, and compilers delete dead stores — this is the classic reason
		// hand-written "secure erase" code does nothing at -O2. Writing through a
		// volatile pointer makes each store an observable side effect, so it survives.
		inline void overwrite_with_zeros(char* bytes, std::size_t length)
		{
			if (bytes == nullptr || length == 0)
			{
				return;
			}

			volatile char* const destination = bytes;

			for (std::size_t index = 0; index < length; ++index)
			{
				destination[index] = '\0';
			}
		}

		// Compares two byte sequences without an early exit.
		//
		// Used for the OAuth `state` nonce and for anything else where a byte-by-byte
		// comparison would leak how much of a value an attacker guessed correctly
		// through how long the comparison took. Lengths are compared openly — the
		// length of a state nonce is a fixed, public property of this client — and the
		// byte loop runs over the full length in every case.
		inline bool constant_time_equals(std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
			{
				return false;
			}

			unsigned char difference = 0;

			for (std::size_t index = 0; index < left.size(); ++index)
			{
				difference = static_cast<unsigned char>(
					difference | (static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index])));
			}

			return difference == 0;
		}
	}

	// Token material. See the header comment for what this type deliberately cannot do.
	class secret_string
	{
	public:
		secret_string() = default;

		explicit secret_string(std::string_view value)
			: value_{value}
		{
		}

		// For a caller that already holds the secret in a mutable std::string — a JSON
		// parse result in the token endpoint implementation, say. Copies the characters
		// in and then zeroises the caller's buffer, so the plaintext does not outlive
		// the call in a string nothing is watching.
		static secret_string adopt(std::string& source)
		{
			secret_string adopted{std::string_view{source}};

			detail::overwrite_with_zeros(source.data(), source.size());
			source.clear();

			return adopted;
		}

		~secret_string()
		{
			clear();
		}

		secret_string(const secret_string& other)
			: value_{other.value_}
		{
		}

		secret_string& operator=(const secret_string& other)
		{
			if (this != &other)
			{
				const std::string replacement = other.value_;

				clear();
				value_ = replacement;
			}

			return *this;
		}

		// Copies rather than steals, then clears the source. See the header comment.
		secret_string(secret_string&& other) noexcept
			: value_{other.value_}
		{
			other.clear();
		}

		secret_string& operator=(secret_string&& other) noexcept
		{
			if (this != &other)
			{
				const std::string replacement = other.value_;

				other.clear();
				clear();
				value_ = replacement;
			}

			return *this;
		}

		// Zeroises the characters and empties the value. The buffer is overwritten
		// before it is released, which is the whole point — `std::string::clear()` on
		// its own only sets the length to zero.
		void clear()
		{
			detail::overwrite_with_zeros(value_.data(), value_.size());
			value_.clear();
		}

		bool empty() const { return value_.empty(); }

		std::size_t size() const { return value_.size(); }

		// The one route to the characters. Deliberately verbose at the call site.
		const std::string& reveal() const { return value_; }

		// What a log line may say about a secret: how long it is, and nothing else.
		std::string describe_for_log() const
		{
			if (value_.empty())
			{
				return "<absent>";
			}

			return "<redacted, " + std::to_string(value_.size()) + " characters>";
		}

	private:
		std::string value_;
	};

	// Constant-time comparison of two secrets, and of a secret against a value that
	// arrived from outside.
	inline bool constant_time_equals(const secret_string& left, const secret_string& right)
	{
		return detail::constant_time_equals(left.reveal(), right.reveal());
	}

	inline bool constant_time_equals(const secret_string& left, std::string_view right)
	{
		return detail::constant_time_equals(left.reveal(), right);
	}
}

#endif
