// Object resolution: turning a `trackSelector` into a track, and marker and region
// GUIDs into the objects they name (requirement 7, design "Object Resolver").
//
// The refusal behaviour is the component. Every other part of this file exists to
// make two refusals reachable and a third outcome impossible:
//
//   - `unresolved_track_selector` when nothing matched. Never a guess.
//   - `ambiguous_track_selector` when the winning source matched more than one
//     track, with every candidate named and its GUID attached, so the agent
//     disambiguates by picking one rather than inventing a narrower pattern.
//
// A resolver that guesses is worse than one that refuses, and not by a little. The
// agent operates on a producer's session; a wrong track is a change to work they
// have already done, discovered later, in a place they were not looking. Refusing
// costs one round trip.
//
// ---------------------------------------------------------------------------
// The outcome set is closed
//
// Requirement 7.5 says exactly one of three outcomes for any pattern and any track
// set. That is modelled as a `std::variant` rather than as a struct with a status
// field and some optionals, because the two are not equally strong. A struct can
// hold a status saying "resolved" next to an empty track and a populated candidate
// list — a fourth outcome nobody designed, reachable by forgetting one assignment.
// The variant makes that unrepresentable: constructing a `TrackResolution` means
// naming exactly one alternative, and reading one means handling all three.
//
// ---------------------------------------------------------------------------
// Source precedence, stated exactly
//
// Requirement 7.2 is the rule most easily got subtly wrong, so it is worth writing
// out in full. A name pattern consults three sources in a fixed order — track name,
// then learned alias, then structural role — and **the first source with a non-empty
// candidate set wins outright**.
//
// Two consequences, both load-bearing:
//
//   - A later source never overrides an earlier match. Once track name has produced
//     a candidate, aliases and roles are not consulted at all.
//   - **An earlier source producing two candidates is ambiguous.** It does not fall
//     through to the next source hoping for a single hit. Two tracks named "Drums"
//     is a question for the producer, and the alias store happening to point at one
//     of them is not an answer to it — the producer's pattern was about the name.
//
// The precedence is data (`name_pattern_sources_in_precedence_order`) rather than
// three chained ifs, so the order is one line to read and one line to test.
//
// ---------------------------------------------------------------------------
// Match tiers inside a source
//
// Requirement 7.6 speaks of a "confident-but-imperfect match", so matching is not
// only equality. Within a single source, candidates are collected at the strongest
// tier that yields anything:
//
//   1. Exact — equal after normalization (case folded, separators collapsed).
//   2. Containment — the normalized pattern appears inside the normalized candidate
//      string. "vox" finds "Lead Vox". This is the imperfect tier.
//
// The tier ladder follows the same rule as the source ladder, for the same reason:
// two exact matches are ambiguous and do not fall through to containment, which
// would only ever match a superset of them.
//
// Containment is deliberately one-directional. The reverse — a candidate name
// appearing inside a longer pattern, so that "the drum bus please" finds "Drum Bus"
// — was considered and left out. It makes short track names match almost any
// pattern, and stripping natural language down to the reference is the agent's work,
// not the resolver's. The cost of leaving it out is a refusal the agent can recover
// from by asking a better question.
//
// The structural role source has no containment tier at all. Role phrases are short
// and overlap ("bus" sits inside "master bus"), so containment there resolves
// confidently to the wrong object — the failure this component exists to prevent.
//
// ---------------------------------------------------------------------------
// Dependencies, and what is not one
//
// Nothing here includes the REAPER SDK. The resolution logic works over a plain
// snapshot of the track list, which is what makes it testable without REAPER and is
// why the test target has no SDK include path. `MediaTrack` is forward-declared and
// only ever held as a pointer, following `entry/reaper_timer_registrar.h`.
//
// The Alias Store is reached through `LearnedAliasLookup`, declared here — one
// method, taking the track resolution is already looking at. Narrow on purpose: the
// store's own business is `P_EXT:` keys and project extension state, and resolution
// needs none of that. `daw/stored_alias_lookup.h` is the adapter that connects the
// two, and it lives on the store's side of the seam.
//
// Structural roles are not derived here. `context/structural_role.h` owns the
// derivation and the Project Context Builder already performs it per track
// (requirement 6.6), so a role arrives on the snapshot as data. A track whose role is
// absent is invisible to the third source, which is the refusing direction.

#ifndef SESH_AI_DAW_OBJECT_RESOLVER_H
#define SESH_AI_DAW_OBJECT_RESOLVER_H

#include <array>
#include <cctype>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <context/structural_role.h>

// REAPER's opaque track handle. Declared by the SDK as `class MediaTrack;` — naming
// it here declares the same type rather than a second one, and the class-key matches
// so MSVC does not warn about it.
class MediaTrack;

// REAPER's opaque project handle, same arrangement.
class ReaProject;

namespace sesh_ai::daw
{
	// ---------------------------------------------------------------------------
	// Contract values
	//
	// Spelled once here rather than at each call site, because every one of them is a
	// string the server or the model reads and a typo is a protocol bug rather than a
	// compile error.
	// ---------------------------------------------------------------------------

	// `reason` in messages/tool-result-refusal.schema.json.
	inline constexpr std::string_view unresolved_track_selector_refusal_reason{"unresolved_track_selector"};
	inline constexpr std::string_view ambiguous_track_selector_refusal_reason{"ambiguous_track_selector"};

	// `acknowledgementField` for an ambiguous selector. The tool schemas document
	// `confirmedTrackSelection` as something set only after this refusal, once the
	// producer has chosen which track they meant.
	//
	// There is no counterpart for an unresolved selector, and that absence is
	// correct: a pattern matching no track is not something acknowledging can get
	// past. The field is optional in the schema, so the refusal simply omits it.
	inline constexpr std::string_view ambiguous_track_selector_acknowledgement_field{"confirmedTrackSelection"};

	// `kind` on a blocking entity. Only `track` is produced here; the other three
	// belong to the undo, render, and routing refusals.
	inline constexpr std::string_view track_blocking_entity_kind{"track"};

	// `maxItems` on `blocking`. A pattern matching more candidates than this is
	// pathological, but silently dropping the overflow would quietly break
	// requirement 7.4, so the last slot carries a count of what did not fit instead.
	inline constexpr std::size_t maximum_blocking_entities = 512;

	// ---------------------------------------------------------------------------
	// Schema shapes
	// ---------------------------------------------------------------------------

	// `trackReference` from mcp-tools/outputs/tool-output-defs.schema.json: how a
	// resolved track is reported. The GUID is what the agent addresses the track by
	// next; the name is what it says to the producer, who cannot see a GUID.
	struct TrackReference
	{
		std::string guid;
		std::string name;
	};

	inline bool operator==(const TrackReference& left, const TrackReference& right)
	{
		return left.guid == right.guid && left.name == right.name;
	}

	inline bool operator!=(const TrackReference& left, const TrackReference& right)
	{
		return !(left == right);
	}

	// `blockingEntity` from the refusal schema. `guid` empty means the property is
	// absent rather than present and empty — the schema's pattern would reject an
	// empty string, so the serialiser must omit it.
	struct BlockingEntity
	{
		std::string kind;
		std::string description;
		std::string guid;
	};

	// The whole refusal payload. `refused` is a schema constant rather than a flag,
	// so it is not settable.
	struct ToolResultRefusal
	{
		std::string reason;
		std::vector<BlockingEntity> blocking;

		// Empty means the optional `acknowledgementField` is absent.
		std::string acknowledgement_field;

		static constexpr bool refused = true;
	};

	// ---------------------------------------------------------------------------
	// Selectors
	// ---------------------------------------------------------------------------

	// Address a track by its stable GUID.
	struct TrackGuidSelector
	{
		std::string guid;
	};

	// Address a track by the producer's own words for it.
	struct TrackNamePatternSelector
	{
		std::string name_pattern;
	};

	// The `trackSelector` oneOf from mcp-tool-defs.schema.json, as a type: exactly
	// one of the two, never both, never neither. The schema's `oneOf` with
	// `additionalProperties: false` on each branch says precisely this, and a struct
	// with two optional strings would not — it can hold both, or hold neither, and
	// then every reader has to decide what that means.
	using TrackSelector = std::variant<TrackGuidSelector, TrackNamePatternSelector>;

	// ---------------------------------------------------------------------------
	// The snapshot resolution works over
	// ---------------------------------------------------------------------------

	// One track, reduced to what resolution can look at.
	//
	// Plain data, deliberately. The REAPER walk collects this and the resolution
	// logic never touches a handle, which is what lets the suite resolve against a
	// synthetic list.
	struct ResolvableTrack
	{
		// REAPER's braced GUID form, as `guidToString` produces it.
		std::string guid;

		// The track name as REAPER shows it at the time of the call. Empty is
		// allowed and common — an unnamed track in REAPER has no name, not a
		// placeholder — and an empty name matches no pattern, which is right.
		std::string name;

		// REAPER's handle. Null on a synthetic list, and null is not an error here:
		// the resolution logic never dereferences it and only the Tool Executor's
		// REAPER-facing half cares whether it is there.
		MediaTrack* track = nullptr;

		// Zero-based index in the project's track list. Negative for the master
		// track, which is not in that list.
		int project_index = -1;

		// True for the master track. Carried rather than inferred from the index,
		// because "not in the list" and "is the master" are the same thing today and
		// need not stay that way.
		bool is_master_track = false;

		// The derived role, when the caller has one. Absent means the structural role
		// source cannot match this track — refusing rather than guessing at a role
		// nobody derived.
		std::optional<context::StructuralRole> structural_role;
	};

	// One marker or region, reduced the same way.
	struct ResolvableMarkerOrRegion
	{
		// REAPER's braced GUID form, read through `GetSetRegionOrMarkerInfo_String`
		// with the read-only `GUID` parameter.
		std::string guid;

		std::string name;

		bool is_region = false;

		double start_seconds = 0.0;

		// Equal to `start_seconds` for a marker, which is what REAPER's `D_ENDPOS`
		// reports for one.
		double end_seconds = 0.0;

		// REAPER's internal index, which is the handle for every later call about
		// this object.
		int internal_index = -1;

		// The number REAPER displays on the ruler. Not unique for markers, which is
		// why it is not what anything resolves by.
		int displayed_number = -1;
	};

	// ---------------------------------------------------------------------------
	// Sources
	// ---------------------------------------------------------------------------

	// Where a candidate came from. The last three are the name-pattern sources of
	// requirement 7.2, in precedence order.
	enum class ResolutionSource
	{
		// A GUID walk. Not part of the precedence ladder — a GUID either names a
		// track in the list or it does not.
		guid,

		track_name,
		learned_alias,
		structural_role
	};

	// Requirement 7.2's fixed order, as data. Resolution iterates this, so the order
	// is stated once and a test can assert against the same array the implementation
	// walks rather than against a copy of the intent.
	inline constexpr std::array<ResolutionSource, 3> name_pattern_sources_in_precedence_order{
		ResolutionSource::track_name,
		ResolutionSource::learned_alias,
		ResolutionSource::structural_role
	};

	// How well the pattern fitted. `imperfect` is requirement 7.6's
	// confident-but-imperfect match: good enough to proceed on for a safe operation,
	// and worth naming in what gets reported back.
	enum class MatchPrecision
	{
		exact,
		imperfect
	};

	inline std::string_view describe_resolution_source(ResolutionSource source)
	{
		switch (source)
		{
			case ResolutionSource::guid:
				return "guid";
			case ResolutionSource::track_name:
				return "track name";
			case ResolutionSource::learned_alias:
				return "learned alias";
			case ResolutionSource::structural_role:
				return "structural role";
		}

		return "unknown source";
	}

	inline std::string_view describe_match_precision(MatchPrecision precision)
	{
		return precision == MatchPrecision::exact ? "exact" : "imperfect";
	}

	// ---------------------------------------------------------------------------
	// The Alias Store seam
	// ---------------------------------------------------------------------------

	// The narrow slice of the Alias Store that object resolution needs: given a
	// track, what has the producer called it.
	//
	// Keyed on the `ResolvableTrack` — the thing resolution is already holding when
	// it asks. Not on the GUID, and the reason is concrete: the Alias Store is keyed
	// by `MediaTrack*`, because requirement 8.1 keeps the aliases on the track
	// itself, and `ResolvableTrack` carries both that handle and the `guid`. A
	// GUID-keyed seam would have the resolver discard the handle it had and the
	// adapter walk the track list to find it again, per track, to recover something
	// that was never lost.
	//
	// Passing the whole track rather than one field of it also lets a substitute key
	// on the `guid`, which is what a synthetic track list has, while the production
	// adapter keys on the handle, which is what REAPER has.
	class LearnedAliasLookup
	{
	public:
		virtual ~LearnedAliasLookup() = default;

		// Every alias learned for this track, in the order the store holds them —
		// insertion order, per requirement 8.4, which is what makes the candidate order
		// below reproducible rather than incidental. Empty when there are none, which
		// is the common case.
		virtual std::vector<std::string> learned_aliases_for_track(const ResolvableTrack& track) const = 0;
	};

	// For callers with no alias store yet, and for resolving in a context where
	// learned vocabulary should not participate. Present so that "no aliases" is a
	// thing you can pass rather than a null pointer every caller has to check.
	class NoLearnedAliases final : public LearnedAliasLookup
	{
	public:
		std::vector<std::string> learned_aliases_for_track(const ResolvableTrack&) const override
		{
			return {};
		}
	};

	// ---------------------------------------------------------------------------
	// The REAPER seams
	//
	// One interface per thing resolution reads, following entry/timer_registration.h:
	// the interface is SDK-free, the production implementation is the only translation
	// unit that includes the SDK, and the suite substitutes plain data.
	// ---------------------------------------------------------------------------

	// The project's tracks, in project order.
	class TrackListSource
	{
	public:
		virtual ~TrackListSource() = default;

		virtual std::vector<ResolvableTrack> tracks_in_project_order() const = 0;
	};

	// The project's markers and regions, in REAPER's enumeration order.
	class MarkerAndRegionSource
	{
	public:
		virtual ~MarkerAndRegionSource() = default;

		virtual std::vector<ResolvableMarkerOrRegion> markers_and_regions_in_enumeration_order() const = 0;
	};

	// ---------------------------------------------------------------------------
	// The three outcomes
	// ---------------------------------------------------------------------------

	// Outcome one: a single track, and how it was found.
	struct ResolvedTrack
	{
		TrackReference reference;
		MediaTrack* track = nullptr;
		int project_index = -1;
		bool is_master_track = false;
		ResolutionSource source = ResolutionSource::guid;
		MatchPrecision precision = MatchPrecision::exact;
	};

	// Outcome one, for a marker or region.
	struct ResolvedMarkerOrRegion
	{
		ResolvableMarkerOrRegion object;
		ResolutionSource source = ResolutionSource::guid;
	};

	// Outcome two: nothing matched. Carries what was asked for, so a log line or a
	// message to the producer can name it — the refusal payload itself has no room
	// for the selector, and the agent already knows what it sent.
	struct UnresolvedSelector
	{
		std::string selector_description;
	};

	// Outcome three: the winning source matched more than one candidate.
	//
	// Templated only so that markers and regions get the same shape without the
	// candidate type being wrong for one of them. `candidates` always holds at least
	// two entries — one would have resolved — in the order the snapshot listed them,
	// so the enumeration the producer sees follows the order they see in REAPER.
	template <typename CandidateType>
	struct AmbiguousSelectorOf
	{
		std::string selector_description;
		ResolutionSource source = ResolutionSource::guid;
		MatchPrecision precision = MatchPrecision::exact;
		std::vector<CandidateType> candidates;
	};

	using AmbiguousTrackSelector = AmbiguousSelectorOf<ResolvedTrack>;
	using AmbiguousMarkerOrRegionSelector = AmbiguousSelectorOf<ResolvedMarkerOrRegion>;

	// Requirement 7.5 as a type. Three alternatives, so a fourth outcome is not
	// something this codebase can express.
	using TrackResolution = std::variant<ResolvedTrack, UnresolvedSelector, AmbiguousTrackSelector>;

	// The same closed shape for markers and regions (requirement 7.8).
	using MarkerOrRegionResolution =
		std::variant<ResolvedMarkerOrRegion, UnresolvedSelector, AmbiguousMarkerOrRegionSelector>;

	inline bool is_resolved(const TrackResolution& resolution)
	{
		return std::holds_alternative<ResolvedTrack>(resolution);
	}

	inline bool is_unresolved(const TrackResolution& resolution)
	{
		return std::holds_alternative<UnresolvedSelector>(resolution);
	}

	inline bool is_ambiguous(const TrackResolution& resolution)
	{
		return std::holds_alternative<AmbiguousTrackSelector>(resolution);
	}

	inline bool is_resolved(const MarkerOrRegionResolution& resolution)
	{
		return std::holds_alternative<ResolvedMarkerOrRegion>(resolution);
	}

	inline bool is_unresolved(const MarkerOrRegionResolution& resolution)
	{
		return std::holds_alternative<UnresolvedSelector>(resolution);
	}

	inline bool is_ambiguous(const MarkerOrRegionResolution& resolution)
	{
		return std::holds_alternative<AmbiguousMarkerOrRegionSelector>(resolution);
	}

	// ---------------------------------------------------------------------------
	// Normalization and matching
	// ---------------------------------------------------------------------------

	// A leading definite article, which normalization removes. See below for why.
	inline constexpr std::string_view leading_definite_article{"the "};

	// Reduces a name, alias, or pattern to the form comparisons happen in: ASCII case
	// folded, runs of whitespace, underscores, and hyphens collapsed to one space with
	// the ends trimmed, and a leading definite article dropped.
	//
	// Folding underscores and hyphens into spaces is what makes "drum_bus",
	// "drum-bus", and "Drum Bus" the same reference, and it also turns the schema's
	// role spellings — `summing_folder_parent` — into the producer's phrasing without
	// a second table.
	//
	// Dropping a leading "the" is the one piece of language handling here, and it
	// earns its place by being symmetric: it applies to names, aliases, and patterns
	// alike, so "the bus" still matches a track literally named "The Bus" exactly.
	// Without it the role source would need an articled variant of every phrase —
	// "the master", "the aux return", and so on — which multiplies the table for no
	// new information, since an article names nothing. It also upgrades some matches
	// from the imperfect tier to the exact one: "the drum bus" against a track called
	// "Drum Bus" is an exact reference that only looked approximate.
	//
	// No further stopword stripping. "the" is unambiguous filler in this position;
	// anything beyond it starts guessing at what the producer meant, which is the one
	// thing this component does not do.
	//
	// ASCII-only folding is a real limitation worth stating rather than hiding. A
	// track named "Bläck" is compared byte-wise on the non-ASCII byte, so a pattern
	// spelling it with a different case there will not match exactly, though
	// containment still works on the ASCII part. Full Unicode case folding needs a
	// table this extension does not carry, and the failure it produces is a refusal
	// rather than a wrong track — the direction worth failing in.
	inline std::string normalize_for_matching(std::string_view text)
	{
		std::string normalized;
		normalized.reserve(text.size());

		bool separator_pending = false;

		for (const char raw_character : text)
		{
			const unsigned char character = static_cast<unsigned char>(raw_character);

			const bool is_separator = std::isspace(character) != 0
				|| character == '_'
				|| character == '-';

			if (is_separator)
			{
				// Leading separators are dropped, and a trailing run never gets
				// emitted because emission is deferred until a real character follows.
				separator_pending = !normalized.empty();
				continue;
			}

			if (separator_pending)
			{
				normalized.push_back(' ');
				separator_pending = false;
			}

			normalized.push_back(static_cast<char>(std::tolower(character)));
		}

		// Removed after collapsing rather than before, so "The   Bus" and "the-bus"
		// are both reached. Once only: a track named "the the" keeps one of them.
		if (normalized.size() > leading_definite_article.size()
			&& normalized.compare(0, leading_definite_article.size(), leading_definite_article) == 0)
		{
			normalized.erase(0, leading_definite_article.size());
		}

		return normalized;
	}

	// Whether a normalized pattern matches a normalized candidate string at the given
	// tier. Both empty operands are refused rather than treated as a match: an empty
	// pattern is whitespace the producer typed, and an empty candidate is an unnamed
	// track, and neither is a reference to anything.
	inline bool matches_at_precision(
		std::string_view normalized_pattern,
		std::string_view normalized_candidate,
		MatchPrecision precision)
	{
		if (normalized_pattern.empty() || normalized_candidate.empty())
		{
			return false;
		}

		if (precision == MatchPrecision::exact)
		{
			return normalized_pattern == normalized_candidate;
		}

		return normalized_candidate.find(normalized_pattern) != std::string_view::npos;
	}

	// Whether two GUIDs name the same object.
	//
	// Compared case-insensitively because the schema's pattern accepts either case
	// while REAPER's `guidToString` emits upper — so a GUID the agent copied out of a
	// payload, lower-cased somewhere along the way, still resolves. Nothing else about
	// a GUID is loosened: no containment tier, no normalization of the braces.
	inline bool guids_match(std::string_view left, std::string_view right)
	{
		if (left.size() != right.size() || left.empty())
		{
			return false;
		}

		for (std::size_t position = 0; position < left.size(); ++position)
		{
			const unsigned char left_character = static_cast<unsigned char>(left[position]);
			const unsigned char right_character = static_cast<unsigned char>(right[position]);

			if (std::tolower(left_character) != std::tolower(right_character))
			{
				return false;
			}
		}

		return true;
	}

	// ---------------------------------------------------------------------------
	// Structural role phrases
	// ---------------------------------------------------------------------------

	// The producer-facing phrases that name a role, already in normalized form.
	//
	// Each list opens with the schema spelling — normalization turns
	// `summing_folder_parent` into "summing folder parent" — and then adds the words
	// producers actually use. The lists are short on purpose. This is the last source
	// consulted, so a phrase added here can only be reached when the pattern matched
	// no track name and no learned alias, and a wrong role at that point resolves
	// confidently to the wrong object.
	//
	// "track" is deliberately not a phrase for `normal`. It would match on any
	// pattern that had already failed the other two sources, turning a typo into an
	// ambiguity over most of the project rather than the unresolved refusal that
	// actually describes what happened.
	inline std::vector<std::string_view> structural_role_phrases(context::StructuralRole role)
	{
		switch (role)
		{
			case context::StructuralRole::summing_folder_parent:
				return {"summing folder parent", "folder parent", "folder", "summing bus", "submix", "group"};
			case context::StructuralRole::silent_folder_parent:
				return {"silent folder parent", "silent folder", "silent bus"};
			case context::StructuralRole::aux_return:
				return {"aux return", "aux", "return", "return track", "effects return"};
			case context::StructuralRole::normal:
				return {"normal", "normal track"};
			case context::StructuralRole::master:
				return {"master", "master track", "master bus"};
		}

		return {};
	}

	// Which role, if any, a normalized pattern names. Exact phrase match only — see
	// the file header for why containment is excluded from this source.
	inline std::optional<context::StructuralRole> structural_role_from_producer_phrase(
		std::string_view normalized_pattern)
	{
		if (normalized_pattern.empty())
		{
			return std::nullopt;
		}

		for (const context::StructuralRole role : context::all_structural_roles)
		{
			for (const std::string_view& phrase : structural_role_phrases(role))
			{
				if (normalized_pattern == phrase)
				{
					return role;
				}
			}
		}

		return std::nullopt;
	}

	// ---------------------------------------------------------------------------
	// Describing a selector
	// ---------------------------------------------------------------------------

	inline std::string describe_track_selector(const TrackSelector& selector)
	{
		if (const TrackGuidSelector* const by_guid = std::get_if<TrackGuidSelector>(&selector))
		{
			return "guid " + by_guid->guid;
		}

		const TrackNamePatternSelector& by_pattern = std::get<TrackNamePatternSelector>(selector);

		return "name pattern \"" + by_pattern.name_pattern + "\"";
	}

	inline std::string describe_marker_or_region_guid_selector(std::string_view guid)
	{
		return "marker or region guid " + std::string{guid};
	}

	// ---------------------------------------------------------------------------
	// Resolution
	// ---------------------------------------------------------------------------

	namespace detail
	{
		inline ResolvedTrack as_resolved_track(
			const ResolvableTrack& track,
			ResolutionSource source,
			MatchPrecision precision)
		{
			ResolvedTrack resolved;
			resolved.reference = TrackReference{track.guid, track.name};
			resolved.track = track.track;
			resolved.project_index = track.project_index;
			resolved.is_master_track = track.is_master_track;
			resolved.source = source;
			resolved.precision = precision;

			return resolved;
		}

		// The strings one source offers for one track. Returned by value because the
		// alias store returns by value and a reference into a temporary is how that
		// goes wrong.
		inline std::vector<std::string> candidate_strings_for_source(
			const ResolvableTrack& track,
			ResolutionSource source,
			const LearnedAliasLookup& learned_aliases)
		{
			if (source == ResolutionSource::track_name)
			{
				return {track.name};
			}

			if (source == ResolutionSource::learned_alias)
			{
				return learned_aliases.learned_aliases_for_track(track);
			}

			return {};
		}
	}

	// What one source had to offer. Empty `candidates` means this source matched
	// nothing and the ladder moves on; anything else means it won, whether that
	// resolves or turns out to be ambiguous.
	struct SourceCandidates
	{
		ResolutionSource source = ResolutionSource::track_name;
		MatchPrecision precision = MatchPrecision::exact;
		std::vector<ResolvedTrack> candidates;
	};

	// Everything one source matches for one pattern, at the strongest tier that
	// yields anything.
	//
	// Exposed rather than kept private because requirement 7.2 is a statement about
	// what each source produces, and a test of the precedence rule wants to compare
	// the ladder's answer against the individual sources' answers rather than
	// reconstruct them.
	inline SourceCandidates collect_candidates_from_source(
		const std::vector<ResolvableTrack>& tracks_in_project_order,
		const LearnedAliasLookup& learned_aliases,
		std::string_view normalized_pattern,
		ResolutionSource source)
	{
		SourceCandidates found;
		found.source = source;

		if (normalized_pattern.empty())
		{
			return found;
		}

		if (source == ResolutionSource::structural_role)
		{
			const std::optional<context::StructuralRole> role =
				structural_role_from_producer_phrase(normalized_pattern);

			if (!role.has_value())
			{
				return found;
			}

			// The phrase matched a role exactly, so every track carrying that role is
			// a candidate. A role names a class of tracks rather than one track, so
			// this is the source most likely to be ambiguous — which is the correct
			// outcome, not a shortcoming: "the bus" in a project with three buses is a
			// question.
			for (const ResolvableTrack& track : tracks_in_project_order)
			{
				if (track.structural_role == role)
				{
					found.candidates.push_back(
						detail::as_resolved_track(track, source, MatchPrecision::exact));
				}
			}

			return found;
		}

		for (const MatchPrecision precision : {MatchPrecision::exact, MatchPrecision::imperfect})
		{
			for (const ResolvableTrack& track : tracks_in_project_order)
			{
				const std::vector<std::string> candidate_strings =
					detail::candidate_strings_for_source(track, source, learned_aliases);

				for (const std::string& candidate_string : candidate_strings)
				{
					if (!matches_at_precision(normalized_pattern, normalize_for_matching(candidate_string), precision))
					{
						continue;
					}

					found.candidates.push_back(detail::as_resolved_track(track, source, precision));

					// One candidate per track. A track with two aliases that both
					// match is still one track, and listing it twice would turn a
					// resolvable reference into an ambiguity.
					break;
				}
			}

			if (!found.candidates.empty())
			{
				found.precision = precision;
				return found;
			}
		}

		return found;
	}

	// Requirement 7.1: resolve a GUID by walking the track list and comparing.
	//
	// A walk rather than a lookup, and that is a decision rather than an omission.
	// REAPER offers no GUID-to-track index, so the alternative is a map the extension
	// maintains itself — and a cache of a structure the producer edits continuously
	// introduces a staleness window in which the resolver confidently returns a track
	// that has been deleted or replaced. A linear walk over a few hundred tracks is
	// not a cost worth trading correctness for.
	//
	// Two GUIDs matching is not something REAPER produces, and it is reported as
	// ambiguous rather than resolved to the first. A duplicate means the snapshot is
	// wrong, and resolving anyway would pick a track for a reason nobody can explain.
	inline TrackResolution resolve_track_by_guid(
		const std::vector<ResolvableTrack>& tracks_in_project_order,
		std::string_view guid)
	{
		std::string selector_description = "guid " + std::string{guid};

		std::vector<ResolvedTrack> matches;

		for (const ResolvableTrack& track : tracks_in_project_order)
		{
			if (guids_match(track.guid, guid))
			{
				matches.push_back(
					detail::as_resolved_track(track, ResolutionSource::guid, MatchPrecision::exact));
			}
		}

		if (matches.empty())
		{
			return UnresolvedSelector{std::move(selector_description)};
		}

		if (matches.size() == 1)
		{
			return matches.front();
		}

		return AmbiguousTrackSelector{
			std::move(selector_description),
			ResolutionSource::guid,
			MatchPrecision::exact,
			std::move(matches)
		};
	}

	// Requirement 7.2, 7.3, 7.4: the precedence ladder.
	//
	// Read the loop body as the rule: a source that matched nothing is skipped, and a
	// source that matched anything at all returns from here. There is no path on
	// which a later source is consulted after an earlier one produced candidates, and
	// no path on which an ambiguity continues down the ladder.
	inline TrackResolution resolve_track_by_name_pattern(
		const std::vector<ResolvableTrack>& tracks_in_project_order,
		const LearnedAliasLookup& learned_aliases,
		std::string_view name_pattern)
	{
		const std::string normalized_pattern = normalize_for_matching(name_pattern);
		std::string selector_description = "name pattern \"" + std::string{name_pattern} + "\"";

		for (const ResolutionSource source : name_pattern_sources_in_precedence_order)
		{
			SourceCandidates found = collect_candidates_from_source(
				tracks_in_project_order,
				learned_aliases,
				normalized_pattern,
				source);

			if (found.candidates.empty())
			{
				continue;
			}

			if (found.candidates.size() == 1)
			{
				return found.candidates.front();
			}

			return AmbiguousTrackSelector{
				std::move(selector_description),
				found.source,
				found.precision,
				std::move(found.candidates)
			};
		}

		return UnresolvedSelector{std::move(selector_description)};
	}

	// The whole of requirement 7 for a track: dispatch on which half of the selector
	// oneOf arrived.
	inline TrackResolution resolve_track(
		const std::vector<ResolvableTrack>& tracks_in_project_order,
		const LearnedAliasLookup& learned_aliases,
		const TrackSelector& selector)
	{
		if (const TrackGuidSelector* const by_guid = std::get_if<TrackGuidSelector>(&selector))
		{
			return resolve_track_by_guid(tracks_in_project_order, by_guid->guid);
		}

		return resolve_track_by_name_pattern(
			tracks_in_project_order,
			learned_aliases,
			std::get<TrackNamePatternSelector>(selector).name_pattern);
	}

	// Requirement 7.8: marker and region GUIDs resolve the same way — by enumeration
	// and comparison, with the same three outcomes.
	//
	// Newer REAPER builds do offer `GetRegionOrMarker` with a GUID argument, which
	// would make this a single call. Enumerating anyway keeps one resolution shape
	// across tracks, markers, and regions, and keeps the logic on this side of the SDK
	// boundary where the suite can reach it. A project's marker count is not a
	// performance question.
	//
	// No name-pattern ladder here. Marker and region names are not unique, not
	// required, and not what the tool schemas address them by — the tools take a GUID,
	// and there is no refusal reason in the taxonomy for an unresolvable marker
	// pattern. An unresolved marker GUID is reported by the calling tool as a failed
	// action with a reason (requirement 9.5), not as one of the two track refusals.
	inline MarkerOrRegionResolution resolve_marker_or_region_by_guid(
		const std::vector<ResolvableMarkerOrRegion>& markers_and_regions_in_enumeration_order,
		std::string_view guid)
	{
		std::string selector_description = describe_marker_or_region_guid_selector(guid);

		std::vector<ResolvedMarkerOrRegion> matches;

		for (const ResolvableMarkerOrRegion& object : markers_and_regions_in_enumeration_order)
		{
			if (guids_match(object.guid, guid))
			{
				matches.push_back(ResolvedMarkerOrRegion{object, ResolutionSource::guid});
			}
		}

		if (matches.empty())
		{
			return UnresolvedSelector{std::move(selector_description)};
		}

		if (matches.size() == 1)
		{
			return matches.front();
		}

		return AmbiguousMarkerOrRegionSelector{
			std::move(selector_description),
			ResolutionSource::guid,
			MatchPrecision::exact,
			std::move(matches)
		};
	}

	// ---------------------------------------------------------------------------
	// Refusals
	// ---------------------------------------------------------------------------

	// Requirement 7.3. `blocking` is empty, and the schema permits that where the
	// reason is self-describing: there is no entity standing in the way, which is
	// exactly the problem. The selector is not repeated into the payload because the
	// payload has no property for it and the agent sent it.
	inline ToolResultRefusal build_unresolved_track_selector_refusal()
	{
		ToolResultRefusal refusal;
		refusal.reason = std::string{unresolved_track_selector_refusal_reason};

		return refusal;
	}

	// Requirement 7.4: every candidate named, with its GUID attached, so the agent
	// disambiguates by picking one and addressing it by GUID on the retry.
	//
	// The GUID is the part that matters. Handing back names alone leaves the agent
	// inventing a narrower pattern, which is guessing one level up from the guessing
	// this component refuses to do.
	//
	// When there are more candidates than `blocking` can carry, the final slot
	// becomes a count of what did not fit rather than the list silently ending. A
	// truncated list that looks complete would have the agent believe it had seen
	// every candidate.
	inline ToolResultRefusal build_ambiguous_track_selector_refusal(
		const AmbiguousTrackSelector& ambiguous)
	{
		ToolResultRefusal refusal;
		refusal.reason = std::string{ambiguous_track_selector_refusal_reason};
		refusal.acknowledgement_field = std::string{ambiguous_track_selector_acknowledgement_field};

		const bool overflows = ambiguous.candidates.size() > maximum_blocking_entities;
		const std::size_t listed_count = overflows
			? maximum_blocking_entities - 1
			: ambiguous.candidates.size();

		refusal.blocking.reserve(overflows ? maximum_blocking_entities : listed_count);

		for (std::size_t candidate_index = 0; candidate_index < listed_count; ++candidate_index)
		{
			const ResolvedTrack& candidate = ambiguous.candidates[candidate_index];

			BlockingEntity entity;
			entity.kind = std::string{track_blocking_entity_kind};
			entity.description = candidate.reference.name.empty()
				? std::string{"unnamed track"}
				: candidate.reference.name;
			entity.guid = candidate.reference.guid;

			refusal.blocking.push_back(std::move(entity));
		}

		if (overflows)
		{
			BlockingEntity overflow_entity;
			overflow_entity.kind = std::string{track_blocking_entity_kind};
			overflow_entity.description = std::to_string(ambiguous.candidates.size() - listed_count)
				+ " further tracks match and are not listed";

			refusal.blocking.push_back(std::move(overflow_entity));
		}

		return refusal;
	}

	// ---------------------------------------------------------------------------
	// What the caller does with an outcome
	// ---------------------------------------------------------------------------

	// Whether the operation about to run is one the agent may perform directly or one
	// the producer confirms first, per the tool classification.
	enum class OperationRisk
	{
		safe,
		destructive
	};

	// The decision a resolution leads to, for requirements 7.6 and 7.7.
	//
	// Both of those requirements are satisfied by the same two fields, which is worth
	// noticing rather than modelling as two mechanisms. A safe operation proceeds and
	// reports which track it used; a destructive one proceeds to a confirmation that
	// names the resolved track. The reporting is identical — one names a track in a
	// result, the other names the same track in a prompt — so `resolved_track` and
	// `report` serve both and `confirmation_required` says which path it is.
	//
	// Requirement 7.7 is specifically that the resolver names the track in the
	// confirmation **that was already happening**. No confirmation is introduced here:
	// a destructive tool was going to ask anyway, and this supplies the words for the
	// track it is about to act on.
	struct TrackResolutionDisposition
	{
		bool proceed = false;

		// Only ever true alongside `proceed` — there is nothing to confirm about a
		// refusal.
		bool confirmation_required = false;

		// Present exactly when `proceed`.
		std::optional<TrackReference> resolved_track;

		// One line naming what was resolved and how. Requirement 7.6's "report which
		// track it used", and the track-naming half of requirement 7.7's confirmation.
		std::string report;

		// Present exactly when not `proceed`.
		std::optional<ToolResultRefusal> refusal;
	};

	// Reads as: resolved proceeds, everything else refuses.
	//
	// Ambiguity refuses regardless of risk. `trackSelector`'s own description is
	// looser about this — it says the extension refuses "when a pattern matches no
	// track, or matches several and the operation is destructive" — but requirements
	// 7.4 and 7.5 are unconditional, and they are the stricter reading. A safe
	// operation on the wrong one of several candidates is still a change to the
	// producer's session, and the round trip that resolves it is cheap.
	//
	// An imperfect match proceeds. That is requirement 7.6 read as written: the
	// judgement it asks for is about how confident the match was, not about whether
	// to act, and the imperfect tier is only reached when the exact tier found
	// nothing. What makes it safe is that the track is named in what comes back, so a
	// producer who sees the wrong track named can undo one step.
	inline TrackResolutionDisposition dispose_of_track_resolution(
		const TrackResolution& resolution,
		OperationRisk risk)
	{
		TrackResolutionDisposition disposition;

		if (const ResolvedTrack* const resolved = std::get_if<ResolvedTrack>(&resolution))
		{
			disposition.proceed = true;
			disposition.confirmation_required = risk == OperationRisk::destructive;
			disposition.resolved_track = resolved->reference;

			std::string report{"resolved to track \""};
			report += resolved->reference.name.empty() ? std::string{"unnamed track"} : resolved->reference.name;
			report += "\" ";
			report += resolved->reference.guid;
			report += " by ";
			report += describe_resolution_source(resolved->source);
			report += " (";
			report += describe_match_precision(resolved->precision);
			report += " match)";

			disposition.report = std::move(report);

			return disposition;
		}

		if (const AmbiguousTrackSelector* const ambiguous = std::get_if<AmbiguousTrackSelector>(&resolution))
		{
			disposition.refusal = build_ambiguous_track_selector_refusal(*ambiguous);
			disposition.report = "refused " + ambiguous->selector_description + ": "
				+ std::to_string(ambiguous->candidates.size()) + " candidates matched by "
				+ std::string{describe_resolution_source(ambiguous->source)};

			return disposition;
		}

		const UnresolvedSelector& unresolved = std::get<UnresolvedSelector>(resolution);

		disposition.refusal = build_unresolved_track_selector_refusal();
		disposition.report = "refused " + unresolved.selector_description + ": nothing matched";

		return disposition;
	}

	// ---------------------------------------------------------------------------
	// The REAPER-facing implementations
	//
	// Declared here, defined in object_resolver.cpp, which is the only translation
	// unit in this component that includes the SDK. Held as pointers to
	// forward-declared SDK types so this header stays includable from the suite.
	// ---------------------------------------------------------------------------

	// Walks REAPER's track list and reads a GUID and a name per track.
	//
	// The master track is appended after the indexed list, because "the master" is a
	// reference a producer makes and `GetTrack` does not return it. Its
	// `project_index` is negative and `is_master_track` is set, so a caller that must
	// not act on the master can tell.
	class ReaperTrackListSource final : public TrackListSource
	{
	public:
		// Supplies the derived role for a track, keyed by GUID. Requirement 6.6 has
		// the Project Context Builder deriving roles already, so this is a callback
		// into work that exists rather than a second derivation — and it is optional,
		// because a resolver constructed before the context builder runs should refuse
		// role patterns rather than answer them wrongly.
		using StructuralRoleProvider =
			std::function<std::optional<context::StructuralRole>(const std::string& track_guid)>;

		// Null project means REAPER's active project, which is what its API takes zero
		// to mean.
		explicit ReaperTrackListSource(
			ReaProject* project = nullptr,
			StructuralRoleProvider structural_role_provider = {});

		std::vector<ResolvableTrack> tracks_in_project_order() const override;

	private:
		ReaProject* project_;
		StructuralRoleProvider structural_role_provider_;
	};

	// Enumerates REAPER's markers and regions and reads a GUID, name, bounds, and
	// indices for each.
	class ReaperMarkerAndRegionSource final : public MarkerAndRegionSource
	{
	public:
		explicit ReaperMarkerAndRegionSource(ReaProject* project = nullptr);

		std::vector<ResolvableMarkerOrRegion> markers_and_regions_in_enumeration_order() const override;

	private:
		ReaProject* project_;
	};
}

#endif
