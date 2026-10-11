// The REAPER side of the project context seam.
//
// This header deliberately does not include the SDK. `ReaProject` is
// forward-declared and only ever held as a pointer, so everything except
// reaper_project_context_source.cpp can talk about a REAPER-backed source without
// pulling reaper_plugin.h — which on macOS and Linux drags in WDL's swell, and which
// the test target has no include path for.
//
// Same shape as `entry/reaper_timer_registrar.h` and `daw/reaper_render_host.h`: the
// interface and this header stay SDK-free, one `reaper_*.cpp` is where the SDK
// lives, and the API functions are resolved through the registration table's
// `GetFunc` rather than through REAPERAPI_IMPLEMENT — so a REAPER build missing one
// of them is a condition this object reports rather than a null pointer call inside a
// producer's session.
//
// All the decisions are on the other side of the seam, in project_context_builder.h.
// This file reads values. The one judgement it makes is which sample rate is the
// project's, and it is documented where it happens.

#ifndef SESH_AI_CONTEXT_REAPER_PROJECT_CONTEXT_SOURCE_H
#define SESH_AI_CONTEXT_REAPER_PROJECT_CONTEXT_SOURCE_H

#include <context/project_context_builder.h>

#include <memory>
#include <string>
#include <vector>

// Declared by reaper_plugin.h as `class ReaProject`, so this is a forward declaration
// of the same type rather than a second one. Declaring it as `struct` would compile
// and then produce a mismatched-tag warning under MSVC.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::context
{
	// The `ProjectContextSource` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it reads.
	// Every read happens on REAPER's main thread, inside the dispatcher's tick, because
	// that is the only legal window for a REAPER C API call (requirement 22.3).
	class ReaperProjectContextSource final : public ProjectContextSource
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays
		// valid for the lifetime of the loaded extension. `project` is null for the
		// active project, which is REAPER's own convention for every function used here.
		ReaperProjectContextSource(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperProjectContextSource() override;

		ReaperProjectContextSource(const ReaperProjectContextSource&) = delete;
		ReaperProjectContextSource& operator=(const ReaperProjectContextSource&) = delete;

		// False when any REAPER function the snapshot needs could not be resolved. A
		// source that is not usable reports its readings unreadable rather than
		// returning an empty project, because an agent told the session has no tracks
		// will act on that.
		bool is_usable() const;

		// The names of the functions that could not be resolved, for the log line that
		// explains an unusable source.
		const std::vector<std::string>& unresolved_function_names() const;

		ProjectContextReading read_project_context() override;
		int read_project_state_change_count() override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_project_context_api;

	private:
		std::unique_ptr<reaper_project_context_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif
