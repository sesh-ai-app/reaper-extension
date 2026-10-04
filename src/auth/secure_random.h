// The entropy seam.
//
// The PKCE code verifier and the OAuth `state` nonce are the two values in the
// authorization code flow whose whole security argument is that nobody can predict
// them. RFC 7636 §7.1 is explicit that the verifier must come from a cryptographically
// secure random number generator; a guessable verifier hands an intercepted
// authorization code back to whoever intercepted it, and a guessable state nonce
// re-opens the CSRF hole state exists to close.
//
// So `rand()`, `std::mt19937`, and anything seeded from the clock are not usable here,
// and neither is `std::random_device` on its own — the standard permits it to be a
// perfectly deterministic pseudo-random engine, and on at least one shipping toolchain
// it is exactly that. The production implementation goes to the operating system's own
// CSPRNG instead (auth/platform_secure_random.h).
//
// This interface exists so the suite can drive the flow with a known byte sequence and
// assert the exact verifier and challenge it produces, following the seam convention
// `entry/timer_registration.h` established: the narrowest interface the component needs,
// one production implementation, a substitute in the tests. It is *not* a seam for
// swapping in a weaker source — the whole contract is "cryptographically secure", and
// a failure is reported rather than papered over with a fallback.

#ifndef SESH_AI_AUTH_SECURE_RANDOM_H
#define SESH_AI_AUTH_SECURE_RANDOM_H

#include <cstddef>

namespace sesh_ai::auth
{
	class SecureRandomSource
	{
	public:
		virtual ~SecureRandomSource() = default;

		// Fills `length` bytes at `destination` with cryptographically secure random
		// bytes.
		//
		// False when the platform entropy source failed, and the caller's only correct
		// response is to abandon what it was doing and report it. There is deliberately
		// no partial success: an implementation that returns false must be assumed to
		// have written nothing usable, and a caller must never proceed with whatever it
		// got. A sign-in that cannot be started is a sign-in the producer retries; a
		// sign-in started with predictable PKCE material is a vulnerability that looks
		// like it worked.
		virtual bool fill_random_bytes(unsigned char* destination, std::size_t length) = 0;
	};
}

#endif
