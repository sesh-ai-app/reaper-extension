// The composition root's translation half: the graph bound to nlohmann/json and to the
// REAPER C API (task 24.3, requirements 1.1, 2.4, 5.1, 5.2, 9.1, 15.4, 22.1).
//
// `entry/extension_composition.h` decides what goes where and what is reported when a
// piece is missing, and it is a header so the suite can drive all of it against stubs.
// This file is the dull half that header's comment promised: one instantiation, the
// nine REAPER-backed adapters constructed, and the three JSON seams filled in.
//
// It is also the file that makes the extension do anything at all. Until it existed,
// `ToolCallPayloadCodec::is_complete()` was false, the dispatch path in
// `ExecutorToolCallRouter` returned early on every call, all 42 tools were unregistered
// because every `register_*_tools` declines an incomplete codec, and
// `plugin_entry.cpp` passed a default-constructed `EnvelopeRouter` that the Main-Thread
// Dispatcher reads as "nothing routes yet".
//
// ---------------------------------------------------------------------------
// Where the 42 tools' JSON actually comes from
//
// None of it is here either. Tasks 24.1 and 24.2 put the readers and the writers in
// `src/transport/`, beside the schemas they answer to, and `entry/tool_payload_codecs.h`
// is where they are assembled — in a header rather than in this file, so that
// `tests/entry/tool_payload_codecs_test.cpp` can call the same two functions this file
// calls and assert the number of tools that register. That matters because a family left
// out of the assembly is not a compile error: every `register_*_tools` declines an
// incomplete codec, so the symptom is tools quietly absent from the registry. See that
// header's own comment.
//
// What is left here is the one thing that genuinely cannot move: turning the renderer's
// text into a document, which is the one call into nlohmann/json on the bridge path.
//
// ---------------------------------------------------------------------------
// The nine adapters, and the one that is deliberately left unwired
//
// Every REAPER-backed adapter in this tree takes `reaper_plugin_info_t*` and resolves
// its own function pointers through the registration table's `GetFunc`, so constructing
// all of them needs nothing from the SDK's import table and this file includes no SDK
// header. An adapter whose functions REAPER did not supply reports that through its own
// `unresolved_function_names`, which is a gap a log line can name rather than a null
// call inside a producer's session.
//
// `ReaperTrackListSource`'s optional `StructuralRoleProvider` is left unset. Requirement
// 6.6 derives structural roles in the Project Context Builder, and wiring the resolver
// to read them back would close a cycle: the builder is a member of the graph and the
// track list source is one of the graph's inputs. The header's own default is the
// conservative answer and is what it documents — a resolver with no role provider
// refuses a role selector rather than answering it wrongly — so a producer saying "the
// drum bus" gets an unresolved selector instead of the wrong track. That is a gap, named
// here, not a silent one.

#include <entry/composed_extension.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <context/reaper_project_context_source.h>
#include <daw/alias_store.h>
#include <daw/object_resolver.h>
#include <daw/reaper_alias_storage.h>
#include <daw/reaper_fx_host.h>
#include <daw/reaper_render_host.h>
#include <daw/reaper_undo_stack.h>
#include <daw/render_coordinator.h>
#include <daw/stored_alias_lookup.h>
#include <daw/tools/reaper_item_host.h>
#include <daw/tools/reaper_marker_region_host.h>
#include <daw/tools/reaper_routing_host.h>
#include <daw/tools/reaper_track_state_host.h>
#include <daw/tools/reaper_track_structure_host.h>
#include <daw/undo_manager.h>
#include <entry/tool_payload_codecs.h>
#include <transport/transport_handler.h>
#include <ui/bridge_parsing.h>

#include <nlohmann/json.hpp>

namespace sesh_ai::entry {

	namespace {

		using Json = nlohmann::json;

		// REAPER's active project, which is what its API takes a null project to mean.
		// Named rather than written as `nullptr` ten times, because "the project the
		// producer is looking at" is the decision and `nullptr` is only how it is spelled.
		ReaProject* const active_project = nullptr;

		// ---------------------------------------------------------------------------
		// The bridge
		// ---------------------------------------------------------------------------

		// One message from the renderer, parsed and then read.
		//
		// The parse is the only part of this that needs a JSON library, which is exactly
		// why `ui::read_inbound_bridge_message` takes an already-parsed document: every
		// decision it makes — direction, object-ness, the properties each schema
		// requires, and whether a `confirm:*` payload's `decision` agrees with the name it
		// arrived under — is then compiled and driven by the suite.
		//
		// `parse` with `allow_exceptions` off, because this is renderer-supplied text on
		// the thread that draws REAPER's UI. A throw here would unwind through CEF's C
		// callback, which is undefined behaviour and in practice a REAPER crash; a
		// discarded document is the same information as a value.
		BridgeMessageDispatcherSink<Json, transport::OutboundEnvelope>::MessageReader
		build_bridge_message_reader()
		{
			return [](std::string_view message_name, const std::string& serialized_json) {
				Json document = Json::parse(serialized_json, nullptr, false);

				std::optional<Json> parsed_payload;

				if (!document.is_discarded()) {
					parsed_payload = std::move(document);
				}

				return ui::read_inbound_bridge_message<Json>(message_name, std::move(parsed_payload));
			};
		}

		// ---------------------------------------------------------------------------
		// The graph, and everything under it
		// ---------------------------------------------------------------------------

		// Owns the nine REAPER-backed adapters, the three sources, and the graph over
		// them.
		//
		// Member declaration order is construction order and is load-bearing twice: the
		// adapters before the things that wrap them — the undo stack before the Undo
		// Manager, the alias storage before the Alias Store before the lookup that reads
		// it, the render host before the Render Coordinator — and all of them before the
		// graph, which holds every one by reference. Read bottom-up it is the destruction
		// order, and the graph going first is what matters: nothing it references is torn
		// down while it can still reach it.
		class ReaperBackedExtension final : public ComposedExtension {
		public:
			ReaperBackedExtension(ComposedExtensionDependencies dependencies)
				: fx_host_{dependencies.plugin_info, active_project},
				item_host_{dependencies.plugin_info, active_project},
				marker_region_host_{dependencies.plugin_info, active_project},
				routing_host_{dependencies.plugin_info, active_project},
				track_state_host_{dependencies.plugin_info, active_project},
				track_structure_host_{dependencies.plugin_info, active_project},
				render_host_{dependencies.plugin_info, active_project},
				render_coordinator_{render_host_},
				undo_stack_{dependencies.plugin_info},
				undo_manager_{undo_stack_},
				alias_storage_{dependencies.plugin_info, active_project},
				alias_store_{alias_storage_},
				learned_aliases_{alias_store_},
				track_list_{dependencies.plugin_info, active_project},
				project_context_source_{dependencies.plugin_info, active_project},
				transport_actions_{dependencies.plugin_info},
				graph_{
					build_graph_dependencies(std::move(dependencies)),
					transport::ToolRequestReader{},
					transport::ToolResultWriter<Json>{}
				}
			{
			}

			ReaperMessageDispatcher& message_dispatcher() override
			{
				return graph_.message_dispatcher();
			}

			std::function<void()> ui_state_publisher() override
			{
				return graph_.ui_state_publisher();
			}

			ui::UiHost& ui_host() override { return graph_.ui_host(); }

			ui::DeferredBrowserHost& deferred_browser_host() override
			{
				return graph_.deferred_browser_host();
			}

			ui::DeferredConnectionStatePresenter& connection_state_presenter() override
			{
				return graph_.connection_state_presenter();
			}

			ReaperProjectContextBuilder& project_context_builder() override
			{
				return graph_.project_context_builder();
			}

			const ToolRegistryCompositionReport& tool_registry_report() const override
			{
				return graph_.tool_registry_report();
			}

			std::size_t unavailable_tool_call_count() const override
			{
				return graph_.tool_call_router().unavailable_call_count();
			}

		private:
			// Built in the initialiser list's last position, so every member it takes a
			// reference to is already constructed. A member function rather than an
			// inline brace-initialiser because the eleven host references read better
			// named than positional.
			ReaperExtensionObjectGraph::Dependencies build_graph_dependencies(
				ComposedExtensionDependencies dependencies)
			{
				ToolHostReferences hosts{
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

				return ReaperExtensionObjectGraph::Dependencies{
					*dependencies.outbound_queue,
					hosts,
					track_list_,
					project_context_source_,
					transport_actions_,
					std::move(dependencies.request_ui_state_publication),
					build_tool_call_codec<transport::InboundEnvelope, Json>(),
					build_tool_payload_codecs<Json>(),
					build_bridge_message_reader(),
					&ui::serialise_verbatim_bridge_payload<Json>
				};
			}

			// ---- the nine REAPER-backed adapters ----
			daw::ReaperFxHost fx_host_;
			daw::tools::ReaperItemHost item_host_;
			daw::tools::ReaperMarkerRegionHost marker_region_host_;
			daw::tools::ReaperRoutingHost routing_host_;
			daw::tools::ReaperTrackStateHost track_state_host_;
			daw::tools::ReaperTrackStructureHost track_structure_host_;

			// Render reaches the tool layer through the Coordinator rather than the host,
			// because `render` is the one tool whose logic is a component of its own.
			daw::ReaperRenderHost render_host_;
			daw::render_coordinator render_coordinator_;

			// ADR 0015's undo discipline, over REAPER's own stack.
			daw::reaper_undo_stack undo_stack_;
			daw::undo_manager undo_manager_;

			// Both faces of the alias store are needed: the two alias tools write through
			// the store, and `list_tracks` and the resolver read through the lookup.
			daw::ReaperAliasStorage alias_storage_;
			daw::AliasStore alias_store_;
			daw::StoredAliasLookup learned_aliases_;

			// ---- the three sources the graph reads rather than registers over ----
			daw::ReaperTrackListSource track_list_;
			context::ReaperProjectContextSource project_context_source_;
			transport::ReaperTransportActionInvoker transport_actions_;

			// Last, because it holds every one of the above by reference.
			ReaperExtensionObjectGraph graph_;
		};

	}

	std::unique_ptr<ComposedExtension> compose_extension(
		ComposedExtensionDependencies dependencies)
	{
		// The one refusal. See the declaration's comment: a graph with no outbound queue
		// could be built and could not answer anything, and an empty handle is a
		// condition the plugin entry already has a log line and a startup path for.
		if (dependencies.outbound_queue == nullptr) {
			return nullptr;
		}

		return std::unique_ptr<ComposedExtension>(
			new ReaperBackedExtension{std::move(dependencies)}
		);
	}

}
