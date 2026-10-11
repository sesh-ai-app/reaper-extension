// Track state tools — `set_track_state`, `list_tracks`, and `get_project_summary`
// (task 10.3, requirements 9.1 and 9.2).
//
// These three are the read-and-write pair for everything the project context snapshot
// deliberately leaves out, plus the session's own globals. Requirement 6.2 keeps
// volume, pan, mute, solo, record arm, colour, learned vocabulary, and item counts out
// of the snapshot and names the read tools as where they live, so `list_tracks` is not
// a convenience over the snapshot — it is the only place that detail is available, and
// `set_track_state` is the only place it is written.
//
// ---------------------------------------------------------------------------
// The three tools, and what each one is for
//
//   `set_track_state`      One entry per track, each carrying only the fields to set:
//                          name, learned alias, volume, pan, mute, solo, record arm,
//                          colour. An action array tool, so the whole array runs in
//                          one undo block and the producer reverts the request with
//                          one Ctrl+Z.
//
//   `list_tracks`          Per-track state, in REAPER's track order or in the order
//                          the call named the tracks. The only read path for learned
//                          vocabulary in the protocol — see below, because that is
//                          what makes the alias array here load-bearing rather than
//                          decorative.
//
//   `get_project_summary`  The session's globals and its size. Project-level only: no
//                          per-track field, in either direction.
//
// ---------------------------------------------------------------------------
// Levels go through one conversion, which is not this file's
//
// REAPER stores a fader as a linear gain factor and a pan position as -1 to 1. The
// schemas report decibels and a percentage, and requirement 19 is that the two agree
// closely enough for a level read by `list_tracks` to be written straight back by
// `set_track_state`. `context::converted_track_levels` is the entry point, delegating
// to `daw/unit_conversion.h`, and it exists precisely so that the read tools and the
// Project Context Builder share one answer instead of each reaching into the
// arithmetic separately and drifting on the edge cases it documents — a stored gain of
// exactly zero, a NaN, the exactness of both bounds.
//
// The write direction goes through `daw::reaper_volume_from_volume_decibels` and
// `daw::reaper_pan_from_pan_percent`, which are the paired inverses of the two
// functions the read uses. Passing decibels straight to `SetMediaTrackInfo_Value` is
// the mistake this paragraph exists to name: REAPER would read -6 dB as a gain of six,
// which is +15.6 dB, so "bring the vocal down a touch" would raise it by a factor of
// six. It is silent, it is destructive, and nothing in REAPER's API refuses it.
//
// ---------------------------------------------------------------------------
// Aliases: every one of them, and the save state before the write
//
// A track answers to several names, not one. `AliasStore` accumulates (requirement
// 8.2), holds insertion order (8.4), and caps the set at sixteen (8.5) — and
// `list-tracks.schema.json` mirrors that with an `aliases` array of up to sixteen
// strings rather than one. Reporting only the first would mean the producer's other
// fifteen names for their kick exist in their project file and are readable by nothing
// in the protocol, since this tool is the only read path for them. So every alias is
// reported, in the order the producer taught them, through
// `daw/stored_alias_lookup.h`'s `StoredAliasLookup` — the adapter keyed on
// `ResolvableTrack`, which is what lets the suite key on a GUID while the extension
// keys on REAPER's handle.
//
// The write side reports three things honestly rather than two. `AliasWriteOutcome`
// distinguishes `learned` from `already_held` from `rejected_cap_reached`, and
// collapsing those into success-or-failure would either tell the producer a name was
// added that was not, or report a track in `aliasesLearned` whose vocabulary is
// unchanged. So: `learned` is an applied action that appears in `aliasesLearned`;
// `already_held` is an applied action that does not, because nothing was written;
// `rejected_cap_reached` is a failed action carrying the store's own reason, which
// names the cap so the producer can be told which name to give up.
//
// `projectSaved` answers requirement 8.10 — that an alias taught in an unsaved session
// is lost on the next REAPER launch, told to the producer at the moment they teach it
// rather than discovered next week. It is sampled **before any write in the call**,
// which is the whole of why `apply_set_track_state` reads it in its first few lines
// and carries it down. Writing an alias dirties the project, and so does writing a
// fader; a save state sampled after either would report "unsaved" for every call that
// changed anything and say nothing about the session the producer is actually in.
// `LearnedAlias::project_saved` is the same question asked one write later and is the
// right answer for the first write in a call — this file samples earlier still,
// because a call that moves a fader and then teaches a name is ordinary.
//
// ---------------------------------------------------------------------------
// `set_track_state` always returns its own payload, never the framework's partial
//
// `schema_validator.h` names the two action array tools whose *own* output schema
// carries `actions`, and this is one of them: `set-track-state.schema.json` requires
// `actions` and `aliasesLearned` together, under `additionalProperties: false`. The
// framework's partial shape carries `actions` and nothing else, so returning it would
// produce a payload missing a field its own schema requires, and the outbound
// validation requirement 4.1 asks for would refuse the tool's valid result.
//
// So every path through this handler ends in `handler_success` holding a
// `set_track_state_result`, including the paths that failed outright — an unusable
// host, unreadable input, an array over the cap. Each of those is a result whose
// `actions` array holds one failed action carrying the reason, with `aliasesLearned`
// empty. That is representable, it is honest, and it is the only shape that can say
// both things at once. `set_item_properties` reached the same conclusion for the same
// reason (task 10.7), and the two are consistent on purpose.
//
// It also means this handler owes the framework one thing the other mutating tools do
// not. `handler_success` normally gets the undo report attached unconditionally, and
// `set-track-state.schema.json` is explicit that the *absence* of `undoPositionBefore` is
// what tells the producer nothing changed. Every outright failure above is a call in which
// nothing changed, so reporting a marker for one would offer "revert all" a position above
// the agent's earlier work — the harm requirement 10.7 names for a refused mutation. The
// framework cannot see it, because requirement 4.4 keeps the payload opaque and it cannot
// count the outcomes inside. `detail::reported_track_state_result` sets
// `handler_success::nothing_was_applied` from `any_change_landed()`, and every return in
// the registration goes through it so no path can forget.
//
// The cap is the same story read from the other end. `set-track-state.schema.json` caps
// `actions` at 512 and the input caps `changes` at 512, so the two already agree and the
// check below can only fire on a call the server should have rejected — but it is the
// refusing half of `tool_executor.h`'s outcome-cap rule either way, and it is checked
// before anything is written. `set_item_properties` needs the same check against a real
// gap: its input allows 4096 changes against its own output schema's 512.
//
// ---------------------------------------------------------------------------
// Requirement 23.9: `list_tracks` fails rather than truncates
//
// `list-tracks.schema.json` caps `tracks` at 4096 and `fxChain` at 256, and carries no
// `totalInRange` and no `truncated` for either. With nothing available to say a list was
// cut, a capped read would be indistinguishable from a smaller project or a shorter
// chain — so a call that would report more than either cap fails with the count named
// instead. That is the same branch `list_track_fx` took (task 10.6) and the opposite of
// `list_selected_items` (10.7), whose schema carries both fields and can therefore cap
// and say so. The difference is what the schema makes sayable, not a change of
// principle.
//
// The two failures differ only in what they can suggest. A session with more tracks than
// the cap is answered by naming the tracks to read; a chain longer than the cap is
// answered by asking again with `includeFxChain` false, which leaves every other track's
// state readable and sends the chain itself to `list_track_fx`.
//
// Both failures leave through `handler_action_outcomes`, which is the only shape a read
// tool has for a reason, and that payload validates against
// `messages/tool-result-partial.schema.json` rather than against
// `list-tracks.schema.json` — which requires `tracks` under `additionalProperties: false`
// and would refuse it. `tool_executor.h`'s `result_schema_path_for` decides that and its
// header explains why a framework partial is never the named tool's own shape.
//
// ---------------------------------------------------------------------------
// Roles are derived from the whole project, and derived once
//
// `list_tracks` reports each track's structural role, and the role is the agent's
// statement to the producer about their own routing — a folder parent reported
// `silent_folder_parent` is Sesh telling them their bus carries nothing. So it is
// derived rather than reinvented: `context::derive_track_role_signals` collects the
// signals and `context::derive_structural_role` classifies them, exactly as
// `routing_tools.h` delegates for `get_routing`. Deriving it a second way here is how
// the read tools and the snapshot come to disagree about whether a producer's folder
// parent sums.
//
// Two consequences worth stating, because both are ways to get a role wrong:
//
//   The whole project is read even when the call named three tracks. Folder nesting
//   exists only as the running total of a delta across track order, so accumulating
//   over a filtered list would total deltas across a list with holes in it. The filter
//   is applied to the reported output, after derivation, never to the input.
//
//   `has_fx` comes from the track's FX count, not from the reported chain. `fxChain`
//   is omitted when the call asked for it to be, and a role that changed with a
//   reporting flag would have the same session classified two ways by two calls.
//
// ---------------------------------------------------------------------------
// What `list_tracks` does not carry, and why that is not a gap
//
// No sends and no receives. `get-routing.schema.json` carries both, together with
// folder depth and the parent send flag, because REAPER sums three separate ways and
// only one of them appears in send enumeration — a routing answer carrying sends alone
// would be wrong rather than incomplete. The design allocates routing detail to
// `get_routing` for that reason, and `list-tracks.schema.json` is closed with
// `additionalProperties: false` around the state fields. The receive count is still
// *read* here, because role derivation needs it to tell an aux return from an ordinary
// track; it is not reported.
//
// ---------------------------------------------------------------------------
// None of the three can refuse
//
// `messages/tool-result-refusal.schema.json` closes `reason` at five values:
// `foreign_undo_entries`, `output_file_collision`, `ambiguous_track_selector`,
// `unresolved_track_selector`, and `signal_cycle`. The first two belong to the undo
// and render tools, the last to routing, and the two track selector refusals are
// produced by the Tool Executor's own target resolution before any handler here runs.
// Nothing these three can discover names a reason, so `set_track_state` registers no
// precondition check and the two reads never return a refusal. The invalid calls they
// can meet — an entry naming no change, an alias past the cap, a project too large to
// report — are failed actions carrying a reason, following the precedent requirement
// 9.6 sets for an inverted time range.
//
// ---------------------------------------------------------------------------
// Shape of this file
//
// Header-only and inline, following the other six tool families. The Catch2 target
// compiles what it finds under `tests/` and does not compile `src/`, so logic the
// suite exercises has to be visible through the header, and nothing here includes the
// REAPER SDK or a JSON library: REAPER is reached through `track_state_host` below and
// the tool payloads through a codec the caller supplies.

#ifndef SESH_AI_DAW_TOOLS_TRACK_STATE_TOOLS_H
#define SESH_AI_DAW_TOOLS_TRACK_STATE_TOOLS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <context/project_context_builder.h>
#include <context/structural_role.h>
#include <daw/alias_store.h>
#include <daw/object_resolver.h>
#include <daw/tool_executor.h>
#include <daw/unit_conversion.h>

namespace sesh_ai::daw::tools
{
	// ---------------------------------------------------------------------------
	// Contract values
	// ---------------------------------------------------------------------------

	// The three tool names, spelled once. A typo here is rejected by the registry
	// rather than surfacing as requirement 23.6's unknown-tool error in a producer's
	// session.
	inline constexpr std::string_view set_track_state_tool_name{"set_track_state"};
	inline constexpr std::string_view list_tracks_tool_name{"list_tracks"};
	inline constexpr std::string_view get_project_summary_tool_name{"get_project_summary"};

	// `code` values on a failed action. Each names something the producer or the agent
	// can act on, rather than restating that the tool did not work.
	inline constexpr std::string_view track_state_host_unavailable_code{"track_state_host_unavailable"};
	inline constexpr std::string_view track_state_input_unreadable_code{"track_state_input_unreadable"};
	inline constexpr std::string_view track_state_unknown_track_code{"track_state_unknown_track"};
	inline constexpr std::string_view track_state_names_no_change_code{"track_state_names_no_change"};
	inline constexpr std::string_view track_state_write_failed_code{"track_state_write_failed"};
	inline constexpr std::string_view track_state_too_many_changes_code{"track_state_too_many_changes"};
	inline constexpr std::string_view track_list_too_long_to_report_code{"track_list_too_long_to_report"};
	inline constexpr std::string_view track_fx_chain_too_long_to_report_code{
		"track_fx_chain_too_long_to_report"};
	inline constexpr std::string_view project_summary_unreadable_code{"project_summary_unreadable"};

	// The four alias refusals, kept apart because they ask the producer for four
	// different things: forget a name, say something, say something shorter, and try
	// again.
	inline constexpr std::string_view track_alias_cap_reached_code{"track_alias_cap_reached"};
	inline constexpr std::string_view track_alias_empty_code{"track_alias_empty"};
	inline constexpr std::string_view track_alias_too_long_code{"track_alias_too_long"};
	inline constexpr std::string_view track_alias_write_failed_code{"track_alias_write_failed"};

	// `list-tracks.schema.json` caps `tracks` at 4096 and has no way to say the list
	// was cut, which is why exceeding it fails rather than truncates.
	inline constexpr std::size_t maximum_reported_track_states = 4096;

	// `list-tracks.schema.json` caps `fxChain` at 256, and carries nothing to say a
	// chain was cut either. So the same answer as the track list: a chain over the cap
	// fails rather than being truncated, which is also what `list_track_fx` does with its
	// own identical cap. The difference from the track list is only in what the producer
	// can do about it — `includeFxChain` is a field of the call, so the failure names a
	// way out that leaves every other track's state readable.
	inline constexpr std::size_t maximum_reported_fx_chain_names = 256;

	// `set-track-state.schema.json` caps `actions` at 512 and `aliasesLearned` at 512,
	// and its input caps `changes` at 512. The same number as
	// `tool_executor.h`'s `maximum_action_outcomes`, which is not a coincidence: an
	// outcome array cut to fit would be changes that happened with nothing said about
	// them, which requirements 9.5 and 23.9 both forbid.
	inline constexpr std::size_t maximum_track_state_changes = maximum_action_outcomes;

	// Field lengths the output schemas impose, used when a name REAPER gave us is
	// longer than the contract allows. Neither carries `minLength`, so an unnamed track
	// reports an empty name, which is what REAPER actually holds for one.
	inline constexpr std::size_t maximum_reported_track_name_length = 512;
	inline constexpr std::size_t maximum_reported_fx_chain_name_length = 512;

	// ---------------------------------------------------------------------------
	// The REAPER seam
	//
	// One interface for the reads and writes these three tools need, following
	// `reaper_fx_host.h` and `reaper_track_structure_host.h`: the interface is SDK-free,
	// the production implementation is the only translation unit that includes the SDK,
	// and the suite substitutes a scripted project. The test target has no SDK include
	// path, which is what enforces the split rather than a convention anybody has to
	// remember.
	// ---------------------------------------------------------------------------

	// One track as these tools read it.
	//
	// Three groups of fields, and the third is the one worth noticing. The first is
	// reported as-is. The second is converted on the way out. The third is read only to
	// derive the structural role and is never serialised — folder depth, the parent send
	// flag, and the receive count are `get_routing`'s to report, and the role cannot be
	// derived without them.
	struct track_state_reading
	{
		// REAPER's braced GUID form, as `guidToString` produces it.
		std::string guid;

		// The name REAPER shows. Empty is allowed and common: an unnamed track in
		// REAPER has no name rather than a placeholder.
		std::string name;

		// REAPER's handle, for the alias seam. Null on a synthetic list, which is
		// `AliasStore::track_aliases`' own tolerated case and means a track list
		// assembled outside REAPER answers with no aliases rather than faulting.
		MediaTrack* track = nullptr;

		// `D_VOL`, REAPER's linear gain factor where 1.0 is unity. Converted, never
		// reported raw.
		double reaper_volume = unity_reaper_volume;

		// `D_PAN`, -1.0 hard left to 1.0 hard right. Converted, never reported raw.
		double reaper_pan = centre_reaper_pan;

		// `B_MUTE`.
		bool muted = false;

		// `I_SOLO` read as a boolean. REAPER distinguishes solo from solo-in-place and
		// the schema does not, so anything non-zero is soloed — which is the question
		// the field name asks.
		bool soloed = false;

		// `I_RECARM`.
		bool armed = false;

		// `I_CUSTOMCOLOR` as a colour, or nothing when REAPER's enabled flag is clear.
		// The flag is honoured on this side of the seam by
		// `color_from_reaper_custom_color`, so a colour REAPER was told to ignore is
		// not reported as one the producer can see.
		std::optional<int> color;

		// The FX on the chain, in chain order. Populated only when the call asked for
		// the chain; `fx_count` is what decides `has_fx` for role derivation, so a
		// role does not change with a reporting flag.
		std::vector<std::string> fx_names;
		int fx_count = 0;

		// `I_FOLDERDEPTH`. Read for role derivation; never serialised.
		int folder_depth_delta = 0;

		// `B_MAINSEND`. Read because a folder parent's children's flags are what decide
		// whether it sums at all; never serialised.
		bool parent_send_enabled = true;

		int item_count = 0;

		// Explicit receives into the track. Read because what makes a track an aux
		// return is that things feed it; never serialised.
		int receive_count = 0;
	};

	// The session's globals and its size, as `get_project_summary` needs them.
	//
	// Deliberately not `context::ProjectGlobalsReading`. That one carries the cursor
	// position, play state, loop points, and time selection, which
	// `get-project-summary.schema.json` does not have and which
	// `additionalProperties: false` would refuse; this one carries `track_count`, which
	// the snapshot expresses as a track array instead. Sharing one reading would mean
	// one of the two schemas carrying fields the other forbids, which is requirement
	// 6.2's separation read from the other end.
	struct project_summary_reading
	{
		// False when the project could not be read — no session open, or a REAPER call
		// that answered with nothing. Reported rather than serialised, because a
		// reading that failed serialises as an empty session and an agent told the
		// session has no tracks will act on that.
		bool readable = false;

		std::string project_name;

		double tempo = 120.0;
		int time_signature_numerator = 4;
		int time_signature_denominator = 4;
		int sample_rate = 48000;

		// Excluding the master track, which sits outside the indexed track list.
		int track_count = 0;

		double project_length = 0.0;

		int marker_count = 0;
		int region_count = 0;

		// Changes beyond the initial tempo and time signature. Zero means the reported
		// pair holds for the whole timeline.
		int tempo_change_count = 0;
	};

	// What these three tools do to a project.
	//
	// Narrow on purpose: it is not "the project", it is these reads and these writes,
	// and a tool that needs another adds it here where the cost of the addition is
	// visible. The writes are one per property rather than one struct write, because
	// requirement 9.4 wants the first refusal named — a single write call that returned
	// one boolean could not say which of seven properties REAPER would not accept.
	class track_state_host
	{
	public:
		virtual ~track_state_host() = default;

		// False when any REAPER function these tools need could not be resolved. A host
		// that is not usable reports an empty project and drops writes on the floor, so
		// every handler checks this first and fails with a reason rather than telling
		// the producer they have no tracks.
		virtual bool is_usable() const = 0;

		// The functions REAPER did not supply, for the reason that accompanies an
		// unusable host.
		virtual std::vector<std::string> unresolved_function_names() const = 0;

		// --- reads ---

		// Project order, which is the order folder deltas accumulate in. The master
		// track is not in this list, matching `TrackListSource` and the structural
		// tools.
		//
		// `include_fx_names` fills `track_state_reading::fx_names`. `fx_count` is
		// filled either way, because role derivation needs to know whether a chain
		// holds anything and must not depend on what the call asked to be reported.
		virtual std::vector<track_state_reading> read_tracks_in_project_order(
			bool include_fx_names) = 0;

		// The session's globals, project-level only.
		virtual project_summary_reading read_project_summary() = 0;

		// --- writes ---
		//
		// Each takes REAPER's own stored representation, converted on this side of the
		// seam. A host that took decibels would be a second place the conversion could
		// be got wrong.

		virtual bool write_track_name(const std::string& track_guid, const std::string& name) = 0;
		virtual bool write_track_volume(const std::string& track_guid, double reaper_volume) = 0;
		virtual bool write_track_pan(const std::string& track_guid, double reaper_pan) = 0;
		virtual bool write_track_muted(const std::string& track_guid, bool muted) = 0;
		virtual bool write_track_soloed(const std::string& track_guid, bool soloed) = 0;
		virtual bool write_track_armed(const std::string& track_guid, bool armed) = 0;

		// `color` is a colour, not an `I_CUSTOMCOLOR` value. The enabled flag is added
		// by the implementation through `reaper_custom_color_from`, because a colour
		// written without it is stored and ignored — which would leave the producer
		// looking at a default colour after a call that reported setting one.
		virtual bool write_track_color(const std::string& track_guid, int color) = 0;
	};

	// ---------------------------------------------------------------------------
	// Requests
	//
	// The tool input as each handler needs it, with every track selector already
	// resolved to a GUID by the Tool Executor (requirement 9.1). Plain structs, so the
	// whole of this file is compilable and testable without a JSON library.
	// ---------------------------------------------------------------------------

	// One entry of `set_track_state`'s `changes` array. Only the track is required;
	// every field present is a field to set, and every field absent is left alone.
	struct track_state_change
	{
		std::string track_guid;

		std::optional<std::string> name;

		// What the producer calls this track, when they have just said so.
		std::optional<std::string> alias;

		// In the schema's units: decibels and a percentage. Converted here, so the
		// codec hands over exactly what arrived on the wire.
		std::optional<double> volume_decibels;
		std::optional<double> pan_percent;

		std::optional<bool> muted;
		std::optional<bool> soloed;
		std::optional<bool> armed;

		// A colour as a 24-bit value, which is what `#RRGGBB` reads as.
		std::optional<int> color;

		// Whether this entry asks for anything at all. An entry naming only a track is
		// reported as a failed action rather than as an applied no-op: the producer
		// asked for something, and a success claiming a change nobody named is the
		// report that cannot be acted on.
		bool names_a_change() const
		{
			return name.has_value()
				|| alias.has_value()
				|| volume_decibels.has_value()
				|| pan_percent.has_value()
				|| muted.has_value()
				|| soloed.has_value()
				|| armed.has_value()
				|| color.has_value();
		}
	};

	struct set_track_state_request
	{
		// In the order the call supplied them, which is the order the outcomes are
		// reported in.
		std::vector<track_state_change> changes;
	};

	struct list_tracks_request
	{
		// Empty means the whole project, in REAPER's track order. Otherwise only these
		// tracks, in the order the call named them.
		std::vector<std::string> track_guids;

		// Defaults to true, matching the input schema's own default.
		bool include_fx_chain = true;
	};

	// `get_project_summary` takes nothing. Named anyway, so the three handlers read
	// alike and a field added to the tool later has somewhere to go.
	struct get_project_summary_request
	{
	};

	// ---------------------------------------------------------------------------
	// Result shapes
	//
	// One struct per tool, mirroring that tool's vendored output schema field for
	// field.
	// ---------------------------------------------------------------------------

	// One track's state, as `list-tracks.schema.json`'s `trackState` holds it.
	struct reported_track_state
	{
		std::string guid;

		// Zero-based position in REAPER's track order. Reported so the agent can say
		// "track 7" to a producer looking at REAPER's track numbers, and explicitly not
		// for addressing the track on a follow-up call.
		int index = 0;

		std::string name;

		// Every alias the producer has taught, in the order they were learned. Empty
		// when none have been, which the codec may omit.
		std::vector<std::string> aliases;

		context::StructuralRole role = context::StructuralRole::normal;

		// The evidence for the role. Not serialised — the schema has no field for it —
		// but carried so a role that looks wrong is diagnosable from one log line
		// rather than by re-deriving it, which is requirement 18.2 applied to this tool
		// the way the snapshot applies it to itself.
		context::StructuralRoleSignals role_signals{};

		double volume_decibels = unity_volume_decibels;
		double pan_percent = centre_pan_percent;

		bool muted = false;
		bool soloed = false;
		bool armed = false;

		int item_count = 0;

		// Absent when REAPER holds no custom colour for the track.
		std::optional<int> color;

		// Absent when the call asked for the chain to be left out. Present and empty
		// for a track with no FX, which is a different answer and worth being able to
		// give: one says nothing was looked at, the other says the chain is empty.
		std::optional<std::vector<std::string>> fx_chain;
	};

	struct list_tracks_result
	{
		std::vector<reported_track_state> tracks;
	};

	// `set_track_state`'s result carries its own per-entry outcomes, because its output
	// schema requires `actions` and `aliasesLearned` together — see the file header.
	struct set_track_state_result
	{
		// One per entry in the call's `changes` array, in the order they were supplied.
		std::vector<action_outcome> actions;

		// The tracks that had an alias *written*. A track whose alias was already held
		// is not here: nothing was written, and reporting it would tell the producer a
		// name was added that was not.
		std::vector<TrackReference> aliases_learned;

		// Present only when an alias was learned, matching the schema. Sampled before
		// any write in the call — see the file header on why that is not a detail.
		std::optional<bool> project_saved;

		bool every_change_landed() const { return count_failed_actions(actions) == 0; }

		// False when every entry failed, which every outright failure of this tool reaches:
		// an unusable host, unreadable input, an array over the cap. The framework needs
		// told, because `set-track-state.schema.json` says the absence of
		// `undoPositionBefore` is what tells the producer nothing changed — see
		// `handler_success::nothing_was_applied`.
		bool any_change_landed() const { return any_action_was_applied(actions); }
	};

	struct project_summary_result
	{
		std::string project_name;

		double tempo = 120.0;
		int time_signature_numerator = 4;
		int time_signature_denominator = 4;
		int sample_rate = 48000;

		int track_count = 0;

		double project_length = 0.0;

		int marker_count = 0;
		int region_count = 0;
		int tempo_change_count = 0;
	};

	// Either the tool's result, or the reason the action failed. A variant, so a caller
	// cannot read a populated result next to a populated failure — the same reasoning
	// `object_resolver.h` gives for its own outcomes.
	//
	// `set_track_state` is not expressed this way, because it has no failure shape
	// outside its own result. See the file header.
	template <typename Result>
	using track_state_outcome = std::variant<Result, action_error>;

	// ---------------------------------------------------------------------------
	// Reading a track list
	// ---------------------------------------------------------------------------

	namespace detail
	{
		// A string bounded to what a schema accepts. Truncated rather than dropped: a
		// name too long for the contract is still the name of a track the producer is
		// looking at.
		inline std::string bounded_track_state_string(const std::string& text, std::size_t maximum_length)
		{
			if (text.size() <= maximum_length)
			{
				return text;
			}

			return text.substr(0, maximum_length);
		}

		// The framework's shape for "this went wrong", from one reason.
		//
		// A single failed action, which is the only shape that can carry a reason:
		// `handler_success` has nowhere to put one, and requirement 9.5 says the reason
		// is not optional.
		inline handler_action_outcomes failed_track_state_action(
			std::string_view target,
			const action_error& reason)
		{
			handler_action_outcomes outcomes;
			outcomes.actions.push_back(failed_action(target, reason.code(), reason.message()));

			return outcomes;
		}

		inline action_error unreadable_track_state_input_error(std::string_view tool_name)
		{
			return action_error{
				track_state_input_unreadable_code,
				"the input for " + std::string{tool_name} + " could not be read into the shape the "
					"handler expects, so nothing was attempted"};
		}

		// `set_track_state`'s result as the framework shape it always takes, with the undo
		// report vetoed when no change landed.
		//
		// Every path through the handler comes through here, including the outright
		// failures, so "nothing landed means no marker" is one line rather than a rule each
		// return statement has to remember. The framework cannot work it out for itself:
		// requirement 4.4 keeps `fields` opaque, so nothing outside this file can count the
		// outcomes inside the payload.
		template <typename JsonValue, typename ResultWriter>
		handler_success<JsonValue> reported_track_state_result(
			const set_track_state_result& result,
			ResultWriter write_result)
		{
			handler_success<JsonValue> produced;
			produced.nothing_was_applied = !result.any_change_landed();
			produced.fields = write_result(result);

			return produced;
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
		bool read_track_state_request(
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
	}

	// Every handler's first check. A host missing a function reports an empty project
	// and drops a write on the floor, so proceeding would tell the producer a change
	// landed when nothing did — and `is_usable` exists precisely so that is reportable.
	inline std::optional<action_error> check_track_state_host_usable(const track_state_host& host)
	{
		if (host.is_usable())
		{
			return std::nullopt;
		}

		std::string message{
			"REAPER did not supply the functions the track state tools need, so no track state "
			"operation can run"};

		const std::vector<std::string> missing = host.unresolved_function_names();

		if (!missing.empty())
		{
			message += " — missing: ";

			for (std::size_t position = 0; position < missing.size(); ++position)
			{
				if (position > 0)
				{
					message += ", ";
				}

				message += missing[position];
			}
		}

		return action_error{track_state_host_unavailable_code, message};
	}

	// Where a GUID sits in a reading, or nothing when it is not there.
	inline std::optional<std::size_t> index_of_track_state(
		const std::vector<track_state_reading>& tracks,
		std::string_view guid)
	{
		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			if (tracks[index].guid == guid)
			{
				return index;
			}
		}

		return std::nullopt;
	}

	inline action_error unknown_track_state_error(std::string_view guid)
	{
		return action_error{
			track_state_unknown_track_code,
			"no track in the project carries the GUID " + std::string{guid}
				+ ", so there was nothing to read or change"};
	}

	// ---------------------------------------------------------------------------
	// Structural roles, derived once and not twice
	// ---------------------------------------------------------------------------

	// The role signals for every track in a reading.
	//
	// Delegates to the Project Context Builder's derivation rather than repeating it,
	// exactly as `routing_tools.h` does for `get_routing`. `list_tracks` reports the
	// same five roles the project context snapshot reports, and deriving them a second
	// way here is how the two come to disagree about whether a producer's folder parent
	// sums — which the agent would then relay to the producer as a statement about
	// their own session.
	//
	// `has_fx` comes from `fx_count` rather than from `fx_names`, so a call that asked
	// for the chain to be left out gets the same role as one that did not.
	inline std::vector<context::StructuralRoleSignals> track_state_role_signals(
		const std::vector<track_state_reading>& tracks_in_project_order)
	{
		std::vector<context::TrackReading> readings;
		readings.reserve(tracks_in_project_order.size());

		for (const track_state_reading& track : tracks_in_project_order)
		{
			context::TrackReading reading;
			reading.guid = track.guid;
			reading.name = track.name;
			reading.folder_depth_delta = track.folder_depth_delta;
			reading.parent_send_enabled = track.parent_send_enabled;
			reading.item_count = track.item_count;
			reading.has_fx = track.fx_count > 0;
			reading.receive_count = track.receive_count;

			readings.push_back(std::move(reading));
		}

		return context::derive_track_role_signals(readings);
	}

	// ---------------------------------------------------------------------------
	// list_tracks
	// ---------------------------------------------------------------------------

	// Which tracks the call asked about, as positions in the project order read.
	//
	// No selectors means every track, in project order. Otherwise the tracks named, in
	// the order they were named — which the schema states explicitly, because the agent
	// asked about them in some order and matching its own question is more useful than
	// REAPER's.
	//
	// A track named twice appears once, at the position it was first named. Two
	// selectors resolving to one track is an ordinary consequence of learned
	// vocabulary — "kick" and "the kick" are both names for it — and reporting the same
	// state twice says nothing the first entry did not.
	struct track_state_selection
	{
		std::vector<std::size_t> positions;

		// The first GUID the call named that is not in the project, if any. Reported as
		// a failed action rather than skipped: a read that quietly returned four of
		// five tracks would have the agent reasoning about a track it never saw.
		std::string unknown_guid;
	};

	inline track_state_selection select_tracks_to_report(
		const std::vector<track_state_reading>& tracks_in_project_order,
		const std::vector<std::string>& requested_guids)
	{
		track_state_selection selection;

		if (requested_guids.empty())
		{
			selection.positions.reserve(tracks_in_project_order.size());

			for (std::size_t index = 0; index < tracks_in_project_order.size(); ++index)
			{
				selection.positions.push_back(index);
			}

			return selection;
		}

		selection.positions.reserve(requested_guids.size());

		for (const std::string& guid : requested_guids)
		{
			const std::optional<std::size_t> position =
				index_of_track_state(tracks_in_project_order, guid);

			if (!position.has_value())
			{
				if (selection.unknown_guid.empty())
				{
					selection.unknown_guid = guid;
				}

				continue;
			}

			const bool already_selected = std::find(
				selection.positions.begin(),
				selection.positions.end(),
				*position) != selection.positions.end();

			if (already_selected)
			{
				continue;
			}

			selection.positions.push_back(*position);
		}

		return selection;
	}

	// One track's reading as the schema reports it.
	inline reported_track_state report_track_state(
		const track_state_reading& track,
		std::size_t project_order_index,
		const context::StructuralRoleSignals& signals,
		std::vector<std::string> aliases,
		bool include_fx_chain)
	{
		reported_track_state reported;
		reported.guid = track.guid;
		reported.index = static_cast<int>(project_order_index);
		reported.name =
			detail::bounded_track_state_string(track.name, maximum_reported_track_name_length);
		reported.aliases = std::move(aliases);

		const context::StructuralRoleDerivation derivation = context::derive_structural_role(signals);
		reported.role = derivation.role;
		reported.role_signals = derivation.signals;

		// One conversion, shared with the Project Context Builder. See the file header.
		const context::TrackLevels levels =
			context::converted_track_levels(track.reaper_volume, track.reaper_pan);
		reported.volume_decibels = levels.volume_decibels;
		reported.pan_percent = levels.pan_percent;

		reported.muted = track.muted;
		reported.soloed = track.soloed;
		reported.armed = track.armed;
		reported.item_count = track.item_count;
		reported.color = track.color;

		if (include_fx_chain)
		{
			// The whole chain. A chain too long for the contract was rejected before this
			// ran — see `check_reported_fx_chain_lengths` — so there is nothing to cut
			// here.
			std::vector<std::string> chain;
			chain.reserve(track.fx_names.size());

			for (const std::string& fx_name : track.fx_names)
			{
				chain.push_back(detail::bounded_track_state_string(
					fx_name,
					maximum_reported_fx_chain_name_length));
			}

			reported.fx_chain = std::move(chain);
		}

		return reported;
	}

	// Requirement 23.9 for the FX chain, checked before anything is reported.
	//
	// `fxChain` has the same cap and the same silence as the track list: nothing in the
	// schema can say a chain was cut, so a cut chain is indistinguishable from a shorter
	// one and the agent would relay it to the producer as their chain. `list_track_fx`
	// reached the same conclusion about its own identical cap.
	//
	// The reason names a way out the producer's next call can take: `includeFxChain` is a
	// field of this call, so the rest of the session's state is still readable without
	// the chain that does not fit.
	inline std::optional<action_error> check_reported_fx_chain_lengths(
		const std::vector<track_state_reading>& tracks,
		const std::vector<std::size_t>& positions_to_report)
	{
		for (const std::size_t position : positions_to_report)
		{
			const track_state_reading& track = tracks[position];

			if (track.fx_names.size() <= maximum_reported_fx_chain_names)
			{
				continue;
			}

			return action_error{
				track_fx_chain_too_long_to_report_code,
				"the FX chain on track " + track.guid + " holds "
					+ std::to_string(track.fx_names.size())
					+ " FX, and a result can carry at most "
					+ std::to_string(maximum_reported_fx_chain_names)
					+ " — the chain cannot be reported without leaving some out, which would be "
					  "indistinguishable from a shorter chain, so ask again without the FX chain "
					  "and read that one track's chain with list_track_fx"};
		}

		return std::nullopt;
	}

	// list_tracks.
	//
	// A read tool, so no undo block and no marker. The whole project is read whatever
	// the call asked about, because folder nesting accumulates across track order and a
	// role derived from a filtered list would be derived from a list with holes in it.
	//
	// Requirement 23.9 is answered by failing rather than capping: this output schema
	// has neither `totalInRange` nor `truncated`, so a cut list is indistinguishable
	// from a smaller project — see the file header.
	inline track_state_outcome<list_tracks_result> apply_list_tracks(
		track_state_host& host,
		const LearnedAliasLookup& learned_aliases,
		const list_tracks_request& request)
	{
		if (const std::optional<action_error> unusable = check_track_state_host_usable(host))
		{
			return *unusable;
		}

		const std::vector<track_state_reading> tracks =
			host.read_tracks_in_project_order(request.include_fx_chain);

		const track_state_selection selection = select_tracks_to_report(tracks, request.track_guids);

		if (!selection.unknown_guid.empty())
		{
			return unknown_track_state_error(selection.unknown_guid);
		}

		if (selection.positions.size() > maximum_reported_track_states)
		{
			return action_error{
				track_list_too_long_to_report_code,
				"this call asks about " + std::to_string(selection.positions.size())
					+ " tracks, and a result can carry at most "
					+ std::to_string(maximum_reported_track_states)
					+ " — the tracks cannot be reported without leaving some out, which would be "
					  "indistinguishable from a smaller session, so name the tracks to read instead"};
		}

		if (request.include_fx_chain)
		{
			if (const std::optional<action_error> chain_too_long =
					check_reported_fx_chain_lengths(tracks, selection.positions))
			{
				return *chain_too_long;
			}
		}

		// Derived over the whole project order, then indexed by position.
		const std::vector<context::StructuralRoleSignals> signals = track_state_role_signals(tracks);

		list_tracks_result result;
		result.tracks.reserve(selection.positions.size());

		for (const std::size_t position : selection.positions)
		{
			const track_state_reading& track = tracks[position];

			// Keyed on the whole `ResolvableTrack`, which is the seam
			// `StoredAliasLookup` implements: the extension keys on REAPER's handle and
			// a synthetic list keys on the GUID, and neither has to pretend to be the
			// other.
			ResolvableTrack resolvable;
			resolvable.guid = track.guid;
			resolvable.name = track.name;
			resolvable.track = track.track;
			resolvable.project_index = static_cast<int>(position);

			std::vector<std::string> aliases = learned_aliases.learned_aliases_for_track(resolvable);

			// Requirement 8.5's cap governs what the store writes, and
			// `decode_alias_list` deliberately reports a hand-edited value as it
			// actually is. The schema caps the array at the same sixteen, so a value
			// that arrived from outside Sesh is bounded here rather than failing the
			// outbound validation.
			if (aliases.size() > maximum_aliases_per_track)
			{
				aliases.resize(maximum_aliases_per_track);
			}

			for (std::string& alias : aliases)
			{
				alias = detail::bounded_track_state_string(alias, maximum_alias_length);
			}

			result.tracks.push_back(report_track_state(
				track,
				position,
				signals[position],
				std::move(aliases),
				request.include_fx_chain));
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// get_project_summary
	// ---------------------------------------------------------------------------

	// get_project_summary.
	//
	// Project-level only, in both directions. Every field of the output schema is
	// required, because a summary with a field missing is indistinguishable from a
	// session that has no tempo and the agent would have to guess which — so an
	// unreadable project is a failed action rather than a summary with defaults in it.
	inline track_state_outcome<project_summary_result> apply_get_project_summary(
		track_state_host& host,
		const get_project_summary_request&)
	{
		if (const std::optional<action_error> unusable = check_track_state_host_usable(host))
		{
			return *unusable;
		}

		const project_summary_reading reading = host.read_project_summary();

		if (!reading.readable)
		{
			return action_error{
				project_summary_unreadable_code,
				"REAPER did not report the session's globals, so there is no summary to give — an "
				"answer assembled from defaults would be indistinguishable from a session that "
				"really is at 120 bpm with nothing in it"};
		}

		project_summary_result result;
		result.project_name = detail::bounded_track_state_string(
			reading.project_name,
			maximum_reported_track_name_length);
		result.tempo = reading.tempo;
		result.time_signature_numerator = reading.time_signature_numerator;
		result.time_signature_denominator = reading.time_signature_denominator;
		result.sample_rate = reading.sample_rate;
		result.track_count = reading.track_count;
		result.project_length = reading.project_length;
		result.marker_count = reading.marker_count;
		result.region_count = reading.region_count;
		result.tempo_change_count = reading.tempo_change_count;

		return result;
	}

	// ---------------------------------------------------------------------------
	// set_track_state
	// ---------------------------------------------------------------------------

	// A result carrying one failed action and nothing else.
	//
	// The shape every outright failure of this tool takes, because its output schema
	// requires `actions` and `aliasesLearned` together and the framework's partial
	// carries only the first — see the file header. `aliasesLearned` is left empty,
	// which the schema requires to be present and says means no alias was set.
	inline set_track_state_result set_track_state_failure(
		std::string_view target,
		const action_error& reason)
	{
		set_track_state_result result;
		result.actions.push_back(failed_action(target, reason.code(), reason.message()));

		return result;
	}

	// The failed action an alias write outcome produces, or nothing when it did not
	// fail.
	//
	// Four outcomes and four reasons, because they ask the producer for four different
	// things. `already_held` is not here: the track answers to the name, which is what
	// was asked for, and it is an applied action that simply wrote nothing.
	inline std::optional<action_error> alias_write_failure(const LearnedAlias& learned)
	{
		switch (learned.outcome)
		{
			case AliasWriteOutcome::learned:
			case AliasWriteOutcome::already_held:
			case AliasWriteOutcome::forgotten:
				return std::nullopt;

			case AliasWriteOutcome::rejected_cap_reached:
				// The store's own reason, which names the cap and says nothing was
				// dropped to make room (requirement 8.6). Restating it here would be a
				// second place the number could drift from the one enforced.
				return action_error{
					track_alias_cap_reached_code,
					learned.reason.empty() ? per_track_alias_cap_reason() : learned.reason};

			case AliasWriteOutcome::rejected_empty:
				return action_error{
					track_alias_empty_code,
					"the alias to teach was empty or only whitespace, so there was no name to "
					"learn — and it is not read as an instruction to forget the ones the track "
					"already answers to"};

			case AliasWriteOutcome::rejected_too_long:
				return action_error{
					track_alias_too_long_code,
					"the alias to teach is longer than the " + std::to_string(maximum_alias_length)
						+ " bytes a stored alias can hold, and half an alias is a different alias "
						  "that would resolve to the wrong track quietly"};

			case AliasWriteOutcome::storage_refused:
				return action_error{
					track_alias_write_failed_code,
					"REAPER would not write the alias onto the track, so the producer's name for it "
					"was not learned"};
		}

		return action_error{
			track_alias_write_failed_code,
			"the alias write reported an outcome this build does not recognise, so whether the "
			"name was learned is unknown"};
	}

	// The failed action a property write produces, naming which property REAPER would
	// not accept.
	inline action_error track_state_write_failure(std::string_view property, std::string_view guid)
	{
		return action_error{
			track_state_write_failed_code,
			"REAPER would not accept the new " + std::string{property} + " for track "
				+ std::string{guid} + ", so that change did not land"};
	}

	// set_track_state.
	//
	// One entry at a time, in the order the call supplied them, and always a result
	// rather than the framework's partial — see the file header.
	//
	// Within one entry the named properties are written in turn and the first refusal
	// ends that entry: the writes already made stay, because requirement 9.4's
	// no-rollback rule applies inside an entry as much as across the array, and nothing
	// further in the entry is attempted, because reporting an alias as learned for an
	// entry whose fader move REAPER refused would claim more than happened. The alias
	// is written last for that reason.
	//
	// Across entries a failure changes nothing: a sibling's refusal does not undo what
	// landed, and every failure carries its own reason (requirements 9.4 and 9.5).
	inline set_track_state_result apply_set_track_state(
		track_state_host& host,
		AliasStore& alias_store,
		const set_track_state_request& request)
	{
		if (const std::optional<action_error> unusable = check_track_state_host_usable(host))
		{
			return set_track_state_failure(set_track_state_tool_name, *unusable);
		}

		if (request.changes.empty())
		{
			// The input schema's `minItems` should have stopped this. Reported rather
			// than returning a result whose `actions` array is empty, which the output
			// schema's own `minItems` forbids.
			return set_track_state_failure(
				set_track_state_tool_name,
				action_error{
					track_state_names_no_change_code,
					"the call named no tracks to change, so there was nothing to do"});
		}

		if (request.changes.size() > maximum_track_state_changes)
		{
			// Checked before anything is written, so a call over the cap changes
			// nothing. An outcome array cut to fit would be changes that happened with
			// nothing said about them.
			return set_track_state_failure(
				set_track_state_tool_name,
				action_error{
					track_state_too_many_changes_code,
					"this call carries " + std::to_string(request.changes.size())
						+ " changes and a result can report the outcome of at most "
						+ std::to_string(maximum_track_state_changes)
						+ " — nothing was changed, since an outcome list cut to fit would be "
						  "changes that happened with nothing said about them"});
		}

		// Requirement 8.10, sampled before any write in the call. Writing an alias
		// dirties the project and so does writing a fader, so a save state read after
		// either would report "unsaved" for every call that changed anything and say
		// nothing about the session the producer is in. See the file header.
		const bool project_saved_before_any_write = alias_store.project_saved();

		const std::vector<track_state_reading> tracks_before = host.read_tracks_in_project_order(false);

		set_track_state_result result;
		result.actions.reserve(request.changes.size());

		bool any_alias_was_learned = false;

		for (const track_state_change& change : request.changes)
		{
			const std::optional<std::size_t> position =
				index_of_track_state(tracks_before, change.track_guid);

			if (!position.has_value())
			{
				const action_error reason = unknown_track_state_error(change.track_guid);
				result.actions.push_back(
					failed_action(change.track_guid, reason.code(), reason.message()));

				continue;
			}

			if (!change.names_a_change())
			{
				result.actions.push_back(failed_action(
					change.track_guid,
					track_state_names_no_change_code,
					"this entry named a track and nothing to change about it, so there was nothing "
					"to apply"));

				continue;
			}

			const track_state_reading& track = tracks_before[*position];

			std::optional<action_error> failure;

			if (!failure.has_value() && change.name.has_value())
			{
				if (!host.write_track_name(change.track_guid, *change.name))
				{
					failure = track_state_write_failure("name", change.track_guid);
				}
			}

			if (!failure.has_value() && change.volume_decibels.has_value())
			{
				// The decibels the producer said, converted to the linear gain factor
				// REAPER stores. Writing the decibels straight through would have
				// REAPER read -6 dB as a gain of six.
				if (!host.write_track_volume(
						change.track_guid,
						reaper_volume_from_volume_decibels(*change.volume_decibels)))
				{
					failure = track_state_write_failure("volume", change.track_guid);
				}
			}

			if (!failure.has_value() && change.pan_percent.has_value())
			{
				if (!host.write_track_pan(
						change.track_guid,
						reaper_pan_from_pan_percent(*change.pan_percent)))
				{
					failure = track_state_write_failure("pan position", change.track_guid);
				}
			}

			if (!failure.has_value() && change.muted.has_value())
			{
				if (!host.write_track_muted(change.track_guid, *change.muted))
				{
					failure = track_state_write_failure("mute state", change.track_guid);
				}
			}

			if (!failure.has_value() && change.soloed.has_value())
			{
				if (!host.write_track_soloed(change.track_guid, *change.soloed))
				{
					failure = track_state_write_failure("solo state", change.track_guid);
				}
			}

			if (!failure.has_value() && change.armed.has_value())
			{
				if (!host.write_track_armed(change.track_guid, *change.armed))
				{
					failure = track_state_write_failure("record arm state", change.track_guid);
				}
			}

			if (!failure.has_value() && change.color.has_value())
			{
				if (!host.write_track_color(change.track_guid, *change.color))
				{
					failure = track_state_write_failure("colour", change.track_guid);
				}
			}

			// Last, so an entry whose earlier writes REAPER refused does not report an
			// alias learned beside a failed action.
			if (!failure.has_value() && change.alias.has_value())
			{
				const LearnedAlias learned =
					alias_store.learn_track_alias(static_cast<TrackHandle>(track.track), *change.alias);

				failure = alias_write_failure(learned);

				if (!failure.has_value() && learned.learned())
				{
					any_alias_was_learned = true;

					if (result.aliases_learned.size() < maximum_track_state_changes)
					{
						result.aliases_learned.push_back(TrackReference{
							track.guid,
							detail::bounded_track_state_string(
								track.name,
								maximum_reported_track_name_length)});
					}
				}
			}

			if (failure.has_value())
			{
				result.actions.push_back(
					failed_action(change.track_guid, failure->code(), failure->message()));

				continue;
			}

			result.actions.push_back(succeeded_action(change.track_guid));
		}

		if (any_alias_was_learned)
		{
			result.project_saved = project_saved_before_any_write;
		}

		return result;
	}

	// ---------------------------------------------------------------------------
	// Registration
	//
	// Through `tool_executor.h`'s seam: `register_mutating_tool` for `set_track_state`,
	// `register_read_tool` for the two reads. No precondition check on the mutating
	// tool, because it has nothing to refuse with — see the file header.
	//
	// Templated on the payload type, on how a request is read out of the already
	// validated input, and on how a result becomes a payload, following `fx_tools.h`
	// and `item_tools.h`. The framework holds the input as an opaque pointer it never
	// dereferences (requirement 4.4), so reading a field is the codec's job and not
	// this file's; injecting both directions is what lets the suite drive all three
	// handlers with no JSON library present.
	//
	// `RequestReader` is called as `read(input, request) -> bool`, overloaded per
	// request type. False means the input could not be read, which becomes a failed
	// outcome rather than a crash — the server validated the input, so this is the belt
	// to that braces.
	//
	// `ResultWriter` is called as `write(result) -> JsonValue`, overloaded per result
	// type.
	// ---------------------------------------------------------------------------

	// What one registration did, so a caller can report a gap at startup rather than
	// discovering it one unknown-tool error at a time while a producer waits.
	struct track_state_tool_registration
	{
		std::string tool_name;
		tool_registration_outcome outcome = tool_registration_outcome::registered;
	};

	// Registers all three. The returned vector holds one entry per tool in registration
	// order.
	//
	// `host`, `alias_store`, `learned_aliases`, and the codec callables must outlive
	// the registry, which outlives one dispatch. The extension constructs them at
	// startup and the suite on the stack; none is captured by value, because a track
	// state host copied into three closures would be three views of one project.
	template <typename JsonValue, typename RequestReader, typename ResultWriter>
	std::vector<track_state_tool_registration> register_track_state_tools(
		tool_handler_registry<JsonValue>& registry,
		track_state_host& host,
		AliasStore& alias_store,
		const LearnedAliasLookup& learned_aliases,
		RequestReader read_request,
		ResultWriter write_result)
	{
		std::vector<track_state_tool_registration> registrations;

		// The one mutating tool, and the one whose payload always carries its per-entry
		// outcomes: its output schema requires `actions` and `aliasesLearned` together,
		// so a framework partial would drop `aliasesLearned`.
		registrations.push_back(track_state_tool_registration{
			std::string{set_track_state_tool_name},
			registry.register_mutating_tool(
				set_track_state_tool_name,
				[&host, &alias_store, read_request, write_result](
					const tool_execution_context<JsonValue>& context)
					-> mutating_handler_result<JsonValue> {
					set_track_state_request request;

					if (!detail::read_track_state_request(context, read_request, request))
					{
						return detail::reported_track_state_result<JsonValue>(
							set_track_state_failure(
								set_track_state_tool_name,
								detail::unreadable_track_state_input_error(set_track_state_tool_name)),
							write_result);
					}

					return detail::reported_track_state_result<JsonValue>(
						apply_set_track_state(host, alias_store, request),
						write_result);
				})});

		// The two reads. `register_read_tool`, so no undo block is opened and no marker
		// is captured (requirement 10.3). A read tool's body *may* refuse, because
		// nothing was opened for it — but neither of these does, for the reason in the
		// file header.
		registrations.push_back(track_state_tool_registration{
			std::string{list_tracks_tool_name},
			registry.register_read_tool(
				list_tracks_tool_name,
				[&host, &learned_aliases, read_request, write_result](
					const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					list_tracks_request request;

					if (!detail::read_track_state_request(context, read_request, request))
					{
						return detail::failed_track_state_action(
							list_tracks_tool_name,
							detail::unreadable_track_state_input_error(list_tracks_tool_name));
					}

					track_state_outcome<list_tracks_result> outcome =
						apply_list_tracks(host, learned_aliases, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_track_state_action(list_tracks_tool_name, *reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<list_tracks_result>(outcome))};
				})});

		registrations.push_back(track_state_tool_registration{
			std::string{get_project_summary_tool_name},
			registry.register_read_tool(
				get_project_summary_tool_name,
				[&host, read_request, write_result](const tool_execution_context<JsonValue>& context)
					-> non_mutating_handler_result<JsonValue> {
					get_project_summary_request request;

					if (!detail::read_track_state_request(context, read_request, request))
					{
						return detail::failed_track_state_action(
							get_project_summary_tool_name,
							detail::unreadable_track_state_input_error(
								get_project_summary_tool_name));
					}

					track_state_outcome<project_summary_result> outcome =
						apply_get_project_summary(host, request);

					if (const action_error* const reason = std::get_if<action_error>(&outcome))
					{
						return detail::failed_track_state_action(
							get_project_summary_tool_name,
							*reason);
					}

					return handler_success<JsonValue>{
						write_result(std::get<project_summary_result>(outcome))};
				})});

		return registrations;
	}

	// How many tools `register_track_state_tools` registers, so a caller checking
	// completeness does not have to count the blocks above.
	inline constexpr std::size_t track_state_tool_count = 3;

	// Whether every registration took. False names a gap the extension can report at
	// startup, rather than discovering it one unknown-tool error at a time while a
	// producer waits.
	inline bool every_track_state_tool_registered(
		const std::vector<track_state_tool_registration>& registrations)
	{
		if (registrations.size() != track_state_tool_count)
		{
			return false;
		}

		for (const track_state_tool_registration& registration : registrations)
		{
			if (registration.outcome != tool_registration_outcome::registered)
			{
				return false;
			}
		}

		return true;
	}
}

#endif  // SESH_AI_DAW_TOOLS_TRACK_STATE_TOOLS_H
