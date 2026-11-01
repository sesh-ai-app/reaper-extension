// The field accessors every tool writer is built out of.
//
// This is the bottom half of task 24.2, and the mirror of `tool_payload_reading.h`.
// `tool_result_writers.h` holds one writer per tool and `tool_outcome_writer.h` holds
// `write_dispatch_outcome`; both of them build a JSON document only through the class
// below.
//
// ---------------------------------------------------------------------------
// Why the outputs are validated when the inputs are not
//
// Requirement 4.4 keeps the 42 tool *input* schemas out of the bundle because the MCP
// Tool Server already checked the input against the authoritative copy. Nothing checked
// the output. The extension is the only thing that produces these payloads, so the 42
// output schemas are bundled, requirement 4.1 validates every one on the way out, and
// that validation happens against a payload this file assembled.
//
// Which makes the class below the thing standing between a handler's result struct and a
// contract with closed objects, closed enumerations, pattern-matched GUIDs, and
// `minLength: 1` on sixteen different string fields. Three kinds of mistake are
// available and this file exists to make two of them unreachable:
//
//   - **A required field left out.** The writers spell every required field of every
//     schema, and the suite validates each one's output, so an omission is a test
//     failure rather than a payload the server rejects mid-turn.
//   - **A string the schema will not accept.** `minLength: 1` fields substitute rather
//     than emit empty — the same answer `tool_executor.h` and `undo_manager.h` already
//     give for an unnamed action target and an unnamed undo entry, and for the same
//     reason: an outcome nobody can name is still an outcome the agent needs told
//     about. `maxLength` fields truncate.
//   - **An optional field the result has data for and the writer never emits.** This is
//     the one no type can catch, and the one the task names: from the server's side, a
//     field the schema permits and the payload omits is indistinguishable from a tool
//     that failed. So "optional" here always means *optional in the result*, never
//     optional in the writer — `set_optional_*` omits a field exactly when the struct
//     has nothing to put in it, and the suite checks emitted coverage of each schema's
//     optional fields rather than only that the required ones validate.
//
// ---------------------------------------------------------------------------
// What is deliberately not done here
//
// **Array lengths are not capped.** Every `maxItems` in the output schemas is already
// enforced by the component that fills the array — `maximum_action_outcomes` in
// `tool_executor.h`, `maximum_undone_entry_description_count` in `undo_manager.h`, the
// per-tool result caps in each `*_tools.h` — and each of those caps *reports* what did
// not fit. A writer that silently dropped the overflow would undo exactly that, which is
// the silence requirements 9.5 and 23.9 both forbid. An array that arrives over its cap
// is a bug in the component that built it, and failing the outbound validation by path
// is how it gets found.
//
// **GUIDs are not checked against the braced pattern.** The GUIDs in a result came out of
// REAPER through `guidToString` by way of the hosts, and a writer is not in a position to
// mint a replacement for one that does not match. Rewriting or dropping it would report a
// different object, or none; emitting it fails the validation with the field named, which
// is the diagnosable answer.
//
// **Non-finite numbers are emitted as they arrive.** nlohmann stores NaN and infinity and
// serialises them as `null`, so a position that is not a number fails the payload's own
// validation with the field named. Substituting zero would instead report a position the
// producer's project does not have, and a render bound or an item move at that position
// is worse than a refused result.
//
// Header-only and templated on the document type, like `tool_payload_reading.h`:
// `tests/CMakeLists.txt` does not compile `src/`, so logic the suite exercises has to be
// reachable through a header.

#ifndef SESH_AI_TRANSPORT_TOOL_PAYLOAD_WRITING_H
#define SESH_AI_TRANSPORT_TOOL_PAYLOAD_WRITING_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// Schema bounds the output schemas share
	//
	// Spelled once each, because every one of them is a number that has to agree with
	// `schemas/mcp-tools/outputs/` and a disagreement is a payload the extension refuses
	// to send rather than a compile error.
	// ---------------------------------------------------------------------------

	// `maxLength` on a track name, an FX name, a parameter name, a marker name, a take
	// name, an undo description — the schemas use 512 for everything a producer named.
	inline constexpr std::size_t maximum_written_name_length = 512;

	// `maxLength` on an alias, on `formattedValue`, and on an installed FX's `vendor`.
	// Three constants at the same number rather than one, because they are three
	// independent bounds in three schemas and sharing a name would make a change to one of
	// them look like a change to all three.
	inline constexpr std::size_t maximum_written_alias_length = 256;
	inline constexpr std::size_t maximum_written_formatted_value_length = 256;
	inline constexpr std::size_t maximum_written_vendor_length = 256;

	// `maxLength` on a file path: `filePath` on both imports, `outputDirectory` and each
	// entry of `outputPaths` on `render`.
	inline constexpr std::size_t maximum_written_file_path_length = 4096;

	// What a `minLength: 1` field carries when the result's own string is empty.
	//
	// Substituted rather than omitted or emitted empty, following `tool_executor.h`'s
	// `unnamed_action_target` and `undo_manager.h`'s `unnamed_undo_entry_description`. An
	// empty string fails the schema, and dropping the field loses the only evidence the
	// object exists.
	inline constexpr std::string_view unnamed_written_fx{"unnamed FX"};
	inline constexpr std::string_view unnamed_written_fx_parameter{"unnamed parameter"};
	inline constexpr std::string_view unnamed_written_fx_identifier{"unnamed FX identifier"};
	inline constexpr std::string_view unnamed_written_file_path{"unnamed file path"};
	inline constexpr std::string_view unnamed_written_undo_entry{"unnamed undo entry"};
	inline constexpr std::string_view unnamed_written_alias{"unnamed alias"};

	// And the deliberate non-substitution, for an array whose entries are GUIDs rather than
	// names: `processedItemGuids`, `skippedItemGuids`, `deletedItemGuids`.
	//
	// Those schemas constrain each entry by the braced 38-character pattern rather than by
	// `minLength`, so there is no placeholder that satisfies them — a substituted sentence
	// fails the pattern exactly as an empty string does, while also reading as though it
	// were acceptable. The empty string is passed instead, and an entry that arrives empty
	// fails the payload's own validation naming the index, which is the diagnosable answer
	// for an identifier only REAPER can mint.
	inline constexpr std::string_view no_substitute_for_a_guid{};

	// Shortens a value to a schema's `maxLength`, substituting when it is empty and the
	// schema's `minLength` is one.
	//
	// The same function `tool_executor.h` has as `detail::within_schema_bounds`, repeated
	// rather than reached for: that one is `daw`'s and takes the two bounds in the order
	// its own fields need, and a transport header reaching into another namespace's
	// `detail` for three lines is a coupling worth not having.
	inline std::string within_written_bounds(
		std::string_view value,
		std::string_view substitute_when_empty,
		std::size_t maximum_length)
	{
		std::string bounded{value.empty() ? substitute_when_empty : value};

		if (bounded.size() > maximum_length) {
			bounded.resize(maximum_length);
		}

		return bounded;
	}

	// `#RRGGBB`, which is the exact inverse of `ToolInputReader::optional_color`'s parse.
	//
	// The `int` a result struct carries is the form that reader produced and
	// `reaper_custom_color_from` writes: the 24-bit value with REAPER's enabled flag
	// already masked off by `color_from_reaper_custom_color`. So the only work here is the
	// hex, and the channels are left in the order the rest of this codebase holds them —
	// a writer that reordered them would disagree with the reader about what a colour is.
	//
	// Spelled by hand rather than through a stream or `snprintf`: the schemas want six
	// upper-case-or-lower hexadecimal digits and nothing else, and a locale-sensitive
	// formatter is a surprising dependency for six nibbles.
	inline std::string written_color_string(int color)
	{
		static constexpr char digits[] = "0123456789ABCDEF";
		static constexpr int channel_mask = 0xFFFFFF;

		const int masked = color & channel_mask;

		std::string text{"#"};
		text.reserve(7);

		for (int shift = 20; shift >= 0; shift -= 4) {
			text += digits[(masked >> shift) & 0xF];
		}

		return text;
	}

	// What a result's optional string field reports, and empty when it reports nothing.
	//
	// Flattens an absent optional and an optional holding an empty string onto the same
	// answer, which is correct for every optional string in the 42: each one's absence
	// means "not reported" — a take name REAPER did not give, an FX vendor the scan did not
	// carry, a `formattedValue` the plugin does not provide — and an empty string would
	// fail the schema or say nothing.
	//
	// The view borrows from the optional, so it lives as long as the argument does. Every
	// caller passes it straight into a `set_optional_string` on the same line.
	inline std::string_view reported_string_view(const std::optional<std::string>& value)
	{
		return value.has_value() ? std::string_view{*value} : std::string_view{};
	}

	// Builds one JSON object, field by field.
	//
	// Holds the document by value and hands it over on `release`, so a half-built payload
	// cannot be read: every writer in `tool_result_writers.h` is a function that
	// constructs one of these, fills it, and releases it as its return value.
	//
	// `set_*` for a field the schema requires, `set_optional_*` for one it permits. The
	// pairing is deliberate and is the whole discipline of this class — the optional
	// overloads take the `std::optional`, the `std::string`, or the container the result
	// struct actually holds, so "is there anything to report" is answered by the result
	// rather than decided by the writer.
	template <typename JsonValue>
	class ToolOutputWriter {
	public:
		ToolOutputWriter()
			: document_{JsonValue::object()}
		{
		}

		// --- strings ---

		// A `maxLength`-only field. Empty is emitted as empty, which every one of these
		// schemas allows: an unnamed track in REAPER has no name, and a placeholder would
		// invent one the producer never typed.
		void set_string(
			std::string_view field_name,
			std::string_view value,
			std::size_t maximum_length)
		{
			document_[std::string{field_name}] =
				within_written_bounds(value, std::string_view{}, maximum_length);
		}

		// A `minLength: 1` field. Substitutes rather than emitting empty — see
		// `unnamed_written_fx` and its neighbours.
		void set_named_string(
			std::string_view field_name,
			std::string_view value,
			std::string_view substitute_when_empty,
			std::size_t maximum_length)
		{
			document_[std::string{field_name}] =
				within_written_bounds(value, substitute_when_empty, maximum_length);
		}

		// Omitted when the result holds nothing. Empty counts as nothing here, because
		// every optional string in these schemas is one whose absence means "not reported"
		// — a take name REAPER did not give, an FX vendor the scan did not carry.
		void set_optional_string(
			std::string_view field_name,
			std::string_view value,
			std::size_t maximum_length)
		{
			if (value.empty()) {
				return;
			}

			set_string(field_name, value, maximum_length);
		}

		// There is deliberately no overload taking a `std::optional<std::string>`, and
		// `reported_string_view` below is why: a `std::string` argument converts to both a
		// `std::string_view` and an optional, so two overloads would be ambiguous at every
		// call site that passes a plain string — which is most of them. Flattening the
		// optional at the call site keeps one method and makes the flattening visible.

		// --- numbers ---

		void set_number(std::string_view field_name, double value)
		{
			document_[std::string{field_name}] = value;
		}

		void set_optional_number(std::string_view field_name, const std::optional<double>& value)
		{
			if (value.has_value()) {
				set_number(field_name, *value);
			}
		}

		void set_integer(std::string_view field_name, int value)
		{
			document_[std::string{field_name}] = value;
		}

		void set_optional_integer(std::string_view field_name, const std::optional<int>& value)
		{
			if (value.has_value()) {
				set_integer(field_name, *value);
			}
		}

		// A count the result holds as a `std::size_t`. Narrowed here rather than at each
		// call site, because `totalInRange` and `clampedItemCount` are counts of things a
		// project can hold and nothing in this contract is wider than an `int`.
		void set_count(std::string_view field_name, std::size_t value)
		{
			document_[std::string{field_name}] = static_cast<int>(value);
		}

		// --- booleans ---

		void set_boolean(std::string_view field_name, bool value)
		{
			document_[std::string{field_name}] = value;
		}

		void set_optional_boolean(std::string_view field_name, const std::optional<bool>& value)
		{
			if (value.has_value()) {
				set_boolean(field_name, *value);
			}
		}

		// --- colours ---

		// Omitted when the object carries no colour, which is what
		// `color_from_reaper_custom_color` answers for a colour REAPER was told to ignore.
		// Showing the producer a colour they cannot see would be worse than saying nothing.
		void set_optional_color(std::string_view field_name, const std::optional<int>& color)
		{
			if (color.has_value()) {
				document_[std::string{field_name}] = written_color_string(*color);
			}
		}

		// --- nested objects and arrays ---

		void set_value(std::string_view field_name, JsonValue value)
		{
			document_[std::string{field_name}] = std::move(value);
		}

		// Omitted when the array is empty.
		//
		// For the optional arrays whose schema carries `minItems: 1` — `render`'s
		// `tracks`, `move_all_items`' `tracks` — where an empty array is not a
		// representable answer and absence is the one the schema names.
		void set_non_empty_array(std::string_view field_name, JsonValue array)
		{
			if (array.empty()) {
				return;
			}

			document_[std::string{field_name}] = std::move(array);
		}

		// --- constants ---

		// A schema `const`. Taken as a `bool` because the only two in this contract are
		// `refused: true` on the refusal and `ok` on an action outcome, and both are
		// declared constants rather than flags by the types that carry them.
		void set_constant(std::string_view field_name, bool value)
		{
			document_[std::string{field_name}] = value;
		}

		// True when nothing has been written yet. Used by the suite's coverage check
		// rather than by a writer.
		bool is_empty() const { return document_.empty(); }

		bool has(std::string_view field_name) const
		{
			return document_.contains(std::string{field_name});
		}

		// The finished object. Moves out, so what the caller gets is what was built and
		// this writer is not usable afterwards.
		JsonValue release() { return std::move(document_); }

	private:
		JsonValue document_;
	};

	// Every element of a container, written through a per-element writer.
	//
	// Free rather than a member, because the element writers live in
	// `tool_result_writers.h` beside the shapes they write and this class deliberately
	// names no `daw` type.
	template <typename JsonValue, typename Container, typename ElementWriter>
	JsonValue write_array_of(const Container& elements, const ElementWriter& write_element)
	{
		JsonValue array = JsonValue::array();

		for (const auto& element : elements) {
			array.push_back(write_element(element));
		}

		return array;
	}

	// An array of strings, each bounded to the schema's limits.
	//
	// The one array shape five fields share — `processedItemGuids`, `skippedItemGuids`,
	// `deletedItemGuids`, `outputPaths`, `undoneEntryDescriptions` — and the substitution
	// is what keeps a `minLength: 1` entry out of an array the producer needs told about.
	template <typename JsonValue>
	JsonValue write_string_array(
		const std::vector<std::string>& values,
		std::string_view substitute_when_empty,
		std::size_t maximum_length)
	{
		JsonValue array = JsonValue::array();

		for (const std::string& value : values) {
			array.push_back(within_written_bounds(value, substitute_when_empty, maximum_length));
		}

		return array;
	}

}

#endif
