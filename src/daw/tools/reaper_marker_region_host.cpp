// The one translation unit in this component that includes the REAPER SDK.
//
// Everything here is translation: a `MarkerRegionHost` call in, one or two REAPER C API
// calls out. No decisions — whether an object is a marker or a region, whether a span is
// well-formed, which tempo point index a write lands on, and how many items moved are
// all decided in marker_region_tools.h, which is what makes them testable without
// REAPER. If a change to this file needs a branch on anything other than "did REAPER
// give me a value", the logic belongs on the other side of the seam.
//
// The function pointers are resolved from the registration table's `GetFunc` rather than
// through REAPERAPI_IMPLEMENT. See reaper_marker_region_host.h for why.

#include <daw/tools/reaper_marker_region_host.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <utility>

namespace
{
	// REAPER's own type for a region or marker handle. Declared by
	// reaper_plugin_functions.h, which this file does not include — the imports it
	// declares would need storage this translation unit deliberately does not provide.
	class ProjectMarker;

	// `guidToString` documents a 64-byte destination, and `GUID` on a region or marker
	// is read through the same braced form.
	constexpr std::size_t guid_buffer_size = 64;

	// Marker and region names. 512 is the `maxLength` both output definitions put on a
	// name, and REAPER truncates to the buffer rather than overrunning it.
	constexpr std::size_t name_buffer_size = 1024;

	// `AddRegionOrMarker`'s `wantidx`: a negative value asks REAPER to assign the
	// displayed number itself. Asking for a particular one would collide with whatever
	// the producer already has on their ruler.
	constexpr int let_reaper_assign_the_number = -1;

	// `GetRegionOrMarker`'s `index`: negative means "look this up by GUID", which is
	// REAPER's own GUID lookup rather than an enumerate-and-compare.
	// `reaper_render_host.cpp` found this while resolving ADR 0018's open question about
	// addressing a region by GUID, and it is the only reason the write calls below can
	// take a GUID and reach a handle in one step.
	constexpr int look_up_by_guid = -1;

	// `SetTempoTimeSigMarker`'s `ptidx`: -1 inserts a new point rather than changing one.
	constexpr int insert_new_tempo_point = -1;

	// `SetTempoTimeSigMarker` takes either a time position or a measure and beat
	// position, and the unused pair is passed as -1. Every position in these tool
	// schemas is in seconds, so the measure and beat are always the unused pair.
	constexpr int position_given_in_seconds = -1;

	// A numerator and denominator of zero is how REAPER expresses "keep the preceding
	// time signature", which is what an absent `timeSignature` means in the input
	// schema.
	constexpr int keep_preceding_time_signature = 0;

	// `GetSet_LoopTimeRange2`'s `isLoop`: false asks for the time selection rather than
	// the loop range, which REAPER keeps as two independent spans even though the UI
	// usually links them.
	constexpr bool time_selection_rather_than_loop = false;

	// `GetSet_LoopTimeRange2`'s `allowautoseek`: false, because setting the time
	// selection is not an instruction to move the play cursor. A producer asking for the
	// last thirty seconds to be selected did not ask for playback to jump there.
	constexpr bool without_autoseek = false;
}

namespace sesh_ai::daw::tools
{
	// The functions this component needs, and only those. Resolved once at construction;
	// every one of them is required, because a marker, region, or tempo path missing any
	// of them reports a session it could not read.
	struct ReaperMarkerRegionHost::reaper_marker_region_api
	{
		int (*GetNumRegionsOrMarkers)(ReaProject*) = nullptr;
		ProjectMarker* (*GetRegionOrMarker)(ReaProject*, int, const char*) = nullptr;
		double (*GetRegionOrMarkerInfo_Value)(ReaProject*, ProjectMarker*, const char*) = nullptr;
		double (*SetRegionOrMarkerInfo_Value)(ReaProject*, ProjectMarker*, const char*, double) = nullptr;
		bool (*GetSetRegionOrMarkerInfo_String)(ReaProject*, ProjectMarker*, const char*, char*, bool) = nullptr;
		ProjectMarker* (*AddRegionOrMarker)(ReaProject*, bool, double, double, const char*, int, int) = nullptr;
		bool (*DeleteProjectMarkerByIndex)(ReaProject*, int) = nullptr;

		int (*CountTempoTimeSigMarkers)(ReaProject*) = nullptr;
		bool (*GetTempoTimeSigMarker)(ReaProject*, int, double*, int*, double*, double*, int*, int*, bool*) = nullptr;
		bool (*SetTempoTimeSigMarker)(ReaProject*, int, double, int, double, double, int, int, bool) = nullptr;
		bool (*DeleteTempoTimeSigMarker)(ReaProject*, int) = nullptr;

		// The project's own tempo and time signature, which no tempo marker holds.
		// `reaper_project_context_source.cpp` and `reaper_track_state_host.cpp` already
		// read the snapshot's `tempo` and `timeSignature` through this same function at
		// position zero, so `list_tempo_changes` and `get_project_summary` cannot
		// disagree about what the session starts at.
		void (*TimeMap_GetTimeSigAtTime)(ReaProject*, double, int*, int*, double*) = nullptr;

		int (*CountMediaItems)(ReaProject*) = nullptr;
		MediaItem* (*GetMediaItem)(ReaProject*, int) = nullptr;
		double (*GetMediaItemInfo_Value)(MediaItem*, const char*) = nullptr;

		void (*GetSet_LoopTimeRange2)(ReaProject*, bool, bool, double*, double*, bool) = nullptr;

		void (*UpdateTimeline)() = nullptr;
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

		// One marker or region, read field by field from a handle REAPER handed back.
		//
		// `I_INDEX` is the internal index, which is the handle every later call uses.
		// `I_NUMBER` is the number REAPER displays on the ruler, which is what the
		// producer sees and what REAPER renumbers as markers move — reported for that
		// reason and never used for addressing.
		marker_or_region read_entry(
			const ReaperMarkerRegionHost::reaper_marker_region_api& api,
			ReaProject* project,
			ProjectMarker* handle)
		{
			marker_or_region entry;

			char guid_buffer[guid_buffer_size] = {};

			if (api.GetSetRegionOrMarkerInfo_String(project, handle, "GUID", guid_buffer, false))
			{
				entry.object.guid = std::string{guid_buffer};
			}

			char name_buffer[name_buffer_size] = {};

			if (api.GetSetRegionOrMarkerInfo_String(project, handle, "P_NAME", name_buffer, false))
			{
				entry.object.name = std::string{name_buffer};
			}

			entry.object.is_region =
				api.GetRegionOrMarkerInfo_Value(project, handle, "B_ISREGION") != 0.0;

			entry.object.start_seconds =
				api.GetRegionOrMarkerInfo_Value(project, handle, "D_STARTPOS");

			// Equal to the start for a marker, which is REAPER's own storage and the
			// reason the end-greater-than-start check must not be applied to one.
			entry.object.end_seconds = api.GetRegionOrMarkerInfo_Value(project, handle, "D_ENDPOS");

			entry.object.internal_index =
				static_cast<int>(api.GetRegionOrMarkerInfo_Value(project, handle, "I_INDEX"));

			entry.object.displayed_number =
				static_cast<int>(api.GetRegionOrMarkerInfo_Value(project, handle, "I_NUMBER"));

			entry.color = color_from_reaper_custom_color(
				static_cast<int>(api.GetRegionOrMarkerInfo_Value(project, handle, "I_CUSTOMCOLOR")));

			return entry;
		}
	}

	ReaperMarkerRegionHost::ReaperMarkerRegionHost(
		reaper_plugin_info_t* plugin_info,
		ReaProject* project)
		: api_{std::make_unique<reaper_marker_region_api>()}
		, project_{project}
	{
		if (plugin_info == nullptr || plugin_info->GetFunc == nullptr)
		{
			unresolved_function_names_.emplace_back("GetFunc");
			return;
		}

		resolve_function(plugin_info, "GetNumRegionsOrMarkers", api_->GetNumRegionsOrMarkers, unresolved_function_names_);
		resolve_function(plugin_info, "GetRegionOrMarker", api_->GetRegionOrMarker, unresolved_function_names_);
		resolve_function(plugin_info, "GetRegionOrMarkerInfo_Value", api_->GetRegionOrMarkerInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "SetRegionOrMarkerInfo_Value", api_->SetRegionOrMarkerInfo_Value, unresolved_function_names_);
		resolve_function(plugin_info, "GetSetRegionOrMarkerInfo_String", api_->GetSetRegionOrMarkerInfo_String, unresolved_function_names_);
		resolve_function(plugin_info, "AddRegionOrMarker", api_->AddRegionOrMarker, unresolved_function_names_);
		resolve_function(plugin_info, "DeleteProjectMarkerByIndex", api_->DeleteProjectMarkerByIndex, unresolved_function_names_);

		resolve_function(plugin_info, "CountTempoTimeSigMarkers", api_->CountTempoTimeSigMarkers, unresolved_function_names_);
		resolve_function(plugin_info, "GetTempoTimeSigMarker", api_->GetTempoTimeSigMarker, unresolved_function_names_);
		resolve_function(plugin_info, "SetTempoTimeSigMarker", api_->SetTempoTimeSigMarker, unresolved_function_names_);
		resolve_function(plugin_info, "DeleteTempoTimeSigMarker", api_->DeleteTempoTimeSigMarker, unresolved_function_names_);
		resolve_function(plugin_info, "TimeMap_GetTimeSigAtTime", api_->TimeMap_GetTimeSigAtTime, unresolved_function_names_);

		resolve_function(plugin_info, "CountMediaItems", api_->CountMediaItems, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaItem", api_->GetMediaItem, unresolved_function_names_);
		resolve_function(plugin_info, "GetMediaItemInfo_Value", api_->GetMediaItemInfo_Value, unresolved_function_names_);

		resolve_function(plugin_info, "GetSet_LoopTimeRange2", api_->GetSet_LoopTimeRange2, unresolved_function_names_);

		resolve_function(plugin_info, "UpdateTimeline", api_->UpdateTimeline, unresolved_function_names_);
	}

	ReaperMarkerRegionHost::~ReaperMarkerRegionHost() = default;

	bool ReaperMarkerRegionHost::is_usable() const
	{
		return unresolved_function_names_.empty();
	}

	const std::vector<std::string>& ReaperMarkerRegionHost::unresolved_function_names() const
	{
		return unresolved_function_names_;
	}

	std::vector<marker_or_region>
	ReaperMarkerRegionHost::read_markers_and_regions_in_enumeration_order()
	{
		std::vector<marker_or_region> markers_and_regions;

		if (!is_usable())
		{
			return markers_and_regions;
		}

		// Markers and regions in one list, which is how REAPER stores them. The order is
		// REAPER's enumeration order, which is the order the producer sees them in and
		// therefore the order the Object Resolver reports candidates in.
		const int marker_and_region_count = api_->GetNumRegionsOrMarkers(project_);

		if (marker_and_region_count <= 0)
		{
			return markers_and_regions;
		}

		markers_and_regions.reserve(static_cast<std::size_t>(marker_and_region_count));

		for (int index = 0; index < marker_and_region_count; ++index)
		{
			ProjectMarker* const handle = api_->GetRegionOrMarker(project_, index, nullptr);

			if (handle == nullptr)
			{
				continue;
			}

			markers_and_regions.push_back(read_entry(*api_, project_, handle));
		}

		return markers_and_regions;
	}

	std::optional<marker_or_region> ReaperMarkerRegionHost::create_marker_or_region(
		const marker_or_region_creation& creation)
	{
		if (!is_usable())
		{
			return std::nullopt;
		}

		ProjectMarker* const created = api_->AddRegionOrMarker(
			project_,
			creation.is_region,
			creation.start_seconds,
			creation.end_seconds,
			creation.name.c_str(),
			let_reaper_assign_the_number,
			creation.color.has_value() ? reaper_custom_color_from(*creation.color) : 0);

		if (created == nullptr)
		{
			return std::nullopt;
		}

		api_->UpdateTimeline();

		// Read back rather than composed from what was asked for: the GUID and the
		// displayed number are REAPER's to assign, and the colour is what REAPER stored
		// rather than what it was handed.
		return read_entry(*api_, project_, created);
	}

	bool ReaperMarkerRegionHost::write_marker_or_region(const marker_or_region_write& write)
	{
		if (!is_usable() || write.guid.empty())
		{
			return false;
		}

		ProjectMarker* const handle =
			api_->GetRegionOrMarker(project_, look_up_by_guid, write.guid.c_str());

		if (handle == nullptr)
		{
			return false;
		}

		if (write.name.has_value())
		{
			// `GetSetRegionOrMarkerInfo_String` writes through a non-const buffer even
			// when setting, so the name is copied rather than pointed at.
			std::string writable_name = *write.name;

			if (!api_->GetSetRegionOrMarkerInfo_String(project_, handle, "P_NAME", writable_name.data(), true))
			{
				return false;
			}
		}

		// The two bounds are independent fields, so the order they are written in does
		// not change what REAPER ends up holding. Which of them is written at all was
		// decided on the other side of the seam: a region gets whichever bounds the call
		// named, and a marker gets both set to one position because they are one value
		// for a marker.
		if (write.start_seconds.has_value())
		{
			api_->SetRegionOrMarkerInfo_Value(project_, handle, "D_STARTPOS", *write.start_seconds);
		}

		if (write.end_seconds.has_value())
		{
			api_->SetRegionOrMarkerInfo_Value(project_, handle, "D_ENDPOS", *write.end_seconds);
		}

		if (write.color.has_value())
		{
			api_->SetRegionOrMarkerInfo_Value(
				project_,
				handle,
				"I_CUSTOMCOLOR",
				static_cast<double>(reaper_custom_color_from(*write.color)));
		}

		api_->UpdateTimeline();

		return true;
	}

	bool ReaperMarkerRegionHost::delete_marker_or_region(const std::string& guid)
	{
		if (!is_usable() || guid.empty())
		{
			return false;
		}

		ProjectMarker* const handle =
			api_->GetRegionOrMarker(project_, look_up_by_guid, guid.c_str());

		if (handle == nullptr)
		{
			return false;
		}

		// `DeleteProjectMarkerByIndex` takes the internal index — 0 for the first marker
		// or region, 1 for the next — rather than the displayed number, which is not
		// unique across markers and regions. `I_INDEX` is that internal index, read off
		// the handle rather than counted, so the deletion cannot address the wrong object
		// because the list changed between the resolve and the delete.
		const int internal_index =
			static_cast<int>(api_->GetRegionOrMarkerInfo_Value(project_, handle, "I_INDEX"));

		if (!api_->DeleteProjectMarkerByIndex(project_, internal_index))
		{
			return false;
		}

		api_->UpdateTimeline();

		return true;
	}

	std::vector<tempo_map_point> ReaperMarkerRegionHost::read_tempo_map()
	{
		std::vector<tempo_map_point> points;

		if (!is_usable())
		{
			return points;
		}

		const int point_count = api_->CountTempoTimeSigMarkers(project_);

		if (point_count <= 0)
		{
			return points;
		}

		points.reserve(static_cast<std::size_t>(point_count));

		for (int index = 0; index < point_count; ++index)
		{
			double position_seconds = 0.0;
			int measure_position = 0;
			double beat_position = 0.0;
			double beats_per_minute = 0.0;
			int time_signature_numerator = 0;
			int time_signature_denominator = 0;
			bool linear_transition = false;

			if (!api_->GetTempoTimeSigMarker(
					project_,
					index,
					&position_seconds,
					&measure_position,
					&beat_position,
					&beats_per_minute,
					&time_signature_numerator,
					&time_signature_denominator,
					&linear_transition))
			{
				continue;
			}

			tempo_map_point point;
			point.position_seconds = position_seconds;
			point.beats_per_minute = beats_per_minute;
			point.linear_transition = linear_transition;

			// Zeroes mean this point carries no time signature change, which is an
			// absence rather than a 0/0 signature.
			if (time_signature_numerator != keep_preceding_time_signature
				&& time_signature_denominator != keep_preceding_time_signature)
			{
				point.time_signature =
					tempo_time_signature{time_signature_numerator, time_signature_denominator};
			}

			points.push_back(point);
		}

		return points;
	}

	std::optional<initial_tempo_and_time_signature>
	ReaperMarkerRegionHost::read_initial_tempo_and_time_signature()
	{
		if (!is_usable())
		{
			// Empty rather than a plausible default. `list_tempo_changes` reports a
			// failed action naming the reason, which is the only honest answer when the
			// session's own tempo could not be read — see `tempo_tools.h`.
			return std::nullopt;
		}

		int numerator = 0;
		int denominator = 0;
		double beats_per_minute = 0.0;

		// At position zero, which is what the project's tempo and time signature mean:
		// the values at bar one. The same call and the same position
		// `reaper_project_context_source.cpp` uses for the snapshot's own fields.
		api_->TimeMap_GetTimeSigAtTime(project_, 0.0, &numerator, &denominator, &beats_per_minute);

		initial_tempo_and_time_signature initial;
		initial.beats_per_minute = beats_per_minute;
		initial.time_signature = tempo_time_signature{numerator, denominator};

		return initial;
	}

	bool ReaperMarkerRegionHost::write_tempo_point(
		std::optional<std::size_t> replacing_index,
		const tempo_map_point& point)
	{
		if (!is_usable())
		{
			return false;
		}

		const int point_index = replacing_index.has_value()
			? static_cast<int>(*replacing_index)
			: insert_new_tempo_point;

		const int numerator = point.time_signature.has_value()
			? point.time_signature->numerator
			: keep_preceding_time_signature;

		const int denominator = point.time_signature.has_value()
			? point.time_signature->denominator
			: keep_preceding_time_signature;

		if (!api_->SetTempoTimeSigMarker(
				project_,
				point_index,
				point.position_seconds,
				position_given_in_seconds,
				static_cast<double>(position_given_in_seconds),
				point.beats_per_minute,
				numerator,
				denominator,
				point.linear_transition))
		{
			return false;
		}

		api_->UpdateTimeline();

		return true;
	}

	bool ReaperMarkerRegionHost::clear_tempo_map()
	{
		if (!is_usable())
		{
			return false;
		}

		// Highest index first, so that removing one does not renumber the ones still to
		// be removed.
		for (int index = api_->CountTempoTimeSigMarkers(project_) - 1; index >= 0; --index)
		{
			if (!api_->DeleteTempoTimeSigMarker(project_, index))
			{
				return false;
			}
		}

		api_->UpdateTimeline();

		return true;
	}

	std::vector<double> ReaperMarkerRegionHost::read_item_positions()
	{
		std::vector<double> positions;

		if (!is_usable())
		{
			return positions;
		}

		const int item_count = api_->CountMediaItems(project_);

		if (item_count <= 0)
		{
			return positions;
		}

		positions.reserve(static_cast<std::size_t>(item_count));

		for (int index = 0; index < item_count; ++index)
		{
			MediaItem* const item = api_->GetMediaItem(project_, index);

			if (item == nullptr)
			{
				continue;
			}

			positions.push_back(api_->GetMediaItemInfo_Value(item, "D_POSITION"));
		}

		return positions;
	}

	bool ReaperMarkerRegionHost::write_time_selection(double start_seconds, double end_seconds)
	{
		if (!is_usable())
		{
			return false;
		}

		double writable_start = start_seconds;
		double writable_end = end_seconds;

		// `GetSet_LoopTimeRange2` returns nothing, so there is no success value to report
		// and none is invented: this returns false only when the host could not make the
		// call at all. The span was already checked for being well formed on the other
		// side of the seam, which is the failure this tool can actually have.
		api_->GetSet_LoopTimeRange2(
			project_,
			true,
			time_selection_rather_than_loop,
			&writable_start,
			&writable_end,
			without_autoseek);

		api_->UpdateTimeline();

		return true;
	}
}
