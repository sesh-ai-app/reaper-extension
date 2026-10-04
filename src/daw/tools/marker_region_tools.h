// Marker, region, tempo map, and time selection tools — `create_marker`,
// `create_region`, `update_marker_or_region`, `delete_marker_or_region`,
// `change_tempo_map`, and `set_time_selection` (task 10.8, requirements 9.1, 9.2,
// 9.6) — plus the two reads over the same list, `list_markers` and `list_regions`
// (task 20.1, requirements 25.2 and 25.3).
//
// The first six are the tools that write the timeline itself rather than anything on
// it. Five of them label or select a span and are cheap to undo; one of them,
// `change_tempo_map`, moves every beat-based item in the session and is classified
// destructive for that reason.
//
// The two reads are here rather than in a file of their own because they answer from
// the same `MarkerRegionHost` list, and because the thing most likely to go wrong in
// them — a region reported without its end, or reported as a marker — is the thing
// this file's first section is already written around. Their third sibling,
// `list_tempo_changes`, is in `tempo_tools.h`: it reads this file's seam and nothing
// else of it.
//
// ---------------------------------------------------------------------------
// Requirement 23.9 across the three read tools: cap and report
//
// `list-markers.schema.json`, `list-regions.schema.json`, and
// `list-tempo-changes.schema.json` all carry `totalInRange` and `truncated` beside an
// array capped at 100. So all three take requirement 23.9's capping branch: return
// what fits, say how many matched, and state that the list is short.
//
// That is the opposite answer from `list_track_fx` (task 10.6) and `list_tracks`
// (task 10.3), which fail a result set over their cap — and the difference is what
// each schema makes sayable rather than a change of principle. A capped read whose
// result cannot say it was capped is a silent truncation, which is exactly what 23.9
// forbids; a capped read that says so is the pagination half of the same sentence,
// and all three input schemas spell the continuation out ("pass the position of the
// last marker returned to continue reading a list that exceeded maximumResults").
//
// ---------------------------------------------------------------------------
// A marker is a point, a region is a span, and REAPER keeps both in one list
//
// This is the fact the whole file is shaped around. REAPER stores markers and regions
// in a single enumeration, distinguished by `B_ISREGION`, and reports `D_ENDPOS` equal
// to `D_STARTPOS` for a marker — a marker is not a span of length zero that REAPER
// happens to draw as a line, it is a point whose end field exists only because the two
// share a record.
//
// `update_marker_or_region` and `delete_marker_or_region` are addressed by GUID
// without being told which kind they name, so both read `B_ISREGION` and behave
// accordingly. Two consequences, and getting either wrong is a bug the type system
// cannot catch:
//
//   - The end-greater-than-start check applies to a region and never to a marker.
//     Applied to a marker it would reject every marker in existence, because a
//     marker's end equals its start by definition. `create_marker` therefore performs
//     no range check at all, and `apply_update_marker_or_region` performs one only on
//     the region branch.
//   - A marker has no end to update. A call naming `end` for a GUID that turns out to
//     be a marker is reported as a failed action rather than silently ignored: the
//     producer asked for something that does not exist on the object they named, and
//     a success would claim a change nobody made.
//
// Moving a marker writes both `D_STARTPOS` and `D_ENDPOS`, because the two are one
// value for a marker and writing only the start would leave the record describing a
// span.
//
// ---------------------------------------------------------------------------
// The inverted range is a failed action, never a refusal
//
// Requirement 9.6 as amended, and `tool_executor.h`'s `check_end_after_start` is the
// single implementation of it. It returns `std::optional<action_error>` rather than a
// `tool_refusal`, which is the amendment stated in the type system: a refusal names an
// acknowledgement the producer can confirm to unblock it, and there is no "yes, I
// really did mean the end before the start".
//
// That function is called and never reimplemented. `tool_executor_property_test.cpp`
// sweeps it over 1156 pairs of doubles — NaN, both infinities, equal pairs, both
// zeros, and `nextafter` neighbours — and asserts that not one of the three contexts
// requirement 9.6 names produces a refusal. A second comparison written here would be
// a second answer to a question that already has one, and it would be the one that
// drifts.
//
// The three contexts, and where each one is:
//
//   - a region (`apply_create_region`)
//   - a marker or region update (`apply_update_marker_or_region`, region branch)
//   - a time selection (`apply_set_time_selection`)
//
// ---------------------------------------------------------------------------
// GUID resolution is the Object Resolver's, not this file's
//
// Requirement 7.8 already puts marker and region GUID resolution in
// `object_resolver.h`: `resolve_marker_or_region_by_guid` produces the same closed set
// of three outcomes as a track selector does. This file calls it and adds nothing.
//
// What it does with the two non-resolving outcomes follows `object_resolver.h`'s own
// instruction: neither is a refusal. The refusal schema's `reason` enum has values for
// an unresolved and an ambiguous *track* selector and none for a marker or region, so
// an unresolvable GUID is reported as a failed action carrying a reason (requirement
// 9.5).
//
// On the REAPER side, `reaper_render_host.cpp` found that `GetRegionOrMarker` with a
// negative index is REAPER's own lookup by GUID — a first-class lookup rather than an
// enumerate-and-compare. That belongs where a write needs a handle, which is why
// `MarkerRegionHost`'s write and delete calls take a GUID and let the translation unit
// resolve the handle, while the read that feeds resolution enumerates the way
// `object_resolver.cpp` already does. Decisions on this side, handles on that one.
//
// ---------------------------------------------------------------------------
// None of the six can refuse, and that is a schema fact
//
// `messages/tool-result-refusal.schema.json` closes `reason` at five values:
// `foreign_undo_entries`, `output_file_collision`, `ambiguous_track_selector`,
// `unresolved_track_selector`, and `signal_cycle`. The first belongs to the undo
// tools, the second to `render`, the last to the routing tools, and the middle two are
// produced by the Tool Executor's own target resolution before any handler here runs —
// and none of these six addresses a track at all.
//
// So nothing these six handlers can discover names a reason, and all six register
// through `register_mutating_tool` with no precondition check. Every invalid call they
// can meet — an inverted span, a GUID naming nothing, an end position on a marker — is
// a failed action carrying a reason.
//
// `change_tempo_map` being destructive does not change this. Destructive means the
// producer confirms before the call is made, which happens on the server; it is not a
// precondition the extension evaluates and not something it refuses.
//
// The two read tools inherit the conclusion and not the mechanism. A read tool's body
// *may* refuse — `non_mutating_handler_result` has the alternative, because no block
// is ever opened for one — and neither of these uses it, for the same schema reason:
// the only invalid call they can meet is a window whose end precedes its start, and
// `reason` has no value for that either. It is a failed action carrying
// `invalid_time_range`, which is the same code the three writing contexts report.
//
// ---------------------------------------------------------------------------
// `change_tempo_map` reports what moved, because that is the question
//
// A tempo change moves everything positioned in beats and nothing positioned in time,
// so one call can desynchronise an arrangement that was correct a moment ago. The
// output schema is shaped around that: `pointsWritten` and `pointsReplaced` are
// separate because "add a tempo change" and "change the tempo change that was there"
// are different operations, `replacedExisting` says whether the producer lost their
// whole map, `tempoChangeCount` reports the map as it now stands, and `movedItemCount`
// is the number that says whether the arrangement survived.
//
// `movedItemCount` is measured rather than predicted: item positions are read before
// the write and after it, and the count is how many of them differ. Predicting it
// would mean reimplementing REAPER's beat-to-time conversion here in order to report a
// number REAPER can simply be asked for.
//
// Ordering is the one piece of REAPER's own bookkeeping this file models. A tempo
// marker is addressed by `ptidx`, its index in position order, so inserting a point
// shifts the indices of every later one. Rather than re-enumerating the map after each
// write, the position list is maintained here in the same order REAPER keeps it, which
// makes the index arithmetic a decision the suite can drive.
//
// ---------------------------------------------------------------------------
// `change_tempo_map` does not refuse a long map, and is the reason that half of the
// outcome-cap rule exists
//
// `tool_executor.h` states the rule: an outcome array is never cut to fit, and a handler
// that can know before it touches anything how many outcomes it owes reports one failed
// action naming the cap instead. The item tools and `set_track_state` are on that side.
// This tool is on the other side, and the difference is real rather than a disagreement.
//
// `change_tempo_map` accepts 1024 points and reports success by aggregate count —
// `pointsWritten`, `pointsReplaced`, `tempoChangeCount`, `movedItemCount` — with no
// per-point array anywhere in its output schema. A call writing all 1024 successfully is
// fully reportable, so refusing it up front to protect a cap it never reaches would
// refuse a call the contract explicitly allows. The 512-outcome cap only binds on the
// fallback path, reached once a point has already failed, and by then the writing has
// happened: there is nothing left to refuse.
//
// So this handler hands the whole outcome list to the framework and
// `build_partial_outcome` applies the backstop — the outcomes that fit, in the order they
// were supplied, and the last slot spent on a failed action naming how many are not
// named. That used to be done here, with a `tempo_outcome_report_truncated` code of this
// file's own; it moved because the cap belongs to `tool-result-partial.schema.json` and
// every handler that can overflow it should report the gap the same way.
//
// ---------------------------------------------------------------------------
// Marker and region counts belong to the snapshot
//
// Requirement 6.1 puts `markerCount` and `regionCount` in the project context
// snapshot, and creating or deleting one changes them. Nothing here pushes a snapshot:
// the Context Builder watches the project state change count on its timer and
// debounces (requirement 6.4), and a tool that also pushed would be a second
// uncoordinated source of the same message. `change_tempo_map`'s result carries
// `tempoChangeCount` because the agent needs the new value within the turn, which is
// what the schema says it is for — not as a substitute for the snapshot.
//
// ---------------------------------------------------------------------------
// Shape of this file
//
// Header-only and inline, following `tool_executor.h`, `track_structure_tools.h`, and
// `routing_tools.h`. The Catch2 target compiles what it finds under `tests/` and does
// not compile `src/`, so logic the suite exercises has to be visible through the
// header. Nothing here includes the REAPER SDK or a JSON library: REAPER is reached
// through `MarkerRegionHost` below, and the tool payloads through a codec the caller
// supplies, which is what lets all six tools be driven against plain structs.

#ifndef SESH_AI_DAW_TOOLS_MARKER_REGION_TOOLS_H
#define SESH_AI_DAW_TOOLS_MARKER_REGION_TOOLS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <daw/object_resolver.h>
#include <daw/tool_executor.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// The six tool names, spelled once. `register_mutating_tool` rejects a name that is
	// not one of the 42, so a typo here is a registration outcome rather than
	// requirement 23.6's unknown-tool error in a producer's session — but naming them
	// once is still cheaper than finding out.
	inline constexpr std::string_view create_marker_tool_name{"create_marker"};
	inline constexpr std::string_view create_region_tool_name{"create_region"};
	inline constexpr std::string_view update_marker_or_region_tool_name{"update_marker_or_region"};
	inline constexpr std::string_view delete_marker_or_region_tool_name{"delete_marker_or_region"};
	inline constexpr std::string_view change_tempo_map_tool_name{"change_tempo_map"};
	inline constexpr std::string_view set_time_selection_tool_name{"set_time_selection"};

	// The two read tools over the same list (task 20.1, requirements 25.2 and 25.3).
	// `register_read_tool` rejects a name that is not one of the 42 just as the mutating
	// seam does, and `schema_validator.h`'s binding table carries both.
	inline constexpr std::string_view list_markers_tool_name{"list_markers"};
	inline constexpr std::string_view list_regions_tool_name{"list_regions"};

	// `code` values on a failed action. Each names something the agent or the producer
	// can act on rather than restating that the tool did not work.
	//
	// The inverted-range code is not here: it is `invalid_time_range_code`, which
	// `check_end_after_start` already carries on the `action_error` it returns, and
	// passing that error's own code through is what keeps the three contexts reporting
	// one code rather than three.
	inline constexpr std::string_view unknown_marker_or_region_code{"unknown_marker_or_region"};
	inline constexpr std::string_view ambiguous_marker_or_region_code{"ambiguous_marker_or_region"};
	inline constexpr std::string_view marker_create_failed_code{"marker_create_failed"};
	inline constexpr std::string_view region_create_failed_code{"region_create_failed"};
	inline constexpr std::string_view marker_or_region_update_failed_code{
		"marker_or_region_update_failed"};
	inline constexpr std::string_view marker_or_region_delete_failed_code{
		"marker_or_region_delete_failed"};
	inline constexpr std::string_view end_position_on_a_marker_code{"end_position_on_a_marker"};
	inline constexpr std::string_view no_marker_or_region_change_requested_code{
		"no_marker_or_region_change_requested"};
	inline constexpr std::string_view no_tempo_points_supplied_code{"no_tempo_points_supplied"};
	inline constexpr std::string_view tempo_map_clear_failed_code{"tempo_map_clear_failed"};
	inline constexpr std::string_view tempo_point_write_failed_code{"tempo_point_write_failed"};

	// There is deliberately no code here for "more outcomes than one result can report".
	// That is `tool_executor.h`'s `outcome_report_truncated_code`, because the cap belongs
	// to `tool-result-partial.schema.json` rather than to tempo maps, and a second code for
	// it would have two tools describing the same gap two ways.
	inline constexpr std::string_view time_selection_write_failed_code{"time_selection_write_failed"};

	// `maxLength` on `markerDetail.name` and `regionDetail.name`. Truncated rather than
	// substituted when it is too long, and never substituted when it is empty: REAPER
	// allows an unnamed marker, `minLength` is absent from both definitions, and
	// inventing a name would report something the producer cannot find on their ruler.
	inline constexpr std::size_t maximum_marker_or_region_name_length = 512;

	// `maxItems` on `change_tempo_map`'s `points`.
	inline constexpr std::size_t maximum_tempo_points = 1024;

	// `maxItems` on `list_markers`' `markers` and `list_regions`' `regions`, which is
	// also the `maximum` on both input schemas' `maximumResults`. One value because the
	// two bounds are the same number for a reason: the cap a call may ask for is the cap
	// the result can carry.
	inline constexpr std::size_t maximum_listed_markers_or_regions = 100;

	// The `default` on `maximumResults` across all three read tools. Applied here as
	// well as on the server because the request struct can be built without one, and a
	// read that silently returned everything would be a different read from the one the
	// contract describes.
	inline constexpr std::size_t default_listed_results = 20;

	// `I_CUSTOMCOLOR` is only honoured with the high bit set — without it REAPER stores
	// the value and ignores it, which would leave the producer looking at a default
	// colour after a call that reported setting one. Spelled on this side of the seam so
	// that reading and writing a colour is testable; `reaper_track_structure_host.cpp`
	// names the same flag for tracks.
	inline constexpr int reaper_custom_color_enabled_flag = 0x1000000;

	// How close two tempo point positions have to be to count as the same position.
	//
	// A producer asking to change the tempo change at bar 33 sends a position the agent
	// derived from the map it read back, so bit-exact equality would miss by an ulp and
	// insert a second point a nanosecond from the first — which REAPER draws on top of
	// the original and nobody can then select. A nanosecond is far below any musically
	// meaningful distance and far above the rounding a round trip through JSON
	// introduces.
	inline constexpr double tempo_point_position_tolerance_seconds = 1e-9;

	// `actionTarget` for the two tools that address no object: a time selection and a
	// region that does not exist yet have no GUID to name. The same words
	// `tool_executor_property_test.cpp` uses for the swept span.
	inline constexpr std::string_view the_requested_span_target{"the requested span"};

	// ---------------------------------------------------------------------------
	// The REAPER seam
	//
	// One interface for the ten things these tools do to a session, following
	// `daw/reaper_render_host.h` and `daw/tools/reaper_track_structure_host.h`: the
	// interface is SDK-free, the production implementation is the only translation unit
	// that includes the SDK, and the suite substitutes a scripted session. The test
	// target has no SDK include path, which is what enforces the split rather than a
	// convention anybody has to remember.
	//
	// Narrow on purpose. It is not "the project" — it is the ten operations, and a tool
	// that needs an eleventh adds it here where the cost of the addition is visible.
	// `read_initial_tempo_and_time_signature` is the tenth, added by task 20.1 because
	// `list_tempo_changes` has to report something REAPER's tempo marker list does not
	// contain — and added here rather than read another way from inside a handler.
	// ---------------------------------------------------------------------------

	// One marker or region as these tools read it.
	//
	// `ResolvableMarkerOrRegion` carries the identity, the kind, the bounds, and both
	// indices already, and is reused rather than restated so that resolution here is
	// the same resolution requirement 7.8 specifies. The colour is the one field these
	// tools need and resolution does not, so it is added alongside rather than by
	// editing a struct the resolver owns.
	struct marker_or_region
	{
		ResolvableMarkerOrRegion object;

		// REAPER's native colour with the enabled flag already taken off, or nothing
		// when this marker or region carries REAPER's default colour. See
		// `color_from_reaper_custom_color`.
		std::optional<int> color;
	};

	// What to create. `end_seconds` equals `start_seconds` for a marker, which is what
	// REAPER stores for one.
	struct marker_or_region_creation
	{
		bool is_region = false;
		double start_seconds = 0.0;
		double end_seconds = 0.0;
		std::string name;

		// REAPER's native colour without the enabled flag; the translation unit sets
		// the flag. Absent leaves REAPER's default.
		std::optional<int> color;
	};

	// What to change about one that exists. Every field absent means "leave it alone",
	// which is what distinguishes a rename from a move.
	struct marker_or_region_write
	{
		std::string guid;
		std::optional<std::string> name;
		std::optional<double> start_seconds;

		// Written for a region, and written equal to the start for a marker, because
		// `D_ENDPOS` and `D_STARTPOS` are one value for a marker.
		std::optional<double> end_seconds;

		std::optional<int> color;
	};

	// One tempo map point, as REAPER's tempo marker record holds it.
	struct tempo_time_signature
	{
		int numerator = 0;
		int denominator = 0;
	};

	inline bool operator==(const tempo_time_signature& left, const tempo_time_signature& right)
	{
		return left.numerator == right.numerator && left.denominator == right.denominator;
	}

	struct tempo_map_point
	{
		double position_seconds = 0.0;
		double beats_per_minute = 0.0;

		// Absent keeps the preceding time signature, which REAPER expresses as a
		// numerator and denominator of zero. Held as an absence here so that "keep what
		// was there" and "change to 0/0" are not the same value.
		std::optional<tempo_time_signature> time_signature;

		// Ramp to the next point rather than jumping at this one.
		bool linear_transition = false;
	};

	// The tempo and time signature in force at the start of the timeline.
	//
	// Not a tempo map point, and that is the whole reason this exists.
	// `CountTempoTimeSigMarkers` counts *markers*, and a project's own tempo and time
	// signature are not markers — so a session that never changes tempo has an empty
	// map, and `read_tempo_map` reports nothing at all about what the tempo is.
	//
	// `list_tempo_changes` needs it for two things its output schema requires and
	// REAPER's map cannot supply: the entry at position zero that
	// `list-tempo-changes.schema.json` says a session which never changes tempo
	// returns, and the baseline for resolving a time signature on a tempo marker that
	// carries none. See `tempo_tools.h`.
	//
	// This is the seam widening task 20.1 needed. The alternative was a second way to
	// read REAPER from inside a handler, which is the thing `MarkerRegionHost` exists to
	// prevent.
	struct initial_tempo_and_time_signature
	{
		double beats_per_minute = 0.0;
		tempo_time_signature time_signature{};
	};

	// What these six tools do to a session.
	class MarkerRegionHost
	{
	public:
		virtual ~MarkerRegionHost() = default;

		// REAPER's own single list, in its enumeration order, markers and regions
		// together. The order resolution reports candidates in, so it is the order the
		// producer sees on the ruler.
		virtual std::vector<marker_or_region> read_markers_and_regions_in_enumeration_order() = 0;

		// Creates one and returns it as REAPER now holds it, including the GUID REAPER
		// minted and the number it displays. Empty means the creation did not happen —
		// the caller reports a failed action rather than composing a result for an
		// object that is not there.
		virtual std::optional<marker_or_region> create_marker_or_region(
			const marker_or_region_creation& creation) = 0;

		// Applies every field the write names and leaves the rest. False when REAPER
		// would not accept it, or when the GUID names nothing.
		virtual bool write_marker_or_region(const marker_or_region_write& write) = 0;

		// Removes it. False when the GUID names nothing.
		virtual bool delete_marker_or_region(const std::string& guid) = 0;

		// The tempo map in position order, which is the order REAPER indexes it in.
		virtual std::vector<tempo_map_point> read_tempo_map() = 0;

		// The tempo and time signature the project starts at, which no tempo marker
		// holds. Empty when REAPER could not be asked.
		//
		// Pure virtual rather than defaulted, because the only default available is a
		// plausible 120 BPM in 4/4 — and an implementation that forgot to answer would
		// then report a tempo that reads like a real session rather than one anybody
		// notices. `project_context_builder.h`'s `bounded_tempo` records the same
		// reasoning for the snapshot's tempo.
		virtual std::optional<initial_tempo_and_time_signature>
			read_initial_tempo_and_time_signature() = 0;

		// Writes one point. `replacing_index` names an existing point to overwrite;
		// absent inserts a new one.
		virtual bool write_tempo_point(
			std::optional<std::size_t> replacing_index,
			const tempo_map_point& point) = 0;

		// Removes every point, leaving the project's initial tempo and time signature.
		// False when any removal failed, in which case the map is whatever REAPER left.
		virtual bool clear_tempo_map() = 0;

		// Every media item's position in seconds, in REAPER's item order. Read before
		// and after a tempo map write so that `movedItemCount` is measured rather than
		// predicted.
		virtual std::vector<double> read_item_positions() = 0;

		// Writes the time selection — REAPER's loop time range with `isLoop` false,
		// which is a span it keeps independently of the loop points.
		virtual bool write_time_selection(double start_seconds, double end_seconds) = 0;
	};

	// ---------------------------------------------------------------------------
	// Colour, which crosses the seam as REAPER stores it
	// ---------------------------------------------------------------------------

	// REAPER's `I_CUSTOMCOLOR` as an optional colour.
	//
	// Nothing when the enabled flag is clear, because REAPER stores a colour it was
	// told to ignore and reporting that value would show the producer a colour they
	// cannot see.
	inline std::optional<int> color_from_reaper_custom_color(int reaper_custom_color)
	{
		if ((reaper_custom_color & reaper_custom_color_enabled_flag) == 0)
		{
			return std::nullopt;
		}

		return reaper_custom_color & ~reaper_custom_color_enabled_flag;
	}

	// The value to write into `I_CUSTOMCOLOR` for a colour.
	inline int reaper_custom_color_from(int color)
	{
		return color | reaper_custom_color_enabled_flag;
	}

	// ---------------------------------------------------------------------------
	// Result shapes
	//
	// One struct per tool, mirroring that tool's vendored output schema field for
	// field. Plain structs rather than JSON, so the whole of this file is compilable
	// and testable without a JSON library; the codec at the bottom is where they
	// become payloads.
	// ---------------------------------------------------------------------------

	// `markerDetail`. The index is REAPER's displayed number, which is what the
	// producer sees on the ruler and what REAPER renumbers as markers are added,
	// removed, and moved — reported for that reason and never used for addressing.
	struct marker_detail
	{
		std::string guid;
		int index = 0;
		double position_seconds = 0.0;
		std::string name;
		std::optional<int> color;
	};

	// `regionDetail`. The start and end are both always present, because a region's
	// span is what turns "render the chorus" into a time range.
	struct region_detail
	{
		std::string guid;
		int index = 0;
		double start_seconds = 0.0;
		double end_seconds = 0.0;
		std::string name;
		std::optional<int> color;
	};

	inline bool operator==(const marker_detail& left, const marker_detail& right)
	{
		return left.guid == right.guid
			&& left.index == right.index
			&& left.position_seconds == right.position_seconds
			&& left.name == right.name
			&& left.color == right.color;
	}

	inline bool operator==(const region_detail& left, const region_detail& right)
	{
		return left.guid == right.guid
			&& left.index == right.index
			&& left.start_seconds == right.start_seconds
			&& left.end_seconds == right.end_seconds
			&& left.name == right.name
			&& left.color == right.color;
	}

	// The `oneOf` on `update_marker_or_region`'s and `delete_marker_or_region`'s output
	// schemas, as a type.
	//
	// A variant rather than a kind field beside two optionals, for the reason
	// `object_resolver.h` gives for its own three outcomes: a struct with a kind saying
	// "region" and a populated marker is a shape nobody designed and one forgotten
	// assignment reaches. Naming an alternative is the only way to construct one, and
	// the discriminator the schema requires is then derived rather than set — see
	// `describe_marker_or_region_kind`.
	using marker_or_region_detail = std::variant<marker_detail, region_detail>;

	// The `kind` constant on each branch.
	inline constexpr std::string_view marker_detail_kind{"marker"};
	inline constexpr std::string_view region_detail_kind{"region"};

	inline std::string_view describe_marker_or_region_kind(const marker_or_region_detail& detail)
	{
		return std::holds_alternative<region_detail>(detail) ? region_detail_kind : marker_detail_kind;
	}

	inline const std::string& marker_or_region_detail_guid(const marker_or_region_detail& detail)
	{
		if (const region_detail* const region = std::get_if<region_detail>(&detail))
		{
			return region->guid;
		}

		return std::get<marker_detail>(detail).guid;
	}

	struct create_marker_result
	{
		marker_detail marker;
	};

	struct create_region_result
	{
		region_detail region;
	};

	struct update_marker_or_region_result
	{
		marker_or_region_detail updated;
	};

	struct delete_marker_or_region_result
	{
		// Read before the deletion, because it cannot be read after — and because the
		// producer needs to hear "removed the region Chorus" rather than a GUID.
		marker_or_region_detail deleted;
	};

	struct change_tempo_map_result
	{
		int points_written = 0;
		int points_replaced = 0;
		bool replaced_existing = false;

		// How many tempo or time signature changes the map holds now, which is the
		// count of tempo markers — the project's initial tempo and time signature are
		// not markers, which is what makes this the same count the snapshot reports.
		int tempo_change_count = 0;

		// How many media items moved as a consequence. Measured, not predicted.
		int moved_item_count = 0;
	};

	struct set_time_selection_result
	{
		double start_seconds = 0.0;
		double end_seconds = 0.0;

		// `end - start`. Derived and reported because the duration is what the producer
		// asked for when they said "the last thirty seconds".
		double length_seconds = 0.0;
	};

	// `list_markers`' result. `totalInRange` and `truncated` are requirement 23.9's
	// capping branch — see "Requirement 23.9 across the three read tools" in the file
	// header.
	struct list_markers_result
	{
		std::vector<marker_detail> markers;

		// How many markers were in the window before the cap. Equal to `markers.size()`
		// when nothing was capped.
		std::size_t total_in_range = 0;

		bool truncated = false;
	};

	struct list_regions_result
	{
		std::vector<region_detail> regions;
		std::size_t total_in_range = 0;
		bool truncated = false;
	};

	// ---------------------------------------------------------------------------
	// Requests
	//
	// What each tool was asked for, once the codec has read the already-validated
	// input. Plain structs for the same reason the results are.
	// ---------------------------------------------------------------------------

	struct create_marker_request
	{
		double position_seconds = 0.0;
		std::string name;
		std::optional<int> color;
	};

	struct create_region_request
	{
		double start_seconds = 0.0;
		double end_seconds = 0.0;
		std::string name;
		std::optional<int> color;
	};

	struct update_marker_or_region_request
	{
		std::string guid;

		// Every one of these absent is a call that asks for no change, which is
		// reported as a failed action rather than as a success that changed nothing.
		std::optional<std::string> name;
		std::optional<double> position_seconds;

		// Meaningful for a region only. Present for a GUID that names a marker is a
		// failed action.
		std::optional<double> end_seconds;

		std::optional<int> color;

		bool asks_for_any_change() const
		{
			return name.has_value()
				|| position_seconds.has_value()
				|| end_seconds.has_value()
				|| color.has_value();
		}
	};

	struct delete_marker_or_region_request
	{
		std::string guid;
	};

	struct change_tempo_map_request
	{
		// In the order the call supplied them, which is the order they are written and
		// the order any per-point outcomes are reported in.
		std::vector<tempo_map_point> points;

		// Clear the whole map before writing. A different conversation from merging:
		// the producer loses every tempo change they had.
		bool replace_existing = false;
	};

	struct set_time_selection_request
	{
		double start_seconds = 0.0;
		double end_seconds = 0.0;
	};

	// What all three read tools are asked for.
	//
	// One struct rather than three identical ones, because `list-markers.schema.json`,
	// `list-regions.schema.json`, and `list-tempo-changes.schema.json` have the same
	// three optional inputs with the same meanings and the same bounds. Three copies
	// would only differ by name, and the one that drifted would be the one nobody
	// noticed.
	//
	// What differs between the three is how the window is *applied*, not what it holds:
	// a marker and a tempo map entry are points and are in or out by position, while a
	// region is a span and is in when it overlaps — "a region overlapping the window is
	// returned in full, not clipped to it", which is `list-regions.schema.json`'s own
	// wording. That difference lives in the handlers.
	struct timeline_read_window
	{
		// Absent means "from the beginning of the project", which is what omitting it
		// means on all three input schemas. Held as an absence rather than defaulted to
		// zero so that a window starting at the timeline's start and no window at all
		// stay distinguishable in a log line.
		std::optional<double> start_position_seconds;

		// Absent means "to the end of the project".
		std::optional<double> end_position_seconds;

		std::size_t maximum_results = default_listed_results;
	};

	// Requirement 9.6's check, applied to a read window rather than to anything written.
	//
	// All three input schemas say the extension checks this, since JSON Schema cannot
	// express it. `check_end_after_start` is `tool_executor.h`'s own, so these three
	// tools report the same code the three writing contexts do rather than a fourth
	// answer to one question.
	//
	// Only when the call supplied both bounds. One bound alone is an open-ended window —
	// the input schemas' "omit to read to the end of the project" — and there is no pair
	// to compare. Comparing against a defaulted zero would reject every call that named
	// only a start.
	inline std::optional<action_error> check_read_window(const timeline_read_window& window)
	{
		if (!window.start_position_seconds.has_value() || !window.end_position_seconds.has_value())
		{
			return std::nullopt;
		}

		return check_end_after_start(
			*window.start_position_seconds,
			*window.end_position_seconds);
	}

	// Whether a point on the timeline is inside the window. Inclusive at both ends,
	// which is what "at or after" and "at or before" say on the input schemas.
	inline bool position_within_read_window(
		const timeline_read_window& window,
		double position_seconds)
	{
		if (window.start_position_seconds.has_value()
			&& position_seconds < *window.start_position_seconds)
		{
			return false;
		}

		if (window.end_position_seconds.has_value()
			&& position_seconds > *window.end_position_seconds)
		{
			return false;
		}

		return true;
	}

	// Whether a span overlaps the window.
	//
	// `list-regions.schema.json`'s filter, stated from the region's side rather than the
	// window's: in when it ends at or after the window's start and starts at or before
	// the window's end. A region straddling either edge is in, and is reported with the
	// bounds REAPER holds rather than the bounds of the window — clipping it would hand
	// the agent a span the producer cannot see on their ruler, and "render the chorus"
	// would render part of it.
	inline bool span_overlaps_read_window(
		const timeline_read_window& window,
		double start_seconds,
		double end_seconds)
	{
		if (window.start_position_seconds.has_value()
			&& end_seconds < *window.start_position_seconds)
		{
			return false;
		}

		if (window.end_position_seconds.has_value()
			&& start_seconds > *window.end_position_seconds)
		{
			return false;
		}

		return true;
	}

	// How many entries this call can actually be answered with.
	//
	// The smaller of what the call asked for and what the output schema's `maxItems` can
	// carry. Both numbers are 100 on a validated call, so the second bound is
	// unreachable from the wire and is here because the request struct can hold
	// anything — `maximum_results` is a `std::size_t`, and a result array longer than
	// `maxItems` is a payload the extension's own outbound validation refuses
	// (requirement 4.1), which a producer experiences as the tool not working.
	//
	// A zero is raised to one rather than to the schema default. The input schemas'
	// `minimum` is 1, so a validated call cannot ask for none; raising it to one keeps
	// the function total without reading more than was asked for, and `truncated` then
	// says the rest is there.
	inline std::size_t bounded_result_cap(std::size_t maximum_results, std::size_t schema_cap)
	{
		if (maximum_results == 0)
		{
			return 1;
		}

		return std::min(maximum_results, schema_cap);
	}

	// What one of these handlers produced: the tool's own result, or per-action
	// outcomes. `handler_action_outcomes` is the framework's own type, so this maps onto
	// `mutating_handler_result` without a conversion that could lose a reason. No
	// refusal alternative, because none of the six has one — see the file header.
	template <typename ResultType>
	using marker_region_outcome = std::variant<ResultType, handler_action_outcomes>;

	namespace marker_region_detail
	{
		// Nested so that the helper names here cannot collide with the ones
		// `track_structure_tools.h` and `routing_tools.h` already spell at namespace
		// scope, which is the arrangement `routing_detail` established.

		inline handler_action_outcomes one_failed_action(
			std::string_view target,
			std::string_view failure_code,
			std::string_view failure_message)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, failure_code, failure_message));

			return outcomes;
		}

		// The failed action an `action_error` describes, reported against a target.
		//
		// The error's own code and message travel unchanged. `check_end_after_start`
		// already chose both, and rewriting them here would give the three contexts
		// requirement 9.6 names three different reports of one rule.
		inline handler_action_outcomes one_failed_action(
			std::string_view target,
			const action_error& failure)
		{
			return one_failed_action(target, failure.code(), failure.message());
		}

		// Truncated to the schema's bound, never substituted. An unnamed marker is a
		// marker REAPER allows, and both name definitions omit `minLength`.
		inline std::string within_name_bounds(std::string_view name)
		{
			std::string bounded{name};

			if (bounded.size() > maximum_marker_or_region_name_length)
			{
				bounded.resize(maximum_marker_or_region_name_length);
			}

			return bounded;
		}

		// The identity and bounds the Object Resolver reads, lifted out of what the host
		// reported, so that resolution runs against exactly the struct requirement 7.8
		// specifies.
		inline std::vector<ResolvableMarkerOrRegion> resolvable_views_of(
			const std::vector<marker_or_region>& markers_and_regions)
		{
			std::vector<ResolvableMarkerOrRegion> views;
			views.reserve(markers_and_regions.size());

			for (const marker_or_region& entry : markers_and_regions)
			{
				views.push_back(entry.object);
			}

			return views;
		}

		// The entry the resolver settled on, with its colour — which the resolver does
		// not carry. Matched on the internal index rather than the GUID because the
		// index is what REAPER hands back as the handle and comparing it is not a
		// case-folded string comparison.
		inline std::optional<marker_or_region> entry_matching(
			const std::vector<marker_or_region>& markers_and_regions,
			const ResolvableMarkerOrRegion& resolved)
		{
			for (const marker_or_region& entry : markers_and_regions)
			{
				if (entry.object.internal_index == resolved.internal_index)
				{
					return entry;
				}
			}

			return std::nullopt;
		}
	}

	// Requirement 7.8's three outcomes, narrowed to what these two tools need: the
	// entry, or the failed action that says why there is none.
	using resolved_marker_region_entry = std::variant<marker_or_region, handler_action_outcomes>;

	// Resolves a GUID against REAPER's list through the Object Resolver.
	//
	// Both non-resolving outcomes come back as failed actions carrying a reason rather
	// than as refusals, which is `object_resolver.h`'s own instruction for marker and
	// region GUIDs: the refusal schema's `reason` names an unresolved and an ambiguous
	// *track* selector and has no value for either of these.
	inline resolved_marker_region_entry resolve_marker_region_entry(
		const std::vector<marker_or_region>& markers_and_regions,
		std::string_view guid)
	{
		if (guid.empty())
		{
			return marker_region_detail::one_failed_action(
				guid,
				unknown_marker_or_region_code,
				"No marker or region GUID was supplied, so there is nothing to address.");
		}

		const MarkerOrRegionResolution resolution = resolve_marker_or_region_by_guid(
			marker_region_detail::resolvable_views_of(markers_and_regions),
			guid);

		if (const AmbiguousMarkerOrRegionSelector* const ambiguous =
				std::get_if<AmbiguousMarkerOrRegionSelector>(&resolution))
		{
			return marker_region_detail::one_failed_action(
				guid,
				ambiguous_marker_or_region_code,
				"More than one marker or region in this session carries that GUID ("
					+ std::to_string(ambiguous->candidates.size())
					+ " of them), so it does not name one thing to change.");
		}

		const ResolvedMarkerOrRegion* const resolved =
			std::get_if<ResolvedMarkerOrRegion>(&resolution);

		if (resolved == nullptr)
		{
			return marker_region_detail::one_failed_action(
				guid,
				unknown_marker_or_region_code,
				"No marker or region in this session carries that GUID. The markers and regions "
				"may have changed since the agent last read them.");
		}

		const std::optional<marker_or_region> entry =
			marker_region_detail::entry_matching(markers_and_regions, resolved->object);

		if (!entry.has_value())
		{
			// The resolver matched a view of a list this function built from the same
			// list, so reaching here means the two disagree about what is in it.
			return marker_region_detail::one_failed_action(
				guid,
				unknown_marker_or_region_code,
				"The marker or region resolved but could not be read back, so nothing was changed.");
		}

		return *entry;
	}

	// ---------------------------------------------------------------------------
	// Details from what REAPER holds
	// ---------------------------------------------------------------------------

	inline marker_detail marker_detail_of(const marker_or_region& entry)
	{
		marker_detail detail;
		detail.guid = entry.object.guid;
		detail.index = entry.object.displayed_number;
		detail.position_seconds = entry.object.start_seconds;
		detail.name = marker_region_detail::within_name_bounds(entry.object.name);
		detail.color = entry.color;

		return detail;
	}

	inline region_detail region_detail_of(const marker_or_region& entry)
	{
		region_detail detail;
		detail.guid = entry.object.guid;
		detail.index = entry.object.displayed_number;
		detail.start_seconds = entry.object.start_seconds;
		detail.end_seconds = entry.object.end_seconds;
		detail.name = marker_region_detail::within_name_bounds(entry.object.name);
		detail.color = entry.color;

		return detail;
	}

	// Which branch of the `oneOf` an entry belongs on, decided by `B_ISREGION` and
	// nothing else.
	inline marker_or_region_detail detail_of(const marker_or_region& entry)
	{
		if (entry.object.is_region)
		{
			return region_detail_of(entry);
		}

		return marker_detail_of(entry);
	}

	// ---------------------------------------------------------------------------
	// create_marker
	//
	// No range check. A marker is a point, REAPER stores its end equal to its start,
	// and `check_end_after_start` applied here would reject every marker ever created.
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<create_marker_result> apply_create_marker(
		MarkerRegionHost& host,
		const create_marker_request& request)
	{
		marker_or_region_creation creation;
		creation.is_region = false;
		creation.start_seconds = request.position_seconds;
		creation.end_seconds = request.position_seconds;
		creation.name = request.name;
		creation.color = request.color;

		const std::optional<marker_or_region> created = host.create_marker_or_region(creation);

		if (!created.has_value())
		{
			return marker_region_detail::one_failed_action(
				request.name,
				marker_create_failed_code,
				"REAPER did not create the marker, so nothing was added to the timeline.");
		}

		create_marker_result result;
		result.marker = marker_detail_of(*created);

		return result;
	}

	// ---------------------------------------------------------------------------
	// create_region
	//
	// The first of requirement 9.6's three contexts.
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<create_region_result> apply_create_region(
		MarkerRegionHost& host,
		const create_region_request& request)
	{
		// Before anything is written. A zero-length region is not a region, and the
		// check is `tool_executor.h`'s so that the answer here is the answer property 28
		// swept.
		const std::optional<action_error> invalid_range =
			check_end_after_start(request.start_seconds, request.end_seconds);

		if (invalid_range.has_value())
		{
			return marker_region_detail::one_failed_action(request.name, *invalid_range);
		}

		marker_or_region_creation creation;
		creation.is_region = true;
		creation.start_seconds = request.start_seconds;
		creation.end_seconds = request.end_seconds;
		creation.name = request.name;
		creation.color = request.color;

		const std::optional<marker_or_region> created = host.create_marker_or_region(creation);

		if (!created.has_value())
		{
			return marker_region_detail::one_failed_action(
				request.name,
				region_create_failed_code,
				"REAPER did not create the region, so nothing was added to the timeline.");
		}

		create_region_result result;
		result.region = region_detail_of(*created);

		return result;
	}

	// ---------------------------------------------------------------------------
	// update_marker_or_region
	//
	// The tool that has to read `B_ISREGION` before it can decide anything, and the
	// second of requirement 9.6's three contexts.
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<update_marker_or_region_result> apply_update_marker_or_region(
		MarkerRegionHost& host,
		const update_marker_or_region_request& request)
	{
		const std::vector<marker_or_region> before =
			host.read_markers_and_regions_in_enumeration_order();

		resolved_marker_region_entry resolution = resolve_marker_region_entry(before, request.guid);

		if (handler_action_outcomes* const unresolved =
				std::get_if<handler_action_outcomes>(&resolution))
		{
			return std::move(*unresolved);
		}

		const marker_or_region entry = std::get<marker_or_region>(resolution);

		if (!request.asks_for_any_change())
		{
			// Reported rather than treated as a satisfied no-op: a success carrying the
			// object unchanged would tell the producer their rename landed.
			return marker_region_detail::one_failed_action(
				entry.object.guid,
				no_marker_or_region_change_requested_code,
				"The call named a marker or region but asked for no change to it, so there was "
				"nothing to apply.");
		}

		if (!entry.object.is_region && request.end_seconds.has_value())
		{
			// A marker has no end. REAPER stores `D_ENDPOS` equal to `D_STARTPOS` for
			// one, so writing an end would either be ignored or turn the marker into
			// something the producer did not ask for.
			return marker_region_detail::one_failed_action(
				entry.object.guid,
				end_position_on_a_marker_code,
				"That GUID names a marker, which is a point on the timeline and has no end "
				"position. Create a region if a span is wanted.");
		}

		marker_or_region_write write;
		write.guid = entry.object.guid;
		write.color = request.color;

		if (request.name.has_value())
		{
			write.name = marker_region_detail::within_name_bounds(*request.name);
		}

		if (entry.object.is_region)
		{
			const double start_seconds = request.position_seconds.value_or(entry.object.start_seconds);
			const double end_seconds = request.end_seconds.value_or(entry.object.end_seconds);

			// Only when the call touches a bound. A rename that moves nothing is not an
			// occasion to reject a span the producer did not supply and may not know
			// about — a region REAPER already holds with an end at its start was not
			// written by this extension, and refusing to rename it would leave the
			// producer unable to fix it by any route this tool offers.
			if (request.position_seconds.has_value() || request.end_seconds.has_value())
			{
				const std::optional<action_error> invalid_range =
					check_end_after_start(start_seconds, end_seconds);

				if (invalid_range.has_value())
				{
					return marker_region_detail::one_failed_action(
						entry.object.guid,
						*invalid_range);
				}
			}

			if (request.position_seconds.has_value())
			{
				write.start_seconds = start_seconds;
			}

			if (request.end_seconds.has_value())
			{
				write.end_seconds = end_seconds;
			}
		}
		else if (request.position_seconds.has_value())
		{
			// Both fields, because they are one value for a marker. Writing only the
			// start would leave a record describing a span from the new position to the
			// old one.
			write.start_seconds = *request.position_seconds;
			write.end_seconds = *request.position_seconds;
		}

		if (!host.write_marker_or_region(write))
		{
			return marker_region_detail::one_failed_action(
				entry.object.guid,
				marker_or_region_update_failed_code,
				entry.object.is_region
					? "REAPER did not accept the change to the region, so it is unchanged."
					: "REAPER did not accept the change to the marker, so it is unchanged.");
		}

		// Read back rather than composed from what was asked for. Moving a marker can
		// change the number REAPER displays for it — markers are numbered in position
		// order — so a composed result would report an index that is already wrong.
		const std::vector<marker_or_region> after =
			host.read_markers_and_regions_in_enumeration_order();

		update_marker_or_region_result result;

		for (const marker_or_region& candidate : after)
		{
			if (guids_match(candidate.object.guid, entry.object.guid))
			{
				result.updated = detail_of(candidate);

				return result;
			}
		}

		// REAPER accepted the write and then did not report the object. The change
		// landed, so a failure here would be a lie; what is reported instead is the
		// object as the write left it, with the index from before — the one field that
		// can be stale, and the one the schema already says must not be used for
		// addressing.
		marker_or_region written = entry;
		written.object.name = write.name.value_or(entry.object.name);
		written.object.start_seconds = write.start_seconds.value_or(entry.object.start_seconds);
		written.object.end_seconds = write.end_seconds.value_or(entry.object.end_seconds);
		written.color = request.color.has_value() ? request.color : entry.color;

		result.updated = detail_of(written);

		return result;
	}

	// ---------------------------------------------------------------------------
	// delete_marker_or_region
	//
	// No range check anywhere. The object is being removed, so its span is not being
	// validated — and applying the check would make a marker impossible to delete.
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<delete_marker_or_region_result> apply_delete_marker_or_region(
		MarkerRegionHost& host,
		const delete_marker_or_region_request& request)
	{
		const std::vector<marker_or_region> before =
			host.read_markers_and_regions_in_enumeration_order();

		resolved_marker_region_entry resolution = resolve_marker_region_entry(before, request.guid);

		if (handler_action_outcomes* const unresolved =
				std::get_if<handler_action_outcomes>(&resolution))
		{
			return std::move(*unresolved);
		}

		const marker_or_region entry = std::get<marker_or_region>(resolution);

		// Read before the deletion, because it cannot be read after it.
		const marker_or_region_detail deleted = detail_of(entry);

		if (!host.delete_marker_or_region(entry.object.guid))
		{
			return marker_region_detail::one_failed_action(
				entry.object.guid,
				marker_or_region_delete_failed_code,
				entry.object.is_region
					? "REAPER did not delete the region, so it is still on the timeline."
					: "REAPER did not delete the marker, so it is still on the timeline.");
		}

		delete_marker_or_region_result result;
		result.deleted = deleted;

		return result;
	}

	// ---------------------------------------------------------------------------
	// change_tempo_map
	// ---------------------------------------------------------------------------

	// The position of every point in the map, in the order REAPER indexes them.
	inline std::vector<double> tempo_point_positions_of(const std::vector<tempo_map_point>& points)
	{
		std::vector<double> positions;
		positions.reserve(points.size());

		for (const tempo_map_point& point : points)
		{
			positions.push_back(point.position_seconds);
		}

		return positions;
	}

	// The index of the point already at this position, or nothing when there is none.
	//
	// A tolerance rather than equality — see `tempo_point_position_tolerance_seconds`.
	// The first match wins, and a well-formed map has at most one: REAPER will not hold
	// two tempo markers at one position.
	inline std::optional<std::size_t> tempo_point_index_at(
		const std::vector<double>& positions_in_order,
		double position_seconds)
	{
		for (std::size_t index = 0; index < positions_in_order.size(); ++index)
		{
			const double separation = positions_in_order[index] - position_seconds;
			const double distance = separation < 0.0 ? -separation : separation;

			if (distance <= tempo_point_position_tolerance_seconds)
			{
				return index;
			}
		}

		return std::nullopt;
	}

	// Where a new point lands in position order, which is the index REAPER will give it
	// and therefore the index every later point shifts by.
	inline std::size_t tempo_point_insertion_index(
		const std::vector<double>& positions_in_order,
		double position_seconds)
	{
		std::size_t index = 0;

		while (index < positions_in_order.size() && positions_in_order[index] < position_seconds)
		{
			++index;
		}

		return index;
	}

	// How many items moved, by comparing positions read either side of the write.
	//
	// Compared by position in REAPER's item order, which is stable across a tempo map
	// change: a tempo change moves items, it does not add or remove them. A differing
	// count means something other than this call changed the session, and the overlap
	// is what can honestly be compared.
	inline int moved_item_count_between(
		const std::vector<double>& positions_before,
		const std::vector<double>& positions_after)
	{
		const std::size_t comparable = std::min(positions_before.size(), positions_after.size());
		int moved = 0;

		for (std::size_t index = 0; index < comparable; ++index)
		{
			if (positions_before[index] != positions_after[index])
			{
				++moved;
			}
		}

		return moved;
	}

	inline marker_region_outcome<change_tempo_map_result> apply_change_tempo_map(
		MarkerRegionHost& host,
		const change_tempo_map_request& request)
	{
		if (request.points.empty())
		{
			// `minItems` is 1 on the input schema, so a validated call cannot arrive
			// empty — but the request struct can hold it, and clearing a map by writing
			// nothing into it is not something this tool offers.
			return marker_region_detail::one_failed_action(
				change_tempo_map_tool_name,
				no_tempo_points_supplied_code,
				"The call supplied no tempo points, so there was nothing to write into the "
				"tempo map.");
		}

		// Read before anything is written, so that what moved is measured rather than
		// predicted from REAPER's beat-to-time conversion.
		const std::vector<double> item_positions_before = host.read_item_positions();

		std::vector<double> positions_in_order =
			tempo_point_positions_of(host.read_tempo_map());

		if (request.replace_existing && !positions_in_order.empty())
		{
			if (!host.clear_tempo_map())
			{
				// Nothing was written, and the map is whatever the partial clear left.
				// Reported as one failure rather than per point, because no point was
				// reached.
				return marker_region_detail::one_failed_action(
					change_tempo_map_tool_name,
					tempo_map_clear_failed_code,
					"REAPER would not clear the existing tempo map, so no new tempo points were "
					"written and the map may not be what it was.");
			}

			positions_in_order.clear();
		}

		std::vector<action_outcome> outcomes;
		outcomes.reserve(request.points.size());

		int points_written = 0;
		int points_replaced = 0;
		bool any_point_failed = false;

		for (const tempo_map_point& point : request.points)
		{
			const std::optional<std::size_t> existing_index =
				tempo_point_index_at(positions_in_order, point.position_seconds);

			const std::string target = "tempo point at " + std::to_string(point.position_seconds)
				+ " seconds";

			if (!host.write_tempo_point(existing_index, point))
			{
				any_point_failed = true;

				outcomes.push_back(failed_action(
					target,
					tempo_point_write_failed_code,
					"REAPER would not write this tempo point, so the map does not hold it."));

				continue;
			}

			if (existing_index.has_value())
			{
				++points_replaced;
				positions_in_order[*existing_index] = point.position_seconds;
			}
			else
			{
				++points_written;

				// Mirrors what REAPER just did to its own ordering, so the next point's
				// index is computed against the map as it now is rather than as it was
				// when this call began.
				const std::size_t insertion_index =
					tempo_point_insertion_index(positions_in_order, point.position_seconds);

				positions_in_order.insert(
					positions_in_order.begin() + static_cast<std::ptrdiff_t>(insertion_index),
					point.position_seconds);
			}

			outcomes.push_back(succeeded_action(target));
		}

		const int tempo_change_count = static_cast<int>(host.read_tempo_map().size());
		const std::vector<double> item_positions_after = host.read_item_positions();

		if (any_point_failed)
		{
			// Some points landed and some did not. Per-action outcomes are the only
			// shape that can carry both, and the ones that landed stay landed
			// (requirement 9.4). The aggregate counts are lost in this shape, which is
			// what the output schema's single success object costs — the outcomes name
			// every point, which is the part the agent needs in order to retry.
			// `points` allows 1024 and `actions` allows 512, so a long map with a failure
			// cannot be reported point by point. Handed over whole rather than cut here:
			// `tool_executor.h`'s outcome-cap rule puts the backstop in
			// `build_partial_outcome`, which keeps the outcomes that fit in the order they
			// were supplied and spends the last slot naming how many are missing. This tool
			// is the case that rule's second half exists for — refusing 1024 points up front
			// would refuse a call the contract allows and the success path reports by count.
			handler_action_outcomes reported;
			reported.actions = std::move(outcomes);

			return reported;
		}

		change_tempo_map_result result;
		result.points_written = points_written;
		result.points_replaced = points_replaced;
		result.replaced_existing = request.replace_existing;
		result.tempo_change_count = tempo_change_count;
		result.moved_item_count =
			moved_item_count_between(item_positions_before, item_positions_after);

		return result;
	}

	// ---------------------------------------------------------------------------
	// set_time_selection
	//
	// The third of requirement 9.6's three contexts, and the one property 28 drives
	// most directly.
	// ---------------------------------------------------------------------------

	inline marker_region_outcome<set_time_selection_result> apply_set_time_selection(
		MarkerRegionHost& host,
		const set_time_selection_request& request)
	{
		const std::optional<action_error> invalid_range =
			check_end_after_start(request.start_seconds, request.end_seconds);

		if (invalid_range.has_value())
		{
			// Nothing is written, which is the half of requirement 9.6 that says the
			// operation does not proceed.
			return marker_region_detail::one_failed_action(
				the_requested_span_target,
				*invalid_range);
		}

		if (!host.write_time_selection(request.start_seconds, request.end_seconds))
		{
			return marker_region_detail::one_failed_action(
				the_requested_span_target,
				time_selection_write_failed_code,
				"REAPER did not accept the time selection, so the selected span is unchanged.");
		}

		set_time_selection_result result;
		result.start_seconds = request.start_seconds;
		result.end_seconds = request.end_seconds;
		result.length_seconds = request.end_seconds - request.start_seconds;

		return result;
	}

	// ---------------------------------------------------------------------------
	// list_markers and list_regions
	//
	// Two tools over one REAPER list, and `B_ISREGION` is the only thing that decides
	// which of them an entry belongs to. Both schemas ask for their array in position
	// order, so the enumeration order REAPER reports is sorted rather than assumed —
	// stably, so that two objects at one position keep the order resolution reports
	// candidates in, which is the order the producer sees on the ruler.
	//
	// Neither reads REAPER a second way.
	// `read_markers_and_regions_in_enumeration_order` already carries the GUID, the
	// kind, both bounds, the displayed number, and the colour, and
	// `marker_detail_of`/`region_detail_of` already shape each kind correctly — which is
	// the part worth not rewriting, because the difference between them is a region's
	// end and that is the field a mistake here would lose.
	// ---------------------------------------------------------------------------

	// Position order, which is what both output schemas ask their array to be in.
	inline bool marker_or_region_precedes_by_start(
		const marker_or_region& left,
		const marker_or_region& right)
	{
		return left.object.start_seconds < right.object.start_seconds;
	}

	// A read tool, so no undo block and no marker — `tool_undo_effect::none`.
	//
	// Markers only. A region returned here would be a named span handed to the agent as
	// a point: its end, the field that made it a region, is not in `markerDetail` at
	// all, so the agent would read the chorus as an instant and `set_time_selection`
	// would select nothing.
	inline marker_region_outcome<list_markers_result> apply_list_markers(
		MarkerRegionHost& host,
		const timeline_read_window& window)
	{
		if (const std::optional<action_error> invalid_window = check_read_window(window))
		{
			// Nothing is read. A window whose end precedes its start does not name a
			// part of the timeline, and an empty list would report a session with no
			// markers in it.
			return marker_region_detail::one_failed_action(
				the_requested_span_target,
				*invalid_window);
		}

		std::vector<marker_or_region> in_window;

		for (const marker_or_region& entry : host.read_markers_and_regions_in_enumeration_order())
		{
			if (entry.object.is_region)
			{
				continue;
			}

			if (!position_within_read_window(window, entry.object.start_seconds))
			{
				continue;
			}

			in_window.push_back(entry);
		}

		std::stable_sort(
			in_window.begin(),
			in_window.end(),
			marker_or_region_precedes_by_start);

		list_markers_result result;

		// The count before the cap, which is what `totalInRange` means.
		result.total_in_range = in_window.size();

		const std::size_t returned = std::min(
			in_window.size(),
			bounded_result_cap(window.maximum_results, maximum_listed_markers_or_regions));

		result.markers.reserve(returned);

		for (std::size_t position = 0; position < returned; ++position)
		{
			result.markers.push_back(marker_detail_of(in_window[position]));
		}

		result.truncated = result.total_in_range > result.markers.size();

		return result;
	}

	// Regions only, and reported with the span REAPER holds.
	//
	// `D_ENDPOS` equals `D_STARTPOS` for a marker, so a marker leaking into this list
	// arrives as a region of zero length — which `region_detail`'s `end` is the one
	// field that would show, and which nothing downstream can do anything useful with.
	inline marker_region_outcome<list_regions_result> apply_list_regions(
		MarkerRegionHost& host,
		const timeline_read_window& window)
	{
		if (const std::optional<action_error> invalid_window = check_read_window(window))
		{
			return marker_region_detail::one_failed_action(
				the_requested_span_target,
				*invalid_window);
		}

		std::vector<marker_or_region> in_window;

		for (const marker_or_region& entry : host.read_markers_and_regions_in_enumeration_order())
		{
			if (!entry.object.is_region)
			{
				continue;
			}

			if (!span_overlaps_read_window(
					window,
					entry.object.start_seconds,
					entry.object.end_seconds))
			{
				continue;
			}

			in_window.push_back(entry);
		}

		std::stable_sort(
			in_window.begin(),
			in_window.end(),
			marker_or_region_precedes_by_start);

		list_regions_result result;
		result.total_in_range = in_window.size();

		const std::size_t returned = std::min(
			in_window.size(),
			bounded_result_cap(window.maximum_results, maximum_listed_markers_or_regions));

		result.regions.reserve(returned);

		for (std::size_t position = 0; position < returned; ++position)
		{
			result.regions.push_back(region_detail_of(in_window[position]));
		}

		result.truncated = result.total_in_range > result.regions.size();

		return result;
	}

	// ---------------------------------------------------------------------------
	// Registration
	// ---------------------------------------------------------------------------

	// How each tool's input and result cross the JSON boundary.
	//
	// Supplied by the caller rather than implemented here, which is the decision
	// `tool_executor.h` and `track_structure_tools.h` both make for the same reason:
	// this file is compiled by the Catch2 target, which has no JSON library, and the
	// framework's payload type is a template parameter precisely so the logic can be
	// driven without one. Reading a tool input is the Envelope Codec's job, and what
	// arrives has already been validated against the authoritative schema by the MCP
	// Tool Server (requirement 4.4).
	//
	// Each reader takes the whole execution context rather than the payload alone, for
	// consistency with `track_structure_payload_codec` — none of these six tools
	// addresses a track, but a reader that can see the call can name it in a log line.
	template <typename JsonValue>
	struct marker_region_payload_codec
	{
		std::function<create_marker_request(const tool_execution_context<JsonValue>&)>
			read_create_marker_request;
		std::function<create_region_request(const tool_execution_context<JsonValue>&)>
			read_create_region_request;
		std::function<update_marker_or_region_request(const tool_execution_context<JsonValue>&)>
			read_update_marker_or_region_request;
		std::function<delete_marker_or_region_request(const tool_execution_context<JsonValue>&)>
			read_delete_marker_or_region_request;
		std::function<change_tempo_map_request(const tool_execution_context<JsonValue>&)>
			read_change_tempo_map_request;
		std::function<set_time_selection_request(const tool_execution_context<JsonValue>&)>
			read_set_time_selection_request;

		std::function<JsonValue(const create_marker_result&)> write_create_marker_result;
		std::function<JsonValue(const create_region_result&)> write_create_region_result;
		std::function<JsonValue(const update_marker_or_region_result&)>
			write_update_marker_or_region_result;
		std::function<JsonValue(const delete_marker_or_region_result&)>
			write_delete_marker_or_region_result;
		std::function<JsonValue(const change_tempo_map_result&)> write_change_tempo_map_result;
		std::function<JsonValue(const set_time_selection_result&)> write_set_time_selection_result;

		bool is_complete() const
		{
			return read_create_marker_request
				&& read_create_region_request
				&& read_update_marker_or_region_request
				&& read_delete_marker_or_region_request
				&& read_change_tempo_map_request
				&& read_set_time_selection_request
				&& write_create_marker_result
				&& write_create_region_result
				&& write_update_marker_or_region_result
				&& write_delete_marker_or_region_result
				&& write_change_tempo_map_result
				&& write_set_time_selection_result;
		}
	};

	namespace marker_region_detail
	{
		// Turns one handler's outcome into the framework's mutating result.
		//
		// The framework attaches the undo report; this only chooses between the tool's
		// own fields and the per-action outcomes. A handler cannot produce a refusal,
		// and `mutating_handler_result` has no alternative for one.
		template <typename JsonValue, typename ResultType, typename ResultWriter>
		mutating_handler_result<JsonValue> as_mutating_result(
			marker_region_outcome<ResultType> outcome,
			const ResultWriter& write_result)
		{
			if (handler_action_outcomes* const outcomes =
					std::get_if<handler_action_outcomes>(&outcome))
			{
				return std::move(*outcomes);
			}

			handler_success<JsonValue> success;
			success.fields = write_result(std::get<ResultType>(outcome));

			return success;
		}

		// The same choice for a read tool.
		//
		// `non_mutating_handler_result` has a third alternative for a refusal, and
		// neither read tool uses it: the refusal schema's `reason` is closed at five
		// values and an inverted read window is not one of them, which is the same
		// reasoning the six mutating tools follow. So the outcomes branch produces a
		// `tool_partial_outcome`, and `result_schema_path_for` routes that to
		// `messages/tool-result-partial.schema.json` rather than to the tool's own
		// output schema — which is what lets a read tool fail at all. `list_markers`'
		// own schema requires `markers`, `totalInRange`, and `truncated` and is closed,
		// so a framework partial validated against it would be refused on every one of
		// the three.
		template <typename JsonValue, typename ResultType, typename ResultWriter>
		non_mutating_handler_result<JsonValue> as_read_result(
			marker_region_outcome<ResultType> outcome,
			const ResultWriter& write_result)
		{
			if (handler_action_outcomes* const outcomes =
					std::get_if<handler_action_outcomes>(&outcome))
			{
				return std::move(*outcomes);
			}

			handler_success<JsonValue> success;
			success.fields = write_result(std::get<ResultType>(outcome));

			return success;
		}
	}

	// One outcome per tool, in registration order, so a caller can report exactly which
	// of the six did not register rather than that something did not.
	struct marker_region_registration_report
	{
		tool_registration_outcome create_marker = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome create_region = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome update_marker_or_region =
			tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome delete_marker_or_region =
			tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome change_tempo_map = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome set_time_selection = tool_registration_outcome::no_handler_supplied;

		bool every_tool_registered() const
		{
			return create_marker == tool_registration_outcome::registered
				&& create_region == tool_registration_outcome::registered
				&& update_marker_or_region == tool_registration_outcome::registered
				&& delete_marker_or_region == tool_registration_outcome::registered
				&& change_tempo_map == tool_registration_outcome::registered
				&& set_time_selection == tool_registration_outcome::registered;
		}
	};

	// Registers all six through the framework's mutating seam.
	//
	// `register_mutating_tool` with no precondition check, for all six. The seam's
	// precondition is the only place a mutating tool may refuse, and none of these six
	// has anything to refuse with: the refusal schema's `reason` is closed at five
	// values and not one of them describes anything these tools decide. Every invalid
	// call they can meet is a failed action carrying a reason instead.
	//
	// `host` and the codec's callables must outlive the registry, which outlives one
	// dispatch. The extension constructs both at startup and the suite on the stack;
	// the host is not captured by value, because a host copied into six closures would
	// be six views of one session.
	template <typename JsonValue>
	marker_region_registration_report register_marker_region_tools(
		tool_handler_registry<JsonValue>& registry,
		MarkerRegionHost& host,
		marker_region_payload_codec<JsonValue> codec)
	{
		marker_region_registration_report report;

		if (!codec.is_complete())
		{
			return report;
		}

		report.create_marker = registry.register_mutating_tool(
			create_marker_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_create_marker(host, codec.read_create_marker_request(context)),
					codec.write_create_marker_result);
			});

		report.create_region = registry.register_mutating_tool(
			create_region_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_create_region(host, codec.read_create_region_request(context)),
					codec.write_create_region_result);
			});

		report.update_marker_or_region = registry.register_mutating_tool(
			update_marker_or_region_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_update_marker_or_region(
						host,
						codec.read_update_marker_or_region_request(context)),
					codec.write_update_marker_or_region_result);
			});

		report.delete_marker_or_region = registry.register_mutating_tool(
			delete_marker_or_region_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_delete_marker_or_region(
						host,
						codec.read_delete_marker_or_region_request(context)),
					codec.write_delete_marker_or_region_result);
			});

		report.change_tempo_map = registry.register_mutating_tool(
			change_tempo_map_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_change_tempo_map(host, codec.read_change_tempo_map_request(context)),
					codec.write_change_tempo_map_result);
			});

		report.set_time_selection = registry.register_mutating_tool(
			set_time_selection_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> mutating_handler_result<JsonValue> {
				return marker_region_detail::as_mutating_result<JsonValue>(
					apply_set_time_selection(host, codec.read_set_time_selection_request(context)),
					codec.write_set_time_selection_result);
			});

		return report;
	}

	// ---------------------------------------------------------------------------
	// The two read tools register separately, through the other seam
	//
	// A second codec and a second registration call rather than two more fields on the
	// six-tool ones, because the seam is different and the difference is the point:
	// `register_read_tool` opens no undo block and captures no marker
	// (`tool_undo_effect::none`), and a read tool's body *may* refuse where a mutating
	// body structurally cannot. Folding them into `register_marker_region_tools` would
	// put two tools with no undo effect inside a function whose whole contract is
	// "all six open a block".
	//
	// An extension registering markers and regions calls both.
	// ---------------------------------------------------------------------------

	template <typename JsonValue>
	struct marker_region_read_payload_codec
	{
		std::function<timeline_read_window(const tool_execution_context<JsonValue>&)>
			read_list_markers_request;
		std::function<timeline_read_window(const tool_execution_context<JsonValue>&)>
			read_list_regions_request;

		std::function<JsonValue(const list_markers_result&)> write_list_markers_result;
		std::function<JsonValue(const list_regions_result&)> write_list_regions_result;

		bool is_complete() const
		{
			return read_list_markers_request
				&& read_list_regions_request
				&& write_list_markers_result
				&& write_list_regions_result;
		}
	};

	struct marker_region_read_registration_report
	{
		tool_registration_outcome list_markers = tool_registration_outcome::no_handler_supplied;
		tool_registration_outcome list_regions = tool_registration_outcome::no_handler_supplied;

		bool every_tool_registered() const
		{
			return list_markers == tool_registration_outcome::registered
				&& list_regions == tool_registration_outcome::registered;
		}
	};

	template <typename JsonValue>
	marker_region_read_registration_report register_marker_region_read_tools(
		tool_handler_registry<JsonValue>& registry,
		MarkerRegionHost& host,
		marker_region_read_payload_codec<JsonValue> codec)
	{
		marker_region_read_registration_report report;

		if (!codec.is_complete())
		{
			return report;
		}

		report.list_markers = registry.register_read_tool(
			list_markers_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> non_mutating_handler_result<JsonValue> {
				return marker_region_detail::as_read_result<JsonValue>(
					apply_list_markers(host, codec.read_list_markers_request(context)),
					codec.write_list_markers_result);
			});

		report.list_regions = registry.register_read_tool(
			list_regions_tool_name,
			[&host, codec](const tool_execution_context<JsonValue>& context)
				-> non_mutating_handler_result<JsonValue> {
				return marker_region_detail::as_read_result<JsonValue>(
					apply_list_regions(host, codec.read_list_regions_request(context)),
					codec.write_list_regions_result);
			});

		return report;
	}
}

#endif  // SESH_AI_DAW_TOOLS_MARKER_REGION_TOOLS_H
