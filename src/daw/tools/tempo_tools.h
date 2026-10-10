// `list_tempo_changes` — the read over the project tempo map (task 20.1,
// requirement 25.1).
//
// One tool, in a file of its own, over a seam that already exists.
// `MarkerRegionHost::read_tempo_map` was built for `change_tempo_map` in task 10.8 and
// is reused unchanged here; `marker_region_tools.h` holds the host, the point struct,
// and the position tolerance, and this file adds no second way to read REAPER.
//
// ---------------------------------------------------------------------------
// The map REAPER keeps is not the map the schema asks for
//
// Two gaps, and both of them are why this file is more than a filter.
//
// **REAPER's tempo markers do not include the project's own tempo.**
// `CountTempoTimeSigMarkers` counts markers, and a project's initial tempo and time
// signature are not markers — which is exactly what makes `tempoChangeCount` in the
// project context snapshot the number it is. So a session at a steady 120 BPM has an
// empty tempo map, and a tool that returned REAPER's list verbatim would answer "what
// does the tempo do across this timeline" with nothing at all.
//
// `list-tempo-changes.schema.json` settles it rather than leaving it to be decided
// here, in two sentences worth quoting because they are the contract: "The initial
// tempo is included as an entry rather than left implicit, so the map reads as a
// complete picture of what the tempo does across the timeline", and "A session that
// never changes tempo returns the single entry at position zero carrying the project
// tempo and time signature."
//
// So an empty map is not an empty result. An entry at position zero is synthesised
// from `read_initial_tempo_and_time_signature` whenever REAPER's own map has no point
// there, and the agent gets a complete picture without a second call to
// `get_project_summary` to find out what the tempo it is reasoning about actually is.
//
// A windowed read can still come back empty, and that is a different thing: a window
// starting at 100 seconds over a session whose only entry is at zero genuinely
// contains no entries, and `totalInRange` of zero says so. That is a complete answer
// and not a failure.
//
// **A REAPER tempo marker may carry no time signature.** It means "keep the one
// already in force", which is why `tempo_map_point::time_signature` is an optional and
// why `change_tempo_map` writes an absence through rather than a zero. But
// `tempoChangeEntry` requires `timeSignature` on every entry, and says why: "resolved
// rather than inherited: REAPER's map lets a point carry a tempo without a signature,
// and an agent reading an entry in isolation would otherwise have to walk backwards to
// find out what bar length applies".
//
// So the walk happens here, once, over the whole map — and before any window is
// applied, because an entry inside the window can inherit its signature from a point
// outside it. Filtering first and resolving second would report 4/4 for a passage the
// producer wrote in 7/8.
//
// ---------------------------------------------------------------------------
// Requirement 23.9: cap and report
//
// `list-tempo-changes.schema.json` carries `totalInRange` and `truncated` beside an
// array capped at 100, so this tool takes the capping branch rather than the failing
// one. `marker_region_tools.h`'s header has the full comparison against
// `list_track_fx` and `list_tracks`, which fail instead because their schemas cannot
// say they were capped.
//
// ---------------------------------------------------------------------------
// A read tool, so no undo block and no marker
//
// `register_read_tool`, `tool_undo_effect::none`. Nothing here writes anything, and the
// one failure it can produce travels as `handler_action_outcomes` — which the framework
// turns into a `tool_partial_outcome` and `result_schema_path_for` routes to
// `messages/tool-result-partial.schema.json`. It is not validated against this tool's
// own output schema, which requires three fields a framework partial has nowhere to put.
//
// ---------------------------------------------------------------------------
// Shape of this file
//
// Header-only and inline, like every other tool file here, for the reason
// `marker_region_tools.h` records: the Catch2 target compiles `tests/` and not `src/`,
// so logic the suite exercises has to be visible through the header. No REAPER SDK and
// no JSON library — REAPER is reached through `MarkerRegionHost` and the payloads
// through a codec the caller supplies.

#ifndef SESH_AI_DAW_TOOLS_TEMPO_TOOLS_H
#define SESH_AI_DAW_TOOLS_TEMPO_TOOLS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <context/project_context_builder.h>
#include <daw/tool_executor.h>
#include <daw/tools/marker_region_tools.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	inline constexpr std::string_view list_tempo_changes_tool_name{"list_tempo_changes"};

	// `maxItems` on `tempoChanges`, and the `maximum` on the input schema's
	// `maximumResults`. The same pairing the two marker and region reads have.
	inline constexpr std::size_t maximum_listed_tempo_changes = 100;

	// The one `code` this tool can report beyond the inverted-window one, which arrives
	// on `check_end_after_start`'s own `action_error`.
	//
	// Reported rather than papered over with a plausible 120 BPM in 4/4: every entry's
	// time signature is resolved from the project's, and the entry at position zero is
	// composed from the project's tempo, so a session whose own tempo could not be read
	// has no honest answer to give. A fabricated one would have the agent writing tempo
	// points against a map it was told the shape of incorrectly, and
	// `change_tempo_map` moves every beat-based item in the session.
	inline constexpr std::string_view project_tempo_unavailable_code{"project_tempo_unavailable"};

	// ---------------------------------------------------------------------------
	// Result shape
	// ---------------------------------------------------------------------------

	// `tempoChangeEntry`, field for field.
	//
	// `time_signature` is a value and not an optional, which is the difference from
	// `tempo_map_point`: that struct models what REAPER stores, where an absence means
	// "keep what was in force", and this one models what the schema reports, where every
	// entry states the signature that applies to it. The conversion between them is
	// `resolved_tempo_map` below, and it is the only place the inheritance rule lives.
	struct tempo_change_entry
	{
		double position_seconds = 0.0;
		double beats_per_minute = 0.0;
		tempo_time_signature time_signature{};
		bool linear_transition = false;
	};

	inline bool operator==(const tempo_change_entry& left, const tempo_change_entry& right)
	{
		return left.position_seconds == right.position_seconds
			&& left.beats_per_minute == right.beats_per_minute
			&& left.time_signature == right.time_signature
			&& left.linear_transition == right.linear_transition;
	}

	struct list_tempo_changes_result
	{
		std::vector<tempo_change_entry> tempo_changes;

		// How many entries were in the window before the cap, counted over the resolved
		// map — so the synthesised entry at position zero is counted when it is in
		// range, because it is an entry the call would have been given.
		std::size_t total_in_range = 0;

		bool truncated = false;
	};

	// ---------------------------------------------------------------------------
	// The map the schema describes, built from the map REAPER holds
	// ---------------------------------------------------------------------------

	// Every entry's time signature resolved, and the project's own tempo present as an
	// entry at position zero when REAPER's map has no point there.
	//
	// Over the whole map, before any window. See the file header for both halves of why.
	//
	// The tempo and the time signature are bounded into the output schema's ranges using
	// `project_context_builder.h`'s own functions rather than new ones: the snapshot
	// reports a tempo against the same bounds, and a second clamp written here would be
	// a second answer that could disagree with the first about the same session.
	// `bounded_tempo` also already decided what a failed read becomes — 1 BPM, which is
	// wrong in a way somebody reports, rather than 120, which is wrong in a way nobody
	// ever finds.
	//
	// Positions are passed through as REAPER reports them, which is what the six
	// mutating tools in `marker_region_tools.h` do with a marker or region position. A
	// tempo value has a project-wide bounding answer already written down and a position
	// does not.
	inline std::vector<tempo_change_entry> resolved_tempo_map(
		const std::vector<tempo_map_point>& map_in_position_order,
		const initial_tempo_and_time_signature& project_initial)
	{
		const tempo_time_signature initial_signature{
			context::bounded_time_signature_numerator(project_initial.time_signature.numerator),
			context::nearest_representable_time_signature_denominator(
				project_initial.time_signature.denominator)};

		std::vector<tempo_change_entry> resolved;
		resolved.reserve(map_in_position_order.size() + 1);

		// `tempo_point_index_at` rather than a comparison written here, so "a point is
		// at this position" means the same thing to this read as it does to
		// `change_tempo_map`'s decision to replace rather than insert — a nanosecond of
		// tolerance, which is far below any musically meaningful distance and far above
		// what a round trip through JSON introduces.
		const bool timeline_start_is_described = tempo_point_index_at(
			tempo_point_positions_of(map_in_position_order),
			0.0).has_value();

		tempo_time_signature in_force = initial_signature;

		if (!timeline_start_is_described)
		{
			tempo_change_entry initial;
			initial.position_seconds = 0.0;
			initial.beats_per_minute = context::bounded_tempo(project_initial.beats_per_minute);
			initial.time_signature = initial_signature;

			// The project's own tempo does not ramp into the first marker — REAPER's
			// linear-transition flag belongs to a tempo marker, and there is no marker
			// here to carry one.
			initial.linear_transition = false;

			resolved.push_back(initial);
		}

		for (const tempo_map_point& point : map_in_position_order)
		{
			if (point.time_signature.has_value())
			{
				in_force = tempo_time_signature{
					context::bounded_time_signature_numerator(point.time_signature->numerator),
					context::nearest_representable_time_signature_denominator(
						point.time_signature->denominator)};
			}

			tempo_change_entry entry;
			entry.position_seconds = point.position_seconds;
			entry.beats_per_minute = context::bounded_tempo(point.beats_per_minute);
			entry.time_signature = in_force;
			entry.linear_transition = point.linear_transition;

			resolved.push_back(entry);
		}

		return resolved;
	}

	// ---------------------------------------------------------------------------
	// list_tempo_changes
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<list_tempo_changes_result> apply_list_tempo_changes(
		MarkerRegionHost& host,
		const timeline_read_window& window)
	{
		if (const std::optional<action_error> invalid_window = check_read_window(window))
		{
			// Nothing is read. A window whose end precedes its start does not name a
			// part of the timeline, and an empty map would report a session that never
			// changes tempo.
			return marker_region_detail::one_failed_action(
				the_requested_span_target,
				*invalid_window);
		}

		const std::optional<initial_tempo_and_time_signature> project_initial =
			host.read_initial_tempo_and_time_signature();

		if (!project_initial.has_value())
		{
			return marker_region_detail::one_failed_action(
				list_tempo_changes_tool_name,
				project_tempo_unavailable_code,
				"REAPER would not report the project's own tempo and time signature, so the "
				"tempo map cannot be described from its start and no entry's bar length can be "
				"resolved.");
		}

		const std::vector<tempo_change_entry> resolved =
			resolved_tempo_map(host.read_tempo_map(), *project_initial);

		std::vector<tempo_change_entry> in_window;
		in_window.reserve(resolved.size());

		for (const tempo_change_entry& entry : resolved)
		{
			// A tempo map entry is a point, like a marker and unlike a region: it is in
			// or out by its own position, and there is no span to overlap.
			if (!position_within_read_window(window, entry.position_seconds))
			{
				continue;
			}

			in_window.push_back(entry);
		}

		list_tempo_changes_result result;

		// The count before the cap, which is what `totalInRange` means.
		result.total_in_range = in_window.size();

		const std::size_t returned = std::min(
			in_window.size(),
			bounded_result_cap(window.maximum_results, maximum_listed_tempo_changes));

		result.tempo_changes.assign(
			in_window.begin(),
			in_window.begin() + static_cast<std::ptrdiff_t>(returned));

		result.truncated = result.total_in_range > result.tempo_changes.size();

		return result;
	}

	// ---------------------------------------------------------------------------
	// Registration
	//
	// `register_read_tool`, so no undo block is opened and no marker is captured. The
	// codec is supplied by the caller for the reason every tool file here gives: the
	// Catch2 target has no JSON library, and the framework's payload type is a template
	// parameter precisely so the logic can be driven without one.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	struct tempo_payload_codec
	{
		std::function<timeline_read_window(const tool_execution_context<JsonValue>&)>
			read_list_tempo_changes_request;

		std::function<JsonValue(const list_tempo_changes_result&)>
			write_list_tempo_changes_result;

		bool is_complete() const
		{
			return read_list_tempo_changes_request && write_list_tempo_changes_result;
		}
	};

	struct tempo_registration_report
	{
		tool_registration_outcome list_tempo_changes =
			tool_registration_outcome::no_handler_supplied;

		bool every_tool_registered() const
		{
			return list_tempo_changes == tool_registration_outcome::registered;
		}
	};

	// `host` and the codec's callables must outlive the registry. The host is not
	// captured by value, because a host copied into a closure would be a second view of
	// one session.
	template <typename JsonValue>
	tempo_registration_report register_tempo_tools(
		tool_handler_registry<JsonValue>& registry,
		MarkerRegionHost& host,
		tempo_payload_codec<JsonValue> codec)
	{
		tempo_registration_report report;

		if (!codec.is_complete())
		{
			// An empty `std::function` would throw `std::bad_function_call` from inside
			// the dispatch. No undo block is open for a read tool, so it costs less here
			// than it does on the mutating path — but a tool that cannot encode its own
			// result is still a tool that should not be reachable.
			return report;
		}

		report.list_tempo_changes = registry.register_read_tool(
			list_tempo_changes_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> non_mutating_handler_result<JsonValue> {
				return marker_region_detail::as_read_result<JsonValue>(
					apply_list_tempo_changes(
						host,
						codec.read_list_tempo_changes_request(context)),
					codec.write_list_tempo_changes_result);
			});

		return report;
	}
}

#endif  // SESH_AI_DAW_TOOLS_TEMPO_TOOLS_H
