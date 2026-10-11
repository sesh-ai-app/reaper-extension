// The REAPER side of the marker, region, tempo map, and time selection seam.
//
// SDK-free, like `daw/reaper_render_host.h` and
// `daw/tools/reaper_track_structure_host.h`, and for the same reason: `ReaProject` is
// forward-declared and only ever held as a pointer, so the Tool Executor can construct
// a REAPER-backed host without pulling reaper_plugin.h — which on macOS and Linux drags
// in WDL's swell, and which the test target has no include path for by design.
//
// `reaper_marker_region_host.cpp` is the only translation unit here that sees the SDK.
// Every decision — whether an object is a marker or a region, whether a span is
// well-formed, which tempo point index a write lands on, how many items moved — is in
// `marker_region_tools.h`, and this file is deliberately dull translation: one seam call
// in, one or two REAPER C API calls out. A change here that needs a branch on anything
// other than "did REAPER give me a value" belongs on the other side of the seam.
//
// The API functions are resolved through the `GetFunc` entry of the registration table
// REAPER hands the extension at load, rather than through the SDK's REAPERAPI_IMPLEMENT
// import mechanism, for the two reasons `reaper_render_host.h` records:
// REAPERAPI_IMPLEMENT needs one translation unit to provide storage for every imported
// pointer, and a missing function is then a condition this object can report rather
// than a null pointer call inside a producer's session.

#ifndef SESH_AI_DAW_TOOLS_REAPER_MARKER_REGION_HOST_H
#define SESH_AI_DAW_TOOLS_REAPER_MARKER_REGION_HOST_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <daw/tools/marker_region_tools.h>

// Declared by reaper_plugin.h as `class ReaProject;`, so naming it here refers to that
// type rather than introducing a second one.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw::tools
{
	// The `MarkerRegionHost` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Every call is made on REAPER's main thread, inside the undo block the Tool
	// Executor opened — the Main Thread Dispatcher is the only thing that reaches this,
	// and requirement 22.1 keeps every REAPER C API call there.
	class ReaperMarkerRegionHost final : public MarkerRegionHost
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays valid
		// for the lifetime of the loaded extension. `project` is null for the active
		// project, which is REAPER's own convention for every function here.
		ReaperMarkerRegionHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperMarkerRegionHost() override;

		ReaperMarkerRegionHost(const ReaperMarkerRegionHost&) = delete;
		ReaperMarkerRegionHost& operator=(const ReaperMarkerRegionHost&) = delete;

		// False when any REAPER function the tools over this seam need could not be
		// resolved. A host that is not usable must not be handed to a handler: its reads
		// would report a session with no markers, no regions, and an empty tempo map, so
		// an update would resolve nothing and a tempo map write would be computed
		// against a session that does not exist.
		bool is_usable() const;

		// The names of the functions that could not be resolved, for the log line that
		// explains an unusable host.
		const std::vector<std::string>& unresolved_function_names() const;

		std::vector<marker_or_region> read_markers_and_regions_in_enumeration_order() override;

		std::optional<marker_or_region> create_marker_or_region(
			const marker_or_region_creation& creation) override;

		bool write_marker_or_region(const marker_or_region_write& write) override;
		bool delete_marker_or_region(const std::string& guid) override;

		std::vector<tempo_map_point> read_tempo_map() override;

		std::optional<initial_tempo_and_time_signature>
			read_initial_tempo_and_time_signature() override;

		bool write_tempo_point(
			std::optional<std::size_t> replacing_index,
			const tempo_map_point& point) override;

		bool clear_tempo_map() override;
		std::vector<double> read_item_positions() override;
		bool write_time_selection(double start_seconds, double end_seconds) override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_marker_region_api;

	private:
		std::unique_ptr<reaper_marker_region_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif  // SESH_AI_DAW_TOOLS_REAPER_MARKER_REGION_HOST_H
