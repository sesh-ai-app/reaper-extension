// The operating system side of the entropy seam.
//
// One implementation of `SecureRandomSource` per platform, each going to the system
// CSPRNG rather than to anything in the C++ standard library:
//
//   macOS    getentropy(2)     — the kernel's random device, since 10.12
//   Linux    getentropy(3)     — getrandom(2) under the hood, since glibc 2.25
//   Windows  BCryptGenRandom   — with BCRYPT_USE_SYSTEM_PREFERRED_RNG
//
// This is a separate header from `auth/secure_random.h` on purpose, and the reason is
// `<windows.h>`: on Windows this file pulls it in, which drags in a few thousand macros
// including `min` and `max`, and nothing else in src/auth should have to live with that.
// Keeping the platform code in its own header confines it to the one translation unit
// that wires up production and the one test that exercises the real source. Everything
// else — the token manager, PKCE, the whole flow — depends on the abstract interface.
//
// Header-only rather than a `*.cpp` under src/, for the same reason the rest of the
// component is: a `.cpp` is compiled into the extension's shared library and nothing
// else, so the suite could never run the real entropy source, and "we call the OS
// CSPRNG" would be a claim resting on nobody having checked. It is checked —
// tests/auth/platform_secure_random_test.cpp runs the real source on whichever platform
// the suite is built for.
//
// ---------------------------------------------------------------------------
// No fallback, deliberately
//
// If the platform call fails, `fill_random_bytes` returns false and that is the end of
// it. There is no `/dev/urandom` path, no `std::random_device` path, no clock-seeded
// last resort. A fallback here would be a predictable PKCE verifier reached by an error
// path nobody tests, which is strictly worse than a sign-in that refuses to start and
// says why.
//
// ---------------------------------------------------------------------------
// Linking
//
// `getentropy` is in libc on both Unix platforms, so there is nothing to link. On
// Windows, `#pragma comment(lib, "bcrypt.lib")` names the import library from inside the
// source, which MSVC honours — so the CMake configuration needs no edit, and in
// particular the test target does not, which keeps this component out of a file other
// work is also editing. MinGW does not honour the pragma; the extension's Windows build
// is MSVC (CEF and the REAPER SDK both assume it), so that is not a supported
// configuration to begin with.

#ifndef SESH_AI_AUTH_PLATFORM_SECURE_RANDOM_H
#define SESH_AI_AUTH_PLATFORM_SECURE_RANDOM_H

#include <auth/secure_random.h>

#include <cstddef>

#if defined(_WIN32)

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#include <windows.h>

	#include <bcrypt.h>

	#if defined(_MSC_VER)
		#pragma comment(lib, "bcrypt.lib")
	#endif

#else

	#include <sys/random.h>

#endif

namespace sesh_ai::auth
{
	// The `SecureRandomSource` the operating system backs.
	//
	// Stateless — there is no generator object to seed, reseed, or fork-protect, because
	// the kernel owns all of that. Cheap to construct, safe to construct more than once,
	// and holds nothing that needs zeroising.
	class PlatformSecureRandomSource final : public SecureRandomSource
	{
	public:
		bool fill_random_bytes(unsigned char* destination, std::size_t length) override
		{
			if (length == 0)
			{
				return true;
			}

			if (destination == nullptr)
			{
				return false;
			}

#if defined(_WIN32)

			// BCryptGenRandom takes a ULONG count. Every call site here asks for tens of
			// bytes, but the cast is guarded rather than assumed.
			if (length > static_cast<std::size_t>(0xffffffffu))
			{
				return false;
			}

			const NTSTATUS status = BCryptGenRandom(
				nullptr,
				reinterpret_cast<PUCHAR>(destination),
				static_cast<ULONG>(length),
				BCRYPT_USE_SYSTEM_PREFERRED_RNG);

			return status == 0;

#else

			// getentropy refuses more than 256 bytes in one call, so a longer request is
			// filled in chunks. Each chunk is independently unpredictable, so splitting
			// costs nothing.
			constexpr std::size_t maximum_bytes_per_call = 256;

			std::size_t filled = 0;

			while (filled < length)
			{
				const std::size_t remaining = length - filled;
				const std::size_t chunk_length =
					remaining < maximum_bytes_per_call ? remaining : maximum_bytes_per_call;

				if (getentropy(destination + filled, chunk_length) != 0)
				{
					return false;
				}

				filled += chunk_length;
			}

			return true;

#endif
		}
	};
}

#endif
