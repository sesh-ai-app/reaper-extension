// The Project Context Builder — the slim session summary, and the discipline that
// keeps it slim.
//
// This is the only payload in the protocol that recurs on every turn. It cannot be
// prompt-cached, because it changes; and it competes for context window space with
// the system prompt, 42 tool schemas, and the whole conversation history. The design
// records where that lesson came from: in a comparable MCP-for-desktop-app project,
// large per-turn payloads were truncated by the AI system before reaching the model,
// which degrades accuracy in a way that is invisible from either end. Summary
// upfront, detail on demand.
//
// So the governing rule of this file is a negative one, and it is requirement 6.2.
// Per-track volume, pan, muted, soloed, armed, alias, color, FX chain, sends,
// receives, items, and item count are **not** in the snapshot, and neither are
// folderDepth or parentSendEnabled. Every one of them is reachable through a read
// tool — `list_tracks`, `list_track_fx`, `get_routing`, `list_selected_items` — and
// the agent asks when the prompt needs it. `project-context.schema.json` permits all
// of them, which is exactly why the rule has to live here: the schema is a ceiling,
// not a shopping list, and "it is useful and the schema allows it" is the argument
// that ends with a truncated payload. `track_properties_absent_from_the_snapshot`
// below names all fourteen so that the rule is code a test can check rather than a
// paragraph a reader has to remember.
//
// What is in it: project globals, per-track `guid`/`index`/`name`/`role`,
// `selectedTrackGuids`, the master track's `guid`/`name`/`role`, and counts for
// markers, regions, and tempo map entries. `index` is there because the schema
// requires it and because a producer looking at REAPER's track numbers deserves the
// agent to be able to say "track 7"; it is never how a tool targets a track.
//
// ---------------------------------------------------------------------------
// The three derivations, and where each one lands
//
// The design names three pieces of work as the extension's own rather than reads,
// and only one of them appears in the snapshot. Worth being explicit, because two of
// the three look missing otherwise:
//
//   - **Structural role** per track (requirement 6.6). In the snapshot. Derived by
//     `context/structural_role.h` from signals this file collects, and reported with
//     the raw signals kept alongside on `SnapshotTrack::role_signals` so a
//     misclassification is diagnosable from `describe_snapshot_role_derivations`
//     rather than invisible. The signals are deliberately not serialised: they are
//     `folderDepth`, `parentSendEnabled`, and `itemCount` under other names, and
//     requirement 6.2 excludes those.
//   - **Folder parentage** accumulated from per-track depth deltas across track
//     order (requirement 6.5). Not in the snapshot — it is what `get_routing`
//     returns. It is computed here because role derivation needs it: a track's
//     nesting level, how many tracks sit inside its folder, and how many of those
//     feed it are three of the five signals.
//   - **Unit conversion**, volume to decibels and pan to a percentage
//     (requirement 6.7). Not in the snapshot, because requirement 6.2 excludes
//     volume and pan. `converted_track_levels` is the entry point, delegating to
//     `daw/unit_conversion.h`, and it is here rather than in the read tools so that
//     `list_tracks` and `get_project_summary` share one answer with this component
//     instead of each deriving their own.
//
// A consequence worth stating: `ProjectContextSource` never reads a track's fader or
// pan position. Two REAPER calls per track, every debounce interval, for values the
// snapshot then discards, is a cost paid on REAPER's main thread for nothing.
//
// ---------------------------------------------------------------------------
// Folder depth accumulation: reconciled onto one implementation
//
// Three components accumulate `I_FOLDERDEPTH` deltas, and the two that landed before
// this one each flagged the duplication as work for when the Context Builder exists.
// This is that moment, and what was done is:
//
// **This component adds no fourth accumulation.** Parentage comes from
// `daw::reconstruct_folder_paths` in `daw/folder_invariant_keeper.h`, which is the
// canonical arithmetic — it is the implementation pinned by the server's own
// property tests in `folder-invariant.property.test.js`, so it is the one with an
// external contract behind it. Child counts, feeding-child counts, and nesting
// levels all fall out of the folder paths it returns.
//
// **The other two are left alone, on purpose.** `daw/cycle_detector.h` accumulates
// over its own `track_node` and returns an optional parent index per track;
// `daw/reaper_render_host.cpp` accumulates inline while walking the REAPER API,
// because it has no plain track list to hand. Collapsing either onto the Keeper is a
// mechanical change and a small one, but both files are inside work that is in
// flight — the cycle detector is being wired into the routing tools (task 10.5) and
// the render host is the component that would instead ask this builder for roles.
// Editing them from here would be editing around another change. The reconciliation
// is therefore: one canonical implementation now exists and is used by the newest
// caller, and the two older callers adopt it as their own tasks touch them. All
// three are read-only and cannot disagree about a given project, so nothing is
// broken while that happens.
//
// Note also what this component does *not* do with the deltas: it does not repair
// them. `repair_folder_depth_deltas` is requirement 11's, applied after a structural
// edit. A snapshot reports the structure REAPER actually has, and a read that
// quietly tidied it would have the agent reasoning about a project the producer is
// not looking at. Malformed deltas are absorbed instead —
// `reconstruct_folder_paths` closes only folders that are open, and
// `normalize_structural_role_signals` clamps what reaches the derivation.
//
// ---------------------------------------------------------------------------
// Debouncing (requirement 6.4)
//
// REAPER raises no project-changed event, so "when the project changes" is polling:
// `GetProjectStateChangeCount` read on the main-thread timer. A producer dragging a
// fader moves that counter on every frame, and a snapshot per frame is both a flood
// the server does not want and a REAPER main thread doing schema work at 30 Hz.
//
// `ProjectChangeDebouncer` is a trailing-edge debounce over tick counts: a change
// arms it, each further change re-arms it, and the snapshot is built once the
// counter has held still for `quiet_ticks`. Ticks rather than a clock, so the
// component has no time dependency and the suite drives it exactly.
//
// There is deliberately no maximum interval forcing a snapshot out during a long
// continuous edit. It would be the obvious safety valve and it is the wrong one: the
// mid-drag states are precisely what the agent does not need, and a producer who
// edits continuously for thirty seconds is not prompting during those thirty
// seconds. When the agent does need current state it asks, and
// `request:project_context` is answered on the next tick.
//
// ---------------------------------------------------------------------------
// Validating the snapshot (requirement 6.8)
//
// Every snapshot validates against the vendored `project-context.schema.json`, and
// the check runs once, in the Envelope Codec, on the network thread. That is not a
// gap in this file — it is where requirement 4.1 and requirement 22.2 put it.
// `schema_validator.h` binds both `state:project_context` and
// `response:project_context` to that schema, so a snapshot that fails is refused
// before it is serialised, with the schema path and failures named, and the assertion
// at the bottom of this header is what holds the two envelope types this component
// emits against that table.
//
// What this file owes in return is that the check passes, which it earns by
// construction rather than by hope. `build_project_context_snapshot` is total: every
// number it emits is finite and inside the schema's stated range, every string is
// truncated to the schema's `maxLength`, and anything it cannot make representable is
// omitted rather than sent malformed. The two omissions worth knowing about:
//
//   - **A track whose GUID does not match the schema's pattern is dropped from the
//     array.** `guid` is required on a track, so a track with an unreadable one
//     cannot be represented — and one track missing from the snapshot is a far
//     better outcome than a snapshot the codec refuses, which is the whole session's
//     context lost rather than one track's. Surviving tracks keep their REAPER track
//     numbers; the array is not renumbered, because `index` means the number the
//     producer sees.
//   - **The master track is omitted entirely if its GUID is unreadable**, for the
//     same reason: `guid` is the one required property of `masterTrack`.
//
// Both are counted on `SnapshotPublication` so a snapshot that quietly lost a track
// says so.
//
// ---------------------------------------------------------------------------
// Shape of the file
//
// REAPER sits behind `ProjectContextSource`, two calls wide, in the arrangement
// `entry/timer_registration.h` established: the interface and this header are
// SDK-free, and `context/reaper_project_context_source.cpp` is the only translation
// unit that includes the SDK. The test target has no SDK include path, so that is
// structural rather than a rule to remember.
//
// Serialisation and the builder are templated on the JSON document type for the same
// reason the Envelope Codec is: the suite drives them with a document type it
// controls, and `project_context_builder.cpp` names them against `nlohmann::json` and
// the real envelope so the build proves the calls exist.

#ifndef SESH_AI_CONTEXT_PROJECT_CONTEXT_BUILDER_H
#define SESH_AI_CONTEXT_PROJECT_CONTEXT_BUILDER_H

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <context/structural_role.h>
#include <daw/folder_invariant_keeper.h>
#include <daw/unit_conversion.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

namespace sesh_ai::context
{
	// ---------------------------------------------------------------------------
	// Envelope types
	//
	// Spelled once so the Message Dispatcher (task 14.1) routes on the same constants
	// this component answers with. The static assertions at the bottom of this header
	// check the two outbound ones against the binding table in schema_validator.h.
	// ---------------------------------------------------------------------------

	// Unprompted: on connect, and when the debounce fires.
	inline constexpr std::string_view project_context_state_envelope_type{"state:project_context"};

	// The answer to a request, carrying the echoed requestId (requirement 6.9).
	inline constexpr std::string_view project_context_response_envelope_type{"response:project_context"};

	// The request itself. Payload is empty and has no bundled schema, which is why it
	// is absent from the inbound binding table.
	inline constexpr std::string_view project_context_request_envelope_type{"request:project_context"};

	// ---------------------------------------------------------------------------
	// Schema bounds
	//
	// Taken from project-context.schema.json, which is the contract this component is
	// obliged to satisfy. Stated as named constants rather than written inline so the
	// suite can sweep the boundaries against the same numbers the builder clamps to,
	// and so a schema change has one place to land.
	// ---------------------------------------------------------------------------

	inline constexpr double minimum_tempo = 1.0;
	inline constexpr double maximum_tempo = 960.0;

	inline constexpr int minimum_time_signature_numerator = 1;
	inline constexpr int maximum_time_signature_numerator = 64;

	// The schema's `denominator` is an enum, not a range: the note values REAPER can
	// represent. Anything else is snapped down to one of these — see
	// `nearest_representable_time_signature_denominator`.
	inline constexpr std::array<int, 7> representable_time_signature_denominators{1, 2, 4, 8, 16, 32, 64};

	inline constexpr int minimum_sample_rate = 8000;
	inline constexpr int maximum_sample_rate = 768000;

	// The schema puts a floor of zero on every position and length and no ceiling,
	// because a timeline has no natural one. A ceiling is still needed here, since
	// JSON has no infinity and nlohmann serialises a non-finite double as `null`,
	// which fails `"type": "number"` and costs the whole snapshot. A thousand million
	// seconds is thirty-one years of timeline — past anything a session reaches, and
	// finite, which is the property that matters.
	inline constexpr double minimum_timeline_seconds = 0.0;
	inline constexpr double maximum_timeline_seconds = 1.0e9;

	// maxLength on projectName, a track's name, and the master track's name.
	inline constexpr std::size_t maximum_name_length = 512;

	// maxItems on selectedTrackGuids.
	inline constexpr std::size_t maximum_selected_track_guids = 4096;

	// ---------------------------------------------------------------------------
	// Property names
	// ---------------------------------------------------------------------------

	// Every top-level property the slim snapshot writes. Two more are conditional —
	// `masterTrack`, and the `timeSelectionStart`/`timeSelectionEnd` pair — so this is
	// the always-present set rather than the whole vocabulary. Listed so the suite can
	// assert the serialised object writes these and nothing beyond the conditional
	// pair.
	inline constexpr std::array<std::string_view, 15> snapshot_property_names{
		"projectName",
		"tempo",
		"timeSignature",
		"sampleRate",
		"projectLength",
		"cursorPosition",
		"playState",
		"loopStart",
		"loopEnd",
		"loopEnabled",
		"tempoChangeCount",
		"markerCount",
		"regionCount",
		"tracks",
		"selectedTrackGuids"
	};

	inline constexpr std::string_view snapshot_master_track_property{"masterTrack"};
	inline constexpr std::string_view snapshot_time_selection_start_property{"timeSelectionStart"};
	inline constexpr std::string_view snapshot_time_selection_end_property{"timeSelectionEnd"};

	// The whole of a snapshot track.
	inline constexpr std::array<std::string_view, 4> snapshot_track_property_names{
		"guid",
		"index",
		"name",
		"role"
	};

	// The whole of the snapshot's master track.
	inline constexpr std::array<std::string_view, 3> snapshot_master_track_property_names{
		"guid",
		"name",
		"role"
	};

	// The rule of this component, as data.
	//
	// The first twelve are requirement 6.2's list, spelled as the schema spells them —
	// note `soloed` rather than `solo`, which is the schema's own participle and
	// matches `set_track_state`'s field, so a value read here is a value that can be
	// written back. The last two come from the design's per-track paragraph, which
	// enumerates `folderDepth` and `parentSendEnabled` alongside the rest as fetched
	// on demand; they are also the raw role signals under other names, and sending
	// them would put the evidence in the payload rather than in the log where
	// requirement 6.6 wants it.
	//
	// The suite asserts the serialised track object shares no key with this array,
	// which is the most direct form the rule can take: adding a field "because it is
	// useful" fails a test that names the requirement.
	inline constexpr std::array<std::string_view, 14> track_properties_absent_from_the_snapshot{
		"volume",
		"pan",
		"muted",
		"soloed",
		"armed",
		"alias",
		"color",
		"fxChain",
		"sends",
		"receives",
		"items",
		"itemCount",
		"folderDepth",
		"parentSendEnabled"
	};

	// The master track's excluded properties. The schema offers the same detail for it
	// as for a track and the design allows it none: a limiter on the mix is found with
	// `list_track_fx`, not in a payload that recurs every turn.
	inline constexpr std::array<std::string_view, 6> master_track_properties_absent_from_the_snapshot{
		"muted",
		"volume",
		"pan",
		"fxChain",
		"receives",
		"color"
	};

	// The two arrays the counts replace. `markerCount` and `regionCount` say whether
	// the arrangement is labelled at all; `list_markers` and `list_regions` say where.
	inline constexpr std::array<std::string_view, 2> snapshot_properties_replaced_by_counts{
		"markers",
		"regions"
	};

	// ---------------------------------------------------------------------------
	// Transport state
	// ---------------------------------------------------------------------------

	// The schema's `playState` enum, spelled as the schema spells it.
	enum class PlayState
	{
		stopped,
		playing,
		paused,
		recording
	};

	inline constexpr std::array<PlayState, 4> all_play_states{
		PlayState::stopped,
		PlayState::playing,
		PlayState::paused,
		PlayState::recording
	};

	// Total over the enum, so no fallback string can reach a snapshot and fail the
	// enum check with a value nobody wrote.
	constexpr std::string_view to_schema_string(PlayState play_state)
	{
		switch (play_state)
		{
			case PlayState::stopped:
				return "stopped";
			case PlayState::playing:
				return "playing";
			case PlayState::paused:
				return "paused";
			case PlayState::recording:
				return "recording";
		}

		// Unreachable for any enumerator. Present because a switch over an enum class
		// with no default is not a guarantee to the compiler that the value is one of
		// them.
		return "stopped";
	}

	// REAPER's play state is a bitfield: 1 playing, 2 paused, 4 recording. More than
	// one bit is normal — recording is reported as playing and recording together, and
	// pausing mid-take sets the paused bit without clearing the recording one.
	//
	// The order below resolves that, and the asymmetry is deliberate: recording wins
	// over paused. An agent told "paused" while a take is in progress might edit a
	// track mid-record, which costs the producer the take. An agent told "recording"
	// while the transport is actually paused holds off, which costs nothing. The
	// cheaper mistake is the one to make.
	inline constexpr int reaper_play_state_playing_bit = 1;
	inline constexpr int reaper_play_state_paused_bit = 2;
	inline constexpr int reaper_play_state_recording_bit = 4;

	constexpr PlayState play_state_from_reaper_play_state(int reaper_play_state)
	{
		if ((reaper_play_state & reaper_play_state_recording_bit) != 0)
		{
			return PlayState::recording;
		}

		if ((reaper_play_state & reaper_play_state_paused_bit) != 0)
		{
			return PlayState::paused;
		}

		if ((reaper_play_state & reaper_play_state_playing_bit) != 0)
		{
			return PlayState::playing;
		}

		return PlayState::stopped;
	}

	// ---------------------------------------------------------------------------
	// Bounds enforcement
	//
	// Every number the builder emits goes through one of these. They are total: a NaN,
	// an infinity, and an out-of-range value each produce something the schema
	// accepts, because the alternative is a payload the codec refuses and a turn with
	// no session context at all.
	//
	// NaN is handled explicitly rather than left to fall through a comparison chain.
	// `daw::detail::clamp_to_range` returns a NaN unchanged by design, with each of
	// its callers dealing with the case first; the same discipline applies here, and
	// the fallbacks below are chosen per field rather than shared.
	// ---------------------------------------------------------------------------

	namespace detail
	{
		constexpr double clamp_finite(double value, double lowest, double highest, double fallback)
		{
			if (!(value == value))
			{
				return fallback;
			}

			if (value < lowest)
			{
				return lowest;
			}

			if (value > highest)
			{
				return highest;
			}

			return value;
		}

		constexpr int clamp_integer(int value, int lowest, int highest)
		{
			if (value < lowest)
			{
				return lowest;
			}

			if (value > highest)
			{
				return highest;
			}

			return value;
		}

		constexpr int clamp_count(int value)
		{
			return value < 0 ? 0 : value;
		}
	}

	// A position or length in seconds. A NaN reads as zero — the read failed, and zero
	// is the one value that is certainly in range and visibly neutral.
	constexpr double bounded_timeline_seconds(double seconds)
	{
		return detail::clamp_finite(
			seconds,
			minimum_timeline_seconds,
			maximum_timeline_seconds,
			minimum_timeline_seconds
		);
	}

	// Tempo, clamped into the schema's range.
	//
	// A NaN falls to the schema minimum of 1 BPM rather than to a plausible 120. The
	// reasoning is the same as the decibel floor's in `unit_conversion.h`, applied to
	// a different failure: a tempo read that failed should produce a number a producer
	// notices, not one that looks like a real session. 1 BPM is wrong in a way
	// somebody reports; 120 is wrong in a way nobody ever finds.
	constexpr double bounded_tempo(double tempo)
	{
		return detail::clamp_finite(tempo, minimum_tempo, maximum_tempo, minimum_tempo);
	}

	// The largest representable denominator not greater than the value, with anything
	// below the smallest snapping up to 1.
	//
	// Snapping down rather than to the nearest keeps the note value at least as long
	// as what was read, so a 5/8 bar — not representable in REAPER's own UI, and so
	// only reachable from a corrupt read — reports as 4 rather than 8. The direction
	// barely matters; being total does.
	constexpr int nearest_representable_time_signature_denominator(int denominator)
	{
		int best = representable_time_signature_denominators.front();

		for (const int candidate : representable_time_signature_denominators)
		{
			if (candidate <= denominator && candidate > best)
			{
				best = candidate;
			}
		}

		return best;
	}

	constexpr int bounded_time_signature_numerator(int numerator)
	{
		return detail::clamp_integer(
			numerator,
			minimum_time_signature_numerator,
			maximum_time_signature_numerator
		);
	}

	constexpr int bounded_sample_rate(int sample_rate)
	{
		return detail::clamp_integer(sample_rate, minimum_sample_rate, maximum_sample_rate);
	}

	// ---------------------------------------------------------------------------
	// GUIDs
	// ---------------------------------------------------------------------------

	// The schema's `guid` pattern: `^\{[0-9A-Fa-f-]{36}\}$` — REAPER's own braced
	// form. Checked rather than assumed, because `guid` is required wherever it
	// appears, so an unreadable one is the difference between dropping a track and
	// losing the snapshot.
	//
	// The pattern is deliberately as loose as the schema's: 36 characters of hex
	// digits and hyphens between braces, with no check on where the hyphens fall. A
	// stricter test here would refuse GUIDs the server accepts.
	inline constexpr std::size_t guid_body_length = 36;

	constexpr bool is_schema_shaped_guid(std::string_view guid)
	{
		if (guid.size() != guid_body_length + 2)
		{
			return false;
		}

		if (guid.front() != '{' || guid.back() != '}')
		{
			return false;
		}

		for (std::size_t position = 1; position <= guid_body_length; ++position)
		{
			const char character = guid[position];

			const bool is_hexadecimal_digit = (character >= '0' && character <= '9')
				|| (character >= 'a' && character <= 'f')
				|| (character >= 'A' && character <= 'F');

			if (!is_hexadecimal_digit && character != '-')
			{
				return false;
			}
		}

		return true;
	}

	// ---------------------------------------------------------------------------
	// Unit conversion (requirement 6.7)
	// ---------------------------------------------------------------------------

	// A track's levels in the units the schemas report. Not part of the snapshot —
	// requirement 6.2 excludes volume and pan — and not read from REAPER by
	// `ProjectContextSource` either. This is the entry point the read tools use, so
	// that `list_tracks`, `get_routing`, and `get_project_summary` share one
	// conversion with this component instead of each reaching into
	// `daw/unit_conversion.h` separately and drifting on the edge cases it documents.
	struct TrackLevels
	{
		double volume_decibels = sesh_ai::daw::unity_volume_decibels;
		double pan_percent = sesh_ai::daw::centre_pan_percent;
	};

	inline TrackLevels converted_track_levels(double reaper_volume, double reaper_pan)
	{
		TrackLevels levels;

		levels.volume_decibels = sesh_ai::daw::volume_decibels_from_reaper_volume(reaper_volume);
		levels.pan_percent = sesh_ai::daw::pan_percent_from_reaper_pan(reaper_pan);

		return levels;
	}

	// ---------------------------------------------------------------------------
	// What the source reads out of REAPER
	//
	// Plain data, no REAPER types, so everything below the seam is testable with a
	// hand-built project. Note what is absent: no volume, no pan, no colour, no FX
	// names, no send lists, no item lists. The snapshot does not carry them, so the
	// reads are not made.
	// ---------------------------------------------------------------------------

	struct ProjectGlobalsReading
	{
		std::string project_name;

		double tempo = 120.0;
		int time_signature_numerator = 4;
		int time_signature_denominator = 4;
		int sample_rate = 48000;

		double project_length = 0.0;
		double cursor_position = 0.0;

		// REAPER's raw bitfield, mapped by `play_state_from_reaper_play_state`. Carried
		// raw so the mapping is on this side of the seam and testable.
		int reaper_play_state = 0;

		double loop_start = 0.0;
		double loop_end = 0.0;
		bool loop_enabled = false;

		// REAPER reports an empty time selection as a zero-length range rather than as
		// an absence, and the schema says both fields are absent when nothing is
		// selected. The source decides which it is; the builder honours it.
		bool time_selection_present = false;
		double time_selection_start = 0.0;
		double time_selection_end = 0.0;

		int tempo_change_count = 0;
		int marker_count = 0;
		int region_count = 0;
	};

	// One track, reduced to what the snapshot needs plus what role derivation needs.
	struct TrackReading
	{
		std::string guid;
		std::string name;

		// REAPER's `I_FOLDERDEPTH`. Read for parentage and role; never serialised.
		int folder_depth_delta = 0;

		// REAPER's `B_MAINSEND`. Read because a folder parent's children's flags are
		// what decide whether it sums at all; never serialised.
		bool parent_send_enabled = true;

		int item_count = 0;
		bool has_fx = false;
		int receive_count = 0;

		bool selected = false;
	};

	struct MasterTrackReading
	{
		// False when REAPER did not hand back a master track, which should not happen
		// in a loaded project and is not worth crashing over.
		bool present = false;

		std::string guid;
		std::string name;

		bool has_fx = false;
		int receive_count = 0;
	};

	// One project, as read in one pass on the main thread.
	struct ProjectContextReading
	{
		// False when the source could not read the project — REAPER functions that did
		// not resolve, or no project open. The builder publishes nothing in that case:
		// a reading that failed serialises as an empty project, and an agent told the
		// session has no tracks will act on that.
		bool readable = false;

		ProjectGlobalsReading globals;
		std::vector<TrackReading> tracks_in_project_order;
		MasterTrackReading master_track;
	};

	// The REAPER seam, two calls wide.
	//
	// The change count is separate from the read because it is polled on every tick
	// and the read is not — that asymmetry is the whole of the debounce, so it is in
	// the interface rather than behind a flag.
	class ProjectContextSource
	{
	public:
		virtual ~ProjectContextSource() = default;

		// One pass over the project. Called at most once per tick, and only when a
		// snapshot is actually going to be built.
		virtual ProjectContextReading read_project_context() = 0;

		// REAPER's `GetProjectStateChangeCount`. Monotonic within one project; treated
		// as "different means changed" rather than "greater means changed", so
		// switching projects is a change rather than a counter that appears to go
		// backwards and is ignored.
		virtual int read_project_state_change_count() = 0;
	};

	// ---------------------------------------------------------------------------
	// The snapshot model
	//
	// The slim model as a C++ value, before any JSON exists. Everything the builder
	// decides is decided here, which is what lets the suite check requirement 6.2 and
	// the schema bounds without a document type at all.
	// ---------------------------------------------------------------------------

	struct SnapshotTimeSignature
	{
		int numerator = 4;
		int denominator = 4;
	};

	struct SnapshotTrack
	{
		std::string guid;

		// REAPER's track number, zero-based. Kept from the reading's position even when
		// earlier tracks were dropped, because it means the number the producer sees in
		// REAPER rather than a position in this array.
		int index = 0;

		std::string name;

		StructuralRole role = StructuralRole::normal;

		// The evidence for the role (requirement 6.6). Reported alongside, not
		// serialised — see the file comment and
		// `track_properties_absent_from_the_snapshot`.
		StructuralRoleSignals role_signals{};
	};

	struct SnapshotMasterTrack
	{
		bool present = false;

		std::string guid;
		std::string name;

		StructuralRole role = StructuralRole::master;
		StructuralRoleSignals role_signals{};
	};

	struct ProjectContextSnapshot
	{
		std::string project_name;

		double tempo = 120.0;
		SnapshotTimeSignature time_signature{};
		int sample_rate = 48000;

		double project_length = 0.0;
		double cursor_position = 0.0;
		PlayState play_state = PlayState::stopped;

		double loop_start = 0.0;
		double loop_end = 0.0;
		bool loop_enabled = false;

		bool time_selection_present = false;
		double time_selection_start = 0.0;
		double time_selection_end = 0.0;

		int tempo_change_count = 0;
		int marker_count = 0;
		int region_count = 0;

		std::vector<SnapshotTrack> tracks;
		std::vector<std::string> selected_track_guids;

		SnapshotMasterTrack master_track{};

		// How many tracks the reading held that could not be represented, because their
		// GUID did not match the schema's pattern. Not serialised; carried so a
		// snapshot that quietly lost a track can say so in a log line.
		std::size_t tracks_omitted_for_unusable_guid = 0;

		// How many selected-track GUIDs were dropped, for the same reason or because
		// the selection exceeded the schema's maxItems.
		std::size_t selected_track_guids_omitted = 0;
	};

	// ---------------------------------------------------------------------------
	// Building the snapshot
	// ---------------------------------------------------------------------------

	// The role signals for every track in a reading, derived in one pass.
	//
	// Parentage comes from `daw::reconstruct_folder_paths` — see the file comment on
	// why this adds no fourth accumulation. A track's enclosing folder is the last
	// entry of its path, its nesting level is the path's length, and the two child
	// counts are tallied by walking the paths once.
	inline std::vector<StructuralRoleSignals> derive_track_role_signals(
		const std::vector<TrackReading>& tracks_in_project_order)
	{
		std::vector<sesh_ai::daw::TrackFolderDepth> folder_depths;
		folder_depths.reserve(tracks_in_project_order.size());

		for (const TrackReading& track : tracks_in_project_order)
		{
			folder_depths.push_back(
				sesh_ai::daw::TrackFolderDepth{track.guid, track.folder_depth_delta}
			);
		}

		const std::vector<std::vector<std::size_t>> folder_paths =
			sesh_ai::daw::reconstruct_folder_paths(folder_depths);

		std::vector<int> child_track_counts(tracks_in_project_order.size(), 0);
		std::vector<int> children_with_parent_send_enabled(tracks_in_project_order.size(), 0);

		for (std::size_t track_index = 0; track_index < tracks_in_project_order.size(); ++track_index)
		{
			if (folder_paths[track_index].empty())
			{
				continue;
			}

			const std::size_t parent_index = folder_paths[track_index].back();

			++child_track_counts[parent_index];

			if (tracks_in_project_order[track_index].parent_send_enabled)
			{
				++children_with_parent_send_enabled[parent_index];
			}
		}

		std::vector<StructuralRoleSignals> signals;
		signals.reserve(tracks_in_project_order.size());

		for (std::size_t track_index = 0; track_index < tracks_in_project_order.size(); ++track_index)
		{
			const TrackReading& track = tracks_in_project_order[track_index];

			StructuralRoleSignals track_signals;

			track_signals.is_master_track = false;
			track_signals.accumulated_folder_depth = static_cast<int>(folder_paths[track_index].size());
			track_signals.child_track_count = child_track_counts[track_index];
			track_signals.children_with_parent_send_enabled =
				children_with_parent_send_enabled[track_index];
			track_signals.parent_send_enabled = track.parent_send_enabled;
			track_signals.item_count = track.item_count;
			track_signals.has_fx = track.has_fx;
			track_signals.receive_count = track.receive_count;

			signals.push_back(track_signals);
		}

		return signals;
	}

	// The master track's signals. `is_master_track` is what decides the role, but the
	// rest are carried anyway so the log line reports what was actually read rather
	// than a shape that claims nothing was.
	inline StructuralRoleSignals derive_master_track_role_signals(const MasterTrackReading& master_track)
	{
		StructuralRoleSignals signals;

		signals.is_master_track = true;
		signals.accumulated_folder_depth = 0;
		signals.child_track_count = 0;
		signals.children_with_parent_send_enabled = 0;

		// The master has nothing above it to send to, which is part of what makes it
		// the master rather than a track.
		signals.parent_send_enabled = false;
		signals.item_count = 0;
		signals.has_fx = master_track.has_fx;
		signals.receive_count = master_track.receive_count;

		return signals;
	}

	// A reading in, a snapshot the schema accepts out.
	//
	// Total. Every field is clamped or truncated, and anything that cannot be made
	// representable is left out rather than emitted malformed. An unreadable reading
	// still produces a snapshot value — the builder is what declines to publish it,
	// because "did the read work" is a decision with a log line attached and not
	// something to encode as an empty project.
	inline ProjectContextSnapshot build_project_context_snapshot(const ProjectContextReading& reading)
	{
		ProjectContextSnapshot snapshot;

		const ProjectGlobalsReading& globals = reading.globals;

		snapshot.project_name =
			sesh_ai::transport::truncate_to_length(globals.project_name, maximum_name_length);

		snapshot.tempo = bounded_tempo(globals.tempo);
		snapshot.time_signature.numerator =
			bounded_time_signature_numerator(globals.time_signature_numerator);
		snapshot.time_signature.denominator =
			nearest_representable_time_signature_denominator(globals.time_signature_denominator);
		snapshot.sample_rate = bounded_sample_rate(globals.sample_rate);

		snapshot.project_length = bounded_timeline_seconds(globals.project_length);
		snapshot.cursor_position = bounded_timeline_seconds(globals.cursor_position);
		snapshot.play_state = play_state_from_reaper_play_state(globals.reaper_play_state);

		snapshot.loop_start = bounded_timeline_seconds(globals.loop_start);
		snapshot.loop_end = bounded_timeline_seconds(globals.loop_end);
		snapshot.loop_enabled = globals.loop_enabled;

		// Both or neither. The schema declares them independently optional and
		// describes them as a pair — "absent when nothing is selected" — and half a
		// range is not something the agent can act on.
		snapshot.time_selection_present = globals.time_selection_present;

		if (snapshot.time_selection_present)
		{
			snapshot.time_selection_start = bounded_timeline_seconds(globals.time_selection_start);
			snapshot.time_selection_end = bounded_timeline_seconds(globals.time_selection_end);
		}

		snapshot.tempo_change_count = detail::clamp_count(globals.tempo_change_count);
		snapshot.marker_count = detail::clamp_count(globals.marker_count);
		snapshot.region_count = detail::clamp_count(globals.region_count);

		const std::vector<StructuralRoleSignals> track_signals =
			derive_track_role_signals(reading.tracks_in_project_order);

		snapshot.tracks.reserve(reading.tracks_in_project_order.size());

		for (std::size_t track_index = 0;
			track_index < reading.tracks_in_project_order.size();
			++track_index)
		{
			const TrackReading& track = reading.tracks_in_project_order[track_index];

			if (!is_schema_shaped_guid(track.guid))
			{
				++snapshot.tracks_omitted_for_unusable_guid;

				// A selected track whose GUID is unusable is absent from the selection
				// for the same reason it is absent from the array. Counted, because a
				// selection the agent acts on should not silently be shorter than the
				// producer thinks it is.
				if (track.selected)
				{
					++snapshot.selected_track_guids_omitted;
				}

				continue;
			}

			SnapshotTrack snapshot_track;

			snapshot_track.guid = track.guid;
			snapshot_track.index = static_cast<int>(track_index);
			snapshot_track.name =
				sesh_ai::transport::truncate_to_length(track.name, maximum_name_length);

			const StructuralRoleDerivation derivation = derive_structural_role(track_signals[track_index]);

			snapshot_track.role = derivation.role;
			snapshot_track.role_signals = derivation.signals;

			snapshot.tracks.push_back(std::move(snapshot_track));

			if (!track.selected)
			{
				continue;
			}

			if (snapshot.selected_track_guids.size() >= maximum_selected_track_guids)
			{
				++snapshot.selected_track_guids_omitted;
				continue;
			}

			snapshot.selected_track_guids.push_back(track.guid);
		}

		if (reading.master_track.present && is_schema_shaped_guid(reading.master_track.guid))
		{
			const StructuralRoleDerivation master_derivation =
				derive_structural_role(derive_master_track_role_signals(reading.master_track));

			snapshot.master_track.present = true;
			snapshot.master_track.guid = reading.master_track.guid;
			snapshot.master_track.name =
				sesh_ai::transport::truncate_to_length(reading.master_track.name, maximum_name_length);
			snapshot.master_track.role = master_derivation.role;
			snapshot.master_track.role_signals = master_derivation.signals;
		}

		return snapshot;
	}

	// ---------------------------------------------------------------------------
	// Diagnostics
	// ---------------------------------------------------------------------------

	// One line per track, each carrying the derived role and every signal behind it —
	// requirement 6.6's "diagnosable from a log" for the whole snapshot. Separate from
	// the snapshot itself so the evidence costs context window space in a log file and
	// not in the payload.
	inline std::vector<std::string> describe_snapshot_role_derivations(
		const ProjectContextSnapshot& snapshot)
	{
		std::vector<std::string> descriptions;
		descriptions.reserve(snapshot.tracks.size() + 1);

		for (const SnapshotTrack& track : snapshot.tracks)
		{
			std::string description{"track index="};
			description += std::to_string(track.index);
			description += " guid=";
			description += track.guid;
			description += " ";
			description += describe_structural_role_derivation(
				StructuralRoleDerivation{track.role, track.role_signals}
			);

			descriptions.push_back(std::move(description));
		}

		if (snapshot.master_track.present)
		{
			std::string description{"master guid="};
			description += snapshot.master_track.guid;
			description += " ";
			description += describe_structural_role_derivation(
				StructuralRoleDerivation{snapshot.master_track.role, snapshot.master_track.role_signals}
			);

			descriptions.push_back(std::move(description));
		}

		return descriptions;
	}

	// The snapshot's own log line: its size and what it dropped. Deliberately says
	// nothing about the project's contents, because a log is not where a producer's
	// track names belong.
	inline std::string describe_project_context_snapshot(const ProjectContextSnapshot& snapshot)
	{
		std::string description{"tracks="};

		description += std::to_string(snapshot.tracks.size());
		description += " selected=";
		description += std::to_string(snapshot.selected_track_guids.size());
		description += " masterTrack=";
		description += snapshot.master_track.present ? "present" : "absent";
		description += " markerCount=";
		description += std::to_string(snapshot.marker_count);
		description += " regionCount=";
		description += std::to_string(snapshot.region_count);
		description += " tempoChangeCount=";
		description += std::to_string(snapshot.tempo_change_count);
		description += " tracksOmitted=";
		description += std::to_string(snapshot.tracks_omitted_for_unusable_guid);
		description += " selectedGuidsOmitted=";
		description += std::to_string(snapshot.selected_track_guids_omitted);

		return description;
	}

	// ---------------------------------------------------------------------------
	// Serialisation
	// ---------------------------------------------------------------------------

	// The snapshot as a document.
	//
	// Templated on the JSON type for the reason the Envelope Codec is: the suite
	// drives it with a document that records what was written, which is how "the
	// snapshot omits every field requirement 6.2 excludes" becomes a test over actual
	// output rather than over a struct that happens not to have the fields. The
	// production instantiation is in project_context_builder.cpp.
	//
	// Every property written here is one of the names listed above, and nothing
	// iterates a schema to decide what to write — the slim model is a fixed shape, and
	// a serialiser that derived its fields from the schema would grow every time the
	// schema did, which is the failure this component exists to prevent.
	template <typename JsonType>
	JsonType serialize_project_context(const ProjectContextSnapshot& snapshot)
	{
		JsonType document = JsonType::object();

		document[std::string{"projectName"}] = snapshot.project_name;
		document[std::string{"tempo"}] = snapshot.tempo;

		JsonType time_signature = JsonType::object();
		time_signature[std::string{"numerator"}] = snapshot.time_signature.numerator;
		time_signature[std::string{"denominator"}] = snapshot.time_signature.denominator;
		document[std::string{"timeSignature"}] = std::move(time_signature);

		document[std::string{"sampleRate"}] = snapshot.sample_rate;
		document[std::string{"projectLength"}] = snapshot.project_length;
		document[std::string{"cursorPosition"}] = snapshot.cursor_position;
		document[std::string{"playState"}] = std::string{to_schema_string(snapshot.play_state)};

		document[std::string{"loopStart"}] = snapshot.loop_start;
		document[std::string{"loopEnd"}] = snapshot.loop_end;
		document[std::string{"loopEnabled"}] = snapshot.loop_enabled;

		// Omitted together when there is no selection. The schema's own description
		// says absent rather than zero, and a zero-length selection at the start of the
		// timeline is a thing a producer can actually have.
		if (snapshot.time_selection_present)
		{
			document[std::string{snapshot_time_selection_start_property}] =
				snapshot.time_selection_start;
			document[std::string{snapshot_time_selection_end_property}] =
				snapshot.time_selection_end;
		}

		document[std::string{"tempoChangeCount"}] = snapshot.tempo_change_count;
		document[std::string{"markerCount"}] = snapshot.marker_count;
		document[std::string{"regionCount"}] = snapshot.region_count;

		JsonType tracks = JsonType::array();

		for (const SnapshotTrack& track : snapshot.tracks)
		{
			JsonType track_document = JsonType::object();

			track_document[std::string{"guid"}] = track.guid;
			track_document[std::string{"index"}] = track.index;
			track_document[std::string{"name"}] = track.name;
			track_document[std::string{"role"}] = std::string{to_schema_string(track.role)};

			tracks.push_back(std::move(track_document));
		}

		document[std::string{"tracks"}] = std::move(tracks);

		JsonType selected_track_guids = JsonType::array();

		for (const std::string& guid : snapshot.selected_track_guids)
		{
			selected_track_guids.push_back(guid);
		}

		document[std::string{"selectedTrackGuids"}] = std::move(selected_track_guids);

		if (snapshot.master_track.present)
		{
			JsonType master_track = JsonType::object();

			master_track[std::string{"guid"}] = snapshot.master_track.guid;
			master_track[std::string{"name"}] = snapshot.master_track.name;
			master_track[std::string{"role"}] =
				std::string{to_schema_string(snapshot.master_track.role)};

			document[std::string{snapshot_master_track_property}] = std::move(master_track);
		}

		return document;
	}

	// ---------------------------------------------------------------------------
	// Debouncing (requirement 6.4)
	// ---------------------------------------------------------------------------

	struct SnapshotDebounceSettings
	{
		// How many consecutive ticks the change counter must hold still before a
		// snapshot is built.
		//
		// REAPER runs registered timers at roughly 30 Hz, so eight ticks is around a
		// quarter of a second. Long enough that a fader drag, an item nudge, or a
		// typed track name produces one snapshot rather than dozens; short enough that
		// a producer who makes a change and then prompts finds the agent already
		// knows. Overridable because the right number is measured in a real session,
		// not reasoned about.
		std::size_t quiet_ticks = 8;
	};

	// A trailing-edge debounce over REAPER's project state change count.
	//
	// Deliberately holds no clock and touches nothing outside itself: a tick and a
	// count go in, a yes or no comes out. That is what makes the flood case — the
	// thing requirement 6.4 is actually about — a test rather than a judgement.
	class ProjectChangeDebouncer
	{
	public:
		explicit ProjectChangeDebouncer(SnapshotDebounceSettings settings = SnapshotDebounceSettings{})
			: settings_{settings}
		{
		}

		// One tick. True when a snapshot is due.
		//
		// The first observation adopts the count and never fires: the initial snapshot
		// is the connect path's, and firing here as well would send two.
		bool note_tick(int project_state_change_count)
		{
			if (!has_seen_a_count_)
			{
				has_seen_a_count_ = true;
				last_seen_change_count_ = project_state_change_count;

				return false;
			}

			if (project_state_change_count != last_seen_change_count_)
			{
				last_seen_change_count_ = project_state_change_count;
				change_pending_ = true;
				quiet_ticks_ = 0;

				return false;
			}

			if (!change_pending_)
			{
				return false;
			}

			++quiet_ticks_;

			if (quiet_ticks_ < settings_.quiet_ticks)
			{
				return false;
			}

			change_pending_ = false;
			quiet_ticks_ = 0;

			return true;
		}

		// A snapshot went out by another route — on connect, or in answer to a request.
		// The count it was built from is now the baseline, and any pending change is
		// satisfied: the snapshot the debounce was waiting to send has effectively been
		// sent, and sending it again a few ticks later is the flood this class exists
		// to prevent, arriving by the other door.
		void note_snapshot_published(int project_state_change_count)
		{
			has_seen_a_count_ = true;
			last_seen_change_count_ = project_state_change_count;
			change_pending_ = false;
			quiet_ticks_ = 0;
		}

		bool change_pending() const { return change_pending_; }

		std::size_t quiet_ticks() const { return quiet_ticks_; }

		bool has_seen_a_count() const { return has_seen_a_count_; }

		int last_seen_change_count() const { return last_seen_change_count_; }

		const SnapshotDebounceSettings& settings() const { return settings_; }

	private:
		SnapshotDebounceSettings settings_;

		bool has_seen_a_count_ = false;
		int last_seen_change_count_ = 0;
		bool change_pending_ = false;
		std::size_t quiet_ticks_ = 0;
	};

	// ---------------------------------------------------------------------------
	// The component
	// ---------------------------------------------------------------------------

	// Why a snapshot was sent. Decides the envelope type, and is worth having in the
	// log: a snapshot nobody asked for arriving mid-turn is a debounce that fired, and
	// one that did not arrive is a debounce that did not.
	enum class SnapshotReason
	{
		connected,
		project_changed,
		requested
	};

	constexpr std::string_view envelope_type_for_snapshot_reason(SnapshotReason reason)
	{
		switch (reason)
		{
			case SnapshotReason::connected:
			case SnapshotReason::project_changed:
				return project_context_state_envelope_type;
			case SnapshotReason::requested:
				return project_context_response_envelope_type;
		}

		return project_context_state_envelope_type;
	}

	constexpr std::string_view describe_snapshot_reason(SnapshotReason reason)
	{
		switch (reason)
		{
			case SnapshotReason::connected:
				return "connected";
			case SnapshotReason::project_changed:
				return "project_changed";
			case SnapshotReason::requested:
				return "requested";
		}

		return "";
	}

	// What one publication attempt did. Returned rather than logged, so the caller —
	// which knows how this build reports things — decides.
	struct SnapshotPublication
	{
		bool published = false;

		SnapshotReason reason = SnapshotReason::connected;

		// Empty when nothing was published.
		std::string envelope_type;

		// Echoed onto a response (requirement 6.9). Empty for the two unprompted
		// reasons.
		std::string request_id;

		// False when the source could not read the project. The one reason a requested
		// or connect-time publication does not happen.
		bool project_readable = false;

		// True when the tick's debounce had nothing due. Not a failure.
		bool debounced = false;

		std::size_t track_count = 0;
		std::size_t selected_track_count = 0;
		bool master_track_present = false;

		std::size_t tracks_omitted_for_unusable_guid = 0;
		std::size_t selected_track_guids_omitted = 0;
	};

	// Assembles the snapshot and puts it on the outbound queue.
	//
	// Every method here runs on the main thread inside the dispatcher's tick, because
	// reading REAPER anywhere else is requirement 22.3's whole prohibition. Nothing
	// here validates or serialises to text: the envelope carries a document, and the
	// network thread does the rest (requirement 22.2).
	//
	// Templated on the document type and the outbound envelope type, so the suite
	// drives the sequencing — connect sends, debounce holds, request answers — with
	// types it controls. `project_context_builder.cpp` names both against the real
	// ones.
	template <typename JsonType, typename OutboundEnvelopeType>
	class ProjectContextBuilder
	{
	public:
		using OutboundQueue = sesh_ai::transport::ConcurrentQueue<OutboundEnvelopeType>;

		explicit ProjectContextBuilder(
			ProjectContextSource& source,
			SnapshotDebounceSettings debounce_settings = SnapshotDebounceSettings{}
		)
			: source_{source}
			, debouncer_{debounce_settings}
		{
		}

		ProjectContextBuilder(const ProjectContextBuilder&) = delete;
		ProjectContextBuilder& operator=(const ProjectContextBuilder&) = delete;
		ProjectContextBuilder(ProjectContextBuilder&&) = delete;
		ProjectContextBuilder& operator=(ProjectContextBuilder&&) = delete;

		// On connect, and on every successful reconnect (requirement 3.5). Unprompted,
		// so no requestId.
		SnapshotPublication publish_on_connect(OutboundQueue& outbound_queue)
		{
			return publish(
				outbound_queue,
				SnapshotReason::connected,
				std::string_view{},
				source_.read_project_state_change_count()
			);
		}

		// Once per timer tick. Reads the change count, and reads the project only when
		// the debounce says a snapshot is due — which is what keeps a quiet session
		// from walking the track list thirty times a second.
		SnapshotPublication publish_on_timer_tick(OutboundQueue& outbound_queue)
		{
			const int change_count = source_.read_project_state_change_count();

			if (!debouncer_.note_tick(change_count))
			{
				SnapshotPublication publication;

				publication.reason = SnapshotReason::project_changed;
				publication.debounced = true;

				return publication;
			}

			return publish(
				outbound_queue,
				SnapshotReason::project_changed,
				std::string_view{},
				change_count
			);
		}

		// The answer to `request:project_context`: the same snapshot, wrapped with the
		// echoed requestId (requirement 6.9).
		SnapshotPublication publish_on_request(
			OutboundQueue& outbound_queue,
			std::string_view request_id
		)
		{
			return publish(
				outbound_queue,
				SnapshotReason::requested,
				request_id,
				source_.read_project_state_change_count()
			);
		}

		// The last snapshot built, whether or not it was published. Held so the UI can
		// show the session the server was told about, and so a diagnostic can ask what
		// the agent thinks it is looking at.
		const ProjectContextSnapshot& last_snapshot() const { return last_snapshot_; }

		bool has_built_a_snapshot() const { return has_built_a_snapshot_; }

		const ProjectChangeDebouncer& debouncer() const { return debouncer_; }

	private:
		// `change_count_before_read` is the counter as it stood *before* the project was
		// walked, and the caller reads it rather than this function so that the tick path
		// does not read it twice.
		//
		// Which side of the read it comes from is load-bearing. Walking a large project
		// is not instantaneous, and a producer can change something during the walk — so
		// the snapshot that goes out may already be a change behind. Adopting the
		// pre-read count leaves that change looking unseen, which is exactly right: the
		// next tick finds the counter different, arms the debounce, and a corrected
		// snapshot follows. Adopting the count read *after* the walk would swallow it,
		// and the server would hold a subtly stale snapshot until the producer happened
		// to touch the project again.
		SnapshotPublication publish(
			OutboundQueue& outbound_queue,
			SnapshotReason reason,
			std::string_view request_id,
			int change_count_before_read
		)
		{
			SnapshotPublication publication;

			publication.reason = reason;
			publication.request_id.assign(request_id);

			const ProjectContextReading reading = source_.read_project_context();

			publication.project_readable = reading.readable;

			if (!reading.readable)
			{
				// Nothing goes out. `publication.project_readable` is false and the
				// caller logs it.
				//
				// The debounce is deliberately not re-armed, so a failed read is not
				// retried every quiet period. The one thing that makes a reading
				// unreadable is a REAPER function the source could not resolve, which is
				// a property of the loaded build rather than a transient condition — it
				// will still be unresolved on the next tick, and on every tick after
				// that. Re-arming would produce the same failure four times a second for
				// as long as REAPER is open. The next actual project change, the next
				// `request:project_context`, and the next reconnect each try again, which
				// is enough for a condition that is not going to clear on its own.
				return publication;
			}

			last_snapshot_ = build_project_context_snapshot(reading);
			has_built_a_snapshot_ = true;

			OutboundEnvelopeType envelope;

			envelope.type = std::string{envelope_type_for_snapshot_reason(reason)};
			envelope.request_id.assign(request_id);
			envelope.payload = serialize_project_context<JsonType>(last_snapshot_);

			outbound_queue.push(std::move(envelope));

			debouncer_.note_snapshot_published(change_count_before_read);

			publication.published = true;
			publication.envelope_type = std::string{envelope_type_for_snapshot_reason(reason)};
			publication.track_count = last_snapshot_.tracks.size();
			publication.selected_track_count = last_snapshot_.selected_track_guids.size();
			publication.master_track_present = last_snapshot_.master_track.present;
			publication.tracks_omitted_for_unusable_guid =
				last_snapshot_.tracks_omitted_for_unusable_guid;
			publication.selected_track_guids_omitted =
				last_snapshot_.selected_track_guids_omitted;

			return publication;
		}

		ProjectContextSource& source_;
		ProjectChangeDebouncer debouncer_;

		ProjectContextSnapshot last_snapshot_{};
		bool has_built_a_snapshot_ = false;
	};

	// ---------------------------------------------------------------------------
	// The snapshot is validated where the codec validates it
	//
	// Requirement 6.8 holds because both envelope types this component emits are bound
	// to project-context.schema.json in schema_validator.h's outbound table, and the
	// Envelope Codec validates an outbound payload before serialising it. Checked at
	// compile time rather than trusted: if either binding is renamed or dropped, the
	// build says so here, next to the constants, instead of the snapshot silently
	// going out unvalidated.
	// ---------------------------------------------------------------------------

	static_assert(
		sesh_ai::transport::outbound_payload_schema_path(project_context_state_envelope_type)
			== sesh_ai::transport::project_context_schema_path,
		"state:project_context must be bound to project-context.schema.json (requirement 6.8)"
	);

	static_assert(
		sesh_ai::transport::outbound_payload_schema_path(project_context_response_envelope_type)
			== sesh_ai::transport::project_context_schema_path,
		"response:project_context must be bound to project-context.schema.json (requirement 6.8)"
	);
}

#endif
