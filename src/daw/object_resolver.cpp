// The REAPER-facing half of object resolution: the walks that turn REAPER's track
// list, markers, and regions into the plain snapshots object_resolver.h reasons over.
//
// This is the only translation unit in the component that includes the SDK, following
// entry/reaper_timer_registrar.cpp. Everything that decides anything — precedence,
// match tiers, the three outcomes, the refusals — is in the header, where the Catch2
// suite can reach it without the SDK on its include path. What is here is collection:
// count, walk, read, append. There is no branch in this file that a test would want to
// cover and could not reach through a synthetic track list instead.
//
// Every API pointer is null-checked before use. The SDK is explicit that the API is
// dynamic and a caller must not assume a function exists — an older REAPER can leave
// any of these null, and a resolver that crashes REAPER on an unexpected version is a
// producer losing a session. A missing function yields an empty or partial snapshot,
// which resolution then refuses on. That is the direction worth failing in.
//
// The function pointers are resolved from the registration table's `GetFunc` at
// construction rather than through REAPERAPI_IMPLEMENT. See object_resolver.h for
// why. Each of the two sources resolves its own, including the ones they share: the
// duplication is cheaper than a shared REAPER object that every component would then
// have to be handed.
//
// `daw/object_resolver.h` is included before the SDK, and that ordering is
// load-bearing rather than tidy — on macOS and Linux reaper_plugin.h reaches for
// WDL's swell, which defines `min` and `max` as function-like macros, and a
// qualified `std::min` parsed after them does not compile.

#include <daw/object_resolver.h>

#include <reaper_plugin.h>

#include <utility>

namespace
{
	// REAPER's own type for a region or marker handle. Declared by
	// reaper_plugin_functions.h, which this file does not include — the imports it
	// declares would need storage this translation unit deliberately does not provide.
	class ProjectMarker;

	// `guidToString` documents its destination as needing 64 bytes.
	constexpr int guid_buffer_size = 64;

	// Track and marker names. The schemas cap a reported name at 512 characters;
	// the buffer is larger so that a longer name in REAPER is truncated by the
	// serialiser against the contract rather than by an undersized read here.
	constexpr int name_buffer_size = 1024;
}

namespace sesh_ai::daw
{
	// The six functions the track walk needs, and only those.
	struct ReaperTrackListSource::reaper_track_list_api
	{
		int (*CountTracks)(ReaProject*) = nullptr;
		MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
		MediaTrack* (*GetMasterTrack)(ReaProject*) = nullptr;

		GUID* (*GetTrackGUID)(MediaTrack*) = nullptr;
		void (*guidToString)(const GUID*, char*) = nullptr;
		bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;
	};

	// The four the marker and region walk needs.
	struct ReaperMarkerAndRegionSource::reaper_marker_and_region_api
	{
		int (*GetNumRegionsOrMarkers)(ReaProject*) = nullptr;
		ProjectMarker* (*GetRegionOrMarker)(ReaProject*, int, const char*) = nullptr;
		double (*GetRegionOrMarkerInfo_Value)(ReaProject*, ProjectMarker*, const char*) = nullptr;
		bool (*GetSetRegionOrMarkerInfo_String)(
			ReaProject*, ProjectMarker*, const char*, char*, bool) = nullptr;
	};

	namespace
	{
		// Resolves one function and records the name when it is missing, so a source
		// that produced a partial snapshot can say which functions REAPER did not
		// supply rather than only that the snapshot was short.
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

		// The helpers take the resolved pointers rather than holding them, which keeps
		// them where they were — file-local, in the anonymous namespace, with nothing
		// of the class in them. Same shape as `reaper_render_host.cpp`, and it is also
		// what lets the track reads serve both the indexed walk and the master track
		// without either of them being a member call.
		std::string read_track_guid(
			const ReaperTrackListSource::reaper_track_list_api& api,
			MediaTrack* track)
		{
			if (track == nullptr || api.GetTrackGUID == nullptr || api.guidToString == nullptr)
			{
				return {};
			}

			const GUID* const guid = api.GetTrackGUID(track);

			if (guid == nullptr)
			{
				return {};
			}

			char guid_text[guid_buffer_size] = {};
			api.guidToString(guid, guid_text);

			return std::string{guid_text};
		}

		// An unnamed track in REAPER has no name rather than a placeholder, and the
		// master track returns null for P_NAME. Both come back as an empty string,
		// which matches no pattern — the resolver treats an empty candidate string as
		// unmatchable rather than as a wildcard.
		std::string read_track_name(
			const ReaperTrackListSource::reaper_track_list_api& api,
			MediaTrack* track)
		{
			if (track == nullptr || api.GetSetMediaTrackInfo_String == nullptr)
			{
				return {};
			}

			char name[name_buffer_size] = {};

			if (!api.GetSetMediaTrackInfo_String(track, "P_NAME", name, false))
			{
				return {};
			}

			return std::string{name};
		}

		std::string read_marker_or_region_guid(
			const ReaperMarkerAndRegionSource::reaper_marker_and_region_api& api,
			ReaProject* project,
			ProjectMarker* marker_or_region)
		{
			if (marker_or_region == nullptr || api.GetSetRegionOrMarkerInfo_String == nullptr)
			{
				return {};
			}

			char guid_text[guid_buffer_size] = {};

			// "GUID" is read-only, so the set flag is false and the buffer is an out
			// parameter.
			if (!api.GetSetRegionOrMarkerInfo_String(project, marker_or_region, "GUID", guid_text, false))
			{
				return {};
			}

			return std::string{guid_text};
		}

		std::string read_marker_or_region_name(
			const ReaperMarkerAndRegionSource::reaper_marker_and_region_api& api,
			ReaProject* project,
			ProjectMarker* marker_or_region)
		{
			if (marker_or_region == nullptr || api.GetSetRegionOrMarkerInfo_String == nullptr)
			{
				return {};
			}

			char name[name_buffer_size] = {};

			if (!api.GetSetRegionOrMarkerInfo_String(project, marker_or_region, "P_NAME", name, false))
			{
				return {};
			}

			return std::string{name};
		}

		double read_marker_or_region_value(
			const ReaperMarkerAndRegionSource::reaper_marker_and_region_api& api,
			ReaProject* project,
			ProjectMarker* marker_or_region,
			const char* parameter_name)
		{
			if (marker_or_region == nullptr || api.GetRegionOrMarkerInfo_Value == nullptr)
			{
				return 0.0;
			}

			return api.GetRegionOrMarkerInfo_Value(project, marker_or_region, parameter_name);
		}
	}

	ReaperTrackListSource::ReaperTrackListSource(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project,
		StructuralRoleProvider structural_role_provider)
		: api_{std::make_unique<reaper_track_list_api>()}
		, project_{project}
		, structural_role_provider_{std::move(structural_role_provider)}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			// Every pointer stays null, so the walk below returns an empty snapshot
			// and resolution refuses on it — the same outcome as a REAPER that
			// exposed none of these.
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
		resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);
		resolve_function(plugin_info, "GetMasterTrack", api_->GetMasterTrack, unresolved_function_names_);

		resolve_function(plugin_info, "GetTrackGUID", api_->GetTrackGUID, unresolved_function_names_);
		resolve_function(plugin_info, "guidToString", api_->guidToString, unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetSetMediaTrackInfo_String",
			api_->GetSetMediaTrackInfo_String,
			unresolved_function_names_);
	}

	ReaperTrackListSource::~ReaperTrackListSource() = default;

	std::vector<ResolvableTrack> ReaperTrackListSource::tracks_in_project_order() const
	{
		std::vector<ResolvableTrack> tracks;

		if (api_->CountTracks == nullptr || api_->GetTrack == nullptr)
		{
			return tracks;
		}

		const int track_count = api_->CountTracks(project_);

		if (track_count > 0)
		{
			tracks.reserve(static_cast<std::size_t>(track_count) + 1);
		}

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = api_->GetTrack(project_, track_index);

			if (track == nullptr)
			{
				continue;
			}

			ResolvableTrack resolvable;
			resolvable.guid = read_track_guid(*api_, track);
			resolvable.name = read_track_name(*api_, track);
			resolvable.track = track;
			resolvable.project_index = track_index;
			resolvable.is_master_track = false;

			if (structural_role_provider_ && !resolvable.guid.empty())
			{
				resolvable.structural_role = structural_role_provider_(resolvable.guid);
			}

			tracks.push_back(std::move(resolvable));
		}

		// The master is not in the indexed list, so requirement 7.1's walk would never
		// reach it — and "the master" is a reference a producer makes constantly. It is
		// appended rather than inserted so that the indexed tracks keep the order the
		// producer sees, which is the order an ambiguity enumerates candidates in.
		if (api_->GetMasterTrack != nullptr)
		{
			MediaTrack* const master_track = api_->GetMasterTrack(project_);

			if (master_track != nullptr)
			{
				ResolvableTrack resolvable;
				resolvable.guid = read_track_guid(*api_, master_track);
				resolvable.name = read_track_name(*api_, master_track);
				resolvable.track = master_track;
				resolvable.project_index = -1;
				resolvable.is_master_track = true;
				resolvable.structural_role = context::StructuralRole::master;

				tracks.push_back(std::move(resolvable));
			}
		}

		return tracks;
	}

	ReaperMarkerAndRegionSource::ReaperMarkerAndRegionSource(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project)
		: api_{std::make_unique<reaper_marker_and_region_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(
			plugin_info,
			"GetNumRegionsOrMarkers",
			api_->GetNumRegionsOrMarkers,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetRegionOrMarker",
			api_->GetRegionOrMarker,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetRegionOrMarkerInfo_Value",
			api_->GetRegionOrMarkerInfo_Value,
			unresolved_function_names_);
		resolve_function(
			plugin_info,
			"GetSetRegionOrMarkerInfo_String",
			api_->GetSetRegionOrMarkerInfo_String,
			unresolved_function_names_);
	}

	ReaperMarkerAndRegionSource::~ReaperMarkerAndRegionSource() = default;

	std::vector<ResolvableMarkerOrRegion>
	ReaperMarkerAndRegionSource::markers_and_regions_in_enumeration_order() const
	{
		std::vector<ResolvableMarkerOrRegion> markers_and_regions;

		if (api_->GetNumRegionsOrMarkers == nullptr || api_->GetRegionOrMarker == nullptr)
		{
			return markers_and_regions;
		}

		const int marker_and_region_count = api_->GetNumRegionsOrMarkers(project_);

		if (marker_and_region_count > 0)
		{
			markers_and_regions.reserve(static_cast<std::size_t>(marker_and_region_count));
		}

		for (int enumeration_index = 0; enumeration_index < marker_and_region_count; ++enumeration_index)
		{
			// The third argument is the by-GUID lookup this deliberately does not use;
			// null asks for the by-index form. A null return is skipped rather than
			// ending the walk, because REAPER documents this index as internal and
			// makes no promise that it is contiguous — stopping at the first gap would
			// silently shorten the snapshot, and a short snapshot resolves a GUID that
			// exists to `unresolved`.
			ProjectMarker* const marker_or_region =
				api_->GetRegionOrMarker(project_, enumeration_index, nullptr);

			if (marker_or_region == nullptr)
			{
				continue;
			}

			ResolvableMarkerOrRegion resolvable;
			resolvable.guid = read_marker_or_region_guid(*api_, project_, marker_or_region);
			resolvable.name = read_marker_or_region_name(*api_, project_, marker_or_region);
			resolvable.is_region =
				read_marker_or_region_value(*api_, project_, marker_or_region, "B_ISREGION") != 0.0;
			resolvable.start_seconds =
				read_marker_or_region_value(*api_, project_, marker_or_region, "D_STARTPOS");
			resolvable.end_seconds =
				read_marker_or_region_value(*api_, project_, marker_or_region, "D_ENDPOS");
			resolvable.internal_index = static_cast<int>(
				read_marker_or_region_value(*api_, project_, marker_or_region, "I_INDEX"));
			resolvable.displayed_number = static_cast<int>(
				read_marker_or_region_value(*api_, project_, marker_or_region, "I_NUMBER"));

			markers_and_regions.push_back(std::move(resolvable));
		}

		return markers_and_regions;
	}
}
