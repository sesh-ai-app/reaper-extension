// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: a `RenderHost` call in, one or two REAPER C API
// calls out. No decisions — the settings composition, the path collision detection,
// and the solo loop all live in render_coordinator.h, which is what makes them
// testable without REAPER. If a change to this file needs a
// branch on anything other than "did REAPER give me a value", the logic belongs on
// the other side of the seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather
// than through REAPERAPI_IMPLEMENT. See reaper_render_host.h for why.

#include <daw/reaper_render_host.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

namespace
{
	// REAPER's own type for a region or marker handle. Declared by
	// reaper_plugin_functions.h, which this file does not include — the imports it
	// declares would need storage this translation unit deliberately does not provide.
	class ProjectMarker;

	// Buffer sizes.
	//
	// `RENDER_TARGETS` is the large one: `outputs/render.schema.json` caps
	// `outputPaths` at 512 entries of up to 4096 characters, so a megabyte covers the
	// widest list the contract allows plus the separators. REAPER truncates to the
	// buffer rather than overrunning it, and a truncated final path is caught
	// downstream as a path that does not exist rather than silently written to.
	constexpr std::size_t render_targets_buffer_size = 1024 * 1024;

	// `guidToString` documents a 64-byte destination.
	constexpr std::size_t guid_buffer_size = 64;

	// Track names and directory paths. 4096 matches the schema's path bound.
	constexpr std::size_t path_buffer_size = 4096;

	// "File: Add project to render queue, using the most recent render settings".
	//
	// Built-in REAPER actions are addressed by numeric command ID — `NamedCommandLookup`
	// resolves the `_RS...` identifiers that scripts and extensions register, not these.
	// This is the ID community ReaScripts use for queueing, and it is the single value
	// in this component that cannot be checked from the SDK headers: the action list is
	// REAPER's, not the SDK's. It belongs in the SDK validation step alongside ADR
	// 0018's other open items, and it is named here rather than written inline so that
	// verifying it is a one-line change.
	constexpr int add_project_to_render_queue_command = 41823;

	// `I_SOLO`: 0 = not soloed, 1 = soloed, 2 = soloed in place. Plain solo is what the
	// via-master loop wants — solo in place leaves the folder parent's other children
	// audible, which would put signal the producer did not ask for into the stem.
	constexpr double solo_off = 0.0;
	constexpr double solo_on = 1.0;
}

namespace sesh_ai::daw
{
	// The functions this component needs, and only those. Resolved once at
	// construction; every one of them is required, because a render path missing any
	// of them writes something other than what was asked for.
	struct ReaperRenderHost::reaper_render_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		int (*CountSelectedTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetSelectedTrack)(ReaProject*, int) = nullptr;
		bool (*SetMediaTrackInfo_Value)(MediaTrack*, const char*, double) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;

		double (*GetProjectLength)(ReaProject*) = nullptr;
		void (*GetSet_LoopTimeRange2)(ReaProject*, bool, bool, double*, double*, bool) = nullptr;
		void (*GetProjectPathEx)(ReaProject*, char*, int) = nullptr;

		double (*GetSetProjectInfo)(ReaProject*, const char*, double, bool) = nullptr;
		bool (*GetSetProjectInfo_String)(ReaProject*, const char*, char*, bool) = nullptr;

		ProjectMarker* (*GetRegionOrMarker)(ReaProject*, int, const char*) = nullptr;
		double (*GetRegionOrMarkerInfo_Value)(ReaProject*, ProjectMarker*, const char*) = nullptr;

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

		// The braced GUID string form the refusal schema's `guid` pattern expects, and
		// the form `GetRegionOrMarker` takes for a GUID lookup.
		std::string read_track_guid(
			const ReaperRenderHost::reaper_render_api& api,
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
			const ReaperRenderHost::reaper_render_api& api,
			MediaTrack* track)
		{
			char name_buffer[path_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "P_NAME", name_buffer, false))
			{
				return {};
			}

			return std::string{name_buffer};
		}
	}

	ReaperRenderHost::ReaperRenderHost(reaper_plugin_info_t* plugin_info, ReaProject* project)
		: api_{std::make_unique<reaper_render_api>()}
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
		resolve_function(plugin_info, "SetMediaTrackInfo_Value", api_->SetMediaTrackInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String, unresolved_function_names_);

		resolve_function(plugin_info, "GetProjectLength", api_->GetProjectLength, unresolved_function_names_);
		resolve_function(plugin_info, "GetSet_LoopTimeRange2", api_->GetSet_LoopTimeRange2, unresolved_function_names_);
		resolve_function(plugin_info, "GetProjectPathEx", api_->GetProjectPathEx, unresolved_function_names_);

		resolve_function(plugin_info, "GetSetProjectInfo", api_->GetSetProjectInfo, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetProjectInfo_String", api_->GetSetProjectInfo_String, unresolved_function_names_);

		resolve_function(plugin_info, "GetRegionOrMarker", api_->GetRegionOrMarker, unresolved_function_names_);
		resolve_function(plugin_info, "GetRegionOrMarkerInfo_Value", api_->GetRegionOrMarkerInfo_Value, unresolved_function_names_);

		resolve_function(plugin_info, "Main_OnCommand", api_->Main_OnCommand, unresolved_function_names_);
	}

	ReaperRenderHost::~ReaperRenderHost() = default;

	bool ReaperRenderHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	const std::vector<std::string>& ReaperRenderHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	render_project_state ReaperRenderHost::read_project_state()
	{
		render_project_state state;

		if (!is_usable())
		{
			return state;
		}

		state.project_length_seconds = api_->GetProjectLength(project_);

		double time_selection_start = 0.0;
		double time_selection_end = 0.0;

		// isLoop false asks for the time selection rather than the loop range, which
		// REAPER keeps as two independent spans even though the UI usually links them.
		api_->GetSet_LoopTimeRange2(project_, false, false, &time_selection_start, &time_selection_end, false);

		state.time_selection = time_span{time_selection_start, time_selection_end};

		// PROJECT_SRATE is only meaningful when PROJECT_SRATE_USE is set; otherwise the
		// project runs at the audio device's rate, and REAPER reports zero here. Zero
		// would reach the result as a sample rate the output schema rejects, so it is
		// reported as absent-shaped (zero) and the coordinator's caller sees the
		// project's own answer rather than a fabricated 44100.
		const bool project_sample_rate_is_used =
			api_->GetSetProjectInfo(project_, "PROJECT_SRATE_USE", 0.0, false) != 0.0;

		if (project_sample_rate_is_used)
		{
			state.sample_rate = static_cast<int>(api_->GetSetProjectInfo(project_, "PROJECT_SRATE", 0.0, false));
		}

		return state;
	}

	std::vector<render_track> ReaperRenderHost::read_selected_tracks()
	{
		std::vector<render_track> selected_tracks;

		if (!is_usable())
		{
			return selected_tracks;
		}

		// Identity only. A queued render job addresses a track by GUID and reports it
		// by name, and the Render Coordinator reads nothing else off a selected track —
		// it queues what the selection resolved to without classifying it. Structural
		// role is derived and reported by the Project Context Builder (requirement
		// 6.5), which is where the agent reads it from.
		const int selected_track_count = api_->CountSelectedTracks(project_);

		for (int selection_index = 0; selection_index < selected_track_count; ++selection_index)
		{
			MediaTrack* const track = api_->GetSelectedTrack(project_, selection_index);

			if (track == nullptr)
			{
				continue;
			}

			render_track selected_track;
			selected_track.guid = read_track_guid(*api_, track);
			selected_track.name = read_track_name(*api_, track);

			selected_tracks.push_back(std::move(selected_track));
		}

		return selected_tracks;
	}

	std::optional<time_span> ReaperRenderHost::find_region_span(const std::string& region_guid)
	{
		if (!is_usable() || region_guid.empty())
		{
			return std::nullopt;
		}

		// A negative index is REAPER's own "look this up by GUID" for
		// `GetRegionOrMarker`, which resolves ADR 0018's open question about GUID
		// addressing for regions: it is a first-class lookup rather than an
		// enumerate-and-compare.
		ProjectMarker* const region = api_->GetRegionOrMarker(project_, -1, region_guid.c_str());

		if (region == nullptr)
		{
			return std::nullopt;
		}

		// `D_ENDPOS` equals `D_STARTPOS` for a marker, so a GUID naming a marker rather
		// than a region resolves to a zero-length span. The coordinator refuses that on
		// the end-greater-than-start check, which is the right answer: a marker is a
		// point, and a render needs a span.
		if (api_->GetRegionOrMarkerInfo_Value(project_, region, "B_ISREGION") == 0.0)
		{
			return std::nullopt;
		}

		return time_span{
			api_->GetRegionOrMarkerInfo_Value(project_, region, "D_STARTPOS"),
			api_->GetRegionOrMarkerInfo_Value(project_, region, "D_ENDPOS"),
		};
	}

	std::string ReaperRenderHost::read_render_directory()
	{
		if (!is_usable())
		{
			return {};
		}

		char directory_buffer[path_buffer_size] = {};

		if (api_->GetSetProjectInfo_String(project_, "RENDER_FILE", directory_buffer, false)
			&& directory_buffer[0] != '\0')
		{
			// RENDER_FILE may be relative to the project path, in which case the
			// absolute form is what the producer needs in order to find the files.
			const std::filesystem::path configured_directory{directory_buffer};

			if (configured_directory.is_absolute())
			{
				return configured_directory.string();
			}

			char project_path_buffer[path_buffer_size] = {};
			api_->GetProjectPathEx(project_, project_path_buffer, static_cast<int>(path_buffer_size));

			return (std::filesystem::path{project_path_buffer} / configured_directory).string();
		}

		// Blank means the project's own media directory, which is what REAPER falls
		// back to.
		char project_path_buffer[path_buffer_size] = {};
		api_->GetProjectPathEx(project_, project_path_buffer, static_cast<int>(path_buffer_size));

		return std::string{project_path_buffer};
	}

	void ReaperRenderHost::write_render_settings(render_settings_word settings)
	{
		if (!is_usable())
		{
			return;
		}

		// The single `RENDER_SETTINGS` write. There is no other call to this key
		// anywhere in the extension, and `single_render_settings_write` is what keeps
		// the coordinator from reaching it twice — see render_coordinator.h.
		api_->GetSetProjectInfo(project_, "RENDER_SETTINGS", static_cast<double>(settings.value()), true);
	}

	void ReaperRenderHost::write_render_bounds(int bounds_flag, const time_span& span)
	{
		if (!is_usable())
		{
			return;
		}

		api_->GetSetProjectInfo(project_, "RENDER_BOUNDSFLAG", static_cast<double>(bounds_flag), true);

		// `RENDER_STARTPOS` and `RENDER_ENDPOS` are read by REAPER only when the bounds
		// flag is custom bounds, but they are written unconditionally so that a stale
		// pair from a previous render cannot be what a later custom-bounds render picks
		// up. Writing them is free; leaving them wrong is a render of the wrong span.
		api_->GetSetProjectInfo(project_, "RENDER_STARTPOS", span.start_seconds, true);
		api_->GetSetProjectInfo(project_, "RENDER_ENDPOS", span.end_seconds, true);
	}

	void ReaperRenderHost::write_render_sample_rate(int sample_rate)
	{
		if (!is_usable())
		{
			return;
		}

		api_->GetSetProjectInfo(project_, "RENDER_SRATE", static_cast<double>(sample_rate), true);
	}

	void ReaperRenderHost::write_render_format(const render_format_request& format)
	{
		if (!is_usable())
		{
			return;
		}

		// `RENDER_FORMAT` takes either a base64-encoded sink configuration or a plain
		// four-byte code meaning "default settings for that sink type". The four-byte
		// form is what is written here, so the bit depth the call asked for is not
		// applied — expressing it needs the encoded configuration, which differs per
		// sink type and is the remaining piece of this method.
		//
		// The result reports the requested depth either way, which would be a lie if it
		// claimed the depth was applied. It does not: `outputs/render.schema.json`
		// describes `bitDepth` as what the queued jobs will write, and until the sink
		// configuration is encoded the honest value is the project's own setting. The
		// render tool handler (task 10.9) should omit `bitDepth` from the result while
		// this is the case rather than report a depth that was not set.
		std::string sink_configuration{format.sink_four_character_code};
		sink_configuration.resize(path_buffer_size, '\0');

		api_->GetSetProjectInfo_String(project_, "RENDER_FORMAT", sink_configuration.data(), true);
	}

	void ReaperRenderHost::write_render_output(
		const std::string& directory,
		const std::string& file_name_pattern)
	{
		if (!is_usable())
		{
			return;
		}

		// `GetSetProjectInfo_String` takes a mutable buffer even when setting, so both
		// values are copied into one that REAPER is free to write into.
		std::string directory_buffer = directory;
		directory_buffer.resize(path_buffer_size, '\0');
		api_->GetSetProjectInfo_String(project_, "RENDER_FILE", directory_buffer.data(), true);

		std::string pattern_buffer = file_name_pattern;
		pattern_buffer.resize(path_buffer_size, '\0');
		api_->GetSetProjectInfo_String(project_, "RENDER_PATTERN", pattern_buffer.data(), true);
	}

	std::string ReaperRenderHost::read_render_targets()
	{
		if (!is_usable())
		{
			return {};
		}

		// This is REAPER's own wildcard resolution, not a reimplementation of it:
		// `RENDER_TARGETS` is documented as the semicolon-separated list of files that
		// would be written if the project were rendered with the settings as they now
		// stand. Reading it after the settings and pattern are written is what makes
		// collision detection run against the paths REAPER would actually write.
		std::vector<char> render_targets_buffer(render_targets_buffer_size, '\0');

		if (!api_->GetSetProjectInfo_String(project_, "RENDER_TARGETS", render_targets_buffer.data(), false))
		{
			return {};
		}

		// REAPER null-terminates within the buffer; constructing from the pointer
		// rather than from the vector's size stops the trailing padding becoming part
		// of the last path.
		return std::string{render_targets_buffer.data()};
	}

	bool ReaperRenderHost::output_file_exists(const std::string& resolved_path)
	{
		if (resolved_path.empty())
		{
			return false;
		}

		// std::filesystem rather than a REAPER call: this is a filesystem question, and
		// the error_code overload turns a permission failure on a parent directory into
		// "not found" rather than an exception thrown across a tool call.
		std::error_code existence_error;
		const bool exists = std::filesystem::exists(std::filesystem::path{resolved_path}, existence_error);

		if (existence_error)
		{
			// Unreadable is not the same as absent, and treating it as absent would
			// queue a render that then fails to write. Reported as existing, which
			// escalates to the producer instead.
			return true;
		}

		return exists;
	}

	void ReaperRenderHost::set_track_solo(const std::string& track_guid, bool soloed)
	{
		if (!is_usable() || track_guid.empty())
		{
			return;
		}

		const int track_count = api_->CountTracks(project_);

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track == nullptr)
			{
				continue;
			}

			if (read_track_guid(*api_, track) != track_guid)
			{
				continue;
			}

			api_->SetMediaTrackInfo_Value(track, "I_SOLO", soloed ? solo_on : solo_off);
			return;
		}
	}

	void ReaperRenderHost::add_project_to_render_queue()
	{
		if (!is_usable())
		{
			return;
		}

		// Queue-only (requirement 12.1). There is no render-now command anywhere in
		// this component, which is the point: the action that would block REAPER for
		// minutes is not reachable from the tool.
		api_->Main_OnCommand(add_project_to_render_queue_command, 0);
	}
}
