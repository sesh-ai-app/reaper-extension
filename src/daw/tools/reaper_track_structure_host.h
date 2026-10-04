// The REAPER side of the track structure seam.
//
// SDK-free, like `daw/reaper_render_host.h` and `daw/reaper_alias_storage.h`, and for
// the same reason: `ReaProject` is forward-declared and only ever held as a pointer,
// so the Tool Executor can construct a REAPER-backed structure host without pulling
// reaper_plugin.h — which on macOS and Linux drags in WDL's swell, and which the test
// target has no include path for by design.
//
// `reaper_track_structure_host.cpp` is the only translation unit here that sees the
// SDK. Every decision — which index a track lands at, which delta each track ends up
// with, what the folder dissolution does — is in `track_structure_tools.h`, and this
// file is deliberately dull translation: one seam call in, one or two REAPER C API
// calls out. A change here that needs a branch on anything other than "did REAPER
// give me a value" belongs on the other side of the seam.
//
// The API functions are resolved through the `GetFunc` entry of the registration
// table REAPER hands the extension at load, rather than through the SDK's
// REAPERAPI_IMPLEMENT import mechanism, for the two reasons `reaper_render_host.h`
// records: REAPERAPI_IMPLEMENT needs one translation unit to provide storage for
// every imported pointer, and a missing function is then a condition this object can
// report rather than a null pointer call inside a producer's session.

#ifndef SESH_AI_DAW_TOOLS_REAPER_TRACK_STRUCTURE_HOST_H
#define SESH_AI_DAW_TOOLS_REAPER_TRACK_STRUCTURE_HOST_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <daw/tools/track_structure_tools.h>

// Declared by reaper_plugin.h as `class ReaProject;`, so naming it here refers to
// that type rather than introducing a second one.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw::tools
{
	// The `TrackStructureHost` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Every call is made on REAPER's main thread, inside the undo block the Tool
	// Executor opened — the Main Thread Dispatcher is the only thing that reaches this,
	// and requirement 22.1 keeps every REAPER C API call there.
	class ReaperTrackStructureHost final : public TrackStructureHost
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays
		// valid for the lifetime of the loaded extension. `project` is null for the
		// active project, which is REAPER's own convention for every function here.
		ReaperTrackStructureHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperTrackStructureHost() override;

		ReaperTrackStructureHost(const ReaperTrackStructureHost&) = delete;
		ReaperTrackStructureHost& operator=(const ReaperTrackStructureHost&) = delete;

		// False when any REAPER function the structural path needs could not be
		// resolved. A host that is not usable must not be handed to a handler: its
		// reads would report an empty project, so a folder repair would be computed
		// against a project that does not exist and written to tracks chosen from it.
		bool is_usable() const;

		// The names of the functions that could not be resolved, for the log line that
		// explains an unusable host.
		const std::vector<std::string>& unresolved_function_names() const;

		std::vector<structural_track> read_tracks_in_project_order() override;

		std::string insert_track(
			int index,
			const std::string& name,
			std::optional<int> color) override;

		bool delete_track(const std::string& guid) override;
		std::vector<std::string> duplicate_tracks(const std::vector<std::string>& guids) override;
		bool reorder_tracks(const std::vector<std::string>& guids_in_project_order) override;
		bool write_folder_depth_delta(const std::string& guid, int folder_depth_delta) override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_track_structure_api;

	private:
		std::unique_ptr<reaper_track_structure_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif  // SESH_AI_DAW_TOOLS_REAPER_TRACK_STRUCTURE_HOST_H
