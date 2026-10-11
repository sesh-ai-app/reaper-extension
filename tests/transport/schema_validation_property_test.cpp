// Schema validation — Properties 12, 13, 14, 15 (task 4.3).
//
// Task 4.2 built the tables and the codec, and its tests substituted the validator.
// That settles what the codec *does* with a verdict and leaves open whether a real
// payload *earns* one. The four properties here split cleanly along that line, and
// the split decides how each one is written — so it is worth stating before the code.
//
// **Properties 14 and 15 are about what the codec asks and what it does with the
// answer.** They need no JSON Schema library, they run everywhere, and they are
// checked exhaustively over a closed corpus with the corpus size asserted.
//
// **Properties 12 and 13 are about whether a payload satisfies a vendored schema.**
// That is a question only a real validator can answer, and the real validator —
// `BundledSchemaValidator` — needs nlohmann/json and nlohmann/json-schema-validator.
// Neither is installed on every machine, and the project build skips
// `src/transport/schema_validator.cpp` when they are absent.
//
// So Properties 12 and 13 are compiled behind `__has_include` on the two headers and
// `SKIP` out with a named reason when they are not there. They **run in CI**, which
// configures with a vcpkg toolchain, so `tests/CMakeLists.txt` links
// `nlohmann_json::nlohmann_json` and `nlohmann_json_schema_validator::validator` and
// both headers are on the include path. They **skip locally** on a machine with
// neither, and skip visibly: `catch_discover_tests` sets `SKIP_RETURN_CODE 4`, so
// ctest reports them as skipped rather than passed. A property that passes by
// validating nothing is worse than one that says it did not run.
//
// **The real implementation is `#include`d as a translation unit.** That is the one
// unusual thing in this file. `BundledSchemaValidator`'s definitions live in
// `src/transport/schema_validator.cpp`, which the test target does not compile —
// `tests/CMakeLists.txt` builds only `tests/*.cpp`. Adding that one source to the
// test target is the right long-term fix and it is a CMakeLists change this task
// deliberately does not make; including the translation unit here gets the *real*
// loader and the *real* validator under test in the meantime, rather than a second
// validator written in the suite that could agree with the schemas while production
// disagreed. Nothing links both copies: the extension's shared library is a separate
// binary.
//
// **What Properties 12 and 13 are about, precisely.** The Project Context Builder
// (task 7.3) and the Tool Executor (task 10.1) are being written concurrently and are
// not available to depend on, so the payloads here are synthetic — generated to the
// shapes the schemas require, at their bounds, with their optional fields present and
// absent. That makes these properties statements about **the vendored schemas and the
// real validator**, not about either producer's output: they establish that the
// bundle compiles, that a correctly shaped payload is accepted, and — through the
// negative controls, which matter more than the positive cases — that a wrongly
// shaped one is rejected rather than waved through. Both should be re-pointed at the
// real producers once those land, at which point the generators below become the
// negative-control half and the producers supply the positive half.
//
// The negative controls are not decoration. "Every generated snapshot validates" is
// satisfied by a validator that accepts everything, by a schema loaded from the wrong
// file, and by a `$ref` that silently resolved to nothing. Each property therefore
// carries a table of payloads that must *fail*, one per way of being wrong, and
// asserts the size of that table too.
//
// Generation follows the repo's convention: exhaustive with an asserted count where
// the space is small, and `DeterministicBytes` (from `tests/daw/alias_store_test.cpp`)
// where it is not. Coverage guards are set from measured values. No new test
// dependency.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/envelope_codec.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

// The two headers the real validator needs. Present when the build was configured
// with a dependency toolchain that provides them — which is what CI does and what a
// machine with no vcpkg does not.
#if __has_include(<nlohmann/json.hpp>) && __has_include(<nlohmann/json-schema.hpp>)
	#define SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION 1
#else
	#define SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION 0
#endif

#if SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	#include <nlohmann/json.hpp>

	// The real loader and the real validator, compiled into this translation unit
	// rather than linked. See the file comment for why this is an include and not a
	// line in tests/CMakeLists.txt.
	#include <transport/schema_validator.cpp>
#endif

using sesh_ai::transport::ConcurrentQueue;
using sesh_ai::transport::EnvelopeCodec;
using sesh_ai::transport::EnvelopeSchemaBinding;
using sesh_ai::transport::InboundRefusalReason;
using sesh_ai::transport::PayloadSchemaValidator;
using sesh_ai::transport::SchemaValidationOutcome;
using sesh_ai::transport::ToolOutputSchemaBinding;
using sesh_ai::transport::bundled_schema_file_count;
using sesh_ai::transport::bundled_tool_output_schema_count;
using sesh_ai::transport::envelope_schema_path;
using sesh_ai::transport::error_code_property;
using sesh_ai::transport::error_envelope_type_property;
using sesh_ai::transport::inbound_envelope_schema_bindings;
using sesh_ai::transport::inbound_payload_schema_path;
using sesh_ai::transport::invalid_payload_error_code;
using sesh_ai::transport::tool_output_schema_bindings;
using sesh_ai::transport::tool_result_schema_path;
using sesh_ai::transport::validation_error_envelope_type;

namespace {

	// Locates the vendored bundle from this file's own path.
	//
	// The same walk `schema_validator_test.cpp` does, and repeated rather than shared
	// for the same reason the stub below is: its copy sits in an anonymous namespace,
	// and lifting either into a shared header means editing a file another piece of
	// work is holding open. Returns empty when the bundle is not reachable, and the
	// cases that need it skip.
	//
	// `MANIFEST.json` is required rather than the directory alone, and that check is
	// not defensive padding — it was earned. `__FILE__` is absolute under CMake, which
	// hands the compiler absolute source paths, but it is *relative* under a hand-rolled
	// compile from the repository root. The walk then resolves to `schemas` relative to
	// the working directory, which in this repository is the monorepo's own
	// `schemas/` — a different directory, holding the 42 tool input schemas that must
	// never be validated against, and looking enough like a bundle to be loaded. Which
	// is ADR 0019's "silently resolves to an unrelated directory" in a new costume, so
	// the same answer applies: identify the bundle by something only the bundle has.
	std::filesystem::path locate_schema_directory()
	{
		std::filesystem::path schema_directory =
			std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "schemas";

		if (!std::filesystem::is_regular_file(schema_directory / "MANIFEST.json")) {
			return {};
		}

		return schema_directory;
	}

	// `set_track_state` becomes `set-track-state`. The one mechanical substitution
	// between a tool name and its schema file name.
	std::string kebab_case(std::string_view tool_name)
	{
		std::string kebab{tool_name};

		std::replace(kebab.begin(), kebab.end(), '_', '-');

		return kebab;
	}

	// ---------------------------------------------------------------------------
	// Properties 14 and 15: the substituted document type and validator
	// ---------------------------------------------------------------------------

	// Stands in for nlohmann::json, providing only the operations the codec performs
	// on a document.
	//
	// A second copy of the idea in `envelope_codec_test.cpp`, trimmed to what these
	// two properties need. It is deliberately not shared: that file's stub is in an
	// anonymous namespace, and hoisting it into a `transport_test_doubles.h` means
	// editing a test file that concurrent work is also editing. The duplication is
	// bounded and local; the collision would not be.
	class ProbeJson {
	public:
		ProbeJson() = default;

		static ProbeJson discarded_document()
		{
			ProbeJson document;
			document.kind_ = Kind::discarded;

			return document;
		}

		static ProbeJson object()
		{
			ProbeJson document;
			document.kind_ = Kind::object_value;

			return document;
		}

		static ProbeJson object_with(std::map<std::string, ProbeJson> properties)
		{
			ProbeJson document;
			document.kind_ = Kind::object_value;
			document.properties_ = std::move(properties);

			return document;
		}

		static ProbeJson string_value(std::string value)
		{
			ProbeJson document;
			document.kind_ = Kind::string_value;
			document.value_ = std::move(value);

			return document;
		}

		static ProbeJson number_value()
		{
			ProbeJson document;
			document.kind_ = Kind::number_value;

			return document;
		}

		bool is_discarded() const { return kind_ == Kind::discarded; }
		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_string() const { return kind_ == Kind::string_value; }

		bool contains(const std::string& property_name) const
		{
			return kind_ == Kind::object_value && properties_.count(property_name) > 0;
		}

		const ProbeJson& at(const std::string& property_name) const
		{
			return properties_.at(property_name);
		}

		template <typename ValueType>
		ValueType get() const
		{
			return value_;
		}

		ProbeJson& operator[](const std::string& property_name)
		{
			if (kind_ == Kind::null_value) {
				kind_ = Kind::object_value;
			}

			return properties_[property_name];
		}

		ProbeJson& operator=(std::string value)
		{
			kind_ = Kind::string_value;
			value_ = std::move(value);
			properties_.clear();

			return *this;
		}

		std::string dump() const
		{
			switch (kind_) {
				case Kind::discarded:
					return "<discarded>";
				case Kind::null_value:
					return "null";
				case Kind::string_value:
					return "\"" + value_ + "\"";
				case Kind::number_value:
					return "0";
				case Kind::object_value:
					break;
			}

			std::string rendered{"{"};

			for (const auto& property : properties_) {
				if (rendered.size() > 1) {
					rendered.append(",");
				}

				rendered.append("\"").append(property.first).append("\":").append(property.second.dump());
			}

			return rendered.append("}");
		}

		template <typename IteratorType>
		static ProbeJson parse(IteratorType first, IteratorType last, std::nullptr_t, bool)
		{
			const std::string message_text{first, last};
			const auto registered = parse_table().find(message_text);

			return registered == parse_table().end() ? discarded_document() : registered->second;
		}

		static void register_parse(std::string message_text, ProbeJson document)
		{
			parse_table()[std::move(message_text)] = std::move(document);
		}

		static void reset_parse_table() { parse_table().clear(); }

	private:
		enum class Kind { null_value, discarded, object_value, string_value, number_value };

		static std::map<std::string, ProbeJson>& parse_table()
		{
			static std::map<std::string, ProbeJson> table;

			return table;
		}

		Kind kind_ = Kind::null_value;
		std::string value_;
		std::map<std::string, ProbeJson> properties_;
	};

	struct ProbeInboundEnvelope {
		std::string type;
		std::string request_id;
		ProbeJson payload;
	};

	struct ProbeOutboundEnvelope {
		std::string type;
		std::string request_id;
		ProbeJson payload;
	};

	// A validator that accepts the envelope wrapper and rejects every other schema it
	// is asked about, recording each request.
	//
	// The rejection is the point, and it is what makes Property 15 stronger here than
	// in `envelope_codec_test.cpp`, where the substituted validator accepted
	// everything. Under this one, a codec that re-validated a tool input would not
	// merely leave a trace in the request log — it would *refuse the tool call*. So the
	// property is checked twice over: by what was asked, and by what a producer would
	// have experienced if it had been.
	class WrapperOnlyValidator final : public PayloadSchemaValidator<ProbeJson> {
	public:
		SchemaValidationOutcome validate_against(
			std::string_view schema_path,
			const ProbeJson& payload
		) override
		{
			requested_schema_paths.emplace_back(schema_path);
			validated_payload_renderings.push_back(payload.dump());

			SchemaValidationOutcome outcome;
			outcome.schema_path.assign(schema_path);

			if (schema_path == envelope_schema_path) {
				outcome.valid = true;

				return outcome;
			}

			outcome.errors.emplace_back("/ rejected by the substituted validator");

			return outcome;
		}

		std::vector<std::string> requested_schema_paths;
		std::vector<std::string> validated_payload_renderings;
	};

	using ProbeCodec = EnvelopeCodec<ProbeJson>;

	ProbeJson envelope_document(
		const std::string& envelope_type,
		ProbeJson payload,
		const std::string& request_id
	)
	{
		std::map<std::string, ProbeJson> properties;

		properties.emplace("type", ProbeJson::string_value(envelope_type));
		properties.emplace("payload", std::move(payload));

		if (!request_id.empty()) {
			properties.emplace("requestId", ProbeJson::string_value(request_id));
		}

		return ProbeJson::object_with(std::move(properties));
	}

	std::string parseable_message(const std::string& message_text, ProbeJson document)
	{
		ProbeJson::register_parse(message_text, std::move(document));

		return message_text;
	}

	std::string read_error_property(const ProbeOutboundEnvelope& envelope, std::string_view property_name)
	{
		const std::string property_key{property_name};

		if (!envelope.payload.contains(property_key)) {
			return std::string{};
		}

		return envelope.payload.at(property_key).template get<std::string>();
	}

	// Inbound envelope types the extension can receive that have no bundled schema.
	//
	// `request:project_context` carries an empty payload; the seven `error:*` types are
	// the ones the server emits; `state:something_new` stands for a type from a server
	// running ahead of this build, which requirement 5.8 logs and ignores. None has a
	// schema in the bundle, so none can be refused for a payload reason — which is the
	// other half of Property 14.
	const std::vector<std::string>& unbundled_inbound_envelope_types()
	{
		static const std::vector<std::string> envelope_types{
			"request:project_context",
			"error:agent",
			"error:confirmation",
			"error:validation",
			"error:transport",
			"error:tool",
			"error:rate_limit",
			"state:something_new"
		};

		return envelope_types;
	}

	struct InboundCorpusEntry {
		std::string envelope_type;
		bool has_bundled_schema = false;
	};

	// Every inbound envelope type this build can meet: the nine bound ones, the 42 tool
	// calls, and the eight unbundled ones above.
	std::vector<InboundCorpusEntry> inbound_envelope_type_corpus()
	{
		std::vector<InboundCorpusEntry> corpus;

		for (const EnvelopeSchemaBinding& binding : inbound_envelope_schema_bindings) {
			corpus.push_back({std::string{binding.envelope_type}, true});
		}

		for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
			corpus.push_back({"request:" + std::string{binding.tool_name}, false});
		}

		for (const std::string& envelope_type : unbundled_inbound_envelope_types()) {
			corpus.push_back({envelope_type, false});
		}

		return corpus;
	}

	// Payload shapes to send a tool call with.
	//
	// Chosen to invite the mistake rather than to be representative: two of them carry
	// the discriminators the outbound path branches on (`refused`, `actions`), one
	// looks like a snapshot, one looks like a nested envelope. A codec that ever
	// decided which schema applies by looking at a payload instead of at a type would
	// be caught by these and not by an empty object.
	std::vector<std::pair<std::string, ProbeJson>> tool_input_payload_shapes()
	{
		std::vector<std::pair<std::string, ProbeJson>> shapes;

		shapes.emplace_back("empty", ProbeJson::object());

		shapes.emplace_back("plausible tool input", ProbeJson::object_with({
			{"trackSelector", ProbeJson::string_value("the drum bus")},
			{"volumeDecibels", ProbeJson::number_value()}
		}));

		shapes.emplace_back("carrying the refusal discriminator", ProbeJson::object_with({
			{"refused", ProbeJson::string_value("true")}
		}));

		shapes.emplace_back("carrying an action array", ProbeJson::object_with({
			{"actions", ProbeJson::object()}
		}));

		shapes.emplace_back("snapshot-shaped", ProbeJson::object_with({
			{"projectName", ProbeJson::string_value("Untitled")},
			{"tracks", ProbeJson::object()}
		}));

		shapes.emplace_back("envelope-shaped", ProbeJson::object_with({
			{"type", ProbeJson::string_value("request:create_track")},
			{"payload", ProbeJson::object()}
		}));

		return shapes;
	}

}

// ---------------------------------------------------------------------------
// Property 14 — an invalid envelope never reaches a handler
// Requirements 4.2, 4.3, 23.4
// ---------------------------------------------------------------------------

TEST_CASE(
	"Property 14: a payload that fails validation is refused, and exactly the bound types can fail",
	"[schemas][validator][property][codec]"
)
{
	ProbeJson::reset_parse_table();

	const std::vector<InboundCorpusEntry> corpus = inbound_envelope_type_corpus();

	// Nine bound, 42 tool calls, eight unbundled. Asserted so a corpus that quietly
	// stopped generating cannot make the property vacuous.
	REQUIRE(corpus.size() == 59);
	REQUIRE(inbound_envelope_schema_bindings.size() == 9);
	REQUIRE(unbundled_inbound_envelope_types().size() == 8);

	std::size_t refused_count = 0;
	std::size_t accepted_count = 0;

	for (const InboundCorpusEntry& entry : corpus) {
		// The corpus' own idea of which types are bound is checked against the table
		// rather than trusted, so the property is about `schema_validator.h` and not
		// about a list copied out of it.
		const std::optional<std::string_view> bound_schema_path =
			inbound_payload_schema_path(entry.envelope_type);

		REQUIRE(bound_schema_path.has_value() == entry.has_bundled_schema);

		WrapperOnlyValidator validator;
		ProbeCodec codec{validator};
		ConcurrentQueue<ProbeInboundEnvelope> inbound_queue;
		ConcurrentQueue<ProbeOutboundEnvelope> outbound_queue;

		const std::string request_id = "relay-" + entry.envelope_type;

		const std::string message_text = parseable_message(
			"inbound " + entry.envelope_type,
			envelope_document(entry.envelope_type, ProbeJson::object(), request_id)
		);

		const auto outcome =
			codec.accept_inbound_message<ProbeInboundEnvelope, ProbeOutboundEnvelope>(
				message_text,
				inbound_queue,
				outbound_queue
			);

		INFO("envelope type " << entry.envelope_type);

		if (!entry.has_bundled_schema) {
			// No schema applied, so there was nothing for the validator to reject it
			// with — even though this validator rejects everything it is asked about.
			// The envelope is dispatched on its type and read defensively.
			++accepted_count;

			CHECK(outcome.accepted);
			CHECK_FALSE(outcome.payload_validated);
			CHECK(outcome.payload_schema_path.empty());
			CHECK(inbound_queue.size() == 1);
			CHECK(outbound_queue.empty());
			CHECK(validator.requested_schema_paths
				== std::vector<std::string>{std::string{envelope_schema_path}});

			continue;
		}

		++refused_count;

		CHECK_FALSE(outcome.accepted);
		CHECK(outcome.refusal_reason
			== std::optional<InboundRefusalReason>{InboundRefusalReason::payload_schema_violation});
		CHECK(outcome.payload_validated);
		CHECK(outcome.payload_schema_path == *bound_schema_path);

		// The whole property. Nothing for the Main-Thread Dispatcher to route, so
		// nothing reaches a handler, so nothing touches the producer's project.
		CHECK(inbound_queue.empty());
		CHECK_FALSE(outcome.envelope.has_value());

		// And the server is told, in terms that name what was refused.
		CHECK(outbound_queue.size() == 1);

		const auto error_envelope = outbound_queue.try_pop();

		REQUIRE(error_envelope.has_value());
		CHECK(error_envelope->type == validation_error_envelope_type);
		CHECK(error_envelope->request_id == request_id);
		CHECK(read_error_property(*error_envelope, error_code_property) == invalid_payload_error_code);
		CHECK(read_error_property(*error_envelope, error_envelope_type_property) == entry.envelope_type);

		// The wrapper first, then the payload schema, and nothing else.
		CHECK(validator.requested_schema_paths == std::vector<std::string>{
			std::string{envelope_schema_path},
			std::string{*bound_schema_path}
		});
	}

	// Exhaustive over the corpus, and the two halves account for all of it.
	REQUIRE(refused_count == 9);
	REQUIRE(accepted_count == 50);
	REQUIRE(refused_count + accepted_count == corpus.size());
}

// ---------------------------------------------------------------------------
// Property 15 — tool inputs are not re-validated
// Requirement 4.4
// ---------------------------------------------------------------------------

TEST_CASE(
	"Property 15: no tool input is offered to the validator, whatever its payload looks like",
	"[schemas][validator][property][codec]"
)
{
	ProbeJson::reset_parse_table();

	// `envelope_codec_test.cpp` already checks this over the 42 tool names with an
	// empty payload and a validator that accepts everything. Two things are stronger
	// here, and both are about the failure mode rather than the count.
	//
	// The validator rejects every schema except the wrapper, so a re-validation does
	// not just leave a trace — it refuses the call, which is the producer-visible harm
	// requirement 4.4 exists to prevent.
	//
	// And the payload varies across shapes that invite payload sniffing, so a codec
	// that decided which schema applied by looking at the contents instead of the type
	// is caught.
	const std::vector<std::pair<std::string, ProbeJson>> payload_shapes = tool_input_payload_shapes();

	REQUIRE(payload_shapes.size() == 6);
	REQUIRE(tool_output_schema_bindings.size() == bundled_tool_output_schema_count);

	WrapperOnlyValidator validator;
	ProbeCodec codec{validator};
	ConcurrentQueue<ProbeInboundEnvelope> inbound_queue;
	ConcurrentQueue<ProbeOutboundEnvelope> outbound_queue;

	std::size_t tool_call_count = 0;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		for (const std::pair<std::string, ProbeJson>& shape : payload_shapes) {
			const std::string tool_request_envelope_type = "request:" + std::string{binding.tool_name};

			const std::string message_text = parseable_message(
				tool_request_envelope_type + " / " + shape.first,
				envelope_document(tool_request_envelope_type, shape.second, "relay-1")
			);

			const auto outcome =
				codec.accept_inbound_message<ProbeInboundEnvelope, ProbeOutboundEnvelope>(
					message_text,
					inbound_queue,
					outbound_queue
				);

			INFO(tool_request_envelope_type << " with a " << shape.first << " payload");

			CHECK(outcome.accepted);
			CHECK_FALSE(outcome.payload_validated);
			CHECK(outcome.payload_schema_path.empty());

			++tool_call_count;
		}
	}

	REQUIRE(tool_call_count == 42 * 6);

	// One request per call, and every one of them the envelope wrapper. The count is
	// what rules out a second validation the assertions above would not have noticed,
	// since a re-check of a *valid* tool input would leave `payload_validated` false
	// only if the codec also forgot to record it.
	REQUIRE(validator.requested_schema_paths.size() == tool_call_count);

	for (const std::string& requested_schema_path : validator.requested_schema_paths) {
		REQUIRE(requested_schema_path == envelope_schema_path);
	}

	// Every call was dispatched and nothing was refused.
	REQUIRE(inbound_queue.size() == tool_call_count);
	REQUIRE(outbound_queue.empty());
}

TEST_CASE(
	"Property 15: the bundle holds no file a tool input could be validated against",
	"[schemas][validator][property]"
)
{
	// The other half, and the one that would survive a codec rewrite: there is nothing
	// in the vendored directory that a re-validation could use. The 42 input schemas
	// live directly under `schemas/mcp-tools/` in the monorepo and the bundler
	// deliberately does not copy them, so a path naming one could not resolve even if
	// some future code asked for it.
	const std::filesystem::path schema_directory = locate_schema_directory();

	if (schema_directory.empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	std::size_t checked_tool_count = 0;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		const std::string input_schema_relative_path =
			"mcp-tools/" + kebab_case(binding.tool_name) + ".schema.json";

		INFO(input_schema_relative_path);
		CHECK_FALSE(std::filesystem::exists(schema_directory / input_schema_relative_path));

		// And the output schema, which is bundled and validated, is there — so the
		// absence above is the bundler's choice about direction rather than a bundle
		// that is simply missing files.
		CHECK(std::filesystem::is_regular_file(schema_directory / std::string{binding.schema_path}));

		++checked_tool_count;
	}

	REQUIRE(checked_tool_count == bundled_tool_output_schema_count);

	// Stronger than the 42 names: `mcp-tools/` holds nothing but the `outputs/`
	// directory, so a tool renamed on the server cannot arrive with an input schema
	// under a name this test did not think to look for.
	std::vector<std::string> mcp_tools_entry_names;

	for (const std::filesystem::directory_entry& entry :
		std::filesystem::directory_iterator{schema_directory / "mcp-tools"}) {
		mcp_tools_entry_names.push_back(entry.path().filename().string());
	}

	REQUIRE(mcp_tools_entry_names == std::vector<std::string>{"outputs"});
}

// ---------------------------------------------------------------------------
// Properties 12 and 13 — real validation against the vendored schemas
// Requirements 4.1, 6.8
// ---------------------------------------------------------------------------
//
// The test cases below are compiled unconditionally and skip from the inside, so a
// build without the JSON dependency reports them as skipped rather than as absent. It
// is the generators and the loader they drive that are conditional.

#if SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION

namespace {

	using sesh_ai::transport::BundledSchemaValidator;
	using sesh_ai::transport::SchemaBundleLoadOutcome;
	using sesh_ai::transport::project_context_schema_path;
	using sesh_ai::transport::tool_result_partial_contract_schema_path;
	using sesh_ai::transport::tool_result_refusal_schema_path;
	using sesh_ai::transport::tool_result_undo_contract_schema_path;

	// The xorshift generator the repo uses where a space is too large to enumerate.
	// Copied from `tests/daw/alias_store_test.cpp` rather than shared, for the reason
	// given above the document stub.
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

		bool coin() { return below(2) == 0; }

		// Whether an optional property is present. Biased towards present, so a
		// generated payload exercises the schema rather than mostly its required fields.
		bool present() { return below(4) != 0; }

	private:
		std::uint32_t state_;
	};

	// The bundle, loaded once for the whole file.
	//
	// `BundledSchemaValidator` is neither copyable nor movable — it hands out compiled
	// schemas by reference — so it is constructed in place as a function-local static
	// rather than returned.
	struct BundleFixture {
		BundleFixture()
			: validator{locate_schema_directory().string()}
		{
			load_outcome = validator.load();
		}

		BundledSchemaValidator validator;
		SchemaBundleLoadOutcome load_outcome;
	};

	BundleFixture& bundle_fixture()
	{
		static BundleFixture fixture;

		return fixture;
	}

	SchemaValidationOutcome validate(std::string_view schema_path, const nlohmann::json& payload)
	{
		return bundle_fixture().validator.validate_against<nlohmann::json>(schema_path, payload);
	}

	// Renders a validator outcome for a failure message. The point of a negative
	// control is the reason it gave, so a control that failed for the wrong reason is
	// visible in the report.
	std::string describe(const SchemaValidationOutcome& outcome)
	{
		std::string rendered = outcome.valid ? "valid" : "invalid";

		for (const std::string& error : outcome.errors) {
			rendered.append(" | ").append(error);
		}

		return rendered;
	}

	// A REAPER GUID in the braced form the schemas' pattern requires: 8-4-4-4-12
	// hexadecimal digits, mixed case, which the pattern allows and REAPER produces.
	std::string generate_guid(DeterministicBytes& bytes)
	{
		static constexpr std::string_view hexadecimal_digits{"0123456789ABCDEFabcdef"};
		static constexpr std::array<std::size_t, 5> group_lengths{8, 4, 4, 4, 12};

		std::string guid{"{"};

		for (std::size_t group_index = 0; group_index < group_lengths.size(); ++group_index) {
			if (group_index > 0) {
				guid.push_back('-');
			}

			for (std::size_t digit_index = 0; digit_index < group_lengths[group_index]; ++digit_index) {
				guid.push_back(hexadecimal_digits[bytes.below(hexadecimal_digits.size())]);
			}
		}

		return guid + "}";
	}

	// A name a producer might have typed, plus the two cases that break a length
	// bound: empty, and exactly the 512 characters the schemas allow.
	std::string generate_name(DeterministicBytes& bytes)
	{
		static const std::vector<std::string> names{
			"",
			"Kick",
			"Snare Top",
			"the drum bus",
			"don't call it \"the drums\"",
			"ドラム",
			"batería",
			"São Paulo take 3",
			"100% wet"
		};

		if (bytes.below(12) == 0) {
			return std::string(512, 'x');
		}

		return names[bytes.below(names.size())];
	}

	double generate_number(DeterministicBytes& bytes, double minimum, double maximum)
	{
		switch (bytes.below(6)) {
			case 0:
				return minimum;
			case 1:
				return maximum;
			default:
				break;
		}

		return minimum + (maximum - minimum) * (static_cast<double>(bytes.below(1001)) / 1000.0);
	}

	int generate_integer(DeterministicBytes& bytes, int minimum, int maximum)
	{
		switch (bytes.below(6)) {
			case 0:
				return minimum;
			case 1:
				return maximum;
			default:
				break;
		}

		return minimum + static_cast<int>(bytes.below(static_cast<std::size_t>(maximum - minimum) + 1));
	}

	std::string generate_color(DeterministicBytes& bytes)
	{
		static constexpr std::string_view hexadecimal_digits{"0123456789abcdefABCDEF"};

		std::string color{"#"};

		for (std::size_t digit_index = 0; digit_index < 6; ++digit_index) {
			color.push_back(hexadecimal_digits[bytes.below(hexadecimal_digits.size())]);
		}

		return color;
	}

	const std::vector<std::string>& structural_role_names()
	{
		static const std::vector<std::string> roles{
			"summing_folder_parent",
			"silent_folder_parent",
			"aux_return",
			"normal",
			"master"
		};

		return roles;
	}

	nlohmann::json generate_send(DeterministicBytes& bytes)
	{
		static const std::vector<std::string> send_modes{"post_fader", "pre_fx", "post_fx"};

		nlohmann::json send = nlohmann::json::object();

		send["trackGuid"] = generate_guid(bytes);

		if (bytes.present()) {
			send["index"] = generate_integer(bytes, 0, 64);
		}

		if (bytes.present()) {
			send["volume"] = generate_number(bytes, -150.0, 12.0);
		}

		if (bytes.present()) {
			send["pan"] = generate_number(bytes, -100.0, 100.0);
		}

		if (bytes.present()) {
			send["muted"] = bytes.coin();
		}

		if (bytes.present()) {
			send["mode"] = send_modes[bytes.below(send_modes.size())];
		}

		return send;
	}

	nlohmann::json generate_item(DeterministicBytes& bytes)
	{
		nlohmann::json item = nlohmann::json::object();

		item["guid"] = generate_guid(bytes);
		item["position"] = generate_number(bytes, 0.0, 3600.0);
		item["length"] = generate_number(bytes, 0.0, 600.0);

		if (bytes.present()) {
			item["name"] = generate_name(bytes);
		}

		if (bytes.present()) {
			item["muted"] = bytes.coin();
		}

		if (bytes.present()) {
			item["selected"] = bytes.coin();
		}

		return item;
	}

	nlohmann::json generate_track(DeterministicBytes& bytes, std::size_t track_index)
	{
		nlohmann::json track = nlohmann::json::object();

		track["guid"] = generate_guid(bytes);
		track["index"] = static_cast<int>(track_index);
		track["name"] = generate_name(bytes);

		if (bytes.present()) {
			track["alias"] = generate_name(bytes);
		}

		if (bytes.present()) {
			track["folderDepth"] = generate_integer(bytes, -64, 1);
		}

		if (bytes.present()) {
			track["parentSendEnabled"] = bytes.coin();
		}

		if (bytes.present()) {
			track["role"] = structural_role_names()[bytes.below(structural_role_names().size())];
		}

		if (bytes.present()) {
			track["armed"] = bytes.coin();
			track["muted"] = bytes.coin();
			track["soloed"] = bytes.coin();
		}

		if (bytes.present()) {
			track["volume"] = generate_number(bytes, -150.0, 12.0);
			track["pan"] = generate_number(bytes, -100.0, 100.0);
		}

		if (bytes.present()) {
			nlohmann::json fx_chain = nlohmann::json::array();

			for (std::size_t fx_index = 0, fx_count = bytes.below(4); fx_index < fx_count; ++fx_index) {
				fx_chain.push_back(generate_name(bytes));
			}

			track["fxChain"] = std::move(fx_chain);
		}

		if (bytes.present()) {
			nlohmann::json sends = nlohmann::json::array();

			for (std::size_t send_index = 0, send_count = bytes.below(4);
				send_index < send_count;
				++send_index) {
				sends.push_back(generate_send(bytes));
			}

			track["sends"] = std::move(sends);
		}

		if (bytes.present()) {
			nlohmann::json receives = nlohmann::json::array();

			for (std::size_t receive_index = 0, receive_count = bytes.below(3);
				receive_index < receive_count;
				++receive_index) {
				receives.push_back(generate_send(bytes));
			}

			track["receives"] = std::move(receives);
		}

		if (bytes.present()) {
			nlohmann::json items = nlohmann::json::array();

			for (std::size_t item_index = 0, item_count = bytes.below(5);
				item_index < item_count;
				++item_index) {
				items.push_back(generate_item(bytes));
			}

			track["itemCount"] = static_cast<int>(items.size());
			track["items"] = std::move(items);
		}

		if (bytes.present()) {
			track["color"] = generate_color(bytes);
		}

		return track;
	}

	nlohmann::json generate_marker(DeterministicBytes& bytes, std::size_t marker_index)
	{
		nlohmann::json marker = nlohmann::json::object();

		marker["index"] = static_cast<int>(marker_index);
		marker["position"] = generate_number(bytes, 0.0, 3600.0);
		marker["name"] = generate_name(bytes);

		if (bytes.present()) {
			marker["guid"] = generate_guid(bytes);
		}

		if (bytes.present()) {
			marker["color"] = generate_color(bytes);
		}

		return marker;
	}

	nlohmann::json generate_region(DeterministicBytes& bytes, std::size_t region_index)
	{
		const double start = generate_number(bytes, 0.0, 1800.0);

		nlohmann::json region = nlohmann::json::object();

		region["index"] = static_cast<int>(region_index);
		region["start"] = start;

		// End strictly after start is a runtime check, not a schema one (requirement
		// 9.6), so the generator honours it rather than relying on the schema to.
		region["end"] = start + generate_number(bytes, 0.1, 600.0);
		region["name"] = generate_name(bytes);

		if (bytes.present()) {
			region["guid"] = generate_guid(bytes);
		}

		if (bytes.present()) {
			region["color"] = generate_color(bytes);
		}

		return region;
	}

	nlohmann::json generate_master_track(DeterministicBytes& bytes)
	{
		nlohmann::json master_track = nlohmann::json::object();

		master_track["guid"] = generate_guid(bytes);

		if (bytes.present()) {
			master_track["name"] = generate_name(bytes);
		}

		if (bytes.present()) {
			master_track["role"] = "master";
		}

		if (bytes.present()) {
			master_track["muted"] = bytes.coin();
		}

		if (bytes.present()) {
			master_track["volume"] = generate_number(bytes, -150.0, 12.0);
			master_track["pan"] = generate_number(bytes, -100.0, 100.0);
		}

		if (bytes.present()) {
			nlohmann::json fx_chain = nlohmann::json::array();

			for (std::size_t fx_index = 0, fx_count = bytes.below(3); fx_index < fx_count; ++fx_index) {
				fx_chain.push_back(generate_name(bytes));
			}

			master_track["fxChain"] = std::move(fx_chain);
		}

		if (bytes.present()) {
			nlohmann::json receives = nlohmann::json::array();

			for (std::size_t receive_index = 0, receive_count = bytes.below(3);
				receive_index < receive_count;
				++receive_index) {
				receives.push_back(generate_send(bytes));
			}

			master_track["receives"] = std::move(receives);
		}

		if (bytes.present()) {
			master_track["color"] = generate_color(bytes);
		}

		return master_track;
	}

	// A snapshot of the shape the Project Context Builder is specified to produce
	// (requirement 6.1), assembled to the schema's requirements rather than from
	// REAPER.
	nlohmann::json generate_snapshot(DeterministicBytes& bytes)
	{
		static const std::vector<std::string> play_states{"stopped", "playing", "paused", "recording"};
		static const std::vector<int> denominators{1, 2, 4, 8, 16, 32, 64};

		nlohmann::json snapshot = nlohmann::json::object();

		snapshot["projectName"] = generate_name(bytes);
		snapshot["tempo"] = generate_number(bytes, 1.0, 960.0);
		snapshot["sampleRate"] = generate_integer(bytes, 8000, 768000);

		nlohmann::json time_signature = nlohmann::json::object();
		time_signature["numerator"] = generate_integer(bytes, 1, 64);
		time_signature["denominator"] = denominators[bytes.below(denominators.size())];
		snapshot["timeSignature"] = std::move(time_signature);

		// Zero tracks is a real session — a project just opened — so the generator
		// reaches it rather than treating one track as the floor.
		const std::size_t track_count = bytes.below(2) == 0 ? bytes.below(3) : bytes.below(12);

		nlohmann::json tracks = nlohmann::json::array();
		std::vector<std::string> track_guids;

		for (std::size_t track_index = 0; track_index < track_count; ++track_index) {
			nlohmann::json track = generate_track(bytes, track_index);

			track_guids.push_back(track.at("guid").get<std::string>());
			tracks.push_back(std::move(track));
		}

		snapshot["tracks"] = std::move(tracks);

		if (bytes.present()) {
			snapshot["projectLength"] = generate_number(bytes, 0.0, 7200.0);
		}

		if (bytes.present()) {
			snapshot["cursorPosition"] = generate_number(bytes, 0.0, 7200.0);
		}

		if (bytes.present()) {
			snapshot["playState"] = play_states[bytes.below(play_states.size())];
		}

		if (bytes.present()) {
			snapshot["masterTrack"] = generate_master_track(bytes);
		}

		if (bytes.present()) {
			nlohmann::json markers = nlohmann::json::array();

			for (std::size_t marker_index = 0, marker_count = bytes.below(5);
				marker_index < marker_count;
				++marker_index) {
				markers.push_back(generate_marker(bytes, marker_index));
			}

			snapshot["markers"] = std::move(markers);
		}

		if (bytes.present()) {
			nlohmann::json regions = nlohmann::json::array();

			for (std::size_t region_index = 0, region_count = bytes.below(5);
				region_index < region_count;
				++region_index) {
				regions.push_back(generate_region(bytes, region_index));
			}

			snapshot["regions"] = std::move(regions);
		}

		if (bytes.present()) {
			// Selected tracks are named by GUID out of the track list, which is the
			// point of the field (requirement 6.5): an index captured here could name a
			// different track by the time a tool call lands.
			nlohmann::json selected_track_guids = nlohmann::json::array();

			for (const std::string& track_guid : track_guids) {
				if (bytes.coin()) {
					selected_track_guids.push_back(track_guid);
				}
			}

			snapshot["selectedTrackGuids"] = std::move(selected_track_guids);
		}

		if (bytes.present()) {
			const double time_selection_start = generate_number(bytes, 0.0, 1800.0);

			snapshot["timeSelectionStart"] = time_selection_start;
			snapshot["timeSelectionEnd"] = time_selection_start + generate_number(bytes, 0.1, 600.0);
		}

		if (bytes.present()) {
			const double loop_start = generate_number(bytes, 0.0, 1800.0);

			snapshot["loopStart"] = loop_start;
			snapshot["loopEnd"] = loop_start + generate_number(bytes, 0.1, 600.0);
			snapshot["loopEnabled"] = bytes.coin();
		}

		return snapshot;
	}

	// The smallest snapshot the schema allows, used as the base for the negative
	// controls so that each control differs from a valid payload in exactly one way.
	nlohmann::json minimal_snapshot()
	{
		return nlohmann::json{
			{"projectName", "Untitled"},
			{"tempo", 120.0},
			{"timeSignature", {{"numerator", 4}, {"denominator", 4}}},
			{"sampleRate", 48000},
			{"tracks", nlohmann::json::array({nlohmann::json{
				{"guid", "{A1B2C3D4-E5F6-1234-5678-90ABCDEF1234}"},
				{"index", 0},
				{"name", "Kick"}
			}})}
		};
	}

	struct NegativeControl {
		std::string description;
		nlohmann::json payload;
	};

}

#endif

TEST_CASE("the vendored bundle compiles, and every selectable schema is in it", "[schemas][validator]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP("no JSON dependency in this build");
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	// Loading is the precondition for Properties 12 and 13 and a claim in its own
	// right: all 65 files parse and compile as draft-07 schemas, and none of the 309
	// references in them points outside its own file — which is what makes the
	// per-file loader in `schema_validator.cpp` correct.
	const SchemaBundleLoadOutcome& load_outcome = bundle_fixture().load_outcome;

	INFO("load errors: " << load_outcome.errors.size());

	for (const std::string& error : load_outcome.errors) {
		INFO(error);
	}

	REQUIRE(load_outcome.errors.empty());
	REQUIRE(load_outcome.compiled_schema_count == bundled_schema_file_count);
	REQUIRE(load_outcome.loaded);

	// Every path the tables can hand back names a schema the loaded bundle holds, in
	// both branches of the tool result selection. Without this, a validator failing
	// closed on a missing schema would look exactly like a payload that was wrong.
	std::size_t selectable_path_count = 0;

	for (const ToolOutputSchemaBinding& binding : tool_output_schema_bindings) {
		for (const bool payload_is_refusal : {false, true}) {
			const std::optional<std::string_view> selected =
				tool_result_schema_path(binding.tool_name, payload_is_refusal);

			REQUIRE(selected.has_value());

			INFO(*selected);
			CHECK(bundle_fixture().validator.has_schema(*selected));

			++selectable_path_count;
		}
	}

	REQUIRE(selectable_path_count == 2 * bundled_tool_output_schema_count);

	// And a path the bundle does not hold fails closed rather than passing.
	const SchemaValidationOutcome unknown_schema_outcome =
		validate("mcp-tools/set-track-state.schema.json", nlohmann::json::object());

	CHECK_FALSE(unknown_schema_outcome.valid);
	CHECK_FALSE(unknown_schema_outcome.errors.empty());
#endif
}

// ---------------------------------------------------------------------------
// Property 12 — snapshots validate
// Requirement 6.8
// ---------------------------------------------------------------------------

TEST_CASE("Property 12: a project context snapshot validates against the vendored schema",
	"[schemas][validator][property][context]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP(
		"nlohmann/json and nlohmann/json-schema-validator are not available to this build, so there is "
		"no real JSON Schema validation to run. Configure with a vcpkg toolchain — CI does — to run "
		"Property 12."
	);
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	REQUIRE(bundle_fixture().load_outcome.loaded);

	// Seeded, so a failure is reproducible and a reader can regenerate the offending
	// snapshot from the iteration number.
	DeterministicBytes bytes{0x0c0de12au};

	static constexpr int snapshot_count = 2000;

	std::size_t empty_project_count = 0;
	std::size_t snapshots_with_markers = 0;
	std::size_t snapshots_with_regions = 0;
	std::size_t snapshots_with_master_track = 0;
	std::size_t snapshots_with_a_selection = 0;
	std::size_t tracks_with_sends = 0;
	std::size_t tracks_with_items = 0;
	std::size_t tracks_at_a_volume_bound = 0;
	std::size_t names_at_the_length_bound = 0;

	for (int iteration = 0; iteration < snapshot_count; ++iteration) {
		const nlohmann::json snapshot = generate_snapshot(bytes);

		if (snapshot.at("tracks").empty()) {
			++empty_project_count;
		}

		if (snapshot.contains("markers") && !snapshot.at("markers").empty()) {
			++snapshots_with_markers;
		}

		if (snapshot.contains("regions") && !snapshot.at("regions").empty()) {
			++snapshots_with_regions;
		}

		if (snapshot.contains("masterTrack")) {
			++snapshots_with_master_track;
		}

		if (snapshot.contains("selectedTrackGuids") && !snapshot.at("selectedTrackGuids").empty()) {
			++snapshots_with_a_selection;
		}

		for (const nlohmann::json& track : snapshot.at("tracks")) {
			if (track.contains("sends") && !track.at("sends").empty()) {
				++tracks_with_sends;
			}

			if (track.contains("items") && !track.at("items").empty()) {
				++tracks_with_items;
			}

			if (track.contains("volume")
				&& (track.at("volume").get<double>() == -150.0 || track.at("volume").get<double>() == 12.0)) {
				++tracks_at_a_volume_bound;
			}

			if (track.at("name").get<std::string>().size() == 512) {
				++names_at_the_length_bound;
			}
		}

		const SchemaValidationOutcome outcome = validate(project_context_schema_path, snapshot);

		INFO("iteration " << iteration << " — " << describe(outcome));
		CHECK(outcome.valid);
	}

	// Coverage guards, set from what this corpus was measured to reach rather than
	// from what it ought to reach. They exist so a generator that quietly stopped
	// producing the interesting cases cannot leave the property passing on 2000
	// empty projects.
	CHECK(empty_project_count >= 382);
	CHECK(snapshots_with_markers >= 1203);
	CHECK(snapshots_with_regions >= 1247);
	CHECK(snapshots_with_master_track >= 1492);
	CHECK(snapshots_with_a_selection >= 975);
	CHECK(tracks_with_sends >= 3783);
	CHECK(tracks_with_items >= 4060);
	CHECK(tracks_at_a_volume_bound >= 1704);
	CHECK(names_at_the_length_bound >= 532);
#endif
}

TEST_CASE("Property 12: a snapshot that is wrong in one way is refused, not waved through",
	"[schemas][validator][property][context]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP("nlohmann/json and nlohmann/json-schema-validator are not available to this build");
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	// The half of Property 12 that carries the weight. "Every snapshot validates" is
	// also true of a validator that accepts anything, of a schema read from the wrong
	// file, and of a `$ref` that resolved to nothing — so each control below is a
	// minimal snapshot broken in exactly one way, and each must be refused.
	std::vector<NegativeControl> controls;

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.erase("tempo");
		controls.push_back({"a required project global is missing", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0).erase("guid");
		controls.push_back({"a track has no GUID", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["guid"] = "A1B2C3D4-E5F6-1234-5678-90ABCDEF1234";
		controls.push_back({"a GUID is missing its braces", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["volume"] = 12.5;
		controls.push_back({"a fader is above the schema's ceiling", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["pan"] = -101.0;
		controls.push_back({"a pan is beyond hard left", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["mood"] = "triumphant";
		controls.push_back({"the snapshot carries an extra property", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["busType"] = "drum";
		controls.push_back({"a track carries an extra property", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("timeSignature")["denominator"] = 3;
		controls.push_back({"a time signature denominator is not a note value", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["playState"] = "fast_forward";
		controls.push_back({"the transport is in a state the enum does not have", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["sampleRate"] = 44100.5;
		controls.push_back({"the sample rate is not an integer", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["sampleRate"] = 4000;
		controls.push_back({"the sample rate is below the schema's floor", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["color"] = "#GGGGGG";
		controls.push_back({"a colour is not hexadecimal", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["role"] = "drum_bus";
		controls.push_back({"a structural role is not one of the five", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["folderDepth"] = 2;
		controls.push_back({"a folder depth delta opens two folders at once", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["tracks"] = nlohmann::json::object();
		controls.push_back({"the track list is not an array", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["markers"] = nlohmann::json::array({nlohmann::json{{"index", 0}, {"position", 12.0}}});
		controls.push_back({"a marker has no name", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["regions"] = nlohmann::json::array({nlohmann::json{
			{"index", 0}, {"start", 12.0}, {"name", "Chorus"}
		}});
		controls.push_back({"a region has no end", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot["masterTrack"] = nlohmann::json{{"name", "MASTER"}};
		controls.push_back({"the master track has no GUID", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["name"] = std::string(513, 'x');
		controls.push_back({"a track name is one character over the bound", std::move(snapshot)});
	}

	{
		nlohmann::json snapshot = minimal_snapshot();
		snapshot.at("tracks").at(0)["sends"] =
			nlohmann::json::array({nlohmann::json{{"index", 0}, {"volume", 0.0}}});
		controls.push_back({"a send names no far end", std::move(snapshot)});
	}

	REQUIRE(controls.size() == 20);

	// The base the controls are derived from is itself valid, which is what makes each
	// of them differ from a valid payload in exactly one way.
	const SchemaValidationOutcome base_outcome = validate(project_context_schema_path, minimal_snapshot());

	INFO(describe(base_outcome));
	REQUIRE(base_outcome.valid);

	for (const NegativeControl& control : controls) {
		const SchemaValidationOutcome outcome = validate(project_context_schema_path, control.payload);

		INFO(control.description << " — " << describe(outcome));
		CHECK_FALSE(outcome.valid);

		// A refusal that names nothing is not diagnosable, and the rendered form is
		// what reaches the log and the error envelope.
		CHECK_FALSE(outcome.errors.empty());
	}
#endif
}

// ---------------------------------------------------------------------------
// Property 13 — outbound payloads validate
// Requirement 4.1
// ---------------------------------------------------------------------------

#if SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION

namespace {

	const std::vector<std::string>& refusal_reasons()
	{
		static const std::vector<std::string> reasons{
			"foreign_undo_entries",
			"output_file_collision",
			"ambiguous_track_selector",
			"unresolved_track_selector",
			"signal_cycle"
		};

		return reasons;
	}

	// The six blocking lists a refusal can carry, one per shape rather than one per
	// value: empty, each of the four entity kinds on its own, and a mixed list whose
	// description sits exactly on the 1024-character bound.
	std::vector<nlohmann::json> blocking_list_shapes()
	{
		std::vector<nlohmann::json> shapes;

		shapes.push_back(nlohmann::json::array());

		shapes.push_back(nlohmann::json::array({nlohmann::json{
			{"kind", "undo_entry"},
			{"description", "Move media items"}
		}}));

		shapes.push_back(nlohmann::json::array({nlohmann::json{
			{"kind", "file_path"},
			{"description", "/Users/producer/Music/render/mix.wav"}
		}}));

		shapes.push_back(nlohmann::json::array({nlohmann::json{
			{"kind", "track"},
			{"description", "Kick"},
			{"guid", "{A1B2C3D4-E5F6-1234-5678-90ABCDEF1234}"}
		}}));

		shapes.push_back(nlohmann::json::array({nlohmann::json{
			{"kind", "send"},
			{"description", "Kick to Drum Bus"},
			{"guid", "{0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0}"}
		}}));

		shapes.push_back(nlohmann::json::array({
			nlohmann::json{{"kind", "undo_entry"}, {"description", std::string(1024, 'u')}},
			nlohmann::json{{"kind", "file_path"}, {"description", "/tmp/take 1.wav"}},
			nlohmann::json{
				{"kind", "track"},
				{"description", "ドラム"},
				{"guid", "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}"}
			}
		}));

		return shapes;
	}

	nlohmann::json generate_action_outcome(DeterministicBytes& bytes)
	{
		nlohmann::json action = nlohmann::json::object();

		action["target"] = bytes.coin() ? generate_guid(bytes) : generate_name(bytes) + "!";

		if (bytes.below(3) != 0) {
			action["ok"] = true;

			return action;
		}

		// A failed action must say why, and the schema's `oneOf` is what makes a
		// reasonless failure unrepresentable rather than something to cope with at
		// runtime.
		action["ok"] = false;
		action["error"] = nlohmann::json{
			{"code", bytes.coin() ? "reaper_api_failure" : "unresolved_track_selector"},
			{"message", generate_name(bytes) + " could not be changed"}
		};

		return action;
	}

}

#endif

TEST_CASE("Property 13: a refusal validates against the vendored refusal contract",
	"[schemas][validator][property][tools]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP(
		"nlohmann/json and nlohmann/json-schema-validator are not available to this build, so there is "
		"no real JSON Schema validation to run. Configure with a vcpkg toolchain — CI does — to run "
		"Property 13."
	);
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	REQUIRE(bundle_fixture().load_outcome.loaded);

	// Exhaustive: five reasons, six blocking list shapes, acknowledgement field present
	// or absent. Sixty payloads, and the count is asserted so a loop that stopped early
	// is a failure rather than a thinner property.
	std::size_t refusal_count = 0;

	for (const std::string& reason : refusal_reasons()) {
		for (const nlohmann::json& blocking : blocking_list_shapes()) {
			for (const bool names_an_acknowledgement_field : {false, true}) {
				nlohmann::json refusal = nlohmann::json::object();

				refusal["refused"] = true;
				refusal["reason"] = reason;
				refusal["blocking"] = blocking;

				if (names_an_acknowledgement_field) {
					refusal["acknowledgementField"] = "acknowledgeForeignUndoEntries";
				}

				const SchemaValidationOutcome outcome =
					validate(tool_result_refusal_schema_path, refusal);

				INFO(reason << " with " << blocking.size() << " blocking entities — " << describe(outcome));
				CHECK(outcome.valid);

				++refusal_count;
			}
		}
	}

	REQUIRE(refusal_reasons().size() == 5);
	REQUIRE(blocking_list_shapes().size() == 6);
	REQUIRE(refusal_count == 60);

	// And the ways a refusal can be wrong, each of which must be refused. The first is
	// the one the codec's discriminator reading exists for: `refused` is a constant
	// true, so a payload carrying false is a refusal that got its own discriminator
	// wrong, and it must fail rather than be quietly accepted.
	std::vector<NegativeControl> controls;

	controls.push_back({"the discriminator is false", nlohmann::json{
		{"refused", false},
		{"reason", "signal_cycle"},
		{"blocking", nlohmann::json::array()}
	}});

	controls.push_back({"the blocking list is absent rather than empty", nlohmann::json{
		{"refused", true},
		{"reason", "signal_cycle"}
	}});

	controls.push_back({"the reason is not one of the five", nlohmann::json{
		{"refused", true},
		{"reason", "producer_said_no"},
		{"blocking", nlohmann::json::array()}
	}});

	controls.push_back({"a tool result field rode along", nlohmann::json{
		{"refused", true},
		{"reason", "signal_cycle"},
		{"blocking", nlohmann::json::array()},
		{"undoPositionBefore", 7}
	}});

	controls.push_back({"a blocking entity has no description", nlohmann::json{
		{"refused", true},
		{"reason", "foreign_undo_entries"},
		{"blocking", nlohmann::json::array({nlohmann::json{{"kind", "undo_entry"}}})}
	}});

	controls.push_back({"a blocking entity's description is empty", nlohmann::json{
		{"refused", true},
		{"reason", "foreign_undo_entries"},
		{"blocking", nlohmann::json::array({nlohmann::json{
			{"kind", "undo_entry"},
			{"description", ""}
		}})}
	}});

	controls.push_back({"a blocking entity is of an unknown kind", nlohmann::json{
		{"refused", true},
		{"reason", "output_file_collision"},
		{"blocking", nlohmann::json::array({nlohmann::json{
			{"kind", "directory"},
			{"description", "/tmp"}
		}})}
	}});

	controls.push_back({"a blocking entity's GUID is malformed", nlohmann::json{
		{"refused", true},
		{"reason", "ambiguous_track_selector"},
		{"blocking", nlohmann::json::array({nlohmann::json{
			{"kind", "track"},
			{"description", "Kick"},
			{"guid", "not-a-guid"}
		}})}
	}});

	REQUIRE(controls.size() == 8);

	for (const NegativeControl& control : controls) {
		const SchemaValidationOutcome outcome = validate(tool_result_refusal_schema_path, control.payload);

		INFO(control.description << " — " << describe(outcome));
		CHECK_FALSE(outcome.valid);
		CHECK_FALSE(outcome.errors.empty());
	}
#endif
}

TEST_CASE("Property 13: a partial outcome validates, and a reasonless failure does not",
	"[schemas][validator][property][tools]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP("nlohmann/json and nlohmann/json-schema-validator are not available to this build");
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	REQUIRE(bundle_fixture().load_outcome.loaded);

	DeterministicBytes bytes{0x5eed1303u};

	static constexpr int outcome_count = 600;

	std::size_t outcomes_with_a_failure = 0;
	std::size_t outcomes_entirely_successful = 0;
	std::size_t outcomes_with_an_undo_description = 0;

	for (int iteration = 0; iteration < outcome_count; ++iteration) {
		nlohmann::json actions = nlohmann::json::array();

		// minItems 1: an action array outcome reporting nothing is not a partial
		// outcome, it is a missing one.
		const std::size_t action_count = 1 + bytes.below(12);

		bool any_failed = false;

		for (std::size_t action_index = 0; action_index < action_count; ++action_index) {
			nlohmann::json action = generate_action_outcome(bytes);

			any_failed = any_failed || !action.at("ok").get<bool>();
			actions.push_back(std::move(action));
		}

		nlohmann::json partial = nlohmann::json::object();
		partial["actions"] = std::move(actions);

		if (bytes.present()) {
			partial["undoPositionBefore"] = generate_integer(bytes, 0, 4096);
		}

		if (bytes.present()) {
			partial["undoDescription"] = "Sesh AI: set_track_state";
			++outcomes_with_an_undo_description;
		}

		if (any_failed) {
			++outcomes_with_a_failure;
		} else {
			++outcomes_entirely_successful;
		}

		const SchemaValidationOutcome outcome =
			validate(tool_result_partial_contract_schema_path, partial);

		INFO("iteration " << iteration << " — " << describe(outcome));
		CHECK(outcome.valid);
	}

	// Measured from this corpus. Both halves have to be reached or the property says
	// nothing about the `oneOf` that splits them.
	CHECK(outcomes_with_a_failure >= 506);
	CHECK(outcomes_entirely_successful >= 94);
	CHECK(outcomes_with_an_undo_description >= 462);

	std::vector<NegativeControl> controls;

	// The control this schema exists for. Requirement 9.5 says every reported failure
	// carries a reason; the `oneOf` is what makes a reasonless one unrepresentable, so
	// this must fail rather than arrive at the agent as "something went wrong".
	controls.push_back({"a failed action carries no reason", nlohmann::json{
		{"actions", nlohmann::json::array({nlohmann::json{{"target", "Kick"}, {"ok", false}}})}
	}});

	controls.push_back({"a succeeded action carries a reason anyway", nlohmann::json{
		{"actions", nlohmann::json::array({nlohmann::json{
			{"target", "Kick"},
			{"ok", true},
			{"error", nlohmann::json{{"code", "x"}, {"message", "y"}}}
		}})}
	}});

	controls.push_back({"the action array is empty", nlohmann::json{
		{"actions", nlohmann::json::array()}
	}});

	controls.push_back({"an action names no target", nlohmann::json{
		{"actions", nlohmann::json::array({nlohmann::json{{"ok", true}}})}
	}});

	controls.push_back({"a failure reason has no message", nlohmann::json{
		{"actions", nlohmann::json::array({nlohmann::json{
			{"target", "Kick"},
			{"ok", false},
			{"error", nlohmann::json{{"code", "reaper_api_failure"}}}
		}})}
	}});

	controls.push_back({"the outcome carries an extra property", nlohmann::json{
		{"actions", nlohmann::json::array({nlohmann::json{{"target", "Kick"}, {"ok", true}}})},
		{"aliasesLearned", nlohmann::json::array()}
	}});

	controls.push_back({"the action array is absent", nlohmann::json{
		{"undoPositionBefore", 3}
	}});

	REQUIRE(controls.size() == 7);

	for (const NegativeControl& control : controls) {
		const SchemaValidationOutcome outcome =
			validate(tool_result_partial_contract_schema_path, control.payload);

		INFO(control.description << " — " << describe(outcome));
		CHECK_FALSE(outcome.valid);
		CHECK_FALSE(outcome.errors.empty());
	}
#endif
}

TEST_CASE("Property 13: an undo report validates against the vendored undo contract",
	"[schemas][validator][property][tools]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP("nlohmann/json and nlohmann/json-schema-validator are not available to this build");
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	REQUIRE(bundle_fixture().load_outcome.loaded);

	DeterministicBytes bytes{0x12d0900du};

	static constexpr int report_count = 400;

	std::size_t reports_with_a_description = 0;
	std::size_t reports_with_tool_specific_fields = 0;

	for (int iteration = 0; iteration < report_count; ++iteration) {
		nlohmann::json report = nlohmann::json::object();

		report["undoPositionBefore"] = generate_integer(bytes, 0, 100000);

		if (bytes.present()) {
			report["undoDescription"] = "Sesh AI: create_track";
			++reports_with_a_description;
		}

		// `additionalProperties: true` on this contract is deliberate: the tool's own
		// result fields sit alongside the undo report rather than inside it, and a
		// contract that forbade them would refuse every real payload.
		if (bytes.present()) {
			report["track"] = nlohmann::json{{"guid", generate_guid(bytes)}, {"name", generate_name(bytes)}};
			report["folderDepthRepairs"] = nlohmann::json::array();
			++reports_with_tool_specific_fields;
		}

		const SchemaValidationOutcome outcome =
			validate(tool_result_undo_contract_schema_path, report);

		INFO("iteration " << iteration << " — " << describe(outcome));
		CHECK(outcome.valid);
	}

	CHECK(reports_with_a_description >= 300);
	CHECK(reports_with_tool_specific_fields >= 320);

	std::vector<NegativeControl> controls;

	controls.push_back({"the undo position is absent", nlohmann::json{
		{"undoDescription", "Sesh AI: create_track"}
	}});

	controls.push_back({"the undo position is negative", nlohmann::json{
		{"undoPositionBefore", -1}
	}});

	controls.push_back({"the undo position is not an integer", nlohmann::json{
		{"undoPositionBefore", 3.5}
	}});

	controls.push_back({"the undo description is empty", nlohmann::json{
		{"undoPositionBefore", 3},
		{"undoDescription", ""}
	}});

	REQUIRE(controls.size() == 4);

	for (const NegativeControl& control : controls) {
		const SchemaValidationOutcome outcome =
			validate(tool_result_undo_contract_schema_path, control.payload);

		INFO(control.description << " — " << describe(outcome));
		CHECK_FALSE(outcome.valid);
		CHECK_FALSE(outcome.errors.empty());
	}
#endif
}

TEST_CASE("Property 13: a tool result validates against that tool's own output schema",
	"[schemas][validator][property][tools]")
{
#if !SESH_AI_TESTS_HAVE_REAL_SCHEMA_VALIDATION
	SKIP("nlohmann/json and nlohmann/json-schema-validator are not available to this build");
#else
	if (locate_schema_directory().empty()) {
		SKIP("the vendored bundle is not reachable from this test's own path");
	}

	REQUIRE(bundle_fixture().load_outcome.loaded);

	// The three contracts above are shared; a tool result is validated against its own
	// file, and that is where the difference between "carries an undo report" and
	// "carries the right undo report" shows up. Three tools, chosen because they are
	// the three shapes: a mutating tool that reports a span, a mutating tool that
	// reports structural repairs, and the one rewinding tool — which must not carry
	// `undoPositionBefore` at all (requirement 10.7).
	DeterministicBytes bytes{0x7001007au};

	static constexpr int result_count = 200;

	for (int iteration = 0; iteration < result_count; ++iteration) {
		const double start = generate_number(bytes, 0.0, 1800.0);
		const double length = generate_number(bytes, 0.1, 600.0);

		nlohmann::json time_selection = nlohmann::json::object();
		time_selection["start"] = start;
		time_selection["end"] = start + length;
		time_selection["lengthSeconds"] = length;
		time_selection["undoPositionBefore"] = generate_integer(bytes, 0, 4096);

		if (bytes.present()) {
			time_selection["undoDescription"] = "Sesh AI: set_time_selection";
		}

		const SchemaValidationOutcome time_selection_outcome =
			validate("mcp-tools/outputs/set-time-selection.schema.json", time_selection);

		INFO("set_time_selection iteration " << iteration << " — " << describe(time_selection_outcome));
		CHECK(time_selection_outcome.valid);

		nlohmann::json created_track = nlohmann::json::object();
		created_track["track"] = nlohmann::json{
			{"guid", generate_guid(bytes)},
			{"name", generate_name(bytes)}
		};
		created_track["index"] = generate_integer(bytes, 0, 64);
		created_track["accumulatedDepth"] = generate_integer(bytes, 0, 8);
		created_track["undoPositionBefore"] = generate_integer(bytes, 0, 4096);

		nlohmann::json folder_depth_repairs = nlohmann::json::array();

		for (std::size_t repair_index = 0, repair_count = bytes.below(4);
			repair_index < repair_count;
			++repair_index) {
			folder_depth_repairs.push_back(nlohmann::json{
				{"guid", generate_guid(bytes)},
				{"name", generate_name(bytes)},
				{"previousFolderDepth", generate_integer(bytes, -64, 1)},
				{"folderDepth", generate_integer(bytes, -64, 1)}
			});
		}

		created_track["folderDepthRepairs"] = std::move(folder_depth_repairs);

		const SchemaValidationOutcome created_track_outcome =
			validate("mcp-tools/outputs/create-track.schema.json", created_track);

		INFO("create_track iteration " << iteration << " — " << describe(created_track_outcome));
		CHECK(created_track_outcome.valid);

		nlohmann::json undone = nlohmann::json::object();
		nlohmann::json undone_descriptions = nlohmann::json::array();

		for (std::size_t entry_index = 0, entry_count = 1 + bytes.below(4);
			entry_index < entry_count;
			++entry_index) {
			undone_descriptions.push_back("Sesh AI: set_track_state");
		}

		undone["undoneEntryDescriptions"] = std::move(undone_descriptions);
		undone["undoStackPositionAfter"] = generate_integer(bytes, 0, 4096);
		undone["wasAgentEntry"] = bytes.coin();

		const SchemaValidationOutcome undone_outcome =
			validate("mcp-tools/outputs/undo-last-action.schema.json", undone);

		INFO("undo_last_action iteration " << iteration << " — " << describe(undone_outcome));
		CHECK(undone_outcome.valid);
	}

	std::vector<std::pair<std::string, std::pair<std::string, nlohmann::json>>> controls;

	// A zero-length span. The extension checks end > start at runtime because JSON
	// Schema cannot express it (requirement 9.6), but the schema does catch the
	// degenerate case through exclusiveMinimum on the length.
	controls.push_back({"a time selection of no length", {
		"mcp-tools/outputs/set-time-selection.schema.json",
		nlohmann::json{
			{"start", 12.0}, {"end", 12.0}, {"lengthSeconds", 0.0}, {"undoPositionBefore", 1}
		}
	}});

	// Absent and empty must not be indistinguishable: an absent repairs array would
	// read as "the extension did not check the folder structure".
	controls.push_back({"a created track reports no folder repairs array", {
		"mcp-tools/outputs/create-track.schema.json",
		nlohmann::json{
			{"track", nlohmann::json{{"guid", "{A1B2C3D4-E5F6-1234-5678-90ABCDEF1234}"}, {"name", "Kick"}}},
			{"index", 0},
			{"accumulatedDepth", 0},
			{"undoPositionBefore", 1}
		}
	}});

	// Requirement 10.7 as a schema rule: a rewinding tool must never report an undo
	// position marker, because walking back to one captured before an undo would redo
	// what had just been reverted.
	controls.push_back({"a rewinding tool reports an undo position marker", {
		"mcp-tools/outputs/undo-last-action.schema.json",
		nlohmann::json{
			{"undoneEntryDescriptions", nlohmann::json::array({"Move media items"})},
			{"undoStackPositionAfter", 4},
			{"wasAgentEntry", false},
			{"undoPositionBefore", 5}
		}
	}});

	REQUIRE(controls.size() == 3);

	for (const auto& control : controls) {
		const SchemaValidationOutcome outcome = validate(control.second.first, control.second.second);

		INFO(control.first << " — " << describe(outcome));
		CHECK_FALSE(outcome.valid);
		CHECK_FALSE(outcome.errors.empty());
	}
#endif
}
