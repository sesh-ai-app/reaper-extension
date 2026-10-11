// Where the Transport Client and the Token Manager meet (requirements 3.1, 3.6, 17.6).
//
// `AccessTokenSource` is declared in transport_client.h without any reference to the
// Token Manager, so that the transport header does not pull auth/token_manager.h and
// everything it brings — PKCE, SHA-256, the authorization request builder — into every
// translation unit that wants to know what a connection state is. This is the one file
// that includes both.
//
// It is header-only and has no I/O, so the suite covers it directly. That matters more
// than it looks: an adapter that is "obviously correct" and untested is where the
// `retryable` distinction gets flattened, and flattening it means either prompting a
// sign-in because a studio's Wi-Fi dropped, or looping forever on a session the server
// has revoked.
//
// The mapping is two calls and one struct conversion. `auth::operation_outcome` and
// `token_refresh_outcome` carry the same three fields for the same reasons; they are
// separate types rather than one shared type because the alternative is the transport
// layer depending on the auth layer's header for a struct, which is the dependency this
// file exists to confine.

#ifndef SESH_AI_TRANSPORT_TOKEN_MANAGER_ACCESS_TOKEN_SOURCE_H
#define SESH_AI_TRANSPORT_TOKEN_MANAGER_ACCESS_TOKEN_SOURCE_H

#include <auth/token_manager.h>
#include <transport/transport_client.h>

#include <optional>
#include <utility>

namespace sesh_ai::transport {

	// `auth::operation_outcome` in the transport layer's terms.
	inline token_refresh_outcome as_token_refresh_outcome(const auth::operation_outcome& outcome)
	{
		token_refresh_outcome converted;

		converted.succeeded = outcome.succeeded;
		converted.retryable = outcome.retryable;
		converted.failure_reason = outcome.failure_reason;

		return converted;
	}

	// The production `AccessTokenSource`.
	//
	// Holds a reference rather than owning the Token Manager: one token set exists, in
	// one place, and the Token Manager is neither copyable nor movable for exactly that
	// reason.
	class TokenManagerAccessTokenSource final : public AccessTokenSource {
	public:
		explicit TokenManagerAccessTokenSource(auth::TokenManager& token_manager)
			: token_manager_{token_manager}
		{
		}

		// `usable_access_token` already returns empty for a token that is expired or
		// for a session that is not in a state to serve one, which is the whole of the
		// gating this side needs — so there is no second expiry check here that could
		// disagree with the Token Manager's.
		std::optional<auth::secret_string> current_access_token() override
		{
			return token_manager_.usable_access_token();
		}

		token_refresh_outcome refresh_access_token() override
		{
			return as_token_refresh_outcome(token_manager_.refresh_access_token());
		}

	private:
		auth::TokenManager& token_manager_;
	};

}

#endif
