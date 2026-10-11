// Render Coordinator — Properties 23, 24, and 25 of the design, stated as properties.
//
// `render_coordinator_test.cpp` beside this file is the example file, and it is not
// thin: it already enumerates all 4096 compositions for the round trip, and it has
// hand-written cases for both collision kinds, for the ordering of collision
// detection against the queue call, and for the solo loop. This file is deliberately
// not a second copy of that under property-shaped names. It adds the three things
// those cases quantify over too narrowly.
//
// **Validates: Requirements 12.2, 12.4, 9.8**
//
// ---------------------------------------------------------------------------
// Property 23 — and a direction that does not round-trip, which is not a bug
//
// The example file sweeps every *composition*: four sources crossed with ten
// independent flags, 4096 settings values, and each one decomposes back to itself.
// That direction is settled, and repeating it here would buy nothing.
//
// The direction nobody covered is the other one. `render_settings_word_as_read_from_reaper`
// exists precisely so a value REAPER already holds can be adopted rather than
// composed, and REAPER's `RENDER_SETTINGS` is a project field a producer, a script,
// or a future REAPER build can set to anything at all. So the quantifier that matters
// is over arbitrary **integers**, including two shapes composition can never produce:
//
//   - several source-axis bits set at once — `&1|&2`, or `&2|&128`
//   - bits this header does not name, whether the documented-but-unmodelled `&32`
//     and `&64` for selected media items via master, or bits above them that a later
//     REAPER adds
//
// Over that space the two directions are **not** symmetric, and the honest statement
// of Property 23 has to say which is which:
//
//   - `settings -> word -> settings` **is** the identity. Every settings value is
//     reachable and recovered exactly. This is the round trip the property claims and
//     the example file's sweep establishes over the whole reachable space.
//
//   - `word -> settings -> word` is **not** the identity, and asserting that it were
//     would be asserting a bug that is not one. Decomposition reads the source axis
//     most-specific-first, as the header documents, so `&1|&2` and `&2` decompose to
//     the same `stems_only` — information is deliberately discarded, because REAPER's
//     Source list shows one entry and the truthful reading of a word carrying `&128`
//     is the via-master one. Unknown bits are dropped for the same reason: a model
//     that cannot name a bit cannot carry it.
//
// What that direction *is* is a **retraction**: composition after decomposition
// canonicalises. `compose(decompose(w))` equals a value this file computes
// independently — unknown bits cleared, the source axis reduced to its most specific
// bit — and canonicalising twice changes nothing. That is a real and checkable law,
// and it is strictly stronger than "it does not crash", which is the other thing
// worth knowing about a total decomposition of hostile input.
//
// So the sweep below is exhaustive over `0 .. 32767` — every combination of the
// thirteen named bits, the two documented-but-unmodelled ones, and nothing above —
// then seeded over full-width integers including negatives, which is what a
// `GetSetProjectInfo` value cast from a double can be.
//
// ---------------------------------------------------------------------------
// Property 24 — over generated path sets, not chosen ones
//
// The example file's collision cases each pick a path set that makes a point: three
// identical paths, two that differ in case, two that differ in separator. Requirement
// 12.4 is a claim about *any* set of resolved paths, and the arrangement that breaks a
// duplicate detector is rarely the arrangement somebody wrote down — three occurrences
// where the second and third must not both be reported, a collision between the first
// and last entry of a twelve-path set, a case-folded duplicate that is also an
// existing file.
//
// So path sets are generated here: arbitrary length, arbitrary duplicate arrangement,
// and duplicates that differ only in ASCII case or only in separator, because the
// implementation normalises both for comparison and reports neither normalised.
//
// Every collision is checked for the same three things, which is the part of
// requirement 12.4 and 9.8 that actually matters:
//
//   - **nothing was queued** — the queue addition count is zero and no queue call
//     appears anywhere in the recorded sequence, so "before any file is written" is
//     asserted against the order of operations rather than inferred from the outcome
//   - **the colliding paths are enumerated**, in resolved form, against an expectation
//     computed here by a different method than the implementation uses
//   - **the right refusal** — a self-collision carries no acknowledgement field because
//     no approval makes two jobs writing one file the intent, while an existing file
//     names `confirmedOverwrite`
//
// With the negative control, because a property satisfied by a coordinator that
// refused everything would be worthless: a set with no duplicates and no existing
// file queues, and an acknowledged overwrite queues.
//
// ---------------------------------------------------------------------------
// Property 25 — determinism, and what that claim can and cannot contain here
//
// This one needs stating carefully, because the obvious test of it is vacuous. The
// fake host answers `RENDER_TARGETS` from a fixed string, so "the same inputs resolve
// to the same paths" would be true of a coordinator that did nothing but echo that
// string, and REAPER's own wildcard resolution — which is where the paths actually
// come from, by design, see `resolve_output_paths` — is not exercised at all.
//
// What the claim does contain here, and it is not nothing:
//
//   - The coordinator introduces **no nondeterminism of its own**. Repeated calls with
//     identical inputs produce an identical outcome *and an identical sequence of host
//     calls* — same reads, same writes, same order, same count. That rules out
//     iteration over an unordered container, a dependence on anything ambient, and a
//     read of something uninitialised, all of which would show up as a log that
//     differs between two runs that should be indistinguishable.
//   - Resolution is a function of the **inputs that are supposed to matter**. Varying
//     only the sample rate and the bit depth — which change the settings written and
//     the reported result, and must not change which files are resolved — leaves the
//     resolved paths and the recorded call sequence identical.
//   - The per-track ordering is **project order, twice**. For `stems_via_master` the
//     resolution pass and the queue pass each walk the selection, and the property
//     asserts both walks visit the same tracks in the same order, which is what makes
//     the paths that collision detection ran against the paths the queued jobs will
//     write.
//
// What it cannot contain: whether REAPER resolves the same wildcards to the same
// files twice. That is REAPER's determinism, not this component's, and checking it
// needs a real REAPER and belongs to the SDK validation step. The seam is what makes
// the distinction clean — this side owns the questions asked and the order they are
// asked in, and nothing else.
//
// ---------------------------------------------------------------------------
// The fake host is a second copy on purpose
//
// `recording_render_host` in the example file lives in that file's anonymous
// namespace and cannot be reached from here. Rather than move it — that file belongs
// to another task and other agents are working concurrently — this file carries its
// own, with the two capabilities these properties need: the queue addition count and
// the ordered call log.
//
// Nothing here includes the REAPER SDK, for the reason `render_coordinator.h`
// explains: the test target has no SDK include path, and that is what keeps settings
// composition, path resolution, and collision detection testable at all.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/render_coordinator.h>

namespace
{
	using namespace sesh_ai::daw;

	// -------------------------------------------------------------------
	// Deterministic byte source
	// -------------------------------------------------------------------

	// The same xorshift `tests/daw/alias_store_test.cpp` uses, for the same reason:
	// the seed is in the failure message, so a counterexample found on CI is a
	// counterexample anybody can replay.
	class DeterministicBytes
	{
	public:
		explicit DeterministicBytes(std::uint32_t seed)
			: state_{seed == 0 ? 0x9e3779b9u : seed}
		{
		}

		std::uint32_t next()
		{
			state_ ^= state_ << 13;
			state_ ^= state_ >> 17;
			state_ ^= state_ << 5;

			return state_;
		}

		std::size_t below(std::size_t bound) { return bound == 0 ? 0 : next() % bound; }

	private:
		std::uint32_t state_;
	};

	// The seed as it is written in the source, so a failure message can be pasted
	// back.
	std::string describe_seed(std::uint32_t seed)
	{
		static const char* const hexadecimal_digits = "0123456789abcdef";

		std::string description = "seed 0x";

		for (int shift = 28; shift >= 0; shift -= 4)
		{
			description += hexadecimal_digits[(static_cast<std::uint32_t>(seed) >> shift) & 0xfu];
		}

		return description;
	}

	// -------------------------------------------------------------------
	// The fake host
	// -------------------------------------------------------------------

	// Answers from data and records the sequence of calls. See the header comment for
	// why this is a second copy rather than a shared fixture.
	class recording_render_host final : public RenderHost
	{
	public:
		// --- what it answers with ---

		render_project_state project_state{};
		std::vector<render_track> selected_tracks{};
		std::optional<time_span> region_span{};
		std::string render_directory{"/Users/producer/Music/Session/renders"};

		// The `RENDER_TARGETS` answer. When `render_targets_per_read` is non-empty it
		// is consumed one entry per read, which is how a `stems_via_master` resolution
		// pass gets a different answer per soloed track.
		std::string render_targets{};
		std::vector<std::string> render_targets_per_read{};

		// Paths that already exist on disk, compared verbatim.
		std::vector<std::string> existing_files{};

		// --- what it recorded ---

		std::vector<std::string> call_log{};
		std::vector<int> written_settings_values{};
		std::vector<int> written_sample_rates{};
		std::vector<std::optional<int>> written_bit_depths{};
		std::vector<std::string> soloed_track_guids{};
		std::vector<std::string> unsoloed_track_guids{};
		int queue_additions = 0;

		render_project_state read_project_state() override
		{
			call_log.emplace_back("read_project_state");
			return project_state;
		}

		std::vector<render_track> read_selected_tracks() override
		{
			call_log.emplace_back("read_selected_tracks");
			return selected_tracks;
		}

		std::optional<time_span> find_region_span(const std::string& region_guid) override
		{
			call_log.push_back("find_region_span:" + region_guid);
			return region_span;
		}

		std::string read_render_directory() override
		{
			call_log.emplace_back("read_render_directory");
			return render_directory;
		}

		void write_render_settings(render_settings_word settings) override
		{
			call_log.emplace_back("write_render_settings");
			written_settings_values.push_back(settings.value());
		}

		void write_render_bounds(int bounds_flag, const time_span& span) override
		{
			call_log.push_back("write_render_bounds:" + std::to_string(bounds_flag));
			static_cast<void>(span);
		}

		void write_render_sample_rate(int sample_rate) override
		{
			call_log.emplace_back("write_render_sample_rate");
			written_sample_rates.push_back(sample_rate);
		}

		void write_render_format(const render_format_request& format) override
		{
			call_log.emplace_back("write_render_format");
			written_bit_depths.push_back(format.bit_depth);
		}

		void write_render_output(
			const std::string& directory,
			const std::string& file_name_pattern) override
		{
			call_log.push_back("write_render_output:" + directory + "|" + file_name_pattern);
		}

		std::string read_render_targets() override
		{
			call_log.emplace_back("read_render_targets");

			if (!render_targets_per_read.empty())
			{
				const std::string answer = render_targets_per_read[next_targets_read_];

				if (next_targets_read_ + 1 < render_targets_per_read.size())
				{
					++next_targets_read_;
				}

				return answer;
			}

			return render_targets;
		}

		bool output_file_exists(const std::string& resolved_path) override
		{
			call_log.push_back("output_file_exists:" + resolved_path);

			return std::find(existing_files.begin(), existing_files.end(), resolved_path)
				!= existing_files.end();
		}

		void set_track_solo(const std::string& track_guid, bool soloed) override
		{
			call_log.push_back(std::string{soloed ? "solo_on:" : "solo_off:"} + track_guid);

			if (soloed)
			{
				soloed_track_guids.push_back(track_guid);
			}
			else
			{
				unsoloed_track_guids.push_back(track_guid);
			}
		}

		void add_project_to_render_queue() override
		{
			call_log.emplace_back("add_project_to_render_queue");
			++queue_additions;
		}

	private:
		std::size_t next_targets_read_ = 0;
	};

	// -------------------------------------------------------------------
	// Shared fixtures
	// -------------------------------------------------------------------

	render_project_state make_project_state()
	{
		render_project_state state;
		state.project_length_seconds = 214.5;
		state.time_selection = time_span{32.0, 64.0};
		state.sample_rate = 48000;

		return state;
	}

	// Distinct braced GUIDs, in the form the refusal schema's `guid` pattern expects.
	render_track make_numbered_track(std::size_t index)
	{
		std::string digit_run(12, static_cast<char>('0' + static_cast<int>(index % 10)));

		render_track track;
		track.guid = "{" + digit_run.substr(0, 8) + "-" + std::to_string(1000 + index).substr(0, 4)
			+ "-0000-0000-" + digit_run + "}";
		track.name = "Track " + std::to_string(index);

		return track;
	}

	std::vector<render_track> make_numbered_tracks(std::size_t count)
	{
		std::vector<render_track> tracks;
		tracks.reserve(count);

		for (std::size_t index = 0; index < count; ++index)
		{
			tracks.push_back(make_numbered_track(index));
		}

		return tracks;
	}

	std::vector<std::string> guids_of(const std::vector<render_track>& tracks)
	{
		std::vector<std::string> guids;
		guids.reserve(tracks.size());

		for (const render_track& track : tracks)
		{
			guids.push_back(track.guid);
		}

		return guids;
	}

	std::string join_with_semicolons(const std::vector<std::string>& paths)
	{
		std::string joined;

		for (std::size_t index = 0; index < paths.size(); ++index)
		{
			if (index > 0)
			{
				joined += ';';
			}

			joined += paths[index];
		}

		return joined;
	}

	std::string describe_paths(const std::vector<std::string>& paths)
	{
		std::string description = "[";

		for (std::size_t index = 0; index < paths.size(); ++index)
		{
			if (index > 0)
			{
				description += " ";
			}

			description += paths[index];
		}

		return description + "]";
	}

	std::vector<std::string> descriptions_of(const std::vector<render_blocking_entity>& blocking)
	{
		std::vector<std::string> descriptions;
		descriptions.reserve(blocking.size());

		for (const render_blocking_entity& entity : blocking)
		{
			descriptions.push_back(entity.description);
		}

		return descriptions;
	}

	int count_calls(const std::vector<std::string>& call_log, const std::string& name)
	{
		return static_cast<int>(std::count(call_log.begin(), call_log.end(), name));
	}

	// -------------------------------------------------------------------
	// Property 23: the reference canonicalisation
	// -------------------------------------------------------------------

	// Every bit the model names. A bit outside this cannot survive a decomposition,
	// because there is no field to put it in.
	constexpr int named_bits_mask =
		render_settings_flag::stems_and_master_mix
		| render_settings_flag::stems_only
		| render_settings_flag::selected_tracks_via_master
		| render_settings_flag::multichannel_tracks_to_multichannel_files
		| render_settings_flag::use_render_matrix
		| render_settings_flag::mono_media_to_mono_files
		| render_settings_flag::embed_transients
		| render_settings_flag::embed_metadata
		| render_settings_flag::embed_take_markers
		| render_settings_flag::second_pass_render
		| render_settings_flag::render_razor_edits
		| render_settings_flag::pre_fader_stems
		| render_settings_flag::only_stem_channels_sent_to_parent;

	// The bits documented for `RENDER_SETTINGS` that this model deliberately does not
	// carry: `&32` and `&64` name selected media items via master, which is not a
	// source this tool offers. A word from REAPER can have them, which is the whole
	// reason this file exists.
	constexpr int documented_but_unmodelled_bits_mask = 32 | 64;

	// Read from the header's stated rule — the source axis is read most-specific-first
	// — rather than by calling the implementation, so the two are able to disagree.
	int reference_source_bit(int word)
	{
		if ((word & render_settings_flag::selected_tracks_via_master) != 0)
		{
			return render_settings_flag::selected_tracks_via_master;
		}

		if ((word & render_settings_flag::stems_only) != 0)
		{
			return render_settings_flag::stems_only;
		}

		if ((word & render_settings_flag::stems_and_master_mix) != 0)
		{
			return render_settings_flag::stems_and_master_mix;
		}

		return 0;
	}

	render_settings_source reference_source(int word)
	{
		switch (reference_source_bit(word))
		{
			case render_settings_flag::selected_tracks_via_master:
				return render_settings_source::selected_tracks_via_master;
			case render_settings_flag::stems_only:
				return render_settings_source::stems_only;
			case render_settings_flag::stems_and_master_mix:
				return render_settings_source::stems_and_master_mix;
			default:
				return render_settings_source::master_mix;
		}
	}

	// What `compose(decompose(w))` has to be: the named non-source bits kept, the
	// source axis reduced to its most specific bit, everything else gone.
	int reference_canonical_word(int word)
	{
		const int named_non_source_bits =
			word & named_bits_mask & ~render_settings_flag::source_axis_mask;

		return named_non_source_bits | reference_source_bit(word);
	}

	// Decomposition read off the flag table independently of `decompose_render_settings`.
	render_settings reference_decomposition(int word)
	{
		render_settings settings;
		settings.source = reference_source(word);
		settings.multichannel_tracks_to_multichannel_files =
			(word & render_settings_flag::multichannel_tracks_to_multichannel_files) != 0;
		settings.use_render_matrix = (word & render_settings_flag::use_render_matrix) != 0;
		settings.mono_media_to_mono_files =
			(word & render_settings_flag::mono_media_to_mono_files) != 0;
		settings.embed_transients = (word & render_settings_flag::embed_transients) != 0;
		settings.embed_metadata = (word & render_settings_flag::embed_metadata) != 0;
		settings.embed_take_markers = (word & render_settings_flag::embed_take_markers) != 0;
		settings.second_pass_render = (word & render_settings_flag::second_pass_render) != 0;
		settings.render_razor_edits = (word & render_settings_flag::render_razor_edits) != 0;
		settings.pre_fader_stems = (word & render_settings_flag::pre_fader_stems) != 0;
		settings.only_stem_channels_sent_to_parent =
			(word & render_settings_flag::only_stem_channels_sent_to_parent) != 0;

		return settings;
	}

	bool is_a_named_source(render_settings_source source)
	{
		return source == render_settings_source::master_mix
			|| source == render_settings_source::stems_and_master_mix
			|| source == render_settings_source::stems_only
			|| source == render_settings_source::selected_tracks_via_master;
	}

	// Every claim Property 23 makes about one word REAPER could have produced.
	// Returns the first violation, so a failure names the word rather than a line
	// inside a loop.
	std::string settings_word_violation(int word)
	{
		const render_settings_word adopted = render_settings_word_as_read_from_reaper(word);

		if (adopted.value() != word)
		{
			return "adopting " + std::to_string(word) + " changed it to "
				+ std::to_string(adopted.value());
		}

		const render_settings decomposed = decompose_render_settings(adopted);

		// Total: a hostile word gets a reading, and the reading names a source that
		// exists rather than an invented one.
		if (!is_a_named_source(decomposed.source))
		{
			return "decomposing " + std::to_string(word) + " invented a source";
		}

		if (decomposed != reference_decomposition(word))
		{
			return "decomposing " + std::to_string(word)
				+ " disagreed with the flag table read independently";
		}

		// `word -> settings -> word` canonicalises rather than round-trips, and this
		// is the exact statement of what it does instead.
		const int recomposed = compose_render_settings(decomposed).value();

		if (recomposed != reference_canonical_word(word))
		{
			return "recomposing " + std::to_string(word) + " gave " + std::to_string(recomposed)
				+ " rather than the canonical " + std::to_string(reference_canonical_word(word));
		}

		// Canonicalising is idempotent, which is what makes it a retraction rather
		// than merely lossy.
		const render_settings twice =
			decompose_render_settings(render_settings_word_as_read_from_reaper(recomposed));

		if (twice != decomposed)
		{
			return "decomposing " + std::to_string(word) + " then its own recomposition "
				+ std::to_string(recomposed) + " gave two different readings";
		}

		if (compose_render_settings(twice).value() != recomposed)
		{
			return "canonicalising " + std::to_string(word) + " twice did not settle";
		}

		// `settings -> word -> settings` **is** the identity, restated on whatever
		// settings this word produced. The example file establishes it over the whole
		// reachable space; here it is the other half of the asymmetry.
		if (decompose_render_settings(compose_render_settings(decomposed)) != decomposed)
		{
			return "the settings read out of " + std::to_string(word) + " did not round-trip";
		}

		return "";
	}

	// The exhaustive span: every combination of the thirteen named bits and the two
	// documented-but-unmodelled ones. The highest named bit is `&16384`, so
	// `0 .. 32767` is exactly that span and nothing above it.
	constexpr int exhaustive_word_bound = 1 << 15;

	// Measured, not guessed. Of the 32768 words, the source-axis bits are three of the
	// fifteen and the unmodelled bits are two, so:
	//   identity words      = 4 source patterns * 2^10 other named bits = 4096
	//   canonicalised words = 32768 - 4096                             = 28672
	//   several source bits = 4 of 8 source patterns * 2^12            = 16384
	//   unmodelled bits set = 3 of 4 patterns * 2^13                   = 24576
	constexpr int expected_identity_words = 4096;
	constexpr int expected_canonicalised_words = 28672;
	constexpr int expected_words_with_several_source_bits = 16384;
	constexpr int expected_words_with_unmodelled_bits = 24576;

	constexpr std::uint32_t settings_word_seed = 0xc0ffee17u;
	constexpr int settings_word_iterations = 4000;

	// A full-width integer, built rather than cast: converting a `std::uint32_t` above
	// `INT_MAX` to `int` is implementation-defined before C++20, and this is a C++17
	// build. Flipping the magnitude reproduces the two's-complement pattern without
	// relying on that.
	int generate_full_width_word(DeterministicBytes& bytes)
	{
		const std::uint32_t raw = bytes.next();
		const int magnitude = static_cast<int>(raw & 0x7fffffffu);

		return ((raw & 0x80000000u) != 0) ? (-magnitude - 1) : magnitude;
	}

	// -------------------------------------------------------------------
	// Property 24: generated path sets and an independent collision oracle
	// -------------------------------------------------------------------

	// The comparison form, read off the header's stated rule: backslashes become
	// forward slashes, ASCII upper case folds down, nothing else changes.
	std::string reference_normalized_path(std::string_view path)
	{
		std::string normalized;
		normalized.reserve(path.size());

		for (const char character : path)
		{
			if (character == '\\')
			{
				normalized.push_back('/');
			}
			else if (character >= 'A' && character <= 'Z')
			{
				normalized.push_back(static_cast<char>(character - 'A' + 'a'));
			}
			else
			{
				normalized.push_back(character);
			}
		}

		return normalized;
	}

	// The paths a set writes more than once, by a different route than the
	// implementation takes: group every index by its normalised form, then report the
	// *second* occurrence of each group that has one. The implementation streams and
	// keeps two "already seen" lists; this groups and indexes. Two formulations
	// agreeing is evidence, one formulation agreeing with itself is not.
	std::vector<std::string> reference_self_collisions(const std::vector<std::string>& paths)
	{
		std::map<std::string, std::vector<std::size_t>> indices_by_normalized_form;

		for (std::size_t index = 0; index < paths.size(); ++index)
		{
			indices_by_normalized_form[reference_normalized_path(paths[index])].push_back(index);
		}

		std::vector<std::size_t> reported_indices;

		for (const std::pair<const std::string, std::vector<std::size_t>>& group :
			indices_by_normalized_form)
		{
			if (group.second.size() > 1)
			{
				reported_indices.push_back(group.second[1]);
			}
		}

		std::sort(reported_indices.begin(), reported_indices.end());

		std::vector<std::string> colliding_paths;
		colliding_paths.reserve(reported_indices.size());

		for (const std::size_t index : reported_indices)
		{
			colliding_paths.push_back(paths[index]);
		}

		return colliding_paths;
	}

	// Twelve base names, so a "distinct" set of up to twelve paths can be built
	// without repeating one by accident.
	const std::vector<std::string>& base_file_names()
	{
		static const std::vector<std::string> names{
			"Session - Kick.wav",
			"Session - Snare.wav",
			"Session - Bass.wav",
			"Session - Gtr L.wav",
			"Session - Gtr R.wav",
			"Session - Keys.wav",
			"Session - Vox.wav",
			"Session - Room.wav",
			"Session - Overheads.wav",
			"Session - Bus.wav",
			"Session - Ref.wav",
			"Session - Mix.wav",
		};

		return names;
	}

	constexpr std::size_t colliding_mode_name_pool = 4;
	constexpr std::size_t maximum_generated_path_count = 12;

	// How a path is spelled. All four spellings of one base name normalise to the same
	// comparison form, which is the whole point: the implementation folds ASCII case
	// and backslashes because macOS and Windows filesystems do, so two of these are
	// one file there.
	constexpr int spelling_as_resolved = 0;
	constexpr int spelling_upper_case = 1;
	constexpr int spelling_backslashes = 2;
	constexpr int spelling_upper_case_backslashes = 3;

	std::string spell_path(const std::string& base_file_name, int spelling)
	{
		const bool upper_case =
			spelling == spelling_upper_case || spelling == spelling_upper_case_backslashes;
		const bool backslashes =
			spelling == spelling_backslashes || spelling == spelling_upper_case_backslashes;

		std::string path = "/Users/producer/Music/Session/renders/" + base_file_name;

		// The spelling the resolver itself would produce, returned before either
		// transformation rather than falling through both of them. Equivalent either
		// way — neither flag is set for it — and it names the fourth constant, which
		// is otherwise declared and never referred to.
		if (spelling == spelling_as_resolved)
		{
			return path;
		}

		if (upper_case)
		{
			for (char& character : path)
			{
				if (character >= 'a' && character <= 'z')
				{
					character = static_cast<char>(character - 'a' + 'A');
				}
			}
		}

		if (backslashes)
		{
			for (char& character : path)
			{
				if (character == '/')
				{
					character = '\\';
				}
			}
		}

		return path;
	}

	// A path set. `distinct` sets take one base name per slot and can only collide
	// through nothing; the rest draw from a pool of four, which is how duplicates,
	// triples, and duplicates that differ only in spelling all turn up.
	std::vector<std::string> generate_path_set(DeterministicBytes& bytes, bool distinct)
	{
		const std::size_t count = 1 + bytes.below(maximum_generated_path_count);

		std::vector<std::string> paths;
		paths.reserve(count);

		for (std::size_t slot = 0; slot < count; ++slot)
		{
			const std::size_t name_index = distinct ? slot : bytes.below(colliding_mode_name_pool);
			const int spelling = static_cast<int>(bytes.below(4));

			paths.push_back(spell_path(base_file_names()[name_index], spelling));
		}

		return paths;
	}

	// A host set up for a stem render whose resolved paths are exactly `paths`.
	void configure_stem_host(
		recording_render_host& host,
		const std::vector<std::string>& paths)
	{
		host.project_state = make_project_state();
		host.selected_tracks = make_numbered_tracks(3);
		host.render_targets = join_with_semicolons(paths);
	}

	constexpr std::uint32_t path_set_seed = 0x9a71c0deu;
	constexpr int path_set_iterations = 2000;

	// -------------------------------------------------------------------
	// Property 25: the configuration grid
	// -------------------------------------------------------------------

	struct render_configuration
	{
		render_source source;
		render_bounds bounds;
		std::size_t selected_track_count;
	};

	const std::string region_guid_for_determinism{"{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}"};

	std::string describe_configuration(const render_configuration& configuration)
	{
		return std::string{to_schema_string(configuration.source)} + " over "
			+ std::string{to_schema_string(configuration.bounds)} + " with "
			+ std::to_string(configuration.selected_track_count) + " selected tracks";
	}

	// One distinct resolved path per index, so a per-track source resolves a set that
	// does not collide with itself and the determinism claim is about a render that
	// actually gets queued.
	std::string distinct_path_for(std::size_t index)
	{
		return "/Users/producer/Music/Session/renders/Session - Track "
			+ std::to_string(index) + ".wav";
	}

	std::vector<std::string> distinct_paths(std::size_t count)
	{
		std::vector<std::string> paths;
		paths.reserve(count);

		for (std::size_t index = 0; index < count; ++index)
		{
			paths.push_back(distinct_path_for(index));
		}

		return paths;
	}

	// Freshly built every time, because a host records what was asked of it and a
	// second call against the same host would be comparing a log to itself plus more.
	void configure_host_for(recording_render_host& host, const render_configuration& configuration)
	{
		host.project_state = make_project_state();
		host.selected_tracks = make_numbered_tracks(configuration.selected_track_count);
		host.region_span = time_span{96.25, 128.75};

		const std::size_t path_count = std::max<std::size_t>(configuration.selected_track_count, 1);

		if (configuration.source == render_source::stems_via_master)
		{
			// One answer per soloed track, which is what the resolution pass asks for.
			host.render_targets_per_read = distinct_paths(path_count);
			host.render_targets = distinct_path_for(0);
		}
		else if (configuration.source == render_source::stems)
		{
			host.render_targets = join_with_semicolons(distinct_paths(path_count));
		}
		else
		{
			host.render_targets = distinct_path_for(0);
		}
	}

	render_request request_for(const render_configuration& configuration)
	{
		render_request request;
		request.source = configuration.source;
		request.bounds = configuration.bounds;

		if (configuration.bounds == render_bounds::region)
		{
			request.region_guid = region_guid_for_determinism;
		}

		return request;
	}

	// Everything about an outcome that resolution determines. Compared as one value so
	// a difference anywhere shows up, rather than as a list of fields somebody has to
	// remember to extend.
	struct resolution_fingerprint
	{
		int status = 0;
		int queued_job_count = 0;
		double start_seconds = 0.0;
		double end_seconds = 0.0;
		std::vector<std::string> output_paths;
		std::string failure_reason;
		std::string refusal_reason;
		std::string acknowledgement_field;
		std::vector<std::string> blocking_descriptions;

		bool operator==(const resolution_fingerprint& other) const
		{
			return status == other.status
				&& queued_job_count == other.queued_job_count
				&& start_seconds == other.start_seconds
				&& end_seconds == other.end_seconds
				&& output_paths == other.output_paths
				&& failure_reason == other.failure_reason
				&& refusal_reason == other.refusal_reason
				&& acknowledgement_field == other.acknowledgement_field
				&& blocking_descriptions == other.blocking_descriptions;
		}
	};

	resolution_fingerprint fingerprint_of(const render_outcome& outcome)
	{
		resolution_fingerprint fingerprint;
		fingerprint.status = static_cast<int>(outcome.status);
		fingerprint.queued_job_count = outcome.result.queued_job_count;
		fingerprint.start_seconds = outcome.result.start_seconds;
		fingerprint.end_seconds = outcome.result.end_seconds;
		fingerprint.output_paths = outcome.result.output_paths;
		fingerprint.failure_reason = outcome.failure_reason;
		fingerprint.refusal_reason = outcome.refusal.reason;
		fingerprint.acknowledgement_field = outcome.refusal.acknowledgement_field;
		fingerprint.blocking_descriptions = descriptions_of(outcome.refusal.blocking);

		return fingerprint;
	}

	// The selected track counts the grid crosses: none, one, a handful, and enough
	// that a per-track render is a real album-sized stem set.
	const std::vector<std::size_t>& determinism_track_counts()
	{
		static const std::vector<std::size_t> counts{0, 1, 2, 3, 7, 64};

		return counts;
	}

	const std::vector<render_source>& all_render_sources()
	{
		static const std::vector<render_source> sources{
			render_source::master_mix,
			render_source::stems,
			render_source::stems_via_master,
		};

		return sources;
	}

	const std::vector<render_bounds>& all_render_bounds()
	{
		static const std::vector<render_bounds> bounds{
			render_bounds::region,
			render_bounds::time_selection,
			render_bounds::project,
		};

		return bounds;
	}

	// 3 sources * 3 bounds * 6 track counts. Asserted below, so a grid that quietly
	// stopped crossing would not make the property vacuous.
	constexpr int expected_determinism_configurations = 54;
}

// ---------------------------------------------------------------------------
// Property 23
// ---------------------------------------------------------------------------

TEST_CASE("Property 23: settings composition round-trips, and every word REAPER could hold decomposes", "[daw][render][property]")
{
	// The example file next door sweeps all 4096 compositions. This sweep goes the
	// other way: every word over the named bits and the two documented-but-unmodelled
	// ones, which is where several source bits at once and unknown bits live.
	std::string first_violation;

	int words_checked = 0;
	int words_with_several_source_bits = 0;
	int words_with_unmodelled_bits = 0;
	int words_that_round_tripped_as_the_identity = 0;
	int words_that_were_canonicalised = 0;

	for (int word = 0; word < exhaustive_word_bound; ++word)
	{
		words_checked += 1;

		const int source_bits = word & render_settings_flag::source_axis_mask;
		const bool several_source_bits =
			source_bits != 0 && (source_bits & (source_bits - 1)) != 0;

		if (several_source_bits)
		{
			words_with_several_source_bits += 1;
		}

		if ((word & documented_but_unmodelled_bits_mask) != 0)
		{
			words_with_unmodelled_bits += 1;
		}

		const int recomposed =
			compose_render_settings(
				decompose_render_settings(render_settings_word_as_read_from_reaper(word)))
				.value();

		if (recomposed == word)
		{
			words_that_round_tripped_as_the_identity += 1;
		}
		else
		{
			words_that_were_canonicalised += 1;
		}

		const std::string violation = settings_word_violation(word);

		if (!violation.empty() && first_violation.empty())
		{
			first_violation = violation;
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Guards on the sweep, measured rather than guessed — see the derivation beside
	// the constants. A truncated sweep would satisfy everything above while checking
	// almost none of it.
	REQUIRE(words_checked == exhaustive_word_bound);
	REQUIRE(words_with_several_source_bits == expected_words_with_several_source_bits);
	REQUIRE(words_with_unmodelled_bits == expected_words_with_unmodelled_bits);
	REQUIRE(words_that_round_tripped_as_the_identity == expected_identity_words);
	REQUIRE(words_that_were_canonicalised == expected_canonicalised_words);

	// The asymmetry, stated as an assertion rather than only in the header comment:
	// most words REAPER could hold do **not** come back unchanged, and that is the
	// design rather than a defect.
	REQUIRE(words_that_were_canonicalised > words_that_round_tripped_as_the_identity);
}

TEST_CASE("Property 23: decomposition is total over full-width integers, negatives included", "[daw][render][property]")
{
	// `GetSetProjectInfo` hands back a double that is cast to an int, so the value can
	// be anything an int can be — including negative, and including bits far above
	// the highest one this model names.
	DeterministicBytes bytes{settings_word_seed};

	std::string first_violation;

	int words_checked = 0;
	int negative_words = 0;
	int words_with_bits_above_the_named_span = 0;
	int words_with_several_source_bits = 0;
	int words_that_round_tripped_as_the_identity = 0;

	const auto check = [&](int word) {
		words_checked += 1;

		if (word < 0)
		{
			negative_words += 1;
		}

		if ((word & ~(exhaustive_word_bound - 1)) != 0)
		{
			words_with_bits_above_the_named_span += 1;
		}

		const int source_bits = word & render_settings_flag::source_axis_mask;

		if (source_bits != 0 && (source_bits & (source_bits - 1)) != 0)
		{
			words_with_several_source_bits += 1;
		}

		if (reference_canonical_word(word) == word)
		{
			words_that_round_tripped_as_the_identity += 1;
		}

		const std::string violation = settings_word_violation(word);

		if (!violation.empty() && first_violation.empty())
		{
			first_violation = describe_seed(settings_word_seed) + ": " + violation;
		}
	};

	// The extremes by hand, because a generator is unlikely to reach them and they are
	// where a sign-bit mistake would show: every bit set, the sign bit alone, and the
	// two bounds of the type.
	check(-1);
	check(0);
	check(1);
	check(-2147483647 - 1);
	check(2147483647);
	check(documented_but_unmodelled_bits_mask);
	check(render_settings_flag::source_axis_mask);

	for (int iteration = 0; iteration < settings_word_iterations; ++iteration)
	{
		check(generate_full_width_word(bytes));
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Measured guards, not chosen ones: every number below is what this seed actually
	// reaches, so they are exact rather than a threshold somebody hoped was low
	// enough. The xorshift, the modulo, and the bit arithmetic are all well-defined on
	// `std::uint32_t`, so these hold on every platform the suite builds on.
	REQUIRE(words_checked == settings_word_iterations + 7);
	REQUIRE(negative_words == 1956);
	REQUIRE(words_with_bits_above_the_named_span == 4003);
	REQUIRE(words_with_several_source_bits == 1979);

	// And the point of the whole case: for a word REAPER produced, coming back
	// unchanged is the exception. Exactly two of these 4007 words are their own
	// canonical form — `0` and `1`, both hand-checked above — so if this corpus were
	// quietly testing the composition direction again, this guard is where that would
	// show.
	REQUIRE(words_that_round_tripped_as_the_identity == 2);
}

// ---------------------------------------------------------------------------
// Property 24
// ---------------------------------------------------------------------------

TEST_CASE("Property 24: a collision in any resolved path set is caught before anything is queued", "[daw][render][property]")
{
	DeterministicBytes bytes{path_set_seed};

	std::string first_violation;

	int sets_checked = 0;
	int sets_with_a_self_collision = 0;
	int sets_with_a_case_only_duplicate = 0;
	int sets_with_a_separator_only_duplicate = 0;
	int sets_with_no_duplicates = 0;
	int existing_file_refusals = 0;
	int acknowledged_overwrites_that_queued = 0;
	int self_collisions_refused_despite_acknowledgement = 0;
	std::size_t longest_set_seen = 0;

	const auto note = [&](const std::vector<std::string>& paths, const std::string& violation) {
		if (!violation.empty() && first_violation.empty())
		{
			first_violation = describe_seed(path_set_seed) + ", " + describe_paths(paths) + ": "
				+ violation;
		}
	};

	for (int iteration = 0; iteration < path_set_iterations; ++iteration)
	{
		// A third of the corpus is built from one base name per slot, which is the
		// negative control: those sets cannot collide with themselves and must queue.
		const bool distinct = iteration % 3 == 0;
		const std::vector<std::string> paths = generate_path_set(bytes, distinct);

		sets_checked += 1;
		longest_set_seen = std::max(longest_set_seen, paths.size());

		const std::vector<std::string> expected_collisions = reference_self_collisions(paths);

		// Which spellings the duplicates differ in, so the corpus guards can say the
		// case-folding and separator-folding halves were actually exercised rather than
		// assumed.
		bool a_case_only_duplicate = false;
		bool a_separator_only_duplicate = false;

		for (std::size_t left = 0; left < paths.size(); ++left)
		{
			for (std::size_t right = left + 1; right < paths.size(); ++right)
			{
				if (paths[left] == paths[right])
				{
					continue;
				}

				if (reference_normalized_path(paths[left]) != reference_normalized_path(paths[right]))
				{
					continue;
				}

				const bool separators_differ =
					(paths[left].find('\\') != std::string::npos)
					!= (paths[right].find('\\') != std::string::npos);

				if (separators_differ)
				{
					a_separator_only_duplicate = true;
				}
				else
				{
					a_case_only_duplicate = true;
				}
			}
		}

		if (a_case_only_duplicate)
		{
			sets_with_a_case_only_duplicate += 1;
		}

		if (a_separator_only_duplicate)
		{
			sets_with_a_separator_only_duplicate += 1;
		}

		// The pure function first, against an oracle that groups rather than streams.
		if (find_self_colliding_output_paths(paths) != expected_collisions)
		{
			note(paths, "find_self_colliding_output_paths disagreed with the grouping oracle");
		}

		if (!expected_collisions.empty())
		{
			sets_with_a_self_collision += 1;

			// --- the unacknowledgeable collision ---

			recording_render_host host;
			configure_stem_host(host, paths);

			render_request request;
			request.source = render_source::stems;

			render_coordinator coordinator{host};
			const render_outcome outcome = coordinator.queue_render(request);

			if (outcome.status != render_outcome_status::refused)
			{
				note(paths, "a self-colliding set was not refused");
			}
			else if (outcome.refusal.reason != std::string{output_file_collision_refusal_reason})
			{
				note(paths, "a self-collision was refused as " + outcome.refusal.reason);
			}
			else if (!outcome.refusal.acknowledgement_field.empty())
			{
				note(paths, "a self-collision offered the acknowledgement "
					+ outcome.refusal.acknowledgement_field);
			}
			else if (descriptions_of(outcome.refusal.blocking) != expected_collisions)
			{
				note(paths, "the refusal enumerated "
					+ describe_paths(descriptions_of(outcome.refusal.blocking)) + " rather than "
					+ describe_paths(expected_collisions));
			}

			for (const render_blocking_entity& entity : outcome.refusal.blocking)
			{
				if (entity.kind != std::string{file_path_blocking_kind})
				{
					note(paths, "a colliding path was reported as kind " + entity.kind);
					break;
				}
			}

			// Requirement 12.4 and 9.8: before anything is written. Asserted against
			// the recorded sequence, not inferred from the outcome.
			if (host.queue_additions != 0
				|| count_calls(host.call_log, "add_project_to_render_queue") != 0)
			{
				note(paths, "a self-colliding render reached the queue "
					+ std::to_string(host.queue_additions) + " times");
			}

			// And an acknowledgement does not move it. Two of this render's own jobs
			// writing one path is not something approval fixes.
			recording_render_host acknowledged_host;
			configure_stem_host(acknowledged_host, paths);
			acknowledged_host.existing_files = paths;

			render_request acknowledged_request = request;
			acknowledged_request.confirmed_overwrite = true;

			render_coordinator acknowledged_coordinator{acknowledged_host};
			const render_outcome acknowledged_outcome =
				acknowledged_coordinator.queue_render(acknowledged_request);

			if (acknowledged_outcome.status != render_outcome_status::refused
				|| !acknowledged_outcome.refusal.acknowledgement_field.empty()
				|| acknowledged_host.queue_additions != 0)
			{
				note(paths, "confirmedOverwrite made a self-collision queueable");
			}
			else
			{
				self_collisions_refused_despite_acknowledgement += 1;
			}

			continue;
		}

		sets_with_no_duplicates += 1;

		// --- the negative control: a distinct set queues ---

		recording_render_host clean_host;
		configure_stem_host(clean_host, paths);

		render_request clean_request;
		clean_request.source = render_source::stems;

		render_coordinator clean_coordinator{clean_host};
		const render_outcome clean_outcome = clean_coordinator.queue_render(clean_request);

		if (clean_outcome.status != render_outcome_status::queued)
		{
			note(paths, "a distinct set was not queued");
		}
		else if (clean_outcome.result.output_paths != paths)
		{
			note(paths, "the queued render enumerated "
				+ describe_paths(clean_outcome.result.output_paths));
		}
		else if (clean_host.queue_additions != 1)
		{
			note(paths, "a distinct set queued " + std::to_string(clean_host.queue_additions)
				+ " jobs rather than one");
		}

		// --- the acknowledgeable collision: some of these files already exist ---

		std::vector<std::string> existing_files;

		for (const std::string& path : paths)
		{
			if (bytes.below(2) == 0)
			{
				existing_files.push_back(path);
			}
		}

		if (existing_files.empty())
		{
			existing_files.push_back(paths[bytes.below(paths.size())]);
		}

		std::vector<std::string> expected_existing;

		for (const std::string& path : paths)
		{
			if (std::find(existing_files.begin(), existing_files.end(), path) != existing_files.end())
			{
				expected_existing.push_back(path);
			}
		}

		recording_render_host existing_host;
		configure_stem_host(existing_host, paths);
		existing_host.existing_files = existing_files;

		render_coordinator existing_coordinator{existing_host};
		const render_outcome existing_outcome = existing_coordinator.queue_render(clean_request);

		if (existing_outcome.status != render_outcome_status::refused)
		{
			note(paths, "an existing output file was not refused");
		}
		else if (existing_outcome.refusal.reason != std::string{output_file_collision_refusal_reason})
		{
			note(paths, "an existing file was refused as " + existing_outcome.refusal.reason);
		}
		else if (existing_outcome.refusal.acknowledgement_field
			!= std::string{overwrite_acknowledgement_field})
		{
			note(paths, "an existing file named the acknowledgement '"
				+ existing_outcome.refusal.acknowledgement_field + "'");
		}
		else if (descriptions_of(existing_outcome.refusal.blocking) != expected_existing)
		{
			note(paths, "the overwrite refusal enumerated "
				+ describe_paths(descriptions_of(existing_outcome.refusal.blocking))
				+ " rather than " + describe_paths(expected_existing));
		}
		else
		{
			existing_file_refusals += 1;
		}

		if (existing_host.queue_additions != 0
			|| count_calls(existing_host.call_log, "add_project_to_render_queue") != 0)
		{
			note(paths, "an existing-file refusal still reached the queue");
		}

		// --- and the acknowledged retry proceeds ---

		recording_render_host retry_host;
		configure_stem_host(retry_host, paths);
		retry_host.existing_files = existing_files;

		render_request retry_request = clean_request;
		retry_request.confirmed_overwrite = true;

		render_coordinator retry_coordinator{retry_host};
		const render_outcome retry_outcome = retry_coordinator.queue_render(retry_request);

		if (retry_outcome.status != render_outcome_status::queued || retry_host.queue_additions != 1)
		{
			note(paths, "the acknowledged overwrite did not queue");
		}
		else
		{
			acknowledged_overwrites_that_queued += 1;
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Measured corpus guards, exact rather than thresholds. Each one is a claim the
	// property above would be vacuous without: a corpus of nothing but distinct
	// single-path sets satisfies all of it while checking none of it.
	//
	// The three identities among these are worth reading as shape rather than as
	// numbers. 1091 + 909 = 2000, so every set took exactly one of the two arms. Every
	// one of the 909 distinct sets produced an existing-file refusal *and* an
	// acknowledged retry that queued. And all 1091 self-colliding sets were refused a
	// second time with `confirmedOverwrite` set, which is the precedence between the
	// two collision kinds stated over the whole corpus rather than once.
	REQUIRE(sets_checked == path_set_iterations);
	REQUIRE(longest_set_seen == maximum_generated_path_count);
	REQUIRE(sets_with_a_self_collision == 1091);
	REQUIRE(sets_with_no_duplicates == 909);
	REQUIRE(sets_with_a_case_only_duplicate == 790);
	REQUIRE(sets_with_a_separator_only_duplicate == 925);
	REQUIRE(existing_file_refusals == 909);
	REQUIRE(acknowledged_overwrites_that_queued == 909);
	REQUIRE(self_collisions_refused_despite_acknowledgement == 1091);
}

// ---------------------------------------------------------------------------
// Property 25
// ---------------------------------------------------------------------------

TEST_CASE("Property 25: path resolution is deterministic across bounds kinds and selection sizes", "[daw][render][property]")
{
	// What this claim contains and what it cannot is set out in the header comment.
	// In short: the fake host answers from fixed data, so REAPER's own wildcard
	// resolution is not under test — what is under test is that this component asks
	// the same questions in the same order for the same inputs, and that the inputs
	// which must not affect resolution do not.
	std::string first_violation;

	int configurations_checked = 0;
	int configurations_that_queued = 0;
	int configurations_that_failed = 0;
	std::size_t largest_selection_seen = 0;

	const auto note = [&](const render_configuration& configuration, const std::string& violation) {
		if (!violation.empty() && first_violation.empty())
		{
			first_violation = describe_configuration(configuration) + ": " + violation;
		}
	};

	for (const render_source source : all_render_sources())
	{
		for (const render_bounds bounds : all_render_bounds())
		{
			for (const std::size_t track_count : determinism_track_counts())
			{
				const render_configuration configuration{source, bounds, track_count};

				configurations_checked += 1;
				largest_selection_seen = std::max(largest_selection_seen, track_count);

				recording_render_host first_host;
				configure_host_for(first_host, configuration);
				render_coordinator first_coordinator{first_host};
				const render_outcome first_outcome =
					first_coordinator.queue_render(request_for(configuration));

				recording_render_host second_host;
				configure_host_for(second_host, configuration);
				render_coordinator second_coordinator{second_host};
				const render_outcome second_outcome =
					second_coordinator.queue_render(request_for(configuration));

				if (!(fingerprint_of(first_outcome) == fingerprint_of(second_outcome)))
				{
					note(configuration, "two identical calls resolved differently");
				}

				// The stronger half: not just the same answer, the same conversation
				// with REAPER. A log that differed would mean an unordered container,
				// something ambient, or something uninitialised.
				if (first_host.call_log != second_host.call_log)
				{
					note(configuration, "two identical calls asked REAPER different questions");
				}

				if (first_host.soloed_track_guids != second_host.soloed_track_guids)
				{
					note(configuration, "two identical calls soloed tracks in different orders");
				}

				// --- varying only what must not matter ---

				recording_render_host varied_host;
				configure_host_for(varied_host, configuration);

				render_request varied_request = request_for(configuration);
				varied_request.sample_rate = 96000;
				varied_request.bit_depth = 24;

				render_coordinator varied_coordinator{varied_host};
				const render_outcome varied_outcome = varied_coordinator.queue_render(varied_request);

				if (varied_outcome.result.output_paths != first_outcome.result.output_paths)
				{
					note(configuration, "changing the sample rate changed the resolved paths");
				}

				if (varied_outcome.status != first_outcome.status)
				{
					note(configuration, "changing the sample rate changed the outcome");
				}

				if (varied_host.call_log != first_host.call_log)
				{
					note(configuration, "changing the sample rate changed the call sequence");
				}

				// It did reach the host, so the variation was not silently ignored —
				// otherwise "varying it changed nothing" is true for the wrong reason.
				if (varied_outcome.status == render_outcome_status::queued)
				{
					if (varied_host.written_sample_rates != std::vector<int>{96000})
					{
						note(configuration, "the varied sample rate never reached REAPER");
					}

					if (first_host.written_sample_rates != std::vector<int>{48000})
					{
						note(configuration, "the project sample rate was not written");
					}
				}

				// --- what the outcome says about the selection ---

				if (first_outcome.status == render_outcome_status::queued)
				{
					configurations_that_queued += 1;

					if (source == render_source::stems_via_master)
					{
						// The resolution pass and the queue pass each walk the
						// selection in project order, so the paths collision
						// detection ran against are the paths the queued jobs write.
						std::vector<std::string> expected_solo_order =
							guids_of(first_host.selected_tracks);
						const std::vector<std::string> project_order = expected_solo_order;
						expected_solo_order.insert(
							expected_solo_order.end(),
							project_order.begin(),
							project_order.end());

						if (first_host.soloed_track_guids != expected_solo_order)
						{
							note(configuration, "the solo order was not project order twice over");
						}

						if (first_host.soloed_track_guids != first_host.unsoloed_track_guids)
						{
							note(configuration, "solo state was not restored");
						}

						if (first_outcome.result.output_paths.size() != track_count)
						{
							note(configuration, "resolved "
								+ std::to_string(first_outcome.result.output_paths.size())
								+ " paths for " + std::to_string(track_count) + " tracks");
						}
					}
				}
				else if (first_outcome.status == render_outcome_status::failed)
				{
					configurations_that_failed += 1;

					// A failure is deterministic too, reason and all, which is what
					// lets the agent say the same thing twice about the same call.
					if (first_outcome.failure_reason != second_outcome.failure_reason
						|| first_outcome.failure_reason.empty())
					{
						note(configuration, "the failure reason was not stable");
					}
				}
			}
		}
	}

	INFO(first_violation);
	REQUIRE(first_violation.empty());

	// Measured grid guards.
	REQUIRE(configurations_checked == expected_determinism_configurations);
	REQUIRE(largest_selection_seen == 64);

	// Both outcomes appear, so the determinism claim covers a render that resolved
	// paths and one that could not. A grid where everything failed would be a
	// determinism property over the failure path only.
	REQUIRE(configurations_that_queued == 48);
	REQUIRE(configurations_that_failed == 6);
}

TEST_CASE("Property 25: a refusal is as deterministic as a queued render", "[daw][render][property]")
{
	// The grid above reaches queued and failed but not refused, and a refusal is the
	// outcome whose stability the agent depends on most: the retry it offers the
	// producer names specific files, so the second call has to enumerate the same ones
	// in the same order.
	const std::vector<std::string> colliding_paths{
		"/Users/producer/Music/Session/renders/Session - Kick.wav",
		"/Users/producer/Music/Session/renders/SESSION - KICK.WAV",
		"C:\\Users\\producer\\renders\\Session - Snare.wav",
		"c:/users/producer/renders/session - snare.wav",
	};

	const auto refuse_once = [&]() {
		recording_render_host host;
		configure_stem_host(host, colliding_paths);

		render_request request;
		request.source = render_source::stems;

		render_coordinator coordinator{host};

		return fingerprint_of(coordinator.queue_render(request));
	};

	const resolution_fingerprint first = refuse_once();
	const resolution_fingerprint second = refuse_once();

	REQUIRE(first.status == static_cast<int>(render_outcome_status::refused));
	REQUIRE(first.refusal_reason == std::string{output_file_collision_refusal_reason});
	REQUIRE(first.blocking_descriptions.size() == 2);
	CHECK(first == second);
}
