// The renderer's half of the bridge, and the browser's reading of what it sends (task
// 31.1, requirements 15.1 and 15.4, ADR 0022).
//
// The React application sends the producer's prompt and confirmation decisions by
// calling `seshAiBridgeSend(messageName, serializedJson)`. That function has to exist in
// the page, and only CEF's renderer process can put it there, so the helper executable
// (`ui/cef_subprocess_helper_main.cpp`) installs it as a V8 function. Each call becomes
// one process message to the browser process, where `CefPanelBrowserHost` reads it and
// queues it for the main thread.
//
// Every decision either process makes about that is here, because neither of the two
// translation units that make them is compiled by the suite: the helper needs a CEF
// distribution and is its own program, and `ui/cef_browser_host.cpp` needs one too. What
// is left in those files is translation between CEF's types and the seams below.
//
// This header is compiled into the helper, which takes CEF's flag lists, including
// `-fno-exceptions` and `-fno-rtti` (task 26.1). So nothing here throws, nothing here
// uses `dynamic_cast` or `typeid`, and it includes nothing but the standard library. It
// deliberately does not include `ui/ui_host.h`: the helper needs none of the contract,
// and the renderer should not be the place that knows the ten message names (see
// "Validation is the browser's" below).
//
// ---------------------------------------------------------------------------
// A process message rather than CEF's message router
//
// `include/wrapper/cef_message_router.h` was read first, as the task asked. It was not
// used, for reasons that all come from what the TypeScript side already expects:
//
//   - The shape does not fit. `ui/src/bridge/transport.ts` calls a global function with
//     two strings and no return value, and reports a throw as `transport_threw`. The
//     router installs `window.cefQuery({request, onSuccess, onFailure})`, which takes one
//     request string and answers through callbacks. Using it would mean a JavaScript
//     shim that packs the name and the JSON into one string, and a second format for
//     the browser side to unpack.
//   - Nothing would use the response. The bridge is fire-and-forget in this direction:
//     what the extension concludes about a prompt or a decision comes back as a view
//     model, published from the tick (`view:confirmation_resolved`, for one). A query
//     answered on the CEF UI thread could not carry that answer anyway, because the
//     answer is decided on REAPER's main thread a tick later.
//   - It is more machinery in both processes. The browser-side router has to be fed
//     `OnBeforeBrowse`, `OnRenderProcessTerminated` and `OnBeforeClose` as well as the
//     message, and it keeps per-query state for cancellation that would never happen.
//
// So the renderer sends one `CefProcessMessage` named
// `message_from_javascript_process_message_name`, carrying exactly two strings.
//
// ---------------------------------------------------------------------------
// Which frames get the binding
//
// Only the main frame of the panel's own page. A frame is eligible when all three hold:
//
//   - **It belongs to the panel browser.** The browser process creates the panel's
//     browser with `panel_browser_marker_key` set in CEF's `extra_info`, which CEF hands
//     to the renderer's `OnBrowserCreated`. A browser created any other way — a popup
//     opened with `window.open`, whose `extra_info` comes from an `OnBeforePopup` this
//     extension does not implement — has no marker and gets no binding.
//   - **It is the main frame.** An iframe is a document the panel did not write.
//   - **Its URL is a `file://` URL.** The application is loaded from disk (requirement
//     15.1). The one remote page the panel ever shows is the Cognito managed login page,
//     a real `https` navigation of the same main frame, and that page must not be able
//     to send a prompt as the producer. A navigation creates a new V8 context, the
//     check runs again, and the sign-in page is refused on its scheme.
//
// The scheme is checked rather than the exact application URL. The browser process
// could pass the URL it loaded through `extra_info`, and the renderer could compare it
// with `CefFrame::GetURL()`. It was not done because the two strings come from
// different encoders — `build_local_asset_url` in `ui/ui_host.h` on one side and
// Chromium's URL canonicaliser on the other — and a disagreement over how a space, a
// non-ASCII character, or a Windows drive letter is spelled would leave the panel with
// no binding and nothing in any log saying why. The scheme is what Chromium itself
// already enforces: an `https` page cannot navigate a frame to `file://`, and
// `UiHost::navigate_to_sign_in` takes a `RemoteNavigationUrl`, which cannot be one.
//
// The browser process applies the same rule to every message it receives, from its own
// view of the frame, so a renderer that bound the function somewhere it should not have
// still cannot get a message through from there.
//
// ---------------------------------------------------------------------------
// Validation is the browser's
//
// The renderer marshals and does nothing else. It checks that it was given exactly two
// strings, because anything else cannot be marshalled, and refuses by throwing a
// JavaScript error, which `sendRawBridgeMessage` already reports as `transport_threw`.
// An empty string is passed on: `UiHost::receive_from_javascript` refuses it.
//
// Whether the name is one of the three that travel from JavaScript, whether the payload
// has the properties its schema requires, and whether a confirmation decision agrees
// with its message name are all `ui/bridge_parsing.h`'s, on the browser side, on
// REAPER's main thread. Checking them here as well would be a second copy of the
// contract in a process the suite cannot reach, and it would decide nothing: the renderer
// is the less trusted of the two processes, so the browser has to check regardless.
//
// ---------------------------------------------------------------------------
// What requirement 15.4 gets from this
//
// The only thing installed in the page is one function. It takes two strings, returns
// `undefined`, and holds nothing but a reference to the frame it sends through. No
// object is exposed, no value comes back, and the process message carries the two
// strings and nothing else. The renderer process has no REAPER handle to leak in the
// first place: it is a separate executable that links CEF and nothing of the
// extension's. On the browser side the two strings become a `SerializedBridgePayload`,
// which `ui/ui_host.h` makes incapable of holding a pointer.

#ifndef SESH_AI_UI_RENDERER_BRIDGE_BINDING_H
#define SESH_AI_UI_RENDERER_BRIDGE_BINDING_H

#include <cstddef>
#include <string>
#include <string_view>

namespace sesh_ai::ui {

	// ---------------------------------------------------------------------------
	// The names both processes agree on
	// ---------------------------------------------------------------------------

	// The global the React application calls. `CefPanelBrowserHost::
	// javascript_bridge_send_function()` returns this, and `ui/src/bridge/transport.ts`
	// declares it as `seshAiBridgeSend(messageName: string, serializedJson: string):
	// void`.
	inline constexpr std::string_view javascript_bridge_send_function_name{"seshAiBridgeSend"};

	// The process message one call becomes. Namespaced so it cannot collide with a name
	// CEF or a future wrapper uses for its own messages.
	inline constexpr std::string_view message_from_javascript_process_message_name{
		"sesh_ai.message_from_javascript"
	};

	// The key the browser process sets in `extra_info` when it creates the panel's
	// browser, and the renderer looks for in `OnBrowserCreated`. Its value is a boolean
	// `true`; anything else is not the marker.
	inline constexpr std::string_view panel_browser_marker_key{"sesh_ai.panel_browser"};

	// The scheme of the panel's own page. The same text as `local_asset_url_scheme` in
	// `ui/ui_host.h`, which this header cannot include; the suite asserts the two agree.
	inline constexpr std::string_view panel_page_url_scheme{"file://"};

	// The arguments of `seshAiBridgeSend`, and the positions they keep in the process
	// message. Stated once, so the renderer that writes the list and the browser that
	// reads it cannot disagree about the order.
	inline constexpr std::size_t bridge_send_argument_count = 2;
	inline constexpr std::size_t message_name_argument_index = 0;
	inline constexpr std::size_t serialized_json_argument_index = 1;

	// ---------------------------------------------------------------------------
	// Which frames get the binding
	// ---------------------------------------------------------------------------

	// What either process knows about a frame, in plain values. The renderer fills it in
	// `OnContextCreated` and the browser in `OnProcessMessageReceived`.
	struct BridgeFrameDescription {
		// True when the frame's browser carried `panel_browser_marker_key` (renderer), or
		// is the browser `CefPanelBrowserHost` created (browser).
		bool belongs_to_panel_browser = false;

		bool is_main_frame = false;

		// The frame's URL as CEF reports it.
		std::string_view url;
	};

	// Why a frame is not given the binding, or `eligible`. In the order they are checked.
	enum class BridgeFrameEligibility {
		eligible,
		not_the_panel_browser,
		not_the_main_frame,
		not_the_panel_page
	};

	namespace detail {

		constexpr char lowercase_ascii(char character)
		{
			return character >= 'A' && character <= 'Z'
				? static_cast<char>(character - 'A' + 'a')
				: character;
		}

		// A case-insensitive prefix match. Chromium reports schemes in lower case, but a
		// URL scheme is case-insensitive by definition, and refusing `FILE://` would be a
		// refusal for a reason nobody would think to look for.
		constexpr bool has_scheme(std::string_view url, std::string_view scheme)
		{
			if (url.size() < scheme.size()) {
				return false;
			}

			for (std::size_t index = 0; index < scheme.size(); ++index) {
				if (lowercase_ascii(url[index]) != scheme[index]) {
					return false;
				}
			}

			return true;
		}

	}

	constexpr BridgeFrameEligibility bridge_frame_eligibility(const BridgeFrameDescription& frame)
	{
		if (!frame.belongs_to_panel_browser) {
			return BridgeFrameEligibility::not_the_panel_browser;
		}

		if (!frame.is_main_frame) {
			return BridgeFrameEligibility::not_the_main_frame;
		}

		if (!detail::has_scheme(frame.url, panel_page_url_scheme)) {
			return BridgeFrameEligibility::not_the_panel_page;
		}

		return BridgeFrameEligibility::eligible;
	}

	// ---------------------------------------------------------------------------
	// The renderer: checking a call before marshalling it
	// ---------------------------------------------------------------------------

	enum class BridgeSendArgumentProblem {
		none,
		wrong_argument_count,
		message_name_not_a_string,
		serialized_json_not_a_string
	};

	// Whether a call to `seshAiBridgeSend` can be marshalled.
	//
	// `ArgumentList` is anything with `size()` and `is_string(index)`. In the helper it is
	// a view over CEF's `CefV8ValueList`; in the suite it is a plain list. Types only: see
	// "Validation is the browser's" in the file comment.
	template <typename ArgumentList>
	BridgeSendArgumentProblem check_bridge_send_arguments(const ArgumentList& arguments)
	{
		if (arguments.size() != bridge_send_argument_count) {
			return BridgeSendArgumentProblem::wrong_argument_count;
		}

		if (!arguments.is_string(message_name_argument_index)) {
			return BridgeSendArgumentProblem::message_name_not_a_string;
		}

		if (!arguments.is_string(serialized_json_argument_index)) {
			return BridgeSendArgumentProblem::serialized_json_not_a_string;
		}

		return BridgeSendArgumentProblem::none;
	}

	// The message of the JavaScript error a refused call throws. `transport.ts` puts it in
	// the `reason` of a `transport_threw` outcome, so it is written for a log line.
	constexpr std::string_view describe_bridge_send_argument_problem(
		BridgeSendArgumentProblem problem)
	{
		switch (problem) {
		case BridgeSendArgumentProblem::none:
			return "";
		case BridgeSendArgumentProblem::wrong_argument_count:
			return "seshAiBridgeSend takes exactly two arguments: a message name and serialized JSON";
		case BridgeSendArgumentProblem::message_name_not_a_string:
			return "seshAiBridgeSend's first argument, the message name, must be a string";
		case BridgeSendArgumentProblem::serialized_json_not_a_string:
			return "seshAiBridgeSend's second argument, the serialized JSON, must be a string";
		}

		return "";
	}

	// The error a call throws when the frame the function was installed in has gone —
	// a call kept alive across a navigation, for instance. There is nowhere to send it.
	inline constexpr std::string_view bridge_send_frame_gone_message{
		"seshAiBridgeSend was called after the page that installed it went away"
	};

	// ---------------------------------------------------------------------------
	// The browser: reading a process message from the renderer
	// ---------------------------------------------------------------------------

	enum class MessageFromJavascriptProblem {
		none,

		// Not ours. `OnProcessMessageReceived` returns false, so CEF offers the message
		// to anything else that handles messages.
		not_a_bridge_message,

		// Ours, from a frame that should not have had the binding. Dropped.
		frame_not_eligible,

		// Ours, and not the two strings the renderer writes. Dropped: something other
		// than this extension's renderer code sent it.
		wrong_argument_count,
		argument_not_a_string
	};

	struct MessageFromJavascriptRead {
		MessageFromJavascriptProblem problem = MessageFromJavascriptProblem::not_a_bridge_message;
		std::string message_name;
		std::string serialized_json;

		// Whether the message was this extension's, read or refused. CEF is told it was
		// handled exactly when this is true.
		bool consumed() const noexcept
		{
			return problem != MessageFromJavascriptProblem::not_a_bridge_message;
		}

		// Whether there is a name and a payload to queue for the main thread.
		bool readable() const noexcept
		{
			return problem == MessageFromJavascriptProblem::none;
		}
	};

	// Turns one process message into the name and JSON text the main thread hands to
	// `UiHost::receive_from_javascript`.
	//
	// `ArgumentList` is anything with `size()`, `is_string(index)`, and
	// `string_at(index)` returning a `std::string`. In `ui/cef_browser_host.cpp` it is a
	// view over CEF's `CefListValue`.
	//
	// Neither string is inspected. Empty ones go through, because `UiHost` refuses them
	// and reports why; the name and the payload are `ui/bridge_parsing.h`'s to judge.
	template <typename ArgumentList>
	MessageFromJavascriptRead read_message_from_javascript(
		std::string_view process_message_name,
		const BridgeFrameDescription& frame,
		const ArgumentList& arguments)
	{
		MessageFromJavascriptRead read;

		if (process_message_name != message_from_javascript_process_message_name) {
			read.problem = MessageFromJavascriptProblem::not_a_bridge_message;
			return read;
		}

		if (bridge_frame_eligibility(frame) != BridgeFrameEligibility::eligible) {
			read.problem = MessageFromJavascriptProblem::frame_not_eligible;
			return read;
		}

		if (arguments.size() != bridge_send_argument_count) {
			read.problem = MessageFromJavascriptProblem::wrong_argument_count;
			return read;
		}

		if (!arguments.is_string(message_name_argument_index)
			|| !arguments.is_string(serialized_json_argument_index)) {
			read.problem = MessageFromJavascriptProblem::argument_not_a_string;
			return read;
		}

		read.problem = MessageFromJavascriptProblem::none;
		read.message_name = arguments.string_at(message_name_argument_index);
		read.serialized_json = arguments.string_at(serialized_json_argument_index);

		return read;
	}

}

#endif
