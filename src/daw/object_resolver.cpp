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
// reaper_plugin_functions.h is included without REAPERAPI_IMPLEMENT, so these are
// extern declarations of the imported pointers. The one translation unit that defines
// them belongs with the plugin entry point, which resolves them from REAPER's GetFunc
// at load.

#include <daw/object_resolver.h>

#include <utility>

#include <reaper_plugin.h>
#include <reaper_plugin_functions.h>

namespace sesh_ai::daw
{
	namespace
	{
		// `guidToString` documents its destination as needing 64 bytes.
		constexpr int guid_buffer_size = 64;

		// Track and marker names. The schemas cap a reported name at 512 characters;
		// the buffer is larger so that a longer name in REAPER is truncated by the
		// serialiser against the contract rather than by an undersized read here.
		constexpr int name_buffer_size = 1024;

		std::string read_track_guid(MediaTrack* track)
		{
			if (track == nullptr || GetTrackGUID == nullptr || guidToString == nullptr)
			{
				return {};
			}

			const GUID* const guid = GetTrackGUID(track);

			if (guid == nullptr)
			{
				return {};
			}

			char guid_text[guid_buffer_size] = {};
			guidToString(guid, guid_text);

			return std::string{guid_text};
		}

		// An unnamed track in REAPER has no name rather than a placeholder, and the
		// master track returns null for P_NAME. Both come back as an empty string,
		// which matches no pattern — the resolver treats an empty candidate string as
		// unmatchable rather than as a wildcard.
		std::string read_track_name(MediaTrack* track)
		{
			if (track == nullptr || GetSetMediaTrackInfo_String == nullptr)
			{
				return {};
			}

			char name[name_buffer_size] = {};

			if (!GetSetMediaTrackInfo_String(track, "P_NAME", name, false))
			{
				return {};
			}

			return std::string{name};
		}

		std::string read_marker_or_region_guid(ReaProject* project, ProjectMarker* marker_or_region)
		{
			if (marker_or_region == nullptr || GetSetRegionOrMarkerInfo_String == nullptr)
			{
				return {};
			}

			char guid_text[guid_buffer_size] = {};

			// "GUID" is read-only, so the set flag is false and the buffer is an out
			// parameter.
			if (!GetSetRegionOrMarkerInfo_String(project, marker_or_region, "GUID", guid_text, false))
			{
				return {};
			}

			return std::string{guid_text};
		}

		std::string read_marker_or_region_name(ReaProject* project, ProjectMarker* marker_or_region)
		{
			if (marker_or_region == nullptr || GetSetRegionOrMarkerInfo_String == nullptr)
			{
				return {};
			}

			char name[name_buffer_size] = {};

			if (!GetSetRegionOrMarkerInfo_String(project, marker_or_region, "P_NAME", name, false))
			{
				return {};
			}

			return std::string{name};
		}

		double read_marker_or_region_value(
			ReaProject* project,
			ProjectMarker* marker_or_region,
			const char* parameter_name)
		{
			if (marker_or_region == nullptr || GetRegionOrMarkerInfo_Value == nullptr)
			{
				return 0.0;
			}

			return GetRegionOrMarkerInfo_Value(project, marker_or_region, parameter_name);
		}
	}

	ReaperTrackListSource::ReaperTrackListSource(
		ReaProject* project,
		StructuralRoleProvider structural_role_provider)
		: project_{project}
		, structural_role_provider_{std::move(structural_role_provider)}
	{
	}

	std::vector<ResolvableTrack> ReaperTrackListSource::tracks_in_project_order() const
	{
		std::vector<ResolvableTrack> tracks;

		if (CountTracks == nullptr || GetTrack == nullptr)
		{
			return tracks;
		}

		const int track_count = CountTracks(project_);

		if (track_count > 0)
		{
			tracks.reserve(static_cast<std::size_t>(track_count) + 1);
		}

		for (int track_index = 0; track_index < track_count; ++track_index)
		{
			MediaTrack* const track = GetTrack(project_, track_index);

			if (track == nullptr)
			{
				continue;
			}

			ResolvableTrack resolvable;
			resolvable.guid = read_track_guid(track);
			resolvable.name = read_track_name(track);
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
		if (GetMasterTrack != nullptr)
		{
			MediaTrack* const master_track = GetMasterTrack(project_);

			if (master_track != nullptr)
			{
				ResolvableTrack resolvable;
				resolvable.guid = read_track_guid(master_track);
				resolvable.name = read_track_name(master_track);
				resolvable.track = master_track;
				resolvable.project_index = -1;
				resolvable.is_master_track = true;
				resolvable.structural_role = context::StructuralRole::master;

				tracks.push_back(std::move(resolvable));
			}
		}

		return tracks;
	}

	ReaperMarkerAndRegionSource::ReaperMarkerAndRegionSource(ReaProject* project)
		: project_{project}
	{
	}

	std::vector<ResolvableMarkerOrRegion>
	ReaperMarkerAndRegionSource::markers_and_regions_in_enumeration_order() const
	{
		std::vector<ResolvableMarkerOrRegion> markers_and_regions;

		if (GetNumRegionsOrMarkers == nullptr || GetRegionOrMarker == nullptr)
		{
			return markers_and_regions;
		}

		const int marker_and_region_count = GetNumRegionsOrMarkers(project_);

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
			ProjectMarker* const marker_or_region = GetRegionOrMarker(project_, enumeration_index, nullptr);

			if (marker_or_region == nullptr)
			{
				continue;
			}

			ResolvableMarkerOrRegion resolvable;
			resolvable.guid = read_marker_or_region_guid(project_, marker_or_region);
			resolvable.name = read_marker_or_region_name(project_, marker_or_region);
			resolvable.is_region =
				read_marker_or_region_value(project_, marker_or_region, "B_ISREGION") != 0.0;
			resolvable.start_seconds = read_marker_or_region_value(project_, marker_or_region, "D_STARTPOS");
			resolvable.end_seconds = read_marker_or_region_value(project_, marker_or_region, "D_ENDPOS");
			resolvable.internal_index = static_cast<int>(
				read_marker_or_region_value(project_, marker_or_region, "I_INDEX"));
			resolvable.displayed_number = static_cast<int>(
				read_marker_or_region_value(project_, marker_or_region, "I_NUMBER"));

			markers_and_regions.push_back(std::move(resolvable));
		}

		return markers_and_regions;
	}
}
