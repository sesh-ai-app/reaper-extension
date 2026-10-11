// The UI Host and the CEF bridge (requirements 15.1, 15.4, 15.5, 15.7, 22.4).
//
// Four things here are worth defending, and they are covered in that order.
//
// **No REAPER handle can cross to JavaScript.** This is structural, so most of it is
// asserted with STATIC_REQUIRE — the only way to test for code that must not compile.
// A red test would mean the handle already got through; a compile error means it never
// could. Same approach as `auth/secret_string_test.cpp` and `StreamKey`'s deleted
// insertion operator.
//
// **Teardown order.** Unregister the timer, destroy the queues, shut down CEF, close
// the socket, exit. Out of order, REAPER crashes on quit. The UI Host owns the third
// step and refuses it when the first two have not happened — refuses *before* touching
// CEF, so the browser is still alive when the violation is caught.
//
// **`file://` assets and the sign-in navigation are different paths.** The React
// application is loaded from disk; the Cognito managed login page is a navigation to a
// real `https` URL. Both directions are checked: a local page must not reach the
// network, and the sign-in navigation must not be pointed at a local path.
//
// **Platform configuration.** Requirement 15.7's three concerns are packaging, not
// logic, so they are asserted as data. What cannot be checked without a CEF
// distribution — that the helper bundles are signed acceptably, that the Windows loader
// is found, that `chrome-sandbox` is setuid, that shutdown returns cleanly — is named in
// `ui/ui_host.h` and left to a manual test session, as design.md's coverage section
// says.

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// The teardown order moved to `entry/shutdown_sequence.h` with task 17.1's plugin
// entry, which is what drives all five of its steps. Included directly rather than
// leaned on transitively through `ui/ui_host.h`, because the cases below assert on the
// sequence itself. `sesh_ai::ui::ShutdownSequence` still names it — see `ui_host.h`.
#include <entry/shutdown_sequence.h>
#include <ui/ui_host.h>

using sesh_ai::ui::BridgeMessage;
using sesh_ai::ui::BridgeRefusalReason;
using sesh_ai::ui::BrowserHost;
using sesh_ai::ui::CefPlatformConfiguration;
using sesh_ai::ui::DispatcherSink;
using sesh_ai::ui::HostPlatform;
using sesh_ai::ui::LocalAssetUrl;
using sesh_ai::ui::LocalAssetUrlResult;
using sesh_ai::ui::RemoteNavigationUrl;
using sesh_ai::ui::RemoteNavigationUrlResult;
using sesh_ai::ui::ResourceRequestVerdict;
using sesh_ai::ui::SerializedBridgePayload;
using sesh_ai::ui::ShutdownSequence;
using sesh_ai::ui::ShutdownStep;
using sesh_ai::ui::UiHost;
using sesh_ai::ui::UrlSchemeKind;
using sesh_ai::ui::bridge_message;
using sesh_ai::ui::build_local_asset_url;
using sesh_ai::ui::build_sign_in_navigation_url;
using sesh_ai::ui::cef_platform_configuration_for;
using sesh_ai::ui::classify_url_scheme;
using sesh_ai::ui::default_application_entry_point;
using sesh_ai::ui::is_bridge_transmittable_v;
using sesh_ai::ui::to_javascript_string_literal;

namespace {

	// Stands in for a REAPER handle. Declared and never defined, only ever held as a
	// pointer — which is exactly what `MediaTrack`, `ReaProject`, and `TrackEnvelope`
	// are in the SDK.
	class opaque_reaper_handle;

	// Stands in for CEF: records every call in order, so the tests can assert the
	// sequence rather than only the end state, and lets a test make CEF refuse.
	class recording_browser_host final : public BrowserHost {
	public:
		bool create_browser(const std::string& local_asset_url) override
		{
			call_log.push_back("create_browser");
			created_urls.push_back(local_asset_url);

			return !refuse_create;
		}

		bool navigate(const std::string& absolute_url) override
		{
			call_log.push_back("navigate");
			navigated_urls.push_back(absolute_url);

			return !refuse_navigate;
		}

		bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json) override
		{
			call_log.push_back("post_message_to_javascript");
			posted_message_names.push_back(message_name);
			posted_payloads.push_back(serialized_json);

			return !refuse_post;
		}

		void close_browser() override
		{
			call_log.push_back("close_browser");
			++close_browser_call_count;
		}

		void shut_down_cef() override
		{
			call_log.push_back("shut_down_cef");
			++shut_down_cef_call_count;
		}

		bool refuse_create = false;
		bool refuse_navigate = false;
		bool refuse_post = false;

		std::vector<std::string> call_log;
		std::vector<std::string> created_urls;
		std::vector<std::string> navigated_urls;
		std::vector<std::string> posted_message_names;
		std::vector<std::string> posted_payloads;
		int close_browser_call_count = 0;
		int shut_down_cef_call_count = 0;
	};

	class recording_dispatcher_sink final : public DispatcherSink {
	public:
		bool accept_message_from_javascript(const BridgeMessage& message) override
		{
			accepted_message_names.push_back(message.message_name());
			accepted_payloads.push_back(message.payload().serialized_json());

			return !refuse_messages;
		}

		bool refuse_messages = false;
		std::vector<std::string> accepted_message_names;
		std::vector<std::string> accepted_payloads;
	};

	std::size_t count_calls(const std::vector<std::string>& call_log, std::string_view name)
	{
		std::size_t count = 0;

		for (const std::string& call : call_log) {
			if (call == name) {
				++count;
			}
		}

		return count;
	}

	std::size_t index_of_call(const std::vector<std::string>& call_log, std::string_view name)
	{
		for (std::size_t index = 0; index < call_log.size(); ++index) {
			if (call_log[index] == name) {
				return index;
			}
		}

		return call_log.size();
	}

	// A valid entry point, so the tests that are about something else do not each have
	// to build one.
	LocalAssetUrl application_entry_point()
	{
		const LocalAssetUrlResult result = build_local_asset_url(
			"/Users/producer/Library/Application Support/REAPER/UserPlugins/sesh_ai_ui",
			default_application_entry_point
		);

		REQUIRE(result.valid);

		return result.url;
	}

	RemoteNavigationUrl cognito_sign_in_url()
	{
		const RemoteNavigationUrlResult result = build_sign_in_navigation_url(
			"https://sesh-ai.auth.us-east-1.amazoncognito.com/oauth2/authorize"
			"?response_type=code&client_id=abc123"
		);

		REQUIRE(result.valid);

		return result.url;
	}

	// Walks the sequence up to, but not including, `step`.
	void walk_shutdown_sequence_up_to(ShutdownSequence& sequence, ShutdownStep step)
	{
		for (const ShutdownStep required_step : ShutdownSequence::required_order()) {
			if (required_step == step) {
				return;
			}

			REQUIRE(sequence.record(required_step));
		}
	}

	// True when the interior of a JavaScript single-quoted literal cannot end it early.
	// Walks escape pairs so that `\\'` — an escaped backslash followed by a real quote —
	// is not mistaken for an escaped quote.
	bool literal_interior_is_closed(std::string_view literal)
	{
		if (literal.size() < 2 || literal.front() != '\'' || literal.back() != '\'') {
			return false;
		}

		const std::string_view interior = literal.substr(1, literal.size() - 2);

		for (std::size_t index = 0; index < interior.size(); ++index) {
			if (interior[index] == '\\') {
				// Skip whatever it escapes. A trailing backslash would escape the closing
				// quote, which is its own way of breaking out.
				if (index + 1 >= interior.size()) {
					return false;
				}

				++index;
				continue;
			}

			if (interior[index] == '\'') {
				return false;
			}
		}

		return true;
	}

}

// ---------------------------------------------------------------------------
// Requirement 15.4 — no REAPER handle crosses to JavaScript
// ---------------------------------------------------------------------------

TEST_CASE("the bridge carries serialized text and refuses a REAPER handle", "[ui][host][bridge]")
{
	SECTION("text goes across")
	{
		STATIC_REQUIRE(is_bridge_transmittable_v<std::string>);

		// A string literal is serialized text already, which is why character pointers
		// are the one pointer kind the payload accepts.
		STATIC_REQUIRE(is_bridge_transmittable_v<const char*>);
		STATIC_REQUIRE(is_bridge_transmittable_v<char*>);
	}

	SECTION("a REAPER handle does not, by any spelling")
	{
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<opaque_reaper_handle*>);
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<const opaque_reaper_handle*>);
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<void*>);
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<const void*>);

		// Nor as the integer somebody reinterprets one into, since there is no numeric
		// constructor at all.
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<int>);
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<unsigned long long>);
		STATIC_REQUIRE_FALSE(is_bridge_transmittable_v<bool>);
	}

	SECTION("nor one level out, through the message or its factory")
	{
		STATIC_REQUIRE(std::is_constructible_v<BridgeMessage, std::string, SerializedBridgePayload>);

		STATIC_REQUIRE_FALSE(
			std::is_constructible_v<BridgeMessage, std::string, opaque_reaper_handle*>
		);
		STATIC_REQUIRE_FALSE(std::is_constructible_v<BridgeMessage, std::string, void*>);
	}

	SECTION("the payload is explicit, so nothing becomes one by accident")
	{
		STATIC_REQUIRE(std::is_constructible_v<SerializedBridgePayload, std::string>);
		STATIC_REQUIRE_FALSE(std::is_convertible_v<std::string, SerializedBridgePayload>);
	}
}

TEST_CASE("what reaches CEF is exactly the bytes that went in", "[ui][host][bridge]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	REQUIRE(ui_host.create_browser(application_entry_point()).created);

	const std::string payload =
		R"({"conversationId":"c-1","text":"kick's too quiet, isn't it?"})";

	const auto outcome = ui_host.publish_to_javascript(bridge_message("view:agent_response", payload));

	CHECK(outcome.published);
	CHECK(outcome.message_name == "view:agent_response");
	CHECK(outcome.serialized_byte_count == payload.size());

	REQUIRE(browser_host.posted_payloads.size() == 1);
	CHECK(browser_host.posted_payloads.front() == payload);
	CHECK(browser_host.posted_message_names.front() == "view:agent_response");
	CHECK(ui_host.published_message_count() == 1);
}

TEST_CASE("a message from JavaScript reaches the dispatcher as serialized text", "[ui][host][bridge]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	REQUIRE(ui_host.create_browser(application_entry_point()).created);

	const auto outcome = ui_host.receive_from_javascript(
		"state:prompt",
		SerializedBridgePayload{std::string{R"({"promptText":"warm up the snare"})"}}
	);

	CHECK(outcome.forwarded);
	CHECK(outcome.message_name == "state:prompt");
	REQUIRE(dispatcher_sink.accepted_message_names.size() == 1);
	CHECK(dispatcher_sink.accepted_message_names.front() == "state:prompt");
	CHECK(dispatcher_sink.accepted_payloads.front() == R"({"promptText":"warm up the snare"})");
	CHECK(ui_host.received_message_count() == 1);

	SECTION("a dispatcher refusal is reported rather than swallowed")
	{
		dispatcher_sink.refuse_messages = true;

		const auto refused = ui_host.receive_from_javascript(
			"state:prompt",
			SerializedBridgePayload{std::string{"{}"}}
		);

		CHECK_FALSE(refused.forwarded);
		CHECK(refused.refusal == BridgeRefusalReason::dispatcher_refused);
		CHECK(ui_host.received_message_count() == 1);
	}
}

// ---------------------------------------------------------------------------
// Requirement 15.1 — the React application loads from `file://`
// ---------------------------------------------------------------------------

TEST_CASE("the application entry point is a file:// url over the built assets", "[ui][host][assets]")
{
	SECTION("a POSIX installation")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/UserPlugins/sesh_ai_ui", "index.html");

		REQUIRE(result.valid);
		CHECK(result.url.value() == "file:///opt/reaper/UserPlugins/sesh_ai_ui/index.html");
		CHECK(classify_url_scheme(result.url.value()) == UrlSchemeKind::local_asset);
	}

	SECTION("a Windows installation keeps the root slash file:// needs")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("C:\\Users\\producer\\REAPER\\UserPlugins\\sesh_ai_ui", "index.html");

		REQUIRE(result.valid);
		CHECK(result.url.value()
			== "file:///C:/Users/producer/REAPER/UserPlugins/sesh_ai_ui/index.html");
	}

	SECTION("a trailing separator does not double up")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/sesh_ai_ui/", "index.html");

		REQUIRE(result.valid);
		CHECK(result.url.value() == "file:///opt/reaper/sesh_ai_ui/index.html");
	}

	SECTION("a space is encoded rather than refused — macOS puts REAPER under one")
	{
		const LocalAssetUrlResult result = build_local_asset_url(
			"/Users/producer/Library/Application Support/REAPER/UserPlugins/sesh_ai_ui",
			"index.html"
		);

		REQUIRE(result.valid);
		CHECK(result.url.value()
			== "file:///Users/producer/Library/Application%20Support/REAPER/"
				"UserPlugins/sesh_ai_ui/index.html");
	}

	SECTION("a nested asset, which is what Vite emits")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/sesh_ai_ui", "assets/index-4f2b1c.js");

		REQUIRE(result.valid);
		CHECK(result.url.value() == "file:///opt/reaper/sesh_ai_ui/assets/index-4f2b1c.js");
	}
}

TEST_CASE("the asset url refuses everything that would not be local", "[ui][host][assets]")
{
	SECTION("a directory that is not an absolute filesystem path")
	{
		CHECK_FALSE(build_local_asset_url("", "index.html").valid);
		CHECK_FALSE(build_local_asset_url("sesh_ai_ui", "index.html").valid);
		CHECK_FALSE(build_local_asset_url("./dist", "index.html").valid);
	}

	SECTION("a directory that is a url")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("https://assets.sesh.ai/ui", "index.html");

		CHECK_FALSE(result.valid);
		CHECK(result.rejection_reason == "the asset directory must be a filesystem path, not a url");
	}

	SECTION("an asset path that is a remote url — the one that would look local and not be")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/sesh_ai_ui", "https://example.invalid/payload.js");

		CHECK_FALSE(result.valid);
		CHECK(result.rejection_reason
			== "the asset path must be relative to the asset directory, not a url");
	}

	SECTION("an asset path that escapes the asset directory")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/sesh_ai_ui", "../../../etc/passwd");

		CHECK_FALSE(result.valid);
		CHECK(result.rejection_reason == "the asset path must not escape the asset directory");
	}

	SECTION("an absolute asset path")
	{
		CHECK_FALSE(build_local_asset_url("/opt/reaper/sesh_ai_ui", "/etc/passwd").valid);
		CHECK_FALSE(build_local_asset_url("/opt/reaper/sesh_ai_ui", "\\windows\\system32").valid);
	}

	SECTION("an empty asset path")
	{
		CHECK_FALSE(build_local_asset_url("/opt/reaper/sesh_ai_ui", "").valid);
	}

	SECTION("a control character, which is how a truncated path arrives")
	{
		CHECK_FALSE(build_local_asset_url(std::string{"/opt/reaper\n"}, "index.html").valid);
		CHECK_FALSE(build_local_asset_url("/opt/reaper", std::string{"index.html\n"}).valid);
	}

	SECTION("two dots inside a file name are not a '..' segment")
	{
		const LocalAssetUrlResult result =
			build_local_asset_url("/opt/reaper/sesh_ai_ui", "assets/my..take.js");

		REQUIRE(result.valid);
		CHECK(result.url.value() == "file:///opt/reaper/sesh_ai_ui/assets/my..take.js");
	}
}

// ---------------------------------------------------------------------------
// The sign-in page is a navigation to a real URL, not a fetch from `file://`
// ---------------------------------------------------------------------------

TEST_CASE("the sign-in navigation takes an https url and nothing else", "[ui][host][signin]")
{
	SECTION("the Cognito managed login page")
	{
		const RemoteNavigationUrlResult result = build_sign_in_navigation_url(
			"https://sesh-ai.auth.us-east-1.amazoncognito.com/oauth2/authorize?response_type=code"
		);

		REQUIRE(result.valid);
		CHECK(classify_url_scheme(result.url.value()) == UrlSchemeKind::secure_remote);
	}

	SECTION("a file:// url is refused — the sign-in page is not a local asset")
	{
		const RemoteNavigationUrlResult result =
			build_sign_in_navigation_url("file:///opt/reaper/sesh_ai_ui/index.html");

		CHECK_FALSE(result.valid);
		CHECK(result.url.empty());
		CHECK(result.rejection_reason
			== "the sign-in page is a navigation to the cognito managed login page, "
				"not a local asset");
	}

	SECTION("a bare filesystem path is refused too — the other half of the same mistake")
	{
		CHECK_FALSE(build_sign_in_navigation_url("/opt/reaper/sesh_ai_ui/index.html").valid);
		CHECK_FALSE(build_sign_in_navigation_url("C:\\reaper\\sesh_ai_ui\\index.html").valid);
		CHECK_FALSE(build_sign_in_navigation_url("index.html").valid);
	}

	SECTION("plaintext http is refused, loopback included — the page carries credentials")
	{
		CHECK_FALSE(build_sign_in_navigation_url("http://sesh-ai.auth.example/oauth2/authorize").valid);
		CHECK_FALSE(build_sign_in_navigation_url("http://localhost:3000/oauth2/authorize").valid);
	}

	SECTION("an https url with no host")
	{
		const RemoteNavigationUrlResult result = build_sign_in_navigation_url("https:///oauth2/authorize");

		CHECK_FALSE(result.valid);
		CHECK(result.rejection_reason == "the sign-in url has no host");
	}

	SECTION("empty, or carrying whitespace")
	{
		CHECK_FALSE(build_sign_in_navigation_url("").valid);
		CHECK_FALSE(build_sign_in_navigation_url("https://sesh.ai/oauth2 /authorize").valid);
		CHECK_FALSE(build_sign_in_navigation_url(std::string{"https://sesh.ai/a\nb"}).valid);
	}
}

TEST_CASE("neither url type can be turned into the other", "[ui][host][signin]")
{
	// The structural half of the same property: the only way to obtain either type is
	// its validating factory, so a local path cannot reach the sign-in navigation and a
	// remote url cannot be loaded as an asset — whatever a caller intends.
	STATIC_REQUIRE_FALSE(std::is_constructible_v<LocalAssetUrl, std::string>);
	STATIC_REQUIRE_FALSE(std::is_constructible_v<RemoteNavigationUrl, std::string>);
	STATIC_REQUIRE_FALSE(std::is_constructible_v<RemoteNavigationUrl, LocalAssetUrl>);
	STATIC_REQUIRE_FALSE(std::is_constructible_v<LocalAssetUrl, RemoteNavigationUrl>);
}

// ---------------------------------------------------------------------------
// A `file://` page must not reach the network, and a remote page must not read disk
// ---------------------------------------------------------------------------

TEST_CASE("a resource request is judged by which page is asking", "[ui][host][origins]")
{
	using sesh_ai::ui::evaluate_resource_request;

	SECTION("the React application loading its own bundle")
	{
		const ResourceRequestVerdict verdict = evaluate_resource_request(
			"file:///opt/reaper/sesh_ai_ui/index.html",
			"file:///opt/reaper/sesh_ai_ui/assets/index-4f2b1c.js"
		);

		CHECK(verdict.permitted);
		CHECK(verdict.refusal_reason.empty());
	}

	SECTION("the React application reaching the network — refused in both schemes")
	{
		for (const char* requested_url : {
				"https://api.sesh.ai/conversations",
				"http://api.sesh.ai/conversations",
				"https://sesh-ai.auth.us-east-1.amazoncognito.com/oauth2/token"
			}) {
			const ResourceRequestVerdict verdict = evaluate_resource_request(
				"file:///opt/reaper/sesh_ai_ui/index.html",
				requested_url
			);

			CHECK_FALSE(verdict.permitted);
			CHECK(verdict.refusal_reason
				== "a file:// page must not reach the network — every request the "
					"extension makes goes through the c++ socket.io client");
		}
	}

	SECTION("the Cognito login page loading its own scripts")
	{
		const ResourceRequestVerdict verdict = evaluate_resource_request(
			"https://sesh-ai.auth.us-east-1.amazoncognito.com/oauth2/authorize",
			"https://sesh-ai.auth.us-east-1.amazoncognito.com/assets/login.js"
		);

		CHECK(verdict.permitted);
	}

	SECTION("the Cognito login page reading the producer's disk")
	{
		const ResourceRequestVerdict verdict = evaluate_resource_request(
			"https://sesh-ai.auth.us-east-1.amazoncognito.com/oauth2/authorize",
			"file:///Users/producer/.aws/credentials"
		);

		CHECK_FALSE(verdict.permitted);
		CHECK(verdict.refusal_reason == "a remote page must not read local assets");
	}

	SECTION("a page loaded without TLS is refused whatever it asks for")
	{
		CHECK_FALSE(evaluate_resource_request("http://example.invalid/", "https://sesh.ai/a").permitted);
		CHECK_FALSE(evaluate_resource_request("http://example.invalid/", "file:///etc/passwd").permitted);
	}

	SECTION("a scheme with no rule is refused rather than guessed at")
	{
		CHECK_FALSE(evaluate_resource_request("data:text/html,<p>", "file:///etc/passwd").permitted);
		CHECK_FALSE(evaluate_resource_request(
			"file:///opt/reaper/sesh_ai_ui/index.html",
			"chrome-extension://abc/background.js"
		).permitted);
	}
}

TEST_CASE("the host judges a request against the page it actually loaded", "[ui][host][origins]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	REQUIRE(ui_host.create_browser(application_entry_point()).created);
	CHECK_FALSE(ui_host.showing_sign_in_page());
	CHECK_FALSE(ui_host.evaluate_resource_request("https://api.sesh.ai/conversations").permitted);

	REQUIRE(ui_host.navigate_to_sign_in(cognito_sign_in_url()).navigated);
	CHECK(ui_host.showing_sign_in_page());

	// The same request is now legitimate: the page asking is Cognito's, not the panel's.
	CHECK(ui_host.evaluate_resource_request(
		"https://sesh-ai.auth.us-east-1.amazoncognito.com/assets/login.js"
	).permitted);
	CHECK_FALSE(ui_host.evaluate_resource_request("file:///Users/producer/.aws/credentials").permitted);

	REQUIRE(ui_host.return_to_application(application_entry_point()).created);
	CHECK_FALSE(ui_host.showing_sign_in_page());
	CHECK_FALSE(ui_host.evaluate_resource_request("https://api.sesh.ai/conversations").permitted);
}

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------

TEST_CASE("the bridge refuses work the browser cannot do yet", "[ui][host][bridge]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	SECTION("publishing before the browser exists")
	{
		const auto outcome = ui_host.publish_to_javascript(bridge_message("view:agent_response", "{}"));

		CHECK_FALSE(outcome.published);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_not_created);
		CHECK(browser_host.call_log.empty());
	}

	SECTION("receiving before the browser exists")
	{
		const auto outcome = ui_host.receive_from_javascript(
			"state:prompt",
			SerializedBridgePayload{std::string{"{}"}}
		);

		CHECK_FALSE(outcome.forwarded);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_not_created);
		CHECK(dispatcher_sink.accepted_message_names.empty());
	}

	SECTION("navigating before the browser exists")
	{
		const auto outcome = ui_host.navigate_to_sign_in(cognito_sign_in_url());

		CHECK_FALSE(outcome.navigated);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_not_created);
		CHECK(browser_host.call_log.empty());
	}

	SECTION("a second browser in the same panel")
	{
		REQUIRE(ui_host.create_browser(application_entry_point()).created);

		const auto outcome = ui_host.create_browser(application_entry_point());

		CHECK_FALSE(outcome.created);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_already_created);
		CHECK(count_calls(browser_host.call_log, "create_browser") == 1);
	}

	SECTION("CEF refusing the creation leaves the host uncreated")
	{
		browser_host.refuse_create = true;

		const auto outcome = ui_host.create_browser(application_entry_point());

		CHECK_FALSE(outcome.created);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_host_refused);
		CHECK_FALSE(ui_host.browser_created());
	}

	SECTION("a nameless or empty message goes nowhere")
	{
		REQUIRE(ui_host.create_browser(application_entry_point()).created);

		CHECK(ui_host.publish_to_javascript(bridge_message("", "{}")).refusal
			== BridgeRefusalReason::empty_message_name);
		CHECK(ui_host.publish_to_javascript(bridge_message("view:agent_response", "")).refusal
			== BridgeRefusalReason::empty_payload);
		CHECK(count_calls(browser_host.call_log, "post_message_to_javascript") == 0);

		CHECK(ui_host.receive_from_javascript("", SerializedBridgePayload{std::string{"{}"}}).refusal
			== BridgeRefusalReason::empty_message_name);
		CHECK(ui_host.receive_from_javascript("state:prompt", SerializedBridgePayload{}).refusal
			== BridgeRefusalReason::empty_payload);
		CHECK(dispatcher_sink.accepted_message_names.empty());
	}

	SECTION("CEF refusing a post is reported")
	{
		REQUIRE(ui_host.create_browser(application_entry_point()).created);
		browser_host.refuse_post = true;

		const auto outcome = ui_host.publish_to_javascript(bridge_message("view:agent_response", "{}"));

		CHECK_FALSE(outcome.published);
		CHECK(outcome.refusal == BridgeRefusalReason::browser_host_refused);
		CHECK(ui_host.published_message_count() == 0);
	}
}

// ---------------------------------------------------------------------------
// Teardown order (requirement 1.2, task 17.1)
// ---------------------------------------------------------------------------

TEST_CASE("the teardown order is the one requirement 1.2 fixes", "[ui][host][teardown]")
{
	// Stated rather than inferred. Every other test in this section depends on this
	// order being right, so the order itself is asserted literally: the timer stops
	// first so no tick can reach a torn-down browser, the queues go next so nothing is
	// left with no destination, CEF third because a tick and a queue both outlive it
	// otherwise, the socket fourth, and the process last.
	constexpr auto order = ShutdownSequence::required_order();

	STATIC_REQUIRE(order.size() == 5);
	STATIC_REQUIRE(order[0] == ShutdownStep::unregister_timer);
	STATIC_REQUIRE(order[1] == ShutdownStep::destroy_queues);
	STATIC_REQUIRE(order[2] == ShutdownStep::shut_down_cef);
	STATIC_REQUIRE(order[3] == ShutdownStep::close_socket_io_connection);
	STATIC_REQUIRE(order[4] == ShutdownStep::exit_extension);
}

TEST_CASE("the sequence accepts the required order and nothing else", "[ui][host][teardown]")
{
	SECTION("walked in order it completes, unbroken")
	{
		ShutdownSequence sequence;

		for (const ShutdownStep step : ShutdownSequence::required_order()) {
			REQUIRE(sequence.is_next(step));
			REQUIRE(sequence.record(step));
		}

		CHECK(sequence.completed());
		CHECK_FALSE(sequence.broken());
		CHECK(sequence.taken_steps().size() == 5);
		CHECK(sequence.violations().empty());
	}

	SECTION("a skipped step is refused and named")
	{
		ShutdownSequence sequence;

		REQUIRE(sequence.record(ShutdownStep::unregister_timer));

		CHECK_FALSE(sequence.record(ShutdownStep::shut_down_cef));
		REQUIRE(sequence.violations().size() == 1);
		CHECK(sequence.violations().front().attempted_step == ShutdownStep::shut_down_cef);
		CHECK(sequence.violations().front().expected_step == ShutdownStep::destroy_queues);
		CHECK(sequence.violations().front().description
			== "teardown step 'shut down cef' was attempted but 'destroy the queues' was "
				"required next");
		CHECK(sequence.broken());
	}

	SECTION("a repeated step is refused")
	{
		ShutdownSequence sequence;

		REQUIRE(sequence.record(ShutdownStep::unregister_timer));
		CHECK_FALSE(sequence.record(ShutdownStep::unregister_timer));
		CHECK(sequence.taken_steps().size() == 1);
	}

	SECTION("a step after completion is refused, and says so")
	{
		ShutdownSequence sequence;

		for (const ShutdownStep step : ShutdownSequence::required_order()) {
			REQUIRE(sequence.record(step));
		}

		CHECK_FALSE(sequence.record(ShutdownStep::shut_down_cef));
		REQUIRE(sequence.violations().size() == 1);
		CHECK(sequence.violations().front().sequence_was_complete);
	}

	SECTION("once broken it stays broken — a later correct step does not repair the record")
	{
		ShutdownSequence sequence;

		CHECK_FALSE(sequence.record(ShutdownStep::shut_down_cef));
		CHECK(sequence.record(ShutdownStep::unregister_timer));
		CHECK(sequence.broken());
		CHECK(sequence.violations().size() == 1);
	}
}

TEST_CASE("the UI Host will not shut CEF down out of turn", "[ui][host][teardown]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	REQUIRE(ui_host.create_browser(application_entry_point()).created);

	SECTION("with the timer still registered and the queues still alive")
	{
		ShutdownSequence sequence;

		const auto outcome = ui_host.shut_down(sequence);

		CHECK_FALSE(outcome.cef_shut_down);
		CHECK(outcome.refusal == BridgeRefusalReason::shutdown_step_out_of_order);

		// The point of claiming the step first: CEF is still up when the violation is
		// caught, so nothing has been half torn down.
		CHECK(browser_host.shut_down_cef_call_count == 0);
		CHECK(browser_host.close_browser_call_count == 0);
		CHECK_FALSE(ui_host.cef_shut_down());
		CHECK(ui_host.browser_created());
		CHECK(sequence.broken());
	}

	SECTION("with the queues still alive")
	{
		ShutdownSequence sequence;

		REQUIRE(sequence.record(ShutdownStep::unregister_timer));

		CHECK_FALSE(ui_host.shut_down(sequence).cef_shut_down);
		CHECK(browser_host.shut_down_cef_call_count == 0);
		CHECK_FALSE(ui_host.cef_shut_down());
	}

	SECTION("at its turn, it closes the browser and then shuts CEF down")
	{
		ShutdownSequence sequence;

		walk_shutdown_sequence_up_to(sequence, ShutdownStep::shut_down_cef);

		const auto outcome = ui_host.shut_down(sequence);

		CHECK(outcome.cef_shut_down);
		CHECK(outcome.refusal == BridgeRefusalReason::none);
		CHECK(browser_host.close_browser_call_count == 1);
		CHECK(browser_host.shut_down_cef_call_count == 1);

		// Closing precedes shutting down: CefShutdown with a live browser is the
		// crash-on-quit this ordering exists to avoid.
		CHECK(index_of_call(browser_host.call_log, "close_browser")
			< index_of_call(browser_host.call_log, "shut_down_cef"));

		CHECK(ui_host.cef_shut_down());
		CHECK_FALSE(ui_host.browser_created());
		CHECK_FALSE(sequence.broken());
		CHECK(sequence.is_next(ShutdownStep::close_socket_io_connection));
	}

	SECTION("a second shutdown is reported, not attempted again")
	{
		ShutdownSequence sequence;

		walk_shutdown_sequence_up_to(sequence, ShutdownStep::shut_down_cef);
		REQUIRE(ui_host.shut_down(sequence).cef_shut_down);

		const auto outcome = ui_host.shut_down(sequence);

		CHECK_FALSE(outcome.cef_shut_down);
		CHECK(outcome.already_shut_down);
		CHECK(outcome.refusal == BridgeRefusalReason::cef_already_shut_down);
		CHECK(browser_host.shut_down_cef_call_count == 1);

		// And the sequence is undisturbed, so the remaining steps still run.
		CHECK_FALSE(sequence.broken());
	}
}

TEST_CASE("nothing crosses the bridge after CEF is gone", "[ui][host][teardown]")
{
	recording_browser_host browser_host;
	recording_dispatcher_sink dispatcher_sink;
	UiHost ui_host{browser_host, dispatcher_sink};

	REQUIRE(ui_host.create_browser(application_entry_point()).created);

	ShutdownSequence sequence;
	walk_shutdown_sequence_up_to(sequence, ShutdownStep::shut_down_cef);
	REQUIRE(ui_host.shut_down(sequence).cef_shut_down);

	const std::size_t calls_after_shutdown = browser_host.call_log.size();

	CHECK(ui_host.publish_to_javascript(bridge_message("view:agent_response", "{}")).refusal
		== BridgeRefusalReason::cef_already_shut_down);
	CHECK(ui_host.receive_from_javascript(
		"state:prompt",
		SerializedBridgePayload{std::string{"{}"}}
	).refusal == BridgeRefusalReason::cef_already_shut_down);
	CHECK(ui_host.navigate_to_sign_in(cognito_sign_in_url()).refusal
		== BridgeRefusalReason::cef_already_shut_down);
	CHECK(ui_host.create_browser(application_entry_point()).refusal
		== BridgeRefusalReason::cef_already_shut_down);
	CHECK(ui_host.return_to_application(application_entry_point()).refusal
		== BridgeRefusalReason::cef_already_shut_down);

	CHECK(browser_host.call_log.size() == calls_after_shutdown);
	CHECK(dispatcher_sink.accepted_message_names.empty());
}

// ---------------------------------------------------------------------------
// Requirement 15.7 — the platform-specific concerns, as data
// ---------------------------------------------------------------------------

TEST_CASE("every platform gets its own process and its own subprocess binary", "[ui][host][platform]")
{
	for (const HostPlatform platform : {HostPlatform::macos, HostPlatform::windows, HostPlatform::linux}) {
		const CefPlatformConfiguration configuration = cef_platform_configuration_for(platform);

		CHECK(configuration.platform == platform);

		// Requirements 15.5 and 22.4.
		CHECK(configuration.browser_runs_in_its_own_process);

		// Requirement 15.7's clean shutdown, on all three.
		CHECK(configuration.requires_cef_shutdown_before_process_exit);

		// The extension is a shared library inside REAPER, so CEF cannot re-execute the
		// host binary to make a renderer. Every platform ships its own helper.
		CHECK_FALSE(configuration.subprocess_executable_name.empty());
	}
}

TEST_CASE("each platform carries its own CEF concern and not the others'", "[ui][host][platform]")
{
	SECTION("macOS — helper bundles, each individually signed")
	{
		const CefPlatformConfiguration configuration =
			cef_platform_configuration_for(HostPlatform::macos);

		CHECK(configuration.helper_processes_require_individual_code_signatures);
		CHECK(configuration.helper_process_bundle_names.size() == 5);
		CHECK(configuration.framework_bundle_name == "Chromium Embedded Framework.framework");
		CHECK(configuration.process_sandbox_enabled);

		// Not Windows' concern and not Linux's.
		CHECK_FALSE(configuration.loader_must_sit_beside_extension_binary);
		CHECK_FALSE(configuration.process_sandbox_requires_setuid_helper);
	}

	SECTION("Windows — the loader beside the extension binary")
	{
		const CefPlatformConfiguration configuration =
			cef_platform_configuration_for(HostPlatform::windows);

		CHECK(configuration.loader_must_sit_beside_extension_binary);
		CHECK(configuration.loader_file_name == "libcef.dll");

		// The sandbox has to be initialized in the host executable's entry point, and
		// that executable is REAPER's. False rather than a setting that looks enabled.
		CHECK_FALSE(configuration.process_sandbox_enabled);

		CHECK_FALSE(configuration.helper_processes_require_individual_code_signatures);
		CHECK(configuration.helper_process_bundle_names.empty());
	}

	SECTION("Linux — the setuid sandbox helper")
	{
		const CefPlatformConfiguration configuration =
			cef_platform_configuration_for(HostPlatform::linux);

		CHECK(configuration.process_sandbox_requires_setuid_helper);
		CHECK(configuration.sandbox_helper_file_name == "chrome-sandbox");
		CHECK(configuration.process_sandbox_enabled);

		CHECK_FALSE(configuration.loader_must_sit_beside_extension_binary);
		CHECK(configuration.framework_bundle_name.empty());
	}
}

// ---------------------------------------------------------------------------
// Delivering a payload into the page
// ---------------------------------------------------------------------------

TEST_CASE("a payload cannot break out of the JavaScript literal it travels in", "[ui][host][bridge]")
{
	SECTION("plain text is quoted and otherwise unchanged")
	{
		CHECK(to_javascript_string_literal("view:agent_response") == "'view:agent_response'");
		CHECK(to_javascript_string_literal("") == "''");
	}

	SECTION("the characters that would end the literal early")
	{
		CHECK(to_javascript_string_literal("it's") == R"('it\'s')");
		CHECK(to_javascript_string_literal("a\\b") == R"('a\\b')");
		CHECK(to_javascript_string_literal("\"quoted\"") == R"('\"quoted\"')");
		CHECK(to_javascript_string_literal("line\nbreak") == R"('line\nbreak')");
		CHECK(to_javascript_string_literal("tab\there") == R"('tab\there')");
	}

	SECTION("a closing script tag cannot appear in the output")
	{
		const std::string literal = to_javascript_string_literal("</script><img onerror=x>");

		CHECK(literal.find("</") == std::string::npos);
		CHECK(literal.find('<') == std::string::npos);
	}

	SECTION("control characters become hexadecimal escapes")
	{
		// Split, because a C++ hexadecimal escape is greedy: "a\x01b" is one character
		// 0x1B, not 0x01 followed by 'b'.
		CHECK(to_javascript_string_literal(std::string{"a\x01" "b"}) == R"('a\x01b')");
		CHECK(to_javascript_string_literal(std::string(1, '\x7f')) == R"('\x7F')");
	}

	SECTION("the two Unicode characters JavaScript treats as line terminators")
	{
		CHECK(to_javascript_string_literal("a\xe2\x80\xa8" "b") == R"('a\u2028b')");
		CHECK(to_javascript_string_literal("a\xe2\x80\xa9" "b") == R"('a\u2029b')");
	}

	SECTION("a trailing backslash does not escape the closing quote")
	{
		const std::string literal = to_javascript_string_literal("ends with\\");

		CHECK(literal == R"('ends with\\')");
		CHECK(literal_interior_is_closed(literal));
	}

	SECTION("the interior is closed for every payload shape that tries to open it")
	{
		for (const char* payload : {
				"'",
				"\\'",
				"\\\\'",
				"'; alert(1); //",
				"'+document.cookie+'",
				"\\",
				"\\\\",
				R"({"text":"it's a \"take\"\n"})",
				"</script>'",
				"\xe2\x80\xa8'"
			}) {
			CHECK(literal_interior_is_closed(to_javascript_string_literal(payload)));
		}
	}
}
