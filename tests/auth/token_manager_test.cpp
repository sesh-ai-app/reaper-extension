// The Token Manager — requirement 17 end to end, without CEF and without a network.
//
// The flow is driven through its two seams, so everything that decides anything is under test:
// what the authorization request contains, which redirects are accepted, what is sent to the
// token endpoint, when a refresh becomes due, what happens when one fails, and what the
// manager will and will not hand out.
//
// Three of these assertions are the requirement rather than an implementation detail, and are
// grouped at the end: that a freshly constructed manager holds nothing and has no route to
// receive a token from a previous launch (17.7), that signing in clears whatever was held
// (17.4), and that no token material reaches a log line or a failure reason (26.4).

#include <chrono>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <auth/token_manager.h>

#include "authentication_test_doubles.h"

using sesh_ai::auth::authentication_state;
using sesh_ai::auth::operation_outcome;
using sesh_ai::auth::parse_redirect_url;
using sesh_ai::auth::percent_encode_component;
using sesh_ai::auth::refresh_lead_time;
using sesh_ai::auth::secret_string;
using sesh_ai::auth::token_endpoint_response;
using sesh_ai::auth::token_grant;
using sesh_ai::auth::TokenManager;
using sesh_ai::tests::DeterministicRandomSource;
using sesh_ai::tests::example_access_token;
using sesh_ai::tests::example_id_token;
using sesh_ai::tests::example_refresh_token;
using sesh_ai::tests::example_rotated_access_token;
using sesh_ai::tests::ManualClock;
using sesh_ai::tests::RecordingLoginPagePresenter;
using sesh_ai::tests::ScriptedTokenEndpoint;
using sesh_ai::tests::sign_in;
using sesh_ai::tests::successful_token_response;
using sesh_ai::tests::test_configuration;

namespace
{
	// Everything a manager needs, assembled once. Declaration order matters: the doubles
	// outlive the manager that holds references to them.
	struct authentication_fixture
	{
		RecordingLoginPagePresenter login_page;
		ScriptedTokenEndpoint token_endpoint;
		DeterministicRandomSource random_source;
		ManualClock clock;

		TokenManager token_manager{
			test_configuration(), login_page, token_endpoint, random_source, clock.reader()};

		// The state nonce the manager generated, read back out of the URL it published —
		// which is the only place it appears, and is where Cognito reads it from too.
		std::string presented_state() const
		{
			return parse_redirect_url(
				login_page.presented_urls.back(),
				"https://login.example.invalid/oauth2/authorize").state;
		}

		std::string redirect_with_code(const std::string& code) const
		{
			return "https://extension.example.invalid/signed-in?code=" +
				percent_encode_component(code) + "&state=" + percent_encode_component(presented_state());
		}
	};

	constexpr std::chrono::seconds one_hour{3600};
}

TEST_CASE("beginning a sign-in presents the managed login page", "[auth][token_manager]")
{
	authentication_fixture fixture;

	const operation_outcome started = fixture.token_manager.begin_sign_in();

	REQUIRE(started.succeeded);
	REQUIRE(fixture.token_manager.state() == authentication_state::awaiting_redirect);
	REQUIRE(fixture.login_page.present_call_count == 1);
	REQUIRE(fixture.login_page.presented_urls.size() == 1);
	REQUIRE(fixture.login_page.presented_urls.back() == fixture.token_manager.authorization_url());
	REQUIRE(fixture.login_page.presented_urls.back().find("code_challenge_method=S256") !=
		std::string::npos);

	// No token exists yet, so nothing is handed to a handshake.
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
}

TEST_CASE("the authorization URL never carries the code verifier", "[auth][token_manager]")
{
	// The failure that would silently turn S256 back into `plain`: publishing the verifier
	// alongside the challenge. The verifier is only ever sent in the token exchange body.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response());

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const std::string url = fixture.token_manager.authorization_url();

	fixture.token_manager.complete_sign_in(fixture.redirect_with_code("an-authorization-code"));

	REQUIRE(fixture.token_endpoint.received_requests.size() == 1);

	const std::string verifier =
		fixture.token_endpoint.received_requests.front().code_verifier.reveal();

	REQUIRE_FALSE(verifier.empty());
	REQUIRE(url.find(verifier) == std::string::npos);
	REQUIRE(url.find(percent_encode_component(verifier)) == std::string::npos);
}

TEST_CASE("the exchange sends the verifier matching the published challenge", "[auth][token_manager]")
{
	// The server hashes the verifier and compares it to the challenge it stored. If these two
	// ever came from different draws, every sign-in would fail against a real Cognito and pass
	// against a lenient double — so it is pinned here.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response());

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const std::string url = fixture.token_manager.authorization_url();

	REQUIRE(fixture.token_manager.complete_sign_in(
		fixture.redirect_with_code("an-authorization-code")).succeeded);

	const auto& request = fixture.token_endpoint.received_requests.front();

	const std::string challenge =
		sesh_ai::auth::pkce_code_challenge_from_verifier(request.code_verifier.reveal());

	REQUIRE(url.find("code_challenge=" + percent_encode_component(challenge)) != std::string::npos);
}

TEST_CASE("the exchange sends the authorization code and no client secret", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response());

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);
	REQUIRE(fixture.token_manager.complete_sign_in(
		fixture.redirect_with_code("the-authorization-code")).succeeded);

	const auto& request = fixture.token_endpoint.received_requests.front();

	REQUIRE(request.grant == token_grant::authorization_code);
	REQUIRE(request.token_endpoint == test_configuration().token_endpoint);
	REQUIRE(request.client_id == test_configuration().client_id);
	REQUIRE(request.redirect_uri == test_configuration().redirect_uri);
	REQUIRE(request.authorization_code.reveal() == "the-authorization-code");
	REQUIRE(request.refresh_token.empty());

	// There is no client_secret field on the request type at all. This extension is a public
	// client and PKCE is what replaces the secret — see authorization_request.h.
}

TEST_CASE("a successful exchange signs in and exposes the access token", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);
	REQUIRE(fixture.token_manager.has_usable_access_token());

	const std::optional<secret_string> token = fixture.token_manager.usable_access_token();

	REQUIRE(token.has_value());
	REQUIRE(token->reveal() == std::string{example_access_token});

	const std::optional<secret_string> identity = fixture.token_manager.usable_id_token();

	REQUIRE(identity.has_value());
	REQUIRE(identity->reveal() == std::string{example_id_token});

	REQUIRE(fixture.token_manager.access_token_lifetime() == one_hour);
	REQUIRE(fixture.token_manager.time_until_access_token_expiry() == one_hour);

	// The login page comes down once the flow is over, whichever way it ended.
	REQUIRE(fixture.login_page.dismiss_call_count == 1);
	REQUIRE(fixture.token_manager.authorization_url().empty());
}

TEST_CASE("a redirect with a mismatched state is refused and nothing is exchanged",
	"[auth][token_manager]")
{
	// The CSRF check. A redirect this manager did not ask for carries a code this manager must
	// not exchange, however well-formed the rest of it looks.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response());

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?code=injected&state=not-the-nonce");

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE_FALSE(completed.retryable);
	REQUIRE(completed.failure_reason.find("state") != std::string::npos);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
	REQUIRE(fixture.login_page.dismiss_call_count == 1);
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
}

TEST_CASE("a redirect with no state at all is refused", "[auth][token_manager]")
{
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?code=injected");

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("a redirect with a matching state but no code is refused", "[auth][token_manager]")
{
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?state=" +
		percent_encode_component(fixture.presented_state()));

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE(completed.failure_reason.find("authorization code") != std::string::npos);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("a redirect carrying an error ends the sign-in without a retry", "[auth][token_manager]")
{
	// The producer cancelled, or the pool refused. Retrying the same redirect cannot change it.
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?error=access_denied"
		"&error_description=The%20producer%20cancelled");

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE_FALSE(completed.retryable);
	REQUIRE(completed.failure_reason.find("access_denied") != std::string::npos);
	REQUIRE(completed.failure_reason.find("The producer cancelled") != std::string::npos);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("a navigation that is not the redirect leaves the sign-in waiting", "[auth][token_manager]")
{
	// The browser navigates many times during a sign-in — the login page, its assets, a social
	// provider, whatever the WebAuthn ceremony touches. None of those may end the flow.
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	REQUIRE_FALSE(fixture.token_manager.is_redirect_navigation(
		"https://login.example.invalid/oauth2/authorize?response_type=code"));
	REQUIRE_FALSE(fixture.token_manager.is_redirect_navigation(
		"https://accounts.google.example.invalid/o/oauth2/auth"));
	REQUIRE(fixture.token_manager.is_redirect_navigation(fixture.redirect_with_code("abc")));

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://login.example.invalid/oauth2/authorize?response_type=code");

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE(completed.retryable);
	REQUIRE(fixture.token_manager.state() == authentication_state::awaiting_redirect);
	REQUIRE(fixture.login_page.dismiss_call_count == 0);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("completing a sign-in that was never started is refused", "[auth][token_manager]")
{
	// A redirect arriving with no flow in progress is either a stale navigation or an attempt to
	// inject one. Either way there is no verifier to exchange with, and the state stays
	// signed_out rather than moving to failed — nothing was attempted.
	authentication_fixture fixture;

	const operation_outcome completed = fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?code=abc&state=abc");

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE(fixture.token_manager.state() == authentication_state::signed_out);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("a verifier is spent after one exchange", "[auth][token_manager]")
{
	// A replayed redirect must not produce a second exchange with the same verifier.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response());

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const std::string redirect = fixture.redirect_with_code("the-code");

	REQUIRE(fixture.token_manager.complete_sign_in(redirect).succeeded);
	REQUIRE_FALSE(fixture.token_manager.complete_sign_in(redirect).succeeded);
	REQUIRE(fixture.token_endpoint.received_requests.size() == 1);
}

TEST_CASE("an unreachable token endpoint is a retryable failure", "[auth][token_manager]")
{
	authentication_fixture fixture;

	token_endpoint_response unreachable;

	unreachable.transport_succeeded = false;

	fixture.token_endpoint.scripted_responses.push_back(unreachable);

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed =
		fixture.token_manager.complete_sign_in(fixture.redirect_with_code("the-code"));

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE(completed.retryable);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
}

TEST_CASE("a token endpoint that refuses the exchange is not retried", "[auth][token_manager]")
{
	authentication_fixture fixture;

	token_endpoint_response refused;

	refused.transport_succeeded = true;
	refused.http_status_code = 400;
	refused.error = "invalid_grant";
	refused.error_description = "Invalid authorization code";

	fixture.token_endpoint.scripted_responses.push_back(refused);

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	const operation_outcome completed =
		fixture.token_manager.complete_sign_in(fixture.redirect_with_code("the-code"));

	REQUIRE_FALSE(completed.succeeded);
	REQUIRE_FALSE(completed.retryable);
	REQUIRE(completed.failure_reason.find("invalid_grant") != std::string::npos);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
}

TEST_CASE("an unusable token response is refused rather than half-accepted",
	"[auth][token_manager]")
{
	// Each of these would otherwise produce a session that looks signed in and then fails in a
	// way that is hard to trace back here.
	const auto attempt = [](const token_endpoint_response& response) {
		authentication_fixture fixture;

		fixture.token_endpoint.scripted_responses.push_back(response);

		REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

		const operation_outcome completed =
			fixture.token_manager.complete_sign_in(fixture.redirect_with_code("the-code"));

		REQUIRE_FALSE(completed.succeeded);
		REQUIRE(fixture.token_manager.state() == authentication_state::failed);
		REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	};

	SECTION("no access token")
	{
		token_endpoint_response response = successful_token_response();

		response.access_token.clear();

		attempt(response);
	}

	SECTION("no lifetime")
	{
		token_endpoint_response response = successful_token_response();

		response.expires_in_seconds = 0;

		attempt(response);
	}

	SECTION("a negative lifetime")
	{
		token_endpoint_response response = successful_token_response();

		response.expires_in_seconds = -60;

		attempt(response);
	}

	SECTION("a token type this client does not understand")
	{
		token_endpoint_response response = successful_token_response();

		response.token_type = "MAC";

		attempt(response);
	}

	SECTION("a status that is not 200")
	{
		token_endpoint_response response = successful_token_response();

		response.http_status_code = 302;

		attempt(response);
	}

	SECTION("no refresh token, which would make a long session impossible")
	{
		// Requirement 17.5 cannot be satisfied without one, and discovering that an hour into
		// a session is worse than refusing at sign-in.
		token_endpoint_response response = successful_token_response(3600, example_access_token, nullptr);

		attempt(response);
	}
}

TEST_CASE("a lowercase bearer token type is accepted", "[auth][token_manager]")
{
	// The token type carries no security decision here — the token goes into the Socket.IO
	// handshake, not an Authorization header — so case is not worth failing a sign-in over.
	authentication_fixture fixture;

	token_endpoint_response response = successful_token_response();

	response.token_type = "bearer";

	fixture.token_endpoint.scripted_responses.push_back(response);

	sign_in(fixture.token_manager, fixture.login_page);

	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);
}

TEST_CASE("the refresh lead time is five minutes, capped at half the lifetime",
	"[auth][token_manager]")
{
	// Cognito's configurable range is 5 minutes to 24 hours. Five minutes of lead covers a
	// timer tick, an HTTPS round trip, and one retry; the half-lifetime cap keeps a
	// short-lived token from being due for refresh the moment it is issued.
	REQUIRE(refresh_lead_time(std::chrono::seconds{3600}) == std::chrono::seconds{300});
	REQUIRE(refresh_lead_time(std::chrono::hours{24}) == std::chrono::seconds{300});
	REQUIRE(refresh_lead_time(std::chrono::seconds{600}) == std::chrono::seconds{300});
	REQUIRE(refresh_lead_time(std::chrono::seconds{300}) == std::chrono::seconds{150});
	REQUIRE(refresh_lead_time(std::chrono::seconds{60}) == std::chrono::seconds{30});
	REQUIRE(refresh_lead_time(std::chrono::seconds{1}) == std::chrono::seconds{0});
	REQUIRE(refresh_lead_time(std::chrono::seconds{0}) == std::chrono::seconds{0});
	REQUIRE(refresh_lead_time(std::chrono::seconds{-60}) == std::chrono::seconds{0});

	// The lead never reaches the whole lifetime, at any lifetime — which is what stops a
	// refresh loop.
	for (long long lifetime = 1; lifetime <= 24 * 3600; lifetime += 37)
	{
		const std::chrono::seconds lead = refresh_lead_time(std::chrono::seconds{lifetime});

		REQUIRE(lead.count() >= 0);
		REQUIRE(lead.count() * 2 <= lifetime);
	}
}

TEST_CASE("a refresh becomes due ahead of expiry and not before", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);
	REQUIRE_FALSE(fixture.token_manager.refresh_due());
	REQUIRE(fixture.token_manager.time_until_refresh_due() == std::chrono::seconds{3300});
	REQUIRE_FALSE(fixture.token_manager.refresh_if_due().has_value());

	// One second before the lead opens.
	fixture.clock.advance(std::chrono::seconds{3299});

	REQUIRE_FALSE(fixture.token_manager.refresh_due());

	fixture.clock.advance(std::chrono::seconds{1});

	REQUIRE(fixture.token_manager.refresh_due());

	// Still five minutes of valid token left, which is the point of the lead: the producer is
	// mid-take and the refresh happens behind them.
	REQUIRE(fixture.token_manager.time_until_access_token_expiry() == std::chrono::seconds{300});
	REQUIRE(fixture.token_manager.has_usable_access_token());
}

TEST_CASE("a refresh rotates the access token and keeps the session", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	// Cognito's refresh response reissues the access and ID tokens and omits the refresh token.
	token_endpoint_response refreshed =
		successful_token_response(3600, example_rotated_access_token, nullptr);

	fixture.token_endpoint.scripted_responses.push_back(refreshed);

	sign_in(fixture.token_manager, fixture.login_page);

	fixture.clock.advance(std::chrono::seconds{3300});

	const std::optional<operation_outcome> outcome = fixture.token_manager.refresh_if_due();

	REQUIRE(outcome.has_value());
	REQUIRE(outcome->succeeded);
	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);

	const std::optional<secret_string> token = fixture.token_manager.usable_access_token();

	REQUIRE(token.has_value());
	REQUIRE(token->reveal() == std::string{example_rotated_access_token});

	// The refresh request carried the refresh token from the initial exchange, and the manager
	// kept it because the response omitted one.
	REQUIRE(fixture.token_endpoint.received_requests.size() == 2);
	REQUIRE(fixture.token_endpoint.received_requests[1].grant == token_grant::refresh_token);
	REQUIRE(fixture.token_endpoint.received_requests[1].refresh_token.reveal() ==
		std::string{example_refresh_token});
	REQUIRE(fixture.token_endpoint.received_requests[1].authorization_code.empty());
	REQUIRE(fixture.token_endpoint.received_requests[1].code_verifier.empty());

	// The expiry window moved with the new token, so the next refresh is due a full lead time
	// before the new expiry rather than immediately.
	REQUIRE(fixture.token_manager.time_until_access_token_expiry() == std::chrono::seconds{3600});
	REQUIRE(fixture.token_manager.time_until_refresh_due() == std::chrono::seconds{3300});
	REQUIRE_FALSE(fixture.token_manager.refresh_due());

	// And a rotated refresh token replaces the old one when the server does send one.
	fixture.token_endpoint.scripted_responses.push_back(
		successful_token_response(3600, example_access_token, "A.ROTATED.REFRESH.TOKEN"));

	fixture.clock.advance(std::chrono::seconds{3300});

	REQUIRE(fixture.token_manager.refresh_access_token().succeeded);

	fixture.token_endpoint.scripted_responses.push_back(
		successful_token_response(3600, example_access_token, nullptr));

	fixture.clock.advance(std::chrono::seconds{3300});

	REQUIRE(fixture.token_manager.refresh_access_token().succeeded);
	REQUIRE(fixture.token_endpoint.received_requests.back().refresh_token.reveal() ==
		"A.ROTATED.REFRESH.TOKEN");
}

TEST_CASE("a refresh that cannot reach the endpoint keeps the session alive",
	"[auth][token_manager]")
{
	// The lead time exists so that this is survivable: the existing token is still valid, so
	// the next tick tries again and the producer notices nothing.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	token_endpoint_response unreachable;

	unreachable.transport_succeeded = false;

	fixture.token_endpoint.scripted_responses.push_back(unreachable);
	fixture.token_endpoint.scripted_responses.push_back(
		successful_token_response(3600, example_rotated_access_token, nullptr));

	sign_in(fixture.token_manager, fixture.login_page);

	fixture.clock.advance(std::chrono::seconds{3300});

	const operation_outcome failed = fixture.token_manager.refresh_access_token();

	REQUIRE_FALSE(failed.succeeded);
	REQUIRE(failed.retryable);
	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);
	REQUIRE(fixture.token_manager.has_usable_access_token());
	REQUIRE(fixture.token_manager.usable_access_token()->reveal() ==
		std::string{example_access_token});
	REQUIRE(fixture.token_manager.refresh_due());

	// A minute later, on a working network.
	fixture.clock.advance(std::chrono::seconds{60});

	REQUIRE(fixture.token_manager.refresh_access_token().succeeded);
	REQUIRE(fixture.token_manager.usable_access_token()->reveal() ==
		std::string{example_rotated_access_token});
}

TEST_CASE("a refused refresh token ends the session", "[auth][token_manager]")
{
	// A refused refresh token does not get better on a retry, and serving an access token that
	// is minutes from expiry only delays the same prompt. So the session is cleared and the
	// producer signs in again.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	token_endpoint_response refused;

	refused.transport_succeeded = true;
	refused.http_status_code = 400;
	refused.error = "invalid_grant";

	fixture.token_endpoint.scripted_responses.push_back(refused);

	sign_in(fixture.token_manager, fixture.login_page);

	fixture.clock.advance(std::chrono::seconds{3300});

	const operation_outcome outcome = fixture.token_manager.refresh_access_token();

	REQUIRE_FALSE(outcome.succeeded);
	REQUIRE_FALSE(outcome.retryable);
	REQUIRE(outcome.failure_reason.find("invalid_grant") != std::string::npos);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	REQUIRE(fixture.token_manager.describe_for_log().find("<absent>") != std::string::npos);
}

TEST_CASE("refreshing without a session is refused", "[auth][token_manager]")
{
	authentication_fixture fixture;

	REQUIRE_FALSE(fixture.token_manager.refresh_access_token().succeeded);
	REQUIRE_FALSE(fixture.token_manager.refresh_due());
	REQUIRE_FALSE(fixture.token_manager.refresh_if_due().has_value());
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("an expired access token is not offered to the handshake", "[auth][token_manager]")
{
	// Requirement 17.6's precondition: a handshake with an expired token is a rejection that
	// looks like a protocol fault. The Transport Client asks for a refresh instead.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	fixture.clock.advance(std::chrono::seconds{3599});

	REQUIRE(fixture.token_manager.has_usable_access_token());

	fixture.clock.advance(std::chrono::seconds{1});

	REQUIRE_FALSE(fixture.token_manager.has_usable_access_token());
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	REQUIRE(fixture.token_manager.time_until_access_token_expiry() == std::chrono::seconds{0});

	// The session is still `signed_in` with a refresh token, so recovery is a refresh rather
	// than a new sign-in.
	REQUIRE(fixture.token_manager.state() == authentication_state::signed_in);
	REQUIRE(fixture.token_manager.refresh_due());
}

TEST_CASE("sign-in cannot start without a usable configuration", "[auth][token_manager]")
{
	RecordingLoginPagePresenter login_page;
	ScriptedTokenEndpoint token_endpoint;
	DeterministicRandomSource random_source;

	TokenManager token_manager{{}, login_page, token_endpoint, random_source};

	const operation_outcome started = token_manager.begin_sign_in();

	REQUIRE_FALSE(started.succeeded);
	REQUIRE(started.failure_reason.find("client_id") != std::string::npos);
	REQUIRE(started.failure_reason.find("redirect_uri") != std::string::npos);
	REQUIRE(login_page.present_call_count == 0);
	REQUIRE(token_manager.state() == authentication_state::failed);
}

TEST_CASE("sign-in cannot start without an entropy source", "[auth][token_manager]")
{
	// Retryable, because the alternative to retrying is not a weaker source — it is no
	// sign-in. Nothing is presented, so the producer is not shown a login page whose PKCE
	// material would be predictable.
	authentication_fixture fixture;

	fixture.random_source.fail_next_fill = true;

	const operation_outcome started = fixture.token_manager.begin_sign_in();

	REQUIRE_FALSE(started.succeeded);
	REQUIRE(started.retryable);
	REQUIRE(started.failure_reason.find("random") != std::string::npos);
	REQUIRE(fixture.login_page.present_call_count == 0);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);
}

TEST_CASE("sign-in cannot start when no browser can be shown", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.login_page.refuse_to_present = true;

	const operation_outcome started = fixture.token_manager.begin_sign_in();

	REQUIRE_FALSE(started.succeeded);
	REQUIRE(started.retryable);
	REQUIRE(fixture.token_manager.state() == authentication_state::failed);

	// And the flow did not half-start: there is no pending verifier waiting for a redirect.
	REQUIRE_FALSE(fixture.token_manager.complete_sign_in(
		"https://extension.example.invalid/signed-in?code=abc&state=abc").succeeded);
	REQUIRE(fixture.token_endpoint.received_requests.empty());
}

TEST_CASE("signing out clears the session", "[auth][token_manager]")
{
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	REQUIRE(fixture.token_manager.has_usable_access_token());

	fixture.token_manager.sign_out();

	REQUIRE(fixture.token_manager.state() == authentication_state::signed_out);
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	REQUIRE_FALSE(fixture.token_manager.usable_id_token().has_value());
	REQUIRE(fixture.token_manager.time_until_access_token_expiry() == std::chrono::seconds{0});
	REQUIRE(fixture.token_manager.time_until_refresh_due() == std::chrono::seconds{0});
	REQUIRE(fixture.token_manager.access_token_lifetime() == std::chrono::seconds{0});
	REQUIRE_FALSE(fixture.token_manager.refresh_due());

	// Nothing is left for a later refresh to reach for.
	REQUIRE_FALSE(fixture.token_manager.refresh_access_token().succeeded);
	REQUIRE(fixture.token_endpoint.received_requests.size() == 1);
}

TEST_CASE("signing out mid-sign-in takes the login page down", "[auth][token_manager]")
{
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	fixture.token_manager.sign_out();

	REQUIRE(fixture.login_page.dismiss_call_count == 1);
	REQUIRE(fixture.token_manager.authorization_url().empty());
	REQUIRE(fixture.token_manager.state() == authentication_state::signed_out);
}

// ---------------------------------------------------------------------------
// The requirements that are about absence
// ---------------------------------------------------------------------------

TEST_CASE("a freshly constructed manager holds nothing at all", "[auth][token_manager]")
{
	// Requirement 17.7 — on REAPER launch, a fresh sign-in; no refresh token carries across
	// launches. This holds structurally rather than by a code path choosing it: the only
	// constructor takes a configuration and three seams, and there is no method anywhere that
	// accepts a token, a refresh token, or a serialised session. A token from a previous launch
	// has nowhere to enter, so there is nothing to test beyond the starting state — which is
	// the point.
	authentication_fixture fixture;

	REQUIRE(fixture.token_manager.state() == authentication_state::signed_out);
	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	REQUIRE_FALSE(fixture.token_manager.usable_id_token().has_value());
	REQUIRE_FALSE(fixture.token_manager.has_usable_access_token());
	REQUIRE_FALSE(fixture.token_manager.refresh_due());
	REQUIRE(fixture.token_manager.authorization_url().empty());
	REQUIRE(fixture.token_manager.access_token_lifetime() == std::chrono::seconds{0});

	const std::string description = fixture.token_manager.describe_for_log();

	REQUIRE(description.find("state=signed_out") != std::string::npos);
	REQUIRE(description.find("access_token=<absent>") != std::string::npos);
	REQUIRE(description.find("refresh_token=<absent>") != std::string::npos);
}

TEST_CASE("beginning a sign-in clears whatever the previous one held", "[auth][token_manager]")
{
	// Requirement 17.4, and the practical version of 17.7: a re-sign-in must not leave a stale
	// token reachable, and must not reuse the previous verifier.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	REQUIRE(fixture.token_manager.has_usable_access_token());

	const std::string first_url = fixture.token_manager.authorization_url();

	REQUIRE(fixture.token_manager.begin_sign_in().succeeded);

	REQUIRE_FALSE(fixture.token_manager.usable_access_token().has_value());
	REQUIRE_FALSE(fixture.token_manager.usable_id_token().has_value());
	REQUIRE(fixture.token_manager.access_token_lifetime() == std::chrono::seconds{0});
	REQUIRE(fixture.token_manager.describe_for_log().find("access_token=<absent>") !=
		std::string::npos);

	// Fresh PKCE material and a fresh nonce, so a code captured from the first attempt is not
	// exchangeable in the second.
	REQUIRE(fixture.token_manager.authorization_url() != first_url);
	REQUIRE(fixture.login_page.presented_urls.size() == 2);
	REQUIRE(fixture.login_page.presented_urls[0] != fixture.login_page.presented_urls[1]);
}

TEST_CASE("no token material reaches a log line or a failure reason", "[auth][token_manager]")
{
	// Requirement 26.4. The manager's only log-facing output is describe_for_log, and its only
	// UI-facing output is a failure reason; neither may carry characters of a token. The
	// example tokens are distinctive strings, so a leak is unmistakable.
	authentication_fixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(successful_token_response(3600));

	sign_in(fixture.token_manager, fixture.login_page);

	const std::string description = fixture.token_manager.describe_for_log();

	REQUIRE(description.find(example_access_token) == std::string::npos);
	REQUIRE(description.find(example_id_token) == std::string::npos);
	REQUIRE(description.find(example_refresh_token) == std::string::npos);
	REQUIRE(description.find("ACCESS.TOKEN") == std::string::npos);
	REQUIRE(description.find("<redacted") != std::string::npos);

	// The authorization URL is published to a browser and is the other thing that gets logged.
	// It carries the challenge, which is public, and must carry no token.
	REQUIRE(fixture.login_page.presented_urls.back().find(example_access_token) ==
		std::string::npos);

	// And a failure reason, which reaches both the log and the UI.
	token_endpoint_response refused;

	refused.transport_succeeded = true;
	refused.http_status_code = 400;
	refused.error = "invalid_grant";
	refused.error_description = "Refresh token has been revoked";

	fixture.token_endpoint.scripted_responses.push_back(refused);

	fixture.clock.advance(std::chrono::seconds{3300});

	const operation_outcome outcome = fixture.token_manager.refresh_access_token();

	REQUIRE_FALSE(outcome.succeeded);
	REQUIRE(outcome.failure_reason.find(example_refresh_token) == std::string::npos);
	REQUIRE(outcome.failure_reason.find("REFRESH.TOKEN") == std::string::npos);
	REQUIRE(outcome.failure_reason.find("invalid_grant") != std::string::npos);
}
