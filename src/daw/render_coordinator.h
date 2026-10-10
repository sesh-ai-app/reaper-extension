// The Render Coordinator (requirement 12, design "Render Coordinator", ADR 0018).
//
// Owns the `render` tool. Composes REAPER's render settings, resolves the output
// paths REAPER's wildcards would produce, refuses before anything is written when
// those paths collide, and otherwise adds the job to REAPER's render queue and
// returns.
//
// ---------------------------------------------------------------------------
// Queue-only. There is no synchronous mode.
//
// ADR 0018 carries a `Status: Accepted, amended` header, and the amendment is what
// this file implements: the two-mode model in the body below it is superseded
// history. A render occupies REAPER while it runs, and the Tool Relay settles a call
// after 30 seconds while the AgentCore harness times out at 120 — so a synchronous
// render of anything album-length reports failure for work that then succeeds and
// writes its files. The duration estimate that was meant to choose between the modes
// was acknowledged as unreliable, so both it and the mode selection are gone. The
// tool always queues (requirement 12.1) and `render.schema.json` no longer carries
// `execution` or `confirmedLongRender`.
//
// The consequence the agent has to say out loud: **nothing is written when this
// result arrives.** The files appear when the producer renders the queue. The output
// schema says so in its own description, and it is why the result enumerates the
// paths the jobs *will* write rather than paths that exist.
//
// ---------------------------------------------------------------------------
// The undo effect is `none` (requirement 12.7)
//
// No undo block is opened here, no undo position marker is captured, and a turn
// whose only action is a render does not offer "revert all". This is visible in this
// file as an absence, which is the kind of thing that gets "helpfully" added back
// later, so: `RenderHost` exposes no undo operation at all, and `render_result` has
// no undo report fields. `outputs/render.schema.json` is the other half of the
// enforcement — it is the one mutating-classified tool whose output schema has no
// undo report, and `additionalProperties` is false, so a result carrying one is
// refused by the extension's own outbound validation.
//
// ADR 0018's body argues the opposite (`mutates`, on a forum report that queueing
// pushes an undo entry). The amendment reverses it: the undoability of a queue
// operation is transient and unreliable, and a marker for a turn that changed no
// project state offers the producer a revert that reverts nothing they can see.
//
// ---------------------------------------------------------------------------
// Three things here are easy to get wrong, and two of them are structural
//
// **Render settings are one integer, OR-ed and written once.** REAPER's
// `RENDER_SETTINGS` is a single composed value, so a second `GetSetProjectInfo` call
// overwrites the first rather than adding to it — write `&2` then `&8` and you have
// `8`, not `10`, and a stem render silently becomes a master mix. A comment saying
// "write once" is not worth much, so the shape rules it out instead:
//
//   - `render_settings_word` is a distinct type whose constructor is private. The
//     only way to obtain one is `compose_render_settings`, which folds every flag
//     into one local and returns it. There is no way to build a word from a loose
//     integer, so there is nothing to accumulate incorrectly.
//   - `RenderHost` has no per-flag setter. Its only settings mutator takes a whole
//     word, so "set one flag" is not an expressible operation.
//   - `single_render_settings_write` is the coordinator's only route to that mutator
//     and permits one write per render call. A second attempt fails rather than
//     overwriting, and the count is observable, so the suite asserts the property
//     rather than trusting it.
//
// **Collision detection runs on resolved paths, before anything is written**
// (requirements 12.4, 9.8). Resolution goes through REAPER itself — settings and the
// output pattern are written, then `RENDER_TARGETS` is read back, which is REAPER's
// own answer to "what files would this write". Only then is the queue touched. Two
// separate collisions are checked, and they are not the same refusal:
//
//   - Two of this render's own resolved paths being equal. Unacknowledgeable — the
//     jobs would clobber each other, and no approval makes that the producer's
//     intent. Nothing is queued.
//   - A resolved path that already exists on disk. Acknowledgeable via
//     `confirmedOverwrite`, which the tool schema says is never set on a first call,
//     because "just overwrite it" said of one file does not authorise destroying
//     eight.
//
// Both enumerate the specific paths in `blocking`, because a producer approving an
// overwrite needs to see which files.
//
// **`stems_via_master` is a loop, not a flag.** See the section below — this is the
// one place where the requirement and the SDK disagree, and it is worth reading
// before changing anything.
//
// ---------------------------------------------------------------------------
// `stems_via_master`, and a finding worth recording
//
// Requirement 12.6 specifies a solo-queue-unsolo loop: for each selected track,
// solo it, queue a render whose source is the master mix, unsolo, and report how
// many jobs were queued. "For each selected track" is **unconditional** — including
// tracks that look ineligible, such as folder parents — so the loop here has no
// filter, and `queuedJobCount` is the selected track count rather than a count of
// tracks that passed a test. That is implemented as specified.
//
// Two things about it should be recorded rather than discovered later.
//
// *The SDK contradicts the ADR's premise.* ADR 0018's "Note on evidence" says the
// documented `RENDER_SETTINGS` flags name a via-master variant for selected media
// *items* (`&32`, `&64`) and none for tracks, and that this is why the loop exists.
// `reaper_plugin_functions.h` at the pinned REAPER 7.80 documents
// `&128=selected tracks via master` — the native flag the ADR concluded might not
// exist. `render_settings_source::selected_tracks_via_master` below carries it, and
// it round-trips like every other flag, so the flag is available the moment the
// requirement changes. It is not used by the loop, because the requirement says
// loop.
//
// *Queued jobs do not capture solo state.* A queued job carries the render settings
// it was queued with, which is REAPER's behaviour and is noted in ADR 0018's
// consequences. Solo is project state, not a render setting — so N jobs queued
// across a solo loop all render with whatever solo state exists when the producer
// runs the queue, which by then is none. The X-Raym script the design cites for the
// technique renders immediately rather than queueing, which is where the difference
// bites. This is an open question for the SDK validation step, not something this
// file can resolve, and it is the second reason `&128` deserves a look.
//
// ---------------------------------------------------------------------------
// Nothing here includes the REAPER SDK
//
// Following `entry/timer_registration.h`: this component declares the narrowest
// interface it needs — `RenderHost` — and the production implementation of that
// interface is the only thing that includes the SDK. Not one ReaperApi listing
// everything REAPER can do; a seam per component, sized to that component's actual
// need. This one's need is wide because rendering touches a lot of project info, and
// it is still only this component's operations.
//
// The enforcement is structural: the test target has no SDK include path, so a
// header that reached the SDK could not be included from a test and the build would
// say so. That is what keeps settings composition, path resolution, and collision
// detection — which is most of this component — testable outside REAPER.
//
// Defined inline in the header, following `cycle_detector.h` and `unit_conversion.h`:
// the test target globs only `tests/*.cpp`, so logic in a `src/*.cpp` is logic the
// suite cannot reach. `reaper_render_host.h`/`.cpp` is the REAPER-facing half.

#ifndef SESH_AI_DAW_RENDER_COORDINATOR_H
#define SESH_AI_DAW_RENDER_COORDINATOR_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sesh_ai::daw
{
	// -----------------------------------------------------------------------
	// Contract values
	//
	// Refusal reasons and acknowledgement field names are schema values, not prose,
	// so they are stated once here instead of spelled out at each call site.
	// -----------------------------------------------------------------------

	// In the refusal schema's closed enum. Covers both collisions — two of this
	// render's own paths being equal, and a path that already exists.
	inline constexpr std::string_view output_file_collision_refusal_reason{"output_file_collision"};

	// `confirmedOverwrite` in render.schema.json: set only after a refusal listed the
	// colliding files and the producer approved overwriting those specific files.
	inline constexpr std::string_view overwrite_acknowledgement_field{"confirmedOverwrite"};

	// `blocking[].kind` in the refusal schema, also a closed enum. Only `file_path` is
	// produced here — a render's blocking entities are the paths it would have
	// written. `track` is spelled by `object_resolver.h`, which is what produces
	// track-blocking refusals.
	inline constexpr std::string_view file_path_blocking_kind{"file_path"};

	// -----------------------------------------------------------------------
	// The tool's own vocabulary, as render.schema.json spells it
	// -----------------------------------------------------------------------

	// `source`. What the queued jobs write.
	enum class render_source
	{
		// The summed mix, one file, through the master chain.
		master_mix,

		// One file per selected track, pre-master. The files sum back to the mix,
		// which is what a mastering handoff wants.
		stems,

		// One file per selected track, each passed through the master chain. Real,
		// usually wrong, and implemented as a loop rather than a flag — see the
		// header comment.
		stems_via_master
	};

	// `bounds`. The span the queued jobs write. Independent of the source: ADR 0018
	// separated them because the retired three-tool split welded a bound to a source,
	// so `render_selected_region` could never render a whole project.
	enum class render_bounds
	{
		region,
		time_selection,
		project
	};

	// `outputFormat`.
	enum class render_output_format
	{
		wav,
		mp3,
		flac
	};

	inline constexpr std::string_view to_schema_string(render_source source)
	{
		switch (source)
		{
			case render_source::master_mix:
				return "master_mix";
			case render_source::stems:
				return "stems";
			case render_source::stems_via_master:
				return "stems_via_master";
		}

		return "master_mix";
	}

	inline constexpr std::string_view to_schema_string(render_bounds bounds)
	{
		switch (bounds)
		{
			case render_bounds::region:
				return "region";
			case render_bounds::time_selection:
				return "time_selection";
			case render_bounds::project:
				return "project";
		}

		return "project";
	}

	inline constexpr std::string_view to_schema_string(render_output_format format)
	{
		switch (format)
		{
			case render_output_format::wav:
				return "wav";
			case render_output_format::mp3:
				return "mp3";
			case render_output_format::flac:
				return "flac";
		}

		return "wav";
	}

	// Whether this source writes a file per selected track, which is what makes a
	// selected track's own signal become a file of its own. A master mix render writes
	// one file through the master chain and does not depend on the selection at all,
	// which is why a per-track source is the only one for which an empty selection is
	// a failure.
	inline constexpr bool render_source_writes_a_file_per_track(render_source source)
	{
		return source == render_source::stems || source == render_source::stems_via_master;
	}

	// mp3 has no bit depth, and the output schema says so by omitting `bitDepth` for
	// it rather than reporting a meaningless number.
	inline constexpr bool bit_depth_applies_to(render_output_format format)
	{
		return format != render_output_format::mp3;
	}

	// REAPER's sink configuration accepts a plain four-byte string to mean "default
	// settings for that sink type". The codes are the format name in reverse byte
	// order, which is why they read backwards; `GetSetProjectInfo_String` documents
	// "evaw" and "l3pm" as its own examples.
	//
	// The four-character form carries no bit depth — that needs a base64-encoded sink
	// configuration — so the requested depth travels alongside it in
	// `render_format_request` and encoding it is the host's business.
	inline constexpr std::string_view sink_four_character_code_for(render_output_format format)
	{
		switch (format)
		{
			case render_output_format::wav:
				return "evaw";
			case render_output_format::mp3:
				return "l3pm";
			case render_output_format::flac:
				return "calf";
		}

		return "evaw";
	}

	// -----------------------------------------------------------------------
	// RENDER_SETTINGS: the composed integer
	// -----------------------------------------------------------------------

	// The flag values, quoted from `GetSetProjectInfo`'s own documentation of
	// `RENDER_SETTINGS` in reaper_plugin_functions.h at the pinned REAPER 7.80.
	// Named rather than written inline at the fold, so a wrong bit is a wrong name
	// and not a number nobody can check.
	namespace render_settings_flag
	{
		// Source axis. A master mix is the absence of both stem bits, not a bit of
		// its own — `(&(1|2))==0`.
		inline constexpr int stems_and_master_mix = 1;
		inline constexpr int stems_only = 2;
		inline constexpr int selected_tracks_via_master = 128;

		inline constexpr int source_axis_mask =
			stems_and_master_mix | stems_only | selected_tracks_via_master;

		// Everything else is an independent flag.
		inline constexpr int multichannel_tracks_to_multichannel_files = 4;
		inline constexpr int use_render_matrix = 8;
		inline constexpr int mono_media_to_mono_files = 16;
		inline constexpr int embed_transients = 256;
		inline constexpr int embed_metadata = 512;
		inline constexpr int embed_take_markers = 1024;
		inline constexpr int second_pass_render = 2048;
		inline constexpr int render_razor_edits = 4096;

		// Documented as "pre-fader stems (not if via master)". Pre-fader is a
		// different axis from pre-master: plain stems are already pre-master, and
		// this additionally takes the fader out, which is not what a mastering
		// handoff asks for. Carried so the word round-trips, not set by this tool.
		inline constexpr int pre_fader_stems = 8192;

		inline constexpr int only_stem_channels_sent_to_parent = 16384;
	}

	// Which entry of REAPER's Source list the composed word selects.
	//
	// Separate from `render_source` on purpose. `stems_via_master` has no word of its
	// own — requirement 12.6 queues it as a master mix with one track soloed — so the
	// tool's three sources and the word's four settings are not the same axis, and
	// collapsing them would make the round-trip below a lie.
	enum class render_settings_source
	{
		master_mix,
		stems_and_master_mix,
		stems_only,
		selected_tracks_via_master
	};

	// The whole of what goes into the composed integer.
	//
	// Every field is an independent bit except `source`, which occupies three
	// mutually exclusive bits. Composition never sets more than one of them, which is
	// what makes decomposition exact.
	struct render_settings
	{
		render_settings_source source = render_settings_source::master_mix;

		bool multichannel_tracks_to_multichannel_files = false;
		bool use_render_matrix = false;
		bool mono_media_to_mono_files = false;
		bool embed_transients = false;
		bool embed_metadata = false;
		bool embed_take_markers = false;
		bool second_pass_render = false;
		bool render_razor_edits = false;
		bool pre_fader_stems = false;
		bool only_stem_channels_sent_to_parent = false;
	};

	inline constexpr bool operator==(const render_settings& left, const render_settings& right)
	{
		return left.source == right.source
			&& left.multichannel_tracks_to_multichannel_files == right.multichannel_tracks_to_multichannel_files
			&& left.use_render_matrix == right.use_render_matrix
			&& left.mono_media_to_mono_files == right.mono_media_to_mono_files
			&& left.embed_transients == right.embed_transients
			&& left.embed_metadata == right.embed_metadata
			&& left.embed_take_markers == right.embed_take_markers
			&& left.second_pass_render == right.second_pass_render
			&& left.render_razor_edits == right.render_razor_edits
			&& left.pre_fader_stems == right.pre_fader_stems
			&& left.only_stem_channels_sent_to_parent == right.only_stem_channels_sent_to_parent;
	}

	inline constexpr bool operator!=(const render_settings& left, const render_settings& right)
	{
		return !(left == right);
	}

	class render_settings_word;

	constexpr render_settings_word compose_render_settings(const render_settings& settings);
	constexpr render_settings_word render_settings_word_as_read_from_reaper(int project_value);

	// One composed `RENDER_SETTINGS` value.
	//
	// A distinct type rather than an `int`, with a private constructor and exactly two
	// friends that can produce one: the composition below, and a named reader for a
	// value that came back out of REAPER. That is the structural half of "OR-ed and
	// written once" — there is no way to hand `RenderHost::write_render_settings` a
	// partially built value, because a partially built value cannot be constructed.
	class render_settings_word final
	{
	public:
		constexpr int value() const { return value_; }

		constexpr bool operator==(const render_settings_word& other) const
		{
			return value_ == other.value_;
		}

		constexpr bool operator!=(const render_settings_word& other) const
		{
			return value_ != other.value_;
		}

	private:
		constexpr explicit render_settings_word(int value)
			: value_{value}
		{
		}

		friend constexpr render_settings_word compose_render_settings(const render_settings& settings);
		friend constexpr render_settings_word render_settings_word_as_read_from_reaper(int project_value);

		int value_;
	};

	// The one place flags become an integer.
	//
	// One accumulator, one `|=` per flag, one return. Adding a flag adds a line to
	// this fold; it cannot add a write, because writing is somewhere else entirely.
	constexpr render_settings_word compose_render_settings(const render_settings& settings)
	{
		int composed = 0;

		switch (settings.source)
		{
			case render_settings_source::master_mix:
				// The absence of the stem bits. Nothing to OR in.
				break;
			case render_settings_source::stems_and_master_mix:
				composed |= render_settings_flag::stems_and_master_mix;
				break;
			case render_settings_source::stems_only:
				composed |= render_settings_flag::stems_only;
				break;
			case render_settings_source::selected_tracks_via_master:
				composed |= render_settings_flag::selected_tracks_via_master;
				break;
		}

		if (settings.multichannel_tracks_to_multichannel_files)
		{
			composed |= render_settings_flag::multichannel_tracks_to_multichannel_files;
		}

		if (settings.use_render_matrix)
		{
			composed |= render_settings_flag::use_render_matrix;
		}

		if (settings.mono_media_to_mono_files)
		{
			composed |= render_settings_flag::mono_media_to_mono_files;
		}

		if (settings.embed_transients)
		{
			composed |= render_settings_flag::embed_transients;
		}

		if (settings.embed_metadata)
		{
			composed |= render_settings_flag::embed_metadata;
		}

		if (settings.embed_take_markers)
		{
			composed |= render_settings_flag::embed_take_markers;
		}

		if (settings.second_pass_render)
		{
			composed |= render_settings_flag::second_pass_render;
		}

		if (settings.render_razor_edits)
		{
			composed |= render_settings_flag::render_razor_edits;
		}

		if (settings.pre_fader_stems)
		{
			composed |= render_settings_flag::pre_fader_stems;
		}

		if (settings.only_stem_channels_sent_to_parent)
		{
			composed |= render_settings_flag::only_stem_channels_sent_to_parent;
		}

		return render_settings_word{composed};
	}

	// A value REAPER already holds, adopted rather than composed. Named so it is
	// obvious at the call site that this is not a fresh composition — the only
	// legitimate use is reading the project's current settings back.
	constexpr render_settings_word render_settings_word_as_read_from_reaper(int project_value)
	{
		return render_settings_word{project_value};
	}

	// The inverse of composition (Property 23).
	//
	// The source axis is read most-specific-first. Composition sets at most one of
	// the three bits, so the order only matters for a word that came out of REAPER
	// with several set — and there, reporting the via-master reading of a word that
	// has `&128` is the truthful one, since that is the Source entry REAPER shows.
	constexpr render_settings decompose_render_settings(render_settings_word word)
	{
		const int value = word.value();

		render_settings settings;

		if ((value & render_settings_flag::selected_tracks_via_master) != 0)
		{
			settings.source = render_settings_source::selected_tracks_via_master;
		}
		else if ((value & render_settings_flag::stems_only) != 0)
		{
			settings.source = render_settings_source::stems_only;
		}
		else if ((value & render_settings_flag::stems_and_master_mix) != 0)
		{
			settings.source = render_settings_source::stems_and_master_mix;
		}
		else
		{
			settings.source = render_settings_source::master_mix;
		}

		settings.multichannel_tracks_to_multichannel_files =
			(value & render_settings_flag::multichannel_tracks_to_multichannel_files) != 0;
		settings.use_render_matrix =
			(value & render_settings_flag::use_render_matrix) != 0;
		settings.mono_media_to_mono_files =
			(value & render_settings_flag::mono_media_to_mono_files) != 0;
		settings.embed_transients =
			(value & render_settings_flag::embed_transients) != 0;
		settings.embed_metadata =
			(value & render_settings_flag::embed_metadata) != 0;
		settings.embed_take_markers =
			(value & render_settings_flag::embed_take_markers) != 0;
		settings.second_pass_render =
			(value & render_settings_flag::second_pass_render) != 0;
		settings.render_razor_edits =
			(value & render_settings_flag::render_razor_edits) != 0;
		settings.pre_fader_stems =
			(value & render_settings_flag::pre_fader_stems) != 0;
		settings.only_stem_channels_sent_to_parent =
			(value & render_settings_flag::only_stem_channels_sent_to_parent) != 0;

		return settings;
	}

	// The Source entry each of the tool's three sources renders through.
	//
	// `stems_via_master` maps to `master_mix` and that is the whole of requirement
	// 12.6's mechanism: every job in the solo loop is a master mix render, and the
	// soloed track is what makes it a stem. The native `&128` is deliberately not
	// reached for here — see the header comment.
	inline constexpr render_settings_source settings_source_for(render_source source)
	{
		switch (source)
		{
			case render_source::master_mix:
				return render_settings_source::master_mix;
			case render_source::stems:
				return render_settings_source::stems_only;
			case render_source::stems_via_master:
				return render_settings_source::master_mix;
		}

		return render_settings_source::master_mix;
	}

	// The settings a render of this source needs, and nothing more.
	//
	// `stems_only` rather than `stems_and_master_mix`: ADR 0018 rejected a combined
	// value because a producer asking for stems should receive stems, not stems and
	// an extra file they did not mention. The tool's output is predictable from its
	// input, which matters more than saving a second call.
	//
	// `pre_fader_stems` stays off — plain stems are already pre-master, and taking the
	// fader out as well is a different request that the tool does not expose.
	inline constexpr render_settings render_settings_for(render_source source)
	{
		render_settings settings;
		settings.source = settings_source_for(source);

		return settings;
	}

	// -----------------------------------------------------------------------
	// RENDER_BOUNDSFLAG
	// -----------------------------------------------------------------------

	// `GetSetProjectInfo`'s `RENDER_BOUNDSFLAG`: 0=custom time bounds, 1=entire
	// project, 2=time selection, 3=all project regions, 4=selected media items,
	// 5=selected project regions, 6=all project markers, 7=selected project markers.
	namespace render_bounds_flag
	{
		inline constexpr int custom_time_bounds = 0;
		inline constexpr int entire_project = 1;
		inline constexpr int time_selection = 2;
	}

	// A region renders as custom time bounds rather than as flag 5.
	//
	// Flag 5 renders whichever regions happen to be *selected*, which would mean
	// mutating the producer's region selection to express "this region" and leaving it
	// mutated — a side effect on a tool whose undo effect is `none` and which
	// therefore has no way to put it back. Resolving the region's GUID to a start and
	// an end instead touches nothing, and it produces the `startSeconds` and
	// `endSeconds` the output schema requires anyway: a producer told "queued the
	// chorus" should be told which seconds that turned out to be.
	//
	// The cost is that `$region` wildcards in the output pattern have no selected
	// region to resolve against. The default pattern below does not use them.
	inline constexpr int bounds_flag_for(render_bounds bounds)
	{
		switch (bounds)
		{
			case render_bounds::region:
				return render_bounds_flag::custom_time_bounds;
			case render_bounds::time_selection:
				return render_bounds_flag::time_selection;
			case render_bounds::project:
				return render_bounds_flag::entire_project;
		}

		return render_bounds_flag::entire_project;
	}

	// -----------------------------------------------------------------------
	// Output naming
	// -----------------------------------------------------------------------

	// `RENDER_PATTERN`, the wildcard template REAPER resolves into file names.
	//
	// The per-track sources must carry `$track`, and that is load-bearing rather than
	// cosmetic. Without it every job in a stem set resolves to the same name, which is
	// exactly the self-collision requirement 12.4 refuses — so getting this wrong
	// produces a refusal rather than eight files overwriting each other, which is the
	// right direction for the mistake to fall but still a mistake worth not making.
	inline constexpr std::string_view default_render_pattern_for(render_source source)
	{
		switch (source)
		{
			case render_source::master_mix:
				return "$project";
			case render_source::stems:
			case render_source::stems_via_master:
				return "$project - $track";
		}

		return "$project";
	}

	// What the host needs in order to write the render format.
	struct render_format_request
	{
		// The four-character sink code, which selects the sink type with its default
		// settings.
		std::string_view sink_four_character_code;

		// The requested bit depth, when the format has one and the call named one.
		// Absent means the project setting stands. Encoding this needs a base64 sink
		// configuration rather than the four-character form, which is the host's
		// business — see `sink_four_character_code_for`.
		std::optional<int> bit_depth;
	};

	// -----------------------------------------------------------------------
	// Paths
	// -----------------------------------------------------------------------

	// `RENDER_TARGETS` comes back as a semicolon-separated list of the files that
	// would be written using the current render settings. Splitting it is the only
	// part of path resolution this side owns — the resolution itself is REAPER's,
	// which is the point: the paths collision detection runs against are the paths
	// REAPER would actually write, not this component's guess at what its own
	// wildcards mean.
	//
	// Empty entries are dropped rather than kept as empty paths, since a trailing
	// separator is not a file.
	inline std::vector<std::string> split_render_targets(std::string_view render_targets)
	{
		std::vector<std::string> paths;

		std::size_t entry_start = 0;

		while (entry_start <= render_targets.size())
		{
			const std::size_t separator = render_targets.find(';', entry_start);
			const std::size_t entry_end =
				(separator == std::string_view::npos) ? render_targets.size() : separator;

			std::string_view entry = render_targets.substr(entry_start, entry_end - entry_start);

			// REAPER does not pad the list, but a hand-written setting or a future
			// format change could, and a path with leading whitespace is a different
			// file from one without.
			while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t'))
			{
				entry.remove_prefix(1);
			}

			while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t'))
			{
				entry.remove_suffix(1);
			}

			if (!entry.empty())
			{
				paths.emplace_back(entry);
			}

			if (separator == std::string_view::npos)
			{
				break;
			}

			entry_start = separator + 1;
		}

		return paths;
	}

	// The form two paths are compared in to decide whether they are the same file.
	//
	// Two normalizations, both because the extension ships on three platforms and two
	// of them would let a "different" path clobber an existing file:
	//
	//   - Backslashes become forward slashes. On Windows both separate directories,
	//     and a pattern that produced one while the render directory used the other
	//     would compare unequal and write to one file.
	//   - ASCII case is folded. macOS and Windows filesystems are case-insensitive by
	//     default, so `Kick.wav` and `kick.wav` are one file there. Folding on Linux
	//     too costs a refusal for a pair of paths that could coexist on that one
	//     platform, and buys the same answer everywhere — which is worth more than the
	//     edge case, since a producer's stem set that renders on Linux and clobbers
	//     itself on their laptop is the worse outcome.
	//
	// ASCII only. Case folding outside ASCII is locale- and normalization-dependent,
	// and a half-correct Unicode fold is worse than a declared ASCII one.
	//
	// The normalized form is never reported. `blocking` and `outputPaths` carry the
	// paths as REAPER resolved them, because those are the paths the producer will
	// look for.
	inline std::string normalize_output_path_for_comparison(std::string_view path)
	{
		std::string normalized;
		normalized.reserve(path.size());

		for (const char character : path)
		{
			if (character == '\\')
			{
				normalized.push_back('/');
				continue;
			}

			if (character >= 'A' && character <= 'Z')
			{
				normalized.push_back(static_cast<char>(character - 'A' + 'a'));
				continue;
			}

			normalized.push_back(character);
		}

		return normalized;
	}

	// The paths this render would write more than once (requirement 12.4).
	//
	// Each colliding path is reported once, in the order it first appeared, so the
	// refusal enumerates files rather than pairs and reads in the order REAPER would
	// have written them. Reported in resolved form; compared in normalized form.
	inline std::vector<std::string> find_self_colliding_output_paths(
		const std::vector<std::string>& resolved_paths)
	{
		std::vector<std::string> normalized_seen;
		std::vector<std::string> normalized_reported;
		std::vector<std::string> colliding_paths;

		normalized_seen.reserve(resolved_paths.size());

		const auto contains = [](const std::vector<std::string>& haystack, const std::string& needle) {
			for (const std::string& candidate : haystack)
			{
				if (candidate == needle)
				{
					return true;
				}
			}

			return false;
		};

		for (const std::string& resolved_path : resolved_paths)
		{
			const std::string normalized = normalize_output_path_for_comparison(resolved_path);

			if (!contains(normalized_seen, normalized))
			{
				normalized_seen.push_back(normalized);
				continue;
			}

			if (contains(normalized_reported, normalized))
			{
				continue;
			}

			normalized_reported.push_back(normalized);
			colliding_paths.push_back(resolved_path);
		}

		return colliding_paths;
	}

	// -----------------------------------------------------------------------
	// Tracks and project state
	// -----------------------------------------------------------------------

	// One selected track, as much of it as rendering depends on.
	struct render_track
	{
		// The stable REAPER GUID, in the braced form the refusal schema's `guid`
		// pattern expects. Opaque here — nothing in this file parses it.
		std::string guid;

		std::string name;
	};

	// A resolved span on the timeline.
	struct time_span
	{
		double start_seconds = 0.0;
		double end_seconds = 0.0;

		constexpr bool is_positive_length() const { return end_seconds > start_seconds; }
	};

	// The project globals a render depends on.
	struct render_project_state
	{
		double project_length_seconds = 0.0;
		time_span time_selection{};
		int sample_rate = 48000;
	};

	// -----------------------------------------------------------------------
	// The request
	// -----------------------------------------------------------------------

	// `render.schema.json`, as the handler hands it over.
	//
	// No `execution` and no `confirmedLongRender`: the input schema was corrected to
	// match requirements 12.1 and 12.7 and both properties are gone. A field for
	// choosing a synchronous render would be a field somebody eventually wires up.
	//
	// `tracks` is absent too. The selectors are resolved by the Object Resolver before
	// this component runs, and what arrives here is the resolved selection from
	// `RenderHost::read_selected_tracks`.
	struct render_request
	{
		render_source source = render_source::master_mix;
		render_bounds bounds = render_bounds::project;

		// Required when `bounds` is `region`, meaningless otherwise.
		std::string region_guid;

		render_output_format output_format = render_output_format::wav;

		// Absent means the project setting stands.
		std::optional<int> bit_depth;

		// Absent means the project sample rate, which is usually what is wanted. The
		// result reports the rate that will actually be used either way.
		std::optional<int> sample_rate;

		// Set only after a refusal listed the colliding files and the producer
		// approved overwriting those specific files.
		bool confirmed_overwrite = false;
	};

	// -----------------------------------------------------------------------
	// The outcome
	// -----------------------------------------------------------------------

	// One entity standing in the way, matching `blocking[]` in the refusal schema.
	struct render_blocking_entity
	{
		// `file_path` or `track`.
		std::string kind;

		// Human-readable identification — a resolved path, or a track name.
		std::string description;

		// Present for a track, so the agent can address it on the retry. Empty for a
		// path, which has no GUID, and the schema omits the property rather than
		// sending an empty string.
		std::string guid;
	};

	// `messages/tool-result-refusal.schema.json`. A refusal replaces the result
	// rather than accompanying it: the operation was understood and deliberately not
	// performed, and nothing was queued.
	struct render_refusal
	{
		std::string reason;
		std::vector<render_blocking_entity> blocking;

		// The input property the agent sets on the retry. Empty when no
		// acknowledgement can change the answer — two of this render's own paths being
		// equal is not something approval fixes.
		std::string acknowledgement_field;
	};

	// `outputs/render.schema.json`. What was added to REAPER's render queue.
	//
	// No undo report, and that absence is requirement 12.7 — see the header comment.
	struct render_result
	{
		// Not always one. `stems_via_master` queues one job per selected track.
		int queued_job_count = 0;

		render_source source = render_source::master_mix;
		render_bounds bounds = render_bounds::project;

		// Resolved rather than echoed: `region` and `time_selection` both name a span
		// indirectly, and the producer should be told which seconds it turned out to be.
		double start_seconds = 0.0;
		double end_seconds = 0.0;

		// Echoed when the bounds were a region. Empty otherwise.
		std::string region_guid;

		std::vector<render_track> tracks;

		std::string output_directory;

		// Every file the queued jobs will write, resolved in full. Enumerated rather
		// than counted because collision detection ran against exactly these paths
		// before anything was queued.
		std::vector<std::string> output_paths;

		render_output_format output_format = render_output_format::wav;

		// Absent for mp3, which has none, and when the call left it to the project.
		std::optional<int> bit_depth;

		// Always resolved, so the agent reports the rate that will be used rather
		// than the absence of one.
		int sample_rate = 0;
	};

	// Three outcomes, and the distinction between the last two matters.
	//
	// A refusal is a precondition the producer can resolve: it names what is in the
	// way and which field to set on the retry, and the MCP Tool Server maps it to
	// `precondition_unmet`. A failure is the call not making sense — a region GUID
	// that resolves to nothing, an empty time selection, no selected tracks for a stem
	// render. Collapsing them would have the agent offering the producer an
	// acknowledgement for something no acknowledgement fixes.
	enum class render_outcome_status
	{
		queued,
		refused,
		failed
	};

	struct render_outcome
	{
		render_outcome_status status = render_outcome_status::failed;

		// Populated when `status` is `queued`.
		render_result result{};

		// Populated when `status` is `refused`.
		render_refusal refusal{};

		// Populated when `status` is `failed`. Never empty for a failure — a
		// reasonless failure is not a representable result.
		std::string failure_reason;
	};

	// -----------------------------------------------------------------------
	// The REAPER seam
	// -----------------------------------------------------------------------

	// Everything this component needs from REAPER, and nothing else.
	//
	// Note what is absent as much as what is present. There is no undo operation,
	// because the render tool's undo effect is `none` (requirement 12.7). There is no
	// per-flag render settings setter, because `RENDER_SETTINGS` is one integer
	// written once. There is no "render now", because the tool always queues
	// (requirement 12.1). Each absence is a requirement that cannot be violated
	// through this interface.
	class RenderHost
	{
	public:
		virtual ~RenderHost() = default;

		// --- reads ---

		virtual render_project_state read_project_state() = 0;

		// The resolved selection, in project order. The Object Resolver has already
		// turned the call's selectors into this.
		virtual std::vector<render_track> read_selected_tracks() = 0;

		// Empty when no region carries that GUID.
		virtual std::optional<time_span> find_region_span(const std::string& region_guid) = 0;

		// `RENDER_FILE` — the render directory, already resolved to an effective path.
		virtual std::string read_render_directory() = 0;

		// --- settings ---

		// The only render settings mutator, and it takes a whole composed word. Call
		// it twice and REAPER keeps the second value only, which is why the coordinator
		// reaches it through `single_render_settings_write` rather than directly.
		virtual void write_render_settings(render_settings_word settings) = 0;

		// `RENDER_BOUNDSFLAG`, plus `RENDER_STARTPOS`/`RENDER_ENDPOS` when the flag is
		// custom bounds. One call, because the three are one decision.
		virtual void write_render_bounds(int bounds_flag, const time_span& span) = 0;

		// `RENDER_SRATE`.
		virtual void write_render_sample_rate(int sample_rate) = 0;

		// `RENDER_FORMAT`.
		virtual void write_render_format(const render_format_request& format) = 0;

		// `RENDER_FILE` and `RENDER_PATTERN`.
		virtual void write_render_output(
			const std::string& directory,
			const std::string& file_name_pattern) = 0;

		// --- path resolution, which is REAPER's own ---

		// `RENDER_TARGETS` — the semicolon-separated list of files that would be
		// written using the settings as they now stand. Reading this after the settings
		// are written is what makes collision detection run against REAPER's wildcard
		// resolution rather than against a reimplementation of it.
		virtual std::string read_render_targets() = 0;

		// Whether a resolved path already exists. The overwrite half of collision
		// detection, and the reason `confirmedOverwrite` exists.
		virtual bool output_file_exists(const std::string& resolved_path) = 0;

		// --- solo, for requirement 12.6's loop ---

		virtual void set_track_solo(const std::string& track_guid, bool soloed) = 0;

		// --- the queue ---

		// The "add project to render queue, using the most recent render settings"
		// action, through `Main_OnCommand`. Returns and writes nothing.
		virtual void add_project_to_render_queue() = 0;
	};

	// The coordinator's only route to `RenderHost::write_render_settings`, permitting
	// one write for the lifetime of the object — which is one render call.
	//
	// This is the runtime half of "OR-ed and written once". The type system already
	// makes a partially composed word unconstructible; this makes a second write of a
	// fully composed one fail loudly instead of silently replacing the first. Both
	// halves are needed: the first rules out building the value wrong, the second
	// rules out writing a correct value twice, and it is the second that turns
	// `&2` then `&8` into `8`.
	//
	// The count is observable so the suite can assert the property rather than trust
	// it.
	class single_render_settings_write final
	{
	public:
		explicit single_render_settings_write(RenderHost& host)
			: host_{host}
		{
		}

		single_render_settings_write(const single_render_settings_write&) = delete;
		single_render_settings_write& operator=(const single_render_settings_write&) = delete;
		single_render_settings_write(single_render_settings_write&&) = delete;
		single_render_settings_write& operator=(single_render_settings_write&&) = delete;

		// False when a write already happened. The host is not touched in that case,
		// so the first value stands.
		bool write(render_settings_word settings)
		{
			if (writes_performed_ > 0)
			{
				return false;
			}

			++writes_performed_;
			host_.write_render_settings(settings);

			return true;
		}

		int writes_performed() const { return writes_performed_; }

	private:
		RenderHost& host_;
		int writes_performed_ = 0;
	};

	// -----------------------------------------------------------------------
	// The coordinator
	// -----------------------------------------------------------------------

	// Queues render jobs and reports what it queued.
	//
	// The order of operations is the requirement, and it is one direction only:
	// resolve the span, check the preconditions that do not need REAPER, write the
	// settings once, ask REAPER what files it would write, check the path
	// preconditions, and only then touch the queue. Nothing before the last step is
	// observable to the producer as a change, so every refusal happens with the
	// project as it was found and the queue untouched.
	class render_coordinator final
	{
	public:
		explicit render_coordinator(RenderHost& host)
			: host_{host}
		{
		}

		render_outcome queue_render(const render_request& request)
		{
			const render_project_state project_state = host_.read_project_state();
			const std::vector<render_track> selected_tracks = host_.read_selected_tracks();

			// --- the span ---

			std::optional<time_span> span = resolve_bounds(request, project_state);

			if (!span.has_value())
			{
				return failed(bounds_failure_reason(request));
			}

			if (!span->is_positive_length())
			{
				// The same end-greater-than-start check requirement 9.6 applies to
				// regions and time selections, applied to whatever the bounds resolved
				// to. A zero-length span also cannot satisfy the output schema, whose
				// `endSeconds` has an exclusive minimum of zero.
				return failed(
					"the resolved render bounds have no length — end must be greater than start");
			}

			// --- preconditions that need no REAPER writes ---

			if (render_source_writes_a_file_per_track(request.source) && selected_tracks.empty())
			{
				return failed(
					"a per-track render needs at least one selected track, and none are selected");
			}

			// --- settings, written exactly once ---

			single_render_settings_write settings_write{host_};

			if (!settings_write.write(compose_render_settings(render_settings_for(request.source))))
			{
				// Unreachable: the gate is fresh and this is its first use. Guarded
				// rather than asserted, because the failure it would represent is a
				// render whose source silently became something else.
				return failed("the render settings were already written for this call");
			}

			host_.write_render_bounds(bounds_flag_for(request.bounds), *span);

			const int resolved_sample_rate = resolve_sample_rate(request, project_state);
			host_.write_render_sample_rate(resolved_sample_rate);

			const std::optional<int> resolved_bit_depth = resolve_bit_depth(request);
			host_.write_render_format(render_format_request{
				sink_four_character_code_for(request.output_format),
				resolved_bit_depth,
			});

			const std::string output_directory = host_.read_render_directory();
			host_.write_render_output(
				output_directory,
				std::string{default_render_pattern_for(request.source)});

			// --- paths, resolved by REAPER ---

			const std::vector<std::string> resolved_paths = resolve_output_paths(request, selected_tracks);

			if (resolved_paths.empty())
			{
				return failed(
					"REAPER resolved no output paths for these render settings, so there is "
					"nothing to queue");
			}

			// --- path preconditions, before the queue is touched ---

			const std::vector<std::string> self_colliding_paths =
				find_self_colliding_output_paths(resolved_paths);

			if (!self_colliding_paths.empty())
			{
				return refused(self_collision_refusal(self_colliding_paths));
			}

			if (!request.confirmed_overwrite)
			{
				const std::vector<std::string> existing_paths = find_existing_output_paths(resolved_paths);

				if (!existing_paths.empty())
				{
					return refused(existing_file_refusal(existing_paths));
				}
			}

			// --- the queue ---

			const int queued_job_count = add_jobs_to_render_queue(request, selected_tracks);

			if (queued_job_count < 1)
			{
				// Unreachable given the emptiness check above, and guarded because the
				// output schema's `queuedJobCount` has a minimum of one — a zero would
				// be a result that fails the extension's own outbound validation.
				return failed("no render jobs were queued");
			}

			render_outcome outcome;
			outcome.status = render_outcome_status::queued;

			render_result& result = outcome.result;
			result.queued_job_count = queued_job_count;
			result.source = request.source;
			result.bounds = request.bounds;
			result.start_seconds = span->start_seconds;
			result.end_seconds = span->end_seconds;
			result.region_guid = (request.bounds == render_bounds::region) ? request.region_guid : std::string{};
			result.tracks = selected_tracks;
			result.output_directory = output_directory;
			result.output_paths = resolved_paths;
			result.output_format = request.output_format;
			result.bit_depth = resolved_bit_depth;
			result.sample_rate = resolved_sample_rate;

			return outcome;
		}

	private:
		std::optional<time_span> resolve_bounds(
			const render_request& request,
			const render_project_state& project_state) const
		{
			switch (request.bounds)
			{
				case render_bounds::project:
					return time_span{0.0, project_state.project_length_seconds};
				case render_bounds::time_selection:
					return project_state.time_selection;
				case render_bounds::region:
					if (request.region_guid.empty())
					{
						return std::nullopt;
					}

					return host_.find_region_span(request.region_guid);
			}

			return std::nullopt;
		}

		static std::string bounds_failure_reason(const render_request& request)
		{
			if (request.bounds != render_bounds::region)
			{
				return "the render bounds could not be resolved";
			}

			if (request.region_guid.empty())
			{
				return "region bounds need a regionGuid, and none was given";
			}

			return "no region in this project carries the GUID " + request.region_guid;
		}

		static int resolve_sample_rate(
			const render_request& request,
			const render_project_state& project_state)
		{
			// Zero is REAPER's own "use the project sample rate" for `RENDER_SRATE`,
			// but the result has to name the rate that will actually be used, so the
			// project's rate is resolved here and written explicitly.
			if (request.sample_rate.has_value())
			{
				return *request.sample_rate;
			}

			return project_state.sample_rate;
		}

		static std::optional<int> resolve_bit_depth(const render_request& request)
		{
			if (!bit_depth_applies_to(request.output_format))
			{
				return std::nullopt;
			}

			return request.bit_depth;
		}

		// Every file the queued jobs would write, as REAPER resolves them.
		//
		// The per-track sources are asked track by track under solo, because that is
		// what requirement 12.6's loop will queue and therefore what has to be checked
		// for collisions. `stems` is one job whose settings already name the selection,
		// so REAPER answers for the whole set in one read.
		//
		// Solo is restored before returning. This pass writes no files and queues
		// nothing — it only asks REAPER what it would write — so a refusal after it
		// leaves the project as it was found.
		std::vector<std::string> resolve_output_paths(
			const render_request& request,
			const std::vector<render_track>& selected_tracks)
		{
			if (request.source != render_source::stems_via_master)
			{
				return split_render_targets(host_.read_render_targets());
			}

			std::vector<std::string> resolved_paths;

			for (const render_track& track : selected_tracks)
			{
				host_.set_track_solo(track.guid, true);

				const std::vector<std::string> paths_for_track =
					split_render_targets(host_.read_render_targets());

				host_.set_track_solo(track.guid, false);

				resolved_paths.insert(
					resolved_paths.end(),
					paths_for_track.begin(),
					paths_for_track.end());
			}

			return resolved_paths;
		}

		std::vector<std::string> find_existing_output_paths(
			const std::vector<std::string>& resolved_paths)
		{
			std::vector<std::string> existing_paths;

			for (const std::string& resolved_path : resolved_paths)
			{
				if (host_.output_file_exists(resolved_path))
				{
					existing_paths.push_back(resolved_path);
				}
			}

			return existing_paths;
		}

		// One job for a master mix or a stem set; one job per selected track for
		// `stems_via_master`.
		//
		// The loop is unconditional over the selection, which requirement 12.6 states
		// explicitly — including tracks that look ineligible, such as folder parents.
		// There is no filter here to get wrong, so `queuedJobCount` is the selected
		// track count by construction rather than by arithmetic that could disagree
		// with what was queued.
		//
		// Unsolo happens on every iteration including the last, so the producer's solo
		// state is where they left it when the call returns.
		int add_jobs_to_render_queue(
			const render_request& request,
			const std::vector<render_track>& selected_tracks)
		{
			if (request.source != render_source::stems_via_master)
			{
				host_.add_project_to_render_queue();

				return 1;
			}

			int queued_job_count = 0;

			for (const render_track& track : selected_tracks)
			{
				host_.set_track_solo(track.guid, true);
				host_.add_project_to_render_queue();
				host_.set_track_solo(track.guid, false);

				++queued_job_count;
			}

			return queued_job_count;
		}

		static render_outcome failed(std::string failure_reason)
		{
			render_outcome outcome;
			outcome.status = render_outcome_status::failed;
			outcome.failure_reason = std::move(failure_reason);

			return outcome;
		}

		static render_outcome refused(render_refusal refusal)
		{
			render_outcome outcome;
			outcome.status = render_outcome_status::refused;
			outcome.refusal = std::move(refusal);

			return outcome;
		}

		static render_refusal self_collision_refusal(const std::vector<std::string>& colliding_paths)
		{
			render_refusal refusal;
			refusal.reason = std::string{output_file_collision_refusal_reason};

			// Deliberately no acknowledgement field. Two jobs writing one path is not
			// a consequence the producer can approve — approving it would mean
			// approving that one of the files is silently not produced.
			for (const std::string& colliding_path : colliding_paths)
			{
				refusal.blocking.push_back(render_blocking_entity{
					std::string{file_path_blocking_kind},
					colliding_path,
					std::string{},
				});
			}

			return refusal;
		}

		static render_refusal existing_file_refusal(const std::vector<std::string>& existing_paths)
		{
			render_refusal refusal;
			refusal.reason = std::string{output_file_collision_refusal_reason};
			refusal.acknowledgement_field = std::string{overwrite_acknowledgement_field};

			for (const std::string& existing_path : existing_paths)
			{
				refusal.blocking.push_back(render_blocking_entity{
					std::string{file_path_blocking_kind},
					existing_path,
					std::string{},
				});
			}

			return refusal;
		}

		RenderHost& host_;
	};
}

#endif
