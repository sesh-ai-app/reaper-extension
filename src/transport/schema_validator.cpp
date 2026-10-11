// The real validator: the 65 vendored files read off disk, compiled once, and
// validated against by path.
//
// Everything that needs nlohmann/json or json-schema-validator is here, which is
// what keeps schema_validator.h includable by the Catch2 suite on a machine with
// neither installed. The same arrangement transport_handler.cpp uses for the REAPER
// SDK, and for the same reason.
//
// Three decisions worth recording.
//
// **One pass, one file at a time.** The server's validator needs two passes over
// `schemas/`, because a `$ref` to a document it has not read yet is a hard failure
// rather than a deferred lookup. The bundle does not have that problem: the bundler's
// resolver rewrites every cross-document reference into a local `#/definitions/...`
// fragment, so all 309 references in the bundle point inside their own file and each
// file stands alone. That is what makes a per-file loader correct here, and it is
// worth checking before anyone reintroduces a multi-document loader on the strength
// of the schemas cross-referencing each other in `schemas/`, which they do.
//
// **No schema loader is supplied.** With every reference local, a loader would only
// ever be reached by a reference to something outside the bundle — which would mean
// the bundle was not what this build was compiled against. Leaving it null turns that
// into a named failure at load rather than a fetch, and requirement 20.2 is that
// nothing about the schemas needs the network.
//
// **Validation failures are collected, not thrown.** The codec runs on the network
// thread inside a Socket.IO callback, and an exception crossing that boundary is a
// dropped connection — which a producer experiences as Sesh losing their session. So
// the error handler collects, every entry point is wrapped, and a validation failure
// is a result rather than a control-flow event.
//
// No digest verification here. `cmake/VerifySchemaBundle.cmake` hashes all 65 files
// on every build and fails the build on a mismatch; requirement 4.6 is explicit that
// this is a build step and never a runtime condition.

#include <transport/schema_validator.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <nlohmann/json-schema.hpp>

namespace sesh_ai::transport {

	namespace {

		// How many individual failures are kept for one payload.
		//
		// A snapshot with four hundred tracks that is wrong in the same way on each of
		// them produces four hundred failures, and the first few say everything the
		// next few hundred do. The rendered form is capped at 4096 characters anyway,
		// so collecting more than this allocates strings nothing will read.
		constexpr std::size_t maximum_collected_validation_errors = 64;

		// Collects failures instead of throwing on the first one.
		//
		// The base class's own error() is what sets the flag `operator bool()` reads, so
		// it is called through rather than replaced.
		//
		// The failing instance is deliberately not rendered into the message. These
		// strings reach a log line and the `validationErrors` field of an error envelope
		// the server receives, and a producer's project data — track names, file paths —
		// has no business in either. The pointer says where; the message says what.
		class ValidationErrorCollector final : public nlohmann::json_schema::basic_error_handler {
		public:
			void error(
				const nlohmann::json::json_pointer& instance_pointer,
				const nlohmann::json& instance,
				const std::string& message
			) override
			{
				nlohmann::json_schema::basic_error_handler::error(instance_pointer, instance, message);

				if (collected_errors.size() >= maximum_collected_validation_errors) {
					return;
				}

				const std::string instance_path = instance_pointer.to_string();

				collected_errors.push_back(
					(instance_path.empty() ? std::string{"/"} : instance_path) + " " + message
				);
			}

			std::vector<std::string> collected_errors;
		};

	}

	// Defined here rather than in the header because json_validator is only a complete
	// type in this translation unit — which is the whole point of the pimpl.
	//
	// std::less<> so a std::string_view can be looked up without allocating a
	// std::string for it. json_validator is move-only, which std::map is fine with.
	struct BundledSchemaValidator::CompiledBundle {
		std::map<std::string, nlohmann::json_schema::json_validator, std::less<>> validators;
	};

	BundledSchemaValidator::BundledSchemaValidator(std::string schema_directory_path)
		: schema_directory_path_{std::move(schema_directory_path)},
		compiled_bundle_{}
	{
	}

	// Out of line because CompiledBundle is incomplete in the header, so the
	// unique_ptr's deleter cannot be instantiated there.
	BundledSchemaValidator::~BundledSchemaValidator() = default;

	SchemaBundleLoadOutcome BundledSchemaValidator::load()
	{
		SchemaBundleLoadOutcome outcome;

		// Built to one side and installed at the end, so a load that partly failed does
		// not leave the validator holding half a bundle while the caller is still
		// deciding what to do about the errors.
		auto loading_bundle = std::make_unique<CompiledBundle>();

		const std::filesystem::path schema_directory{schema_directory_path_};
		const std::vector<std::string> schema_paths = all_bundled_schema_paths();

		for (const std::string& relative_schema_path : schema_paths) {
			// The relative paths carry forward slashes, which std::filesystem::path
			// accepts as a generic separator on every platform the extension targets.
			const std::filesystem::path schema_file_path =
				schema_directory / std::filesystem::path{relative_schema_path};

			std::ifstream schema_file{schema_file_path, std::ios::binary};

			if (!schema_file.is_open()) {
				outcome.errors.push_back(relative_schema_path + ": could not be opened for reading");

				continue;
			}

			nlohmann::json schema_document = nlohmann::json::parse(schema_file, nullptr, false);

			if (schema_document.is_discarded()) {
				outcome.errors.push_back(relative_schema_path + ": could not be read as JSON");

				continue;
			}

			try {
				// No schema loader, for the reason in the file comment. The format checker
				// is the library's own: two fields in the bundle declare `format:
				// date-time`, and without a checker the keyword is accepted and ignored,
				// which would let a malformed expiry through the one check it has.
				nlohmann::json_schema::json_validator schema_validator{
					nullptr,
					nlohmann::json_schema::default_string_format_check
				};

				schema_validator.set_root_schema(std::move(schema_document));

				loading_bundle->validators.emplace(relative_schema_path, std::move(schema_validator));
			} catch (const std::exception& compilation_failure) {
				outcome.errors.push_back(relative_schema_path + ": " + compilation_failure.what());
			}
		}

		outcome.compiled_schema_count = loading_bundle->validators.size();
		outcome.loaded = outcome.errors.empty() && outcome.compiled_schema_count == schema_paths.size();

		compiled_bundle_ = std::move(loading_bundle);

		return outcome;
	}

	bool BundledSchemaValidator::has_schema(std::string_view schema_path) const
	{
		if (compiled_bundle_ == nullptr) {
			return false;
		}

		return compiled_bundle_->validators.find(schema_path) != compiled_bundle_->validators.end();
	}

	std::size_t BundledSchemaValidator::compiled_schema_count() const
	{
		return compiled_bundle_ == nullptr ? 0 : compiled_bundle_->validators.size();
	}

	template <typename PayloadType>
	SchemaValidationOutcome BundledSchemaValidator::validate_against(
		std::string_view schema_path,
		const PayloadType& payload
	)
	{
		SchemaValidationOutcome outcome;

		// Fails closed, the way the server's validator does: a boundary with no
		// registered schema refuses rather than letting unvalidated data through. The
		// two ways to get here are a load that was never run and a path the tables name
		// but the bundle does not hold, and the second is what the suite's check of
		// every path against MANIFEST.json exists to prevent.
		if (compiled_bundle_ == nullptr) {
			outcome.errors.emplace_back("/ the vendored schema bundle has not been loaded");

			return outcome;
		}

		const auto compiled_schema = compiled_bundle_->validators.find(schema_path);

		if (compiled_schema == compiled_bundle_->validators.end()) {
			outcome.errors.push_back("/ no vendored schema at \"" + std::string{schema_path} + "\"");

			return outcome;
		}

		outcome.schema_path.assign(schema_path);

		ValidationErrorCollector error_collector;

		try {
			compiled_schema->second.validate(payload, error_collector);
		} catch (const std::exception& validation_failure) {
			// The error handler makes an ordinary validation failure a collected error
			// rather than an exception, so reaching here means the library itself could
			// not proceed. Reported as a failure with its own message, because the one
			// thing that must not happen is this escaping onto the network thread.
			outcome.errors.push_back(std::string{"/ "} + validation_failure.what());

			return outcome;
		}

		if (static_cast<bool>(error_collector)) {
			outcome.errors = std::move(error_collector.collected_errors);

			return outcome;
		}

		outcome.valid = true;

		return outcome;
	}

	// The compile-time proof that the member template works with the payload type the
	// extension actually carries. A template nobody instantiates is a template nobody
	// compiles.
	template SchemaValidationOutcome BundledSchemaValidator::validate_against<nlohmann::json>(
		std::string_view,
		const nlohmann::json&
	);

}
