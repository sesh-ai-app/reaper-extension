// The registration count, against the codecs the extension actually ships (task 24.3,
// requirements 4.1, 9.1, 9.2, 15.4, 22.1).
//
// This is the file the whole of task 24 exists to make possible, and the reason it
// exists is worth stating plainly: seventy-eight tasks passed, 1033 test cases passed,
// and the extension did nothing. Every component was built against a seam and tested
// against a recorder, which is what made the suite possible without REAPER, CEF, or a
// JSON library — and it left exactly one thing unchecked, which is whether the real
// objects were ever handed to each other.
//
// `tests/entry/extension_composition_test.cpp` closed half of that in task 22.1: it
// drives the whole graph and asserts all 42 tools register. But it does so over
// `stub_tool_payload_codecs`, which is complete by construction. It would have passed
// unchanged on every day between task 22.1 and task 24.3 — days on which
// `ToolCallPayloadCodec::is_complete()` was false, `ExecutorToolCallRouter` returned
// early on every call, and not one of the 42 tools was registered in any build a
// producer could install.
//
// So this file composes the registry over `build_tool_payload_codecs`, the function
// `extension_composition.cpp` itself calls, and asserts the number that comes back.
//
// ---------------------------------------------------------------------------
// Three assertions, and the third is the one that would have caught it
//
// **`is_complete()` is true.** The predicate the dispatch path branches on. False means
// every tool call is answered as an unknown tool without being dispatched, which is a
// composition fault wearing a protocol fault's clothes.
//
// **All 42 register, counted rather than checked for non-emptiness.** `register_*_tools`
// declines an incomplete codec rather than registering a handler that would throw from
// inside an undo block, so a missing binding is not a crash and not a compile error —
// it is a family quietly absent. A test that asserted "some tools registered" would pass
// with thirty-six.
//
// **`unavailable_call_count()` is zero after a real call.** The counter
// `ExecutorToolCallRouter` increments on the early return. Zero is the only acceptable
// value in a composed build, and the case below also drives the opposite — a
// default-constructed codec, which is what shipped before this task — so that the
// assertion is known to be capable of failing.
//
// Driven against nlohmann/json, because a stand-in document is exactly what this file
// exists not to trust.

#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#if __has_include(<nlohmann/json.hpp>)
	#define SESH_AI_TESTS_HAVE_JSON 1
#else
	#define SESH_AI_TESTS_HAVE_JSON 0
#endif

#if SESH_AI_TESTS_HAVE_JSON
	#include <nlohmann/json.hpp>

	#include <daw/stored_alias_lookup.h>
	#include <entry/tool_payload_codecs.h>
	#include <transport/schema_validator.h>

	#include "composition_test_doubles.h"
#endif

#if SESH_AI_TESTS_HAVE_JSON

namespace {

	using sesh_ai::entry::ExecutorToolCallRouter;
	using sesh_ai::entry::ToolCallPayloadCodec;
	using sesh_ai::entry::ToolHostReferences;
	using sesh_ai::entry::ToolPayloadCodecs;
	using sesh_ai::entry::ToolRegistryCompositionReport;
	using sesh_ai::entry::build_tool_call_codec;
	using sesh_ai::entry::build_tool_payload_codecs;
	using sesh_ai::entry::compose_tool_handler_registry;
	using sesh_ai::transport::ToolRequestReader;
	using sesh_ai::transport::ToolResultWriter;
	using sesh_ai::transport::tool_output_schema_bindings;

	using Json = nlohmann::json;

	// Stands in for `transport/envelope.h`'s `InboundEnvelope`, field for field across
	// everything the codec touches. The real one carries the same `nlohmann::json`
	// payload; what it also carries is a schema path the reader has no use for.
	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		Json payload = Json::object();
	};

	using Codec = ToolCallPayloadCodec<StubInboundEnvelope, Json>;

	StubInboundEnvelope a_tool_call(std::string tool_name, std::string request_id)
	{
		StubInboundEnvelope envelope;
		envelope.type = "request:" + tool_name;
		envelope.request_id = std::move(request_id);

		return envelope;
	}

	// The nine REAPER-backed adapters, replaced, and the registry composed over them.
	//
	// The host stubs come from `composition_test_doubles.h` and are reused rather than
	// restated, which they can be because not one of them depends on the document type —
	// `ToolHostReferences` has no `JsonValue` parameter at all. The REAPER side of this
	// extension does not know what a JSON document is, which is the whole reason the
	// codecs are a separate thing to get wrong.
	class RealCodecRegistry {
	public:
		explicit RealCodecRegistry(ToolPayloadCodecs<Json> codecs)
		{
			report_ = compose_tool_handler_registry<Json>(
				registry_,
				hosts(),
				std::move(codecs),
				ToolRequestReader{},
				ToolResultWriter<Json>{}
			);
		}

		RealCodecRegistry(const RealCodecRegistry&) = delete;
		RealCodecRegistry& operator=(const RealCodecRegistry&) = delete;
		RealCodecRegistry(RealCodecRegistry&&) = delete;
		RealCodecRegistry& operator=(RealCodecRegistry&&) = delete;

		const ToolRegistryCompositionReport& report() const { return report_; }

		sesh_ai::daw::tool_executor_of<Json>& executor() { return executor_; }

	private:
		ToolHostReferences hosts()
		{
			return ToolHostReferences{
				fx_host_,
				item_host_,
				marker_region_host_,
				routing_host_,
				track_state_host_,
				track_structure_host_,
				render_coordinator_,
				undo_manager_,
				alias_store_,
				learned_aliases_
			};
		}

		sesh_ai_tests::StubFxHost fx_host_;
		sesh_ai_tests::StubItemHost item_host_;
		sesh_ai_tests::StubMarkerRegionHost marker_region_host_;
		sesh_ai_tests::StubRoutingHost routing_host_;
		sesh_ai_tests::StubTrackStateHost track_state_host_;
		sesh_ai_tests::StubTrackStructureHost track_structure_host_;

		sesh_ai_tests::StubRenderHost render_host_;
		sesh_ai::daw::render_coordinator render_coordinator_{render_host_};

		sesh_ai_tests::StubUndoStack undo_stack_;
		sesh_ai::daw::undo_manager undo_manager_{undo_stack_};

		sesh_ai_tests::StubAliasStorage alias_storage_;
		sesh_ai::daw::AliasStore alias_store_{alias_storage_};
		sesh_ai::daw::StoredAliasLookup learned_aliases_{alias_store_};

		sesh_ai_tests::StubTrackListSource track_list_;

		sesh_ai::daw::tool_handler_registry<Json> registry_;

		sesh_ai::daw::tool_executor_of<Json> executor_{
			registry_,
			undo_manager_,
			track_list_,
			learned_aliases_
		};

		ToolRegistryCompositionReport report_;
	};

}

TEST_CASE("the shipped tool call codec is complete", "[codecs][registry]")
{
	// The predicate `ExecutorToolCallRouter::execute_tool_call` branches on. Every build
	// before task 24.3 answered false here, and answered "unknown tool" to all 42 tools
	// as a result.
	const Codec codec = build_tool_call_codec<StubInboundEnvelope, Json>();

	REQUIRE(codec.is_complete());

	SECTION("both halves are present, not just one")
	{
		// `is_complete()` is an `and`, so this says the same thing twice on purpose —
		// the two members are bound by two separate statements and a test that only read
		// the predicate could not say which was missing.
		REQUIRE(static_cast<bool>(codec.read_tool_call));
		REQUIRE(static_cast<bool>(codec.write_dispatch_outcome));
	}

	SECTION("and a default-constructed one is not, which is what shipped before")
	{
		// The assertion above is only worth anything if it can fail. This is the value it
		// would have had.
		REQUIRE_FALSE(Codec{}.is_complete());
	}
}

TEST_CASE("the shipped codecs register all 42 constrained tools", "[codecs][registry]")
{
	RealCodecRegistry composed{build_tool_payload_codecs<Json>()};

	const ToolRegistryCompositionReport& report = composed.report();

	SECTION("nothing was refused and nothing was left out")
	{
		INFO("refused: " << sesh_ai_tests::describe_failures(report));
		INFO("unregistered: " << sesh_ai_tests::describe_unregistered(report));

		REQUIRE_FALSE(report.anything_was_refused());
		REQUIRE(report.every_constrained_tool_registered());
	}

	SECTION("the count is the whole of the protocol's tool list")
	{
		// Counted, not checked for non-emptiness. A family whose codec was not bound is
		// declined by its own `register_*_tools` rather than failing to compile, so the
		// difference between a correct build and one missing the six marker and region
		// tools is this number and nothing else.
		//
		// Read from `tool_output_schema_bindings` so it cannot drift from the vendored
		// bundle, and then stated as a literal as well: the 42 is requirement 4.1's
		// number, and a change to it is a protocol change that should be noticed here
		// rather than absorbed.
		REQUIRE(report.registered_tool_names.size() == tool_output_schema_bindings.size());
		REQUIRE(report.registered_tool_names.size() == 42);
	}

	SECTION("every registered name is distinct")
	{
		// The registry declines a second handler for one tool, so a duplicate would show
		// up in `failures` rather than here — which means this is really a check on the
		// report itself, and on the ten registration calls not double-counting a family.
		std::vector<std::string> sorted = report.registered_tool_names;

		for (std::size_t outer = 0; outer < sorted.size(); ++outer) {
			for (std::size_t inner = outer + 1; inner < sorted.size(); ++inner) {
				INFO("duplicate name: " << sorted[outer]);
				REQUIRE(sorted[outer] != sorted[inner]);
			}
		}
	}
}

TEST_CASE("a codec left unbound costs exactly its own family", "[codecs][registry]")
{
	// The failure mode, induced. This is what dropping one `bind_*` pair from
	// `build_tool_payload_codecs` would do, and the point of inducing it is that the
	// assertions in the case above are then known to be the ones that catch it.
	ToolPayloadCodecs<Json> codecs = build_tool_payload_codecs<Json>();

	SECTION("the six mutating marker and region tools")
	{
		codecs.marker_region = sesh_ai::daw::tools::marker_region_payload_codec<Json>{};

		RealCodecRegistry composed{std::move(codecs)};

		REQUIRE(composed.report().registered_tool_names.size() == 42 - 6);
		REQUIRE_FALSE(composed.report().every_constrained_tool_registered());

		// The two reads over the same host are a different family and are unaffected,
		// which is the distinction that makes the per-family codecs worth having.
		REQUIRE_FALSE(sesh_ai_tests::contains(
			composed.report().unregistered_constrained_tool_names,
			"list_markers"
		));
	}

	SECTION("render, which is one tool and its own family")
	{
		// The same failure mode at its smallest. `register_render_tool` declines an
		// incomplete codec the way `register_marker_region_tools` does, so the count
		// speaks for it too and the load-time log line names it.
		codecs.render = sesh_ai::daw::tools::render_payload_codec<Json>{};

		RealCodecRegistry composed{std::move(codecs)};

		REQUIRE(composed.report().registered_tool_names.size() == 42 - 1);
		REQUIRE_FALSE(composed.report().every_constrained_tool_registered());

		const std::vector<std::string>& missing =
			composed.report().unregistered_constrained_tool_names;

		REQUIRE(missing.size() == 1);
		REQUIRE(sesh_ai_tests::contains(missing, "render"));
	}
}

TEST_CASE("a composed build dispatches rather than reporting the tool unavailable", "[codecs][registry]")
{
	// `unavailable_call_count()` is the counter on the early return, and zero is the only
	// acceptable value once a codec is configured. The sections below drive both sides of
	// it, because a counter that is zero in a build where nothing was called is zero for
	// the wrong reason.
	RealCodecRegistry composed{build_tool_payload_codecs<Json>()};

	REQUIRE(composed.report().every_constrained_tool_registered());

	SECTION("a real call is executed, and nothing is counted as unavailable")
	{
		ExecutorToolCallRouter<StubInboundEnvelope, Json> router{
			composed.executor(),
			build_tool_call_codec<StubInboundEnvelope, Json>()
		};

		// `get_project_summary` takes no field out of its payload and addresses no
		// track, so an empty object is a complete call — which keeps this case about the
		// codec being reached rather than about any one reader.
		const sesh_ai::transport::ToolCallResponse<Json> response =
			router.execute_tool_call("get_project_summary", a_tool_call("get_project_summary", "call-1"));

		REQUIRE(router.executed_call_count() == 1);
		REQUIRE(router.unavailable_call_count() == 0);

		// The tool is known, so it was dispatched rather than reported as a gap.
		REQUIRE(response.tool_is_known);

		// And a payload came back. An incomplete codec returns a default-constructed
		// response, so this is the difference between the two visible from outside.
		REQUIRE(response.payload.is_object());
	}

	SECTION("an unknown tool is still answered, and is not counted as unavailable either")
	{
		// Requirement 23.6. The two are different conditions and the counter must not
		// conflate them: a name this build does not implement is a visible gap reported
		// to the server, while an unavailable call is a build that could not dispatch
		// anything at all.
		ExecutorToolCallRouter<StubInboundEnvelope, Json> router{
			composed.executor(),
			build_tool_call_codec<StubInboundEnvelope, Json>()
		};

		const sesh_ai::transport::ToolCallResponse<Json> response =
			router.execute_tool_call("polish_the_mix", a_tool_call("polish_the_mix", "call-2"));

		REQUIRE_FALSE(response.tool_is_known);
		REQUIRE(router.unavailable_call_count() == 0);
		REQUIRE(router.executed_call_count() == 1);
	}

	SECTION("and an unconfigured codec counts every call unavailable, dispatching none")
	{
		// What shipped before task 24.3, driven through the same router so the assertions
		// above are known to be capable of failing. Forty-two tools registered and every
		// call answered without one of them running.
		ExecutorToolCallRouter<StubInboundEnvelope, Json> router{composed.executor(), Codec{}};

		const sesh_ai::transport::ToolCallResponse<Json> response =
			router.execute_tool_call("get_project_summary", a_tool_call("get_project_summary", "call-3"));

		REQUIRE(router.unavailable_call_count() == 1);
		REQUIRE(router.executed_call_count() == 0);
		REQUIRE_FALSE(response.tool_is_known);
		REQUIRE(response.payload.is_null());
	}
}

#else

TEST_CASE("the shipped tool payload codecs need nlohmann/json")
{
	SKIP(
		"nlohmann/json is not on the include path, so the codecs cannot be built against the "
		"library they bind to. Configure with a vcpkg toolchain."
	);
}

#endif
