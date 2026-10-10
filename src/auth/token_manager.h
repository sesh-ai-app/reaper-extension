// The Token Manager (requirement 17, design "Token Manager").
//
// Runs the Cognito authorization code flow with PKCE against the managed login page,
// holds the resulting tokens in memory for the life of the REAPER session, refreshes the
// access token ahead of expiry, and hands the access token to the Socket.IO handshake.
// Nothing it holds outlives the process, and nothing it holds is ever written anywhere.
//
// ---------------------------------------------------------------------------
// In memory only, and why that is structural rather than a convention
//
// Requirement 17.4: all tokens in memory only — no persistent storage, no platform
// keychain, no secret material on disk. Requirement 26.1: Cognito tokens and the IVS
// publish token are the only credential material the extension ever sees. This repository
// is public, so "we were careful" is not a defence anybody can check. Four things make
// the no-disk property hard to violate by accident:
//
//   - **Persistence is not an expressible operation.** Neither seam below has a `save`,
//     `load`, `store`, or `keychain` method, and neither does this class. There is no
//     function to call, so there is no call to review. Nothing in src/auth includes
//     `<fstream>`, `<filesystem>`, or `<cstdio>`, and adding a write would mean adding
//     one of them to a component whose header says why it does not have them.
//   - **Tokens cannot be handed in from outside.** The only constructor takes a
//     configuration and three seams. There is no method that accepts a token, a refresh
//     token, or a serialised session. So there is no route by which a token saved during
//     one launch could be loaded into the next — which is requirement 17.7 holding
//     because the alternative is unrepresentable, not because a code path chose not to
//     take it. A freshly constructed manager is `signed_out`, always.
//   - **Tokens cannot be logged.** They are `secret_string` (see auth/secret_string.h):
//     no `operator<<`, no implicit conversion, no Catch2 stringifier, one deliberately
//     conspicuous `reveal()`. `describe_for_log` is what a log line gets, and it reports
//     lengths and times, never characters. The suite asserts that no token text appears
//     in it or in any failure reason this class produces.
//   - **Tokens are zeroised.** `secret_string` overwrites its buffer through a volatile
//     pointer on destruction, on reassignment, and on `clear()`, so a token does not
//     survive in the process image for a crash dump to collect. `sign_out` clears
//     everything, and so does the start of a new sign-in.
//
// This class is neither copyable nor movable. One token set exists, in one place, and it
// is not duplicated into a container or a lambda capture.
//
// ---------------------------------------------------------------------------
// Fresh sign-in on every launch (requirement 17.7)
//
// Nothing carries across launches — not the refresh token, not the ID token, not a
// session cookie this component owns. That follows from the paragraph above, but it is
// also the intended producer experience rather than an accepted cost: with a passkey on
// the platform authenticator, signing in is a Touch ID tap, so a fresh sign-in each
// launch is cheaper than remembering a password would be, and it means a stolen laptop
// yields no Sesh session.
//
// ---------------------------------------------------------------------------
// Refreshing ahead of expiry (requirement 17.5), and the lead time
//
// A Cognito access token lasts 60 minutes by default, configurable from 5 minutes to 24
// hours. `refresh_lead_time` returns `min(5 minutes, lifetime / 2)`.
//
// Five minutes because the refresh has to fit inside the lead: one timer tick to notice
// (the dispatcher's tick, not a dedicated timer), one HTTPS round trip to Cognito, and
// room for that round trip to fail and be retried on the following tick, all on whatever
// network a studio happens to have. Five minutes covers that with room to spare, and
// costs one extra exchange per hour against a 60-minute token — which is the trade
// requirement 17.5 is asking for: the producer is mid-take and must not be interrupted.
//
// Half the lifetime as a ceiling because a 5-minute token — Cognito's floor — with a
// 5-minute lead would be due for refresh the instant it was issued, and the manager would
// spend the session exchanging tokens. Capping at half the lifetime keeps at most one
// refresh per half-life whatever the pool is configured for, and the arithmetic stays
// correct at the extreme instead of needing a special case.
//
// A refresh failure is not the same as an expiry. A network failure leaves the existing
// token in place and the state `signed_in`, so the next tick tries again and the producer
// notices nothing. A refused refresh token is terminal — it cannot be retried into
// working — so the tokens are cleared and the producer is asked to sign in again, which is
// the only honest outcome.
//
// ---------------------------------------------------------------------------
// Two seams, and neither of them is here
//
// Following `entry/timer_registration.h`: this component declares the narrowest
// interfaces it needs and nothing here includes CEF, the REAPER SDK, a TLS stack, or a
// JSON parser.
//
// `LoginPagePresenter` is the browser. In production it is the CEF browser the UI Host
// already owns (task 17.2) — design.md has the UI Host driving the Token Manager for
// exactly this reason, so the browser is created and shut down in one place rather than
// two. The CEF-backed implementation therefore lands with the UI Host, not here: CEF is a
// separate binary distribution that this build only locates behind `SESH_AI_CEF_FOUND`,
// and a CEF-dependent translation unit under src/ would be swept into the extension
// library's source glob and break the build for everyone configuring without it.
//
// `TokenEndpoint` is one HTTPS POST. Its production implementation belongs with the
// transport layer, which is what brings the TLS stack and the JSON parser. It is declared
// so that everything either side of the exchange is testable now, and so the shape of the
// exchange — including the fact that the response is handed over already decomposed, with
// no JSON document for a token to be logged from — is fixed before anything implements it.
//
// What that leaves in this file is the whole of the flow's decision-making: PKCE
// generation, the authorization URL, redirect validation, the state nonce check, the
// exchange, expiry arithmetic, refresh scheduling, and the state machine. All of it runs
// in the suite without CEF.

#ifndef SESH_AI_AUTH_TOKEN_MANAGER_H
#define SESH_AI_AUTH_TOKEN_MANAGER_H

#include <auth/authorization_request.h>
#include <auth/pkce.h>
#include <auth/secret_string.h>
#include <auth/secure_random.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sesh_ai::auth
{
	// Monotonic on purpose. A token's lifetime arrives as "expires in N seconds", and the
	// wall clock is not a safe base for that: NTP steps it, the producer changes it, a
	// laptop resuming from sleep corrects it. A steady clock cannot go backwards, so a
	// token cannot appear to un-expire, and a refresh that is due cannot be un-due.
	//
	// Sleep is the honest caveat: a steady clock on macOS and Linux does not advance while
	// the machine is suspended, so a laptop closed for two hours wakes believing less time
	// passed than the authorization server thinks. The token is then expired server-side
	// while this side still considers it fresh — which is precisely the case requirement
	// 17.6 covers: the server drops the socket and the Transport Client reconnects with a
	// refreshed token, so the producer sees a reconnecting state rather than an error.
	// Trying to detect the gap by cross-checking a wall clock would trade that handled
	// case for a new class of bug on every clock adjustment.
	using steady_clock = std::chrono::steady_clock;
	using clock_reader = std::function<steady_clock::time_point()>;

	enum class authentication_state
	{
		// A freshly constructed manager, and a signed-out one. Holds nothing.
		signed_out,

		// The managed login page is showing. The producer is choosing a method, tapping a
		// platform authenticator, or typing a password. No token exists yet; the PKCE
		// verifier and state nonce do.
		awaiting_redirect,

		// The redirect arrived and carried a code; the exchange is in flight.
		exchanging_code,

		// Tokens held. The steady state for a session.
		signed_in,

		// A refresh is in flight. The existing access token is still valid and still
		// served — that is the point of refreshing ahead of expiry.
		refreshing,

		// This sign-in attempt cannot be recovered without starting over. `begin_sign_in`
		// is the only way out.
		failed
	};

	inline const char* describe_authentication_state(authentication_state state)
	{
		switch (state)
		{
			case authentication_state::signed_out: return "signed_out";
			case authentication_state::awaiting_redirect: return "awaiting_redirect";
			case authentication_state::exchanging_code: return "exchanging_code";
			case authentication_state::signed_in: return "signed_in";
			case authentication_state::refreshing: return "refreshing";
			case authentication_state::failed: return "failed";
		}

		return "unknown";
	}

	// The result of any operation on the manager.
	//
	// `retryable` separates "the network was unavailable" from "the authorization server
	// said no". The first is worth another attempt on the next tick; the second needs the
	// producer, and retrying it is a loop that never terminates. Task 15.1's reconnect
	// logic reads exactly this distinction.
	//
	// `failure_reason` is written for a log line and for the UI, which means it never
	// contains token material. The suite asserts that.
	struct operation_outcome
	{
		bool succeeded = false;
		bool retryable = false;
		std::string failure_reason;

		static operation_outcome success()
		{
			operation_outcome outcome;

			outcome.succeeded = true;

			return outcome;
		}

		static operation_outcome failure(std::string reason, bool retryable = false)
		{
			operation_outcome outcome;

			outcome.succeeded = false;
			outcome.retryable = retryable;
			outcome.failure_reason = std::move(reason);

			return outcome;
		}
	};

	enum class token_grant
	{
		authorization_code,
		refresh_token
	};

	// One POST to `token_endpoint`, `application/x-www-form-urlencoded`, over TLS.
	//
	// The fields are named rather than handed over as a form because two of them are
	// secrets: the implementation percent-encodes them into a body and does not get the
	// opportunity to put them anywhere else. There is no `client_secret` — see
	// authorization_request.h on why this is a public client.
	struct token_endpoint_request
	{
		std::string token_endpoint;
		std::string client_id;
		token_grant grant = token_grant::authorization_code;

		// `authorization_code` grant only. The redirect URI is echoed back for the
		// server's own binding check.
		std::string redirect_uri;
		secret_string authorization_code;
		secret_string code_verifier;

		// `refresh_token` grant only.
		secret_string refresh_token;
	};

	// The response, already decomposed.
	//
	// Deliberately not a JSON document. A parsed token response held as JSON is one
	// `dump()` away from a token in a log file, and the JSON dependency is optional in this
	// build anyway — keeping the parse on the other side of the seam means the whole flow
	// is testable without it, and means there is no document here for anything to
	// serialise.
	struct token_endpoint_response
	{
		// False when the request never got an answer: DNS, TLS, timeout, refused
		// connection. Distinguished from an answer that said no, because only this one is
		// worth retrying.
		bool transport_succeeded = false;

		int http_status_code = 0;

		// OAuth 2.0 error code and description when the endpoint refused, e.g.
		// `invalid_grant`. Public values, not credentials.
		std::string error;
		std::string error_description;

		secret_string access_token;
		secret_string id_token;

		// Absent on a refresh response — Cognito does not reissue one — and the manager
		// keeps the one it already has in that case.
		secret_string refresh_token;

		long long expires_in_seconds = 0;
		std::string token_type;
	};

	class TokenEndpoint
	{
	public:
		virtual ~TokenEndpoint() = default;

		virtual token_endpoint_response exchange(const token_endpoint_request& request) = 0;
	};

	// The browser showing Cognito's managed login page. CEF-backed in production, owned by
	// the UI Host — see the header comment.
	//
	// Two operations, because two is all this component needs. It does not create the
	// browser, size it, dock it, or know it is CEF. WebAuthn needs nothing here at all:
	// the platform authenticator dialog is presented by the operating system through
	// Chromium's own WebAuthn path inside the page, so from this side a passkey sign-in and
	// a password sign-in are the same navigation ending in the same redirect.
	class LoginPagePresenter
	{
	public:
		virtual ~LoginPagePresenter() = default;

		// False when no browser could be shown, which is a sign-in that cannot start.
		virtual bool present_authorization_page(const std::string& authorization_url) = 0;

		// Called once the flow has ended, whichever way it ended. Idempotent by contract:
		// the manager may call it on a flow that was never presented.
		virtual void dismiss_authorization_page() = 0;
	};

	// See the header comment for the reasoning. Exposed as a free function so the suite can
	// state it directly at both extremes of Cognito's configurable range.
	inline std::chrono::seconds refresh_lead_time(std::chrono::seconds access_token_lifetime)
	{
		constexpr std::chrono::seconds preferred_lead{5 * 60};

		if (access_token_lifetime <= std::chrono::seconds::zero())
		{
			return std::chrono::seconds::zero();
		}

		const std::chrono::seconds half_of_lifetime{access_token_lifetime.count() / 2};

		return half_of_lifetime < preferred_lead ? half_of_lifetime : preferred_lead;
	}

	class TokenManager
	{
	public:
		TokenManager(
			cognito_configuration configuration,
			LoginPagePresenter& login_page,
			TokenEndpoint& token_endpoint,
			SecureRandomSource& random_source,
			clock_reader read_clock = {})
			: configuration_{std::move(configuration)}
			, login_page_{login_page}
			, token_endpoint_{token_endpoint}
			, random_source_{random_source}
			, read_clock_{read_clock ? std::move(read_clock) : clock_reader{[] { return steady_clock::now(); }}}
		{
		}

		// One token set, in one place. See the header comment.
		TokenManager(const TokenManager&) = delete;
		TokenManager& operator=(const TokenManager&) = delete;
		TokenManager(TokenManager&&) = delete;
		TokenManager& operator=(TokenManager&&) = delete;

		authentication_state state() const { return state_; }

		const cognito_configuration& configuration() const { return configuration_; }

		// Requirements 17.1, 17.2, 17.3, 17.7. Generates a fresh PKCE pair and state nonce
		// and asks the presenter to show the managed login page.
		//
		// Clears whatever was held first, so a re-sign-in never leaves a stale token
		// reachable and a second call cannot reuse the first call's verifier.
		operation_outcome begin_sign_in()
		{
			clear_all_secrets();

			const std::vector<configuration_problem> problems =
				find_configuration_problems(configuration_);

			if (!problems.empty())
			{
				state_ = authentication_state::failed;

				std::string reason = "Cognito is not configured: ";

				for (std::size_t index = 0; index < problems.size(); ++index)
				{
					if (index > 0)
					{
						reason += "; ";
					}

					reason += problems[index].field + " " + problems[index].reason;
				}

				return operation_outcome::failure(std::move(reason));
			}

			std::optional<pkce_pair> pair = generate_pkce_pair(random_source_);
			std::optional<std::string> nonce = generate_state_nonce(random_source_);

			if (!pair.has_value() || !nonce.has_value())
			{
				state_ = authentication_state::failed;

				// Retryable: an entropy source that failed once may not fail again, and
				// the alternative to retrying is not a weaker source — it is no sign-in.
				return operation_outcome::failure(
					"the operating system's secure random source is unavailable, so sign-in cannot start",
					true);
			}

			const std::string url = build_authorization_url(configuration_, pair->code_challenge, *nonce);

			if (!login_page_.present_authorization_page(url))
			{
				state_ = authentication_state::failed;

				return operation_outcome::failure("the sign-in page could not be opened", true);
			}

			pending_code_verifier_ = std::move(pair->code_verifier);
			pending_state_nonce_ = std::move(*nonce);
			authorization_url_ = url;
			state_ = authentication_state::awaiting_redirect;

			return operation_outcome::success();
		}

		// The authorization URL currently showing. Carries the code challenge, never the
		// verifier — public by design, safe to log, and the suite checks that.
		const std::string& authorization_url() const { return authorization_url_; }

		// Whether a navigation the browser observed is the redirect that ends the flow.
		//
		// The browser sees many navigations during a sign-in — the login page, its assets,
		// a social provider, whatever the WebAuthn ceremony touches — and only one of them
		// is this. The presenter's owner filters with this before calling
		// `complete_sign_in`.
		bool is_redirect_navigation(std::string_view navigated_url) const
		{
			return parse_redirect_url(navigated_url, configuration_.redirect_uri).matches_redirect_uri;
		}

		// Requirement 17.1. Validates the redirect, exchanges the code for tokens, and
		// dismisses the login page.
		//
		// Synchronous against the seam: the HTTPS POST happens inside this call. That is
		// the transport implementation's concern to schedule off the main thread — this
		// component does no threading of its own, so there is no half-completed exchange
		// for a caller to observe.
		operation_outcome complete_sign_in(std::string_view redirect_url)
		{
			if (state_ != authentication_state::awaiting_redirect)
			{
				return operation_outcome::failure("no sign-in is waiting for a redirect");
			}

			const redirect_parameters parameters =
				parse_redirect_url(redirect_url, configuration_.redirect_uri);

			if (!parameters.matches_redirect_uri)
			{
				// Not the redirect. The flow is untouched and still waiting, so this is not
				// a state change — the producer is probably still on the login page.
				return operation_outcome::failure(
					"the navigation is not the configured redirect, so the sign-in is still waiting", true);
			}

			if (!parameters.error.empty())
			{
				// The producer cancelled, or the authorization server refused. Either way
				// there is no code and no retry that would change it.
				std::string reason = "sign-in was refused: " + parameters.error;

				if (!parameters.error_description.empty())
				{
					reason += " (" + parameters.error_description + ")";
				}

				abandon_flow();

				return operation_outcome::failure(std::move(reason));
			}

			// The CSRF check. A redirect whose state does not match the nonce this manager
			// generated is a redirect this manager did not ask for, and the code in it is
			// not to be exchanged. Compared without an early exit so the comparison does
			// not report how much of the nonce a caller guessed.
			if (parameters.state.empty() ||
				!detail::constant_time_equals(parameters.state, pending_state_nonce_))
			{
				abandon_flow();

				return operation_outcome::failure(
					"the redirect carried an unexpected state value, so it was not this sign-in");
			}

			if (parameters.authorization_code.empty())
			{
				abandon_flow();

				return operation_outcome::failure("the redirect carried no authorization code");
			}

			state_ = authentication_state::exchanging_code;

			token_endpoint_request request;

			request.token_endpoint = configuration_.token_endpoint;
			request.client_id = configuration_.client_id;
			request.grant = token_grant::authorization_code;
			request.redirect_uri = configuration_.redirect_uri;
			request.authorization_code = secret_string{parameters.authorization_code};
			request.code_verifier = pending_code_verifier_;

			token_endpoint_response response = token_endpoint_.exchange(request);

			// A verifier and a nonce are single-use. Whatever the exchange did, they are
			// spent, and a retry means a new authorization request.
			pending_code_verifier_.clear();
			pending_state_nonce_.clear();

			login_page_.dismiss_authorization_page();
			authorization_url_.clear();

			const operation_outcome accepted = accept_token_response(response, true);

			if (!accepted.succeeded)
			{
				state_ = authentication_state::failed;
			}

			return accepted;
		}

		// Requirement 17.5. True once the access token is inside its refresh lead time.
		bool refresh_due() const
		{
			if (state_ != authentication_state::signed_in)
			{
				return false;
			}

			if (refresh_token_.empty() || !refresh_at_.has_value())
			{
				return false;
			}

			return read_clock_() >= *refresh_at_;
		}

		// What the main-thread dispatcher's tick calls. Empty when no refresh was due, so a
		// caller can tell "nothing to do" from "tried and succeeded".
		std::optional<operation_outcome> refresh_if_due()
		{
			if (!refresh_due())
			{
				return std::nullopt;
			}

			return refresh_access_token();
		}

		// Requirement 17.5, and the retry path task 15.1 uses when the server rejects a
		// handshake on an expired token.
		operation_outcome refresh_access_token()
		{
			if (state_ != authentication_state::signed_in)
			{
				return operation_outcome::failure("there is no session to refresh");
			}

			if (refresh_token_.empty())
			{
				return operation_outcome::failure(
					"this session has no refresh token, so signing in again is the only way forward");
			}

			state_ = authentication_state::refreshing;

			token_endpoint_request request;

			request.token_endpoint = configuration_.token_endpoint;
			request.client_id = configuration_.client_id;
			request.grant = token_grant::refresh_token;
			request.refresh_token = refresh_token_;

			token_endpoint_response response = token_endpoint_.exchange(request);

			if (!response.transport_succeeded)
			{
				// The existing token is untouched and very likely still valid — that is
				// what the lead time bought. Back to signed_in so the next tick tries
				// again and the producer notices nothing.
				state_ = authentication_state::signed_in;

				return operation_outcome::failure("the token endpoint could not be reached", true);
			}

			const operation_outcome accepted = accept_token_response(response, false);

			if (!accepted.succeeded)
			{
				// The refresh token was refused, or the response was unusable. Neither
				// gets better on a retry, and continuing to serve an access token that is
				// about to expire only delays the same prompt.
				clear_all_secrets();
				state_ = authentication_state::failed;
			}

			return accepted;
		}

		// The access token, and the accessor the Socket.IO handshake `auth` uses (task 15.1)
		// and that any REST call to the server uses.
		//
		// Empty unless there is a token that is actually usable: signed in — or mid-refresh,
		// where the previous token is still the current one and still valid, which is the
		// whole point of refreshing ahead of expiry — and not past expiry. Handing out an
		// expired token would produce a handshake the server rejects, and the rejection
		// would look like a protocol fault rather than an expiry. An empty result is the
		// caller's signal to refresh (requirement 17.6) or to prompt a sign-in.
		std::optional<secret_string> usable_access_token() const
		{
			if (!is_token_currently_usable())
			{
				return std::nullopt;
			}

			if (access_token_.empty())
			{
				return std::nullopt;
			}

			return access_token_;
		}

		bool has_usable_access_token() const { return usable_access_token().has_value(); }

		// The OIDC identity token. Says who the producer is; it is not what authorises the
		// socket. Gated on the same validity as the access token, so a stale identity is not
		// handed out either.
		std::optional<secret_string> usable_id_token() const
		{
			if (!is_token_currently_usable())
			{
				return std::nullopt;
			}

			if (id_token_.empty())
			{
				return std::nullopt;
			}

			return id_token_;
		}

		// Zero when there is no token, or when it has already expired.
		std::chrono::seconds time_until_access_token_expiry() const
		{
			if (!access_token_expires_at_.has_value())
			{
				return std::chrono::seconds::zero();
			}

			const steady_clock::time_point now = read_clock_();

			if (now >= *access_token_expires_at_)
			{
				return std::chrono::seconds::zero();
			}

			return std::chrono::duration_cast<std::chrono::seconds>(*access_token_expires_at_ - now);
		}

		// Zero when a refresh is already due, or when there is nothing to refresh.
		std::chrono::seconds time_until_refresh_due() const
		{
			if (!refresh_at_.has_value())
			{
				return std::chrono::seconds::zero();
			}

			const steady_clock::time_point now = read_clock_();

			if (now >= *refresh_at_)
			{
				return std::chrono::seconds::zero();
			}

			return std::chrono::duration_cast<std::chrono::seconds>(*refresh_at_ - now);
		}

		// The lifetime the authorization server reported for the current access token.
		std::chrono::seconds access_token_lifetime() const { return access_token_lifetime_; }

		// Clears everything and dismisses the login page if it is still up. The manager is
		// then indistinguishable from a freshly constructed one.
		void sign_out()
		{
			const bool login_page_is_showing = state_ == authentication_state::awaiting_redirect;

			clear_all_secrets();
			state_ = authentication_state::signed_out;

			if (login_page_is_showing)
			{
				login_page_.dismiss_authorization_page();
			}
		}

		// What a log line may say. Lengths and times, never characters — see
		// `secret_string::describe_for_log`.
		std::string describe_for_log() const
		{
			std::string description = "authentication state=";

			description += describe_authentication_state(state_);
			description += " access_token=" + access_token_.describe_for_log();
			description += " id_token=" + id_token_.describe_for_log();
			description += " refresh_token=" + refresh_token_.describe_for_log();
			description +=
				" expires_in=" + std::to_string(time_until_access_token_expiry().count()) + "s";
			description += " refresh_in=" + std::to_string(time_until_refresh_due().count()) + "s";

			return description;
		}

	private:
		// Whether the session is in a state where the tokens it holds are worth handing out.
		bool is_token_currently_usable() const
		{
			if (state_ != authentication_state::signed_in && state_ != authentication_state::refreshing)
			{
				return false;
			}

			if (!access_token_expires_at_.has_value())
			{
				return false;
			}

			return read_clock_() < *access_token_expires_at_;
		}

		// Validates a token response and takes it, or explains why it was not usable.
		//
		// `is_initial_exchange` distinguishes the two grants: the authorization code grant
		// must produce a refresh token to be worth anything for a long session, while a
		// refresh response legitimately omits one and the existing token is kept.
		operation_outcome accept_token_response(token_endpoint_response& response, bool is_initial_exchange)
		{
			if (!response.transport_succeeded)
			{
				return operation_outcome::failure("the token endpoint could not be reached", true);
			}

			if (!response.error.empty())
			{
				std::string reason = "the token endpoint refused the exchange: " + response.error;

				if (!response.error_description.empty())
				{
					reason += " (" + response.error_description + ")";
				}

				return operation_outcome::failure(std::move(reason));
			}

			if (response.http_status_code != 200)
			{
				return operation_outcome::failure(
					"the token endpoint answered with status " + std::to_string(response.http_status_code));
			}

			if (response.access_token.empty())
			{
				return operation_outcome::failure("the token endpoint returned no access token");
			}

			// Cognito returns "Bearer". An empty value is tolerated because the token type
			// carries no security decision here — the token goes into the Socket.IO
			// handshake, not an Authorization header whose scheme has to match — but a
			// value that names some *other* scheme means the response is not what this
			// client understands.
			if (!response.token_type.empty() && !is_bearer_token_type(response.token_type))
			{
				return operation_outcome::failure(
					"the token endpoint returned an unsupported token type");
			}

			if (response.expires_in_seconds <= 0)
			{
				return operation_outcome::failure(
					"the token endpoint reported no usable access token lifetime");
			}

			if (is_initial_exchange && response.refresh_token.empty())
			{
				// Without a refresh token there is no way to satisfy requirement 17.5, and
				// the producer would be re-prompted mid-session at expiry. That is a
				// misconfigured app client, and failing loudly at sign-in is better than
				// discovering it an hour into a session.
				return operation_outcome::failure(
					"the token endpoint returned no refresh token, so the session could not be kept alive");
			}

			const steady_clock::time_point now = read_clock_();
			const std::chrono::seconds lifetime{response.expires_in_seconds};

			access_token_ = std::move(response.access_token);
			id_token_ = std::move(response.id_token);

			if (!response.refresh_token.empty())
			{
				refresh_token_ = std::move(response.refresh_token);
			}

			access_token_lifetime_ = lifetime;
			access_token_expires_at_ = now + lifetime;
			refresh_at_ = now + (lifetime - refresh_lead_time(lifetime));
			state_ = authentication_state::signed_in;

			return operation_outcome::success();
		}

		static bool is_bearer_token_type(std::string_view token_type)
		{
			static constexpr std::string_view bearer{"bearer"};

			if (token_type.size() != bearer.size())
			{
				return false;
			}

			for (std::size_t index = 0; index < bearer.size(); ++index)
			{
				char character = token_type[index];

				if (character >= 'A' && character <= 'Z')
				{
					character = static_cast<char>(character - 'A' + 'a');
				}

				if (character != bearer[index])
				{
					return false;
				}
			}

			return true;
		}

		// A sign-in that ended without tokens. The login page comes down and the
		// single-use material is spent.
		void abandon_flow()
		{
			pending_code_verifier_.clear();
			pending_state_nonce_.clear();
			authorization_url_.clear();
			state_ = authentication_state::failed;
			login_page_.dismiss_authorization_page();
		}

		void clear_all_secrets()
		{
			access_token_.clear();
			id_token_.clear();
			refresh_token_.clear();
			pending_code_verifier_.clear();
			pending_state_nonce_.clear();
			authorization_url_.clear();
			access_token_expires_at_.reset();
			refresh_at_.reset();
			access_token_lifetime_ = std::chrono::seconds::zero();
		}

		cognito_configuration configuration_;
		LoginPagePresenter& login_page_;
		TokenEndpoint& token_endpoint_;
		SecureRandomSource& random_source_;
		clock_reader read_clock_;

		authentication_state state_ = authentication_state::signed_out;

		secret_string access_token_;
		secret_string id_token_;
		secret_string refresh_token_;

		// In flight during a sign-in only. The verifier is a secret; the nonce is not, but
		// it is cleared on the same schedule because both are single-use.
		secret_string pending_code_verifier_;
		std::string pending_state_nonce_;

		std::string authorization_url_;

		std::optional<steady_clock::time_point> access_token_expires_at_;
		std::optional<steady_clock::time_point> refresh_at_;
		std::chrono::seconds access_token_lifetime_{0};
	};
}

#endif
