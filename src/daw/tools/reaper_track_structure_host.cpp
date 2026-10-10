// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: a `TrackStructureHost` call in, one or two REAPER C
// API calls out. No decisions — which index a track lands at, which delta every track
// ends up with, and what deleting a folder parent does to its children are all
// decided in track_structure_tools.h against the Folder Invariant Keeper, which is
// what makes them testable without REAPER.
//
// The function pointers are resolved from the registration table's `GetFunc` rather
// than through REAPERAPI_IMPLEMENT. See reaper_track_structure_host.h for why.

#include <daw/tools/reaper_track_structure_host.h>

#include <reaper_plugin.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace
{
	// `guidToString` documents a 64-byte destination.
	constexpr std::size_t guid_buffer_size = 64;

	// Track names. 512 is the `maxLength` every output schema puts on a track name, and
	// REAPER truncates to the buffer rather than overrunning it.
	constexpr std::size_t name_buffer_size = 1024;

	// "Track: Duplicate tracks".
	//
	// Built-in REAPER actions are addressed by numeric command ID — `NamedCommandLookup`
	// resolves the `_RS...` identifiers that scripts and extensions register, not these.
	// This is the ID community ReaScripts use for duplicating a track selection, and
	// like the render queue command in reaper_render_host.cpp it is a value that cannot
	// be checked from the SDK headers: the action list is REAPER's, not the SDK's. It
	// belongs in the SDK validation step alongside ADR 0018's other open items, and it
	// is named here rather than written inline so that verifying it is a one-line
	// change.
	constexpr int duplicate_tracks_command = 40062;

	// `I_CUSTOMCOLOR` is only honoured with the high bit set. Without it REAPER stores
	// the value and ignores it, which would leave the producer looking at a default
	// colour after a call that reported setting one.
	constexpr int custom_color_enabled_flag = 0x1000000;

	// `ReorderSelectedTracks`'s `makePrevFolder`: 0 leaves the folder structure alone.
	// The deltas are written separately from the plan the Keeper repaired, and letting
	// the reorder make its own folder decision would overwrite that plan with REAPER's
	// guess.
	constexpr int reorder_without_changing_folders = 0;

	// `TrackList_AdjustWindows`'s `isMinor`: false redraws the track control panels as
	// well as the arrange view, which is what a structural change needs.
	constexpr bool adjust_windows_fully = false;

	// `GetTrackNumSends`'s category: 0 is sends, which is the only one
	// `duplicate_track`'s result reports. Receives and hardware outputs are the routing
	// tools' business.
	constexpr int send_category = 0;
}

namespace sesh_ai::daw::tools
{
	// The functions this component needs, and only those. Resolved once at
	// construction; every one of them is required, because a structural path missing
	// any of them computes a folder repair against a project it could not read.
	struct ReaperTrackStructureHost::reaper_track_structure_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		int (*CountSelectedTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetSelectedTrack)(ReaProject*, int) = nullptr;

		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;
		double (*GetMediaTrackInfo_Value)(MediaTrack*, const char*) = nullptr;
		bool (*SetMediaTrackInfo_Value)(MediaTrack*, const char*, double) = nullptr;

		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		int (*TrackFX_GetCount)(MediaTrack*) = nullptr;
		int (*GetTrackNumSends)(MediaTrack*, int) = nullptr;

		void (*InsertTrackAtIndex)(int, bool) = nullptr;
		void (*DeleteTrack)(MediaTrack*) = nullptr;
		bool (*ReorderSelectedTracks)(int, int) = nullptr;
		void (*TrackList_AdjustWindows)(bool) = nullptr;

		void (*SetOnlyTrackSelected)(MediaTrack*) = nullptr;
		void (*SetTrackSelected)(MediaTrack*, bool) = nullptr;

		void (*Main_OnCommand)(int, int) = nullptr;
	};

	namespace
	{
		// Resolves one function and records the name when it is missing, so an unusable
		// host can say which functions REAPER did not supply rather than only that it is
		// unusable.
		template <typename FunctionPointer>
		void resolve_function(
			reaper_plugin_info_t* plugin_info,
			const char* name,
			FunctionPointer& destination,
			std::vector<std::string>& unresolved_names)
		{
			void* const resolved = plugin_info->GetFunc(name);

			if (resolved == nullptr)
			{
				unresolved_names.emplace_back(name);
				return;
			}

			destination = reinterpret_cast<FunctionPointer>(resolved);
		}

		std::string read_track_guid(
			const ReaperTrackStructureHost::reaper_track_structure_api& api,
			MediaTrack* track)
		{
			char guid_buffer[guid_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "GUID", guid_buffer, false))
			{
				return {};
			}

			return std::string{guid_buffer};
		}

		std::string read_track_name(
			const ReaperTrackStructureHost::reaper_track_structure_api& api,
			MediaTrack* track)
		{
			char name_buffer[name_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "P_NAME", name_buffer, false))
			{
				return {};
			}

			return std::string{name_buffer};
		}
	}

	ReaperTrackStructureHost::ReaperTrackStructureHost(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project)
		: api_{std::make_unique<reaper_track_structure_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);
		resolve_function(plugin_info, "CountSelectedTracks", api_->CountSelectedTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetSelectedTrack", api_->GetSelectedTrack, unresolved_function_names_);

		resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaTrackInfo_Value", api_->GetMediaTrackInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaTrackInfo_Value", api_->SetMediaTrackInfo_Value, unresolved_function_names_);

		resolve_function(plugin_info, "CountTrackMediaItems", api_->CountTrackMediaItems, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetCount", api_->TrackFX_GetCount, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrackNumSends", api_->GetTrackNumSends, unresolved_function_names_);

		resolve_function(plugin_info, "InsertTrackAtIndex", api_->InsertTrackAtIndex, unresolved_function_names_);
		resolve_function(plugin_info, "DeleteTrack", api_->DeleteTrack, unresolved_function_names_);
		resolve_function(plugin_info, "ReorderSelectedTracks", api_->ReorderSelectedTracks, unresolved_function_names_);
		resolve_function(plugin_info, "TrackList_AdjustWindows", api_->TrackList_AdjustWindows, unresolved_function_names_);

		resolve_function(plugin_info, "SetOnlyTrackSelected", api_->SetOnlyTrackSelected, unresolved_function_names_);
		resolve_function(plugin_info, "SetTrackSelected", api_->SetTrackSelected, unresolved_function_names_);

		resolve_function(plugin_info, "Main_OnCommand", api_->Main_OnCommand, unresolved_function_names_);
	}

	ReaperTrackStructureHost::~ReaperTrackStructureHost() = default;

	bool ReaperTrackStructureHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	const std::vector<std::string>& ReaperTrackStructureHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	std::vector<structural_track> ReaperTrackStructureHost::read_tracks_in_project_order()
	{
		std::vector<structural_track> tracks;

		if (!is_usable())
		{
			return tracks;
		}

		const int track_count = api_->CountTracks(project_);

		// Not `std::max`. On macOS and Linux reaper_plugin.h reaches for WDL's swell,
		// which defines `max` as a function-like macro, and the macro eats the
		// qualified call before the compiler sees it.
		tracks.reserve(static_cast<std::size_t>(track_count > 0 ? track_count : 0));

		for (int index = 0; index < track_count; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr)
			{
				continue;
			}

			structural_track read_track;
			read_track.guid = read_track_guid(*api_, track);
			read_track.name = read_track_name(*api_, track);
			read_track.folder_depth_delta =
				static_cast<int>(api_->GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"));
			read_track.item_count = api_->CountTrackMediaItems(track);
			read_track.fx_count = api_->TrackFX_GetCount(track);
			read_track.send_count = api_->GetTrackNumSends(track, send_category);

			tracks.push_back(std::move(read_track));
		}

		return tracks;
	}

	std::string ReaperTrackStructureHost::insert_track(
		int index,
		const std::string& name,
		std::optional<int> color)
	{
		if (!is_usable())
		{
			return {};
		}

		// `InsertTrackAtIndex` takes no project parameter, so it acts on the active
		// project. That is the project this host is constructed for — a null
		// `ReaProject*` is REAPER's own convention for the active one — and a host built
		// for some other project would insert into the wrong place. Recorded rather than
		// guarded because the guard would have to compare against a project pointer
		// REAPER does not hand out.
		api_->InsertTrackAtIndex(index, true);
		api_->TrackList_AdjustWindows(adjust_windows_fully);

		MediaTrack* const inserted = api_->GetTrack(project_, index);

		if (inserted == nullptr)
		{
			return {};
		}

		if (!name.empty())
		{
			// `GetSetMediaTrackInfo_String` writes through a non-const buffer even when
			// setting, so the name is copied rather than pointed at.
			std::string writable_name = name;
			api_->GetSetMediaTrackInfo_String(inserted, "P_NAME", writable_name.data(), true);
		}

		if (color.has_value())
		{
			api_->SetMediaTrackInfo_Value(
				inserted,
				"I_CUSTOMCOLOR",
				static_cast<double>(*color | custom_color_enabled_flag));
		}

		return read_track_guid(*api_, inserted);
	}

	bool ReaperTrackStructureHost::delete_track(const std::string& guid)
	{
		if (!is_usable())
		{
			return false;
		}

		const int track_count = api_->CountTracks(project_);

		for (int index = 0; index < track_count; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr || read_track_guid(*api_, track) != guid)
			{
				continue;
			}

			api_->DeleteTrack(track);
			api_->TrackList_AdjustWindows(adjust_windows_fully);

			return true;
		}

		return false;
	}

	std::vector<std::string> ReaperTrackStructureHost::duplicate_tracks(
		const std::vector<std::string>& guids)
	{
		if (!is_usable() || guids.empty())
		{
			return {};
		}

		// The producer's own selection is project state they can see, and the render
		// tool reads it. Restored after the duplication, because this tool changes
		// structure and was not asked to change what is selected.
		std::vector<MediaTrack*> selection_before;
		const int selected_count = api_->CountSelectedTracks(project_);

		for (int index = 0; index < selected_count; ++index)
		{
			MediaTrack* const selected = api_->GetSelectedTrack(project_, index);

			if (selected != nullptr)
			{
				selection_before.push_back(selected);
			}
		}

		std::vector<std::string> guids_before;
		std::vector<MediaTrack*> tracks_to_duplicate;
		const int track_count = api_->CountTracks(project_);

		for (int index = 0; index < track_count; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr)
			{
				continue;
			}

			std::string guid = read_track_guid(*api_, track);

			if (std::find(guids.begin(), guids.end(), guid) != guids.end())
			{
				tracks_to_duplicate.push_back(track);
			}

			guids_before.push_back(std::move(guid));
		}

		if (tracks_to_duplicate.size() != guids.size())
		{
			return {};
		}

		api_->SetOnlyTrackSelected(tracks_to_duplicate.front());

		for (std::size_t index = 1; index < tracks_to_duplicate.size(); ++index)
		{
			api_->SetTrackSelected(tracks_to_duplicate[index], true);
		}

		api_->Main_OnCommand(duplicate_tracks_command, 0);
		api_->TrackList_AdjustWindows(adjust_windows_fully);

		// Which tracks are new, rather than which are selected. REAPER leaves the copies
		// selected, but reading the selection would make this depend on that staying
		// true across versions, while "a GUID that was not in the project a moment ago"
		// is a fact about the project.
		std::vector<std::string> copy_guids;
		const int track_count_after = api_->CountTracks(project_);

		for (int index = 0; index < track_count_after; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr)
			{
				continue;
			}

			std::string guid = read_track_guid(*api_, track);

			if (std::find(guids_before.begin(), guids_before.end(), guid) != guids_before.end())
			{
				continue;
			}

			copy_guids.push_back(std::move(guid));
		}

		if (!selection_before.empty())
		{
			api_->SetOnlyTrackSelected(selection_before.front());

			for (std::size_t index = 1; index < selection_before.size(); ++index)
			{
				api_->SetTrackSelected(selection_before[index], true);
			}
		}
		else
		{
			for (MediaTrack* const track : tracks_to_duplicate)
			{
				api_->SetTrackSelected(track, false);
			}

			for (const std::string& guid : copy_guids)
			{
				const int count = api_->CountTracks(project_);

				for (int index = 0; index < count; ++index)
				{
					MediaTrack* const track = api_->GetTrack(project_, index);

					if (track != nullptr && read_track_guid(*api_, track) == guid)
					{
						api_->SetTrackSelected(track, false);
						break;
					}
				}
			}
		}

		return copy_guids;
	}

	bool ReaperTrackStructureHost::reorder_tracks(
		const std::vector<std::string>& guids_in_project_order)
	{
		if (!is_usable())
		{
			return false;
		}

		std::vector<MediaTrack*> selection_before;
		const int selected_count = api_->CountSelectedTracks(project_);

		for (int index = 0; index < selected_count; ++index)
		{
			MediaTrack* const selected = api_->GetSelectedTrack(project_, index);

			if (selected != nullptr)
			{
				selection_before.push_back(selected);
			}
		}

		bool every_move_landed = true;

		// One pass over the target order, moving whichever track belongs at each
		// position into it. `ReorderSelectedTracks` moves the selection to just before a
		// given index, so a track always travels backwards to a position already
		// settled, and each position is settled once.
		for (std::size_t position = 0; position < guids_in_project_order.size(); ++position)
		{
			const int track_count = api_->CountTracks(project_);

			if (static_cast<std::size_t>(track_count > 0 ? track_count : 0) <= position)
			{
				every_move_landed = false;
				break;
			}

			MediaTrack* const track_in_place = api_->GetTrack(project_, static_cast<int>(position));

			if (track_in_place != nullptr
				&& read_track_guid(*api_, track_in_place) == guids_in_project_order[position])
			{
				continue;
			}

			MediaTrack* track_to_move = nullptr;

			for (int index = static_cast<int>(position) + 1; index < track_count; ++index)
			{
				MediaTrack* const candidate = api_->GetTrack(project_, index);

				if (candidate != nullptr
					&& read_track_guid(*api_, candidate) == guids_in_project_order[position])
				{
					track_to_move = candidate;
					break;
				}
			}

			if (track_to_move == nullptr)
			{
				every_move_landed = false;
				continue;
			}

			api_->SetOnlyTrackSelected(track_to_move);

			if (!api_->ReorderSelectedTracks(
					static_cast<int>(position),
					reorder_without_changing_folders))
			{
				every_move_landed = false;
			}
		}

		api_->TrackList_AdjustWindows(adjust_windows_fully);

		if (!selection_before.empty())
		{
			api_->SetOnlyTrackSelected(selection_before.front());

			for (std::size_t index = 1; index < selection_before.size(); ++index)
			{
				api_->SetTrackSelected(selection_before[index], true);
			}
		}

		return every_move_landed;
	}

	bool ReaperTrackStructureHost::write_folder_depth_delta(
		const std::string& guid,
		int folder_depth_delta)
	{
		if (!is_usable())
		{
			return false;
		}

		const int track_count = api_->CountTracks(project_);

		for (int index = 0; index < track_count; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr || read_track_guid(*api_, track) != guid)
			{
				continue;
			}

			return api_->SetMediaTrackInfo_Value(
				track,
				"I_FOLDERDEPTH",
				static_cast<double>(folder_depth_delta));
		}

		return false;
	}
}
