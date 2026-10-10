// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: a `track_state_host` call in, one or two REAPER C API
// calls out. No decisions — the unit conversion, the role derivation, the per-entry
// outcome reporting, the alias write outcomes, and the report caps are all in
// track_state_tools.h, which is what makes them testable without REAPER. If a change to
// this file needs a branch on anything other than "did REAPER give me a value", the
// logic belongs on the other side of the seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather than
// through REAPERAPI_IMPLEMENT. See reaper_track_state_host.h for why, and for the two
// storage translations — `I_CUSTOMCOLOR`'s enabled flag and `I_SOLO`'s third state —
// that do belong here.

#include <daw/tools/reaper_track_state_host.h>

// Before reaper_plugin.h, and that ordering is load-bearing rather than tidy.
//
// This is where `color_from_reaper_custom_color` and `reaper_custom_color_from` live —
// one implementation of `I_CUSTOMCOLOR`'s enabled flag rule, shared with the marker and
// region host rather than written a second time here. But on macOS and Linux
// reaper_plugin.h reaches for WDL's swell, which defines `min` and `max` as function-like
// macros, and marker_region_tools.h calls `std::min`. Parsed after the SDK, the macro
// eats the qualified call and the header does not compile. Parsed before it, there is
// nothing to eat. The same hazard is why nothing below this line calls `std::max`.
#include <daw/tools/marker_region_tools.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

namespace
{
	// `guidToString` documents a 64-byte destination.
	constexpr std::size_t guid_buffer_size = 64;

	// Track, FX, and project names. 512 is the `maxLength` the output schemas put on
	// each, and `track_state_tools.h` truncates to that, so this is only about giving
	// REAPER somewhere to write.
	constexpr std::size_t name_buffer_size = 1024;

	// `GetAudioDeviceInfo` writes a short decimal string for "SRATE".
	constexpr std::size_t audio_device_info_buffer_size = 64;

	// `GetTrackNumSends` categories: -1 receives, 0 sends, 1 hardware outputs. Only
	// receives are read, and only to derive the structural role — what makes a track an
	// aux return is that things feed it. Neither sends nor receives are reported by
	// `list_tracks`; `get_routing` carries those.
	constexpr int receive_send_category = -1;

	// `I_SOLO` off. REAPER writes 1 for solo and 2 for solo in place; the schema carries
	// a boolean, so a write turns on plain solo and a read collapses anything non-zero.
	constexpr double solo_off = 0.0;
	constexpr double solo_on = 1.0;

	// REAPER's project extension for a saved project. Stripped so `projectName` is the
	// name without path or extension, which is what
	// `get-project-summary.schema.json` asks for.
	constexpr const char* reaper_project_extension = ".rpp";

	// The name without directory or extension.
	//
	// `GetProjectName` hands back the file name for a saved project and an empty string
	// for one that has never been saved — which is itself worth reporting, because an
	// alias taught in a session with no file on disk does not survive the next launch
	// (requirement 8.10). Directory separators should not appear and are stripped
	// anyway: the schema says "without path", and an empty string is better than a name
	// that turns out to be a path on one platform.
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

namespace sesh_ai::daw::tools
{
	// The functions this component needs, and only those. Resolved once at construction;
	// every one is required, because a track state path missing any of them reports a
	// project the producer is not looking at, or writes to a track chosen from one.
	struct ReaperTrackStateHost::reaper_track_state_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;

		double (*GetMediaTrackInfo_Value)(MediaTrack*, const char*) = nullptr;
		bool (*SetMediaTrackInfo_Value)(MediaTrack*, const char*, double) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;

		int (*CountTrackMediaItems)(MediaTrack*) = nullptr;
		int (*TrackFX_GetCount)(MediaTrack*) = nullptr;
		bool (*TrackFX_GetFXName)(MediaTrack*, int, char*, int) = nullptr;
		int (*GetTrackNumSends)(MediaTrack*, int) = nullptr;

		void (*GetProjectName)(ReaProject*, char*, int) = nullptr;
		double (*GetProjectLength)(ReaProject*) = nullptr;
		void (*TimeMap_GetTimeSigAtTime)(ReaProject*, double, int*, int*, double*) = nullptr;
		double (*GetSetProjectInfo)(ReaProject*, const char*, double, bool) = nullptr;
		bool (*GetAudioDeviceInfo)(const char*, char*, int) = nullptr;
		int (*CountProjectMarkers)(ReaProject*, int*, int*) = nullptr;
		int (*CountTempoTimeSigMarkers)(ReaProject*) = nullptr;
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

		std::string read_track_string(
			const ReaperTrackStateHost::reaper_track_state_api& api,
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
		// REAPER keeps a project sample rate and a flag saying whether the project
		// overrides the audio device. With the flag off the project runs at whatever the
		// device is set to, and reporting the stored override instead would tell the
		// agent the session is 48 kHz while REAPER renders at 44.1. The reasoning and
		// the fallback order match `reaper_project_context_source.cpp`, deliberately: the
		// summary and the snapshot report the same field and must not disagree about it.
		int read_project_sample_rate(
			const ReaperTrackStateHost::reaper_track_state_api& api,
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

	ReaperTrackStateHost::ReaperTrackStateHost(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project)
		: api_{std::make_unique<reaper_track_state_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);

		resolve_function(
			plugin_info,
			"GetMediaTrackInfo_Value",
			api_->GetMediaTrackInfo_Value,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"SetMediaTrackInfo_Value",
			api_->SetMediaTrackInfo_Value,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetSetMediaTrackInfo_String",
			api_->GetSetMediaTrackInfo_String,
			unresolved_function_names_);

		resolve_function(
			plugin_info,
			"CountTrackMediaItems",
			api_->CountTrackMediaItems,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"TrackFX_GetCount",
			api_->TrackFX_GetCount,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"TrackFX_GetFXName",
			api_->TrackFX_GetFXName,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetTrackNumSends",
			api_->GetTrackNumSends,
			unresolved_function_names_);

		resolve_function(plugin_info, "GetProjectName", api_->GetProjectName, unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetProjectLength",
			api_->GetProjectLength,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"TimeMap_GetTimeSigAtTime",
			api_->TimeMap_GetTimeSigAtTime,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetSetProjectInfo",
			api_->GetSetProjectInfo,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetAudioDeviceInfo",
			api_->GetAudioDeviceInfo,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"CountProjectMarkers",
			api_->CountProjectMarkers,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"CountTempoTimeSigMarkers",
			api_->CountTempoTimeSigMarkers,
			unresolved_function_names_);
	}

	ReaperTrackStateHost::~ReaperTrackStateHost() = default;

	bool ReaperTrackStateHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	std::vector<std::string> ReaperTrackStateHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	std::vector<track_state_reading> ReaperTrackStateHost::read_tracks_in_project_order(
		bool include_fx_names)
	{
		std::vector<track_state_reading> tracks;

		if (!is_usable())
		{
			return tracks;
		}

		const int track_count = api_->CountTracks(project_);

		// Not `std::max`. On macOS and Linux reaper_plugin.h reaches for WDL's swell,
		// which defines `max` as a function-like macro, and the macro eats the qualified
		// call before the compiler sees it.
		tracks.reserve(static_cast<std::size_t>(track_count > 0 ? track_count : 0));

		for (int index = 0; index < track_count; ++index)
		{
			MediaTrack* const track = api_->GetTrack(project_, index);

			if (track == nullptr)
			{
				continue;
			}

			track_state_reading reading;
			reading.guid = read_track_string(*api_, track, "GUID", guid_buffer_size);
			reading.name = read_track_string(*api_, track, "P_NAME", name_buffer_size);
			reading.track = track;

			reading.reaper_volume = api_->GetMediaTrackInfo_Value(track, "D_VOL");
			reading.reaper_pan = api_->GetMediaTrackInfo_Value(track, "D_PAN");

			reading.muted = api_->GetMediaTrackInfo_Value(track, "B_MUTE") != 0.0;
			reading.soloed = api_->GetMediaTrackInfo_Value(track, "I_SOLO") != solo_off;
			reading.armed = api_->GetMediaTrackInfo_Value(track, "I_RECARM") != 0.0;

			reading.color = color_from_reaper_custom_color(
				static_cast<int>(api_->GetMediaTrackInfo_Value(track, "I_CUSTOMCOLOR")));

			reading.fx_count = api_->TrackFX_GetCount(track);

			// Read only when the call asked for the chain. `fx_count` is filled either
			// way, because the structural role turns on whether a chain holds anything
			// and a role that changed with a reporting flag would have one session
			// classified two ways by two calls.
			if (include_fx_names)
			{
				reading.fx_names.reserve(
					static_cast<std::size_t>(reading.fx_count > 0 ? reading.fx_count : 0));

				for (int fx_index = 0; fx_index < reading.fx_count; ++fx_index)
				{
					char fx_name[name_buffer_size] = {};

					if (!api_->TrackFX_GetFXName(track, fx_index, fx_name, name_buffer_size))
					{
						// An FX REAPER would not name is still on the producer's chain,
						// so its position is kept with an empty name rather than dropped —
						// dropping it would shift every later position in the reported
						// chain order.
						reading.fx_names.emplace_back();
						continue;
					}

					reading.fx_names.emplace_back(fx_name);
				}
			}

			reading.folder_depth_delta =
				static_cast<int>(api_->GetMediaTrackInfo_Value(track, "I_FOLDERDEPTH"));
			reading.parent_send_enabled = api_->GetMediaTrackInfo_Value(track, "B_MAINSEND") != 0.0;

			reading.item_count = api_->CountTrackMediaItems(track);
			reading.receive_count = api_->GetTrackNumSends(track, receive_send_category);

			tracks.push_back(std::move(reading));
		}

		return tracks;
	}

	project_summary_reading ReaperTrackStateHost::read_project_summary()
	{
		project_summary_reading reading;

		if (!is_usable())
		{
			// `readable` stays false, and the handler reports a failed action rather than
			// a summary assembled from defaults.
			return reading;
		}

		{
			char project_file_name[name_buffer_size] = {};
			api_->GetProjectName(project_, project_file_name, name_buffer_size);

			reading.project_name =
				project_name_without_path_or_extension(std::string{project_file_name});
		}

		{
			int numerator = 0;
			int denominator = 0;
			double tempo = 0.0;

			// Read at the start of the timeline, which is what the schema's `tempo` and
			// `timeSignature` mean: the values at bar one. `tempoChangeCount` is what
			// says whether they hold beyond it.
			api_->TimeMap_GetTimeSigAtTime(project_, 0.0, &numerator, &denominator, &tempo);

			reading.tempo = tempo;
			reading.time_signature_numerator = numerator;
			reading.time_signature_denominator = denominator;
		}

		reading.sample_rate = read_project_sample_rate(*api_, project_);
		reading.project_length = api_->GetProjectLength(project_);

		// Excluding the master track, which sits outside the indexed track list — which
		// is what the schema says `trackCount` means.
		reading.track_count = api_->CountTracks(project_);

		{
			int marker_count = 0;
			int region_count = 0;

			api_->CountProjectMarkers(project_, &marker_count, &region_count);

			reading.marker_count = marker_count;
			reading.region_count = region_count;
		}

		// One entry per tempo or time signature change. A project that never changes
		// either has none, which is the zero the agent reads as "the reported tempo holds
		// for the whole timeline". The same overstatement `reaper_project_context_source.cpp`
		// documents applies: a producer who places a point at bar one without changing
		// anything counts as one, and the consequence is the agent calling
		// `list_tempo_changes` and finding nothing interesting.
		reading.tempo_change_count = api_->CountTempoTimeSigMarkers(project_);

		reading.readable = true;

		return reading;
	}

	namespace
	{
		// The track carrying this GUID, or null.
		//
		// A walk rather than a lookup, following `reaper_track_structure_host.cpp`:
		// REAPER offers no index from GUID back to track, and the alternative is a
		// mapping this host would have to keep correct across every edit the producer
		// makes in REAPER's own UI.
		MediaTrack* find_track_by_guid(
			const ReaperTrackStateHost::reaper_track_state_api& api,
			ReaProject* project,
			const std::string& guid)
		{
			const int track_count = api.CountTracks(project);

			for (int index = 0; index < track_count; ++index)
			{
				MediaTrack* const track = api.GetTrack(project, index);

				if (track == nullptr)
				{
					continue;
				}

				if (read_track_string(api, track, "GUID", guid_buffer_size) == guid)
				{
					return track;
				}
			}

			return nullptr;
		}
	}

	bool ReaperTrackStateHost::write_track_name(const std::string& track_guid, const std::string& name)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		// `GetSetMediaTrackInfo_String` writes through a non-const buffer even when
		// setting, so the name is copied rather than pointed at.
		std::string writable_name = name;

		return api_->GetSetMediaTrackInfo_String(track, "P_NAME", writable_name.data(), true);
	}

	bool ReaperTrackStateHost::write_track_volume(const std::string& track_guid, double reaper_volume)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		// Already REAPER's linear gain factor. The decibels the producer said were
		// converted on the other side of the seam, which is the only place that
		// conversion happens.
		return api_->SetMediaTrackInfo_Value(track, "D_VOL", reaper_volume);
	}

	bool ReaperTrackStateHost::write_track_pan(const std::string& track_guid, double reaper_pan)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		return api_->SetMediaTrackInfo_Value(track, "D_PAN", reaper_pan);
	}

	bool ReaperTrackStateHost::write_track_muted(const std::string& track_guid, bool muted)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		return api_->SetMediaTrackInfo_Value(track, "B_MUTE", muted ? 1.0 : 0.0);
	}

	bool ReaperTrackStateHost::write_track_soloed(const std::string& track_guid, bool soloed)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		// Plain solo rather than solo in place. The schema carries a boolean, so there is
		// no way for the producer to have asked for the other one, and choosing it for
		// them would change what their other tracks do.
		return api_->SetMediaTrackInfo_Value(track, "I_SOLO", soloed ? solo_on : solo_off);
	}

	bool ReaperTrackStateHost::write_track_armed(const std::string& track_guid, bool armed)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		return api_->SetMediaTrackInfo_Value(track, "I_RECARM", armed ? 1.0 : 0.0);
	}

	bool ReaperTrackStateHost::write_track_color(const std::string& track_guid, int color)
	{
		if (!is_usable())
		{
			return false;
		}

		MediaTrack* const track = find_track_by_guid(*api_, project_, track_guid);

		if (track == nullptr)
		{
			return false;
		}

		// `I_CUSTOMCOLOR` is only honoured with the enabled flag set. Without it REAPER
		// stores the value and ignores it, which would leave the producer looking at a
		// default colour after a call that reported setting one.
		return api_->SetMediaTrackInfo_Value(
			track,
			"I_CUSTOMCOLOR",
			static_cast<double>(reaper_custom_color_from(color)));
	}
}
