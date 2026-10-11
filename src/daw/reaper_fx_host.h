// The REAPER side of the FX seam.
//
// This header deliberately does not include the SDK. `ReaProject` is forward-declared
// and only ever held as a pointer, so everything except reaper_fx_host.cpp can talk
// about a REAPER-backed FX host without pulling reaper_plugin.h — which on macOS and
// Linux drags in WDL's swell, and which the test target has no include path for.
//
// Same shape as `reaper_render_host.h` and `entry/reaper_timer_registrar.h`: the
// interface and this header stay SDK-free, and one `*_reaper_*.cpp` is where the SDK
// lives. The interface itself, and every decision the FX tools make, is in
// tools/fx_tools.h.
//
// The API functions are resolved through the `GetFunc` entry of the registration table
// REAPER hands the extension at load, rather than through the SDK's REAPERAPI_IMPLEMENT
// import mechanism. Two reasons, both the same as the render host's:
//
//   - REAPERAPI_IMPLEMENT needs exactly one translation unit to provide storage for
//     every imported pointer, and choosing which one belongs with the plugin entry
//     (task 17.1). Resolving locally means this file does not depend on that decision
//     having been made, and does not force it.
//   - A missing function is then a condition this object can report, rather than a null
//     pointer call inside a producer's session. `is_usable` is false when anything the
//     FX path needs could not be resolved, and every FX handler checks it first and
//     fails the action with a reason that names the functions REAPER did not supply.
//     That matters more here than for most seams: a silently unusable FX host reports
//     empty chains and drops writes, so the producer would be told an FX was added to a
//     chain that never received one.

#ifndef SESH_AI_DAW_REAPER_FX_HOST_H
#define SESH_AI_DAW_REAPER_FX_HOST_H

#include <daw/tools/fx_tools.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Declared by reaper_plugin.h as `class`, so this is a forward declaration of the same
// type rather than a second one. Declaring it as `struct` would compile and then produce
// a mismatched-tag warning under MSVC.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw
{
	// The `fx_host` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Everything that decides anything — the range checks, the match ranking, the
	// removal ordering, the read caps — is in tools/fx_tools.h; this is the translation
	// layer, and it is deliberately dull, because it is the one part of this component
	// the suite cannot reach.
	//
	// Tracks and items are addressed by GUID across the seam, so the lookups from GUID
	// to `MediaTrack*` and `MediaItem*` happen here. That costs a walk of the project per
	// call, which is the right trade: the alternative is SDK pointers in a header the
	// test target compiles.
	class ReaperFxHost final : public tools::fx_host
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays valid
		// for the lifetime of the loaded extension. `project` is null for the active
		// project, which is REAPER's own convention for every function here.
		ReaperFxHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperFxHost() override;

		ReaperFxHost(const ReaperFxHost&) = delete;
		ReaperFxHost& operator=(const ReaperFxHost&) = delete;
		ReaperFxHost(ReaperFxHost&&) = delete;
		ReaperFxHost& operator=(ReaperFxHost&&) = delete;

		bool is_usable() const override;
		std::vector<std::string> unresolved_function_names() const override;

		std::vector<tools::fx_chain_entry> read_track_fx(const std::string& track_guid) override;
		std::vector<tools::fx_parameter> read_fx_parameters(
			const std::string& track_guid,
			int fx_index) override;
		std::vector<tools::installed_fx> read_installed_fx() override;
		std::vector<std::string> read_track_item_guids(const std::string& track_guid) override;

		std::optional<int> insert_fx(
			const std::string& track_guid,
			const std::string& fx_identifier,
			std::optional<int> chain_position) override;
		bool delete_fx(const std::string& track_guid, int fx_index) override;
		bool set_fx_bypassed(const std::string& track_guid, int fx_index, bool bypassed) override;
		bool set_fx_parameter_value(
			const std::string& track_guid,
			int fx_index,
			int parameter_index,
			double value) override;
		bool apply_fx_to_item(const std::string& item_guid, bool replace_existing_take) override;

		// The resolved function pointers. Declared but not defined here: the definition is
		// in the translation unit, so the SDK types its members are made of never reach
		// this header. Public only because the translation unit's own helpers name the
		// type; nothing outside can do anything with an incomplete struct.
		struct reaper_fx_api;

	private:
		std::unique_ptr<reaper_fx_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif
