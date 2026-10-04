// The UI Host (requirements 15.1, 15.4, 15.5, 15.7, 22.4; design "UI Host").
//
// Creates the CEF browser inside REAPER's dockable panel, loads the React application
// from the Vite-built static assets over `file://`, navigates to the Cognito managed
// login page when the producer signs in, and carries serialized messages between the
// browser and the Main-Thread Dispatcher.
//
// Everything in this header is decision logic over strings. CEF lives behind
// `BrowserHost`, the dispatcher behind `DispatcherSink`, and the real implementations
// of both are elsewhere — `ui/cef_browser_host.h` for the first, task 17.1's plugin
// entry for the second. That is the same arrangement, for the same reason, as
// `entry/timer_registration.h` and `daw/render_coordinator.h`: the suite has no CEF
// distribution and no REAPER SDK on its include path, so the logic worth defending has
// to be reachable without either.
//
// Defined inline here rather than in `ui_host.cpp`, following `stream_presenter.h`: the
// test target globs only `tests/*.cpp`, so logic in a `src/*.cpp` is logic the suite
// cannot reach.
//
// ---------------------------------------------------------------------------
// No REAPER handle can cross to JavaScript, and that is a compile error rather
// than a test failure (requirement 15.4)
//
// "Serialize everything" is a rule somebody breaks the first time a tool result is
// awkward to encode and a `MediaTrack*` is right there. So the bridge is built out of
// types that cannot hold one:
//
//   - `SerializedBridgePayload` is constructible from an owned `std::string` and from
//     character pointers, which are already text. Every other pointer type hits a
//     deleted constructor, so `SerializedBridgePayload{track}` names itself at compile
//     time instead of resolving to some conversion.
//   - `BridgeMessage` carries a name and one of those payloads, and repeats the
//     deletion one level out so the error points at the call site that tried.
//   - `BrowserHost` — the only route from this component into CEF — takes
//     `const std::string&` and nothing else. There is no overload a handle fits.
//   - `DispatcherSink` takes a `BridgeMessage`, so the inbound direction is closed the
//     same way.
//
// This mirrors `StreamKey`'s deleted `operator<<` in `stream_presenter.h`. The suite
// asserts the absence with `std::is_constructible_v`, which is the only way to test for
// code that must not compile.
//
// ---------------------------------------------------------------------------
// The message names are declared here, because nothing links the two halves
//
// Requirement 15.4's serialized bridge has a cost: the TypeScript side is compiled by
// a different toolchain and the two never meet, so a name that drifts is not a build
// failure on either side. It is a panel that quietly stops showing one thing.
//
// So the ten names, their direction, and the schema each payload is described by are
// declared once, below, and `ui/` reads them from the generated bundle rather than
// spelling its own. The split between the four protocol payloads carried verbatim and
// the six view models is design.md's "The CEF bridge contract"; the short version is
// that a view model carries fields the C++ side derived and no protocol schema has, so
// forwarding a payload instead would mean re-deriving them in TypeScript.
//
// They live in this header rather than beside `javascript_bridge_receive_function()` in
// `ui/cef_browser_host.h`, which is where they were first proposed, for the reason that
// decides everything else in this component: that file needs a CEF distribution, so the
// suite never compiles it. A contract nothing can assert is the contract that drifts.
// `cef_browser_host.h` includes this one, so the adapter sees them anyway.
//
// ---------------------------------------------------------------------------
// `file://` assets and the sign-in navigation are two different paths, and neither
// can be pointed at the other
//
// The React application is loaded from disk: `base: './'` in `ui/vite.config.ts` makes
// every emitted asset reference relative, so `file:///…/dist/index.html` resolves its
// own scripts and stylesheets and needs no origin. No CORS question arises because the
// browser makes no network requests at all — the Socket.IO connection and the REST
// calls are the C++ client's, on the network thread (requirement 22.2).
//
// The Cognito managed login page is the opposite: a real navigation to a real `https`
// URL, because WebAuthn platform authenticators and Cognito's own scripts need a secure
// origin. Fetching it from the `file://` page would be a cross-origin request from an
// opaque origin, which is both refused by Chromium and the wrong shape — the producer
// has to see and interact with that page.
//
// Conflating the two is the failure worth ruling out structurally, so:
//
//   - `LocalAssetUrl` and `RemoteNavigationUrl` are distinct types with private
//     constructors. The only way to obtain either is its validating factory, and
//     neither converts to the other.
//   - `UiHost::create_browser` takes a `LocalAssetUrl`; `UiHost::navigate_to_sign_in`
//     takes a `RemoteNavigationUrl`. Passing a filesystem path to the sign-in
//     navigation is not an expressible call.
//   - `build_local_asset_url` refuses anything carrying a scheme, and
//     `build_sign_in_navigation_url` refuses `file://` and refuses plaintext `http`
//     (requirement 26.3).
//   - `evaluate_resource_request` answers the runtime half: a `file://` page must never
//     reach a remote URL, and a remote page must never reach a local asset.
//
// ---------------------------------------------------------------------------
// Teardown order is load-bearing, and this component owns one step of it
//
// Requirement 1.2 and task 17.1 fix the order: unregister the timer, destroy the
// queues, shut down CEF, close the Socket.IO connection, exit. Out of order, REAPER
// crashes on quit, which producers report as Sesh losing their session.
//
// CEF third is not arbitrary. Shutting it down while the timer is still registered
// leaves a tick that can publish into a browser that is going away; shutting it down
// while the queues still hold envelopes leaves work whose only destination is gone. And
// it has to happen before the process exits on all three platforms, because CEF's
// helper processes outlive an abrupt exit.
//
// `ShutdownSequence` makes that order a value rather than a comment. `UiHost::shut_down`
// claims its step from the sequence *before* it touches CEF, and refuses when the step
// is not the next one required — so an out-of-order teardown is detectable, and CEF is
// still running when it is detected.
//
// The sequence was declared here, because the UI Host was the first component that had
// to refuse on it. It now lives in `entry/shutdown_sequence.h`, with the plugin entry
// that drives it end to end — the move this comment used to describe as pending. It was
// a rename: `sesh_ai::ui::ShutdownSequence` still names the same type, and this
// component still owns exactly the third step.
//
// ---------------------------------------------------------------------------
// The platform-specific work is configuration, so it is data
//
// Requirement 15.7's three concerns — helper process signing on macOS, loader placement
// on Windows, process sandboxing on Linux — are packaging and launch arrangements, not
// logic. `CefPlatformConfiguration` states them per platform so that they are at least
// assertable and so that the packaging work (task 21.x) reads them from one place
// instead of rediscovering them.
//
// What genuinely cannot be verified here, and is named so it is not mistaken for
// covered: that the helper bundles are signed with entitlements CEF accepts, that
// `libcef.dll` is found by the Windows loader from REAPER's `UserPlugins` directory,
// that `chrome-sandbox` has its setuid bit on a real install, and that
// `CefShutdown` returns cleanly. All four need a CEF distribution and a running REAPER.
// They are manual test sessions, as design.md's "Not coverable this way" section says.

#ifndef SESH_AI_UI_UI_HOST_H
#define SESH_AI_UI_UI_HOST_H

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <entry/shutdown_sequence.h>

// GCC defines `linux` as a macro in its GNU dialects, which would turn the enumerator
// of that name below into a syntax error. The project sets CMAKE_CXX_EXTENSIONS OFF so
// it is not defined in this build; undefining it keeps that from being a property of
// the compiler flags.
#ifdef linux
#undef linux
#endif

namespace sesh_ai::ui {

	// ---------------------------------------------------------------------------
	// The bridge payload
	// ---------------------------------------------------------------------------

	// Already-serialized text on its way across the CEF bridge, in either direction.
	//
	// A distinct type rather than a `std::string`, so that the one thing requirement
	// 15.4 forbids is not expressible. See the file comment; the short version is that
	// there is exactly one way in and it takes text.
	class SerializedBridgePayload {
	public:
		SerializedBridgePayload() = default;

		explicit SerializedBridgePayload(std::string serialized_json)
			: serialized_json_{std::move(serialized_json)}
		{
		}

		// Every pointer except a character pointer, refused by name.
		//
		// Without this, `SerializedBridgePayload{media_track}` would already fail —
		// nothing converts a REAPER handle to a `std::string`. Declaring it and deleting
		// it makes the diagnostic point at the line that tried, and closes the door on a
		// future conversion making the expression compile again by accident.
		//
		// Character pointers are exempt because a string literal is serialized text
		// already; a REAPER handle is not text at all.
		template <
			typename OpaqueHandleType,
			typename = std::enable_if_t<
				!std::is_same_v<std::remove_cv_t<OpaqueHandleType>, char>
			>
		>
		explicit SerializedBridgePayload(OpaqueHandleType*) = delete;

		const std::string& serialized_json() const noexcept
		{
			return serialized_json_;
		}

		bool empty() const noexcept
		{
			return serialized_json_.empty();
		}

		std::size_t size() const noexcept
		{
			return serialized_json_.size();
		}

	private:
		std::string serialized_json_;
	};

	// True only for the types the bridge can carry. Stated as a name so the suite and a
	// reviewer can ask the question directly.
	template <typename ValueType>
	inline constexpr bool is_bridge_transmittable_v =
		std::is_constructible_v<SerializedBridgePayload, ValueType>;

	// One message across the bridge: a name the other side routes on, and a payload.
	class BridgeMessage {
	public:
		BridgeMessage() = default;

		BridgeMessage(std::string message_name, SerializedBridgePayload payload)
			: message_name_{std::move(message_name)}, payload_{std::move(payload)}
		{
		}

		// The same deletion as the payload's, one level out, so
		// `BridgeMessage{"view:agent_response", media_track}` names itself.
		template <
			typename OpaqueHandleType,
			typename = std::enable_if_t<
				!std::is_same_v<std::remove_cv_t<OpaqueHandleType>, char>
			>
		>
		BridgeMessage(std::string, OpaqueHandleType*) = delete;

		const std::string& message_name() const noexcept
		{
			return message_name_;
		}

		const SerializedBridgePayload& payload() const noexcept
		{
			return payload_;
		}

	private:
		std::string message_name_;
		SerializedBridgePayload payload_;
	};

	// The readable way to build one. Text in, text out, and nothing else fits.
	inline BridgeMessage bridge_message(std::string message_name, std::string serialized_json)
	{
		return BridgeMessage{
			std::move(message_name),
			SerializedBridgePayload{std::move(serialized_json)}
		};
	}

	// And the same refusal for the factory, so the convenient spelling is no weaker
	// than the explicit one.
	template <
		typename OpaqueHandleType,
		typename = std::enable_if_t<
			!std::is_same_v<std::remove_cv_t<OpaqueHandleType>, char>
		>
	>
	BridgeMessage bridge_message(std::string, OpaqueHandleType*) = delete;

	// ---------------------------------------------------------------------------
	// The bridge message names (design "The CEF bridge contract")
	// ---------------------------------------------------------------------------
	//
	// Ten names, declared once, because the two halves of the bridge are written in
	// different languages and compiled by different toolchains. Nothing links them: a
	// C++ side publishing `view:connection_status` to a TypeScript side listening for
	// `view:connection_state` produces no error anywhere — the panel simply never
	// shows a connection state, and there is no failing build and no log line saying
	// why. So the names are data here, and `ui/dist` reads them from the generated
	// bundle rather than spelling its own.
	//
	// The set is split by what the payload is, and the two halves are governed
	// differently:
	//
	//   - **Protocol payloads, verbatim.** The name *is* the envelope type and the
	//     payload is the one `messages/*.schema.json` already describes. Four of them:
	//     the ReaScript download routed straight through (requirement 5.7), and the
	//     three outbound types the UI originates.
	//   - **View models.** Already decoded by the C++ side, carrying fields no
	//     protocol schema has, and described by `bridge-messages/*.schema.json`. Six
	//     of them.
	//
	// The `view:` namespace is not one of the seven `envelope.schema.json` enumerates,
	// and that is load-bearing rather than decorative: a view model name can never be
	// mistaken for an envelope type, and a view model accidentally pushed onto the
	// outbound queue is refused by the envelope schema rather than sent to a server
	// with no handler for it. The static assertions below hold both halves to that.

	// Every field crossing this bridge is camelCase, matching the protocol schemas.
	// C++ members stay snake_case and the serialiser translates — so
	// `PendingConfirmationView::action_summary` is published as `actionSummary`, which
	// is also the case `confirmation-request.schema.json`'s own `action_summary` is
	// converted *into* on the way through. Stated as a constant so the serialisers and
	// the suite can name the rule rather than each restating it.
	inline constexpr std::string_view bridge_field_naming_convention{"camelCase"};

	// The namespace every view model name carries.
	inline constexpr std::string_view view_bridge_message_namespace{"view:"};

	// ---- Protocol payloads, verbatim -----------------------------------------

	// The generated ReaScript download (requirement 5.7, ADR 0011). The one inbound
	// envelope with no view model: the Message Dispatcher routes it here and the
	// payload crosses untouched, because there is nothing for the C++ side to decide.
	// A download, never an execution.
	inline constexpr std::string_view script_download_bridge_message_name{"script:download"};

	// The producer's prompt (requirement 15.2). `messages/state-prompt.schema.json`,
	// whose `promptText` is the field the standalone Bedrock Guardrail's `dataPath`
	// resolves to — which is why the UI originates the payload rather than handing
	// over bare text for the C++ to frame.
	inline constexpr std::string_view producer_prompt_bridge_message_name{"state:prompt"};

	// The producer's answer to a confirmation (requirements 13.2, 13.3). Two names and
	// one schema — `messages/confirm-decision.schema.json` serves both, and its
	// `decision` must agree with the name, which is the same rule the server applies to
	// the envelope.
	inline constexpr std::string_view confirmation_approval_bridge_message_name{"confirm:approve"};
	inline constexpr std::string_view confirmation_rejection_bridge_message_name{"confirm:reject"};

	// ---- View models ----------------------------------------------------------

	// `PendingConfirmationView` (requirement 13.1).
	inline constexpr std::string_view pending_confirmation_bridge_message_name{
		"view:confirmation_pending"
	};

	// `ConfirmationResolutionView` (requirements 13.5, 23.7).
	inline constexpr std::string_view confirmation_resolution_bridge_message_name{
		"view:confirmation_resolved"
	};

	// `StreamPresenter`'s `ResponseView` (requirement 16.1).
	inline constexpr std::string_view agent_response_bridge_message_name{"view:agent_response"};

	// `StreamIngestDisplay` (requirements 16.2, 16.3). Carries a credential.
	inline constexpr std::string_view stream_ingest_bridge_message_name{"view:stream_ingest"};

	// `StreamErrorView` (requirements 5.5, 23.2). Also where the seven `error:*`
	// envelope types land, which have no bundled payload schema of their own.
	inline constexpr std::string_view stream_error_bridge_message_name{"view:stream_error"};

	// `TransportClient`'s `connection_status` (requirement 3.4).
	inline constexpr std::string_view connection_state_bridge_message_name{
		"view:connection_state"
	};

	// ---- The sets, by direction ----------------------------------------------

	enum class BridgeMessageDirection {
		// C++ → JavaScript. Published from the timer tick, through
		// `UiHost::publish_to_javascript` and nowhere else (requirement 2.3).
		to_javascript,

		// JavaScript → C++. Arrives through `UiHost::receive_from_javascript` and goes
		// on to the Main-Thread Dispatcher.
		from_javascript
	};

	inline constexpr std::array<std::string_view, 7> bridge_messages_to_javascript{
		script_download_bridge_message_name,
		pending_confirmation_bridge_message_name,
		confirmation_resolution_bridge_message_name,
		agent_response_bridge_message_name,
		stream_ingest_bridge_message_name,
		stream_error_bridge_message_name,
		connection_state_bridge_message_name
	};

	inline constexpr std::array<std::string_view, 3> bridge_messages_from_javascript{
		producer_prompt_bridge_message_name,
		confirmation_approval_bridge_message_name,
		confirmation_rejection_bridge_message_name
	};

	// Both halves, for a test or a UI that wants to enumerate the contract.
	inline constexpr std::array<std::string_view, 10> all_bridge_message_names{
		script_download_bridge_message_name,
		producer_prompt_bridge_message_name,
		confirmation_approval_bridge_message_name,
		confirmation_rejection_bridge_message_name,
		pending_confirmation_bridge_message_name,
		confirmation_resolution_bridge_message_name,
		agent_response_bridge_message_name,
		stream_ingest_bridge_message_name,
		stream_error_bridge_message_name,
		connection_state_bridge_message_name
	};

	// ---- Classification -------------------------------------------------------

	// True when the name carries the `view:` namespace, which is exactly the view
	// model half of the set.
	constexpr bool is_view_model_bridge_message(std::string_view message_name)
	{
		return message_name.size() > view_bridge_message_namespace.size()
			&& message_name.substr(0, view_bridge_message_namespace.size())
				== view_bridge_message_namespace;
	}

	// Which direction a name travels. Empty for a name that is not in the set at all —
	// which is a message neither side should be sending, and the honest answer rather
	// than a default direction that would let one through.
	constexpr std::optional<BridgeMessageDirection> bridge_message_direction_for(
		std::string_view message_name)
	{
		for (const std::string_view candidate : bridge_messages_to_javascript) {
			if (candidate == message_name) {
				return BridgeMessageDirection::to_javascript;
			}
		}

		for (const std::string_view candidate : bridge_messages_from_javascript) {
			if (candidate == message_name) {
				return BridgeMessageDirection::from_javascript;
			}
		}

		return std::nullopt;
	}

	constexpr bool is_bridge_message_name(std::string_view message_name)
	{
		return bridge_message_direction_for(message_name).has_value();
	}

	// The schema a bridge message's payload is described by, relative to the vendored
	// bundle. Empty for a name outside the set.
	//
	// Carried here rather than left to each serialiser to work out, for the same reason
	// `ToolCallResponse::result_schema_path` crosses its seam as data: a payload plus a
	// guess at its schema is how a framework partial ends up validated against a tool's
	// own output schema. `transport/message_dispatcher.h` describes that failure in
	// full. Here the mapping is static, so stating it once is enough.
	constexpr std::string_view bridge_message_schema_path_for(std::string_view message_name)
	{
		if (message_name == script_download_bridge_message_name) {
			return "messages/script-download.schema.json";
		}

		if (message_name == producer_prompt_bridge_message_name) {
			return "messages/state-prompt.schema.json";
		}

		// One schema, both decisions. See the constants above.
		if (message_name == confirmation_approval_bridge_message_name
			|| message_name == confirmation_rejection_bridge_message_name) {
			return "messages/confirm-decision.schema.json";
		}

		if (message_name == pending_confirmation_bridge_message_name) {
			return "bridge-messages/confirmation-pending.schema.json";
		}

		if (message_name == confirmation_resolution_bridge_message_name) {
			return "bridge-messages/confirmation-resolved.schema.json";
		}

		if (message_name == agent_response_bridge_message_name) {
			return "bridge-messages/agent-response.schema.json";
		}

		if (message_name == stream_ingest_bridge_message_name) {
			return "bridge-messages/stream-ingest.schema.json";
		}

		if (message_name == stream_error_bridge_message_name) {
			return "bridge-messages/stream-error.schema.json";
		}

		if (message_name == connection_state_bridge_message_name) {
			return "bridge-messages/connection-state.schema.json";
		}

		return std::string_view{};
	}

	namespace detail {

		// The seven namespaces `envelope.schema.json` enumerates. A type outside them is
		// refused at the envelope rather than reaching a dispatcher with no handler for
		// it, which is what the assertions below rely on.
		inline constexpr std::array<std::string_view, 7> envelope_type_namespaces{
			"state",
			"request",
			"response",
			"confirm",
			"stream",
			"script",
			"error"
		};

		constexpr std::string_view namespace_of(std::string_view message_name)
		{
			const std::size_t separator = message_name.find(':');

			if (separator == std::string_view::npos) {
				return std::string_view{};
			}

			return message_name.substr(0, separator);
		}

		constexpr bool names_an_envelope_namespace(std::string_view message_name)
		{
			const std::string_view message_namespace = namespace_of(message_name);

			if (message_namespace.empty() || message_namespace.size() + 1 >= message_name.size()) {
				return false;
			}

			for (const std::string_view candidate : envelope_type_namespaces) {
				if (candidate == message_namespace) {
					return true;
				}
			}

			return false;
		}

	}

	// A view model can never be mistaken for an envelope type, in either direction:
	// its namespace is not one of the seven, so `envelope.schema.json` would refuse it.
	static_assert(
		!detail::names_an_envelope_namespace(pending_confirmation_bridge_message_name)
			&& !detail::names_an_envelope_namespace(confirmation_resolution_bridge_message_name)
			&& !detail::names_an_envelope_namespace(agent_response_bridge_message_name)
			&& !detail::names_an_envelope_namespace(stream_ingest_bridge_message_name)
			&& !detail::names_an_envelope_namespace(stream_error_bridge_message_name)
			&& !detail::names_an_envelope_namespace(connection_state_bridge_message_name),
		"a view model bridge message must not name an envelope namespace"
	);

	// And the verbatim half is the other way round: each one is a real envelope type,
	// because that is what makes forwarding the payload untouched correct.
	static_assert(
		detail::names_an_envelope_namespace(script_download_bridge_message_name)
			&& detail::names_an_envelope_namespace(producer_prompt_bridge_message_name)
			&& detail::names_an_envelope_namespace(confirmation_approval_bridge_message_name)
			&& detail::names_an_envelope_namespace(confirmation_rejection_bridge_message_name),
		"a verbatim bridge message must name a real envelope type"
	);

	// The two sets partition the whole, so neither a name in both nor a name the
	// classifier cannot place is representable.
	static_assert(
		bridge_messages_to_javascript.size() + bridge_messages_from_javascript.size()
			== all_bridge_message_names.size(),
		"every bridge message name belongs to exactly one direction"
	);

	// ---------------------------------------------------------------------------
	// Delivering a payload into the page
	// ---------------------------------------------------------------------------

	// Wraps `value` as a JavaScript single-quoted string literal, escaped.
	//
	// CEF's own browser-to-renderer message channel needs a `CefRenderProcessHandler`,
	// which lives in the helper executable — a target that does not exist yet — so the
	// adapter delivers a payload by evaluating a call in the frame. That puts the
	// payload inside JavaScript source, and the payload carries agent-authored
	// conversation text: a quote, a backslash, a newline, or a `</script>` in it would
	// end the literal early. That is an injection, not a formatting bug, so the escaping
	// lives here where the suite can hold it to the invariants rather than in the
	// adapter where nothing can reach it.
	//
	// Escaped: the backslash and both quote characters, every control character, `<` so
	// that no `</` can appear in the output, and U+2028 and U+2029, which JavaScript
	// treats as line terminators inside a string literal.
	inline std::string to_javascript_string_literal(std::string_view value)
	{
		static constexpr char hexadecimal_digits[] = "0123456789ABCDEF";

		std::string literal;
		literal.reserve(value.size() + 2);
		literal.push_back('\'');

		for (std::size_t index = 0; index < value.size(); ++index) {
			const char character = value[index];
			const auto code = static_cast<unsigned char>(character);

			// The UTF-8 encodings of U+2028 and U+2029. Passed through as-is they are
			// valid UTF-8 and invalid JavaScript.
			if (code == 0xe2 && index + 2 < value.size()
				&& static_cast<unsigned char>(value[index + 1]) == 0x80
				&& (static_cast<unsigned char>(value[index + 2]) == 0xa8
					|| static_cast<unsigned char>(value[index + 2]) == 0xa9)) {
				literal.append(
					static_cast<unsigned char>(value[index + 2]) == 0xa8
						? "\\u2028"
						: "\\u2029"
				);
				index += 2;
				continue;
			}

			switch (character) {
			case '\\':
				literal.append("\\\\");
				continue;
			case '\'':
				literal.append("\\'");
				continue;
			case '"':
				literal.append("\\\"");
				continue;
			case '\n':
				literal.append("\\n");
				continue;
			case '\r':
				literal.append("\\r");
				continue;
			case '\t':
				literal.append("\\t");
				continue;
			case '<':
				// `\x3C` is `<`. Escaping it is what makes `</script>` harmless.
				literal.append("\\x3C");
				continue;
			default:
				break;
			}

			if (code < 0x20 || code == 0x7f) {
				literal.append("\\x");
				literal.push_back(hexadecimal_digits[(code >> 4) & 0x0f]);
				literal.push_back(hexadecimal_digits[code & 0x0f]);
				continue;
			}

			literal.push_back(character);
		}

		literal.push_back('\'');

		return literal;
	}

	// ---------------------------------------------------------------------------
	// URL classification
	// ---------------------------------------------------------------------------

	inline constexpr std::string_view local_asset_url_scheme{"file://"};
	inline constexpr std::string_view secure_remote_url_scheme{"https://"};
	inline constexpr std::string_view plaintext_remote_url_scheme{"http://"};

	// The file Vite emits as the application's entry point.
	inline constexpr std::string_view default_application_entry_point{"index.html"};

	enum class UrlSchemeKind {
		// `file://` — the Vite-built assets on disk.
		local_asset,

		// `https://` — the Cognito managed login page, and anything it loads.
		secure_remote,

		// `http://` — refused everywhere in this component. Requirement 26.3 is TLS on
		// every outbound connection, and the browser's only remote navigation is a
		// sign-in page carrying credentials.
		plaintext_remote,

		// No scheme, or one this component has no rule for.
		unrecognised
	};

	namespace detail {

		inline bool has_prefix(std::string_view value, std::string_view prefix)
		{
			return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
		}

		// Control characters only. A space is legitimate in a filesystem path — macOS
		// puts REAPER's resource directory under "Application Support" — so it is
		// percent-encoded rather than refused.
		inline bool contains_control_character(std::string_view value)
		{
			for (const char character : value) {
				const auto code = static_cast<unsigned char>(character);

				if (code < 0x20 || code == 0x7f) {
					return true;
				}
			}

			return false;
		}

		// Whitespace and control characters. A URL has no business carrying either, and
		// a URL with a space in it is the shape a concatenation bug produces.
		inline bool contains_whitespace_or_control_character(std::string_view value)
		{
			for (const char character : value) {
				const auto code = static_cast<unsigned char>(character);

				if (code <= 0x20 || code == 0x7f) {
					return true;
				}
			}

			return false;
		}

		inline bool contains_scheme_separator(std::string_view value)
		{
			return value.find("://") != std::string_view::npos;
		}

		inline bool is_drive_letter_absolute_path(std::string_view value)
		{
			if (value.size() < 3 || value[1] != ':') {
				return false;
			}

			const char drive_letter = value.front();
			const bool is_letter = (drive_letter >= 'A' && drive_letter <= 'Z')
				|| (drive_letter >= 'a' && drive_letter <= 'z');

			return is_letter && (value[2] == '/' || value[2] == '\\');
		}

		inline bool is_absolute_path(std::string_view value)
		{
			return !value.empty()
				&& (value.front() == '/' || value.front() == '\\'
					|| is_drive_letter_absolute_path(value));
		}

		// A `..` *segment*, not the two characters anywhere: `my..take.wav` is a
		// legitimate file name and refusing it would be a bug of its own.
		inline bool contains_parent_directory_segment(std::string_view value)
		{
			std::size_t segment_start = 0;

			while (true) {
				std::size_t segment_end = segment_start;

				while (segment_end < value.size()
					&& value[segment_end] != '/' && value[segment_end] != '\\') {
					++segment_end;
				}

				if (value.substr(segment_start, segment_end - segment_start) == "..") {
					return true;
				}

				if (segment_end >= value.size()) {
					return false;
				}

				segment_start = segment_end + 1;
			}
		}

		inline std::string normalise_path_separators(std::string_view value)
		{
			std::string normalised{value};

			for (char& character : normalised) {
				if (character == '\\') {
					character = '/';
				}
			}

			return normalised;
		}

		// The three characters that change what a `file://` URL means rather than being
		// part of the path. A percent sign is deliberately left alone: encoding it would
		// double-encode a path that already carries an escape.
		inline std::string percent_encode_url_path(std::string_view path)
		{
			std::string encoded;
			encoded.reserve(path.size());

			for (const char character : path) {
				switch (character) {
				case ' ':
					encoded.append("%20");
					break;
				case '#':
					encoded.append("%23");
					break;
				case '?':
					encoded.append("%3F");
					break;
				default:
					encoded.push_back(character);
					break;
				}
			}

			return encoded;
		}

		// Everything between the scheme and the first `/`, `?`, or `#`.
		inline std::string_view host_of(std::string_view url, std::string_view scheme)
		{
			std::string_view remainder = url.substr(scheme.size());
			const std::size_t host_end = remainder.find_first_of("/?#");

			if (host_end == std::string_view::npos) {
				return remainder;
			}

			return remainder.substr(0, host_end);
		}

	}

	inline UrlSchemeKind classify_url_scheme(std::string_view url)
	{
		if (detail::has_prefix(url, local_asset_url_scheme)) {
			return UrlSchemeKind::local_asset;
		}

		if (detail::has_prefix(url, secure_remote_url_scheme)) {
			return UrlSchemeKind::secure_remote;
		}

		if (detail::has_prefix(url, plaintext_remote_url_scheme)) {
			return UrlSchemeKind::plaintext_remote;
		}

		return UrlSchemeKind::unrecognised;
	}

	// ---------------------------------------------------------------------------
	// The two URL types, and their validating factories
	// ---------------------------------------------------------------------------

	class LocalAssetUrl;
	class RemoteNavigationUrl;
	struct LocalAssetUrlResult;
	struct RemoteNavigationUrlResult;

	LocalAssetUrlResult build_local_asset_url(
		std::string_view asset_directory,
		std::string_view relative_asset_path
	);

	RemoteNavigationUrlResult build_sign_in_navigation_url(std::string_view sign_in_url);

	// A `file://` URL over the Vite-built assets, and only that.
	//
	// The constructor is private and `build_local_asset_url` is its only friend, so a
	// value of this type has been through the validation. There is no conversion to
	// `RemoteNavigationUrl` and no way to build one from a remote URL.
	class LocalAssetUrl {
	public:
		LocalAssetUrl() = default;

		const std::string& value() const noexcept
		{
			return url_;
		}

		bool empty() const noexcept
		{
			return url_.empty();
		}

	private:
		friend LocalAssetUrlResult build_local_asset_url(std::string_view, std::string_view);

		explicit LocalAssetUrl(std::string url)
			: url_{std::move(url)}
		{
		}

		std::string url_;
	};

	// An absolute `https` URL the browser navigates to — the Cognito managed login page.
	//
	// Same arrangement as above, in the other direction: the only way to obtain one is
	// a factory that refuses `file://` and refuses plaintext `http`.
	class RemoteNavigationUrl {
	public:
		RemoteNavigationUrl() = default;

		const std::string& value() const noexcept
		{
			return url_;
		}

		bool empty() const noexcept
		{
			return url_.empty();
		}

	private:
		friend RemoteNavigationUrlResult build_sign_in_navigation_url(std::string_view);

		explicit RemoteNavigationUrl(std::string url)
			: url_{std::move(url)}
		{
		}

		std::string url_;
	};

	struct LocalAssetUrlResult {
		bool valid = false;
		LocalAssetUrl url;

		// Empty when valid. Phrased for a log line a developer reads while the panel is
		// blank, which is the symptom of every failure here.
		std::string rejection_reason;
	};

	struct RemoteNavigationUrlResult {
		bool valid = false;
		RemoteNavigationUrl url;
		std::string rejection_reason;
	};

	// Builds the `file://` URL the CEF browser loads the React application from
	// (requirement 15.1).
	//
	// `asset_directory` is where CMake put `ui/dist` — an absolute filesystem path, not
	// a URL. `relative_asset_path` is the entry point within it.
	inline LocalAssetUrlResult build_local_asset_url(
		std::string_view asset_directory,
		std::string_view relative_asset_path)
	{
		LocalAssetUrlResult result;

		if (asset_directory.empty()) {
			result.rejection_reason = "the asset directory is empty";
			return result;
		}

		if (detail::contains_scheme_separator(asset_directory)) {
			result.rejection_reason =
				"the asset directory must be a filesystem path, not a url";
			return result;
		}

		if (!detail::is_absolute_path(asset_directory)) {
			result.rejection_reason = "the asset directory must be an absolute path";
			return result;
		}

		if (detail::contains_control_character(asset_directory)) {
			result.rejection_reason = "the asset directory contains a control character";
			return result;
		}

		if (detail::contains_parent_directory_segment(asset_directory)) {
			result.rejection_reason = "the asset directory contains a '..' segment";
			return result;
		}

		if (relative_asset_path.empty()) {
			result.rejection_reason = "the asset path is empty";
			return result;
		}

		// The one that matters most: a remote URL handed in as the "asset path" would
		// otherwise be concatenated into something that looks local and is not.
		if (detail::contains_scheme_separator(relative_asset_path)) {
			result.rejection_reason =
				"the asset path must be relative to the asset directory, not a url";
			return result;
		}

		if (detail::is_absolute_path(relative_asset_path)) {
			result.rejection_reason = "the asset path must be relative, not absolute";
			return result;
		}

		if (detail::contains_control_character(relative_asset_path)) {
			result.rejection_reason = "the asset path contains a control character";
			return result;
		}

		if (detail::contains_parent_directory_segment(relative_asset_path)) {
			result.rejection_reason = "the asset path must not escape the asset directory";
			return result;
		}

		std::string joined_path = detail::normalise_path_separators(asset_directory);

		if (joined_path.back() != '/') {
			joined_path.push_back('/');
		}

		joined_path.append(detail::normalise_path_separators(relative_asset_path));

		// `file://` plus an always-present root slash: a POSIX path keeps its own leading
		// slash, and a Windows path gains the one `file:///C:/…` needs.
		std::string_view path_without_leading_slash{joined_path};

		while (!path_without_leading_slash.empty() && path_without_leading_slash.front() == '/') {
			path_without_leading_slash.remove_prefix(1);
		}

		std::string url{local_asset_url_scheme};
		url.push_back('/');
		url.append(detail::percent_encode_url_path(path_without_leading_slash));

		result.valid = true;
		result.url = LocalAssetUrl{std::move(url)};

		return result;
	}

	// Builds the URL the browser navigates to for sign-in.
	//
	// The Cognito managed login page is a navigation, not a fetch from `file://`, and it
	// is always `https`: it carries credentials and WebAuthn platform authenticators
	// require a secure origin.
	inline RemoteNavigationUrlResult build_sign_in_navigation_url(std::string_view sign_in_url)
	{
		RemoteNavigationUrlResult result;

		if (sign_in_url.empty()) {
			result.rejection_reason = "the sign-in url is empty";
			return result;
		}

		if (detail::contains_whitespace_or_control_character(sign_in_url)) {
			result.rejection_reason = "the sign-in url contains whitespace or a control character";
			return result;
		}

		switch (classify_url_scheme(sign_in_url)) {
		case UrlSchemeKind::local_asset:
			result.rejection_reason =
				"the sign-in page is a navigation to the cognito managed login page, "
				"not a local asset";
			return result;

		case UrlSchemeKind::plaintext_remote:
			result.rejection_reason = "the sign-in url must use tls";
			return result;

		case UrlSchemeKind::unrecognised:
			// Catches a bare filesystem path too, which is the other half of the same
			// mistake: a local path must never reach the sign-in navigation.
			result.rejection_reason = "the sign-in url must be an absolute https url";
			return result;

		case UrlSchemeKind::secure_remote:
			break;
		}

		const std::string_view host = detail::host_of(sign_in_url, secure_remote_url_scheme);

		if (host.empty()) {
			result.rejection_reason = "the sign-in url has no host";
			return result;
		}

		result.valid = true;
		result.url = RemoteNavigationUrl{std::string{sign_in_url}};

		return result;
	}

	// ---------------------------------------------------------------------------
	// Resource requests
	// ---------------------------------------------------------------------------

	struct ResourceRequestVerdict {
		bool permitted = false;

		// Empty when permitted.
		std::string refusal_reason;
	};

	// Whether a page inside the browser may load a given URL.
	//
	// Both arguments are page URLs rather than serialized origins, deliberately: a
	// `file://` frame's serialized origin is opaque — Chromium reports it as `null` —
	// so the frame URL is the only thing that says which of the two pages is asking.
	//
	// Four combinations, and only two of them are legitimate:
	//
	//   local  -> local   the React application loading its own bundle. Permitted.
	//   local  -> remote  refused. Every network request the extension makes goes
	//                     through the C++ Socket.IO client on the network thread
	//                     (requirement 22.2), so a fetch from the panel is either a
	//                     mistake or something that should not be there.
	//   remote -> remote  the Cognito login page loading its own scripts. Permitted.
	//   remote -> local   refused. Nothing served from the internet reads the
	//                     producer's disk through this browser.
	inline ResourceRequestVerdict evaluate_resource_request(
		std::string_view requesting_page_url,
		std::string_view requested_url)
	{
		ResourceRequestVerdict verdict;

		const UrlSchemeKind requesting_scheme = classify_url_scheme(requesting_page_url);
		const UrlSchemeKind requested_scheme = classify_url_scheme(requested_url);

		if (requesting_scheme == UrlSchemeKind::unrecognised) {
			verdict.refusal_reason = "the requesting page url carries no recognised scheme";
			return verdict;
		}

		if (requesting_scheme == UrlSchemeKind::plaintext_remote) {
			verdict.refusal_reason = "the requesting page was loaded without tls";
			return verdict;
		}

		if (requested_scheme == UrlSchemeKind::unrecognised) {
			verdict.refusal_reason = "the requested url carries no recognised scheme";
			return verdict;
		}

		if (requesting_scheme == UrlSchemeKind::local_asset) {
			if (requested_scheme != UrlSchemeKind::local_asset) {
				verdict.refusal_reason =
					"a file:// page must not reach the network — every request the "
					"extension makes goes through the c++ socket.io client";
				return verdict;
			}

			verdict.permitted = true;
			return verdict;
		}

		if (requested_scheme == UrlSchemeKind::local_asset) {
			verdict.refusal_reason = "a remote page must not read local assets";
			return verdict;
		}

		if (requested_scheme == UrlSchemeKind::plaintext_remote) {
			verdict.refusal_reason = "a remote page must not load a resource without tls";
			return verdict;
		}

		verdict.permitted = true;

		return verdict;
	}

	// ---------------------------------------------------------------------------
	// Platform configuration (requirement 15.7)
	// ---------------------------------------------------------------------------

	enum class HostPlatform {
		macos,
		windows,
		linux
	};

	constexpr HostPlatform current_host_platform()
	{
#if defined(__APPLE__)
		return HostPlatform::macos;
#elif defined(_WIN32)
		return HostPlatform::windows;
#else
		return HostPlatform::linux;
#endif
	}

	// What CEF's multi-process model asks of each platform, as data.
	//
	// Not one field of this is logic — it is the packaging contract, stated where the
	// UI Host and the packaging step can both read it instead of each carrying its own
	// copy. The values match the distribution layout `CMakeLists.txt` records when it
	// locates CEF.
	struct CefPlatformConfiguration {
		HostPlatform platform = HostPlatform::macos;

		// Requirements 15.5 and 22.4. True on every platform, and there is no field
		// here that could turn CEF's single-process mode on: that mode is precisely the
		// arrangement in which the panel freezes with REAPER's main thread, which is
		// the thing these two requirements exist to prevent.
		bool browser_runs_in_its_own_process = true;

		// Requirement 15.7's third clause. CEF's shutdown has to complete before the
		// process exits on all three platforms, because its helper processes outlive an
		// abrupt exit and REAPER is the process that gets blamed.
		bool requires_cef_shutdown_before_process_exit = true;

		// The extension ships its own subprocess executable on every platform, and this
		// is not a macOS peculiarity. CEF's default is to re-execute the host binary with
		// process-type arguments — and the host binary here is REAPER, which the
		// extension does not own and cannot ask to become a renderer. So
		// `browser_subprocess_path` is always set, and the name is never empty.
		std::string subprocess_executable_name;

		// macOS and Linux sandbox the renderer; Windows cannot from here. The Windows
		// sandbox has to be initialized in the host executable's entry point, which
		// belongs to REAPER — so the honest value is false rather than a setting that
		// looks enabled and is not.
		bool process_sandbox_enabled = false;

		// macOS. The helper processes are separate `.app` bundles inside the extension
		// bundle and each one carries its own code signature — a single signature over
		// the outer bundle is not enough, and an unsigned helper fails to launch under
		// Gatekeeper with no diagnostic the producer can act on.
		bool helper_processes_require_individual_code_signatures = false;
		std::vector<std::string> helper_process_bundle_names;

		// macOS. Embedded in the extension bundle and loaded through `@rpath`.
		std::string framework_bundle_name;

		// Windows. The loader has to sit beside `reaper_sesh_ai.dll` in REAPER's
		// `UserPlugins` directory; a `libcef.dll` anywhere else is not found and the
		// extension fails to load rather than failing to show a panel.
		bool loader_must_sit_beside_extension_binary = false;
		std::string loader_file_name;

		// Linux. The sandbox is entered through a setuid helper binary. Without it CEF
		// either refuses to start the renderer or runs it unsandboxed, and the second
		// is worse than the first because it is silent.
		bool process_sandbox_requires_setuid_helper = false;
		std::string sandbox_helper_file_name;
	};

	inline CefPlatformConfiguration cef_platform_configuration_for(HostPlatform platform)
	{
		CefPlatformConfiguration configuration;
		configuration.platform = platform;

		switch (platform) {
		case HostPlatform::macos:
			configuration.helper_processes_require_individual_code_signatures = true;
			configuration.helper_process_bundle_names = {
				"Sesh AI Helper.app",
				"Sesh AI Helper (Alerts).app",
				"Sesh AI Helper (GPU).app",
				"Sesh AI Helper (Plugin).app",
				"Sesh AI Helper (Renderer).app"
			};
			configuration.framework_bundle_name = "Chromium Embedded Framework.framework";
			configuration.subprocess_executable_name =
				"Sesh AI Helper.app/Contents/MacOS/Sesh AI Helper";
			configuration.process_sandbox_enabled = true;
			break;

		case HostPlatform::windows:
			configuration.loader_must_sit_beside_extension_binary = true;
			configuration.loader_file_name = "libcef.dll";
			configuration.subprocess_executable_name = "sesh_ai_cef_helper.exe";
			configuration.process_sandbox_enabled = false;
			break;

		case HostPlatform::linux:
			configuration.process_sandbox_requires_setuid_helper = true;
			configuration.sandbox_helper_file_name = "chrome-sandbox";
			configuration.subprocess_executable_name = "sesh_ai_cef_helper";
			configuration.process_sandbox_enabled = true;
			break;
		}

		return configuration;
	}

	// ---------------------------------------------------------------------------
	// The process teardown order (requirement 1.2, task 17.1)
	// ---------------------------------------------------------------------------

	// Declared in `entry/shutdown_sequence.h` as of task 17.1, which is the move this
	// file's comment above anticipated. These declarations keep it a rename: the type
	// is the plugin entry's, and `sesh_ai::ui::ShutdownSequence` still names it, so
	// `UiHost::shut_down`'s signature and every caller of it read as before.
	using entry::ShutdownSequence;
	using entry::ShutdownStep;
	using entry::ShutdownViolation;

	// ---------------------------------------------------------------------------
	// The two seams
	// ---------------------------------------------------------------------------

	// What CEF looks like from this side.
	//
	// Five operations, all of them over strings. `ui/cef_browser_host.h` is the
	// implementation; the suite substitutes a recorder. There is no overload here a
	// REAPER handle fits, which is the other half of requirement 15.4's enforcement.
	class BrowserHost {
	public:
		virtual ~BrowserHost() = default;

		// Creates the browser inside REAPER's dockable panel and loads `local_asset_url`.
		// False when CEF refused — no distribution, no panel window, or a browser
		// already there.
		virtual bool create_browser(const std::string& local_asset_url) = 0;

		// Navigates the existing browser. Used for the Cognito managed login page.
		virtual bool navigate(const std::string& absolute_url) = 0;

		// Delivers one serialized message to JavaScript.
		virtual bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json
		) = 0;

		virtual void close_browser() = 0;

		// Completes CEF's shutdown. Must happen before the process exits, and after the
		// timer and the queues are gone. See ShutdownSequence.
		virtual void shut_down_cef() = 0;
	};

	// Where a message from JavaScript goes: into the Main-Thread Dispatcher's routing,
	// on the main thread, as a serialized message.
	class DispatcherSink {
	public:
		virtual ~DispatcherSink() = default;

		// False when the dispatcher refused it — an unknown message name, or a payload
		// that failed its schema.
		virtual bool accept_message_from_javascript(const BridgeMessage& message) = 0;
	};

	// ---------------------------------------------------------------------------
	// Outcomes
	// ---------------------------------------------------------------------------

	enum class BridgeRefusalReason {
		none,

		// Nothing has been created yet, so there is no browser to talk to.
		browser_not_created,

		// One browser per panel. A second create would orphan the first.
		browser_already_created,

		// CEF has been shut down. Everything after that is refused rather than
		// attempted: a post into a torn-down browser is the crash-on-quit this
		// component exists to avoid.
		cef_already_shut_down,

		// A message with no name cannot be routed by either side.
		empty_message_name,

		// Every bridge message carries a serialized JSON document. `{}` is the empty
		// one; an empty string is not JSON at all.
		empty_payload,

		// CEF itself said no.
		browser_host_refused,

		// The dispatcher said no.
		dispatcher_refused,

		// The step was not the next one the teardown order requires.
		shutdown_step_out_of_order
	};

	inline constexpr std::string_view describe(BridgeRefusalReason reason)
	{
		switch (reason) {
		case BridgeRefusalReason::none:
			return "";
		case BridgeRefusalReason::browser_not_created:
			return "the browser has not been created";
		case BridgeRefusalReason::browser_already_created:
			return "the browser has already been created";
		case BridgeRefusalReason::cef_already_shut_down:
			return "cef has already been shut down";
		case BridgeRefusalReason::empty_message_name:
			return "the message name is empty";
		case BridgeRefusalReason::empty_payload:
			return "the message payload is empty";
		case BridgeRefusalReason::browser_host_refused:
			return "cef refused the operation";
		case BridgeRefusalReason::dispatcher_refused:
			return "the dispatcher refused the message";
		case BridgeRefusalReason::shutdown_step_out_of_order:
			return "the teardown step was out of order";
		}

		return "unknown refusal";
	}

	struct CreateBrowserOutcome {
		bool created = false;

		// The `file://` URL handed to CEF, for the log line.
		std::string loaded_url;

		BridgeRefusalReason refusal = BridgeRefusalReason::none;
	};

	struct SignInNavigationOutcome {
		bool navigated = false;
		std::string navigated_url;
		BridgeRefusalReason refusal = BridgeRefusalReason::none;
	};

	struct PublishOutcome {
		bool published = false;
		std::string message_name;

		// The payload's length rather than the payload. A publish is logged on every
		// tick that has state to push, and the payload can carry conversation text.
		std::size_t serialized_byte_count = 0;

		BridgeRefusalReason refusal = BridgeRefusalReason::none;
	};

	struct ReceiveOutcome {
		bool forwarded = false;
		std::string message_name;
		std::size_t serialized_byte_count = 0;
		BridgeRefusalReason refusal = BridgeRefusalReason::none;
	};

	struct ShutdownOutcome {
		bool cef_shut_down = false;

		// A second shutdown. Reported rather than treated as a failure — the plugin
		// entry may unload after an error path already tore down.
		bool already_shut_down = false;

		BridgeRefusalReason refusal = BridgeRefusalReason::none;
	};

	// ---------------------------------------------------------------------------
	// The UI Host
	// ---------------------------------------------------------------------------

	// Bridges the CEF browser and the Main-Thread Dispatcher.
	//
	// Single-threaded and main-thread only, like the Stream Presenter: every entry point
	// is called from the timer tick that drains the queues and publishes UI state
	// (requirements 2.3, 22.3). There is no locking here because there is no second
	// thread — CEF's own threads are reached only through `BrowserHost`, whose
	// implementation is responsible for getting onto the CEF UI thread.
	class UiHost {
	public:
		UiHost(
			BrowserHost& browser_host,
			DispatcherSink& dispatcher_sink,
			CefPlatformConfiguration platform_configuration =
				cef_platform_configuration_for(current_host_platform())
		)
			: browser_host_{browser_host},
			dispatcher_sink_{dispatcher_sink},
			platform_configuration_{std::move(platform_configuration)}
		{
		}

		UiHost(const UiHost&) = delete;
		UiHost& operator=(const UiHost&) = delete;
		UiHost(UiHost&&) = delete;
		UiHost& operator=(UiHost&&) = delete;

		// Creates the browser in REAPER's dockable panel and loads the React application
		// (requirement 15.1). Takes a `LocalAssetUrl`, so a remote URL is not an
		// expressible argument.
		CreateBrowserOutcome create_browser(const LocalAssetUrl& application_entry_point)
		{
			CreateBrowserOutcome outcome;

			if (cef_shut_down_) {
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (browser_created_) {
				outcome.refusal = BridgeRefusalReason::browser_already_created;
				return outcome;
			}

			if (application_entry_point.empty()) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			if (!browser_host_.create_browser(application_entry_point.value())) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			browser_created_ = true;
			loaded_page_url_ = application_entry_point.value();
			showing_sign_in_page_ = false;

			outcome.created = true;
			outcome.loaded_url = loaded_page_url_;

			return outcome;
		}

		// Navigates to the Cognito managed login page. Takes a `RemoteNavigationUrl`, so
		// a filesystem path is not an expressible argument.
		SignInNavigationOutcome navigate_to_sign_in(const RemoteNavigationUrl& sign_in_url)
		{
			SignInNavigationOutcome outcome;

			if (cef_shut_down_) {
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (!browser_created_) {
				outcome.refusal = BridgeRefusalReason::browser_not_created;
				return outcome;
			}

			if (sign_in_url.empty()) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			if (!browser_host_.navigate(sign_in_url.value())) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			loaded_page_url_ = sign_in_url.value();
			showing_sign_in_page_ = true;

			outcome.navigated = true;
			outcome.navigated_url = loaded_page_url_;

			return outcome;
		}

		// Returns the browser to the React application after sign-in completes.
		CreateBrowserOutcome return_to_application(const LocalAssetUrl& application_entry_point)
		{
			CreateBrowserOutcome outcome;

			if (cef_shut_down_) {
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (!browser_created_) {
				outcome.refusal = BridgeRefusalReason::browser_not_created;
				return outcome;
			}

			if (application_entry_point.empty()
				|| !browser_host_.navigate(application_entry_point.value())) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			loaded_page_url_ = application_entry_point.value();
			showing_sign_in_page_ = false;

			outcome.created = true;
			outcome.loaded_url = loaded_page_url_;

			return outcome;
		}

		// Publishes UI state to JavaScript. Called from the dispatcher's publish step and
		// nowhere else (requirement 2.3).
		PublishOutcome publish_to_javascript(const BridgeMessage& message)
		{
			PublishOutcome outcome;
			outcome.message_name = message.message_name();
			outcome.serialized_byte_count = message.payload().size();

			if (cef_shut_down_) {
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (!browser_created_) {
				outcome.refusal = BridgeRefusalReason::browser_not_created;
				return outcome;
			}

			if (message.message_name().empty()) {
				outcome.refusal = BridgeRefusalReason::empty_message_name;
				return outcome;
			}

			if (message.payload().empty()) {
				outcome.refusal = BridgeRefusalReason::empty_payload;
				return outcome;
			}

			if (!browser_host_.post_message_to_javascript(
					message.message_name(),
					message.payload().serialized_json())) {
				outcome.refusal = BridgeRefusalReason::browser_host_refused;
				return outcome;
			}

			++published_message_count_;
			outcome.published = true;

			return outcome;
		}

		// A message arriving from JavaScript, on its way to the dispatcher.
		//
		// The payload is a `SerializedBridgePayload` because that is the only thing that
		// can have come from JavaScript — the parameter type is not a restriction on the
		// caller so much as a statement of what the other side is capable of sending.
		ReceiveOutcome receive_from_javascript(
			std::string message_name,
			SerializedBridgePayload payload)
		{
			ReceiveOutcome outcome;
			outcome.message_name = message_name;
			outcome.serialized_byte_count = payload.size();

			if (cef_shut_down_) {
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (!browser_created_) {
				outcome.refusal = BridgeRefusalReason::browser_not_created;
				return outcome;
			}

			if (message_name.empty()) {
				outcome.refusal = BridgeRefusalReason::empty_message_name;
				return outcome;
			}

			if (payload.empty()) {
				outcome.refusal = BridgeRefusalReason::empty_payload;
				return outcome;
			}

			const BridgeMessage message{std::move(message_name), std::move(payload)};

			if (!dispatcher_sink_.accept_message_from_javascript(message)) {
				outcome.refusal = BridgeRefusalReason::dispatcher_refused;
				return outcome;
			}

			++received_message_count_;
			outcome.forwarded = true;

			return outcome;
		}

		// This component's step in the process teardown.
		//
		// The step is claimed from the sequence before CEF is touched, so an out-of-order
		// teardown is refused while the browser is still alive rather than detected after
		// REAPER has already crashed.
		ShutdownOutcome shut_down(ShutdownSequence& sequence)
		{
			ShutdownOutcome outcome;

			if (cef_shut_down_) {
				outcome.already_shut_down = true;
				outcome.refusal = BridgeRefusalReason::cef_already_shut_down;
				return outcome;
			}

			if (!sequence.record(ShutdownStep::shut_down_cef)) {
				outcome.refusal = BridgeRefusalReason::shutdown_step_out_of_order;
				return outcome;
			}

			if (browser_created_) {
				browser_host_.close_browser();
			}

			browser_host_.shut_down_cef();

			browser_created_ = false;
			showing_sign_in_page_ = false;
			loaded_page_url_.clear();
			cef_shut_down_ = true;

			outcome.cef_shut_down = true;

			return outcome;
		}

		// Whether a page in this browser may load a URL. Delegates to the free function
		// so the rules are testable without a host.
		ResourceRequestVerdict evaluate_resource_request(std::string_view requested_url) const
		{
			return ui::evaluate_resource_request(loaded_page_url_, requested_url);
		}

		bool browser_created() const noexcept { return browser_created_; }
		bool cef_shut_down() const noexcept { return cef_shut_down_; }
		bool showing_sign_in_page() const noexcept { return showing_sign_in_page_; }
		const std::string& loaded_page_url() const noexcept { return loaded_page_url_; }
		std::size_t published_message_count() const noexcept { return published_message_count_; }
		std::size_t received_message_count() const noexcept { return received_message_count_; }

		const CefPlatformConfiguration& platform_configuration() const noexcept
		{
			return platform_configuration_;
		}

	private:
		BrowserHost& browser_host_;
		DispatcherSink& dispatcher_sink_;
		CefPlatformConfiguration platform_configuration_;

		bool browser_created_ = false;
		bool cef_shut_down_ = false;
		bool showing_sign_in_page_ = false;
		std::string loaded_page_url_;
		std::size_t published_message_count_ = 0;
		std::size_t received_message_count_ = 0;
	};

}

#endif
