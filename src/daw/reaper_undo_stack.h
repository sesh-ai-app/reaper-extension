// The REAPER side of the undo seam.
//
// SDK-free, following `entry/reaper_timer_registrar.h`: `reaper_plugin_info_t` is
// forward-declared and only ever held as a pointer, so the plugin entry can
// construct one of these without `reaper_plugin.h` reaching any header the test
// target compiles. `reaper_undo_stack.cpp` is the only translation unit in this
// component that includes the SDK.
//
// The six API functions are resolved through REAPER's own `GetFunc` at construction
// rather than through the SDK's `REAPERAPI_LoadAPI` import table. That is a
// deliberate independence from task 17.1: the import table needs exactly one
// translation unit defining `REAPERAPI_IMPLEMENT` to give the function pointers
// storage, and that belongs with the plugin entry. Resolving here means this
// component works whichever way the entry point ends up arranged, and it makes a
// REAPER too old to expose one of the functions a reported condition rather than a
// null call through a pointer nobody checked.

#ifndef SESH_AI_DAW_REAPER_UNDO_STACK_H
#define SESH_AI_DAW_REAPER_UNDO_STACK_H

#include <string>

#include <daw/undo_manager.h>

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`, so naming the struct here is a forward declaration of the
// same type rather than a second one.
struct reaper_plugin_info_t;

namespace sesh_ai::daw
{
	// REAPER's undo stack for the active project.
	//
	// The active project rather than a captured `ReaProject*`: REAPER's undo API takes
	// null to mean "whatever project is in front of the producer", and a tool call
	// applies to the session they are looking at. A captured handle would go stale the
	// moment they switched tabs, and applying an undo to a project they are not
	// watching is exactly the silent damage this component exists to prevent.
	class reaper_undo_stack final : public undo_stack
	{
	public:
		// The pointer REAPER passed to `ReaperPluginEntry`. It stays valid for the
		// lifetime of the loaded extension, which outlives this object.
		explicit reaper_undo_stack(reaper_plugin_info_t* plugin_info);

		// False when REAPER did not supply every function needed. Worth checking
		// before a turn rather than discovering it mid-block: with the undo API
		// unavailable, every call below is a no-op, so a tool would mutate the project
		// with nothing recording it — the one state in which the producer cannot
		// recover. The Tool Executor should refuse to mutate rather than proceed.
		bool is_usable() const;

		int current_position() override;
		std::string entry_description_at(int position) override;
		void begin_block() override;
		void end_block(const std::string& description, int extra_flags) override;
		bool undo_one_entry() override;

	private:
		// Deliberately not the SDK's typedefs — naming these here would need
		// `reaper_plugin_functions.h`, which is what this header exists to avoid. The
		// signatures are declared in the translation unit and these are the resolved
		// addresses.
		void* undo_begin_block_2_ = nullptr;
		void* undo_end_block_2_ = nullptr;
		void* undo_get_current_entry_ = nullptr;
		void* undo_get_entry_description_ = nullptr;
		void* undo_do_undo_2_ = nullptr;
	};
}

#endif
