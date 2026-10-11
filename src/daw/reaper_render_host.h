// The REAPER side of the render seam.
//
// This header deliberately does not include the SDK. `ReaProject` is
// forward-declared and only ever held as a pointer, so everything except
// reaper_render_host.cpp can talk about a REAPER-backed render host without pulling
// reaper_plugin.h — which on macOS and Linux drags in WDL's swell, and which the
// test target has no include path for.
//
// Same shape as `entry/reaper_timer_registrar.h`: the interface and this header stay
// SDK-free, and one `*_reaper_*.cpp` is where the SDK lives. The interface itself,
// and all of the component's logic, is in render_coordinator.h.
//
// The API functions are resolved through the `GetFunc` entry of the registration
// table REAPER hands the extension at load, rather than through the SDK's
// REAPERAPI_IMPLEMENT import mechanism. Two reasons, both the same as the timer
// registrar's:
//
//   - REAPERAPI_IMPLEMENT needs exactly one translation unit to provide storage for
//     every imported pointer, and choosing which one belongs with the plugin entry
//     (task 17.1). Resolving locally means this file does not depend on that decision
//     having been made, and does not force it.
//   - A missing function is then a condition this object can report, rather than a
//     null pointer call inside a producer's session. `is_usable` is false when
//     anything the render path needs could not be resolved, and the render tool can
//     say so instead of crashing REAPER.

#ifndef SESH_AI_DAW_REAPER_RENDER_HOST_H
#define SESH_AI_DAW_REAPER_RENDER_HOST_H

#include <daw/render_coordinator.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Both are declared by reaper_plugin.h as `class`, so these are forward declarations
// of the same types rather than second ones. Declaring them as `struct` would compile
// and then produce a mismatched-tag warning under MSVC.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw
{
	// The `RenderHost` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Everything that decides anything is in `render_coordinator`; this is the
	// translation layer, and it is deliberately dull — it is the one part of this
	// component the suite cannot reach.
	class ReaperRenderHost final : public RenderHost
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays
		// valid for the lifetime of the loaded extension. `project` is null for the
		// active project, which is REAPER's own convention for every function here.
		ReaperRenderHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperRenderHost() override;

		ReaperRenderHost(const ReaperRenderHost&) = delete;
		ReaperRenderHost& operator=(const ReaperRenderHost&) = delete;

		// False when any REAPER function the render path needs could not be resolved.
		// A host that is not usable must not be handed to a coordinator: its reads
		// would report an empty project and its writes would do nothing, which would
		// queue a render of silence rather than failing.
		bool is_usable() const;

		// The names of the functions that could not be resolved, for the log line that
		// explains an unusable host.
		const std::vector<std::string>& unresolved_function_names() const;

		render_project_state read_project_state() override;
		std::vector<render_track> read_selected_tracks() override;
		std::optional<time_span> find_region_span(const std::string& region_guid) override;
		std::string read_render_directory() override;

		void write_render_settings(render_settings_word settings) override;
		void write_render_bounds(int bounds_flag, const time_span& span) override;
		void write_render_sample_rate(int sample_rate) override;
		void write_render_format(const render_format_request& format) override;
		void write_render_output(
			const std::string& directory,
			const std::string& file_name_pattern) override;

		std::string read_render_targets() override;
		bool output_file_exists(const std::string& resolved_path) override;

		void set_track_solo(const std::string& track_guid, bool soloed) override;
		void add_project_to_render_queue() override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_render_api;

	private:
		std::unique_ptr<reaper_render_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif
