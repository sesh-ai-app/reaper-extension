// The six view-model serialisers, and the JSON writer under them.
//
// Four things are worth testing here, in roughly this order of consequence.
//
// **Escaping.** The response view carries agent-authored conversation text and the
// error view carries a server message, and both end up inside a JSON document that is
// then placed inside a JavaScript string literal in a Chromium renderer. A quote or a
// backslash that survives unescaped is not a formatting bug, it is a payload that
// breaks out of its own string. So this is checked character by character, including
// the control characters that have no short escape and the multi-byte sequences that
// must pass through untouched.
//
// **Absent is omitted, never null.** Six fields across three payloads are optional on
// the C++ side, and `additionalProperties: false` plus a concrete type on every
// property means none of the six schemas admits `null`. An unset optional written as
// `null` would be refused by the schema it is supposed to satisfy, and there is no
// local schema validation in this build to catch it — the nlohmann-dependent properties
// skip. So the omission is asserted directly.
//
// **Every required field is present, under the camelCase name the schema uses.** The
// bridge's two halves are compiled by different toolchains and nothing links them, so a
// field name that drifts is a panel that quietly stops showing one thing. The cases
// read the names out of the schema files rather than restating them, which is the only
// way this assertion means anything.
//
// **The right message name.** `bridge_message_for` is overloaded on the view model's
// type, and the name comes from `ui_host.h`'s constants — so a confirmation resolution
// published under the pending confirmation's name is not expressible. Checked anyway,
// because the overload set is the thing that makes it true and an overload set is easy
// to add a wrong member to.
//
// No CEF, no REAPER, no JSON library. The writer is the point.

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ui/bridge_serialisation.h>

using sesh_ai::transport::AnsweringClient;
using sesh_ai::transport::ConfirmationResolution;
using sesh_ai::transport::ConfirmationResolutionView;
using sesh_ai::transport::ConfirmationRiskLevel;
using sesh_ai::transport::PendingConfirmationView;
using sesh_ai::transport::ProducerVerdict;
using sesh_ai::transport::connection_state;
using sesh_ai::transport::connection_status;
using sesh_ai::transport::handshake_rejection;
using sesh_ai::transport::producer_action;
using sesh_ai::ui::JsonObjectWriter;
using sesh_ai::ui::ResponseStreamState;
using sesh_ai::ui::ResponseView;
using sesh_ai::ui::StreamErrorView;
using sesh_ai::ui::StreamIngestDisplay;
using sesh_ai::ui::agent_response_bridge_message_name;
using sesh_ai::ui::bridge_message_for;
using sesh_ai::ui::confirmation_resolution_bridge_message_name;
using sesh_ai::ui::connection_state_bridge_message_name;
using sesh_ai::ui::escape_json_string;
using sesh_ai::ui::pending_confirmation_bridge_message_name;
using sesh_ai::ui::serialise_confirmation_resolution_view;
using sesh_ai::ui::serialise_connection_status;
using sesh_ai::ui::serialise_pending_confirmation_view;
using sesh_ai::ui::serialise_response_view;
using sesh_ai::ui::serialise_stream_error_view;
using sesh_ai::ui::serialise_stream_ingest_display;
using sesh_ai::ui::stream_error_bridge_message_name;
using sesh_ai::ui::stream_ingest_bridge_message_name;
using sesh_ai::ui::to_schema_string;

namespace {

	// True when `document` carries `"name":` at the top level. Enough to answer "is
	// this field present", which is all these cases ask — the values are asserted
	// separately and exactly.
	bool has_field(const std::string& document, std::string_view field_name)
	{
		std::string needle{"\""};
		needle.append(field_name);
		needle.append("\":");

		return document.find(needle) != std::string::npos;
	}

	// The field's value as written, from the colon to the next top-level comma or the
	// closing brace. Deliberately simple: these payloads are flat and the only
	// structured value is an array of integers, so there is no nesting for a naive scan
	// to get wrong — and a scanner clever enough to handle nesting would be a second
	// JSON implementation in the file testing the first.
	std::string field_value(const std::string& document, std::string_view field_name)
	{
		std::string needle{"\""};
		needle.append(field_name);
		needle.append("\":");

		const std::size_t key_at = document.find(needle);

		if (key_at == std::string::npos) {
			return {};
		}

		const std::size_t value_at = key_at + needle.size();
		bool inside_string = false;
		bool escaped = false;
		std::size_t array_depth = 0;

		for (std::size_t index = value_at; index < document.size(); ++index) {
			const char character = document[index];

			if (escaped) {
				escaped = false;
				continue;
			}

			if (inside_string) {
				if (character == '\\') {
					escaped = true;
				} else if (character == '"') {
					inside_string = false;
				}

				continue;
			}

			if (character == '"') {
				inside_string = true;
				continue;
			}

			// `missingSequences` is the one array-valued field, and its commas are not
			// field separators.
			if (character == '[') {
				++array_depth;
				continue;
			}

			if (character == ']') {
				--array_depth;
				continue;
			}

			if (array_depth > 0) {
				continue;
			}

			if (character == ',' || character == '}') {
				return document.substr(value_at, index - value_at);
			}
		}

		return document.substr(value_at);
	}

	// The `required` array of a bundled bridge schema, read from the file rather than
	// restated here.
	//
	// The whole value of this is that it is not a second copy: a schema that gains a
	// required field makes these cases fail, which is the only mechanism in this
	// repository that keeps the C++ serialiser and the generated schema in agreement.
	// The schemas are digest-verified by the build, so the file being read is the
	// bundled one.
	//
	// Scanned rather than parsed, for the reason the suite has no JSON library at all:
	// `"required"` is followed by a bracketed list of quoted names and nothing else in
	// any of the six files.
	// Where the vendored bundle is, or empty when this translation unit cannot tell.
	//
	// `MANIFEST.json` is required rather than the directory alone, and
	// `tests/transport/schema_validation_property_test.cpp` records why in full: under
	// a hand-rolled compile from the repository root, `__FILE__` is relative and the
	// walk lands on the monorepo's own `schemas/`, which holds the 42 tool *input*
	// schemas and looks enough like a bundle to be read. So the bundle is identified by
	// something only the bundle has.
	std::filesystem::path locate_schema_directory()
	{
		const std::filesystem::path schema_directory =
			std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "schemas";

		if (!std::filesystem::is_regular_file(schema_directory / "MANIFEST.json")) {
			return {};
		}

		return schema_directory;
	}

	std::vector<std::string> required_fields_of_schema(const std::string& schema_file_name)
	{
		const std::filesystem::path schema_directory = locate_schema_directory();

		if (schema_directory.empty()) {
			return {};
		}

		std::ifstream schema_file{schema_directory / "bridge-messages" / schema_file_name};
		REQUIRE(schema_file.is_open());

		std::stringstream contents;
		contents << schema_file.rdbuf();
		const std::string schema = contents.str();

		const std::size_t required_at = schema.find("\"required\"");
		REQUIRE(required_at != std::string::npos);

		const std::size_t list_opens = schema.find('[', required_at);
		const std::size_t list_closes = schema.find(']', list_opens);
		REQUIRE(list_opens != std::string::npos);
		REQUIRE(list_closes != std::string::npos);

		const std::string list = schema.substr(list_opens, list_closes - list_opens);

		std::vector<std::string> field_names;
		std::size_t index = 0;

		while (true) {
			const std::size_t name_opens = list.find('"', index);

			if (name_opens == std::string::npos) {
				break;
			}

			const std::size_t name_closes = list.find('"', name_opens + 1);

			if (name_closes == std::string::npos) {
				break;
			}

			field_names.push_back(list.substr(name_opens + 1, name_closes - name_opens - 1));
			index = name_closes + 1;
		}

		return field_names;
	}

	void require_every_required_field(
		const std::string& document,
		const std::string& schema_file_name)
	{
		const std::vector<std::string> required_fields =
			required_fields_of_schema(schema_file_name);

		// Empty only when the bundle could not be located, which is the hand-rolled
		// compile `locate_schema_directory` describes. Under the project build it is
		// always found, and the bundle is digest-verified before the suite is built —
		// so this reads the same file CI reads.
		if (required_fields.empty()) {
			WARN("the vendored schema bundle could not be located from " << __FILE__);
			return;
		}

		for (const std::string& field_name : required_fields) {
			INFO("required by " << schema_file_name << ": " << field_name);
			REQUIRE(has_field(document, field_name));
		}
	}

	PendingConfirmationView a_pending_confirmation()
	{
		PendingConfirmationView view;
		view.request_id = "confirm-7";
		view.action_summary = "Delete the track \"Kick\"";
		view.details = "One track, with 4 items on it.";
		view.risk_level = ConfirmationRiskLevel::high;
		view.risk_level_reported = true;
		view.confirmation_window_seconds = 120;
		view.description_complete = true;

		return view;
	}

	ResponseView a_streaming_response()
	{
		ResponseView view;
		view.active = true;
		view.conversation_id = "conversation-1";
		view.turn_id = "turn-3";
		view.message_id = "message-9";
		view.state = ResponseStreamState::streaming;
		view.text = "Adding a compressor.";
		view.applied_delta_count = 4;

		return view;
	}

}

TEST_CASE("the writer escapes everything a JSON string may not carry", "[bridge][serialisation]")
{
	SECTION("the two characters that would end the string or the escape")
	{
		REQUIRE(escape_json_string("a\"b") == "\"a\\\"b\"");
		REQUIRE(escape_json_string("a\\b") == "\"a\\\\b\"");
	}

	SECTION("the five with short escapes")
	{
		REQUIRE(escape_json_string("\b") == "\"\\b\"");
		REQUIRE(escape_json_string("\f") == "\"\\f\"");
		REQUIRE(escape_json_string("\n") == "\"\\n\"");
		REQUIRE(escape_json_string("\r") == "\"\\r\"");
		REQUIRE(escape_json_string("\t") == "\"\\t\"");
	}

	SECTION("every other control character, as a four-digit escape")
	{
		// The whole C0 range, so a character with no short escape cannot be passed
		// through on the strength of being unusual. 0x00 through 0x1f, minus the five
		// above.
		for (int code = 0x00; code <= 0x1f; ++code) {
			if (code == '\b' || code == '\f' || code == '\n' || code == '\r' || code == '\t') {
				continue;
			}

			const std::string value(1, static_cast<char>(code));
			const std::string escaped = escape_json_string(value);

			INFO("control character " << code);
			REQUIRE(escaped.size() == 8);
			REQUIRE(escaped.substr(0, 4) == "\"\\u0");
		}
	}

	SECTION("a closing script tag is harmless, because the quote and backslash are the only exits")
	{
		// `<` is *not* escaped here, and that is correct: this produces a JSON
		// document, and `ui_host.h`'s `to_javascript_string_literal` is what escapes
		// `<` on the next leg, where the payload enters JavaScript source. Escaping it
		// twice is what would mangle it.
		REQUIRE(escape_json_string("</script>") == "\"</script>\"");
	}

	SECTION("multi-byte UTF-8 passes through byte for byte")
	{
		// A payload is UTF-8 on both sides of the bridge. Re-encoding it here would
		// make a producer's German or Japanese prompt unreadable, and the seven locales
		// requirement 15.3 names are not an edge case.
		const std::string japanese{"\xe3\x82\xad\xe3\x83\x83\xe3\x82\xaf"};
		REQUIRE(escape_json_string(japanese) == "\"" + japanese + "\"");

		// U+2028, which `to_javascript_string_literal` escapes and this must not —
		// see the file comment on why the two functions differ.
		const std::string line_separator{"\xe2\x80\xa8"};
		REQUIRE(escape_json_string(line_separator) == "\"" + line_separator + "\"");
	}
}

TEST_CASE("the writer produces one flat object with no stray commas", "[bridge][serialisation]")
{
	SECTION("an object with nothing in it")
	{
		JsonObjectWriter writer;
		REQUIRE(writer.finish() == "{}");
	}

	SECTION("the comma goes between fields and nowhere else")
	{
		JsonObjectWriter writer;
		writer.write_string("a", "x");
		writer.write_boolean("b", true);
		writer.write_integer("c", -2);

		REQUIRE(writer.finish() == "{\"a\":\"x\",\"b\":true,\"c\":-2}");
	}

	SECTION("an omitted optional does not leave a comma behind")
	{
		// The shape that gets this wrong: a field skipped in the middle.
		JsonObjectWriter writer;
		writer.write_string("a", "x");
		writer.write_optional_string("b", std::nullopt);
		writer.write_optional_integer("c", std::nullopt);
		writer.write_boolean("d", false);

		REQUIRE(writer.finish() == "{\"a\":\"x\",\"d\":false}");
	}

	SECTION("an empty array is written, not omitted")
	{
		// `missingSequences` is required by the schema, so an empty one is `[]` rather
		// than absent — a response with no gaps still has to say so.
		JsonObjectWriter writer;
		writer.write_integer_array("missingSequences", {});

		REQUIRE(writer.finish() == "{\"missingSequences\":[]}");
	}

	SECTION("an array's elements are comma-separated and its order is kept")
	{
		JsonObjectWriter writer;
		writer.write_integer_array("missingSequences", {2, 3, 7});

		REQUIRE(writer.finish() == "{\"missingSequences\":[2,3,7]}");
	}

	SECTION("a field name is escaped the same way a value is")
	{
		// Nothing in the six payloads has a name needing it, and that is exactly why
		// it is worth holding: the writer is the thing a seventh payload would be built
		// with.
		JsonObjectWriter writer;
		writer.write_boolean("a\"b", true);

		REQUIRE(writer.finish() == "{\"a\\\"b\":true}");
	}
}

TEST_CASE("the pending confirmation carries every field its schema requires", "[bridge][serialisation]")
{
	const std::string document = serialise_pending_confirmation_view(a_pending_confirmation());

	require_every_required_field(document, "confirmation-pending.schema.json");

	SECTION("the values are the view's, in the schema's spelling")
	{
		REQUIRE(field_value(document, "requestId") == "\"confirm-7\"");
		REQUIRE(field_value(document, "riskLevel") == "\"high\"");
		REQUIRE(field_value(document, "riskLevelReported") == "true");
		REQUIRE(field_value(document, "confirmationWindowSeconds") == "120");
		REQUIRE(field_value(document, "descriptionComplete") == "true");
	}

	SECTION("the action summary's quotes are escaped rather than ending the string")
	{
		// The summary is the agent's prose and routinely names a track in quotes.
		REQUIRE(field_value(document, "actionSummary") == "\"Delete the track \\\"Kick\\\"\"");
	}

	SECTION("a conservative default is reported as one")
	{
		// `risk_level` defaults to `high` when the payload's was missing or
		// unrecognised, and `riskLevelReported` false is how the UI knows not to
		// present a guess as a fact. The two have to cross together.
		PendingConfirmationView view = a_pending_confirmation();
		view.risk_level_reported = false;
		view.description_complete = false;

		const std::string defaulted = serialise_pending_confirmation_view(view);

		REQUIRE(field_value(defaulted, "riskLevel") == "\"high\"");
		REQUIRE(field_value(defaulted, "riskLevelReported") == "false");
		REQUIRE(field_value(defaulted, "descriptionComplete") == "false");
	}

	SECTION("it is published under the pending confirmation's name")
	{
		const auto message = bridge_message_for(a_pending_confirmation());

		REQUIRE(message.message_name() == pending_confirmation_bridge_message_name);
		REQUIRE(message.payload().serialized_json() == document);
	}
}

TEST_CASE("the confirmation resolution omits what it does not know", "[bridge][serialisation]")
{
	SECTION("an expiry has no resolution answer and no answering client")
	{
		// Requirement 23.7's case. Nothing answered, so naming an answering client
		// would be inventing one — and `nothingWasChanged` is the field the producer's
		// reassurance comes from.
		ConfirmationResolutionView view;
		view.request_id = "confirm-7";
		view.resolution = ConfirmationResolution::expired;
		view.nothing_was_changed = true;

		const std::string document = serialise_confirmation_resolution_view(view);

		require_every_required_field(document, "confirmation-resolved.schema.json");

		REQUIRE(field_value(document, "resolution") == "\"expired\"");
		REQUIRE_FALSE(has_field(document, "answeringClient"));
		REQUIRE_FALSE(has_field(document, "localVerdict"));
		REQUIRE(field_value(document, "nothingWasChanged") == "true");
	}

	SECTION("a resolution this build could not read is absent rather than null")
	{
		// The prompt still comes down — it is demonstrably no longer pending — but the
		// UI is told the confirmation ended without being told how. `null` would be
		// refused by the schema, which has no null anywhere.
		ConfirmationResolutionView view;
		view.request_id = "confirm-7";

		const std::string document = serialise_confirmation_resolution_view(view);

		REQUIRE_FALSE(has_field(document, "resolution"));
		REQUIRE(document.find("null") == std::string::npos);
		REQUIRE(field_value(document, "nothingWasChanged") == "false");
	}

	SECTION("a contradicted local verdict carries both answers")
	{
		// The producer tapping Approve as the window closed. They need to hear the
		// server's outcome *and* that theirs did not take effect, which is two fields
		// and not one.
		ConfirmationResolutionView view;
		view.request_id = "confirm-7";
		view.resolution = ConfirmationResolution::expired;
		view.answered_by_this_client = true;
		view.local_verdict = ProducerVerdict::approved;
		view.contradicts_local_verdict = true;
		view.nothing_was_changed = true;

		const std::string document = serialise_confirmation_resolution_view(view);

		REQUIRE(field_value(document, "resolution") == "\"expired\"");
		REQUIRE(field_value(document, "localVerdict") == "\"approved\"");
		REQUIRE(field_value(document, "contradictsLocalVerdict") == "true");
		REQUIRE(field_value(document, "answeredByThisClient") == "true");
	}

	SECTION("the phone answering is named, so the UI need not guess whose tap it was")
	{
		ConfirmationResolutionView view;
		view.request_id = "confirm-7";
		view.resolution = ConfirmationResolution::approved;
		view.answering_client = AnsweringClient::pwa;

		const std::string document = serialise_confirmation_resolution_view(view);

		REQUIRE(field_value(document, "answeringClient") == "\"pwa\"");
		REQUIRE(field_value(document, "answeredByThisClient") == "false");
	}

	SECTION("it is published under the resolution's own name")
	{
		ConfirmationResolutionView view;
		view.request_id = "confirm-7";

		REQUIRE(bridge_message_for(view).message_name()
			== confirmation_resolution_bridge_message_name);
	}
}

TEST_CASE("the agent response reports its gaps and its text", "[bridge][serialisation]")
{
	SECTION("a streaming response with nothing missing")
	{
		const std::string document = serialise_response_view(a_streaming_response());

		require_every_required_field(document, "agent-response.schema.json");

		REQUIRE(field_value(document, "active") == "true");
		REQUIRE(field_value(document, "state") == "\"streaming\"");
		REQUIRE(field_value(document, "appliedDeltaCount") == "4");
		REQUIRE(field_value(document, "missingSequences") == "[]");
		REQUIRE(field_value(document, "textMayBeIncomplete") == "false");

		// Requirement 16.1's two optional fields, absent while the response is in
		// flight. An absent `stopReason` means still streaming and an empty string
		// would mean stopped for a reason nobody can read — the schema's own comment
		// makes that distinction, so collapsing them here would lose it.
		REQUIRE_FALSE(has_field(document, "stopReason"));
		REQUIRE_FALSE(has_field(document, "highestSequenceSeen"));
	}

	SECTION("a gap is reported in ascending order with the flag that goes with it")
	{
		ResponseView view = a_streaming_response();
		view.highest_sequence_seen = 7;
		view.missing_sequences = {2, 3, 5};
		view.text_may_be_incomplete = true;

		const std::string document = serialise_response_view(view);

		REQUIRE(field_value(document, "highestSequenceSeen") == "7");
		REQUIRE(field_value(document, "missingSequences") == "[2,3,5]");
		REQUIRE(field_value(document, "textMayBeIncomplete") == "true");
	}

	SECTION("a stopped response carries the server's reason and its assembled text")
	{
		ResponseView view = a_streaming_response();
		view.state = ResponseStreamState::ended;
		view.stop_reason = "end_turn";
		view.reconciled_with_assembled_text = true;

		const std::string document = serialise_response_view(view);

		REQUIRE(field_value(document, "state") == "\"ended\"");
		REQUIRE(field_value(document, "stopReason") == "\"end_turn\"");
		REQUIRE(field_value(document, "reconciledWithAssembledText") == "true");
	}

	SECTION("an inactive response is still a payload the schema accepts")
	{
		// The presenter answers an inactive view with every other field left at its
		// default, and that view crosses the bridge — a panel showing the previous turn
		// forever is what happens if it does not.
		const std::string document = serialise_response_view(ResponseView{});

		require_every_required_field(document, "agent-response.schema.json");

		REQUIRE(field_value(document, "active") == "false");
		REQUIRE(field_value(document, "state") == "\"idle\"");
	}

	SECTION("the three stream states have the three spellings the schema enumerates")
	{
		REQUIRE(to_schema_string(ResponseStreamState::idle) == "idle");
		REQUIRE(to_schema_string(ResponseStreamState::streaming) == "streaming");
		REQUIRE(to_schema_string(ResponseStreamState::ended) == "ended");
	}

	SECTION("conversation text with a quote, a newline, and a backslash survives")
	{
		ResponseView view = a_streaming_response();
		view.text = "Set the \"Kick\" track to -6 dB.\nPath: C:\\Renders";

		const std::string document = serialise_response_view(view);

		REQUIRE(field_value(document, "text")
			== "\"Set the \\\"Kick\\\" track to -6 dB.\\nPath: C:\\\\Renders\"");
	}

	SECTION("it is published under the agent response's name")
	{
		REQUIRE(bridge_message_for(a_streaming_response()).message_name()
			== agent_response_bridge_message_name);
	}
}

TEST_CASE("the stream ingest display carries the token the producer transcribes", "[bridge][serialisation]")
{
	StreamIngestDisplay ingest;
	ingest.stage_arn = "arn:aws:ivs:us-east-1:1:stage/abc";
	ingest.rtmps_ingest_url = "rtmps://1.global-contribute.live-video.net:443/app/";
	ingest.stream_key_plaintext = "sk_us-east-1_example_token";
	ingest.participant_id = "participant-1";
	ingest.expires_at = "2026-01-01T00:00:00Z";
	ingest.reissued = true;

	const std::string document = serialise_stream_ingest_display(ingest);

	require_every_required_field(document, "stream-ingest.schema.json");

	SECTION("the token crosses in full, because ReaCast needs it typed in by hand")
	{
		REQUIRE(field_value(document, "streamKeyPlaintext") == "\"sk_us-east-1_example_token\"");
	}

	SECTION("the expiry crosses beside it, so it is visible before it bites")
	{
		REQUIRE(field_value(document, "expiresAt") == "\"2026-01-01T00:00:00Z\"");
		REQUIRE(field_value(document, "reissued") == "true");
	}

	SECTION("it is published under the stream ingest's name")
	{
		REQUIRE(bridge_message_for(ingest).message_name() == stream_ingest_bridge_message_name);
	}
}

TEST_CASE("the stream error carries all four fields even when three are empty", "[bridge][serialisation]")
{
	// The seven `error:*` envelope types have no bundled payload schema at all, so they
	// fold into this view with whatever they carried. All four fields are required, so
	// an empty one is `""` and not an absence.
	StreamErrorView error;
	error.code = "authentication";

	const std::string document = serialise_stream_error_view(error);

	require_every_required_field(document, "stream-error.schema.json");

	REQUIRE(field_value(document, "code") == "\"authentication\"");
	REQUIRE(field_value(document, "message") == "\"\"");
	REQUIRE(field_value(document, "envelopeType") == "\"\"");
	REQUIRE(field_value(document, "validationErrors") == "\"\"");

	REQUIRE(bridge_message_for(error).message_name() == stream_error_bridge_message_name);
}

TEST_CASE("the connection state names its unit and carries no credential", "[bridge][serialisation]")
{
	SECTION("a healthy connection")
	{
		connection_status status;
		status.state = connection_state::connected;
		status.notice = "Connected to Sesh AI.";

		const std::string document = serialise_connection_status(status);

		require_every_required_field(document, "connection-state.schema.json");

		REQUIRE(field_value(document, "state") == "\"connected\"");
		REQUIRE(field_value(document, "consecutiveFailedAttempts") == "0");
		REQUIRE(field_value(document, "reconnectionDelayMilliseconds") == "0");
		REQUIRE(field_value(document, "requiredProducerAction") == "\"none\"");

		// Absent until a handshake has been refused. For diagnosis rather than
		// display, so the UI gets nothing to render rather than a value meaning
		// "nothing went wrong".
		REQUIRE_FALSE(has_field(document, "lastRejection"));
	}

	SECTION("the delay crosses in milliseconds, which is what the field name says")
	{
		// A `std::chrono::milliseconds` arriving in TypeScript as a bare number has no
		// unit at all, so `count()` is the only conversion and the name carries the
		// unit. A seconds-based reading of 30000 would show a half-minute outage as
		// eight hours.
		connection_status status;
		status.state = connection_state::reconnecting;
		status.consecutive_failed_attempts = 7;
		status.reconnection_delay = std::chrono::milliseconds{30000};
		status.last_rejection = handshake_rejection::network_or_timeout;

		const std::string document = serialise_connection_status(status);

		REQUIRE(field_value(document, "reconnectionDelayMilliseconds") == "30000");
		REQUIRE(field_value(document, "consecutiveFailedAttempts") == "7");
		REQUIRE(field_value(document, "lastRejection") == "\"network_or_timeout\"");
	}

	SECTION("the duplicate-client case names the action the producer can take")
	{
		// Requirement 3.5. `disconnected` alone does not say whether the extension is
		// about to recover on its own, which is why the action is its own field and the
		// one the UI branches on.
		connection_status status;
		status.state = connection_state::disconnected;
		status.required_producer_action = producer_action::close_the_other_reaper_instance;
		status.last_rejection = handshake_rejection::duplicate_client_type;
		status.notice = "Another REAPER instance is already signed in.";

		const std::string document = serialise_connection_status(status);

		REQUIRE(field_value(document, "state") == "\"disconnected\"");
		REQUIRE(field_value(document, "requiredProducerAction")
			== "\"close_the_other_reaper_instance\"");
		REQUIRE(field_value(document, "lastRejection") == "\"duplicate_client_type\"");
	}

	SECTION("no field the schema forbids, which is every field there is no member for")
	{
		// `additionalProperties: false` plus requirement 26.1 is the real assertion:
		// there is no token, no expiry, and no part of one on `connection_status`, so
		// there is none here. Checked by counting fields, because a new member on the
		// struct is exactly how one would arrive.
		connection_status status;
		status.last_rejection = handshake_rejection::invalid_or_expired_token;

		const std::string document = serialise_connection_status(status);

		std::size_t field_count = 0;

		for (const std::string& field_name : std::array<std::string, 6>{
				"state",
				"consecutiveFailedAttempts",
				"reconnectionDelayMilliseconds",
				"requiredProducerAction",
				"notice",
				"lastRejection"
			}) {
			if (has_field(document, field_name)) {
				++field_count;
			}
		}

		REQUIRE(field_count == 6);

		// Six colons at the top level means six fields and nothing else. The notice is
		// the only field whose value could contain one, and it is empty here.
		std::size_t colon_count = 0;

		for (const char character : document) {
			if (character == ':') {
				++colon_count;
			}
		}

		REQUIRE(colon_count == 6);
	}

	SECTION("it is published under the connection state's name")
	{
		REQUIRE(bridge_message_for(connection_status{}).message_name()
			== connection_state_bridge_message_name);
	}
}
