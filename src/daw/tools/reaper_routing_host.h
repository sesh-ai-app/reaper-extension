// The REAPER side of the routing seam.
//
// Same shape as `daw/reaper_render_host.h`, and for the same reasons. This header does
// not include the SDK: `ReaProject` is forward-declared and only ever held as a
// pointer, so everything except reaper_routing_host.cpp can talk about a REAPER-backed
// routing host without pulling reaper_plugin.h — which on macOS and Linux drags in
// WDL's swell, and which the test target has no include path for. The interface itself,
// and every decision the seven routing tools make, is in routing_tools.h.
//
// The API functions are resolved through the `GetFunc` entry of the registration table
// REAPER hands the extension at load rather than through the SDK's
// REAPERAPI_IMPLEMENT import mechanism. Two reasons, both the render host's:
//
//   - REAPERAPI_IMPLEMENT needs exactly one translation unit to provide storage for
//     every imported pointer, and choosing which one belongs with the plugin entry
//     (task 17.1). Resolving locally means this file does not depend on that decision
//     having been made, and does not force it.
//   - A missing function is then a condition this object can report rather than a null
//     pointer call inside a producer's session. `is_usable` is false when anything the
//     routing path needs could not be resolved, and the tools can say so instead of
//     crashing REAPER.

#ifndef SESH_AI_DAW_TOOLS_REAPER_ROUTING_HOST_H
#define SESH_AI_DAW_TOOLS_REAPER_ROUTING_HOST_H

#include <daw/tools/routing_tools.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Declared by reaper_plugin.h as `class`, so this is a forward declaration of the same
// type rather than a second one. Declaring it as `struct` would compile and then
// produce a mismatched-tag warning under MSVC.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw::tools
{
	// The `routing_host` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Everything that decides anything is in routing_tools.h; this is the
	// translation layer, and it is deliberately dull — it is the one part of these seven
	// tools the suite cannot reach.
	class ReaperRoutingHost final : public routing_host
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays valid
		// for the lifetime of the loaded extension. `project` is null for the active
		// project, which is REAPER's own convention for every function here.
		ReaperRoutingHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperRoutingHost() override;

		ReaperRoutingHost(const ReaperRoutingHost&) = delete;
		ReaperRoutingHost& operator=(const ReaperRoutingHost&) = delete;
		ReaperRoutingHost(ReaperRoutingHost&&) = delete;
		ReaperRoutingHost& operator=(ReaperRoutingHost&&) = delete;

		// False when any REAPER function the routing path needs could not be resolved. A
		// host that is not usable must not be handed to the tools: its reads would report
		// a session with no tracks and its writes would do nothing, so a cycle check
		// would be run against an empty project and permit anything.
		bool is_usable() const;

		// The names of the functions that could not be resolved, for the log line that
		// explains an unusable host.
		const std::vector<std::string>& unresolved_function_names() const;

		routing_snapshot read_routing_snapshot() override;
		std::optional<routing_send> read_send(const std::string& source_track_guid, int send_index) override;

		std::optional<int> create_send(
			const std::string& source_track_guid,
			const std::string& destination_track_guid) override;
		bool remove_send(const std::string& source_track_guid, int send_index) override;
		bool write_send_state(
			const std::string& source_track_guid,
			int send_index,
			const send_state_change& change) override;

		bool write_parent_send(const std::string& track_guid, bool enabled) override;

		std::optional<TrackReference> insert_track(int project_index, const std::string& name) override;
		bool delete_track(const std::string& track_guid) override;
		bool apply_track_order(const std::vector<std::string>& track_guids_in_project_order) override;
		bool write_folder_depth(const std::string& track_guid, int folder_depth_delta) override;

		std::optional<routing_fx_placement> add_fx(
			const std::string& track_guid,
			const std::string& fx_name) override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_routing_api;

	private:
		// The tools address a track by GUID, because that is the only identity that
		// survives the structural edits these tools make. REAPER addresses it by index, so
		// every call here starts with a walk. `MediaTrack` is the SDK's opaque handle,
		// forward-declared by `daw/object_resolver.h`.
		MediaTrack* find_track_by_guid(const std::string& track_guid);
		int find_track_index_by_guid(const std::string& track_guid);

		std::unique_ptr<reaper_routing_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif
