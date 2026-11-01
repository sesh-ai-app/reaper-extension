// The defensive field accessors every tool reader is built out of.
//
// This is the bottom half of task 24.1. `tool_request_readers.h` holds one reader per
// tool and `tool_call_reader.h` holds `read_tool_call`; both of them reach a JSON
// document only through the class below.
//
// ---------------------------------------------------------------------------
// Why a payload the MCP Tool Server already validated is read defensively anyway
//
// Requirement 4.4 keeps the 42 tool *input* schemas out of the vendored bundle, and the
// reasoning is sound: the MCP Tool Server validated the input against the authoritative
// copy, and a client re-validating against a vendored copy one commit behind can only
// reject something valid inside a producer's session.
//
// That is an argument against duplicating the *validation*. It is not an argument for
// assuming well-formed JSON arrived. The payload reaching a reader has crossed a
// network, a Socket.IO frame, a JSON parser, and a thread boundary, and the reader is
// the first thing in this extension that treats it as having fields at all. So every
// read here answers three questions rather than one — is the field there, is it the type
// the handler's argument expects, and is the value one the handler can act on — and an
// answer of no is recorded rather than papered over with a default.
//
// What that buys is the difference between an invalid call and undefined behaviour.
// `remove_fx` whose `fxIndex` is a string is reported; it does not remove the FX at
// index zero. `create_bus` whose `type` is misspelled is reported; it does not build a
// folder because `bus_type` happens to default to summing, which `routing_tools.h`
// already names as the failure this seam exists to prevent.
//
// ---------------------------------------------------------------------------
// What is checked, and what is deliberately not
//
// Checked: presence, JSON type, membership of a closed enumeration, that a number is
// finite, and that a colour is the `#RRGGBB` form this reader has to parse anyway.
//
// Not checked: the numeric bounds the input schemas carry — `volumeDecibels` between
// -150 and 12, `panPercent` between -100 and 100, `maximumResults` between 1 and 100.
// Those are the server's, and a bound restated here is a bound that can drift from the
// authoritative copy in the one direction requirement 4.4 warns about. A value outside
// them is still a value the handler can act on or report on; a field of the wrong type
// is not, which is where the line falls.
//
// Not checked either: that a GUID matches `mcp-tool-defs.schema.json`'s braced
// 36-character pattern. The GUIDs these tools carry are the ones this extension itself
// reported in the project context and in earlier tool results, so a pattern check here
// could only refuse an identifier this build minted. Non-empty is the useful bar: an
// empty string names nothing, and every handler already reports an identifier it cannot
// find.
//
// ---------------------------------------------------------------------------
// Why the problem is recorded rather than thrown or returned
//
// A reader returns the request type its handler takes — those signatures are
// `src/daw/tools/*.h`'s and task 24.1 does not get to change them — so there is no
// return slot for "and this is why there is no request". An exception would be reported
// by `tool_executor.h`'s catch as `reaper_api_failure`, which is a different thing that
// went wrong, and on the mutating path it would be thrown from inside an undo block.
//
// So the problem is recorded on a `ToolInputProblem` the whole read shares, and the
// caller asks afterwards. `tool_call_reader.h` is what acts on the answer: a payload
// with a recorded problem reaches the executor as no payload at all, which is the one
// case every tool family already reports — `missing_tool_input` in routing and track
// structure, `fx_input_unreadable`, `item_input_unreadable`,
// `track_state_input_unreadable`. The reader decides what is wrong; nothing here decides
// what to say about it.
//
// Header-only and templated on the document type, like `transport_handler.h` and
// `stream_presenter.h`: `tests/CMakeLists.txt` does not compile `src/`, so logic the
// suite exercises has to be reachable through a header, and the template is what keeps
// a JSON library out of this file.

#ifndef SESH_AI_TRANSPORT_TOOL_PAYLOAD_READING_H
#define SESH_AI_TRANSPORT_TOOL_PAYLOAD_READING_H

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sesh_ai::transport {

	// The first thing found wrong with a tool input, and nothing after it.
	//
	// First rather than all of them, because the answer is consumed by a single failed
	// action carrying a single reason, and because a payload with one field of the wrong
	// type usually has one mistake rather than a list — a reader that collected every
	// problem would spend the reason on the least informative of them.
	class ToolInputProblem {
	public:
		// Ignored once something has already been recorded. Callers do not have to check
		// before reading the next field, which is what lets a reader read straight
		// through a malformed payload without branching after every line.
		void record(std::string description)
		{
			if (recorded_) {
				return;
			}

			recorded_ = true;
			description_ = std::move(description);
		}

		bool anything_recorded() const { return recorded_; }

		// Empty when nothing was recorded.
		const std::string& description() const { return description_; }

	private:
		bool recorded_ = false;
		std::string description_;
	};

	// Reads one JSON object's fields, recording anything it cannot read.
	//
	// Holds the document by pointer, and a null pointer is a recorded problem on the
	// first read rather than a dereference: `daw::tool_call` carries the input as a
	// pointer the Tool Executor never dereferences, so a null one really does reach here.
	//
	// Every `required_*` returns a value even when it records a problem, because the
	// caller is building a request struct and branching after each field would make a
	// twelve-field reader unreadable. The value is the type's own zero and is never acted
	// on: `tool_call_reader.h` discards the whole payload when anything was recorded.
	template <typename JsonValue>
	class ToolInputReader {
	public:
		ToolInputReader(const JsonValue* document, ToolInputProblem& problem)
			: document_{document},
			problem_{problem}
		{
		}

		// True when there is an object here to read fields out of. Recorded as a problem
		// the first time a field is asked for, not here, so a tool with no fields at all
		// can still check the shape without reading one.
		bool is_readable_object() const { return document_ != nullptr && document_->is_object(); }

		// False for a document that is not an object, so a field on a payload that
		// arrived as an array or a string is absent rather than a dereference.
		bool has(const char* field_name) const
		{
			return is_readable_object() && document_->contains(field_name);
		}

		// --- strings ---

		std::string required_string(const char* field_name)
		{
			const std::optional<std::string> value = read_string(field_name);

			if (!value.has_value()) {
				record_missing_or_wrong_type(field_name, "a string");

				return std::string{};
			}

			return *value;
		}

		std::optional<std::string> optional_string(const char* field_name)
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const std::optional<std::string> value = read_string(field_name);

			if (!value.has_value()) {
				record_wrong_type(field_name, "a string");

				return std::nullopt;
			}

			return value;
		}

		// An identifier this extension minted and the agent handed back. Non-empty is the
		// whole check — see the file comment on why the braced pattern is not restated.
		std::string required_identifier(const char* field_name)
		{
			const std::string value = required_string(field_name);

			if (!problem_.anything_recorded() && value.empty()) {
				problem_.record(
					std::string{"\""} + field_name + "\" is an empty identifier, which names nothing");
			}

			return value;
		}

		// A required string the tool cannot do anything with when it is empty — a file to
		// import, an FX parameter to set. Separate from `required_identifier` only so the
		// reason reads like what it is rather than calling a path an identifier.
		std::string required_non_empty_string(const char* field_name)
		{
			const std::string value = required_string(field_name);

			if (!problem_.anything_recorded() && value.empty()) {
				problem_.record(
					std::string{"\""} + field_name + "\" is empty, and this tool has nothing to act on "
						"without it");
			}

			return value;
		}

		// --- numbers ---

		double required_number(const char* field_name)
		{
			const std::optional<double> value = read_number(field_name);

			if (!value.has_value()) {
				record_missing_or_wrong_type(field_name, "a finite number");

				return 0.0;
			}

			return *value;
		}

		std::optional<double> optional_number(const char* field_name)
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const std::optional<double> value = read_number(field_name);

			if (!value.has_value()) {
				record_wrong_type(field_name, "a finite number");

				return std::nullopt;
			}

			return value;
		}

		int required_integer(const char* field_name)
		{
			const std::optional<int> value = read_integer(field_name);

			if (!value.has_value()) {
				record_missing_or_wrong_type(field_name, "a whole number");

				return 0;
			}

			return *value;
		}

		std::optional<int> optional_integer(const char* field_name)
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const std::optional<int> value = read_integer(field_name);

			if (!value.has_value()) {
				record_wrong_type(field_name, "a whole number");

				return std::nullopt;
			}

			return value;
		}

		// --- booleans ---

		bool required_boolean(const char* field_name)
		{
			const std::optional<bool> value = read_boolean(field_name);

			if (!value.has_value()) {
				record_missing_or_wrong_type(field_name, "true or false");

				return false;
			}

			return *value;
		}

		std::optional<bool> optional_boolean(const char* field_name)
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const std::optional<bool> value = read_boolean(field_name);

			if (!value.has_value()) {
				record_wrong_type(field_name, "true or false");

				return std::nullopt;
			}

			return value;
		}

		// --- closed enumerations ---

		// The position of the value in `accepted`, which is how the caller maps it onto
		// its own enum without a second table of strings to keep in step.
		//
		// A value outside the enumeration is a recorded problem rather than the first
		// entry. Every one of these enumerations decides something audible — which bus
		// gets built, where a send is taken from, what a render writes — so falling back
		// to a default would perform a different operation than the one named.
		std::size_t required_enumeration(
			const char* field_name,
			std::initializer_list<std::string_view> accepted)
		{
			const std::optional<std::size_t> position = read_enumeration(field_name, accepted);

			if (!position.has_value()) {
				record_enumeration_problem(field_name, accepted);

				return 0;
			}

			return *position;
		}

		std::optional<std::size_t> optional_enumeration(
			const char* field_name,
			std::initializer_list<std::string_view> accepted)
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const std::optional<std::size_t> position = read_enumeration(field_name, accepted);

			if (!position.has_value()) {
				record_enumeration_problem(field_name, accepted);

				return std::nullopt;
			}

			return position;
		}

		// --- colours ---

		// `#RRGGBB` as the 24-bit value the tool layer holds.
		//
		// Parsed rather than passed through, so the format is checked because it has to
		// be: a hand-rolled hex parse over an unchecked string is how a colour becomes
		// whatever the first two characters happened to be.
		std::optional<int> optional_color(const char* field_name)
		{
			const std::optional<std::string> text = optional_string(field_name);

			if (!text.has_value()) {
				return std::nullopt;
			}

			const std::optional<int> color = parse_hexadecimal_color(*text);

			if (!color.has_value()) {
				problem_.record(
					std::string{"\""} + field_name
						+ "\" is not a colour of the form #RRGGBB, so there is no colour to set");

				return std::nullopt;
			}

			return color;
		}

		// --- arrays and nested objects ---

		// The array node, or null with a problem recorded. Null is also the answer for an
		// array the caller required and the payload did not carry.
		const JsonValue* required_array(const char* field_name)
		{
			const JsonValue* const array = read_array(field_name);

			if (array == nullptr) {
				record_missing_or_wrong_type(field_name, "an array");
			}

			return array;
		}

		// Null for an absent array, with no problem recorded; null with a problem recorded
		// for a field that is present and is not an array.
		const JsonValue* optional_array(const char* field_name)
		{
			if (!has(field_name)) {
				return nullptr;
			}

			const JsonValue* const array = read_array(field_name);

			if (array == nullptr) {
				record_wrong_type(field_name, "an array");
			}

			return array;
		}

		const JsonValue* optional_object(const char* field_name)
		{
			if (!has(field_name)) {
				return nullptr;
			}

			const JsonValue& field = document_->at(field_name);

			if (!field.is_object()) {
				record_wrong_type(field_name, "an object");

				return nullptr;
			}

			return &field;
		}

		// A reader over one element of an array, sharing this read's problem. Named rather
		// than constructed inline at each call site so that the sharing is not something a
		// caller can forget, which would lose the problem it recorded.
		ToolInputReader<JsonValue> reader_for(const JsonValue& element) const
		{
			return ToolInputReader<JsonValue>{&element, problem_};
		}

		// Visible to the per-tool readers so one of them can record something this class
		// has no general notion of — an inverted range, or a selector shape that is
		// neither of the two the schema allows.
		void record_problem(std::string description) { problem_.record(std::move(description)); }

		bool anything_recorded() const { return problem_.anything_recorded(); }

	private:
		std::optional<std::string> read_string(const char* field_name) const
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const JsonValue& field = document_->at(field_name);

			if (!field.is_string()) {
				return std::nullopt;
			}

			return field.template get<std::string>();
		}

		std::optional<double> read_number(const char* field_name) const
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const JsonValue& field = document_->at(field_name);

			// A boolean is a number in neither JSON nor nlohmann, but the check is worth
			// being explicit about: every number these tools read is a position, a level,
			// or a tempo, and `true` is none of them.
			if (!field.is_number()) {
				return std::nullopt;
			}

			const double value = field.template get<double>();

			// NaN and infinity reach here from a document a lenient parser accepted, and
			// every one of them would be written straight into REAPER — a position that
			// is not a number moves an item nowhere describable, and a tempo that is not
			// a number is a tempo map the producer cannot undo their way out of.
			if (!std::isfinite(value)) {
				return std::nullopt;
			}

			return value;
		}

		// A whole number, which is not the same question as "did the JSON library store
		// this as an integer".
		//
		// JSON Schema's `integer` type matches any number with a zero fractional part, so
		// `2.0` satisfies every `fxIndex` in the contract and the MCP Tool Server accepted
		// it. nlohmann stores that as a float and answers `is_number_integer()` with
		// false. Reading only the integer storage would therefore refuse a call the
		// authoritative schema allows, inside a producer's session — which is the exact
		// harm requirement 4.4 is about.
		std::optional<int> read_integer(const char* field_name) const
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const JsonValue& field = document_->at(field_name);

			if (field.is_number_integer()) {
				return within_integer_range(field.template get<long long>());
			}

			if (!field.is_number()) {
				return std::nullopt;
			}

			const double value = field.template get<double>();

			if (!std::isfinite(value) || value != std::trunc(value)) {
				return std::nullopt;
			}

			// Compared as a double before the cast, because converting a value outside
			// `long long`'s range is undefined rather than merely wrong.
			if (value < static_cast<double>(minimum_readable_integer)
				|| value > static_cast<double>(maximum_readable_integer)) {
				return std::nullopt;
			}

			return static_cast<int>(value);
		}

		// The tool layer holds every one of these as an `int` — an FX index, a chain
		// position, a result cap, a bit depth. A value outside that range is reported
		// rather than wrapped, because a wrapped index addresses a different FX.
		static std::optional<int> within_integer_range(long long value)
		{
			if (value < minimum_readable_integer || value > maximum_readable_integer) {
				return std::nullopt;
			}

			return static_cast<int>(value);
		}

		std::optional<bool> read_boolean(const char* field_name) const
		{
			if (!has(field_name)) {
				return std::nullopt;
			}

			const JsonValue& field = document_->at(field_name);

			if (!field.is_boolean()) {
				return std::nullopt;
			}

			return field.template get<bool>();
		}

		std::optional<std::size_t> read_enumeration(
			const char* field_name,
			std::initializer_list<std::string_view> accepted) const
		{
			const std::optional<std::string> value = read_string(field_name);

			if (!value.has_value()) {
				return std::nullopt;
			}

			std::size_t position = 0;

			for (const std::string_view& candidate : accepted) {
				if (candidate == *value) {
					return position;
				}

				++position;
			}

			return std::nullopt;
		}

		const JsonValue* read_array(const char* field_name) const
		{
			if (!has(field_name)) {
				return nullptr;
			}

			const JsonValue& field = document_->at(field_name);

			if (!field.is_array()) {
				return nullptr;
			}

			return &field;
		}

		void record_missing_or_wrong_type(const char* field_name, const char* expected)
		{
			if (document_ == nullptr) {
				problem_.record("this call carried no input to read");

				return;
			}

			if (!document_->is_object()) {
				problem_.record("this call's input is not an object, so it carries no fields");

				return;
			}

			if (!document_->contains(field_name)) {
				problem_.record(
					std::string{"\""} + field_name + "\" is required and the call did not carry it");

				return;
			}

			record_wrong_type(field_name, expected);
		}

		void record_wrong_type(const char* field_name, const char* expected)
		{
			problem_.record(std::string{"\""} + field_name + "\" is not " + expected);
		}

		void record_enumeration_problem(
			const char* field_name,
			std::initializer_list<std::string_view> accepted)
		{
			if (!has(field_name) || !document_->at(field_name).is_string()) {
				record_missing_or_wrong_type(field_name, "one of this tool's accepted values");

				return;
			}

			std::string accepted_list;

			for (const std::string_view& candidate : accepted) {
				if (!accepted_list.empty()) {
					accepted_list += ", ";
				}

				accepted_list.append(candidate.data(), candidate.size());
			}

			problem_.record(
				std::string{"\""} + field_name + "\" is not one of " + accepted_list);
		}

		// `#RRGGBB` only. REAPER's own stored value packs the channels the other way
		// round and carries an enabled flag with them; both of those belong to the hosts
		// that write colours, and neither belongs in a reader.
		static std::optional<int> parse_hexadecimal_color(const std::string& text)
		{
			if (text.size() != 7 || text.front() != '#') {
				return std::nullopt;
			}

			int packed = 0;

			for (std::size_t index = 1; index < text.size(); ++index) {
				const std::optional<int> digit = hexadecimal_digit_value(text[index]);

				if (!digit.has_value()) {
					return std::nullopt;
				}

				packed = (packed * 16) + *digit;
			}

			return packed;
		}

		static std::optional<int> hexadecimal_digit_value(char character)
		{
			if (character >= '0' && character <= '9') {
				return character - '0';
			}

			if (character >= 'a' && character <= 'f') {
				return (character - 'a') + 10;
			}

			if (character >= 'A' && character <= 'F') {
				return (character - 'A') + 10;
			}

			return std::nullopt;
		}

		// Spelled out rather than taken from `std::numeric_limits<int>` so the bound is
		// visible at the comparison, which is the only place it matters.
		static constexpr long long minimum_readable_integer = -2147483648LL;
		static constexpr long long maximum_readable_integer = 2147483647LL;

		const JsonValue* document_ = nullptr;
		ToolInputProblem& problem_;
	};

	// Every element of an array of identifiers, in the order the call supplied them.
	//
	// Free rather than a member, because it is the one array shape five tools share —
	// `itemGuids` on `move_items`, `split_items`, `delete_items`, and
	// `apply_fx_destructively`, and `fxIdentifiers` on `create_bus` — and nothing else
	// needs an array of scalars.
	//
	// A null array is an empty list and records nothing: whether the array was required
	// is `required_array`'s question and it has already recorded its own answer.
	template <typename JsonValue>
	std::vector<std::string> read_identifier_array(
		const JsonValue* array,
		const char* field_name,
		ToolInputReader<JsonValue>& reader)
	{
		std::vector<std::string> identifiers;

		if (array == nullptr) {
			return identifiers;
		}

		for (const JsonValue& element : *array) {
			if (!element.is_string()) {
				reader.record_problem(
					std::string{"\""} + field_name + "\" holds an entry that is not an identifier");

				return identifiers;
			}

			std::string identifier = element.template get<std::string>();

			if (identifier.empty()) {
				reader.record_problem(
					std::string{"\""} + field_name + "\" holds an empty identifier, which names nothing");

				return identifiers;
			}

			identifiers.push_back(std::move(identifier));
		}

		return identifiers;
	}

}

#endif
