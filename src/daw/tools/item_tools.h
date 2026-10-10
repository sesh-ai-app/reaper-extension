// The eight item tools: set_item_properties, move_items, move_all_items,
// split_items, delete_items, list_selected_items, import_audio_file, and
// import_midi_file (task 10.7, requirements 9.1 and 9.2).
//
// Header-only and inline, for the reason `tool_executor.h` gives for itself: the
// Catch2 target compiles what it finds under `tests/` and does not compile `src/`, so
// logic the suite exercises has to be visible through the header.
// `reaper_item_host.h` and its one translation unit are the other side of the seam,
// and they contain no decisions at all.
//
// "Item" throughout is REAPER's term for a media clip on a track, and "take" is a
// version of the content inside one. Neither is called a clip, a region, or a block:
// a region is a named span of the timeline and is the marker tools' business.
//
// ---------------------------------------------------------------------------
// No item tool registers a precondition
//
// `messages/tool-result-refusal.schema.json` closes `reason` at five values —
// `foreign_undo_entries`, `output_file_collision`, `ambiguous_track_selector`,
// `unresolved_track_selector`, `signal_cycle` — and not one of them describes anything
// an item tool decides. An item GUID that is not in the project, a split position that
// falls outside every item named, a file path that does not exist, a file REAPER
// cannot decode: each is an invalid call, and there is no acknowledgement the producer
// could set on a retry that would make it valid. Requirement 9.6 settles the same
// question for an inverted time range in the same direction, and the reasoning
// transfers.
//
// The `confirmedTrackSelection` field on `move_items`, `move_all_items` and the two
// imports looks at first like a precondition these handlers own, and it is not. It is
// the acknowledgement field named by the *two track selector* refusals, which
// `object_resolver.h` builds and `tool_executor_of::execute` returns from
// `resolve_call_targets` before a handler is reached. So there is nothing left for a
// precondition check here to return but empty, and every mutating tool below registers
// without one.
//
// ---------------------------------------------------------------------------
// Items are addressed by GUID, and the GUID is read against one snapshot
//
// Unlike FX, which the contract addresses positionally, every item tool addresses
// items by GUID: `itemGuids`, `itemPropertyChange.itemGuid`. That is what makes these
// handlers safe under an edit that reorders items, and it is why nothing here holds an
// index between two REAPER calls.
//
// Each handler reads the project's items once before it writes and once after, rather
// than per item. Reading per item would let the producer's own edit land between two
// reads of one call, which is how a multi-item tool ends up reporting one project's
// items and having changed another's. The read after is what the results are built
// from, because what an item's bounds *became* is not derivable from the request: a
// trim is clamped by the underlying audio, and REAPER mints the second half of a split
// itself.
//
// ---------------------------------------------------------------------------
// Both move tools take an offset, and neither takes a position
//
// Worth stating because getting it backwards moves a producer's session by the wrong
// amount and looks like it worked. `move-items.schema.json` and
// `move-all-items.schema.json` both carry `offsetSeconds`, described as how far to
// move along the timeline; neither carries a destination position. The one absolute
// position in this file's inputs is `itemPropertyChange.position` on
// `set_item_properties`, which sets where an item starts.
//
// The two differ in what they address, not in how they move. `move_items` names items
// by GUID and may also carry `toTrack`; `move_all_items` names *tracks* — or nothing,
// meaning the whole project — and shifts every item on them.
//
// Items are never moved before zero, so a negative offset is clamped at zero, and both
// output schemas require `clampedItemCount` to say how many items that happened to. A
// clamp breaks the relative spacing of a group, which is a musical consequence rather
// than a rounding detail, so it is reported rather than absorbed.
//
// A move can also cross a track boundary, which makes "where is it now" two questions.
// `move_items` reports both: the destination track in `toTrack`, and each item's
// `trackGuid` and `position` read back from the project afterwards.
//
// ---------------------------------------------------------------------------
// A split produces a second item, so the result names both halves
//
// `split-items.schema.json` requires, per split, the `originalItemGuid` and *both*
// `leftItem` and `rightItem`, and says why: REAPER mints a new GUID for the second
// half, so after the split "that item" is ambiguous and the agent cannot address
// either piece without being told which is which. The left half keeps the original's
// GUID.
//
// A split position that does not fall inside an item is **not** a failure here, and
// that is the schema's decision rather than this file's: `skippedItemGuids` exists for
// exactly that case, described as reporting items the position did not fall inside
// "rather than silently ignored". So a miss is reported as skipped, and the agent is
// told to read a list equal to everything the call named as a wrong position rather
// than a completed split. An item GUID that is not in the project at all is a
// different thing and does fail.
//
// ---------------------------------------------------------------------------
// The three ways an import can fail are three different reasons
//
// A path that does not exist, a path that exists and cannot be read, and a file REAPER
// will not decode are distinct, and the producer's next step differs for each: find
// the file, fix its permissions, convert it. So they carry three codes.
//
// The first two are filesystem facts and are asked of the host rather than answered
// here, so the suite can stage all three. What is deliberately *not* done is reading
// the file's contents to decide whether REAPER can import it: REAPER decides what it
// can decode, and this file reports what it said. `probe_file` looks at existence and
// readability and nothing else.
//
// ---------------------------------------------------------------------------
// delete_items is reversible, and the undo report says so honestly
//
// `infra/lib/tool-classification.js` puts `delete_items` at `medium` risk — the band
// documented as "fully reversible through REAPER's undo history" — not at `high`,
// which is reserved for operations that discard data REAPER's undo cannot restore.
// `apply_fx_destructively` is the only tool in that band. So `delete_items` registers
// as an ordinary mutating tool, one undo block, and `undoPositionBefore` is reported
// because reverting really does bring the items back. Nothing here claims more than
// that, and nothing claims less.
//
// What the deletion cannot do is read an item's track after the item is gone, which is
// why `affectedTracks` and `deletedLengthSeconds` are computed from the snapshot taken
// before the first delete. `delete-items.schema.json` says the same thing about its own
// field.
//
// ---------------------------------------------------------------------------
// Requirement 23.9 for list_selected_items: cap and report
//
// `list-selected-items.schema.json` carries both `totalInRange` and `truncated`, so a
// capped read is visible to the agent rather than a silent truncation it reasons over
// as if it were the whole set. That is the opposite answer from `list_track_fx`, whose
// output schema has neither field and which therefore fails rather than truncating
// (task 10.6) — the difference is what the schema makes sayable, not a change of
// principle. Here the cap is the schema's own `maxItems` of 4096, `totalInRange` is the
// count before it, and `truncated` is stated rather than left to be inferred.
//
// ---------------------------------------------------------------------------
// Which framework shape each tool returns, and why they are not all the same
//
// `set_item_properties` is one of the two action array tools whose *own* output schema
// carries `actions` — `schema_validator.h` names both — and it requires `actions` and
// `items` together. So its payload is always the tool's own, carrying the per-entry
// outcomes inside it, and the handler always returns `handler_success`. Returning the
// framework's partial shape instead would drop `items`, which the schema requires.
//
// That has one consequence the other seven do not have. Returning `handler_success`
// would otherwise get the undo report attached unconditionally, and
// `set-item-properties.schema.json` is explicit that the *absence* of
// `undoPositionBefore` is what tells the producer nothing changed — so an array in which
// every entry failed would report a marker for a call that moved nothing, and a marker
// sitting above the agent's earlier work shortens the range "revert all" walks. The
// framework cannot see this: requirement 4.4 keeps the payload opaque, so it cannot count
// the outcomes inside. The handler says so instead, by setting
// `handler_success::nothing_was_applied` from `any_change_landed()`.
//
// `move_items`, `split_items`, `delete_items` and the two imports have no `actions` in
// their own output schemas, so they follow `apply_fx_destructively`: the tool's own
// fields when everything landed, and the framework's per-action outcomes when
// something did not.
//
// `move_all_items` reports neither per item nor per action, and that is its schema's
// decision: it "counts rather than enumerates" because a project-wide call would
// return thousands of GUIDs the agent has no use for. So a failure there is one failed
// action naming how many items moved and how many did not, rather than an outcome per
// item.
//
// One consequence worth naming: `tool-result-partial.schema.json` caps `actions` at
// 512 while every item input here allows up to 4096 GUIDs — and
// `set-item-properties.schema.json` caps its *own* `actions` at 512 against an input
// allowing 4096 changes, so that one is not even a framework bound. A truncated outcome
// array is what requirement 23.9 and requirement 9.5 forbid: actions that happened with
// nothing said about them.
//
// `tool_executor.h` states the rule these tools follow. The count is knowable here before
// anything is touched — it is the length of an input array — so the four tools that report
// per item report one failed action naming the cap and change nothing, leaving the agent
// to retry with fewer targets against an untouched session. That is the refusing half of
// the rule; `change_tempo_map` is on the other half, for a reason `marker_region_tools.h`
// gives. `move_all_items` is unaffected either way, since it reports one outcome however
// many items it shifts.

#ifndef SESH_AI_DAW_TOOLS_ITEM_TOOLS_H
#define SESH_AI_DAW_TOOLS_ITEM_TOOLS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <daw/tool_executor.h>
#include <daw/unit_conversion.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	//
	// Spelled once each, because every one is a string the server or the model reads,
	// where a typo is a protocol bug rather than a compile error.
	// ---------------------------------------------------------------------------

	inline constexpr std::string_view set_item_properties_tool_name{"set_item_properties"};
	inline constexpr std::string_view move_items_tool_name{"move_items"};
	inline constexpr std::string_view move_all_items_tool_name{"move_all_items"};
	inline constexpr std::string_view split_items_tool_name{"split_items"};
	inline constexpr std::string_view delete_items_tool_name{"delete_items"};
	inline constexpr std::string_view list_selected_items_tool_name{"list_selected_items"};
	inline constexpr std::string_view import_audio_file_tool_name{"import_audio_file"};
	inline constexpr std::string_view import_midi_file_tool_name{"import_midi_file"};

	// `code` values on a failed action. Every item-specific problem lands on one of
	// these, because none of them is a refusal — see the file header.
	inline constexpr std::string_view item_host_unavailable_code{"item_host_unavailable"};
	inline constexpr std::string_view item_input_unreadable_code{"item_input_unreadable"};
	inline constexpr std::string_view item_not_found_code{"item_not_found"};
	inline constexpr std::string_view item_track_not_resolved_code{"item_track_not_resolved"};
	inline constexpr std::string_view item_names_no_change_code{"item_names_no_change"};
	inline constexpr std::string_view item_value_not_finite_code{"item_value_not_finite"};
	inline constexpr std::string_view item_property_write_failed_code{"item_property_write_failed"};
	inline constexpr std::string_view item_track_move_failed_code{"item_track_move_failed"};
	inline constexpr std::string_view item_position_write_failed_code{"item_position_write_failed"};
	inline constexpr std::string_view item_split_failed_code{"item_split_failed"};
	inline constexpr std::string_view item_delete_failed_code{"item_delete_failed"};
	inline constexpr std::string_view item_read_back_failed_code{"item_read_back_failed"};
	inline constexpr std::string_view item_too_many_items_code{"item_too_many_items"};
	inline constexpr std::string_view item_no_items_in_scope_code{"item_no_items_in_scope"};
	inline constexpr std::string_view item_duplicate_guid_code{"item_duplicate_guid"};
	inline constexpr std::string_view item_file_not_found_code{"item_file_not_found"};
	inline constexpr std::string_view item_file_not_readable_code{"item_file_not_readable"};
	inline constexpr std::string_view item_file_not_importable_code{"item_file_not_importable"};
	inline constexpr std::string_view item_import_unreportable_code{"item_import_unreportable"};

	// Bounds from the schemas, so a payload these handlers fill in cannot fail the
	// outbound validation requirement 4.1 asks for.

	// `list-selected-items.schema.json` caps `items` at 4096, and carries
	// `totalInRange` and `truncated` to say when the cap bit.
	inline constexpr std::size_t maximum_reported_items = 4096;

	// The most items a tool reporting an outcome per item can address. Not the input
	// schemas' 4096: `tool-result-partial.schema.json` caps `actions` at 512, which is
	// also `tool_executor.h`'s `maximum_action_outcomes`, and an outcome array
	// truncated to fit would be actions that happened with nothing said about them.
	inline constexpr std::size_t maximum_items_per_reported_call = maximum_action_outcomes;

	// `move-all-items.schema.json`'s `tracks` is `trackReferences`, capped at 512 —
	// the same number `trackSelectors` caps the call's input at.
	inline constexpr std::size_t maximum_moved_track_references = 512;

	// `delete-items.schema.json` caps `affectedTracks` at 4096.
	inline constexpr std::size_t maximum_affected_track_references = 4096;

	// Field lengths the schemas impose, used when a name or path REAPER gave us is
	// longer than the contract allows.
	inline constexpr std::size_t maximum_item_name_length = 512;
	inline constexpr std::size_t maximum_item_track_name_length = 512;
	inline constexpr std::size_t maximum_item_file_path_length = 4096;

	// ---------------------------------------------------------------------------
	// What the seam moves across it
	// ---------------------------------------------------------------------------

	// One media item, as the item tools need to see it.
	//
	// Plain data, deliberately: the decision logic never touches a REAPER handle, which
	// is what lets the suite drive all eight handlers against a synthetic project.
	//
	// `take_name` is the active take's name, which is what REAPER shows on the item.
	// Empty is allowed and is not an error — an unnamed take has no name rather than a
	// placeholder — and both `itemPlacement.name` and `selectedItem.takeName` carry a
	// `maxLength` with no `minLength`, so an empty one is omitted rather than invented.
	struct item_state
	{
		std::string guid;
		std::string track_guid;
		std::string track_name;

		// Zero-based index of the item's track in the project's track list. Carried so
		// results can be put in track order without a second read, and so
		// `list_selected_items` can order items the way its schema asks for.
		int track_project_index = 0;

		double position_seconds = 0.0;
		double length_seconds = 0.0;
		std::string take_name;
		bool muted = false;

		// Whether the producer has this item selected, and whether they have its track
		// selected. Two flags rather than one, because `list_selected_items`' three
		// scopes ask two different questions and an item inside a selected track is not
		// necessarily one the producer highlighted.
		bool selected = false;
		bool track_selected = false;

		double end_seconds() const { return position_seconds + length_seconds; }
	};

	// The producer's current time selection. Absent when they have none.
	struct item_time_range
	{
		double start_seconds = 0.0;
		double end_seconds = 0.0;

		bool is_empty() const { return !(end_seconds > start_seconds); }
	};

	// What one split produced. The left half keeps the original's GUID; the right half
	// is the one REAPER minted, and is the piece the producer usually meant to act on
	// next.
	struct item_split_halves
	{
		std::string left_item_guid;
		std::string right_item_guid;
	};

	// What the filesystem says about a path, which is not what REAPER says about the
	// file. Three values because the producer's next step differs for each.
	enum class item_file_access
	{
		readable,
		missing,
		unreadable
	};

	// What REAPER made of a file it imported.
	//
	// The zeroes are "REAPER did not say" rather than measurements. `sample_rate` and
	// `channel_count` are optional in `import-audio-file.schema.json`, so a zero is
	// omitted; `imported_track_count` is required with a minimum of 1, so a zero there
	// is a result that cannot be described and is reported as such rather than rounded
	// up to a claim that one part arrived.
	struct imported_media
	{
		std::string item_guid;
		std::string resolved_file_path;

		int sample_rate = 0;
		int channel_count = 0;

		int imported_track_count = 0;
		int note_count = 0;
	};

	// ---------------------------------------------------------------------------
	// The seam
	// ---------------------------------------------------------------------------

	// Everything the item tools need from REAPER, and nothing else.
	//
	// Note what is absent as much as what is present, following `fx_host` and
	// `RenderHost`. There is no undo operation, because the undo block belongs to
	// `undo_manager` and is opened by the executor before a handler runs. There is no
	// "run a REAPER action", because ADR 0005 chose constrained tools over generic
	// scripting and a hole that size would let any decision leak to this side of the
	// seam. There is no take operation beyond reading the active take's name, because
	// no tool in the contract edits takes. Each absence is a requirement that cannot be
	// violated through this interface.
	//
	// Every method addresses items and tracks by REAPER GUID rather than by
	// `MediaItem*` or `MediaTrack*`, so this header stays free of the SDK and the suite
	// can implement the interface with no REAPER present.
	class item_host
	{
	public:
		virtual ~item_host() = default;

		// False when any REAPER function the item path needs could not be resolved. A
		// host that is not usable reports an empty project and silently drops writes, so
		// every handler checks this first and fails the action with a reason rather than
		// telling the producer a change landed.
		virtual bool is_usable() const = 0;

		// The functions REAPER did not supply, for the reason that accompanies an
		// unusable host.
		virtual std::vector<std::string> unresolved_function_names() const = 0;

		// --- reads ---

		// Every media item in the project, in track order and then timeline order.
		// Empty for a project with no items, which is a normal answer and not a failure.
		//
		// One read per call rather than a lookup per item, so the snapshot a handler
		// checks against is the snapshot it writes to — see the file header.
		virtual std::vector<item_state> read_project_items() = 0;

		// The producer's current time selection, or nothing when they have none.
		virtual std::optional<item_time_range> read_time_selection() = 0;

		// Whether a path exists and can be read. Deliberately not "whether REAPER can
		// import it": that is REAPER's answer and comes back from
		// `import_media_file`.
		virtual item_file_access probe_file(const std::string& file_path) = 0;

		// --- mutations ---
		//
		// One property per call. A single `set_item_property(name, value)` would put the
		// choice of REAPER parameter name on the far side of an untested seam; these
		// keep the translation unit dull and keep every decision on this side.

		virtual bool set_item_position(const std::string& item_guid, double position_seconds) = 0;
		virtual bool set_item_length(const std::string& item_guid, double length_seconds) = 0;
		virtual bool set_item_fade_in(const std::string& item_guid, double fade_seconds) = 0;
		virtual bool set_item_fade_out(const std::string& item_guid, double fade_seconds) = 0;

		// Linear gain, not decibels. The conversion is `unit_conversion.h`'s and happens
		// on this side of the seam, so the translation unit writes the number REAPER
		// stores and decides nothing.
		virtual bool set_item_volume(const std::string& item_guid, double reaper_volume) = 0;

		virtual bool set_item_muted(const std::string& item_guid, bool muted) = 0;

		// Moves an item to another track, leaving its position alone. Two calls make a
		// cross-track move, which is why `move_items` reports both halves of it.
		virtual bool move_item_to_track(
			const std::string& item_guid,
			const std::string& track_guid) = 0;

		// The two halves, or nothing when REAPER would not split. The caller has already
		// checked that the position falls inside the item, so nothing here means REAPER
		// refused rather than that the position missed.
		virtual std::optional<item_split_halves> split_item(
			const std::string& item_guid,
			double position_seconds) = 0;

		virtual bool delete_item(const std::string& item_guid) = 0;

		// Imports a file onto a track at a position. Nothing when REAPER would not
		// decode it, which for a file REAPER has no reader for is the answer rather than
		// a failure to report as one. Audio and MIDI go through the same call because
		// REAPER's import does; which fields come back populated is what differs.
		virtual std::optional<imported_media> import_media_file(
			const std::string& track_guid,
			const std::string& file_path,
			double position_seconds) = 0;
	};
	// ---------------------------------------------------------------------------
	// Requests
	//
	// One per tool, mirroring the input schemas. `confirmedTrackSelection` is absent
	// throughout: it is read by target resolution, which has already run — see the file
	// header.
	// ---------------------------------------------------------------------------

	// One entry of `set_item_properties`' `changes` array. Only the item is required;
	// every property is optional, and an absent one is left alone rather than reset.
	//
	// `position_seconds` is where the item starts, absolute — the one absolute position
	// in this file's inputs. `length_seconds` trims the end without discarding the
	// underlying audio, so what lands is clamped by the source and is read back rather
	// than assumed.
	struct item_property_change
	{
		std::string item_guid;

		std::optional<double> position_seconds;
		std::optional<double> length_seconds;
		std::optional<double> fade_in_seconds;
		std::optional<double> fade_out_seconds;

		// Decibels, as `volumeDecibels` defines them. Converted to REAPER's linear gain
		// before it crosses the seam.
		std::optional<double> volume_decibels;

		std::optional<bool> muted;

		// False for an entry naming an item and nothing to do to it. Reported rather
		// than treated as a success, because a change that changed nothing is not a
		// change and the producer would be told otherwise.
		bool names_a_change() const
		{
			return position_seconds.has_value()
				|| length_seconds.has_value()
				|| fade_in_seconds.has_value()
				|| fade_out_seconds.has_value()
				|| volume_decibels.has_value()
				|| muted.has_value();
		}
	};

	struct set_item_properties_request
	{
		std::vector<item_property_change> changes;
	};

	// `move_items`. The destination track is not here: it arrives as `toTrack` in the
	// call's track selectors and is resolved by the executor before this handler runs,
	// so an empty resolved target list is what "no destination track" looks like from in
	// here. Keeping it out of the request is what stops the two from disagreeing.
	struct move_items_request
	{
		std::vector<std::string> item_guids;

		// Absent means the call only moved items to another track, which the output
		// schema reports as an applied offset of zero.
		std::optional<double> offset_seconds;
	};

	// `move_all_items`. The tracks are not here either, for the same reason: an empty
	// resolved target list means the call named none, which is the whole project.
	struct move_all_items_request
	{
		double offset_seconds = 0.0;
	};

	struct split_items_request
	{
		std::vector<std::string> item_guids;
		double position_seconds = 0.0;
	};

	struct delete_items_request
	{
		std::vector<std::string> item_guids;
	};

	// Which items `list_selected_items` covers. Three values, and the schema echoes the
	// one used back, because the same session answers the three very differently and
	// "nothing is selected" and "the time selection is empty" need different things said
	// to the producer.
	enum class item_scope
	{
		selected_items,
		selected_tracks,
		time_selection
	};

	inline constexpr std::string_view describe_item_scope(item_scope scope)
	{
		switch (scope)
		{
			case item_scope::selected_items:
				return "selected_items";
			case item_scope::selected_tracks:
				return "selected_tracks";
			case item_scope::time_selection:
				return "time_selection";
		}

		return "selected_items";
	}

	struct list_selected_items_request
	{
		item_scope scope = item_scope::selected_items;

		// The input schema's default is true.
		bool include_take_names = true;
	};

	// Both imports take the same three things, and the track arrives through target
	// resolution. One request type rather than two identical ones, because a second
	// would only differ by name.
	struct import_file_request
	{
		std::string file_path;
		double position_seconds = 0.0;
	};

	// ---------------------------------------------------------------------------
	// Results
	//
	// One per tool, field for field with the output schemas. `track` is the resolved
	// reference the executor already produced, and the undo report is attached by the
	// framework — a handler never sees one, which is what keeps a handler from reporting
	// a marker for a tool that should not have one.
	// ---------------------------------------------------------------------------

	// `itemPlacement` from `mcp-tools/outputs/tool-output-defs.schema.json`. Carries the
	// track as well as the bounds, because an item can be moved between tracks and so
	// "where is it now" is two questions.
	struct item_placement
	{
		std::string guid;
		std::string track_guid;
		double position_seconds = 0.0;
		double length_seconds = 0.0;

		// Active take name. Empty means REAPER named none, and the field is omitted
		// rather than filled in.
		std::string name;
	};

	// `set_item_properties`' result carries its own per-entry outcomes, because its
	// output schema requires `actions` and `items` together — see the file header. This
	// is the one result in this file that holds `action_outcome`s.
	struct set_item_properties_result
	{
		// One per entry in the call's `changes` array, in the order they were supplied.
		std::vector<action_outcome> actions;

		// Only the entries that succeeded, with their bounds after the change. Read back
		// rather than echoed: a trim is clamped by the underlying audio, and the
		// resulting length is what the producer hears.
		std::vector<item_placement> items;

		bool every_change_landed() const { return count_failed_actions(actions) == 0; }

		// False when every entry failed, which is a result this tool can reach and must
		// report honestly: `set-item-properties.schema.json` says the absence of
		// `undoPositionBefore` is what tells the producer nothing changed, so the framework
		// needs told. See `handler_success::nothing_was_applied`.
		bool any_change_landed() const { return any_action_was_applied(actions); }
	};

	struct move_items_result
	{
		// Where each item actually ended up, not the offset that was requested.
		std::vector<item_placement> items;

		// Zero when the call only moved items to another track.
		double applied_offset_seconds = 0.0;

		// How many items were held at zero instead of moving the full offset. Non-zero
		// means the group no longer sits at its original internal spacing.
		std::size_t clamped_item_count = 0;

		// The destination track, when the call named one.
		std::optional<TrackReference> to_track;
	};

	struct move_all_items_result
	{
		// The tracks the call named, in the order it named them. Absent — empty here —
		// when the call named none, which `whole_project` says instead of listing the
		// session back.
		std::vector<TrackReference> tracks;

		bool whole_project = false;

		// How many items actually changed position. Counted rather than enumerated,
		// which is this schema's own decision: a project-wide call would return
		// thousands of GUIDs the agent has no use for.
		std::size_t moved_item_count = 0;

		double applied_offset_seconds = 0.0;
		std::size_t clamped_item_count = 0;
	};

	// One item split into two. Both halves are named because REAPER mints a new GUID
	// for the right one, so without the pairing the agent cannot address either piece.
	struct item_split
	{
		std::string original_item_guid;
		item_placement left_item;
		item_placement right_item;
	};

	struct split_items_result
	{
		// One entry per item that was actually split, in timeline order.
		std::vector<item_split> splits;

		// Items the position did not fall inside, so nothing was done to them. A list
		// equal to everything the call named means the position missed every item.
		std::vector<std::string> skipped_item_guids;

		double position_seconds = 0.0;
	};

	struct delete_items_result
	{
		std::vector<std::string> deleted_item_guids;

		// The tracks the deleted items sat on, in track order. Read before the deletion,
		// because an item's track cannot be read once the item is gone.
		std::vector<TrackReference> affected_tracks;

		double deleted_length_seconds = 0.0;
	};

	// `selectedItem` from `list-selected-items.schema.json`. The track name travels with
	// the track GUID so the agent can tell the producer which items it means.
	struct selected_item
	{
		std::string guid;
		std::string track_guid;
		std::string track_name;
		double position_seconds = 0.0;
		double length_seconds = 0.0;

		// Present when the call asked for take names, which it does by default.
		std::optional<std::string> take_name;

		bool muted = false;
		bool selected = false;
	};

	struct list_selected_items_result
	{
		item_scope scope = item_scope::selected_items;
		std::vector<selected_item> items;

		// How many items were in scope before the cap. Equal to `items.size()` when
		// nothing was capped.
		std::size_t total_in_range = 0;

		bool truncated = false;
	};

	struct import_audio_file_result
	{
		TrackReference track;
		item_placement item;

		// As REAPER resolved it, which is what the producer would find on disk.
		std::string file_path;

		// Optional in the schema, so absent when REAPER did not say.
		std::optional<int> sample_rate;
		std::optional<int> channel_count;
	};

	struct import_midi_file_result
	{
		TrackReference track;
		item_placement item;
		std::string file_path;

		// How many parts arrived. More than one means they landed together on a single
		// item rather than separated, which is rarely what a producer importing a
		// multi-part file expects.
		int imported_track_count = 1;

		// Zero means the file parsed but carried no notes — a real outcome, worth saying
		// rather than reporting a successful import of nothing.
		int note_count = 0;
	};

	// Either the tool's result, or the reason the action failed. A variant, so a caller
	// cannot read a populated result next to a populated failure — the same reasoning
	// `object_resolver.h` gives for its own outcomes.
	template <typename Result>
	using item_outcome = std::variant<Result, action_error>;

	// The per-item outcomes a tool reporting per item produces, plus the result it would
	// report if every item landed.
	//
	// Both, because the caller has to choose between the two framework shapes: a success
	// carrying the tool's fields when nothing failed, and a partial carrying the
	// per-item outcomes when something did. The result's own item list and the succeeded
	// outcomes' targets name the same items either way, which is what keeps a partial as
	// informative as the success would have been.
	template <typename Result>
	struct item_action_outcomes
	{
		std::vector<action_outcome> actions;
		Result result;

		bool every_item_landed() const { return count_failed_actions(actions) == 0; }
	};

	template <typename Result>
	using reported_item_outcome = std::variant<item_action_outcomes<Result>, action_error>;

	// ---------------------------------------------------------------------------
	// Shared checks and small conversions
	// ---------------------------------------------------------------------------

	namespace detail
	{
		// A `maxLength` with no `minLength`: empty stays empty rather than gaining a
		// substitute. An unnamed take has no name, and inventing one would have the agent
		// telling the producer what their item is called.
		inline std::string bounded_item_string(std::string_view value, std::size_t maximum_length)
		{
			std::string bounded{value};

			if (bounded.size() > maximum_length)
			{
				bounded.resize(maximum_length);
			}

			return bounded;
		}

		// Seconds, for a message a producer reads. Three decimals: enough to name a
		// position they can find, short enough not to read like a machine dump.
		inline std::string describe_seconds(double seconds)
		{
			if (!std::isfinite(seconds))
			{
				return "an unrepresentable position";
			}

			std::string rendered = std::to_string(seconds);

			const std::size_t point = rendered.find('.');

			if (point != std::string::npos && rendered.size() > point + 4)
			{
				rendered.resize(point + 4);
			}

			return rendered + "s";
		}

		// The framework's shape for "this whole call went wrong", from one reason.
		//
		// A single failed action, which is the only shape that can carry a reason:
		// `handler_success` has nowhere to put one, and requirement 9.5 says the reason
		// is not optional.
		inline handler_action_outcomes failed_item_action(
			std::string_view target,
			const action_error& reason)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, reason.code(), reason.message()));

			return outcomes;
		}

		inline action_error unreadable_item_input_error(std::string_view tool_name)
		{
			return action_error{
				item_input_unreadable_code,
				"the input for " + std::string{tool_name}
					+ " could not be read into the shape the handler expects, so nothing was attempted"};
		}

		inline action_error no_item_track_error(std::string_view tool_name)
		{
			return action_error{
				item_track_not_resolved_code,
				"this call named no track, and " + std::string{tool_name} + " addresses one"};
		}

		// Reads one tool's request out of the call's validated input.
		//
		// A null input is the same answer as an unreadable one: the framework holds the
		// payload as a pointer it never dereferences, and a handler that dereferenced a
		// null one would crash REAPER rather than report anything. The server validated
		// the input against the authoritative schema before it arrived, so reaching
		// either branch means something upstream is wrong — which is exactly the case
		// worth reporting rather than assuming away.
		template <typename JsonValue, typename RequestReader, typename Request>
		bool read_item_request(
			const tool_execution_context<JsonValue>& context,
			const RequestReader& read_request,
			Request& request)
		{
			if (context.call.validated_input == nullptr)
			{
				return false;
			}

			return read_request(*context.call.validated_input, request);
		}

		// The track a single-target item tool addresses.
		//
		// Target resolution runs before a handler does and refuses a selector it cannot
		// resolve, so an empty list here means the call named no track at all.
		template <typename JsonValue>
		std::optional<TrackReference> first_item_track(const tool_execution_context<JsonValue>& context)
		{
			if (context.resolved_targets.empty())
			{
				return std::nullopt;
			}

			return context.resolved_targets.front().reference;
		}
	}

	// An unusable host, as a reason rather than as a crash.
	//
	// Every handler's first check. A host missing a function reports an empty project and
	// drops a write on the floor, so proceeding would tell the producer a change landed
	// when nothing did — and `is_usable` exists precisely so that is reportable.
	inline std::optional<action_error> check_item_host_usable(const item_host& host)
	{
		if (host.is_usable())
		{
			return std::nullopt;
		}

		std::string reason{
			"REAPER did not supply the functions the item tools need, so no item operation can run."};

		const std::vector<std::string> missing = host.unresolved_function_names();

		if (!missing.empty())
		{
			reason += " Missing:";

			for (const std::string& function_name : missing)
			{
				reason += ' ';
				reason += function_name;
			}

			reason += '.';
		}

		return action_error{item_host_unavailable_code, reason};
	}

	// How many items a call may address and still have every outcome reported.
	//
	// Checked before anything is written, so a call over the cap changes nothing. See
	// the file header: the input schemas allow 4096 GUIDs and the outcome array holds
	// 512, and a truncated outcome array is actions that happened with nothing said
	// about them.
	inline std::optional<action_error> check_addressed_item_count(
		std::size_t addressed_count,
		std::string_view tool_name)
	{
		if (addressed_count <= maximum_items_per_reported_call)
		{
			return std::nullopt;
		}

		return action_error{
			item_too_many_items_code,
			"this call names " + std::to_string(addressed_count) + " items and a "
				+ std::string{tool_name} + " result can report the outcome of at most "
				+ std::to_string(maximum_items_per_reported_call)
				+ " — nothing was changed, since an outcome list cut to fit would be actions that "
				  "happened with nothing said about them"};
	}

	// A number that arrived from the input and is about to be written to REAPER.
	//
	// The server validated the input against the authoritative schema, so this is the
	// belt to that braces — but a NaN or an infinity written to an item position is a
	// session the producer cannot repair by hand, which is worth one comparison.
	inline std::optional<action_error> check_item_value_finite(double value, std::string_view what)
	{
		if (std::isfinite(value))
		{
			return std::nullopt;
		}

		return action_error{
			item_value_not_finite_code,
			std::string{what} + " is not a finite number, so it was not written"};
	}

	// The item with this GUID, or null.
	//
	// A walk rather than a map, following the reasoning `object_resolver.h` gives for its
	// own GUID walk: the snapshot is read once per call and walked from, and a second
	// data structure would be a second thing that can disagree with the project.
	inline const item_state* find_item_by_guid(
		const std::vector<item_state>& items,
		const std::string& item_guid)
	{
		for (const item_state& item : items)
		{
			if (item.guid == item_guid)
			{
				return &item;
			}
		}

		return nullptr;
	}

	inline action_error item_not_found_error(const std::string& item_guid)
	{
		return action_error{
			item_not_found_code,
			"no item in this project has the GUID " + item_guid
				+ " — call list_selected_items again, since an item the producer deleted or a GUID from "
				  "an earlier session names nothing now"};
	}

	// An item's placement, built from the snapshot taken after the writes.
	inline item_placement placement_of(const item_state& item)
	{
		item_placement placement;
		placement.guid = item.guid;
		placement.track_guid = item.track_guid;
		placement.position_seconds = item.position_seconds;
		placement.length_seconds = item.length_seconds;
		placement.name = detail::bounded_item_string(item.take_name, maximum_item_name_length);

		return placement;
	}

	inline TrackReference track_reference_of(const item_state& item)
	{
		TrackReference reference;
		reference.guid = item.track_guid;
		reference.name = detail::bounded_item_string(item.track_name, maximum_item_track_name_length);

		return reference;
	}

	// Where an item lands when an offset is applied to it, and whether it was held.
	//
	// Items are never moved before zero, which both move schemas state and both results
	// have a field for. Clamping rather than refusing, because a producer asking to pull
	// the arrangement earlier means it — and the clamp is reported, so the agent can tell
	// them the group's internal spacing changed.
	struct offset_placement
	{
		double position_seconds = 0.0;
		bool clamped = false;
	};

	inline offset_placement apply_offset(double position_seconds, double offset_seconds)
	{
		const double moved = position_seconds + offset_seconds;

		if (moved < 0.0)
		{
			return offset_placement{0.0, true};
		}

		return offset_placement{moved, false};
	}

	// Whether a split at this position would fall inside this item.
	//
	// Strictly inside: a position on either edge splits nothing, since one of the halves
	// would be empty. That is the case `skippedItemGuids` exists to report.
	inline bool split_falls_inside(const item_state& item, double position_seconds)
	{
		return position_seconds > item.position_seconds && position_seconds < item.end_seconds();
	}

	// Whether an item sounds during a time range.
	//
	// Overlap rather than containment. A producer who selected the chorus and asks what
	// is in it means the items they can hear across it, and an item running from before
	// the selection into it is one of those — reporting only fully-contained items would
	// leave the loudest thing in the range out of the answer.
	inline bool item_overlaps_range(const item_state& item, const item_time_range& range)
	{
		return item.position_seconds < range.end_seconds && item.end_seconds() > range.start_seconds;
	}

	// Timeline order and then track order, which is the order
	// `list-selected-items.schema.json` asks for. The GUID is the last tiebreak so that
	// two items at the same position on the same track come back in a stable order
	// rather than whichever order the project walk happened to produce.
	inline bool item_precedes(const item_state& left, const item_state& right)
	{
		if (left.position_seconds != right.position_seconds)
		{
			return left.position_seconds < right.position_seconds;
		}

		if (left.track_project_index != right.track_project_index)
		{
			return left.track_project_index < right.track_project_index;
		}

		return left.guid < right.guid;
	}

	// A GUID the call already named.
	//
	// Repeats are reported rather than processed twice, and this is the check that makes
	// `move_items` honest: the same GUID listed twice with an offset of four seconds
	// would move that item eight, and the result would report four. The other three
	// per-item tools deduplicate for the same reason — a second split finds the position
	// outside the half that kept the GUID, and a second delete fails against an item that
	// has gone — so each says so instead.
	inline action_error duplicate_item_guid_error(const std::string& item_guid)
	{
		return action_error{
			item_duplicate_guid_code,
			"this call names item " + item_guid
				+ " more than once, and it was acted on for the first mention only — applying the "
				  "same change twice would move it further than the call asked for"};
	}

	// ---------------------------------------------------------------------------
	// The eight handlers
	//
	// Each takes the host, whatever the call resolved, and the request, and returns the
	// tool's result or the reason the action failed. No handler opens an undo block,
	// captures a marker, or builds a refusal: the first two belong to `undo_manager`
	// through the executor, and the third is not something an item tool can produce.
	// ---------------------------------------------------------------------------

	// set_item_properties.
	//
	// Always returns a result rather than a bare outcome list, because its output schema
	// requires `actions` and `items` together — see the file header. Entries are applied
	// in the order the call supplied them, and a failure part way through one entry
	// leaves that entry's earlier writes in place: requirement 9.4's no-rollback rule
	// applies within an entry as well as across the array, and the reason says so.
	inline item_outcome<set_item_properties_result> apply_set_item_properties(
		item_host& host,
		const set_item_properties_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (request.changes.empty())
		{
			return action_error{
				item_no_items_in_scope_code,
				"this call names no changes, so there was nothing to apply"};
		}

		if (const std::optional<action_error> too_many =
				check_addressed_item_count(request.changes.size(), set_item_properties_tool_name))
		{
			return *too_many;
		}

		const std::vector<item_state> items_before = host.read_project_items();

		set_item_properties_result result;
		result.actions.reserve(request.changes.size());

		// Which outcome belongs to which item, for the read-back below. Positional
		// because the outcomes are reported in the order the changes were supplied.
		std::vector<std::pair<std::size_t, std::string>> applied_entries;
		std::vector<std::string> already_named;

		for (const item_property_change& change : request.changes)
		{
			const std::size_t outcome_index = result.actions.size();

			const item_state* const item = find_item_by_guid(items_before, change.item_guid);

			if (item == nullptr)
			{
				const action_error reason = item_not_found_error(change.item_guid);
				result.actions.push_back(
					failed_action(change.item_guid, reason.code(), reason.message()));

				continue;
			}

			if (std::find(already_named.begin(), already_named.end(), change.item_guid)
				!= already_named.end())
			{
				const action_error reason = duplicate_item_guid_error(change.item_guid);
				result.actions.push_back(
					failed_action(change.item_guid, reason.code(), reason.message()));

				continue;
			}

			if (!change.names_a_change())
			{
				result.actions.push_back(failed_action(
					change.item_guid,
					item_names_no_change_code,
					"this entry names an item and no property to change, so nothing was done to it"));

				continue;
			}

			std::optional<action_error> failure;

			// The finite checks run before any write, so an entry carrying one bad number
			// changes nothing rather than half of what it asked for.
			if (change.position_seconds.has_value())
			{
				failure = check_item_value_finite(*change.position_seconds, "the item position");
			}

			if (!failure.has_value() && change.length_seconds.has_value())
			{
				failure = check_item_value_finite(*change.length_seconds, "the item length");
			}

			if (!failure.has_value() && change.fade_in_seconds.has_value())
			{
				failure = check_item_value_finite(*change.fade_in_seconds, "the fade-in length");
			}

			if (!failure.has_value() && change.fade_out_seconds.has_value())
			{
				failure = check_item_value_finite(*change.fade_out_seconds, "the fade-out length");
			}

			if (!failure.has_value() && change.volume_decibels.has_value())
			{
				failure = check_item_value_finite(*change.volume_decibels, "the item volume");
			}

			if (failure.has_value())
			{
				result.actions.push_back(
					failed_action(change.item_guid, failure->code(), failure->message()));

				continue;
			}

			// Applied one property at a time, and the first write that REAPER refuses
			// stops the entry. `written` names what already landed, because the reason has
			// to say so — the producer's route back from "position moved, length did not"
			// is not the route back from "nothing happened".
			std::vector<std::string_view> written;
			std::string_view refused_property;

			if (change.position_seconds.has_value())
			{
				if (host.set_item_position(change.item_guid, *change.position_seconds))
				{
					written.push_back("position");
				}
				else
				{
					refused_property = "position";
				}
			}

			if (refused_property.empty() && change.length_seconds.has_value())
			{
				if (host.set_item_length(change.item_guid, *change.length_seconds))
				{
					written.push_back("length");
				}
				else
				{
					refused_property = "length";
				}
			}

			if (refused_property.empty() && change.fade_in_seconds.has_value())
			{
				if (host.set_item_fade_in(change.item_guid, *change.fade_in_seconds))
				{
					written.push_back("fade-in");
				}
				else
				{
					refused_property = "fade-in";
				}
			}

			if (refused_property.empty() && change.fade_out_seconds.has_value())
			{
				if (host.set_item_fade_out(change.item_guid, *change.fade_out_seconds))
				{
					written.push_back("fade-out");
				}
				else
				{
					refused_property = "fade-out";
				}
			}

			if (refused_property.empty() && change.volume_decibels.has_value())
			{
				// Decibels in, REAPER's linear gain out. The conversion is
				// `unit_conversion.h`'s, so the seam writes the number REAPER stores.
				const double reaper_volume = reaper_volume_from_volume_decibels(*change.volume_decibels);

				if (host.set_item_volume(change.item_guid, reaper_volume))
				{
					written.push_back("volume");
				}
				else
				{
					refused_property = "volume";
				}
			}

			if (refused_property.empty() && change.muted.has_value())
			{
				if (host.set_item_muted(change.item_guid, *change.muted))
				{
					written.push_back("mute");
				}
				else
				{
					refused_property = "mute";
				}
			}

			already_named.push_back(change.item_guid);

			if (!refused_property.empty())
			{
				std::string reason{"REAPER would not set the "};
				reason += refused_property;
				reason += " of this item";

				if (written.empty())
				{
					reason += ", and nothing on it was changed";
				}
				else
				{
					reason += ". What was already set on it stands:";

					for (const std::string_view& property : written)
					{
						reason += ' ';
						reason += property;
					}

					reason += '.';
				}

				result.actions.push_back(
					failed_action(change.item_guid, item_property_write_failed_code, reason));

				continue;
			}

			result.actions.push_back(succeeded_action(change.item_guid));
			applied_entries.emplace_back(outcome_index, change.item_guid);
		}

		// One read after every write, which is what the reported bounds come from. A trim
		// is clamped by the underlying audio, so the length the producer hears is not the
		// length the call asked for.
		const std::vector<item_state> items_after = host.read_project_items();

		for (const std::pair<std::size_t, std::string>& entry : applied_entries)
		{
			const item_state* const item = find_item_by_guid(items_after, entry.second);

			if (item == nullptr)
			{
				// The writes were accepted and the item has gone. The output schema
				// requires a placement for every entry in `items`, so reporting this one
				// as applied would mean either a missing placement or an invented one.
				result.actions[entry.first] = failed_action(
					entry.second,
					item_read_back_failed_code,
					"this item's properties were set and the item could not be read back afterwards, so "
					"the bounds it now has are unknown");

				continue;
			}

			result.items.push_back(placement_of(*item));
		}

		return result;
	}

	// move_items.
	//
	// `destination_track` is the resolved `toTrack`, or nothing when the call named none.
	// An offset and a destination are both optional in the schema and a call carrying
	// neither names no move at all, which is reported rather than treated as a success.
	inline reported_item_outcome<move_items_result> apply_move_items(
		item_host& host,
		const std::optional<TrackReference>& destination_track,
		const move_items_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (request.item_guids.empty())
		{
			return action_error{
				item_no_items_in_scope_code,
				"this call names no items, so there was nothing to move"};
		}

		if (const std::optional<action_error> too_many =
				check_addressed_item_count(request.item_guids.size(), move_items_tool_name))
		{
			return *too_many;
		}

		if (!request.offset_seconds.has_value() && !destination_track.has_value())
		{
			return action_error{
				item_names_no_change_code,
				"this call names neither an offset along the timeline nor a destination track, so it "
				"names no move — supply offsetSeconds, toTrack, or both"};
		}

		if (request.offset_seconds.has_value())
		{
			if (const std::optional<action_error> not_finite =
					check_item_value_finite(*request.offset_seconds, "the offset"))
			{
				return *not_finite;
			}
		}

		const std::vector<item_state> items_before = host.read_project_items();

		item_action_outcomes<move_items_result> outcomes;
		outcomes.result.applied_offset_seconds = request.offset_seconds.value_or(0.0);
		outcomes.result.to_track = destination_track;
		outcomes.actions.reserve(request.item_guids.size());

		std::vector<std::pair<std::size_t, std::string>> moved_entries;
		std::vector<std::string> already_named;

		for (const std::string& item_guid : request.item_guids)
		{
			const std::size_t outcome_index = outcomes.actions.size();

			const item_state* const item = find_item_by_guid(items_before, item_guid);

			if (item == nullptr)
			{
				const action_error reason = item_not_found_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			if (std::find(already_named.begin(), already_named.end(), item_guid) != already_named.end())
			{
				const action_error reason = duplicate_item_guid_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			already_named.push_back(item_guid);

			// The track first, then the position, so the position written last is the one
			// that stands. A cross-track move is two changes and the result reports both.
			if (destination_track.has_value() && item->track_guid != destination_track->guid)
			{
				if (!host.move_item_to_track(item_guid, destination_track->guid))
				{
					outcomes.actions.push_back(failed_action(
						item_guid,
						item_track_move_failed_code,
						"REAPER would not move this item to \"" + destination_track->name
							+ "\", so it is still on the track it was on and was not moved along the "
							  "timeline either"));

					continue;
				}
			}

			if (request.offset_seconds.has_value())
			{
				const offset_placement landing =
					apply_offset(item->position_seconds, *request.offset_seconds);

				if (!host.set_item_position(item_guid, landing.position_seconds))
				{
					outcomes.actions.push_back(failed_action(
						item_guid,
						item_position_write_failed_code,
						"REAPER would not move this item to " + detail::describe_seconds(landing.position_seconds)
							+ ", so it is where it was"));

					continue;
				}

				if (landing.clamped)
				{
					++outcomes.result.clamped_item_count;
				}
			}

			outcomes.actions.push_back(succeeded_action(item_guid));
			moved_entries.emplace_back(outcome_index, item_guid);
		}

		const std::vector<item_state> items_after = host.read_project_items();

		for (const std::pair<std::size_t, std::string>& entry : moved_entries)
		{
			const item_state* const item = find_item_by_guid(items_after, entry.second);

			if (item == nullptr)
			{
				outcomes.actions[entry.first] = failed_action(
					entry.second,
					item_read_back_failed_code,
					"this item was moved and could not be read back afterwards, so where it now sits is "
					"unknown");

				continue;
			}

			outcomes.result.items.push_back(placement_of(*item));
		}

		return outcomes;
	}

	// What `move_all_items` produced.
	//
	// One summary rather than an outcome per item, which is this tool's own schema's
	// decision: it counts rather than enumerates, because a project-wide call would
	// return thousands of GUIDs the agent has no use for. `actions` is empty when every
	// item moved, and otherwise carries one applied action for what landed and one
	// failure naming how many items did not — the applied one matters, because it is what
	// keeps the undo report on a result where the session really did change.
	struct move_all_items_outcome
	{
		std::vector<action_outcome> actions;
		move_all_items_result result;

		bool every_item_moved() const { return actions.empty(); }
	};

	// move_all_items.
	//
	// `tracks` is the resolved `tracks` selector list, or empty when the call named none,
	// which means the whole project. Destructive and confirmed upstream by the Bedrock
	// agent; by the time it reaches here the producer has approved the shift.
	inline std::variant<move_all_items_outcome, action_error> apply_move_all_items(
		item_host& host,
		const std::vector<TrackReference>& tracks,
		const move_all_items_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (const std::optional<action_error> not_finite =
				check_item_value_finite(request.offset_seconds, "the offset"))
		{
			return *not_finite;
		}

		move_all_items_outcome outcome;
		outcome.result.applied_offset_seconds = request.offset_seconds;
		outcome.result.whole_project = tracks.empty();

		if (!tracks.empty())
		{
			outcome.result.tracks = tracks;

			if (outcome.result.tracks.size() > maximum_moved_track_references)
			{
				outcome.result.tracks.resize(maximum_moved_track_references);
			}
		}

		const std::vector<item_state> items_before = host.read_project_items();

		std::size_t refused_count = 0;

		for (const item_state& item : items_before)
		{
			if (!outcome.result.whole_project)
			{
				bool on_a_named_track = false;

				for (const TrackReference& track : tracks)
				{
					if (track.guid == item.track_guid)
					{
						on_a_named_track = true;
						break;
					}
				}

				if (!on_a_named_track)
				{
					continue;
				}
			}

			const offset_placement landing = apply_offset(item.position_seconds, request.offset_seconds);

			if (!host.set_item_position(item.guid, landing.position_seconds))
			{
				++refused_count;
				continue;
			}

			if (landing.clamped)
			{
				++outcome.result.clamped_item_count;
			}

			// Counted only when the position actually changed, which makes it a count of
			// movement rather than a count of writes. Two cases separate the two: an
			// offset of zero is a schema-valid call that moves nothing, and an item
			// already at zero pulled earlier is held rather than moved — it belongs in
			// `clampedItemCount` and nowhere else, since the producer cannot hear a shift
			// that did not happen.
			if (landing.position_seconds != item.position_seconds)
			{
				++outcome.result.moved_item_count;
			}
		}

		if (refused_count > 0)
		{
			// Requirement 9.5: the failure carries the reason, and here the reason is the
			// two counts — which is the same shape the success reports in, because this
			// tool describes a shift by how much of the session moved.
			outcome.actions.push_back(succeeded_action(move_all_items_tool_name));
			outcome.actions.push_back(failed_action(
				move_all_items_tool_name,
				item_position_write_failed_code,
				"REAPER would not move " + std::to_string(refused_count) + " of the items in scope; "
					+ std::to_string(outcome.result.moved_item_count)
					+ " moved and are left where they landed"));
		}

		return outcome;
	}

	// split_items.
	//
	// A position outside an item is reported as skipped rather than failed — the output
	// schema has `skippedItemGuids` for exactly that, and the agent is told to read a
	// full skip list as a wrong position. An item GUID that names nothing in the project
	// is a different thing and fails.
	inline reported_item_outcome<split_items_result> apply_split_items(
		item_host& host,
		const split_items_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (request.item_guids.empty())
		{
			return action_error{
				item_no_items_in_scope_code,
				"this call names no items, so there was nothing to split"};
		}

		if (const std::optional<action_error> too_many =
				check_addressed_item_count(request.item_guids.size(), split_items_tool_name))
		{
			return *too_many;
		}

		if (const std::optional<action_error> not_finite =
				check_item_value_finite(request.position_seconds, "the split position"))
		{
			return *not_finite;
		}

		const std::vector<item_state> items_before = host.read_project_items();

		item_action_outcomes<split_items_result> outcomes;
		outcomes.result.position_seconds = request.position_seconds;
		outcomes.actions.reserve(request.item_guids.size());

		// The halves, paired with the outcome that reports them, so a half that cannot be
		// read back afterwards turns its own outcome into a failure rather than leaving a
		// split with one side named.
		std::vector<std::pair<std::size_t, item_split_halves>> split_entries;
		std::vector<std::string> already_named;

		for (const std::string& item_guid : request.item_guids)
		{
			const std::size_t outcome_index = outcomes.actions.size();

			const item_state* const item = find_item_by_guid(items_before, item_guid);

			if (item == nullptr)
			{
				const action_error reason = item_not_found_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			if (std::find(already_named.begin(), already_named.end(), item_guid) != already_named.end())
			{
				const action_error reason = duplicate_item_guid_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			already_named.push_back(item_guid);

			if (!split_falls_inside(*item, request.position_seconds))
			{
				// Not a failure. Nothing was done to this item and the result says so by
				// name, which is what lets the agent tell a wrong position from a
				// completed split.
				outcomes.result.skipped_item_guids.push_back(item_guid);
				outcomes.actions.push_back(succeeded_action(item_guid));

				continue;
			}

			const std::optional<item_split_halves> halves =
				host.split_item(item_guid, request.position_seconds);

			if (!halves.has_value())
			{
				outcomes.actions.push_back(failed_action(
					item_guid,
					item_split_failed_code,
					"REAPER would not split this item at "
						+ detail::describe_seconds(request.position_seconds)
						+ ", even though that position falls inside it; the item is whole"));

				continue;
			}

			outcomes.actions.push_back(succeeded_action(item_guid));
			split_entries.emplace_back(outcome_index, *halves);
		}

		const std::vector<item_state> items_after = host.read_project_items();

		for (const std::pair<std::size_t, item_split_halves>& entry : split_entries)
		{
			const item_state* const left = find_item_by_guid(items_after, entry.second.left_item_guid);
			const item_state* const right = find_item_by_guid(items_after, entry.second.right_item_guid);

			if (left == nullptr || right == nullptr)
			{
				// Both halves or neither. The schema requires `leftItem` and `rightItem`
				// together, and "the part after the split" is usually what the producer
				// meant to act on next — a split reported with one side named would send
				// the agent to address an item it cannot see.
				outcomes.actions[entry.first] = failed_action(
					entry.second.left_item_guid,
					item_read_back_failed_code,
					"this item was split and both halves could not be read back afterwards, so neither "
					"piece can be addressed");

				continue;
			}

			item_split split;
			split.original_item_guid = entry.second.left_item_guid;
			split.left_item = placement_of(*left);
			split.right_item = placement_of(*right);

			outcomes.result.splits.push_back(std::move(split));
		}

		// Timeline order, which is the order the output schema asks for.
		std::stable_sort(
			outcomes.result.splits.begin(),
			outcomes.result.splits.end(),
			[](const item_split& left, const item_split& right) {
				return left.left_item.position_seconds < right.left_item.position_seconds;
			});

		return outcomes;
	}

	// delete_items.
	//
	// Everything the result reports about the items is read before the first delete,
	// because an item's track and length cannot be read once the item is gone. Reversible
	// through REAPER's undo, which is the band `tool-classification.js` puts this tool in
	// — see the file header.
	inline reported_item_outcome<delete_items_result> apply_delete_items(
		item_host& host,
		const delete_items_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (request.item_guids.empty())
		{
			return action_error{
				item_no_items_in_scope_code,
				"this call names no items, so there was nothing to delete"};
		}

		if (const std::optional<action_error> too_many =
				check_addressed_item_count(request.item_guids.size(), delete_items_tool_name))
		{
			return *too_many;
		}

		const std::vector<item_state> items_before = host.read_project_items();

		item_action_outcomes<delete_items_result> outcomes;
		outcomes.actions.reserve(request.item_guids.size());

		// Track order, which is the order `affectedTracks` asks for. Collected as
		// (project index, reference) so the order does not depend on which item happened
		// to be named first.
		std::vector<std::pair<int, TrackReference>> affected;
		std::vector<std::string> already_named;

		for (const std::string& item_guid : request.item_guids)
		{
			const item_state* const item = find_item_by_guid(items_before, item_guid);

			if (item == nullptr)
			{
				const action_error reason = item_not_found_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			if (std::find(already_named.begin(), already_named.end(), item_guid) != already_named.end())
			{
				const action_error reason = duplicate_item_guid_error(item_guid);
				outcomes.actions.push_back(failed_action(item_guid, reason.code(), reason.message()));

				continue;
			}

			already_named.push_back(item_guid);

			// Read before the delete, and kept only if the delete lands.
			const TrackReference track = track_reference_of(*item);
			const double length_seconds = item->length_seconds;
			const int track_index = item->track_project_index;

			if (!host.delete_item(item_guid))
			{
				outcomes.actions.push_back(failed_action(
					item_guid,
					item_delete_failed_code,
					"REAPER would not delete this item from \"" + track.name
						+ "\"; it is still on the track"));

				continue;
			}

			outcomes.actions.push_back(succeeded_action(item_guid));
			outcomes.result.deleted_item_guids.push_back(item_guid);
			outcomes.result.deleted_length_seconds += length_seconds;

			bool track_already_named = false;

			for (const std::pair<int, TrackReference>& named : affected)
			{
				if (named.second.guid == track.guid)
				{
					track_already_named = true;
					break;
				}
			}

			if (!track_already_named && affected.size() < maximum_affected_track_references)
			{
				affected.emplace_back(track_index, track);
			}
		}

		std::stable_sort(
			affected.begin(),
			affected.end(),
			[](const std::pair<int, TrackReference>& left, const std::pair<int, TrackReference>& right) {
				return left.first < right.first;
			});

		outcomes.result.affected_tracks.reserve(affected.size());

		for (std::pair<int, TrackReference>& named : affected)
		{
			outcomes.result.affected_tracks.push_back(std::move(named.second));
		}

		return outcomes;
	}

	// list_selected_items.
	//
	// A read tool, so no undo block and no marker. Requirement 23.9 is answered by the
	// cap plus `totalInRange` and `truncated`, which this output schema has and
	// `list_track_fx`'s does not — see the file header.
	inline item_outcome<list_selected_items_result> apply_list_selected_items(
		item_host& host,
		const list_selected_items_request& request)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		std::vector<item_state> items = host.read_project_items();

		std::optional<item_time_range> time_selection;

		if (request.scope == item_scope::time_selection)
		{
			time_selection = host.read_time_selection();
		}

		std::vector<item_state> in_scope;

		for (item_state& item : items)
		{
			switch (request.scope)
			{
				case item_scope::selected_items:
					if (!item.selected)
					{
						continue;
					}

					break;

				case item_scope::selected_tracks:
					if (!item.track_selected)
					{
						continue;
					}

					break;

				case item_scope::time_selection:
					// No time selection, or an empty one, puts nothing in scope. That is a
					// complete answer and not a failure: the scope is echoed on the result
					// so an empty list can be explained rather than just reported.
					if (!time_selection.has_value() || time_selection->is_empty()
						|| !item_overlaps_range(item, *time_selection))
					{
						continue;
					}

					break;
			}

			in_scope.push_back(std::move(item));
		}

		std::stable_sort(in_scope.begin(), in_scope.end(), item_precedes);

		list_selected_items_result result;
		result.scope = request.scope;

		// The count before the cap, which is what `totalInRange` means.
		result.total_in_range = in_scope.size();

		const std::size_t returned = std::min(in_scope.size(), maximum_reported_items);
		result.items.reserve(returned);

		for (std::size_t position = 0; position < returned; ++position)
		{
			const item_state& item = in_scope[position];

			selected_item reported;
			reported.guid = item.guid;
			reported.track_guid = item.track_guid;
			reported.track_name =
				detail::bounded_item_string(item.track_name, maximum_item_track_name_length);
			reported.position_seconds = item.position_seconds;
			reported.length_seconds = item.length_seconds;
			reported.muted = item.muted;
			reported.selected = item.selected;

			if (request.include_take_names)
			{
				reported.take_name =
					detail::bounded_item_string(item.take_name, maximum_item_name_length);
			}

			result.items.push_back(std::move(reported));
		}

		result.truncated = result.total_in_range > result.items.size();

		return result;
	}

	// What an import produced, before it is shaped into one of the two results.
	struct imported_item
	{
		imported_media media;
		item_placement placement;
	};

	// The part `import_audio_file` and `import_midi_file` share.
	//
	// Three failures rather than one, because the producer's next step differs: find the
	// file, fix its permissions, or convert it. The first two are filesystem facts and
	// come from `probe_file`; the third is REAPER's answer and comes from the import
	// itself. Nothing here reads the file's contents to guess which — REAPER decides what
	// it can decode.
	inline item_outcome<imported_item> import_media_onto_track(
		item_host& host,
		const TrackReference& track,
		const import_file_request& request,
		std::string_view what_kind)
	{
		if (const std::optional<action_error> unusable = check_item_host_usable(host))
		{
			return *unusable;
		}

		if (request.file_path.empty())
		{
			return action_error{
				item_file_not_found_code,
				"no file path was given, so there was nothing to import"};
		}

		if (const std::optional<action_error> not_finite =
				check_item_value_finite(request.position_seconds, "the position"))
		{
			return *not_finite;
		}

		switch (host.probe_file(request.file_path))
		{
			case item_file_access::missing:
				return action_error{
					item_file_not_found_code,
					"there is no file at \"" + request.file_path
						+ "\" — nothing was imported, and the path has to be one that exists on the "
						  "machine REAPER is running on"};

			case item_file_access::unreadable:
				return action_error{
					item_file_not_readable_code,
					"the file at \"" + request.file_path
						+ "\" exists and cannot be read — nothing was imported, and the producer will "
						  "need to fix its permissions before this call can work"};

			case item_file_access::readable:
				break;
		}

		const std::optional<imported_media> imported =
			host.import_media_file(track.guid, request.file_path, request.position_seconds);

		if (!imported.has_value())
		{
			return action_error{
				item_file_not_importable_code,
				"REAPER would not import \"" + request.file_path + "\" as " + std::string{what_kind}
					+ " — the file is there and readable, so this is REAPER saying it has no reader for "
					  "it rather than a problem with the path"};
		}

		// The item REAPER created, read back from the project. Its length is the point:
		// the call supplies a position but not a duration, so until the file is opened
		// nobody knows where the item ends.
		const std::vector<item_state> items_after = host.read_project_items();
		const item_state* const item = find_item_by_guid(items_after, imported->item_guid);

		if (item == nullptr)
		{
			return action_error{
				item_read_back_failed_code,
				"REAPER reported importing \"" + request.file_path
					+ "\" and the item it named cannot be found in the project, so where the item sits "
					  "and how long it is are unknown"};
		}

		imported_item result;
		result.media = *imported;
		result.media.resolved_file_path = detail::bounded_item_string(
			imported->resolved_file_path.empty() ? request.file_path : imported->resolved_file_path,
			maximum_item_file_path_length);
		result.placement = placement_of(*item);

		return result;
	}

	inline item_outcome<import_audio_file_result> apply_import_audio_file(
		item_host& host,
		const TrackReference& track,
		const import_file_request& request)
	{
		item_outcome<imported_item> imported =
			import_media_onto_track(host, track, request, "audio");

		if (const action_error* const reason = std::get_if<action_error>(&imported))
		{
			return *reason;
		}

		const imported_item& item = std::get<imported_item>(imported);

		import_audio_file_result result;
		result.track = track;
		result.item = item.placement;
		result.file_path = item.media.resolved_file_path;

		// Both optional in the schema, so a zero means REAPER did not say and the field
		// is left out rather than reported as a sample rate of nothing.
		if (item.media.sample_rate > 0)
		{
			result.sample_rate = item.media.sample_rate;
		}

		if (item.media.channel_count > 0)
		{
			result.channel_count = item.media.channel_count;
		}

		return result;
	}

	inline item_outcome<import_midi_file_result> apply_import_midi_file(
		item_host& host,
		const TrackReference& track,
		const import_file_request& request)
	{
		item_outcome<imported_item> imported = import_media_onto_track(host, track, request, "MIDI");

		if (const action_error* const reason = std::get_if<action_error>(&imported))
		{
			return *reason;
		}

		const imported_item& item = std::get<imported_item>(imported);

		if (item.media.imported_track_count < 1)
		{
			// `importedTrackCount` is required with a minimum of 1, and the field exists to
			// stop a multi-part file being reported as a clean import. Rounding an unknown
			// count up to one would say exactly the thing the field is there to prevent,
			// so the import is reported as a failed action instead and REAPER's undo is the
			// way back.
			return action_error{
				item_import_unreportable_code,
				"\"" + request.file_path
					+ "\" was imported and REAPER did not say how many parts it held, so whether the "
					  "parts arrived together on one item cannot be reported — undo to put it back"};
		}

		import_midi_file_result result;
		result.track = track;
		result.item = item.placement;
		result.file_path = item.media.resolved_file_path;
		result.imported_track_count = item.media.imported_track_count;
		result.note_count = item.media.note_count < 0 ? 0 : item.media.note_count;

		return result;
	}

	// ---------------------------------------------------------------------------
	// Registration
	//
	// Through `tool_executor.h`'s seam: `register_read_tool` for `list_selected_items`,
	// `register_mutating_tool` for the other seven. No mutating tool supplies a
	// precondition check, because none of them can refuse — see the file header.
	//
	// Templated on the payload type, on how a request is read out of the already
	// validated input, and on how a result becomes a payload. The framework holds the
	// input as an opaque pointer it never dereferences (requirement 4.4), so reading a
	// field is the codec's job and not this file's; injecting both directions is what
	// lets the suite drive all eight handlers with no JSON library present.
	//
	// `RequestReader` is called as `read(input, request) -> bool`, overloaded per request
	// type. False means the input could not be read, which becomes a failed action rather
	// than a crash — the server validated the input, so this is the belt to that braces.
	//
	// The two imports share `import_file_request`, so the reader cannot tell them apart by
	// type. It is called with the tool name as a third argument for those two, which is
	// the one place a reader needs to know which tool it is reading for.
	//
	// `ResultWriter` is called as `write(result) -> JsonValue`, overloaded per result
	// type.
	// ---------------------------------------------------------------------------

	// What one registration did, so a caller can report a gap at startup rather than
	// discovering it one unknown-tool error at a time while a producer waits.
	struct item_tool_registration
	{
		std::string tool_name;
		tool_registration_outcome outcome = tool_registration_outcome::registered;
	};

	namespace detail
	{
		// Every track the call resolved, in the order the input named them.
		template <typename JsonValue>
		std::vector<TrackReference> resolved_item_tracks(
			const tool_execution_context<JsonValue>& context)
		{
			std::vector<TrackReference> tracks;
			tracks.reserve(context.resolved_targets.size());

			for (const ResolvedTrack& resolved : context.resolved_targets)
			{
				tracks.push_back(resolved.reference);
			}

			return tracks;
		}

		// The two imports read into the same request type, so the tool name goes with it.
		template <typename JsonValue, typename RequestReader>
		bool read_import_request(
			const tool_execution_context<JsonValue>& context,
			const RequestReader& read_request,
			std::string_view tool_name,
			import_file_request& request)
		{
			if (context.call.validated_input == nullptr)
			{
				return false;
			}

			return read_request(*context.call.validated_input, request, tool_name);
		}
	}

	// Registers all eight. The returned vector holds one entry per tool in registration
	// order, so a caller can log the ones that did not take.
	//
	// Written as eight explicit lambdas rather than one generic adapter over all of them,
	// following `register_fx_tools` and for the reason it records: a single parameterised
	// registrar needs `decltype` on each handler's own return type to name the shape it is
	// building, which makes a change to any one tool a puzzle about template deduction
	// across the other seven.
	template <typename JsonValue, typename RequestReader, typename ResultWriter>
	std::vector<item_tool_registration> register_item_tools(
		tool_handler_registry<JsonValue>& registry,
		item_host& host,
		RequestReader read_request,
		ResultWriter write_result)
	{
		std::vector<item_tool_registration> registrations;

		// The one action array tool here, and the one whose payload always carries its
		// per-entry outcomes: its output schema requires `actions` and `items` together,
		// so a framework partial would drop `items`.
		registrations.push_back(item_tool_registration{
			std::string{set_item_properties_tool_name},
			registry.register_mutating_tool(
				set_item_properties_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					set_item_properties_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							set_item_properties_tool_name,
							detail::unreadable_item_input_error(set_item_properties_tool_name));
					}

					item_outcome<set_item_properties_result> outcome =
						apply_set_item_properties(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(set_item_properties_tool_name, *reason);
					}

					const set_item_properties_result& result =
						std::get<set_item_properties_result>(outcome);

					// The undo report is the framework's to attach and this handler's to
					// veto. An array in which every entry failed left nothing for "revert
					// all" to walk back to, and the framework holds the payload opaquely
					// (requirement 4.4) so it cannot count the outcomes inside it.
					handler_success<JsonValue> produced;
					produced.nothing_was_applied = !result.any_change_landed();
					produced.fields = write_result(result);

					return produced;
				})});

		registrations.push_back(item_tool_registration{
			std::string{move_items_tool_name},
			registry.register_mutating_tool(
				move_items_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					move_items_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							move_items_tool_name,
							detail::unreadable_item_input_error(move_items_tool_name));
					}

					// Empty means the call named no `toTrack`, which is a move along the
					// timeline only. The selector is resolved before this handler runs, so
					// an unresolvable one already refused the call.
					const std::optional<TrackReference> destination =
						detail::first_item_track<JsonValue>(context);

					reported_item_outcome<move_items_result> outcome =
						apply_move_items(host, destination, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(move_items_tool_name, *reason);
					}

					item_action_outcomes<move_items_result>& outcomes =
						std::get<item_action_outcomes<move_items_result>>(outcome);

					if (!outcomes.every_item_landed())
					{
						// Requirement 9.4: what landed stays landed, and each failure names
						// the item it belongs to by GUID.
						return handler_action_outcomes{std::move(outcomes.actions)};
					}

					return handler_success<JsonValue>{write_result(outcomes.result)};
				})});

		// The one tool that reports a count rather than an item list, so a partial is one
		// summary rather than thousands of outcomes.
		registrations.push_back(item_tool_registration{
			std::string{move_all_items_tool_name},
			registry.register_mutating_tool(
				move_all_items_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					move_all_items_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							move_all_items_tool_name,
							detail::unreadable_item_input_error(move_all_items_tool_name));
					}

					// Empty means the call named no tracks, which is the whole project.
					std::variant<move_all_items_outcome, action_error> outcome = apply_move_all_items(
						host,
						detail::resolved_item_tracks<JsonValue>(context),
						request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(move_all_items_tool_name, *reason);
					}

					move_all_items_outcome& moved = std::get<move_all_items_outcome>(outcome);

					if (!moved.every_item_moved())
					{
						return handler_action_outcomes{std::move(moved.actions)};
					}

					return handler_success<JsonValue>{write_result(moved.result)};
				})});

		registrations.push_back(item_tool_registration{
			std::string{split_items_tool_name},
			registry.register_mutating_tool(
				split_items_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					split_items_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							split_items_tool_name,
							detail::unreadable_item_input_error(split_items_tool_name));
					}

					reported_item_outcome<split_items_result> outcome =
						apply_split_items(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(split_items_tool_name, *reason);
					}

					item_action_outcomes<split_items_result>& outcomes =
						std::get<item_action_outcomes<split_items_result>>(outcome);

					if (!outcomes.every_item_landed())
					{
						return handler_action_outcomes{std::move(outcomes.actions)};
					}

					return handler_success<JsonValue>{write_result(outcomes.result)};
				})});

		registrations.push_back(item_tool_registration{
			std::string{delete_items_tool_name},
			registry.register_mutating_tool(
				delete_items_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					delete_items_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							delete_items_tool_name,
							detail::unreadable_item_input_error(delete_items_tool_name));
					}

					reported_item_outcome<delete_items_result> outcome =
						apply_delete_items(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(delete_items_tool_name, *reason);
					}

					item_action_outcomes<delete_items_result>& outcomes =
						std::get<item_action_outcomes<delete_items_result>>(outcome);

					if (!outcomes.every_item_landed())
					{
						return handler_action_outcomes{std::move(outcomes.actions)};
					}

					return handler_success<JsonValue>{write_result(outcomes.result)};
				})});

		registrations.push_back(item_tool_registration{
			std::string{import_audio_file_tool_name},
			registry.register_mutating_tool(
				import_audio_file_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					import_file_request request;

					if (!detail::read_import_request(
							context,
							read_request,
							import_audio_file_tool_name,
							request))
					{
						return detail::failed_item_action(
							import_audio_file_tool_name,
							detail::unreadable_item_input_error(import_audio_file_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_item_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_item_action(
							import_audio_file_tool_name,
							detail::no_item_track_error(import_audio_file_tool_name));
					}

					item_outcome<import_audio_file_result> outcome =
						apply_import_audio_file(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(import_audio_file_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<import_audio_file_result>(outcome))};
				})});

		registrations.push_back(item_tool_registration{
			std::string{import_midi_file_tool_name},
			registry.register_mutating_tool(
				import_midi_file_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					import_file_request request;

					if (!detail::read_import_request(
							context,
							read_request,
							import_midi_file_tool_name,
							request))
					{
						return detail::failed_item_action(
							import_midi_file_tool_name,
							detail::unreadable_item_input_error(import_midi_file_tool_name));
					}

					const std::optional<TrackReference> track =
						detail::first_item_track<JsonValue>(context);

					if (!track.has_value())
					{
						return detail::failed_item_action(
							import_midi_file_tool_name,
							detail::no_item_track_error(import_midi_file_tool_name));
					}

					item_outcome<import_midi_file_result> outcome =
						apply_import_midi_file(host, *track, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(import_midi_file_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<import_midi_file_result>(outcome))};
				})});

		// The one read. `register_read_tool`, so no undo block is opened and no marker is
		// captured (requirement 10.3). A read tool's body *may* refuse, because nothing
		// was opened for it — but this one does not, for the reason in the file header.
		registrations.push_back(item_tool_registration{
			std::string{list_selected_items_tool_name},
			registry.register_read_tool(
				list_selected_items_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					list_selected_items_request request;

					if (!detail::read_item_request(context, read_request, request))
					{
						return detail::failed_item_action(
							list_selected_items_tool_name,
							detail::unreadable_item_input_error(list_selected_items_tool_name));
					}

					item_outcome<list_selected_items_result> outcome =
						apply_list_selected_items(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_item_action(list_selected_items_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<list_selected_items_result>(outcome))};
				})});

		return registrations;
	}

	// How many tools `register_item_tools` registers, so a caller checking completeness
	// does not have to count the blocks above.
	inline constexpr std::size_t item_tool_count = 8;

	// Whether every registration took. False names a gap the extension can report at
	// startup, rather than discovering it one unknown-tool error at a time while a
	// producer waits.
	inline bool every_item_tool_registered(const std::vector<item_tool_registration>& registrations)
	{
		if (registrations.size() != item_tool_count)
		{
			return false;
		}

		for (const item_tool_registration& registration : registrations)
		{
			if (registration.outcome != tool_registration_outcome::registered)
			{
				return false;
			}
		}

		return true;
	}
}

#endif  // SESH_AI_DAW_TOOLS_ITEM_TOOLS_H
