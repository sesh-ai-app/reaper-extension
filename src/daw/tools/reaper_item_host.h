// The REAPER side of the item seam.
//
// SDK-free, like `reaper_track_structure_host.h`, `daw/reaper_fx_host.h` and
// `daw/reaper_render_host.h`, and for the same reason: `ReaProject` is forward-declared
// and only ever held as a pointer, so the Tool Executor can construct a REAPER-backed
// item host without pulling reaper_plugin.h — which on macOS and Linux drags in WDL's
// swell, and which the test target has no include path for by design.
//
// `reaper_item_host.cpp` is the only translation unit here that sees the SDK. Every
// decision — which items are in scope, where an offset lands, whether a split position
// falls inside an item, which of the three import failures a path has, what a result
// reports — is in `item_tools.h`, and this file is deliberately dull translation: one
// seam call in, one or two REAPER C API calls out. A change here that needs a branch on
// anything other than "did REAPER give me a value" belongs on the other side of the
// seam.
//
// The API functions are resolved through the `GetFunc` entry of the registration table
// REAPER hands the extension at load, rather than through the SDK's REAPERAPI_IMPLEMENT
// import mechanism, for the two reasons `reaper_render_host.h` records: REAPERAPI_IMPLEMENT
// needs one translation unit to provide storage for every imported pointer, and a
// missing function is then a condition this object can report rather than a null pointer
// call inside a producer's session.
//
// That second reason matters more here than for most seams. An item host missing a
// function reports a project with no items in it, which would have `list_selected_items`
// tell the producer their selection is empty and have `delete_items` report nothing to
// delete — both plausible answers, and both wrong. `is_usable` is false when anything
// the item path needs could not be resolved, and every item handler checks it first and
// fails the action with a reason naming the functions REAPER did not supply.

#ifndef SESH_AI_DAW_TOOLS_REAPER_ITEM_HOST_H
#define SESH_AI_DAW_TOOLS_REAPER_ITEM_HOST_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <daw/tools/item_tools.h>

// Declared by reaper_plugin.h as `class ReaProject;`, so naming it here refers to that
// type rather than introducing a second one.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`.
struct reaper_plugin_info_t;

namespace sesh_ai::daw::tools
{
	// The `item_host` REAPER actually backs.
	//
	// Holds no state beyond the resolved function pointers and the project it operates
	// on. Every call is made on REAPER's main thread, inside the undo block the Tool
	// Executor opened — the Main Thread Dispatcher is the only thing that reaches this,
	// and requirement 22.1 keeps every REAPER C API call there.
	//
	// Items are addressed by GUID across the seam, so the lookup from GUID to
	// `MediaItem*` happens here. That costs a walk of the project per mutation, which is
	// the right trade: the alternative is SDK pointers in a header the test target
	// compiles, and an index held between two calls that any edit can invalidate.
	class ReaperItemHost final : public item_host
	{
	public:
		// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays valid
		// for the lifetime of the loaded extension. `project` is null for the active
		// project, which is REAPER's own convention for every function here.
		ReaperItemHost(reaper_plugin_info_t* plugin_info, ReaProject* project);
		~ReaperItemHost() override;

		ReaperItemHost(const ReaperItemHost&) = delete;
		ReaperItemHost& operator=(const ReaperItemHost&) = delete;
		ReaperItemHost(ReaperItemHost&&) = delete;
		ReaperItemHost& operator=(ReaperItemHost&&) = delete;

		bool is_usable() const override;
		std::vector<std::string> unresolved_function_names() const override;

		std::vector<item_state> read_project_items() override;
		std::optional<item_time_range> read_time_selection() override;
		item_file_access probe_file(const std::string& file_path) override;

		bool set_item_position(const std::string& item_guid, double position_seconds) override;
		bool set_item_length(const std::string& item_guid, double length_seconds) override;
		bool set_item_fade_in(const std::string& item_guid, double fade_seconds) override;
		bool set_item_fade_out(const std::string& item_guid, double fade_seconds) override;
		bool set_item_volume(const std::string& item_guid, double reaper_volume) override;
		bool set_item_muted(const std::string& item_guid, bool muted) override;

		bool move_item_to_track(const std::string& item_guid, const std::string& track_guid) override;

		std::optional<item_split_halves> split_item(
			const std::string& item_guid,
			double position_seconds) override;

		bool delete_item(const std::string& item_guid) override;

		std::optional<imported_media> import_media_file(
			const std::string& track_guid,
			const std::string& file_path,
			double position_seconds) override;

		// The resolved function pointers. Declared but not defined here: the definition is
		// in the translation unit, so the SDK types its members are made of never reach
		// this header. Public only because the translation unit's own helpers name the
		// type; nothing outside can do anything with an incomplete struct.
		struct reaper_item_api;

	private:
		std::unique_ptr<reaper_item_api> api_;
		ReaProject* project_;
		std::vector<std::string> unresolved_function_names_;
	};
}

#endif  // SESH_AI_DAW_TOOLS_REAPER_ITEM_HOST_H
