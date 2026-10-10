// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: an `item_host` call in, one or two REAPER C API calls
// out. No decisions — which items are in scope, where an offset lands, whether a split
// position falls inside an item, which of the three import failures a path has, and
// what every result reports are all decided in item_tools.h, which is what makes them
// testable without REAPER.
//
// The function pointers are resolved from the registration table's `GetFunc` rather than
// through REAPERAPI_IMPLEMENT. See reaper_item_host.h for why.

#include <daw/tools/reaper_item_host.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <sys/stat.h>

namespace
{
	// `guidToString` documents a 64-byte destination.
	constexpr std::size_t guid_buffer_size = 64;

	// Track and take names. 512 is the `maxLength` every output schema puts on one, and
	// REAPER truncates to the buffer rather than overrunning it.
	constexpr std::size_t name_buffer_size = 1024;

	// Media paths. 4096 is the `maxLength` both import schemas put on `filePath`.
	constexpr std::size_t path_buffer_size = 4104;

	// `InsertMedia`'s mode: 0 adds the media to the current track at the edit cursor.
	// That is why the import below sets the cursor and narrows the track selection first,
	// and restores both afterwards — the call takes no track and no position of its own.
	constexpr int insert_media_on_current_track = 0;

	// `SetMediaItemPosition` and `SetMediaItemLength`'s `refreshUI`. False on each write
	// and one `UpdateArrange` after the batch instead: refreshing per item makes a
	// 500-item move redraw the arrange view 500 times while the producer watches.
	constexpr bool defer_ui_refresh = false;

	// `GetSet_LoopTimeRange`'s `isLoop`: false reads the time selection rather than the
	// loop points. They are separate ranges in REAPER and `list_selected_items`' scope is
	// the time selection.
	constexpr bool read_time_selection_not_loop = false;

	// `SetEditCurPos`'s `moveview` and `seekplay`. Both false: moving the producer's view
	// or starting playback because a file was imported would be a side effect they did
	// not ask for.
	constexpr bool leave_view_alone = false;
	constexpr bool do_not_seek_playback = false;

	// The most MIDI notes walked when working out how many parts a file held. A part count
	// is answered by the channels the notes sit on, and a file long enough to exceed this
	// has already shown every channel it uses — walking the rest would cost a producer's
	// main thread for an answer that cannot change.
	constexpr int maximum_notes_walked_for_part_count = 4096;

	// The 16 MIDI channels.
	constexpr int midi_channel_count = 16;
}

namespace sesh_ai::daw::tools
{
	// The functions this component needs, and only those. Resolved once at construction;
	// every one of them is required, because an item path missing any of them reports a
	// project with no items in it, which reads as a producer with nothing selected.
	struct ReaperItemHost::reaper_item_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		int (*CountSelectedTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetSelectedTrack)(ReaProject*, int) = nullptr;
		void (*SetOnlyTrackSelected)(MediaTrack*) = nullptr;
		void (*SetTrackSelected)(MediaTrack*, bool) = nullptr;

		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;
		double (*GetMediaTrackInfo_Value)(MediaTrack*, const char*) = nullptr;

		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		MediaItem* (*GetTrackMediaItem)(MediaTrack*, int) = nullptr;
		MediaTrack* (*GetMediaItem_Track)(MediaItem*) = nullptr;

		bool (*GetSetMediaItemInfo_String)(MediaItem*, const char*, char*, bool) = nullptr;
		double (*GetMediaItemInfo_Value)(MediaItem*, const char*) = nullptr;
		bool (*SetMediaItemInfo_Value)(MediaItem*, const char*, double) = nullptr;
		bool (*SetMediaItemPosition)(MediaItem*, double, bool) = nullptr;
		bool (*SetMediaItemLength)(MediaItem*, double, bool) = nullptr;

		bool (*MoveMediaItemToTrack)(MediaItem*, MediaTrack*) = nullptr;
		MediaItem* (*SplitMediaItem)(MediaItem*, double) = nullptr;
		bool (*DeleteTrackMediaItem)(MediaTrack*, MediaItem*) = nullptr;

		MediaItem_Take* (*GetActiveTake)(MediaItem*) = nullptr;
		const char* (*GetTakeName)(MediaItem_Take*) = nullptr;
		bool (*TakeIsMIDI)(MediaItem_Take*) = nullptr;
		PCM_source* (*GetMediaItemTake_Source)(MediaItem_Take*) = nullptr;
		void (*GetMediaSourceFileName)(PCM_source*, char*, int) = nullptr;
		int (*GetMediaSourceSampleRate)(PCM_source*) = nullptr;
		int (*GetMediaSourceNumChannels)(PCM_source*) = nullptr;

		int (*MIDI_CountEvts)(MediaItem_Take*, int*, int*, int*) = nullptr;
		bool (*MIDI_GetNote)(
			MediaItem_Take*,
			int,
			bool*,
			bool*,
			double*,
			double*,
			int*,
			int*,
			int*) = nullptr;

		void (*GetSet_LoopTimeRange)(bool, bool, double*, double*, bool) = nullptr;
		double (*GetCursorPosition)() = nullptr;
		void (*SetEditCurPos)(double, bool, bool) = nullptr;
		int (*InsertMedia)(const char*, int) = nullptr;
		void (*UpdateArrange)() = nullptr;
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
			const ReaperItemHost::reaper_item_api& api,
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
			const ReaperItemHost::reaper_item_api& api,
			MediaTrack* track)
		{
			char name_buffer[name_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "P_NAME", name_buffer, false))
			{
				return {};
			}

			return std::string{name_buffer};
		}

		std::string read_item_guid(
			const ReaperItemHost::reaper_item_api& api,
			MediaItem* item)
		{
			char guid_buffer[guid_buffer_size] = {};

			if (!api.GetSetMediaItemInfo_String(item, "GUID", guid_buffer, false))
			{
				return {};
			}

			return std::string{guid_buffer};
		}

		// The active take's name, which is what REAPER shows on the item. Empty when the
		// item has no take or the take has no name — both normal, and neither invented
		// into a placeholder on this side of the seam.
		std::string read_active_take_name(
			const ReaperItemHost::reaper_item_api& api,
			MediaItem* item)
		{
			MediaItem_Take* const take = api.GetActiveTake(item);

			if (take == nullptr)
			{
				return {};
			}

			const char* const name = api.GetTakeName(take);

			if (name == nullptr)
			{
				return {};
			}

			return std::string{name};
		}

		// GUID to `MediaItem*`, across every track. The seam addresses an item by GUID
		// alone, and an item's track is something the decision layer reads rather than
		// supplies, so this lookup deliberately does not filter by track.
		MediaItem* find_item(
			const ReaperItemHost::reaper_item_api& api,
			ReaProject* project,
			const std::string& item_guid)
		{
			if (item_guid.empty())
			{
				return nullptr;
			}

			const int track_count = api.CountTracks(project);

			for (int track_index = 0; track_index < track_count; ++track_index)
			{
				MediaTrack* const track = api.GetTrack(project, track_index);

				if (track == nullptr)
				{
					continue;
				}

				const int item_count = api.CountTrackMediaItems(track);

				for (int item_index = 0; item_index < item_count; ++item_index)
				{
					MediaItem* const item = api.GetTrackMediaItem(track, item_index);

					if (item == nullptr)
					{
						continue;
					}

					if (read_item_guid(api, item) == item_guid)
					{
						return item;
					}
				}
			}

			return nullptr;
		}

		// GUID to `MediaTrack*`. The master track is not searched: it holds no media
		// items, so no item tool can address it.
		MediaTrack* find_track(
			const ReaperItemHost::reaper_item_api& api,
			ReaProject* project,
			const std::string& track_guid)
		{
			if (track_guid.empty())
			{
				return nullptr;
			}

			const int track_count = api.CountTracks(project);

			for (int track_index = 0; track_index < track_count; ++track_index)
			{
				MediaTrack* const track = api.GetTrack(project, track_index);

				if (track != nullptr && read_track_guid(api, track) == track_guid)
				{
					return track;
				}
			}

			return nullptr;
		}

		// How many parts an imported MIDI item holds, and how many notes.
		//
		// REAPER exposes no count of the tracks a standard MIDI file contained: by the
		// time the import has happened the file's own structure is gone and what is left
		// is one take's worth of events. The channels those events sit on are what stands
		// in for it, and they answer the question `importedTrackCount` exists to answer —
		// whether several parts arrived together on one item — even though they are not
		// literally the file's track count. A take with no notes is one part rather than
		// none, since a file that parsed and carried nothing still arrived as one item.
		//
		// Zero parts is returned for a take that is not MIDI at all, which is what a
		// producer pointing import_midi_file at an audio file produces. The decision layer
		// reports that as a result it cannot describe rather than claiming one part
		// arrived.
		struct midi_take_summary
		{
			int part_count = 0;
			int note_count = 0;
		};

		midi_take_summary read_midi_take_summary(
			const ReaperItemHost::reaper_item_api& api,
			MediaItem* item)
		{
			midi_take_summary summary;

			MediaItem_Take* const take = api.GetActiveTake(item);

			if (take == nullptr || !api.TakeIsMIDI(take))
			{
				return summary;
			}

			int note_count = 0;
			int cc_count = 0;
			int text_count = 0;

			api.MIDI_CountEvts(take, &note_count, &cc_count, &text_count);

			summary.note_count = note_count < 0 ? 0 : note_count;
			summary.part_count = 1;

			if (summary.note_count == 0)
			{
				return summary;
			}

			bool channel_seen[midi_channel_count] = {};

			const int notes_to_walk = summary.note_count < maximum_notes_walked_for_part_count
				? summary.note_count
				: maximum_notes_walked_for_part_count;

			for (int note_index = 0; note_index < notes_to_walk; ++note_index)
			{
				bool selected = false;
				bool muted = false;
				double start_ppq = 0.0;
				double end_ppq = 0.0;
				int channel = 0;
				int pitch = 0;
				int velocity = 0;

				if (!api.MIDI_GetNote(
						take,
						note_index,
						&selected,
						&muted,
						&start_ppq,
						&end_ppq,
						&channel,
						&pitch,
						&velocity))
				{
					continue;
				}

				if (channel >= 0 && channel < midi_channel_count)
				{
					channel_seen[channel] = true;
				}
			}

			int channels_used = 0;

			for (const bool seen : channel_seen)
			{
				if (seen)
				{
					++channels_used;
				}
			}

			if (channels_used > 0)
			{
				summary.part_count = channels_used;
			}

			return summary;
		}
	}

	ReaperItemHost::ReaperItemHost(reaper_plugin_info_t* plugin_info, ReaProject* project)
		: api_{std::make_unique<reaper_item_api>()}
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
		resolve_function(plugin_info, "SetOnlyTrackSelected", api_->SetOnlyTrackSelected, unresolved_function_names_);
		resolve_function(plugin_info, "SetTrackSelected", api_->SetTrackSelected, unresolved_function_names_);

		resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaTrackInfo_Value", api_->GetMediaTrackInfo_Value, unresolved_function_names_);

		resolve_function(plugin_info, "CountTrackMediaItems", api_->CountTrackMediaItems, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrackMediaItem", api_->GetTrackMediaItem, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaItem_Track", api_->GetMediaItem_Track, unresolved_function_names_);

		resolve_function(plugin_info, "GetSetMediaItemInfo_String", api_->GetSetMediaItemInfo_String, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaItemInfo_Value", api_->GetMediaItemInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaItemInfo_Value", api_->SetMediaItemInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaItemPosition", api_->SetMediaItemPosition, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaItemLength", api_->SetMediaItemLength, unresolved_function_names_);

		resolve_function(plugin_info, "MoveMediaItemToTrack", api_->MoveMediaItemToTrack, unresolved_function_names_);
		resolve_function(plugin_info, "SplitMediaItem", api_->SplitMediaItem, unresolved_function_names_);
		resolve_function(plugin_info, "DeleteTrackMediaItem", api_->DeleteTrackMediaItem, unresolved_function_names_);

		resolve_function(plugin_info, "GetActiveTake", api_->GetActiveTake, unresolved_function_names_);
		resolve_function(plugin_info, "GetTakeName", api_->GetTakeName, unresolved_function_names_);
		resolve_function(plugin_info, "TakeIsMIDI", api_->TakeIsMIDI, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaItemTake_Source", api_->GetMediaItemTake_Source, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaSourceFileName", api_->GetMediaSourceFileName, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaSourceSampleRate", api_->GetMediaSourceSampleRate, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaSourceNumChannels", api_->GetMediaSourceNumChannels, unresolved_function_names_);

		resolve_function(plugin_info, "MIDI_CountEvts", api_->MIDI_CountEvts, unresolved_function_names_);
		resolve_function(plugin_info, "MIDI_GetNote", api_->MIDI_GetNote, unresolved_function_names_);

		resolve_function(plugin_info, "GetSet_LoopTimeRange", api_->GetSet_LoopTimeRange, unresolved_function_names_);
		resolve_function(plugin_info, "GetCursorPosition", api_->GetCursorPosition, unresolved_function_names_);
		resolve_function(plugin_info, "SetEditCurPos", api_->SetEditCurPos, unresolved_function_names_);
		resolve_function(plugin_info, "InsertMedia", api_->InsertMedia, unresolved_function_names_);
		resolve_function(plugin_info, "UpdateArrange", api_->UpdateArrange, unresolved_function_names_);
	}

	ReaperItemHost::~ReaperItemHost() = default;

	bool ReaperItemHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	std::vector<std::string> ReaperItemHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	std::vector<item_state> ReaperItemHost::read_project_items()
	{
		std::vector<item_state> items;

		if (!is_usable())
		{
			return items;
		}

		const int track_count = api_->CountTracks(project_);

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track == nullptr)
			{
				continue;
			}

			const std::string track_guid = read_track_guid(*api_, track);
			const std::string track_name = read_track_name(*api_, track);
			const bool track_selected = api_->GetMediaTrackInfo_Value(track, "I_SELECTED") > 0.5;

			// REAPER enumerates a track's items in timeline order, which is the order the
			// decision layer relies on for a per-track walk.
			const int item_count = api_->CountTrackMediaItems(track);

			for (int item_index = 0; item_index < item_count; ++item_index)
			{
				MediaItem* const item = api_->GetTrackMediaItem(track, item_index);

				if (item == nullptr)
				{
					continue;
				}

				item_state read_item;
				read_item.guid = read_item_guid(*api_, item);

				if (read_item.guid.empty())
				{
					// An item REAPER will not name cannot be addressed by any item tool,
					// and reporting it would put a GUID of nothing in front of the agent.
					continue;
				}

				read_item.track_guid = track_guid;
				read_item.track_name = track_name;
				read_item.track_project_index = track_index;
				read_item.position_seconds = api_->GetMediaItemInfo_Value(item, "D_POSITION");
				read_item.length_seconds = api_->GetMediaItemInfo_Value(item, "D_LENGTH");
				read_item.take_name = read_active_take_name(*api_, item);
				read_item.muted = api_->GetMediaItemInfo_Value(item, "B_MUTE") > 0.5;

				// `B_UISEL` is the item selection the producer sees, which is what
				// `list_selected_items`' first scope means.
				read_item.selected = api_->GetMediaItemInfo_Value(item, "B_UISEL") > 0.5;
				read_item.track_selected = track_selected;

				items.push_back(std::move(read_item));
			}
		}

		return items;
	}

	std::optional<item_time_range> ReaperItemHost::read_time_selection()
	{
		if (!is_usable())
		{
			return std::nullopt;
		}

		double start_seconds = 0.0;
		double end_seconds = 0.0;

		api_->GetSet_LoopTimeRange(
			false,
			read_time_selection_not_loop,
			&start_seconds,
			&end_seconds,
			false);

		// REAPER reports an equal pair when there is no time selection. Reported as
		// nothing rather than as an empty range, so the decision layer is not asked to
		// tell the two apart.
		if (!(end_seconds > start_seconds))
		{
			return std::nullopt;
		}

		item_time_range range;
		range.start_seconds = start_seconds;
		range.end_seconds = end_seconds;

		return range;
	}

	item_file_access ReaperItemHost::probe_file(const std::string& file_path)
	{
		if (file_path.empty())
		{
			return item_file_access::missing;
		}

		struct stat file_status = {};

		if (::stat(file_path.c_str(), &file_status) != 0)
		{
			return item_file_access::missing;
		}

		// A directory exists and cannot be read as a file. Reported as unreadable rather
		// than missing, because "there is nothing there" would send the producer looking
		// for a path that is in front of them.
		if ((file_status.st_mode & S_IFMT) == S_IFDIR)
		{
			return item_file_access::unreadable;
		}

#ifdef _WIN32
		// `_access`'s mode 4 is read permission.
		if (::_access(file_path.c_str(), 4) != 0)
#else
		if (::access(file_path.c_str(), R_OK) != 0)
#endif
		{
			return item_file_access::unreadable;
		}

		// Readable. Whether REAPER can decode it is REAPER's answer and comes back from
		// `import_media_file`; nothing here opens the file to guess.
		return item_file_access::readable;
	}

	bool ReaperItemHost::set_item_position(const std::string& item_guid, double position_seconds)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		const bool written = api_->SetMediaItemPosition(item, position_seconds, defer_ui_refresh);

		if (written)
		{
			api_->UpdateArrange();
		}

		return written;
	}

	bool ReaperItemHost::set_item_length(const std::string& item_guid, double length_seconds)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		// `SetMediaItemLength` rather than writing `D_LENGTH` directly: it is the call
		// that trims against the take rather than only recording a number, which is why
		// the resulting length is read back on the other side of the seam.
		const bool written = api_->SetMediaItemLength(item, length_seconds, defer_ui_refresh);

		if (written)
		{
			api_->UpdateArrange();
		}

		return written;
	}

	bool ReaperItemHost::set_item_fade_in(const std::string& item_guid, double fade_seconds)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		return api_->SetMediaItemInfo_Value(item, "D_FADEINLEN", fade_seconds);
	}

	bool ReaperItemHost::set_item_fade_out(const std::string& item_guid, double fade_seconds)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		return api_->SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fade_seconds);
	}

	bool ReaperItemHost::set_item_volume(const std::string& item_guid, double reaper_volume)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		// Linear gain, converted from decibels by `unit_conversion.h` before it crossed
		// the seam. Nothing here converts anything.
		return api_->SetMediaItemInfo_Value(item, "D_VOL", reaper_volume);
	}

	bool ReaperItemHost::set_item_muted(const std::string& item_guid, bool muted)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return false;
		}

		return api_->SetMediaItemInfo_Value(item, "B_MUTE", muted ? 1.0 : 0.0);
	}

	bool ReaperItemHost::move_item_to_track(
		const std::string& item_guid,
		const std::string& track_guid)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaItem* const item = find_item(*api_, project_, item_guid);
		MediaTrack* const destination = find_track(*api_, project_, track_guid);

		if (item == nullptr || destination == nullptr)
		{
			return false;
		}

		const bool moved = api_->MoveMediaItemToTrack(item, destination);

		if (moved)
		{
			api_->UpdateArrange();
		}

		return moved;
	}

	std::optional<item_split_halves> ReaperItemHost::split_item(
		const std::string& item_guid,
		double position_seconds)
	{
		MediaItem* const item = is_usable() ? find_item(*api_, project_, item_guid) : nullptr;

		if (item == nullptr)
		{
			return std::nullopt;
		}

		// REAPER keeps the original item as the left half — GUID included — and returns
		// the newly minted right half. Null means it declined to split.
		MediaItem* const right = api_->SplitMediaItem(item, position_seconds);

		if (right == nullptr)
		{
			return std::nullopt;
		}

		item_split_halves halves;
		halves.left_item_guid = read_item_guid(*api_, item);
		halves.right_item_guid = read_item_guid(*api_, right);

		api_->UpdateArrange();

		if (halves.left_item_guid.empty() || halves.right_item_guid.empty())
		{
			// The split happened and one of the halves cannot be named. Reported as no
			// split rather than as half of one: the decision layer's result requires both
			// GUIDs, and a pair with one side missing would send the agent to address an
			// item it cannot see.
			return std::nullopt;
		}

		return halves;
	}

	bool ReaperItemHost::delete_item(const std::string& item_guid)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaItem* const item = find_item(*api_, project_, item_guid);

		if (item == nullptr)
		{
			return false;
		}

		// The item's own track, read from the item rather than passed in, so a stale track
		// GUID cannot send the delete at the wrong track's item list.
		MediaTrack* const track = api_->GetMediaItem_Track(item);

		if (track == nullptr)
		{
			return false;
		}

		const bool deleted = api_->DeleteTrackMediaItem(track, item);

		if (deleted)
		{
			api_->UpdateArrange();
		}

		return deleted;
	}

	std::optional<imported_media> ReaperItemHost::import_media_file(
		const std::string& track_guid,
		const std::string& file_path,
		double position_seconds)
	{
		if (!is_usable())
		{
			return std::nullopt;
		}

		MediaTrack* const track = find_track(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return std::nullopt;
		}

		// `InsertMedia` takes neither a track nor a position: it puts the media on the
		// current track at the edit cursor. So the cursor and the track selection are the
		// arguments, and both are the producer's own state — they are snapshotted here and
		// put back below, because an import that silently moved the producer's cursor and
		// cleared their track selection would be a change they did not ask for and cannot
		// see in the result.
		const double cursor_before = api_->GetCursorPosition();

		const int track_count = api_->CountTracks(project_);
		std::vector<bool> selected_before;
		selected_before.reserve(static_cast<std::size_t>(track_count > 0 ? track_count : 0));

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const candidate = api_->GetTrack(project_, track_index);

			selected_before.push_back(
				candidate != nullptr && api_->GetMediaTrackInfo_Value(candidate, "I_SELECTED") > 0.5);
		}

		// The items already on the track, so the one REAPER creates can be told from them.
		// By GUID rather than by count: an import that added several items would otherwise
		// be indistinguishable from one that added one, and the GUIDs say which is new.
		std::vector<std::string> item_guids_before;

		{
			const int item_count = api_->CountTrackMediaItems(track);

			for (int item_index = 0; item_index < item_count; ++item_index)
			{
				MediaItem* const existing = api_->GetTrackMediaItem(track, item_index);

				if (existing != nullptr)
				{
					item_guids_before.push_back(read_item_guid(*api_, existing));
				}
			}
		}

		api_->SetOnlyTrackSelected(track);
		api_->SetEditCurPos(position_seconds, leave_view_alone, do_not_seek_playback);

		api_->InsertMedia(file_path.c_str(), insert_media_on_current_track);

		// The cursor and selection go back before anything is read, so an early return
		// below cannot leave the producer's session rearranged.
		api_->SetEditCurPos(cursor_before, leave_view_alone, do_not_seek_playback);

		for (std::size_t track_index = 0; track_index < selected_before.size(); ++track_index)
		{
			MediaTrack* const candidate = api_->GetTrack(project_, static_cast<int>(track_index));

			if (candidate != nullptr)
			{
				api_->SetTrackSelected(candidate, selected_before[track_index]);
			}
		}

		// The item that was not there before. `InsertMedia` returns a count rather than a
		// handle, so the new item is found rather than reported.
		MediaItem* imported_item = nullptr;
		std::string imported_guid;

		{
			const int item_count = api_->CountTrackMediaItems(track);

			for (int item_index = 0; item_index < item_count; ++item_index)
			{
				MediaItem* const candidate = api_->GetTrackMediaItem(track, item_index);

				if (candidate == nullptr)
				{
					continue;
				}

				std::string candidate_guid = read_item_guid(*api_, candidate);

				if (candidate_guid.empty())
				{
					continue;
				}

				bool was_there_before = false;

				for (const std::string& existing_guid : item_guids_before)
				{
					if (existing_guid == candidate_guid)
					{
						was_there_before = true;
						break;
					}
				}

				if (!was_there_before)
				{
					imported_item = candidate;
					imported_guid = std::move(candidate_guid);
					break;
				}
			}
		}

		if (imported_item == nullptr)
		{
			// Nothing new on the track. REAPER has no reader for the file, which is the
			// answer the decision layer turns into its third import failure rather than a
			// problem with the path.
			return std::nullopt;
		}

		api_->UpdateArrange();

		imported_media imported;
		imported.item_guid = std::move(imported_guid);

		MediaItem_Take* const take = api_->GetActiveTake(imported_item);

		if (take != nullptr)
		{
			PCM_source* const source = api_->GetMediaItemTake_Source(take);

			if (source != nullptr)
			{
				char path_buffer[path_buffer_size] = {};
				api_->GetMediaSourceFileName(source, path_buffer, static_cast<int>(path_buffer_size));
				imported.resolved_file_path = std::string{path_buffer};

				// Zero for a source REAPER does not describe in these terms — a MIDI take
				// has no sample rate — and the decision layer omits a zero rather than
				// reporting it.
				const int sample_rate = api_->GetMediaSourceSampleRate(source);
				const int channel_count = api_->GetMediaSourceNumChannels(source);

				imported.sample_rate = sample_rate > 0 ? sample_rate : 0;
				imported.channel_count = channel_count > 0 ? channel_count : 0;
			}
		}

		const midi_take_summary midi = read_midi_take_summary(*api_, imported_item);
		imported.imported_track_count = midi.part_count;
		imported.note_count = midi.note_count;

		return imported;
	}
}
