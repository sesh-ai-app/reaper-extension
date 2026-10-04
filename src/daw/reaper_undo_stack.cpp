// The only translation unit in the Undo Manager that includes the REAPER SDK.
//
// Five calls, and one convention decision worth recording because the rest of the
// component is stated in terms of it.
//
// ---------------------------------------------------------------------------
// Positions and indices
//
// `undo_stack` is stated in *positions*: a count that goes up by one when an entry
// is recorded and down when one is undone, where the entry at position p is the one
// that took the stack from p to p + 1. `Undo_GetCurEntry` returns REAPER's current
// undo entry index and `Undo_GetEntryDesc(proj, index)` names the entry at an index,
// and this file maps one to the other directly.
//
// That mapping is the component's remaining SDK validation item — the same class of
// item the design records for `stems_via_master`. Both functions are declared in
// `reaper_plugin_functions.h` with a one-line comment and no statement of which end
// of the history index zero sits at, and the answer is not derivable from the header.
// It is also cheap to check in a running REAPER and cheap to correct here: the
// mapping is confined to this file, so if the index runs the other way the fix is the
// arithmetic in `entry_description_at` and nothing else. Nothing in
// `undo_manager.h`, and no test, depends on the convention.
//
// What does not depend on it either way: a description REAPER does not supply comes
// back empty, and an empty description is classified foreign. So a wrong mapping
// produces refusals — "I cannot identify what is in this range" — rather than an undo
// that walks through the producer's work believing it to be the agent's. The failure
// direction is the safe one by construction.

#include <daw/reaper_undo_stack.h>

#include <reaper_plugin.h>

namespace sesh_ai::daw
{
	namespace
	{
		using undo_begin_block_2_function = void (*)(ReaProject*);
		using undo_end_block_2_function = void (*)(ReaProject*, const char*, int);
		using undo_get_current_entry_function = int (*)(ReaProject*);
		using undo_get_entry_description_function = const char* (*)(ReaProject*, int);
		using undo_do_undo_2_function = int (*)(ReaProject*);

		// Null is REAPER's "the active project". See the header for why that is the
		// right target rather than a captured handle.
		ReaProject* const active_project = nullptr;

		void* resolve_reaper_function(reaper_plugin_info_t* plugin_info, const char* function_name)
		{
			if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
			{
				return nullptr;
			}

			return plugin_info->GetFunc(function_name);
		}
	}

	reaper_undo_stack::reaper_undo_stack(reaper_plugin_info_t* plugin_info)
		: undo_begin_block_2_{resolve_reaper_function(plugin_info, "Undo_BeginBlock2")},
		undo_end_block_2_{resolve_reaper_function(plugin_info, "Undo_EndBlock2")},
		undo_get_current_entry_{resolve_reaper_function(plugin_info, "Undo_GetCurEntry")},
		undo_get_entry_description_{resolve_reaper_function(plugin_info, "Undo_GetEntryDesc")},
		undo_do_undo_2_{resolve_reaper_function(plugin_info, "Undo_DoUndo2")}
	{
	}

	bool reaper_undo_stack::is_usable() const
	{
		return undo_begin_block_2_ != nullptr
			&& undo_end_block_2_ != nullptr
			&& undo_get_current_entry_ != nullptr
			&& undo_get_entry_description_ != nullptr
			&& undo_do_undo_2_ != nullptr;
	}

	int reaper_undo_stack::current_position()
	{
		if (undo_get_current_entry_ == nullptr)
		{
			return 0;
		}

		const int current_entry_index =
			reinterpret_cast<undo_get_current_entry_function>(undo_get_current_entry_)(active_project);

		// A project with no undo history reports a negative index in some REAPER
		// versions. Zero is the position the manager expects for "nothing recorded",
		// and it already clamps, but clamping at the seam keeps a negative from being
		// something every caller has to think about.
		return current_entry_index < 0 ? 0 : current_entry_index;
	}

	std::string reaper_undo_stack::entry_description_at(int position)
	{
		if (undo_get_entry_description_ == nullptr || position < 0)
		{
			return {};
		}

		const char* const description =
			reinterpret_cast<undo_get_entry_description_function>(undo_get_entry_description_)(
				active_project,
				position);

		// Empty rather than a placeholder. The manager decides what an unidentified
		// entry is called, and it classifies one as foreign — which is the safe
		// reading, and the reason this returns the absence rather than papering over
		// it.
		if (description == nullptr)
		{
			return {};
		}

		return std::string{description};
	}

	void reaper_undo_stack::begin_block()
	{
		if (undo_begin_block_2_ == nullptr)
		{
			return;
		}

		reinterpret_cast<undo_begin_block_2_function>(undo_begin_block_2_)(active_project);
	}

	void reaper_undo_stack::end_block(const std::string& description, int extra_flags)
	{
		if (undo_end_block_2_ == nullptr)
		{
			return;
		}

		reinterpret_cast<undo_end_block_2_function>(undo_end_block_2_)(
			active_project,
			description.c_str(),
			extra_flags);
	}

	bool reaper_undo_stack::undo_one_entry()
	{
		if (undo_do_undo_2_ == nullptr)
		{
			return false;
		}

		// REAPER returns non-zero when it performed an undo. False ends the walk
		// rather than being retried — a stack that will not move does not start
		// moving on the second ask, and the manager reports the walk as having
		// stopped short instead of spinning.
		return reinterpret_cast<undo_do_undo_2_function>(undo_do_undo_2_)(active_project) != 0;
	}
}
