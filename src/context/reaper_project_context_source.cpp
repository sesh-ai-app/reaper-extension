// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: a `ProjectContextSource` call in, REAPER C API
// calls out. No decisions — the bounds enforcement, the role derivation, the folder
// accumulation, the debounce, and the serialisation all live in
// project_context_builder.h, which is what makes them testable without REAPER. If a
// change to this file needs a branch on anything other than "did REAPER give me a
// value", the logic belongs on the other side of the seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather
// than through REAPERAPI_IMPLEMENT. See reaper_project_context_source.h for why.

#include <context/reaper_project_context_source.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

namespace
{
	// Track and project names. The schema caps both at 512 characters and the builder
	// truncates to that, so this is only about giving REAPER somewhere to write.
	constexpr std::size_t name_buffer_size = 1024;

	// `GetSetMediaTrackInfo_String(…, "GUID", …)` writes REAPER's braced form, which is
	// 38 characters. 64 matches what the SDK documents for `guidToString`.
	constexpr std::size_t guid_buffer_size = 64;

	// `GetAudioDeviceInfo` writes a short decimal string for "SRATE".
	constexpr std::size_t audio_device_info_buffer_size = 64;

	// `GetTrackNumSends` categories: -1 receives, 0 sends, 1 hardware outputs. Only
	// receives are read — what makes a track an aux return is that things feed it, and
	// its own sends say nothing about that.
	constexpr int receive_send_category = -1;

	// `GetSetRepeatEx` with -1 queries rather than sets.
	constexpr int query_repeat_state = -1;

	// The two ranges `GetSet_LoopTimeRange2` distinguishes. They are the same range in
	// a default REAPER install and diverge once the producer unlinks loop points from
	// the time selection, which is why both are read rather than one being copied into
	// the other.
	constexpr bool read_the_loop_range = true;
	constexpr bool read_the_time_selection = false;

	// REAPER's project extension for a saved project. Stripped so `projectName` is the
	// name without path or extension, which is what the schema asks for.
	constexpr const char* reaper_project_extension = ".rpp";

	// The name without directory or extension.
	//
	// `GetProjectName` hands back the file name for a saved project and an empty string
	// for one that has never been saved. Directory separators should not appear, and are
	// stripped anyway: the schema says "without path", and an empty string is better
	// than a name that turns out to be a path on one platform.
	std::string project_name_without_path_or_extension(const std::string& project_file_name)
	{
		const std::size_t last_separator = project_file_name.find_last_of("/\\");

		std::string name = last_separator == std::string::npos
			? project_file_name
			: project_file_name.substr(last_separator + 1);

		const std::size_t extension_length = std::string{reaper_project_extension}.size();

		if (name.size() <= extension_length)
		{
			return name;
		}

		std::string trailing = name.substr(name.size() - extension_length);

		for (char& character : trailing)
		{
			if (character >= 'A' && character <= 'Z')
			{
				character = static_cast<char>(character - 'A' + 'a');
			}
		}

		if (trailing == reaper_project_extension)
		{
			name.resize(name.size() - extension_length);
		}

		return name;
	}
}

namespace sesh_ai::context
{
	// The functions this component needs, and only those. Resolved once at
	// construction; every one is required, because a snapshot missing any of them
	// reports a project the producer is not looking at.
	struct ReaperProjectContextSource::reaper_project_context_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		MediaTrack* (*GetMasterTrack)(ReaProject*) = nullptr;
		double (*GetMediaTrackInfo_Value)(MediaTrack*, const char*) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;
		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		int (*TrackFX_GetCount)(MediaTrack*) = nullptr;
		int (*GetTrackNumSends)(MediaTrack*, int) = nullptr;
		bool (*IsTrackSelected)(MediaTrack*) = nullptr;

		void (*GetProjectName)(ReaProject*, char*, int) = nullptr;
		double (*GetProjectLength)(ReaProject*) = nullptr;
		double (*GetCursorPositionEx)(ReaProject*) = nullptr;
		int (*GetPlayStateEx)(ReaProject*) = nullptr;
		int (*GetProjectStateChangeCount)(ReaProject*) = nullptr;

		void (*GetSet_LoopTimeRange2)(ReaProject*, bool, bool, double*, double*, bool) = nullptr;
		int (*GetSetRepeatEx)(ReaProject*, int) = nullptr;

		void (*TimeMap_GetTimeSigAtTime)(ReaProject*, double, int*, int*, double*) = nullptr;
		double (*GetSetProjectInfo)(ReaProject*, const char*, double, bool) = nullptr;
		bool (*GetAudioDeviceInfo)(const char*, char*, int) = nullptr;

		int (*CountProjectMarkers)(ReaProject*, int*, int*) = nullptr;
		int (*CountTempoTimeSigMarkers)(ReaProject*) = nullptr;
	};

	namespace
	{
		// Resolves one function and records the name when it is missing, so an unusable
		// source can say which functions REAPER did not supply rather than only that it
		// is unusable.
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

		std::string read_track_string(
			const ReaperProjectContextSource::reaper_project_context_api& api,
			MediaTrack* track,
			const char* parameter_name,
			std::size_t buffer_size)
		{
			std::string buffer(buffer_size, '\0');

			if (!api.GetSetMediaTrackInfo_String(track, parameter_name, buffer.data(), false))
			{
				return {};
			}

			buffer.resize(std::string{buffer.c_str()}.size());

			return buffer;
		}

		// The project's sample rate, which is not simply `PROJECT_SRATE`.
		//
		// REAPER keeps two things: a project sample rate, and a flag saying whether the
		// project overrides the audio device. When the flag is off the project runs at
		// whatever the device is set to, and reporting the stored override instead would
		// tell the agent the session is 48 kHz while REAPER renders at 44.1 — which
		// changes what a render sounds like.
		//
		// So the flag decides, and the device rate is read from `GetAudioDeviceInfo`
		// when the override is off. A device that reports nothing usable falls through to
		// the stored project rate, and the builder clamps whatever arrives into the
		// schema's range.
		int read_project_sample_rate(
			const ReaperProjectContextSource::reaper_project_context_api& api,
			ReaProject* project)
		{
			const double project_sample_rate =
				api.GetSetProjectInfo(project, "PROJECT_SRATE", 0.0, false);

			const bool project_overrides_the_device =
				api.GetSetProjectInfo(project, "PROJECT_SRATE_USE", 0.0, false) != 0.0;

			if (project_overrides_the_device)
			{
				return static_cast<int>(project_sample_rate);
			}

			char device_sample_rate[audio_device_info_buffer_size] = {};

			if (api.GetAudioDeviceInfo("SRATE", device_sample_rate, audio_device_info_buffer_size))
			{
				const int parsed = std::atoi(device_sample_rate);

				if (parsed > 0)
				{
					return parsed;
				}
			}

			return static_cast<int>(project_sample_rate);
		}
	}

	ReaperProjectContextSource::ReaperProjectContextSource(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project)
		: api_{std::make_unique<reaper_project_context_api>()}
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
		resolve_function(
			plugin_info,
			"GetMediaTrackInfo_Value",
			api_->GetMediaTrackInfo_Value,
			unresolved_function_names_
		);
		resolve_function(
			plugin_info,
			"GetSetMediaTrackInfo_String",
			api_->GetSetMediaTrackInfo_String,
			unresolved_function_names_
		);
		resolve_function(
			plugin_info,
			"CountTrackMediaItems",
			api_->CountTrackMediaItems,
			unresolved_function_names_
		);
		resolve_function(plugin_info, "TrackFX_GetCount", api_->TrackFX_GetCount, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrackNumSends", api_->GetTrackNumSends, unresolved_function_names_);
		resolve_function(plugin_info, "IsTrackSelected", api_->IsTrackSelected, unresolved_function_names_);

		resolve_function(plugin_info, "GetProjectName", api_->GetProjectName, unresolved_function_names_);
		resolve_function(plugin_info, "GetProjectLength", api_->GetProjectLength, unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetCursorPositionEx",
			api_->GetCursorPositionEx,
			unresolved_function_names_
		);
		resolve_function(plugin_info, "GetPlayStateEx", api_->GetPlayStateEx, unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetProjectStateChangeCount",
			api_->GetProjectStateChangeCount,
			unresolved_function_names_
		);

		resolve_function(
			plugin_info,
			"GetSet_LoopTimeRange2",
			api_->GetSet_LoopTimeRange2,
			unresolved_function_names_
		);
		resolve_function(plugin_info, "GetSetRepeatEx", api_->GetSetRepeatEx, unresolved_function_names_);

		resolve_function(
			plugin_info,
			"TimeMap_GetTimeSigAtTime",
			api_->TimeMap_GetTimeSigAtTime,
			unresolved_function_names_
		);
		resolve_function(plugin_info, "GetSetProjectInfo", api_->GetSetProjectInfo, unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetAudioDeviceInfo",
			api_->GetAudioDeviceInfo,
			unresolved_function_names_
		);

		resolve_function(
			plugin_info,
			"CountProjectMarkers",
			api_->CountProjectMarkers,
			unresolved_function_names_
		);
		resolve_function(
			plugin_info,
			"CountTempoTimeSigMarkers",
			api_->CountTempoTimeSigMarkers,
			unresolved_function_names_
		);
	}

	ReaperProjectContextSource::~ReaperProjectContextSource() = default;

	bool ReaperProjectContextSource::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	const std::vector<std::string>& ReaperProjectContextSource::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	int ReaperProjectContextSource::read_project_state_change_count()
	{
		if (!is_usable())
		{
			// Constant, so the debounce never fires on an unusable source rather than
			// firing every tick on a counter that looks like it keeps changing.
			return 0;
		}

		return api_->GetProjectStateChangeCount(project_);
	}

	ProjectContextReading ReaperProjectContextSource::read_project_context()
	{
		ProjectContextReading reading;

		if (!is_usable())
		{
			// `readable` stays false, and the builder publishes nothing.
			return reading;
		}

		ProjectGlobalsReading& globals = reading.globals;

		{
			char project_file_name[name_buffer_size] = {};
			api_->GetProjectName(project_, project_file_name, name_buffer_size);

			globals.project_name = project_name_without_path_or_extension(std::string{project_file_name});
		}

		{
			int numerator = 0;
			int denominator = 0;
			double tempo = 0.0;

			// Read at the start of the timeline, which is what the schema's `tempo` and
			// `timeSignature` mean: the values at bar one. `tempoChangeCount` is what
			// says whether they hold beyond it.
			api_->TimeMap_GetTimeSigAtTime(project_, 0.0, &numerator, &denominator, &tempo);

			globals.tempo = tempo;
			globals.time_signature_numerator = numerator;
			globals.time_signature_denominator = denominator;
		}

		globals.sample_rate = read_project_sample_rate(*api_, project_);
		globals.project_length = api_->GetProjectLength(project_);
		globals.cursor_position = api_->GetCursorPositionEx(project_);
		globals.reaper_play_state = api_->GetPlayStateEx(project_);

		{
			double loop_start = 0.0;
			double loop_end = 0.0;

			api_->GetSet_LoopTimeRange2(
				project_,
				false,
				read_the_loop_range,
				&loop_start,
				&loop_end,
				false
			);

			globals.loop_start = loop_start;
			globals.loop_end = loop_end;
		}

		globals.loop_enabled = api_->GetSetRepeatEx(project_, query_repeat_state) != 0;

		{
			double selection_start = 0.0;
			double selection_end = 0.0;

			api_->GetSet_LoopTimeRange2(
				project_,
				false,
				read_the_time_selection,
				&selection_start,
				&selection_end,
				false
			);

			// REAPER reports no time selection as a zero-length range rather than as an
			// absence, and the schema wants the two fields absent in that case. A
			// zero-length range is the same thing a producer gets by clicking once in
			// the arrange view, so treating it as nothing selected matches what they
			// see.
			globals.time_selection_present = selection_end > selection_start;
			globals.time_selection_start = selection_start;
			globals.time_selection_end = selection_end;
		}

		{
			int marker_count = 0;
			int region_count = 0;

			api_->CountProjectMarkers(project_, &marker_count, &region_count);

			globals.marker_count = marker_count;
			globals.region_count = region_count;
		}

		// REAPER's tempo map holds one entry per tempo or time signature change. A
		// project that never changes either has none, which is the zero the agent reads
		// as "the reported tempo holds for the whole timeline".
		//
		// A producer who places a single point at bar one without changing anything gets
		// a count of one here, which overstates it. That direction is chosen: the
		// consequence is the agent calling `list_tempo_changes` and finding nothing
		// interesting, whereas understating it would have the agent trust a tempo that
		// stops being true at bar 33. Excluding a bar-one entry would mean enumerating
		// the map on every snapshot to check one position, which is a per-entry read for
		// a question the read tool answers exactly.
		globals.tempo_change_count = api_->CountTempoTimeSigMarkers(project_);

		const int track_count = api_->CountTracks(project_);

		reading.tracks_in_project_order.reserve(static_cast<std::size_t>(track_count < 0 ? 0 : track_count));

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track == nullptr)
			{
				// Skipping shifts nothing: the builder assigns `index` from the position
				// in this vector, and a null track in the middle of REAPER's own track
				// list is not a state the snapshot can describe honestly either way.
				continue;
			}

			TrackReading track_reading;

			track_reading.guid = read_track_string(*api_, track, "GUID", guid_buffer_size);
			track_reading.name = read_track_string(*api_, track, "P_NAME", name_buffer_size);

			track_reading.folder_depth_delta =
				static_cast<int>(api_->GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"));
			track_reading.parent_send_enabled =
				api_->GetMediaTrackInfo_Value(track, "B_MAINSEND") != 0.0;

			track_reading.item_count = api_->CountTrackMediaItems(track);
			track_reading.has_fx = api_->TrackFX_GetCount(track) > 0;
			track_reading.receive_count = api_->GetTrackNumSends(track, receive_send_category);

			track_reading.selected = api_->IsTrackSelected(track);

			reading.tracks_in_project_order.push_back(std::move(track_reading));
		}

		if (MediaTrack* const master_track = api_->GetMasterTrack(project_); master_track != nullptr)
		{
			reading.master_track.present = true;
			reading.master_track.guid = read_track_string(*api_, master_track, "GUID", guid_buffer_size);
			reading.master_track.name =
				read_track_string(*api_, master_track, "P_NAME", name_buffer_size);
			reading.master_track.has_fx = api_->TrackFX_GetCount(master_track) > 0;
			reading.master_track.receive_count =
				api_->GetTrackNumSends(master_track, receive_send_category);
		}

		reading.readable = true;

		return reading;
	}
}
