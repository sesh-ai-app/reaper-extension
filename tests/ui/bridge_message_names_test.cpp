// The CEF bridge message-name set (design "The CEF bridge contract").
//
// This file exists because of a gap nothing else can cover. The bridge carries
// serialized messages between C++ and TypeScript compiled by different toolchains that
// never meet, so a name that drifts on one side produces no error on either — the panel
// just stops showing one thing, silently. There is no type system spanning the two.
//
// What *can* be held here is the C++ half of the contract: that the set is the ten
// names design.md declares, that each travels in exactly one direction, that every
// payload names a schema the vendored bundle actually carries, and that the two
// categories stay on their own sides of the envelope namespace. The TypeScript side is
// checked against the same generated bundle, which is what makes these two independent
// readings of one artefact rather than two copies of a string.
//
// Most of it is `STATIC_REQUIRE`, because the whole set is `constexpr` and a contract
// that can be checked at compile time should not wait for a test run. The one case that
// touches the filesystem reads `MANIFEST.json` the way
// `transport/schema_validator_test.cpp` does — by text, with no JSON library, because
// this suite has to run on a machine with none installed.

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <ui/ui_host.h>

using sesh_ai::ui::BridgeMessageDirection;
using sesh_ai::ui::agent_response_bridge_message_name;
using sesh_ai::ui::all_bridge_message_names;
using sesh_ai::ui::bridge_field_naming_convention;
using sesh_ai::ui::bridge_message_direction_for;
using sesh_ai::ui::bridge_message_schema_path_for;
using sesh_ai::ui::bridge_messages_from_javascript;
using sesh_ai::ui::bridge_messages_to_javascript;
using sesh_ai::ui::confirmation_approval_bridge_message_name;
using sesh_ai::ui::confirmation_rejection_bridge_message_name;
using sesh_ai::ui::confirmation_resolution_bridge_message_name;
using sesh_ai::ui::connection_state_bridge_message_name;
using sesh_ai::ui::is_bridge_message_name;
using sesh_ai::ui::is_view_model_bridge_message;
using sesh_ai::ui::pending_confirmation_bridge_message_name;
using sesh_ai::ui::producer_prompt_bridge_message_name;
using sesh_ai::ui::script_download_bridge_message_name;
using sesh_ai::ui::stream_error_bridge_message_name;
using sesh_ai::ui::stream_ingest_bridge_message_name;
using sesh_ai::ui::view_bridge_message_namespace;

namespace {

	// The vendored bundle, found the same way `schema_validator_test.cpp` finds it:
	// `__FILE__` is absolute because CMake hands the compiler absolute source paths, so
	// walking up from `tests/ui/` reaches the repository root.
	//
	// Empty rather than a failure when it is not there. A missing bundle already fails
	// the build — `sesh_ai_verify_schema_bundle` hashes every file — so a second report
	// here would be noise.
	std::filesystem::path locate_schema_directory()
	{
		std::filesystem::path schema_directory =
			std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "schemas";

		if (!std::filesystem::is_directory(schema_directory)) {
			return {};
		}

		return schema_directory;
	}

	// The `path` values `MANIFEST.json` lists. Lifted out by text: the manifest is
	// generated one property per line, and this suite must run with no JSON library
	// available.
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

}

TEST_CASE("the bridge carries exactly the ten declared messages", "[ui][bridge]")
{
	// The number is the assertion. A name added to one direction's array and forgotten
	// in `all_bridge_message_names` is caught by the static assertion in the header;
	// this catches the opposite, which is a name declared as a constant and wired into
	// no direction at all — reachable from C++, unknown to the classifier, and so
	// unroutable by the UI.
	STATIC_REQUIRE(all_bridge_message_names.size() == 10);
	STATIC_REQUIRE(bridge_messages_to_javascript.size() == 7);
	STATIC_REQUIRE(bridge_messages_from_javascript.size() == 3);

	SECTION("every declared name is in the set")
	{
		STATIC_REQUIRE(is_bridge_message_name(script_download_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(producer_prompt_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(confirmation_approval_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(confirmation_rejection_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(pending_confirmation_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(confirmation_resolution_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(agent_response_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(stream_ingest_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(stream_error_bridge_message_name));
		STATIC_REQUIRE(is_bridge_message_name(connection_state_bridge_message_name));
	}

	SECTION("a plausible near-miss is not")
	{
		// The failure mode this whole file exists for: a name one side invented. Each of
		// these is what someone writing the TypeScript from memory would plausibly
		// produce, and none of them is in the set.
		STATIC_REQUIRE(!is_bridge_message_name("view:connection_status"));
		STATIC_REQUIRE(!is_bridge_message_name("view:confirmation"));
		STATIC_REQUIRE(!is_bridge_message_name("state:project_context"));
		STATIC_REQUIRE(!is_bridge_message_name("session:state"));
		STATIC_REQUIRE(!is_bridge_message_name("request:prompt"));
		STATIC_REQUIRE(!is_bridge_message_name("view:"));
		STATIC_REQUIRE(!is_bridge_message_name(""));
	}
}

TEST_CASE("each bridge message travels in exactly one direction", "[ui][bridge]")
{
	SECTION("published to JavaScript")
	{
		// The seven the C++ side produces. `script:download` is here and not in the
		// other array even though the UI acts on it, because requirement 5.7 routes it
		// inbound from the server and the bridge only ever carries it one way.
		for (const std::string_view message_name : bridge_messages_to_javascript) {
			const auto direction = bridge_message_direction_for(message_name);

			REQUIRE(direction.has_value());
			CHECK(*direction == BridgeMessageDirection::to_javascript);
		}
	}

	SECTION("received from JavaScript")
	{
		// The three the producer originates. All three are protocol payloads carried
		// verbatim: the UI builds the shape `messages/*.schema.json` already describes,
		// so the C++ side queues it rather than re-assembling it.
		for (const std::string_view message_name : bridge_messages_from_javascript) {
			const auto direction = bridge_message_direction_for(message_name);

			REQUIRE(direction.has_value());
			CHECK(*direction == BridgeMessageDirection::from_javascript);
		}
	}

	SECTION("no name appears in both")
	{
		for (const std::string_view published : bridge_messages_to_javascript) {
			for (const std::string_view received : bridge_messages_from_javascript) {
				CHECK(published != received);
			}
		}
	}

	SECTION("a name outside the set has no direction")
	{
		// Not a default direction. A message neither side should be sending has to be
		// refusable, and a classifier that answered `to_javascript` for an unknown name
		// would let it through.
		STATIC_REQUIRE(!bridge_message_direction_for("view:anything_else").has_value());
		STATIC_REQUIRE(!bridge_message_direction_for("confirm:expired").has_value());
	}
}

TEST_CASE("the view model half is exactly the view: namespace", "[ui][bridge]")
{
	STATIC_REQUIRE(view_bridge_message_namespace == "view:");

	SECTION("the six view models carry it")
	{
		STATIC_REQUIRE(is_view_model_bridge_message(pending_confirmation_bridge_message_name));
		STATIC_REQUIRE(is_view_model_bridge_message(confirmation_resolution_bridge_message_name));
		STATIC_REQUIRE(is_view_model_bridge_message(agent_response_bridge_message_name));
		STATIC_REQUIRE(is_view_model_bridge_message(stream_ingest_bridge_message_name));
		STATIC_REQUIRE(is_view_model_bridge_message(stream_error_bridge_message_name));
		STATIC_REQUIRE(is_view_model_bridge_message(connection_state_bridge_message_name));
	}

	SECTION("the four verbatim payloads do not")
	{
		// Which is what makes them safe to forward: each one names a real envelope type,
		// so the payload that crossed the bridge is the payload that goes on the wire.
		STATIC_REQUIRE(!is_view_model_bridge_message(script_download_bridge_message_name));
		STATIC_REQUIRE(!is_view_model_bridge_message(producer_prompt_bridge_message_name));
		STATIC_REQUIRE(!is_view_model_bridge_message(confirmation_approval_bridge_message_name));
		STATIC_REQUIRE(!is_view_model_bridge_message(confirmation_rejection_bridge_message_name));
	}

	SECTION("a bare namespace is not a view model")
	{
		STATIC_REQUIRE(!is_view_model_bridge_message("view:"));
		STATIC_REQUIRE(!is_view_model_bridge_message("view"));
		STATIC_REQUIRE(!is_view_model_bridge_message(""));
	}

	SECTION("the two categories partition the set")
	{
		std::size_t view_model_count = 0;

		for (const std::string_view message_name : all_bridge_message_names) {
			if (is_view_model_bridge_message(message_name)) {
				++view_model_count;
			}
		}

		CHECK(view_model_count == 6);
	}
}

TEST_CASE("the two confirmation decisions share one schema", "[ui][bridge]")
{
	// `messages/confirm-decision.schema.json` serves both, because the shape is
	// identical and the name already carries the verdict. Two names and one schema is
	// the one place in the set where the mapping is not injective, so it is asserted
	// rather than left to be rediscovered as a bug.
	STATIC_REQUIRE(
		bridge_message_schema_path_for(confirmation_approval_bridge_message_name)
			== bridge_message_schema_path_for(confirmation_rejection_bridge_message_name)
	);

	STATIC_REQUIRE(
		bridge_message_schema_path_for(confirmation_approval_bridge_message_name)
			== "messages/confirm-decision.schema.json"
	);
}

TEST_CASE("the verbatim half names messages/ schemas and the view half names bridge-messages/", "[ui][bridge]")
{
	for (const std::string_view message_name : all_bridge_message_names) {
		const std::string_view schema_path = bridge_message_schema_path_for(message_name);

		REQUIRE_FALSE(schema_path.empty());

		// The directory is the category, which is what makes the split visible in the
		// bundle rather than only in a design document. A view model under `messages/`
		// would be a protocol payload the server does not send; a verbatim payload under
		// `bridge-messages/` would be a second, drifting copy of a schema the server
		// already validates against.
		if (is_view_model_bridge_message(message_name)) {
			CHECK(schema_path.substr(0, 16) == "bridge-messages/");
		} else {
			CHECK(schema_path.substr(0, 9) == "messages/");
		}
	}
}

TEST_CASE("a name outside the set names no schema", "[ui][bridge]")
{
	// Empty rather than a plausible-looking path. A serialiser handed an unknown name
	// must not end up validating against a schema chosen by a fallback — that is the
	// same failure `message_dispatcher.h` describes for a re-derived tool result schema,
	// one layer out.
	STATIC_REQUIRE(bridge_message_schema_path_for("view:connection_status").empty());
	STATIC_REQUIRE(bridge_message_schema_path_for("session:state").empty());
	STATIC_REQUIRE(bridge_message_schema_path_for("").empty());
}

TEST_CASE("every bridge message's schema is in the vendored bundle", "[ui][bridge][schemas]")
{
	// The assertion that stops a schema path from being aspirational. A name whose
	// schema was authored in the monorepo but never bundled would leave both sides
	// validating against a file that is not there — and on the TypeScript side that is
	// an import error at build time, which is the good case; on this side it is a
	// validator that cannot load its schema at runtime.
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored schema bundle is not present");
	}

	const std::set<std::string> manifest_paths = read_manifest_schema_paths(schema_directory);

	REQUIRE_FALSE(manifest_paths.empty());

	for (const std::string_view message_name : all_bridge_message_names) {
		const std::string schema_path{bridge_message_schema_path_for(message_name)};

		CAPTURE(message_name, schema_path);

		// In the manifest, so the digest check covers it.
		CHECK(manifest_paths.count(schema_path) == 1);

		// And on disk, so a manifest entry for a file the bundler did not write is
		// caught here rather than at the first validation.
		CHECK(std::filesystem::is_regular_file(schema_directory / schema_path));
	}
}

TEST_CASE("the field naming convention is stated once", "[ui][bridge]")
{
	// Not a mechanism — the serialisers do the translating, and there is no C++ type
	// that could enforce it. It is here so the rule has a name a reviewer and the
	// TypeScript side can both point at, the same way design.md states it.
	STATIC_REQUIRE(bridge_field_naming_convention == "camelCase");
}
