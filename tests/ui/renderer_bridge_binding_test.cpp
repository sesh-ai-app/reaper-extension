// The renderer's half of the bridge, and the browser's reading of it (task 31.1,
// requirements 15.1 and 15.4).
//
// `ui/renderer_bridge_binding.h` holds every decision the two CEF-backed translation units
// make about `seshAiBridgeSend`, and neither of those units is compiled here. So the
// argument lists below are plain lists standing where CEF's `CefV8ValueList` and
// `CefListValue` stand in production. They are seams over CEF's types, not stand-ins for
// the logic: the functions under test are the ones the helper and the browser host call.
//
// Three groups, in the order of what each would cost if it were wrong.
//
// **Only the panel's own page gets the binding.** The sign-in page shares the panel's
// main frame, and a function that let it send `state:prompt` would let a remote page
// speak as the producer. Checked exhaustively over every combination of the three
// conditions and a corpus of URLs.
//
// **What JavaScript sent is what the main thread gets, byte for byte.** The prompt text
// is what the standalone Bedrock Guardrail reads, so nothing here may rewrite it. Checked
// over a seeded corpus of hostile text.
//
// **Anything that is not two strings is refused on both sides.** In the renderer as a
// thrown JavaScript error; in the browser as a message consumed and dropped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ui/renderer_bridge_binding.h>
#include <ui/ui_host.h>

namespace {

	using sesh_ai::ui::BridgeFrameDescription;
	using sesh_ai::ui::BridgeFrameEligibility;
	using sesh_ai::ui::BridgeSendArgumentProblem;
	using sesh_ai::ui::MessageFromJavascriptProblem;
	using sesh_ai::ui::MessageFromJavascriptRead;

	// One argument as either process sees it: a string, or something that is not one.
	struct ArgumentForTest {
		bool is_string = true;
		std::string text;
	};

	// The shape both `check_bridge_send_arguments` and `read_message_from_javascript`
	// read. A V8 argument list has no `string_at`; nothing here calls it on the renderer
	// side, so one type serves both.
	class ArgumentListForTest {
	public:
		ArgumentListForTest() = default;

		explicit ArgumentListForTest(std::vector<ArgumentForTest> arguments)
			: arguments_{std::move(arguments)}
		{
		}

		std::size_t size() const { return arguments_.size(); }

		bool is_string(std::size_t index) const { return arguments_[index].is_string; }

		std::string string_at(std::size_t index) const { return arguments_[index].text; }

	private:
		std::vector<ArgumentForTest> arguments_;
	};

	ArgumentListForTest two_strings(std::string message_name, std::string serialized_json)
	{
		return ArgumentListForTest{{
			ArgumentForTest{true, std::move(message_name)},
			ArgumentForTest{true, std::move(serialized_json)}
		}};
	}

	ArgumentForTest not_a_string()
	{
		return ArgumentForTest{false, std::string{}};
	}

	constexpr std::string_view application_url{
		"file:///Users/producer/Library/Application%20Support/REAPER/UserPlugins/sesh_ai_ui/index.html"
	};

	constexpr std::string_view sign_in_url{
		"https://sesh-ai.auth.us-east-1.amazoncognito.com/login?client_id=abc"
	};

	BridgeFrameDescription panel_main_frame(std::string_view url)
	{
		BridgeFrameDescription frame;
		frame.belongs_to_panel_browser = true;
		frame.is_main_frame = true;
		frame.url = url;
		return frame;
	}

	const std::string prompt_payload{
		R"({"promptText":"make the drums punchier","locale":"en-US"})"
	};

	// The xorshift source the rest of the suite uses, so a failure is reproducible from
	// the seed on any machine.
	class DeterministicBytes {
	public:
		explicit DeterministicBytes(std::uint32_t seed)
			: state_{seed == 0 ? 0x9e3779b9u : seed}
		{
		}

		std::uint32_t next()
		{
			state_ ^= state_ << 13;
			state_ ^= state_ >> 17;
			state_ ^= state_ << 5;

			return state_;
		}

		std::size_t below(std::size_t bound) { return bound == 0 ? 0 : next() % bound; }

	private:
		std::uint32_t state_;
	};

	// Everything that has broken a string on its way between two languages before: both
	// quote characters, the backslash, a NUL, line terminators including the two only
	// JavaScript treats as such, `</script>`, a lone UTF-8 continuation byte, and text
	// from the languages the panel is localised into.
	std::string generate_hostile_text(DeterministicBytes& bytes)
	{
		static const std::array<std::string_view, 16> pieces{
			"\"",
			"'",
			"\\",
			std::string_view{"\0", 1},
			"\n",
			"\r\n",
			"\xe2\x80\xa8",
			"\xe2\x80\xa9",
			"</script>",
			"\x80",
			"{\"promptText\":",
			"ドラムを強く",
			"Schlagzeug lauter",
			"🥁",
			" ",
			"state:prompt"
		};

		const std::size_t piece_count = bytes.below(24);
		std::string text;

		for (std::size_t index = 0; index < piece_count; ++index) {
			text.append(pieces[bytes.below(pieces.size())]);
		}

		return text;
	}

}

TEST_CASE("the names both processes agree on are the contract's", "[bridge][renderer]")
{
	// The TypeScript side calls this global by name (`ui/src/bridge/transport.ts`).
	CHECK(sesh_ai::ui::javascript_bridge_send_function_name == "seshAiBridgeSend");

	// The renderer's copy of the scheme cannot include `ui_host.h`, so it is held to it.
	CHECK(sesh_ai::ui::panel_page_url_scheme == sesh_ai::ui::local_asset_url_scheme);

	// The process message name is transport, not one of the ten bridge messages, so it
	// can never be mistaken for one in a log or a routing table.
	CHECK_FALSE(sesh_ai::ui::is_bridge_message_name(
		sesh_ai::ui::message_from_javascript_process_message_name
	));

	CHECK(sesh_ai::ui::bridge_send_argument_count == 2);
	CHECK(sesh_ai::ui::message_name_argument_index != sesh_ai::ui::serialized_json_argument_index);
	CHECK(sesh_ai::ui::message_name_argument_index < sesh_ai::ui::bridge_send_argument_count);
	CHECK(sesh_ai::ui::serialized_json_argument_index < sesh_ai::ui::bridge_send_argument_count);
}

TEST_CASE("only the main frame of the panel's own page gets the binding", "[bridge][renderer]")
{
	SECTION("the application, loaded from disk, is eligible")
	{
		CHECK(sesh_ai::ui::bridge_frame_eligibility(panel_main_frame(application_url))
			== BridgeFrameEligibility::eligible);
	}

	SECTION("the scheme is matched without regard to case")
	{
		CHECK(sesh_ai::ui::bridge_frame_eligibility(
			panel_main_frame("FILE:///C:/Users/producer/sesh_ai_ui/index.html"))
			== BridgeFrameEligibility::eligible);
	}

	SECTION("the sign-in page, in the same main frame, is refused")
	{
		CHECK(sesh_ai::ui::bridge_frame_eligibility(panel_main_frame(sign_in_url))
			== BridgeFrameEligibility::not_the_panel_page);
	}

	SECTION("pages that are not a file URL are refused")
	{
		for (const std::string_view url : {
			std::string_view{"http://localhost:5173/index.html"},
			std::string_view{"about:blank"},
			std::string_view{""},
			std::string_view{"file:/Users/producer/index.html"},
			std::string_view{"filesystem:file:///index.html"},
			std::string_view{"data:text/html,<script>seshAiBridgeSend()</script>"}
		}) {
			CAPTURE(url);
			CHECK(sesh_ai::ui::bridge_frame_eligibility(panel_main_frame(url))
				== BridgeFrameEligibility::not_the_panel_page);
		}
	}

	SECTION("an iframe inside the application is refused")
	{
		BridgeFrameDescription frame = panel_main_frame(application_url);
		frame.is_main_frame = false;

		CHECK(sesh_ai::ui::bridge_frame_eligibility(frame)
			== BridgeFrameEligibility::not_the_main_frame);
	}

	SECTION("a browser the extension did not mark — a popup — is refused")
	{
		BridgeFrameDescription frame = panel_main_frame(application_url);
		frame.belongs_to_panel_browser = false;

		CHECK(sesh_ai::ui::bridge_frame_eligibility(frame)
			== BridgeFrameEligibility::not_the_panel_browser);
	}
}

TEST_CASE("a frame is eligible exactly when all three conditions hold", "[bridge][renderer][property]")
{
	// **Validates: Requirements 15.1, 15.4**
	//
	// Exhaustive: every combination of the two flags, over URLs on both sides of the
	// scheme rule. Eligible if and only if the browser is the panel's, the frame is the
	// main one, and the URL is a file URL.
	const std::array<std::string_view, 6> urls{
		application_url,
		std::string_view{"file:///index.html"},
		sign_in_url,
		std::string_view{"about:blank"},
		std::string_view{""},
		std::string_view{"chrome-error://chromewebdata/"}
	};

	std::size_t eligible_count = 0;

	for (const bool belongs_to_panel_browser : {false, true}) {
		for (const bool is_main_frame : {false, true}) {
			for (const std::string_view url : urls) {
				BridgeFrameDescription frame;
				frame.belongs_to_panel_browser = belongs_to_panel_browser;
				frame.is_main_frame = is_main_frame;
				frame.url = url;

				const bool url_is_a_file_url = url.substr(0, 7) == "file://";
				const bool expected = belongs_to_panel_browser && is_main_frame && url_is_a_file_url;

				CAPTURE(belongs_to_panel_browser, is_main_frame, url);
				CHECK((sesh_ai::ui::bridge_frame_eligibility(frame)
					== BridgeFrameEligibility::eligible) == expected);

				if (expected) {
					++eligible_count;
				}
			}
		}
	}

	// A sweep in which nothing was eligible would make the check above vacuous.
	CHECK(eligible_count == 2);
}

TEST_CASE("the renderer marshals two strings and refuses anything else", "[bridge][renderer]")
{
	SECTION("two strings are accepted, whatever they say")
	{
		CHECK(sesh_ai::ui::check_bridge_send_arguments(two_strings("state:prompt", prompt_payload))
			== BridgeSendArgumentProblem::none);

		// Not the renderer's to refuse: `UiHost` refuses an empty name and payload, and
		// `ui/bridge_parsing.h` an undeclared or wrong-direction name.
		CHECK(sesh_ai::ui::check_bridge_send_arguments(two_strings("", ""))
			== BridgeSendArgumentProblem::none);
		CHECK(sesh_ai::ui::check_bridge_send_arguments(two_strings("view:agent_response", "{}"))
			== BridgeSendArgumentProblem::none);
	}

	SECTION("the wrong number of arguments is refused")
	{
		CHECK(sesh_ai::ui::check_bridge_send_arguments(ArgumentListForTest{})
			== BridgeSendArgumentProblem::wrong_argument_count);
		CHECK(sesh_ai::ui::check_bridge_send_arguments(
			ArgumentListForTest{{ArgumentForTest{true, "state:prompt"}}})
			== BridgeSendArgumentProblem::wrong_argument_count);
		CHECK(sesh_ai::ui::check_bridge_send_arguments(ArgumentListForTest{{
			ArgumentForTest{true, "state:prompt"},
			ArgumentForTest{true, prompt_payload},
			ArgumentForTest{true, "extra"}
		}}) == BridgeSendArgumentProblem::wrong_argument_count);
	}

	SECTION("an argument that is not a string is refused by position")
	{
		CHECK(sesh_ai::ui::check_bridge_send_arguments(
			ArgumentListForTest{{not_a_string(), ArgumentForTest{true, prompt_payload}}})
			== BridgeSendArgumentProblem::message_name_not_a_string);
		CHECK(sesh_ai::ui::check_bridge_send_arguments(
			ArgumentListForTest{{ArgumentForTest{true, "state:prompt"}, not_a_string()}})
			== BridgeSendArgumentProblem::serialized_json_not_a_string);
	}

	SECTION("every refusal throws an error that names the function")
	{
		CHECK(sesh_ai::ui::describe_bridge_send_argument_problem(BridgeSendArgumentProblem::none)
			.empty());

		for (const BridgeSendArgumentProblem problem : {
			BridgeSendArgumentProblem::wrong_argument_count,
			BridgeSendArgumentProblem::message_name_not_a_string,
			BridgeSendArgumentProblem::serialized_json_not_a_string
		}) {
			const std::string_view message =
				sesh_ai::ui::describe_bridge_send_argument_problem(problem);

			CHECK(message.find("seshAiBridgeSend") != std::string_view::npos);
		}

		CHECK(sesh_ai::ui::bridge_send_frame_gone_message.find("seshAiBridgeSend")
			!= std::string_view::npos);
	}
}

TEST_CASE("the browser reads a process message into a name and JSON text", "[bridge][renderer]")
{
	const std::string process_message_name{
		sesh_ai::ui::message_from_javascript_process_message_name
	};

	SECTION("a message from the panel's page is read verbatim")
	{
		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			process_message_name,
			panel_main_frame(application_url),
			two_strings("state:prompt", prompt_payload)
		);

		CHECK(read.problem == MessageFromJavascriptProblem::none);
		CHECK(read.consumed());
		CHECK(read.readable());
		CHECK(read.message_name == "state:prompt");
		CHECK(read.serialized_json == prompt_payload);
	}

	SECTION("a process message that is not the bridge's is left for someone else")
	{
		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			"some.other.message",
			panel_main_frame(application_url),
			two_strings("state:prompt", prompt_payload)
		);

		CHECK(read.problem == MessageFromJavascriptProblem::not_a_bridge_message);
		CHECK_FALSE(read.consumed());
		CHECK_FALSE(read.readable());
	}

	SECTION("a message from the sign-in page is consumed and carries nothing")
	{
		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			process_message_name,
			panel_main_frame(sign_in_url),
			two_strings("state:prompt", prompt_payload)
		);

		CHECK(read.problem == MessageFromJavascriptProblem::frame_not_eligible);
		CHECK(read.consumed());
		CHECK_FALSE(read.readable());
		CHECK(read.message_name.empty());
		CHECK(read.serialized_json.empty());
	}

	SECTION("a message from an iframe is consumed and dropped")
	{
		BridgeFrameDescription frame = panel_main_frame(application_url);
		frame.is_main_frame = false;

		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			process_message_name, frame, two_strings("state:prompt", prompt_payload));

		CHECK(read.problem == MessageFromJavascriptProblem::frame_not_eligible);
		CHECK_FALSE(read.readable());
	}

	SECTION("arguments the renderer would never write are dropped")
	{
		CHECK(sesh_ai::ui::read_message_from_javascript(
			process_message_name, panel_main_frame(application_url), ArgumentListForTest{})
			.problem == MessageFromJavascriptProblem::wrong_argument_count);

		CHECK(sesh_ai::ui::read_message_from_javascript(
			process_message_name,
			panel_main_frame(application_url),
			ArgumentListForTest{{ArgumentForTest{true, "state:prompt"}, not_a_string()}})
			.problem == MessageFromJavascriptProblem::argument_not_a_string);
	}

	SECTION("empty strings pass through for the UI Host to refuse")
	{
		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			process_message_name, panel_main_frame(application_url), two_strings("", ""));

		CHECK(read.readable());
		CHECK(read.message_name.empty());
		CHECK(read.serialized_json.empty());
	}
}

TEST_CASE("what JavaScript sent is what the main thread gets, byte for byte", "[bridge][renderer][property]")
{
	// **Validates: Requirements 15.4**
	//
	// The renderer accepts any two strings, and the browser reads back exactly the two it
	// was given. Seeded, so a failure names its own reproduction.
	DeterministicBytes bytes{0x31u};
	const std::string process_message_name{
		sesh_ai::ui::message_from_javascript_process_message_name
	};

	std::size_t text_carrying_a_nul_count = 0;

	for (int iteration = 0; iteration < 500; ++iteration) {
		const std::string message_name = generate_hostile_text(bytes);
		const std::string serialized_json = generate_hostile_text(bytes);

		if (serialized_json.find('\0') != std::string::npos) {
			++text_carrying_a_nul_count;
		}

		const ArgumentListForTest arguments = two_strings(message_name, serialized_json);

		CAPTURE(iteration);
		REQUIRE(sesh_ai::ui::check_bridge_send_arguments(arguments)
			== BridgeSendArgumentProblem::none);

		const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
			process_message_name, panel_main_frame(application_url), arguments);

		REQUIRE(read.readable());
		CHECK(read.message_name == message_name);
		CHECK(read.serialized_json == serialized_json);
	}

	// The corpus has to exercise the byte most likely to truncate a C string.
	CHECK(text_carrying_a_nul_count > 0);
}

TEST_CASE("anything that is not two strings is refused on both sides", "[bridge][renderer][property]")
{
	// **Validates: Requirements 15.4**
	//
	// Exhaustive over lists of up to four arguments, each a string or not. Exactly one
	// shape is accepted, by both the renderer and the browser.
	const std::string process_message_name{
		sesh_ai::ui::message_from_javascript_process_message_name
	};

	std::size_t shape_count = 0;
	std::size_t accepted_shape_count = 0;

	for (std::size_t argument_count = 0; argument_count <= 4; ++argument_count) {
		for (std::size_t string_mask = 0; string_mask < (std::size_t{1} << argument_count); ++string_mask) {
			std::vector<ArgumentForTest> argument_values;

			for (std::size_t index = 0; index < argument_count; ++index) {
				const bool is_string = (string_mask & (std::size_t{1} << index)) != 0;
				argument_values.push_back(ArgumentForTest{is_string, is_string ? "text" : ""});
			}

			const ArgumentListForTest arguments{argument_values};
			const bool two_strings_were_given = argument_count == 2 && string_mask == 0b11;

			const bool renderer_accepted =
				sesh_ai::ui::check_bridge_send_arguments(arguments) == BridgeSendArgumentProblem::none;
			const MessageFromJavascriptRead read = sesh_ai::ui::read_message_from_javascript(
				process_message_name, panel_main_frame(application_url), arguments);

			CAPTURE(argument_count, string_mask);
			CHECK(renderer_accepted == two_strings_were_given);
			CHECK(read.readable() == two_strings_were_given);
			CHECK(read.consumed());

			++shape_count;

			if (two_strings_were_given) {
				++accepted_shape_count;
			}
		}
	}

	CHECK(shape_count == 31);
	CHECK(accepted_shape_count == 1);
}
