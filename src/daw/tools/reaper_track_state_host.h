// The REAPER side of the track state seam.
//
// SDK-free, like `daw/tools/reaper_track_structure_host.h` and
// `daw/reaper_render_host.h`, and for the same reason: `ReaProject` is
// forward-declared and only ever held as a pointer, so the Tool Executor can construct
// a REAPER-backed track state host without pulling reaper_plugin.h — which on macOS and
// Linux drags in WDL's swell, and which the test target has no include path for by
// design.
//
// `reaper_track_state_host.cpp` is the only translation unit here that sees the SDK.
// Every decision — which units cross the wire, which role a track has, which entry of
// an action array failed and why — is in `track_state_tools.h`, and this file is
// deliberately dull translation: one seam call in, one or two REAPER C API calls out. A
// change here that needs a branch on anything other than "did REAPER give me a value"
// belongs on the other side of the seam.
//
// Two translations do happen here, and both are about REAPER's own storage rather than
// about the tools:
//
//   `I_CUSTOMCOLOR` is only honoured with its high bit set, and REAPER stores a colour
//   it was told to ignore. The flag is applied on the way in and read on the way out
//   through `marker_region_tools.h`'s `reaper_custom_color_from` and
//   `color_from_reaper_custom_color`, which are the same two functions the marker and
//   region host uses. One implementation of the rule rather than two: a second would be
//   the one that drifts, and the symptom would be a producer looking at a default
//   colour after a call that reported setting one.
//
//   `I_SOLO` carries more than a boolean — REAPER distinguishes solo from solo in
//   place — while the schema carries `soloed`. Reading collapses anything non-zero to
//   true and writing sets plain solo, which is stated here because it is the one place
//   the collapse happens.
//
// The API functions are resolved through the `GetFunc` entry of the registration table
// REAPER hands the extension at load, rather than through the SDK's REAPERAPI_IMPLEMENT
// import mechanism, for the two reasons `reaper_render_host.h` records:
// REAPERAPI_IMPLEMENT needs one translation unit to provide storage for every imported
// pointer, and a missing function is then a condition this object can report rather
// than a null pointer call inside a producer's session.

#ifndef SESH_AI_DAW_TOOLS_REAPER_TRACK_STATE_HOST_H
#define SESH_AI_DAW_TOOLS_REAPER_TRACK_STATE_HOST_H

#include <memory>
#include <string>
#include <vector>

#include <daw/tools/track_state_tools.h>

// Declared by reaper_plugin.h as `class ReaProject;`, so naming it here refers to that
// type rather than introducing a second one.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw::tools
{
	// The `track_state_host` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Every call is made on REAPER's main thread — for `set_track_state`, inside the
	// undo block the Tool Executor opened — because the Main Thread Dispatcher is the
	// only thing that reaches this and requirement 22.1 keeps every REAPER C API call
	// there.
	class ReaperTrackStateHost final : public track_state_host
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays valid
		// for the lifetime of the loaded extension. `project` is null for the active
		// project, which is REAPER's own convention for every function here.
		ReaperTrackStateHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperTrackStateHost() override;

		ReaperTrackStateHost(const ReaperTrackStateHost&) = delete;
		ReaperTrackStateHost& operator=(const ReaperTrackStateHost&) = delete;

		// False when any REAPER function the track state path needs could not be
		// resolved. A host that is not usable must not be handed to a handler: its reads
		// report an empty project, which would have `list_tracks` tell the producer they
		// have no tracks, and its writes report failure after the undo block has already
		// been opened.
		bool is_usable() const override;

		std::vector<std::string> unresolved_function_names() const override;

		std::vector<track_state_reading> read_tracks_in_project_order(bool include_fx_names) override;

		project_summary_reading read_project_summary() override;

		bool write_track_name(const std::string& track_guid, const std::string& name) override;
		bool write_track_volume(const std::string& track_guid, double reaper_volume) override;
		bool write_track_pan(const std::string& track_guid, double reaper_pan) override;
		bool write_track_muted(const std::string& track_guid, bool muted) override;
		bool write_track_soloed(const std::string& track_guid, bool soloed) override;
		bool write_track_armed(const std::string& track_guid, bool armed) override;
		bool write_track_color(const std::string& track_guid, int color) override;

		// The resolved function pointers. Declared but not defined here: the definition
		// is in the translation unit, so the SDK types its members are made of never
		// reach this header. Public only because the translation unit's own helpers name
		// the type; nothing outside can do anything with an incomplete struct.
		struct reaper_track_state_api;

	private:
		std::unique_ptr<reaper_track_state_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif  // SESH_AI_DAW_TOOLS_REAPER_TRACK_STATE_HOST_H
