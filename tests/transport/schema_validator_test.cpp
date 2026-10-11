// Which bundled schema governs which payload.
//
// The tables in schema_validator.h are the protocol contract expressed as data, and
// the four things worth checking about them are worth checking in roughly this order
// of consequence.
//
// **No tool input is ever mapped to a schema.** This is the mistake the schema-bundle
// steering names, and the harm is specific: a vendored copy one commit behind the
// server cannot catch anything the MCP Tool Server missed, but it can reject a valid
// tool call inside a producer's session. So the check is exhaustive over all 42 tool
// names and over both directions, and it is the first case in the file.
//
// **Every path names a file the bundle actually holds.** A path that does not is a
// validator failing closed at runtime on a schema it could not find — which, for an
// outbound payload, is the extension refusing to send its own valid result. Checked
// against MANIFEST.json as a set equality in both directions, so a bundled file that
// nothing points at fails too.
//
// **A refusal selects the refusal contract and a result selects the tool's own
// schema.** The tempting third branch — `actions` means a partial outcome — is wrong,
// and wrong in a way that only shows up for two of the 42 tools. That case is here to
// keep it from being added.
//
// **The error payload fits inside the schema that describes it.** Truncation is not
// cosmetic: an error too long to describe cannot be sent, and the error is the only
// thing the server gets when an envelope is refused.
//
// No validation library here, and none needed: everything above is a question about
// the tables. Whether a real snapshot satisfies project-context.schema.json is a
// different question, needs the real validator, and is task 4.3's.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/schema_validator.h>

using sesh_ai::transport::EnvelopeSchemaBinding;
using sesh_ai::transport::ToolOutputSchemaBinding;
using sesh_ai::transport::all_bundled_schema_paths;
using sesh_ai::transport::bundled_envelope_and_message_schema_count;
using sesh_ai::transport::bundled_bridge_message_schema_count;
using sesh_ai::transport::bundled_schema_file_count;
using sesh_ai::transport::bundled_tool_output_schema_count;
using sesh_ai::transport::confirm_decision_schema_path;
using sesh_ai::transport::envelope_schema_path;
using sesh_ai::transport::inbound_envelope_schema_bindings;
using sesh_ai::transport::inbound_payload_schema_path;
using sesh_ai::transport::is_constrained_tool_name;
using sesh_ai::transport::maximum_error_envelope_type_length;
using sesh_ai::transport::maximum_rendered_validation_errors_length;
using sesh_ai::transport::outbound_envelope_schema_bindings;
using sesh_ai::transport::outbound_payload_schema_path;
using sesh_ai::transport::project_context_schema_path;
using sesh_ai::transport::render_validation_errors;
using sesh_ai::transport::tool_name_from_request_envelope_type;
using sesh_ai::transport::tool_name_from_response_envelope_type;
using sesh_ai::transport::tool_output_schema_bindings;
using sesh_ai::transport::tool_output_schema_path;
using sesh_ai::transport::tool_result_partial_contract_schema_path;
using sesh_ai::transport::tool_result_refusal_schema_path;
using sesh_ai::transport::tool_result_schema_path;
using sesh_ai::transport::tool_result_undo_contract_schema_path;
using sesh_ai::transport::truncate_to_length;

namespace {

	// Locates the vendored bundle from this file's own path.
	//
	// The test target has no compile definition naming the schema directory, and
	// adding one would mean editing tests/CMakeLists.txt, which independent pieces of
	// work are deliberately kept out of. __FILE__ is an absolute path here because
	// CMake's GLOB_RECURSE hands the compiler absolute source paths, so walking up
	// three directories from tests/transport/ reaches the repository root.
	//
	// Returns empty rather than failing when the directory is not there, and the cases
	// that use it skip. A bundle that is missing is a build failure already — the
	// verification target hashes all 65 files on every build — so a test failing here
	// would only be a second, less informative report of the same thing.
	std::filesystem::path locate_schema_directory()
	{
		std::filesystem::path schema_directory =
			std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "schemas";

		if (!std::filesystem::is_directory(schema_directory)) {
			return {};
		}

		return schema_directory;
	}

	// The `path` values MANIFEST.json lists, read without a JSON library.
	//
	// The manifest is generated with one property per line and two-tab indentation, so
	// the paths can be lifted out by looking for the `"path": "` prefix. Crude, and
	// appropriate: this suite must run on a machine with no JSON dependency installed,
	// and the alternative is not testing the tables against the manifest at all.
	std::set<std::string> read_manifest_schema_paths(const std::filesystem::path& schema_directory)
	{
		std::set<std::string> manifest_paths;

		std::ifstream manifest_file{schema_directory / "MANIFEST.json", std::ios::binary};

		if (!manifest_file.is_open()) {
			return manifest_paths;
		}

		static const std::string path_property_prefix{"\"path\": \""};

		std::string manifest_line;

		while (std::getline(manifest_file, manifest_line)) {
			const std::size_t prefix_position = manifest_line.find(path_property_prefix);

			if (prefix_position == std::string::npos) {
				continue;
			}

			const std::size_t value_position = prefix_position + path_property_prefix.size();
			const std::size_t closing_quote_position = manifest_line.find('"', value_position);

			if (closing_quote_position == std::string::npos) {
				continue;
			}

			manifest_paths.insert(
				manifest_line.substr(value_position, closing_quote_position - value_position)
			);
		}

		return manifest_paths;
	}

	bool names_a_tool_input_schema(std::string_view schema_path)
	{
		static constexpr std::string_view tool_directory_prefix{"mcp-tools/"};
		static constexpr std::string_view tool_output_directory_prefix{"mcp-tools/outputs/"};

		if (schema_path.substr(0, tool_directory_prefix.size()) != tool_directory_prefix) {
			return false;
		}

		return schema_path.substr(0, tool_output_directory_prefix.size()) != tool_output_directory_prefix;
	}

}

// Requirement 4.4. The one case in this file whose failure is a producer-visible
// regression rather than a build-time one, so it goes first.
TEST_CASE("no tool input is mapped to a bundled schema in either direction", "[schemas][validator]")
{
	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		const std::string tool_request_envelope_type = "request:" + std::string{binding.tool_name};

		// The inbound lookup is the one the codec consults for an arriving envelope.
		// Empty here is what makes a tool call dispatch without a vendored re-check.
		REQUIRE_FALSE(inbound_payload_schema_path(tool_request_envelope_type).has_value());

		// And the outbound one, because `request:<tool_name>` is not something the
		// extension sends either — a binding here would be dead and misleading.
		REQUIRE_FALSE(outbound_payload_schema_path(tool_request_envelope_type).has_value());
	}
}

TEST_CASE("no table names a file from the tool input directory", "[schemas][validator]")
{
	// The 42 input schemas live directly under mcp-tools/ and are absent from the
	// bundle, so a path naming one could not resolve. This checks the stronger thing:
	// that no such path is spelled at all, in case the bundle ever ships them for some
	// other consumer.
	for (const std::string& schema_path : all_bundled_schema_paths()) {
		REQUIRE_FALSE(names_a_tool_input_schema(schema_path));
	}
}

TEST_CASE("a bound request type is never one of the constrained tools", "[schemas][validator]")
{
	// `request:transport` and `request:project_context` are bound (the first to a
	// schema, the second to nothing), and neither is a tool. If a tool were ever named
	// `transport`, the codec's lookup order would decide which rule applied to it —
	// so the collision is ruled out here instead of being resolved there.
	for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
		REQUIRE_FALSE(tool_name_from_request_envelope_type(binding.envelope_type).has_value());
	}

	REQUIRE_FALSE(is_constrained_tool_name("transport"));
	REQUIRE_FALSE(is_constrained_tool_name("project_context"));
}

TEST_CASE("the inbound table binds nine envelope types, each exactly once", "[schemas][validator]")
{
	REQUIRE(inbound_envelope_schema_bindings.size() == 9);

	std::set<std::string_view> bound_envelope_types;

	for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
		REQUIRE(bound_envelope_types.insert(binding.envelope_type).second);
		REQUIRE_FALSE(binding.schema_path.empty());
	}

	// Spot-checks against the four inbound types other components already name, so a
	// binding renamed on one side of the codebase and not the other fails here.
	REQUIRE(inbound_payload_schema_path("request:transport")
		== std::optional<std::string_view>{"messages/transport-command.schema.json"});
	REQUIRE(inbound_payload_schema_path("confirm:request")
		== std::optional<std::string_view>{"messages/confirmation-request.schema.json"});
	REQUIRE(inbound_payload_schema_path("confirm:resolved")
		== std::optional<std::string_view>{"messages/confirm-resolved.schema.json"});
	REQUIRE(inbound_payload_schema_path("stream:agent_response_delta")
		== std::optional<std::string_view>{"messages/agent-response-delta.schema.json"});
}

TEST_CASE("an inbound type with no bundled schema reports none rather than failing", "[schemas][validator]")
{
	// Requirement 4.2 read the way it is written: validate the ones that have a
	// schema. These four arrive and are handled; none has a schema in the bundle.
	REQUIRE_FALSE(inbound_payload_schema_path("request:project_context").has_value());
	REQUIRE_FALSE(inbound_payload_schema_path("error:agent").has_value());
	REQUIRE_FALSE(inbound_payload_schema_path("error:confirmation").has_value());
	REQUIRE_FALSE(inbound_payload_schema_path("error:validation").has_value());

	// And a type no build has ever seen, which requirement 5.8 logs and ignores.
	REQUIRE_FALSE(inbound_payload_schema_path("state:something_new").has_value());
	REQUIRE_FALSE(inbound_payload_schema_path("").has_value());
}

TEST_CASE("the outbound table binds six envelope types, each exactly once", "[schemas][validator]")
{
	REQUIRE(outbound_envelope_schema_bindings.size() == 6);

	std::set<std::string_view> bound_envelope_types;

	for (const EnvelopeSchemaBinding& binding : outbound_envelope_schema_bindings) {
		REQUIRE(bound_envelope_types.insert(binding.envelope_type).second);
		REQUIRE_FALSE(binding.schema_path.empty());
	}

	// The snapshot is the same payload whether it was asked for or not.
	REQUIRE(outbound_payload_schema_path("state:project_context")
		== std::optional<std::string_view>{project_context_schema_path});
	REQUIRE(outbound_payload_schema_path("response:project_context")
		== std::optional<std::string_view>{project_context_schema_path});

	// And a confirmation decision is the same shape either way, because the envelope
	// type already carries the verdict.
	REQUIRE(outbound_payload_schema_path("confirm:approve")
		== std::optional<std::string_view>{confirm_decision_schema_path});
	REQUIRE(outbound_payload_schema_path("confirm:reject")
		== std::optional<std::string_view>{confirm_decision_schema_path});

	// Sent, and deliberately unbound: the bundle has no schema for its payload.
	REQUIRE_FALSE(outbound_payload_schema_path("stream:request_audio").has_value());
}

TEST_CASE("all 42 constrained tools have their own distinct output schema", "[schemas][validator]")
{
	REQUIRE(tool_output_schema_bindings.size() == bundled_tool_output_schema_count);

	std::set<std::string_view> tool_names;
	std::set<std::string_view> schema_paths;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		REQUIRE(tool_names.insert(binding.tool_name).second);
		REQUIRE(schema_paths.insert(binding.schema_path).second);

		// The tool name is snake_case and the file is the same name in kebab-case under
		// mcp-tools/outputs/. Checked rather than relied on, because the paths are
		// listed rather than derived and a transposition would otherwise be invisible.
		std::string expected_schema_path = "mcp-tools/outputs/" + std::string{binding.tool_name};

		std::replace(expected_schema_path.begin(), expected_schema_path.end(), '_', '-');
		expected_schema_path.append(".schema.json");

		REQUIRE(binding.schema_path == expected_schema_path);
	}
}

TEST_CASE("an unknown tool name yields no output schema rather than a guessed path", "[schemas][validator]")
{
	// Requirement 23.6's case: a tool the server knows about and this build does not.
	// It is answered with an error result naming the tool, which a derived path would
	// turn into a validator failing to open a file.
	REQUIRE_FALSE(tool_output_schema_path("summon_a_bass_player").has_value());
	REQUIRE_FALSE(tool_output_schema_path("").has_value());
	REQUIRE_FALSE(tool_output_schema_path("create-track").has_value());

	REQUIRE(tool_output_schema_path("create_track")
		== std::optional<std::string_view>{"mcp-tools/outputs/create-track.schema.json"});
}

TEST_CASE("the tool envelope type readers recognise exactly the 42", "[schemas][validator]")
{
	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		const std::string request_type = "request:" + std::string{binding.tool_name};
		const std::string response_type = "response:" + std::string{binding.tool_name};

		REQUIRE(tool_name_from_request_envelope_type(request_type)
			== std::optional<std::string_view>{binding.tool_name});
		REQUIRE(tool_name_from_response_envelope_type(response_type)
			== std::optional<std::string_view>{binding.tool_name});
	}

	// Neither prefix on its own, nor the other prefix, nor a name that is not a tool.
	REQUIRE_FALSE(tool_name_from_request_envelope_type("request:").has_value());
	REQUIRE_FALSE(tool_name_from_response_envelope_type("response:").has_value());
	REQUIRE_FALSE(tool_name_from_request_envelope_type("response:create_track").has_value());
	REQUIRE_FALSE(tool_name_from_response_envelope_type("request:create_track").has_value());
	REQUIRE_FALSE(tool_name_from_request_envelope_type("request:drop_the_beat").has_value());
	REQUIRE_FALSE(tool_name_from_request_envelope_type("create_track").has_value());
}

TEST_CASE("a refusal selects the refusal contract and a result the tool's own schema", "[schemas][validator]")
{
	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		REQUIRE(tool_result_schema_path(binding.tool_name, true)
			== std::optional<std::string_view>{tool_result_refusal_schema_path});

		REQUIRE(tool_result_schema_path(binding.tool_name, false)
			== std::optional<std::string_view>{binding.schema_path});
	}

	// A refusal for a tool this build does not implement still selects the refusal
	// contract: the payload's shape does not depend on which tool declined.
	REQUIRE(tool_result_schema_path("summon_a_bass_player", true)
		== std::optional<std::string_view>{tool_result_refusal_schema_path});

	REQUIRE_FALSE(tool_result_schema_path("summon_a_bass_player", false).has_value());
}

TEST_CASE("the partial and undo contracts are never selected for a tool result", "[schemas][validator]")
{
	// The reason is specific and easy to get wrong: `set_track_state` and
	// `set_item_properties` return an `actions` array as their ordinary result, so an
	// `actions` branch would route two tools' valid output to the partial contract,
	// whose additionalProperties:false then rejects the extra fields each requires.
	// Both contracts are hoisted into the tool output schemas by the bundler instead.
	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		for (const bool payload_is_refusal : {false, true}) {
			const std::optional<std::string_view> selected =
				tool_result_schema_path(binding.tool_name, payload_is_refusal);

			REQUIRE(selected.has_value());
			REQUIRE(*selected != tool_result_partial_contract_schema_path);
			REQUIRE(*selected != tool_result_undo_contract_schema_path);
		}
	}

	// The two tools the mistake would have affected, named so the case is about them.
	REQUIRE(tool_result_schema_path("set_track_state", false)
		== std::optional<std::string_view>{"mcp-tools/outputs/set-track-state.schema.json"});
	REQUIRE(tool_result_schema_path("set_item_properties", false)
		== std::optional<std::string_view>{"mcp-tools/outputs/set-item-properties.schema.json"});
}

TEST_CASE("the distinct schema paths come to the bundle's file count", "[schemas][validator]")
{
	const std::vector<std::string> schema_paths = all_bundled_schema_paths();

	REQUIRE(schema_paths.size() == bundled_schema_file_count);

	// Sorted and deduplicated, which is what makes the count above meaningful.
	REQUIRE(std::is_sorted(schema_paths.begin(), schema_paths.end()));
	REQUIRE(std::adjacent_find(schema_paths.begin(), schema_paths.end()) == schema_paths.end());

	// And the division the steering states: 42 tool output schemas, 6 CEF bridge view
	// models, 17 envelope and message payload schemas. The three are counted
	// separately and summed, so a file moving between categories is a failure here
	// rather than a total that still adds up.
	const auto count_paths_under = [&schema_paths](std::string_view directory_prefix) {
		return static_cast<std::size_t>(std::count_if(
			schema_paths.begin(),
			schema_paths.end(),
			[directory_prefix](const std::string& schema_path) {
				return schema_path.rfind(directory_prefix, 0) == 0;
			}
		));
	};

	const std::size_t tool_output_path_count = count_paths_under("mcp-tools/outputs/");
	const std::size_t bridge_message_path_count = count_paths_under("bridge-messages/");

	REQUIRE(tool_output_path_count == bundled_tool_output_schema_count);
	REQUIRE(bridge_message_path_count == bundled_bridge_message_schema_count);
	REQUIRE(
		schema_paths.size() - tool_output_path_count - bridge_message_path_count
			== bundled_envelope_and_message_schema_count
	);
}

TEST_CASE("the paths the tables name are exactly the files the manifest lists", "[schemas][validator]")
{
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SUCCEED("the vendored bundle is not reachable from this test's own path");

		return;
	}

	const std::set<std::string> manifest_paths = read_manifest_schema_paths(schema_directory);

	REQUIRE(manifest_paths.size() == bundled_schema_file_count);

	const std::vector<std::string> table_paths = all_bundled_schema_paths();
	const std::set<std::string> table_path_set{table_paths.begin(), table_paths.end()};

	// Equality both ways. A path the tables name and the manifest does not is a
	// validator that will fail closed at runtime; a file the manifest lists and no
	// table names is a schema the extension is shipping and ignoring, which for an
	// outbound payload means an unvalidated one.
	REQUIRE(table_path_set == manifest_paths);

	// And each one is really on disk under the name the table spells, which is the
	// part the manifest cannot tell us.
	for (const std::string& schema_path : table_paths) {
		REQUIRE(std::filesystem::is_regular_file(schema_directory / schema_path));
	}
}

TEST_CASE("rendered validation failures are joined and capped", "[schemas][validator]")
{
	REQUIRE(render_validation_errors({}, 100).empty());
	REQUIRE(render_validation_errors({"/tempo is not a number"}, 100) == "/tempo is not a number");
	REQUIRE(render_validation_errors({"/a first", "/b second"}, 100) == "/a first; /b second");

	// The cap is the one that matters: stream-error.schema.json allows 4096 characters
	// for this field, and a snapshot that failed on four hundred tracks would otherwise
	// produce an error payload that fails its own schema — an error too long to
	// describe, which cannot be sent at all.
	const std::vector<std::string> many_failures(500, std::string(120, 'x'));
	const std::string rendered =
		render_validation_errors(many_failures, maximum_rendered_validation_errors_length);

	REQUIRE(rendered.size() == maximum_rendered_validation_errors_length);

	// Capping at exactly the budget, not one under or over.
	REQUIRE(render_validation_errors({std::string(10, 'y')}, 10) == std::string(10, 'y'));
	REQUIRE(render_validation_errors({std::string(11, 'y')}, 10) == std::string(10, 'y'));
}

TEST_CASE("a string too long for its schema is shortened rather than refused", "[schemas][validator]")
{
	REQUIRE(truncate_to_length("response:create_track", maximum_error_envelope_type_length)
		== "response:create_track");

	REQUIRE(truncate_to_length(std::string(200, 'z'), maximum_error_envelope_type_length).size()
		== maximum_error_envelope_type_length);

	REQUIRE(truncate_to_length("", 10).empty());
	REQUIRE(truncate_to_length("abc", 0).empty());
}

TEST_CASE("the envelope wrapper schema is named once and used in both directions", "[schemas][validator]")
{
	// The wrapper is what enforces the namespace:action type pattern and that no fourth
	// property rode along, so it is not bound to a type — it applies to every envelope.
	// It must therefore be in the load list without appearing in either table.
	const std::vector<std::string> schema_paths = all_bundled_schema_paths();

	REQUIRE(std::find(schema_paths.begin(), schema_paths.end(), std::string{envelope_schema_path})
		!= schema_paths.end());

	for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
		REQUIRE(binding.schema_path != envelope_schema_path);
	}

	for (const EnvelopeSchemaBinding& binding : outbound_envelope_schema_bindings) {
		REQUIRE(binding.schema_path != envelope_schema_path);
	}
}
