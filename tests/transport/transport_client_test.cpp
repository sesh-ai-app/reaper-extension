// The Transport Client.
//
// Six things are worth testing here, and they are worth testing in roughly this order
// of consequence to a producer mid-session.
//
// **A duplicate rejection must never retry.** The producer has REAPER open twice and
// the server refuses the second one. Retrying is a connection attempt every few
// seconds for as long as both windows are open, and the producer is never told why.
// This is the one behaviour with no escape clause, so it is asserted over the whole
// decision table and then again against every automatic entry point on the client —
// including the socket's own close callback, which is the specific way terminality
// would quietly stop holding.
//
// **A stale token must never be sent.** Requirement 3.2 says a fresh token in each new
// handshake, and the failure mode is silent: a reconnection that presents the token
// that was just refused is refused again, and the producer sees a loop rather than a
// recovery. So the token source hands out a different token on every call and the
// suite checks that the socket saw every one of them exactly once.
//
// **Retry once means once.** Requirement 3.6 branches three ways on one condition and
// the branch that must not exist is a second retry. It is asserted over the product of
// both attempt kinds and all six rejections rather than over a scenario, because a
// scenario proves one ordering and the bound has to hold for all of them.
//
// **A snapshot on every connection.** Requirement 3.3. Stated as an equality between
// two counters over a long sequence of connects and drops, which is the form that
// catches "the reconnect path sends one but the first connect does not".
//
// **The backoff schedule.** Monotonic, capped, and it does not wrap for a client that
// has been failing for a week.
//
// **The endpoint cannot be plaintext.** Requirement 26.3, as far as this side of the
// seam can carry it — the other half is the `#error` in transport_client.cpp.
//
// No network here, and no Socket.IO client. All four seams are substituted, which is
// what transport_client.h is shaped for.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <auth/secret_string.h>
#include <transport/transport_client.h>

using sesh_ai::auth::secret_string;
using sesh_ai::transport::AccessTokenSource;
using sesh_ai::transport::ConnectionStatePresenter;
using sesh_ai::transport::HandshakeAuth;
using sesh_ai::transport::ProjectContextSnapshotPublisher;
using sesh_ai::transport::SecureEndpoint;
using sesh_ai::transport::SocketConnection;
using sesh_ai::transport::TransportClient;
using sesh_ai::transport::attempt_after;
using sesh_ai::transport::classify_handshake_rejection;
using sesh_ai::transport::connection_state;
using sesh_ai::transport::connection_status;
using sesh_ai::transport::decide_after_handshake_rejection;
using sesh_ai::transport::extension_client_type;
using sesh_ai::transport::handshake_attempt;
using sesh_ai::transport::handshake_auth_client_type_field;
using sesh_ai::transport::handshake_auth_protocol_version_field;
using sesh_ai::transport::handshake_auth_token_field;
using sesh_ai::transport::handshake_rejection;
using sesh_ai::transport::initial_reconnection_delay;
using sesh_ai::transport::maximum_reconnection_delay;
using sesh_ai::transport::producer_action;
using sesh_ai::transport::reconnection_decision;
using sesh_ai::transport::reconnection_delay_for_attempt;
using sesh_ai::transport::secure_endpoint_for_host;
using sesh_ai::transport::socket_io_path;
using sesh_ai::transport::token_refresh_outcome;

namespace {

	// Every rejection the client distinguishes, so the decision table can be asserted
	// over the whole product rather than over the cases somebody thought of.
	constexpr handshake_rejection all_handshake_rejections[] = {
		handshake_rejection::invalid_or_expired_token,
		handshake_rejection::duplicate_client_type,
		handshake_rejection::unsupported_client_type,
		handshake_rejection::authentication_service_unavailable,
		handshake_rejection::network_or_timeout,
		handshake_rejection::unrecognised
	};

	constexpr handshake_attempt all_handshake_attempts[] = {
		handshake_attempt::first_or_backoff,
		handshake_attempt::single_retry_after_token_refresh
	};

	// Records what every handshake was given. The token is copied out as plain text
	// here — which is the one place in this suite that is acceptable, because the
	// assertion is about *which* token was sent and there is nothing else to compare.
	class RecordingSocketConnection final : public SocketConnection {
	public:
		bool open(const SecureEndpoint& endpoint, const HandshakeAuth& handshake_auth) override
		{
			opened_urls.push_back(endpoint.url());
			presented_tokens.push_back(handshake_auth.access_token().reveal());
			presented_client_types.emplace_back(HandshakeAuth::client_type());
			presented_protocol_versions.emplace_back(HandshakeAuth::protocol_version());

			return !refuse_to_open;
		}

		void close() override
		{
			++close_count;

			// The real library's close produces a close callback. Reproducing it is the
			// point: a client whose hold is undone by its own close is a client that
			// retries a duplicate rejection, and nothing else in this suite would
			// notice.
			if (report_drop_on_close && transport_client != nullptr) {
				transport_client->on_disconnected();
			}
		}

		TransportClient* transport_client = nullptr;
		bool refuse_to_open = false;
		bool report_drop_on_close = false;
		std::size_t close_count = 0;
		std::vector<std::string> opened_urls;
		std::vector<std::string> presented_tokens;
		std::vector<std::string> presented_client_types;
		std::vector<std::string> presented_protocol_versions;
	};

	// Hands out a different token on every call, so "a fresh token per handshake" is
	// checkable by comparing what the socket saw against what was issued.
	class IssuingAccessTokenSource final : public AccessTokenSource {
	public:
		std::optional<secret_string> current_access_token() override
		{
			++current_access_token_call_count;

			if (!has_token) {
				return std::nullopt;
			}

			std::string issued = "access-token-" + std::to_string(++issued_token_count);

			issued_tokens.push_back(issued);

			return secret_string{issued};
		}

		token_refresh_outcome refresh_access_token() override
		{
			++refresh_call_count;

			if (!refresh_succeeds) {
				token_refresh_outcome outcome;

				outcome.succeeded = false;
				outcome.retryable = refresh_failure_is_retryable;
				outcome.failure_reason = "the token endpoint said no";

				return outcome;
			}

			token_refresh_outcome outcome;

			outcome.succeeded = true;

			return outcome;
		}

		bool has_token = true;
		bool refresh_succeeds = true;
		bool refresh_failure_is_retryable = false;
		std::size_t current_access_token_call_count = 0;
		std::size_t refresh_call_count = 0;
		std::size_t issued_token_count = 0;
		std::vector<std::string> issued_tokens;
	};

	class CountingSnapshotPublisher final : public ProjectContextSnapshotPublisher {
	public:
		bool publish_project_context_snapshot() override
		{
			++publish_count;

			return !refuse_to_publish;
		}

		bool refuse_to_publish = false;
		std::size_t publish_count = 0;
	};

	class RecordingStatePresenter final : public ConnectionStatePresenter {
	public:
		void present_connection_status(const connection_status& status) override
		{
			published.push_back(status);
		}

		const connection_status& latest() const { return published.back(); }

		std::vector<connection_status> published;
	};

	// A clock the suite moves by hand, so the backoff is exercised without sleeping.
	class ManualClock {
	public:
		std::chrono::steady_clock::time_point now() const { return now_; }

		void advance(std::chrono::milliseconds elapsed) { now_ += elapsed; }

	private:
		std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::time_point{}
			+ std::chrono::hours{1}};
	};

	// Everything a case needs, wired together.
	struct ClientHarness {
		ClientHarness()
			: endpoint{*secure_endpoint_for_host("sesh.example")},
			client{
				endpoint,
				socket_connection,
				access_token_source,
				snapshot_publisher,
				state_presenter,
				[this] { return clock.now(); }
			}
		{
			socket_connection.transport_client = &client;
		}

		// Connects and reports the server accepting the handshake.
		void connect_successfully()
		{
			REQUIRE(client.connect());
			client.on_connected();
		}

		// Waits out whatever delay is scheduled and takes the reconnection.
		bool advance_past_the_backoff_and_reconnect()
		{
			const std::optional<std::chrono::milliseconds> remaining =
				client.time_until_reconnection();

			REQUIRE(remaining.has_value());

			clock.advance(*remaining);

			return client.reconnect_if_due();
		}

		ManualClock clock;
		RecordingSocketConnection socket_connection;
		IssuingAccessTokenSource access_token_source;
		CountingSnapshotPublisher snapshot_publisher;
		RecordingStatePresenter state_presenter;
		SecureEndpoint endpoint;
		TransportClient client;
	};

}

// ---------------------------------------------------------------------------
// Requirement 26.3 — TLS, made unconstructable rather than checked
// ---------------------------------------------------------------------------

TEST_CASE("the only endpoint that can be built is a TLS one", "[transport][transport_client]")
{
	const std::optional<SecureEndpoint> endpoint = secure_endpoint_for_host("sesh.example");

	REQUIRE(endpoint.has_value());

	// The scheme is written by the factory from a literal. There is no argument that
	// could have named another one.
	CHECK(endpoint->url().rfind("https://", 0) == 0);
	CHECK(endpoint->url().find("http://") == std::string::npos);
	CHECK(endpoint->url().find("ws://") == std::string::npos);
	CHECK(endpoint->port() == 443);
	CHECK(endpoint->host() == "sesh.example");
}

TEST_CASE("the endpoint carries the Socket.IO path with its trailing slash", "[transport][transport_client]")
{
	const std::optional<SecureEndpoint> endpoint = secure_endpoint_for_host("sesh.example");

	REQUIRE(endpoint.has_value());

	// Engine.io matches its configured path as a prefix ending in a slash, so a URL
	// without one is answered with a 404 rather than a handshake.
	CHECK(endpoint->url() == "https://sesh.example:443/ws/");
	CHECK(socket_io_path.back() == '/');
}

TEST_CASE("a host that smuggles in a scheme, a path, or a port is refused", "[transport][transport_client]")
{
	// Each of these is a way to make the resulting URL mean something other than what
	// the caller wrote, and the plaintext schemes are the reason the check exists at
	// all.
	CHECK_FALSE(secure_endpoint_for_host("http://sesh.example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("https://sesh.example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("ws://sesh.example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh.example/ws").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh.example:8080").has_value());
	CHECK_FALSE(secure_endpoint_for_host("producer@sesh.example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh.example\n").has_value());
	CHECK_FALSE(secure_endpoint_for_host("").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh.example", 0).has_value());
	CHECK_FALSE(secure_endpoint_for_host(".sesh.example").has_value());
	CHECK_FALSE(secure_endpoint_for_host("sesh.example.").has_value());
}

// ---------------------------------------------------------------------------
// Requirement 3.1 — the handshake auth
// ---------------------------------------------------------------------------

TEST_CASE("the handshake carries the token, the client type, and the protocol version", "[transport][transport_client]")
{
	ClientHarness harness;

	REQUIRE(harness.client.connect());

	REQUIRE(harness.socket_connection.presented_client_types.size() == 1);
	CHECK(harness.socket_connection.presented_client_types.front() == "extension");
	CHECK(harness.socket_connection.presented_tokens.front() == "access-token-1");

	// Twelve lowercase hexadecimal characters, derived by the monorepo's bundler from
	// the digests of the vendored schema files and baked in at configure time.
	const std::string& protocol_version =
		harness.socket_connection.presented_protocol_versions.front();

	CHECK(protocol_version.size() == 12);
	CHECK(std::all_of(protocol_version.begin(), protocol_version.end(), [](char character) {
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	}));

	// The field names are the server's, read off `socket.handshake.auth`.
	CHECK(handshake_auth_token_field == "token");
	CHECK(handshake_auth_client_type_field == "clientType");
	CHECK(handshake_auth_protocol_version_field == "protocolVersion");
	CHECK(extension_client_type == "extension");
}

TEST_CASE("nothing a handshake can say about itself in a log contains the token", "[transport][transport_client]")
{
	const HandshakeAuth handshake_auth{secret_string{"a-real-looking-access-token"}};

	const std::string description = handshake_auth.describe_for_log();

	CHECK(description.find("a-real-looking-access-token") == std::string::npos);
	CHECK(description.find("clientType=extension") != std::string::npos);
	CHECK(description.find("<redacted") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Requirement 3.2 — a fresh token in every handshake
// ---------------------------------------------------------------------------

TEST_CASE("every handshake presents a token that has never been presented before", "[transport][transport_client]")
{
	ClientHarness harness;

	// A connection, a drop, a reconnection, another drop, another reconnection — and
	// a token rejection in the middle, so the refresh path is included rather than
	// only the backoff path.
	harness.connect_successfully();

	harness.client.on_disconnected();
	REQUIRE(harness.advance_past_the_backoff_and_reconnect());
	harness.client.on_connected();

	harness.client.on_handshake_rejected("expired_token");
	harness.client.on_connected();

	harness.client.on_disconnected();
	REQUIRE(harness.advance_past_the_backoff_and_reconnect());
	harness.client.on_connected();

	const std::vector<std::string>& presented = harness.socket_connection.presented_tokens;

	REQUIRE(presented.size() == harness.client.handshake_count());
	REQUIRE(presented.size() >= 4);

	// Every handshake asked the token source for a token, and no token was presented
	// twice. A cached token would show up as a repeat.
	std::vector<std::string> sorted = presented;

	std::sort(sorted.begin(), sorted.end());

	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
	CHECK(presented == harness.access_token_source.issued_tokens);
}

TEST_CASE("a handshake with no usable token asks the producer to sign in rather than backing off", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.access_token_source.has_token = false;

	CHECK_FALSE(harness.client.connect());
	CHECK(harness.client.is_holding_for_sign_in());
	CHECK(harness.client.state() == connection_state::disconnected);

	// Nothing was opened, and nothing is scheduled — a handshake with no token is one
	// the server refuses by design, so retrying it would spend the session on an
	// attempt that cannot succeed.
	CHECK(harness.socket_connection.presented_tokens.empty());
	CHECK_FALSE(harness.client.time_until_reconnection().has_value());
}

// ---------------------------------------------------------------------------
// Requirement 3.3 — a snapshot on every connection
// ---------------------------------------------------------------------------

TEST_CASE("a project context snapshot is requested once per connection, first one included", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	CHECK(harness.client.connection_count() == 1);
	CHECK(harness.client.project_context_snapshot_request_count() == 1);

	for (int reconnection = 0; reconnection < 5; ++reconnection) {
		harness.client.on_disconnected();
		REQUIRE(harness.advance_past_the_backoff_and_reconnect());
		harness.client.on_connected();
	}

	CHECK(harness.client.connection_count() == 6);
	CHECK(harness.snapshot_publisher.publish_count == 6);

	// The equality is the property. A path that connects without requesting one, or
	// requests two for one connection, breaks it whatever the absolute numbers are.
	CHECK(harness.client.project_context_snapshot_request_count()
		== harness.client.connection_count());
}

TEST_CASE("a publisher that refuses is reported rather than retried", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.snapshot_publisher.refuse_to_publish = true;

	harness.connect_successfully();

	// The request still happened, and the outcome is visible for the log line.
	CHECK(harness.client.project_context_snapshot_request_count() == 1);
	CHECK_FALSE(harness.client.last_project_context_snapshot_published());

	// Requirement 3.3 specifies the send, not a retry, and the connection is not
	// treated as failed for it.
	CHECK(harness.client.state() == connection_state::connected);
}

// ---------------------------------------------------------------------------
// Requirement 3.4 — the connection state reaches the UI
// ---------------------------------------------------------------------------

TEST_CASE("every state change is published, and the three states are reachable", "[transport][transport_client]")
{
	ClientHarness harness;

	REQUIRE(harness.client.connect());
	CHECK(harness.state_presenter.latest().state == connection_state::reconnecting);

	harness.client.on_connected();
	CHECK(harness.state_presenter.latest().state == connection_state::connected);
	CHECK(harness.state_presenter.latest().required_producer_action == producer_action::none);

	harness.client.on_disconnected();
	CHECK(harness.state_presenter.latest().state == connection_state::reconnecting);
	CHECK(harness.state_presenter.latest().reconnection_delay == initial_reconnection_delay);

	harness.client.shut_down();
	CHECK(harness.state_presenter.latest().state == connection_state::disconnected);

	// Nothing changed the status without publishing it: there is a publication for
	// each of the four transitions above.
	CHECK(harness.state_presenter.published.size() == 4);
}

TEST_CASE("the published status carries a producer-facing notice and never a token", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();
	harness.client.on_handshake_rejected("duplicate_client_type");

	const connection_status& status = harness.state_presenter.latest();

	CHECK(status.required_producer_action == producer_action::close_the_other_reaper_instance);

	// The notice names the cause a producer can act on rather than reporting a code.
	CHECK(status.notice.find("REAPER") != std::string::npos);
	CHECK(status.notice.find("duplicate_client_type") == std::string::npos);

	for (const connection_status& published : harness.state_presenter.published) {
		CHECK(published.notice.find("access-token") == std::string::npos);
	}

	CHECK(harness.client.describe_for_log().find("access-token") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Requirement 3.5 — duplicate_client_type is terminal
// ---------------------------------------------------------------------------

TEST_CASE("a duplicate rejection is terminal for every attempt kind and never retries", "[transport][transport_client]")
{
	// The decision table first: no attempt kind produces anything but the stop.
	for (const handshake_attempt attempt : all_handshake_attempts) {
		CHECK(decide_after_handshake_rejection(attempt, handshake_rejection::duplicate_client_type)
			== reconnection_decision::stop_and_report_session_conflict);
	}
}

TEST_CASE("no automatic path reconnects after a duplicate rejection", "[transport][transport_client]")
{
	ClientHarness harness;

	// The socket reports a drop when it is closed, which is what the real library does
	// and is the specific way terminality would stop holding: the client closes the
	// socket on entering the hold, and the resulting callback must not schedule
	// anything.
	harness.socket_connection.report_drop_on_close = true;

	harness.connect_successfully();

	const std::size_t handshakes_before = harness.client.handshake_count();

	harness.client.on_handshake_rejected("duplicate_client_type");

	CHECK(harness.client.is_holding_for_session_conflict());
	CHECK(harness.client.state() == connection_state::disconnected);
	CHECK_FALSE(harness.client.time_until_reconnection().has_value());

	// Every automatic entry point, and a long wait, and none of them tries again.
	harness.clock.advance(std::chrono::hours{1});

	CHECK_FALSE(harness.client.reconnect_if_due());
	CHECK_FALSE(harness.client.connect());

	harness.client.on_disconnected();
	harness.client.on_handshake_rejected("expired_token");
	harness.client.on_handshake_rejected("");

	CHECK_FALSE(harness.client.reconnect_if_due());

	CHECK(harness.client.handshake_count() == handshakes_before);
	CHECK(harness.access_token_source.refresh_call_count == 0);
	CHECK(harness.client.is_holding_for_session_conflict());
}

TEST_CASE("only a producer-initiated reconnection clears a session conflict", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();
	harness.client.on_handshake_rejected("duplicate_client_type");

	REQUIRE(harness.client.is_holding_for_session_conflict());

	// The producer has closed the other REAPER instance. This is the single deliberate
	// escape, and it is named for who performs it.
	CHECK(harness.client.reconnect_at_producer_request());
	CHECK_FALSE(harness.client.is_holding_for_session_conflict());

	harness.client.on_connected();

	CHECK(harness.client.state() == connection_state::connected);
}

// ---------------------------------------------------------------------------
// Requirement 3.6 — refresh, retry once, and the three outcomes
// ---------------------------------------------------------------------------

TEST_CASE("the decision table is total, and the retry state is reachable only from an ordinary attempt", "[transport][transport_client]")
{
	for (const handshake_attempt attempt : all_handshake_attempts) {
		for (const handshake_rejection rejection : all_handshake_rejections) {
			const reconnection_decision decision =
				decide_after_handshake_rejection(attempt, rejection);

			// The retry is granted to an ordinary attempt and to nothing else. This is
			// the structural bound: since `attempt_after` produces the retry state only
			// for this one decision, and this one decision is unreachable from the retry
			// state, two consecutive retries cannot occur in any ordering of events.
			if (decision == reconnection_decision::refresh_token_and_retry_once) {
				CHECK(attempt == handshake_attempt::first_or_backoff);
				CHECK(rejection == handshake_rejection::invalid_or_expired_token);
			}

			// And the retry state is produced by that decision alone, so there is no
			// second route into it.
			if (attempt_after(decision) == handshake_attempt::single_retry_after_token_refresh) {
				CHECK(decision == reconnection_decision::refresh_token_and_retry_once);
			}
		}
	}

	// The three clauses of requirement 3.6, spelled out.
	CHECK(decide_after_handshake_rejection(
		handshake_attempt::first_or_backoff,
		handshake_rejection::invalid_or_expired_token
	) == reconnection_decision::refresh_token_and_retry_once);

	CHECK(decide_after_handshake_rejection(
		handshake_attempt::single_retry_after_token_refresh,
		handshake_rejection::invalid_or_expired_token
	) == reconnection_decision::prompt_sign_in);

	CHECK(decide_after_handshake_rejection(
		handshake_attempt::single_retry_after_token_refresh,
		handshake_rejection::network_or_timeout
	) == reconnection_decision::back_off_and_reconnect);
}

TEST_CASE("a token rejection refreshes and retries once, immediately", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	const std::size_t handshakes_before = harness.client.handshake_count();

	harness.client.on_handshake_rejected("expired_token");

	// Refreshed once, and the retry happened without waiting — the token was the
	// problem and it has been replaced.
	CHECK(harness.access_token_source.refresh_call_count == 1);
	CHECK(harness.client.handshake_count() == handshakes_before + 1);
	CHECK(harness.client.next_handshake_attempt()
		== handshake_attempt::single_retry_after_token_refresh);
	CHECK(harness.client.state() == connection_state::reconnecting);
	CHECK_FALSE(harness.client.time_until_reconnection().has_value());
}

TEST_CASE("an authentication rejection on the retry prompts sign-in rather than retrying again", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	harness.client.on_handshake_rejected("expired_token");

	const std::size_t handshakes_after_the_retry = harness.client.handshake_count();

	// The retry was refused on the token too.
	harness.client.on_handshake_rejected("invalid_token");

	CHECK(harness.client.is_holding_for_sign_in());
	CHECK(harness.client.state() == connection_state::disconnected);
	CHECK(harness.state_presenter.latest().required_producer_action
		== producer_action::sign_in_again);

	// No second refresh and no third handshake.
	CHECK(harness.access_token_source.refresh_call_count == 1);
	CHECK(harness.client.handshake_count() == handshakes_after_the_retry);

	harness.clock.advance(std::chrono::hours{1});

	CHECK_FALSE(harness.client.reconnect_if_due());
	CHECK(harness.client.handshake_count() == handshakes_after_the_retry);
}

TEST_CASE("a network failure on the retry falls back to normal backoff", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	harness.client.on_handshake_rejected("expired_token");

	REQUIRE(harness.client.next_handshake_attempt()
		== handshake_attempt::single_retry_after_token_refresh);

	// The retry reached nobody. Requirement 3.6's third clause: normal reconnection
	// with backoff, not a sign-in prompt.
	harness.client.on_handshake_rejected("");

	CHECK_FALSE(harness.client.is_holding_for_sign_in());
	CHECK_FALSE(harness.client.is_holding_for_session_conflict());
	CHECK(harness.client.state() == connection_state::reconnecting);
	CHECK(harness.client.time_until_reconnection().has_value());

	// The retry entitlement went back, so a later token rejection gets its own refresh
	// rather than being treated as a second retry.
	CHECK(harness.client.next_handshake_attempt() == handshake_attempt::first_or_backoff);

	REQUIRE(harness.advance_past_the_backoff_and_reconnect());

	harness.client.on_handshake_rejected("expired_token");

	CHECK(harness.access_token_source.refresh_call_count == 2);
}

TEST_CASE("a refused refresh prompts sign-in and an unreachable one backs off", "[transport][transport_client]")
{
	SECTION("the token endpoint refused the refresh token, which no retry fixes")
	{
		ClientHarness harness;

		harness.access_token_source.refresh_succeeds = false;
		harness.access_token_source.refresh_failure_is_retryable = false;

		harness.connect_successfully();
		harness.client.on_handshake_rejected("expired_token");

		CHECK(harness.client.is_holding_for_sign_in());
		CHECK_FALSE(harness.client.time_until_reconnection().has_value());
	}

	SECTION("the token endpoint could not be reached, which the next tick may fix")
	{
		ClientHarness harness;

		harness.access_token_source.refresh_succeeds = false;
		harness.access_token_source.refresh_failure_is_retryable = true;

		harness.connect_successfully();
		harness.client.on_handshake_rejected("expired_token");

		CHECK_FALSE(harness.client.is_holding_for_sign_in());
		CHECK(harness.client.state() == connection_state::reconnecting);
		CHECK(harness.client.time_until_reconnection().has_value());

		// The retry was never taken, so its entitlement is still available for the
		// attempt that the backoff schedules.
		CHECK(harness.client.next_handshake_attempt() == handshake_attempt::first_or_backoff);
	}
}

// ---------------------------------------------------------------------------
// Requirement 17.6 — an expired token mid-session is a reconnection, not an error
// ---------------------------------------------------------------------------

TEST_CASE("a socket dropped for an expired token recovers through the refresh path", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	// What the server pushes in its `error:authentication` envelope before dropping the
	// socket, handed on by the Message Dispatcher.
	harness.client.on_disconnected("expired_token");

	CHECK(harness.access_token_source.refresh_call_count == 1);

	// A reconnecting state rather than an error, which is what requirement 17.6 asks
	// the producer to see.
	CHECK(harness.client.state() == connection_state::reconnecting);
	CHECK(harness.state_presenter.latest().required_producer_action == producer_action::none);
}

// ---------------------------------------------------------------------------
// The server's rejection vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("the server's stable rejection codes classify as the responses they need", "[transport][transport_client]")
{
	CHECK(classify_handshake_rejection("duplicate_client_type")
		== handshake_rejection::duplicate_client_type);

	CHECK(classify_handshake_rejection("missing_token")
		== handshake_rejection::invalid_or_expired_token);
	CHECK(classify_handshake_rejection("invalid_token")
		== handshake_rejection::invalid_or_expired_token);
	CHECK(classify_handshake_rejection("expired_token")
		== handshake_rejection::invalid_or_expired_token);

	CHECK(classify_handshake_rejection("missing_client_type")
		== handshake_rejection::unsupported_client_type);
	CHECK(classify_handshake_rejection("invalid_client_type")
		== handshake_rejection::unsupported_client_type);

	// The server could not reach Cognito's key set. Not a bad token, so refreshing
	// this side's token cannot help — and transient, so backing off can.
	CHECK(classify_handshake_rejection("authentication_service_unavailable")
		== handshake_rejection::authentication_service_unavailable);

	// No code: the connection never got an answer.
	CHECK(classify_handshake_rejection("") == handshake_rejection::network_or_timeout);

	// A code this build does not know, and a prefix of one it does. Exact comparison
	// rather than a substring test, so a future code that happens to start with a
	// terminal one is not treated as terminal.
	CHECK(classify_handshake_rejection("something_new")
		== handshake_rejection::unrecognised);
	CHECK(classify_handshake_rejection("duplicate_client_type_pending")
		== handshake_rejection::unrecognised);
}

// ---------------------------------------------------------------------------
// Requirement 3.2 — the backoff schedule
// ---------------------------------------------------------------------------

TEST_CASE("the backoff schedule is monotonic, exponential, and capped", "[transport][transport_client]")
{
	CHECK(reconnection_delay_for_attempt(0) == std::chrono::milliseconds::zero());
	CHECK(reconnection_delay_for_attempt(1) == initial_reconnection_delay);
	CHECK(reconnection_delay_for_attempt(2) == initial_reconnection_delay * 2);
	CHECK(reconnection_delay_for_attempt(3) == initial_reconnection_delay * 4);

	// Non-decreasing and never above the cap, across a range that goes well past the
	// point where the cap is reached.
	for (std::size_t attempt = 1; attempt < 200; ++attempt) {
		const std::chrono::milliseconds previous = reconnection_delay_for_attempt(attempt - 1);
		const std::chrono::milliseconds current = reconnection_delay_for_attempt(attempt);

		CHECK(current >= previous);
		CHECK(current <= maximum_reconnection_delay);
		CHECK(current > std::chrono::milliseconds::zero());
	}

	// The cap is reached rather than merely respected, so the schedule is a backoff
	// and not an unbounded ramp that happens to be tested below its limit.
	CHECK(reconnection_delay_for_attempt(64) == maximum_reconnection_delay);

	// And a count large enough to overflow a shift still produces the cap rather than
	// a wrapped value — which is what would let a client that had been failing for a
	// week reconnect faster than one that had been failing for a second.
	CHECK(reconnection_delay_for_attempt(64) == reconnection_delay_for_attempt(1000000));
	CHECK(reconnection_delay_for_attempt(
		static_cast<std::size_t>(-1)
	) == maximum_reconnection_delay);
}

TEST_CASE("the client waits out each delay and lengthens it on each consecutive failure", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();

	harness.client.on_disconnected();

	// Not due yet.
	harness.clock.advance(initial_reconnection_delay - std::chrono::milliseconds{1});
	CHECK_FALSE(harness.client.reconnect_if_due());

	harness.clock.advance(std::chrono::milliseconds{1});
	REQUIRE(harness.client.reconnect_if_due());

	// The attempt failed, so the next delay is longer.
	harness.client.on_handshake_rejected("");
	CHECK(harness.client.status().consecutive_failed_attempts == 2);
	CHECK(harness.client.status().reconnection_delay == initial_reconnection_delay * 2);

	REQUIRE(harness.advance_past_the_backoff_and_reconnect());
	harness.client.on_handshake_rejected("");
	CHECK(harness.client.status().reconnection_delay == initial_reconnection_delay * 4);

	// A successful connection resets the schedule, so a drop after a long healthy
	// session starts at the beginning rather than wherever the last outage left it.
	REQUIRE(harness.advance_past_the_backoff_and_reconnect());
	harness.client.on_connected();

	CHECK(harness.client.status().consecutive_failed_attempts == 0);

	harness.client.on_disconnected();

	CHECK(harness.client.status().reconnection_delay == initial_reconnection_delay);
}

TEST_CASE("a socket that will not open is treated as a failure to back off from", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.socket_connection.refuse_to_open = true;

	CHECK_FALSE(harness.client.connect());
	CHECK(harness.client.state() == connection_state::reconnecting);
	CHECK(harness.client.status().reconnection_delay == initial_reconnection_delay);
	CHECK(harness.client.connection_count() == 0);
	CHECK(harness.client.project_context_snapshot_request_count() == 0);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

TEST_CASE("shutting down stops every automatic path for good", "[transport][transport_client]")
{
	ClientHarness harness;

	harness.connect_successfully();
	harness.client.on_disconnected();

	REQUIRE(harness.client.time_until_reconnection().has_value());

	harness.client.shut_down();

	const std::size_t handshakes_before = harness.client.handshake_count();

	harness.clock.advance(std::chrono::hours{1});

	CHECK(harness.client.has_shut_down());
	CHECK(harness.socket_connection.close_count >= 1);
	CHECK_FALSE(harness.client.reconnect_if_due());
	CHECK_FALSE(harness.client.connect());
	CHECK_FALSE(harness.client.reconnect_at_producer_request());

	harness.client.on_connected();
	harness.client.on_disconnected();
	harness.client.on_handshake_rejected("expired_token");

	CHECK(harness.client.handshake_count() == handshakes_before);
	CHECK(harness.client.state() == connection_state::disconnected);
}
