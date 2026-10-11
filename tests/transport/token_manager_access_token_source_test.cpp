// Where the Transport Client and the Token Manager meet.
//
// Two things are worth checking, and the second is the reason this file exists rather
// than the adapter being taken on trust.
//
// The token accessor has to be a pass-through, not a second opinion. A duplicate expiry
// check on this side would eventually disagree with the Token Manager's, and the
// disagreement would present as a server refusing a token the extension believes is
// fine.
//
// And `retryable` has to survive the conversion. It is the field that decides between
// prompting a sign-in and waiting for the next tick, so flattening it means either
// asking a producer to sign in again because a studio's Wi-Fi dropped, or looping
// forever on a session the server has revoked. A three-field struct conversion is
// exactly the kind of code that is written once, read as obviously right, and drops a
// field.
//
// The Token Manager's own doubles are reused rather than restated — they already cover a
// browser, an authorization server, and an entropy source, which is everything a
// signed-in manager needs.

#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <auth/token_manager.h>
#include <transport/token_manager_access_token_source.h>

#include "../auth/authentication_test_doubles.h"

using sesh_ai::auth::TokenManager;
using sesh_ai::auth::operation_outcome;
using sesh_ai::auth::secret_string;
using sesh_ai::auth::token_endpoint_response;
using sesh_ai::tests::DeterministicRandomSource;
using sesh_ai::tests::RecordingLoginPagePresenter;
using sesh_ai::tests::ScriptedTokenEndpoint;
using sesh_ai::tests::example_access_token;
using sesh_ai::tests::example_rotated_access_token;
using sesh_ai::tests::sign_in;
using sesh_ai::tests::successful_token_response;
using sesh_ai::tests::test_configuration;
using sesh_ai::transport::TokenManagerAccessTokenSource;
using sesh_ai::transport::as_token_refresh_outcome;
using sesh_ai::transport::token_refresh_outcome;

namespace {

	// A signed-in manager and the seams it holds references to, kept alive together.
	struct SignedInFixture {
		SignedInFixture()
			: token_manager{test_configuration(), login_page, token_endpoint, random_source}
		{
			token_endpoint.scripted_responses.push_back(successful_token_response());

			sign_in(token_manager, login_page);
		}

		RecordingLoginPagePresenter login_page;
		ScriptedTokenEndpoint token_endpoint;
		DeterministicRandomSource random_source;
		TokenManager token_manager;
	};

}

TEST_CASE("the access token accessor is a pass-through, not a second opinion", "[transport][auth]")
{
	SignedInFixture fixture;

	TokenManagerAccessTokenSource source{fixture.token_manager};

	const std::optional<secret_string> from_the_source = source.current_access_token();
	const std::optional<secret_string> from_the_manager =
		fixture.token_manager.usable_access_token();

	REQUIRE(from_the_source.has_value());
	REQUIRE(from_the_manager.has_value());
	CHECK(from_the_source->reveal() == from_the_manager->reveal());
	CHECK(from_the_source->reveal() == example_access_token);

	// A manager holding nothing offers nothing, rather than the adapter inventing an
	// empty token for a handshake to present — which the server would refuse with
	// `missing_token`.
	fixture.token_manager.sign_out();

	CHECK_FALSE(source.current_access_token().has_value());
	CHECK_FALSE(fixture.token_manager.usable_access_token().has_value());
}

TEST_CASE("a successful refresh replaces the token the next handshake presents", "[transport][auth]")
{
	SignedInFixture fixture;

	fixture.token_endpoint.scripted_responses.push_back(
		successful_token_response(3600, example_rotated_access_token, nullptr)
	);

	TokenManagerAccessTokenSource source{fixture.token_manager};

	const std::optional<secret_string> before = source.current_access_token();

	REQUIRE(before.has_value());

	const token_refresh_outcome refreshed = source.refresh_access_token();

	CHECK(refreshed.succeeded);
	CHECK(refreshed.failure_reason.empty());

	const std::optional<secret_string> after = source.current_access_token();

	REQUIRE(after.has_value());
	CHECK(after->reveal() == example_rotated_access_token);
	CHECK(after->reveal() != before->reveal());
}

TEST_CASE("the refresh outcome keeps the field that decides the response", "[transport][auth]")
{
	SECTION("a refused refresh token is terminal, so the producer is asked to sign in")
	{
		SignedInFixture fixture;

		token_endpoint_response refused;

		refused.transport_succeeded = true;
		refused.http_status_code = 400;
		refused.error = "invalid_grant";

		fixture.token_endpoint.scripted_responses.push_back(refused);

		TokenManagerAccessTokenSource source{fixture.token_manager};

		const token_refresh_outcome outcome = source.refresh_access_token();

		CHECK_FALSE(outcome.succeeded);
		CHECK_FALSE(outcome.retryable);
		CHECK_FALSE(outcome.failure_reason.empty());
	}

	SECTION("a token endpoint that could not be reached is worth another attempt")
	{
		SignedInFixture fixture;

		token_endpoint_response unreachable;

		unreachable.transport_succeeded = false;

		fixture.token_endpoint.scripted_responses.push_back(unreachable);

		TokenManagerAccessTokenSource source{fixture.token_manager};

		const token_refresh_outcome outcome = source.refresh_access_token();

		CHECK_FALSE(outcome.succeeded);
		CHECK(outcome.retryable);
	}
}

TEST_CASE("the conversion carries all three fields", "[transport][auth]")
{
	// Stated directly as well as through the manager, because the conversion is where a
	// field would go missing and the manager does not produce every combination.
	const token_refresh_outcome succeeded = as_token_refresh_outcome(operation_outcome::success());

	CHECK(succeeded.succeeded);
	CHECK_FALSE(succeeded.retryable);
	CHECK(succeeded.failure_reason.empty());

	const token_refresh_outcome retryable =
		as_token_refresh_outcome(operation_outcome::failure("the endpoint was unreachable", true));

	CHECK_FALSE(retryable.succeeded);
	CHECK(retryable.retryable);
	CHECK(retryable.failure_reason == "the endpoint was unreachable");

	const token_refresh_outcome terminal =
		as_token_refresh_outcome(operation_outcome::failure("the endpoint refused", false));

	CHECK_FALSE(terminal.succeeded);
	CHECK_FALSE(terminal.retryable);
	CHECK(terminal.failure_reason == "the endpoint refused");
}

TEST_CASE("no failure reason the adapter passes on contains token material", "[transport][auth]")
{
	SignedInFixture fixture;

	token_endpoint_response refused;

	refused.transport_succeeded = true;
	refused.http_status_code = 400;
	refused.error = "invalid_grant";
	refused.error_description = "Refresh token has been revoked";

	fixture.token_endpoint.scripted_responses.push_back(refused);

	TokenManagerAccessTokenSource source{fixture.token_manager};

	const token_refresh_outcome outcome = source.refresh_access_token();

	// The reason reaches the UI and the log, so it must carry the authorization
	// server's public error code and nothing from the token set.
	CHECK(outcome.failure_reason.find("invalid_grant") != std::string::npos);
	CHECK(outcome.failure_reason.find(example_access_token) == std::string::npos);
	CHECK(outcome.failure_reason.find("REFRESH.TOKEN.MATERIAL") == std::string::npos);
}
