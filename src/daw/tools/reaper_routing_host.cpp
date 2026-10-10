// The REAPER side of the routing seam.
//
// Everything here is translation: a `routing_host` call in, one or a few REAPER C API
// calls out. No decisions — the cycle checks, the folder arithmetic, the bus
// classification, and every result shape live in routing_tools.h, which is what makes
// them testable without REAPER. If a change to this file needs a branch on anything
// other than "did REAPER give me a value", the logic belongs on the other side of the
// seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather
// than through REAPERAPI_IMPLEMENT. See reaper_routing_host.h for why.
//
// ---------------------------------------------------------------------------
// Two readings that are not the obvious ones
//
// **Receives are inverted from sends rather than enumerated.** REAPER will enumerate a
// track's receives directly (`GetTrackNumSends` with a negative category), but the index
// it reports is then the receive's index on the destination, and `remove_send` and
// `set_send_state` address a send by its index *on the source*. So the receives here are
// built by walking every track's sends once and mirroring each onto its destination.
// The far end and the source-side index both come out right, and the alternative is a
// second enumeration whose indices mean something different.
//
// **Folder summing is read, not derived.** `I_FOLDERDEPTH` and `B_MAINSEND` are the only
// evidence that a folder parent sums anything — the summing itself appears in no send
// enumeration — so both are on every track in the snapshot. A reading that carried sends
// alone would describe a fully routed session as unrouted.

#include <daw/tools/reaper_routing_host.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <utility>

namespace
{
	// Buffer sizes. Track names are bounded at 512 characters by the output schemas;
	// `guidToString` documents a 64-byte destination. Both are rounded up to something
	// REAPER will never overrun.
	constexpr std::size_t guid_buffer_size = 64;
	constexpr std::size_t name_buffer_size = 1024;

	// `GetTrackNumSends` / `GetTrackSendInfo_Value` categories: 0 is sends, negative is
	// receives, positive is hardware outputs. Only sends are read here — see the file
	// header on why receives are inverted rather than enumerated.
	constexpr int send_category = 0;

	// `I_SENDMODE`: 0 = post-fader, 1 = pre-FX, 2 = post-FX (deprecated), 3 = post-FX.
	// Mode 2 is read as post-FX and never written, which is what "deprecated" means here:
	// a project that holds one reports it faithfully, and writing it back would preserve
	// a value REAPER itself has moved on from.
	constexpr int reaper_send_mode_post_fader = 0;
	constexpr int reaper_send_mode_pre_fx = 1;
	constexpr int reaper_send_mode_deprecated_post_fx = 2;
	constexpr int reaper_send_mode_post_fx = 3;

	// `ReorderSelectedTracks`'s second argument. Zero leaves folder state alone, which is
	// the only correct value here: the folder depths that follow a rearrangement are the
	// Folder Invariant Keeper's answer, written explicitly, and letting REAPER guess at a
	// folder boundary mid-move would fight it.
	constexpr int reorder_without_changing_folder_state = 0;

	// `InsertTrackAtIndex`'s `wantDefaults`. True, so the new track arrives with the
	// producer's own defaults — including `B_MAINSEND` on, which is REAPER's default and
	// what the bus construction plans assume.
	constexpr bool insert_track_with_defaults = true;
}

namespace sesh_ai::daw::tools
{
	// The functions this component needs, and only those. Resolved once at construction;
	// every one of them is required, because a routing path missing any of them either
	// reads a session that is not there or writes something other than what was asked
	// for.
	struct ReaperRoutingHost::reaper_routing_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		MediaTrack* (*GetMasterTrack)(ReaProject*) = nullptr;

		double (*GetMediaTrackInfo_Value)(MediaTrack*, const char*) = nullptr;
		bool (*SetMediaTrackInfo_Value)(MediaTrack*, const char*, double) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;

		int (*GetTrackNumSends)(MediaTrack*, int) = nullptr;
		double (*GetTrackSendInfo_Value)(MediaTrack*, int, int, const char*) = nullptr;
		bool (*SetTrackSendInfo_Value)(MediaTrack*, int, int, const char*, double) = nullptr;
		void* (*GetSetTrackSendInfo)(MediaTrack*, int, int, const char*, void*) = nullptr;
		int (*CreateTrackSend)(MediaTrack*, MediaTrack*) = nullptr;
		bool (*RemoveTrackSend)(MediaTrack*, int, int) = nullptr;

		void (*InsertTrackAtIndex)(int, bool) = nullptr;
		void (*DeleteTrack)(MediaTrack*) = nullptr;
		void (*SetOnlyTrackSelected)(MediaTrack*) = nullptr;
		bool (*ReorderSelectedTracks)(int, int) = nullptr;
		void (*TrackList_AdjustWindows)(bool) = nullptr;

		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		int (*TrackFX_AddByName)(MediaTrack*, const char*, bool, int) = nullptr;
		int (*TrackFX_GetCount)(MediaTrack*) = nullptr;
		bool (*TrackFX_GetFXName)(MediaTrack*, int, char*, int) = nullptr;
		bool (*TrackFX_GetEnabled)(MediaTrack*, int) = nullptr;
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
			const ReaperRoutingHost::reaper_routing_api& api,
			MediaTrack* track)
		{
			char guid_buffer[guid_buffer_size] = {};

			if (track == nullptr || !api.GetSetMediaTrackInfo_String(track, "GUID", guid_buffer, false))
			{
				return {};
			}

			return std::string{guid_buffer};
		}

		std::string read_track_name(
			const ReaperRoutingHost::reaper_routing_api& api,
			MediaTrack* track)
		{
			char name_buffer[name_buffer_size] = {};

			if (track == nullptr || !api.GetSetMediaTrackInfo_String(track, "P_NAME", name_buffer, false))
			{
				return {};
			}

			return std::string{name_buffer};
		}

		routing_send_mode send_mode_from_reaper(int reaper_send_mode)
		{
			switch (reaper_send_mode)
			{
				case reaper_send_mode_pre_fx:
					return routing_send_mode::pre_fx;
				case reaper_send_mode_deprecated_post_fx:
				case reaper_send_mode_post_fx:
					return routing_send_mode::post_fx;
				default:
					return routing_send_mode::post_fader;
			}
		}

		int reaper_send_mode_from(routing_send_mode mode)
		{
			switch (mode)
			{
				case routing_send_mode::pre_fx:
					return reaper_send_mode_pre_fx;
				case routing_send_mode::post_fx:
					return reaper_send_mode_post_fx;
				case routing_send_mode::post_fader:
					return reaper_send_mode_post_fader;
			}

			return reaper_send_mode_post_fader;
		}

		// One send, read from the source side. `index` is the send's index on the source
		// track, which is how the tools address it.
		routing_send read_send_at(
			const ReaperRoutingHost::reaper_routing_api& api,
			MediaTrack* source,
			int send_index)
		{
			routing_send send;
			send.index = send_index;

			MediaTrack* const destination = static_cast<MediaTrack*>(
				api.GetSetTrackSendInfo(source, send_category, send_index, "P_DESTTRACK", nullptr));

			send.track_guid = read_track_guid(api, destination);
			send.track_name = read_track_name(api, destination);

			send.volume_decibels = volume_decibels_from_reaper_volume(
				api.GetTrackSendInfo_Value(source, send_category, send_index, "D_VOL"));
			send.pan_percent = pan_percent_from_reaper_pan(
				api.GetTrackSendInfo_Value(source, send_category, send_index, "D_PAN"));
			send.muted =
				api.GetTrackSendInfo_Value(source, send_category, send_index, "B_MUTE") != 0.0;
			send.mode = send_mode_from_reaper(static_cast<int>(
				api.GetTrackSendInfo_Value(source, send_category, send_index, "I_SENDMODE")));

			return send;
		}
	}

	ReaperRoutingHost::ReaperRoutingHost(reaper_plugin_info_t* plugin_info, ReaProject* project)
		: api_{std::make_unique<reaper_routing_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);
		resolve_function(plugin_info, "GetMasterTrack", api_->GetMasterTrack, unresolved_function_names_);

		resolve_function(plugin_info, "GetMediaTrackInfo_Value", api_->GetMediaTrackInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetMediaTrackInfo_Value", api_->SetMediaTrackInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String, unresolved_function_names_);

		resolve_function(plugin_info, "GetTrackNumSends", api_->GetTrackNumSends, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrackSendInfo_Value", api_->GetTrackSendInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetTrackSendInfo_Value", api_->SetTrackSendInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetTrackSendInfo", api_->GetSetTrackSendInfo, unresolved_function_names_);
		resolve_function(plugin_info, "CreateTrackSend", api_->CreateTrackSend, unresolved_function_names_);
		resolve_function(plugin_info, "RemoveTrackSend", api_->RemoveTrackSend, unresolved_function_names_);

		resolve_function(plugin_info, "InsertTrackAtIndex", api_->InsertTrackAtIndex, unresolved_function_names_);
		resolve_function(plugin_info, "DeleteTrack", api_->DeleteTrack, unresolved_function_names_);
		resolve_function(plugin_info, "SetOnlyTrackSelected", api_->SetOnlyTrackSelected, unresolved_function_names_);
		resolve_function(plugin_info, "ReorderSelectedTracks", api_->ReorderSelectedTracks, unresolved_function_names_);
		resolve_function(plugin_info, "TrackList_AdjustWindows", api_->TrackList_AdjustWindows, unresolved_function_names_);

		resolve_function(plugin_info, "CountTrackMediaItems", api_->CountTrackMediaItems, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_AddByName", api_->TrackFX_AddByName, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetCount", api_->TrackFX_GetCount, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetFXName", api_->TrackFX_GetFXName, unresolved_function_names_);
		resolve_function(plugin_info, "TrackFX_GetEnabled", api_->TrackFX_GetEnabled, unresolved_function_names_);
	}

	ReaperRoutingHost::~ReaperRoutingHost() = default;

	bool ReaperRoutingHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	const std::vector<std::string>& ReaperRoutingHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	// The whole project's routing, in one pass.
	//
	// `readable` stays false on an unusable host, which matters: the tools treat an
	// unreadable snapshot as a failed action rather than as a session with no routing, so
	// a cycle check is never run against an empty graph it would permit anything against.
	routing_snapshot ReaperRoutingHost::read_routing_snapshot()
	{
		routing_snapshot snapshot;

		if (!is_usable())
		{
			return snapshot;
		}

		const int track_count = api_->CountTracks(project_);

		snapshot.tracks_in_project_order.reserve(track_count < 0 ? 0 : static_cast<std::size_t>(track_count));

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track == nullptr)
			{
				continue;
			}

			routing_track reading;
			reading.guid = read_track_guid(*api_, track);
			reading.name = read_track_name(*api_, track);

			// The two signals folder summing is only observable through.
			reading.folder_depth_delta =
				static_cast<int>(api_->GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"));
			reading.parent_send_enabled = api_->GetMediaTrackInfo_Value(track, "B_MAINSEND") != 0.0;

			reading.item_count = api_->CountTrackMediaItems(track);
			reading.fx_count = api_->TrackFX_GetCount(track);

			const int send_count = api_->GetTrackNumSends(track, send_category);

			for (int send_index = 0; send_index < send_count; ++send_index)
			{
				reading.sends.push_back(read_send_at(*api_, track, send_index));
			}

			snapshot.tracks_in_project_order.push_back(std::move(reading));
		}

		MediaTrack* const master = api_->GetMasterTrack(project_);

		if (master != nullptr)
		{
			snapshot.master_track.present = true;
			snapshot.master_track.guid = read_track_guid(*api_, master);
			snapshot.master_track.name = read_track_name(*api_, master);
			snapshot.master_track.fx_count = api_->TrackFX_GetCount(master);
		}

		// Receives, mirrored from the sends already read. See the file header: enumerating
		// them would report an index that means something else.
		for (std::size_t source_index = 0; source_index < snapshot.tracks_in_project_order.size();
			++source_index)
		{
			const routing_track& source = snapshot.tracks_in_project_order[source_index];

			for (const routing_send& send : source.sends)
			{
				routing_send incoming = send;
				incoming.track_guid = source.guid;
				incoming.track_name = source.name;

				if (snapshot.master_track.present && send.track_guid == snapshot.master_track.guid)
				{
					snapshot.master_track.receives.push_back(std::move(incoming));
					continue;
				}

				for (routing_track& destination : snapshot.tracks_in_project_order)
				{
					if (destination.guid == send.track_guid)
					{
						destination.receives.push_back(std::move(incoming));
						break;
					}
				}
			}
		}

		snapshot.readable = true;

		return snapshot;
	}

	std::optional<routing_send> ReaperRoutingHost::read_send(
		const std::string& source_track_guid,
		int send_index)
	{
		MediaTrack* const source = find_track_by_guid(source_track_guid);

		if (source == nullptr || send_index < 0
			|| send_index >= api_->GetTrackNumSends(source, send_category))
		{
			return std::nullopt;
		}

		return read_send_at(*api_, source, send_index);
	}

	std::optional<int> ReaperRoutingHost::create_send(
		const std::string& source_track_guid,
		const std::string& destination_track_guid)
	{
		MediaTrack* const source = find_track_by_guid(source_track_guid);
		MediaTrack* const destination = find_track_by_guid(destination_track_guid);

		// A null destination would create a hardware output rather than a send, which is
		// a different routing object and not what was asked for.
		if (source == nullptr || destination == nullptr)
		{
			return std::nullopt;
		}

		const int send_index = api_->CreateTrackSend(source, destination);

		if (send_index < 0)
		{
			return std::nullopt;
		}

		return send_index;
	}

	bool ReaperRoutingHost::remove_send(const std::string& source_track_guid, int send_index)
	{
		MediaTrack* const source = find_track_by_guid(source_track_guid);

		if (source == nullptr || send_index < 0)
		{
			return false;
		}

		return api_->RemoveTrackSend(source, send_category, send_index);
	}

	bool ReaperRoutingHost::write_send_state(
		const std::string& source_track_guid,
		int send_index,
		const send_state_change& change)
	{
		MediaTrack* const source = find_track_by_guid(source_track_guid);

		if (source == nullptr || send_index < 0)
		{
			return false;
		}

		bool every_write_landed = true;

		if (change.volume_decibels.has_value())
		{
			every_write_landed = api_->SetTrackSendInfo_Value(
				source,
				send_category,
				send_index,
				"D_VOL",
				reaper_volume_from_volume_decibels(*change.volume_decibels)) && every_write_landed;
		}

		if (change.pan_percent.has_value())
		{
			every_write_landed = api_->SetTrackSendInfo_Value(
				source,
				send_category,
				send_index,
				"D_PAN",
				reaper_pan_from_pan_percent(*change.pan_percent)) && every_write_landed;
		}

		if (change.muted.has_value())
		{
			every_write_landed = api_->SetTrackSendInfo_Value(
				source,
				send_category,
				send_index,
				"B_MUTE",
				*change.muted ? 1.0 : 0.0) && every_write_landed;
		}

		if (change.mode.has_value())
		{
			every_write_landed = api_->SetTrackSendInfo_Value(
				source,
				send_category,
				send_index,
				"I_SENDMODE",
				static_cast<double>(reaper_send_mode_from(*change.mode))) && every_write_landed;
		}

		return every_write_landed;
	}

	// `B_MAINSEND` is a flag on the child, not a send object. Nothing else moves.
	bool ReaperRoutingHost::write_parent_send(const std::string& track_guid, bool enabled)
	{
		MediaTrack* const track = find_track_by_guid(track_guid);

		if (track == nullptr)
		{
			return false;
		}

		const bool written = api_->SetMediaTrackInfo_Value(track, "B_MAINSEND", enabled ? 1.0 : 0.0);

		api_->TrackList_AdjustWindows(true);

		return written;
	}

	// `InsertTrackAtIndex` returns nothing, so the new track is found by reading the
	// index back rather than by trusting the call.
	std::optional<TrackReference> ReaperRoutingHost::insert_track(
		int project_index,
		const std::string& name)
	{
		if (!is_usable() || project_index < 0)
		{
			return std::nullopt;
		}

		api_->InsertTrackAtIndex(project_index, insert_track_with_defaults);
		api_->TrackList_AdjustWindows(false);

		MediaTrack* const inserted = api_->GetTrack(project_, project_index);

		if (inserted == nullptr)
		{
			return std::nullopt;
		}

		if (!name.empty())
		{
			std::string writable_name = name;

			// `GetSetMediaTrackInfo_String` writes through the buffer it is given, so the
			// name cannot be passed as a const pointer.
			api_->GetSetMediaTrackInfo_String(inserted, "P_NAME", writable_name.data(), true);
		}

		return TrackReference{read_track_guid(*api_, inserted), read_track_name(*api_, inserted)};
	}

	bool ReaperRoutingHost::delete_track(const std::string& track_guid)
	{
		MediaTrack* const track = find_track_by_guid(track_guid);

		if (track == nullptr)
		{
			return false;
		}

		api_->DeleteTrack(track);
		api_->TrackList_AdjustWindows(false);

		// REAPER's `DeleteTrack` returns nothing, so the check is that the track is gone.
		return find_track_by_guid(track_guid) == nullptr;
	}

	// Puts the tracks in the requested order, left to right.
	//
	// `ReorderSelectedTracks` moves the selection to before a given index, so the order is
	// applied as an in-place insertion sort: at step i everything before i is already
	// right, and the track that belongs at i is moved there from wherever it is. Folder
	// state is left alone on every move — the depths that follow are the Folder Invariant
	// Keeper's answer and are written separately.
	bool ReaperRoutingHost::apply_track_order(const std::vector<std::string>& track_guids_in_project_order)
	{
		if (!is_usable())
		{
			return false;
		}

		if (static_cast<int>(track_guids_in_project_order.size()) != api_->CountTracks(project_))
		{
			// A partially applied order is worse than an unapplied one: it leaves the
			// folder structure describing a session that no longer exists.
			return false;
		}

		for (std::size_t target_index = 0; target_index < track_guids_in_project_order.size();
			++target_index)
		{
			const int current_index = find_track_index_by_guid(track_guids_in_project_order[target_index]);

			if (current_index < 0)
			{
				return false;
			}

			if (current_index == static_cast<int>(target_index))
			{
				continue;
			}

			MediaTrack* const track = api_->GetTrack(project_, current_index);

			if (track == nullptr)
			{
				return false;
			}

			api_->SetOnlyTrackSelected(track);

			if (!api_->ReorderSelectedTracks(
					static_cast<int>(target_index),
					reorder_without_changing_folder_state))
			{
				return false;
			}
		}

		api_->TrackList_AdjustWindows(false);

		return true;
	}

	bool ReaperRoutingHost::write_folder_depth(const std::string& track_guid, int folder_depth_delta)
	{
		MediaTrack* const track = find_track_by_guid(track_guid);

		if (track == nullptr)
		{
			return false;
		}

		const bool written = api_->SetMediaTrackInfo_Value(
			track,
			"I_FOLDERDEPTH",
			static_cast<double>(folder_depth_delta));

		// Folder depth is one of the attributes REAPER's own documentation says needs the
		// arrange and track panels updated by hand after a write.
		api_->TrackList_AdjustWindows(false);

		return written;
	}

	std::optional<routing_fx_placement> ReaperRoutingHost::add_fx(
		const std::string& track_guid,
		const std::string& fx_name)
	{
		MediaTrack* const track = find_track_by_guid(track_guid);

		if (track == nullptr || fx_name.empty())
		{
			return std::nullopt;
		}

		// `instantiate` of -1 means "add a new instance even if one is already on the
		// chain", which is what a bus being built asks for: two instances of the same FX
		// in a chain is a legitimate thing to want and reusing one silently would give the
		// producer a chain they did not describe.
		const int fx_index = api_->TrackFX_AddByName(track, fx_name.c_str(), false, -1);

		if (fx_index < 0)
		{
			return std::nullopt;
		}

		routing_fx_placement placement;
		placement.index = fx_index;
		placement.name = fx_name;

		char name_buffer[name_buffer_size] = {};

		if (api_->TrackFX_GetFXName(track, fx_index, name_buffer, static_cast<int>(name_buffer_size)))
		{
			// REAPER's own name for what it loaded, which is not always the name it was
			// asked for.
			placement.name = std::string{name_buffer};
		}

		placement.bypassed = !api_->TrackFX_GetEnabled(track, fx_index);

		return placement;
	}

	MediaTrack* ReaperRoutingHost::find_track_by_guid(const std::string& track_guid)
	{
		const int track_index = find_track_index_by_guid(track_guid);

		return track_index < 0 ? nullptr : api_->GetTrack(project_, track_index);
	}

	int ReaperRoutingHost::find_track_index_by_guid(const std::string& track_guid)
	{
		if (!is_usable() || track_guid.empty())
		{
			return -1;
		}

		const int track_count = api_->CountTracks(project_);

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track != nullptr && read_track_guid(*api_, track) == track_guid)
			{
				return track_index;
			}
		}

		return -1;
	}
}
