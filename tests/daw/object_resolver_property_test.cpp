// Object resolution — the formal properties, over generated track sets and patterns.
//
// `object_resolver_test.cpp` is the example file: 40 hand-written cases, each chosen
// because it is a way to get resolution wrong. It already pins the GUID walk, the
// precedence order as an array, the worked precedence collisions, the match tiers,
// the role source, the two refusals including the 600-candidate overflow, the
// dispositions, and a bounded 4 x 11 three-outcome grid. This file is the other half:
// properties 4 to 7 are stated over *any* track set and *any* pattern, so they are
// checked over a corpus nobody chose one case at a time.
//
// **Validates: Requirements 7.2, 7.3, 7.4, 7.5**
//
// ---------------------------------------------------------------------------
// What is genuinely new here, relative to the examples
//
//   - Property 4 is checked against `collect_candidates_from_source` rather than
//     against a restatement of the rule. For every generated pair, what each of the
//     three sources offers is computed independently, and the ladder's answer is
//     asserted equal to the first non-empty one — candidate for candidate, with its
//     source and its match tier. The example file does this for one project; here it
//     is the whole corpus, and the cases where *several* sources would match are
//     counted, because precedence is unobservable when only one source matches.
//   - Property 4's sharp sub-claim gets its own counted subset: an earlier source
//     yielding two candidates is ambiguous and does not fall through to a later
//     source that would have yielded one. Counted for all three source pairings.
//   - Property 5 is quantified over patterns that match nothing anywhere, including
//     near-misses the implementation deliberately rejects — reverse containment
//     ("the vox please" against a track named "Vox") and role containment ("bus"
//     inside the phrase "master bus"). Those two are counted separately, since a
//     no-match corpus made only of nonsense strings would not test the refusal that
//     matters.
//   - Property 6 separates two claims the example file checks at one candidate count.
//     The claim about the *outcome* is that every candidate is present with its GUID,
//     at any candidate count. The claim about the *refusal* is different: the schema
//     caps `blocking` at 512, so the payload lists 511 and counts the remainder, and
//     the count plus the listing has to account for every candidate. Both are swept
//     over candidate counts either side of the cap.
//   - Property 7's closure is partly structural and is asserted as such rather than
//     tested: `TrackResolution` is a `std::variant` of exactly three alternatives, so
//     a fourth outcome is not expressible and no sweep could find one. What the sweep
//     checks is the part with content — that `is_resolved`, `is_unresolved`, and
//     `is_ambiguous` partition the space, and that an ambiguous outcome always
//     carries at least two candidates.
//
// ---------------------------------------------------------------------------
// Where the corpus comes from
//
// Two corpora, following the convention the rest of this suite settled on
// (`tests/daw/folder_invariant_keeper_test.cpp`, `tests/cycle_detector_property_test.cpp`).
// Catch2 in this build has no generator library, and adding one would mean touching
// CMakeLists.txt for a dependency only the tests want.
//
// Exhaustive over a chosen track alphabet. A track's resolvable content is a name, a
// set of learned aliases, and an optional role, and the interesting part is not the
// strings themselves but how they collide — so eight track shapes are chosen for
// their collisions and every project of up to four tracks over those shapes is
// enumerated. That is 4,681 projects, crossed with 24 patterns: 112,344 resolutions.
// Both counts are asserted, for the reason the folder and routing files assert theirs
// — a sweep that silently stopped enumerating would make every property below
// vacuously true.
//
// Deterministically seeded beyond that, because the shapes cap ambiguity at four
// candidates and properties 6 and 7 are claims about arbitrary candidate counts. The
// sampled corpus draws names, aliases, and roles from pools with heavy reuse over one
// to twelve tracks, so large ambiguities appear. `DeterministicBytes` is the xorshift
// source from `tests/daw/alias_store_test.cpp`; the seed is fixed and reported, so a
// failure here is reproducible on any machine.
//
// Every coverage guard below is set from a measured value, not from an estimate. The
// measurements, for the record, over the exhaustive corpus's 112,344 resolutions and
// the sampled corpus's 72,000:
//
//                                              exhaustive     sampled
//   resolved / unresolved / ambiguous     31263/63944/17137   22529/33196/16275
//   won by track name / alias / role        28640/8046/11714   22525/9496/6783
//   two or more sources matched                      24326       17834
//   all three sources matched                         1992        2311
//   a later source named another track               21026       16716
//   name ambiguous, alias would resolve               3548        3013
//   name ambiguous, role would resolve                 165         445
//   alias ambiguous, role would resolve                968        1050
//   no match: a track name inside the pattern         9040        4823
//   no match: an alias inside the pattern             7940        3884
//   no match: pattern inside a role phrase            3126        1269
//   no match: pattern empty after normalizing         9362           —
//   ambiguities by track name / alias / role  11152/3164/2821   9957/3474/2844
//   ambiguities at the imperfect tier                 2550        3362
//   widest ambiguity / distinct widths                 4 / 3       7 / 6
//
// The refusal cap is swept separately over eleven candidate counts, eight at or below
// `blocking`'s maximum of 512 and three above it.
//
// ---------------------------------------------------------------------------
// The alias seam
//
// `LearnedAliasLookup` takes the whole `ResolvableTrack` and returns every alias the
// track holds, which is what the Alias Store provides: the store is keyed by
// `MediaTrack*` because requirement 8.1 keeps the aliases on the track, and a
// `ResolvableTrack` carries that handle alongside the GUID. So this file substitutes
// a map keyed on the GUID — the field a synthetic track list has — and
// `daw/stored_alias_lookup.h` keys on the handle in the extension. Neither has to
// pretend to be the other.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/structural_role.h>
#include <daw/object_resolver.h>

namespace
{
	using sesh_ai::context::StructuralRole;
	using sesh_ai::daw::AmbiguousTrackSelector;
	using sesh_ai::daw::BlockingEntity;
	using sesh_ai::daw::LearnedAliasLookup;
	using sesh_ai::daw::MatchPrecision;
	using sesh_ai::daw::OperationRisk;
	using sesh_ai::daw::ResolutionSource;
	using sesh_ai::daw::ResolvableTrack;
	using sesh_ai::daw::ResolvedTrack;
	using sesh_ai::daw::SourceCandidates;
	using sesh_ai::daw::ToolResultRefusal;
	using sesh_ai::daw::TrackResolution;
	using sesh_ai::daw::TrackResolutionDisposition;
	using sesh_ai::daw::UnresolvedSelector;
	using sesh_ai::daw::collect_candidates_from_source;
	using sesh_ai::daw::describe_match_precision;
	using sesh_ai::daw::describe_resolution_source;
	using sesh_ai::daw::dispose_of_track_resolution;
	using sesh_ai::daw::is_ambiguous;
	using sesh_ai::daw::is_resolved;
	using sesh_ai::daw::is_unresolved;
	using sesh_ai::daw::maximum_blocking_entities;
	using sesh_ai::daw::name_pattern_sources_in_precedence_order;
	using sesh_ai::daw::normalize_for_matching;
	using sesh_ai::daw::resolve_track_by_name_pattern;
	using sesh_ai::daw::structural_role_phrases;

	constexpr std::size_t source_count = 3;

	// -----------------------------------------------------------------------
	// Property 7, the structural half
	//
	// The closed outcome set is a type-level claim, so it is checked at compile time
	// rather than swept for. No corpus could find a fourth outcome, because naming one
	// would not compile.
	// -----------------------------------------------------------------------

	static_assert(
		std::variant_size_v<TrackResolution> == source_count,
		"requirement 7.5 is three outcomes, and TrackResolution is what makes that closed");

	static_assert(
		std::is_same_v<std::variant_alternative_t<0, TrackResolution>, ResolvedTrack>,
		"outcome one is a single resolved track");

	static_assert(
		std::is_same_v<std::variant_alternative_t<1, TrackResolution>, UnresolvedSelector>,
		"outcome two is an unresolved selector");

	static_assert(
		std::is_same_v<std::variant_alternative_t<2, TrackResolution>, AmbiguousTrackSelector>,
		"outcome three is an ambiguous selector");

	// -----------------------------------------------------------------------
	// The alias store stand-in
	// -----------------------------------------------------------------------

	class RecordedAliases final : public LearnedAliasLookup
	{
	public:
		void learn(std::string track_guid, std::vector<std::string> aliases)
		{
			aliases_by_track_guid_[std::move(track_guid)] = std::move(aliases);
		}

		std::vector<std::string> learned_aliases_for_track(const ResolvableTrack& track) const override
		{
			const auto recorded = aliases_by_track_guid_.find(track.guid);

			if (recorded == aliases_by_track_guid_.end())
			{
				return {};
			}

			return recorded->second;
		}

	private:
		std::unordered_map<std::string, std::vector<std::string>> aliases_by_track_guid_;
	};

	// REAPER's braced GUID form from an ordinal, matching the schemas' pattern. Unique
	// per position within a project, so no generated project has a duplicate GUID and
	// the distinctness assertions below are about the resolver rather than the fixture.
	std::string guid_for(std::size_t ordinal)
	{
		std::string digits = std::to_string(ordinal);

		while (digits.size() < 12)
		{
			digits.insert(digits.begin(), '0');
		}

		return "{00000000-0000-0000-0000-" + digits + "}";
	}

	// -----------------------------------------------------------------------
	// Generated projects
	// -----------------------------------------------------------------------

	struct GeneratedProject
	{
		std::vector<ResolvableTrack> tracks;
		RecordedAliases aliases;
	};

	std::string describe_project(const GeneratedProject& project)
	{
		if (project.tracks.empty())
		{
			return "tracks: none";
		}

		std::string description{"tracks:"};

		for (const ResolvableTrack& track : project.tracks)
		{
			description += " [" + track.guid + " name \"" + track.name + "\" aliases";

			const std::vector<std::string> aliases =
				project.aliases.learned_aliases_for_track(track);

			if (aliases.empty())
			{
				description += " none";
			}

			for (const std::string& alias : aliases)
			{
				description += " \"" + alias + "\"";
			}

			description += track.structural_role.has_value()
				? (" role " + std::string{sesh_ai::context::to_schema_string(*track.structural_role)})
				: std::string{" role absent"};

			description += "]";
		}

		return description;
	}

	// One track's resolvable content, chosen for how it collides with the others.
	struct TrackShape
	{
		std::string_view name;
		std::vector<std::string> aliases;
		std::optional<StructuralRole> structural_role;
	};

	// The alphabet. Eight shapes, each one there to make some collision reachable:
	//
	//   0  a plain name, so two of them is a name ambiguity.
	//   1  a name that matches one pattern while its alias and its role both match
	//      another — the dense case where two later sources agree and lose anyway.
	//   2  a name matching "vox" only by containment, with an exact alias for it, so
	//      the tier ladder and the source ladder interact.
	//   3  unnamed and master, so the role source is the only way to reach it.
	//   4  a name that is also a role phrase, so name and role collide exactly.
	//   5  a folder parent, reachable by several role phrases and by no name.
	//   6  name, alias, and role all live at once.
	//   7  a name that is a role phrase for a role it does not carry.
	const std::vector<TrackShape>& track_shapes()
	{
		static const std::vector<TrackShape> shapes{
			TrackShape{"Vox", {}, StructuralRole::normal},
			TrackShape{"Vox", {"aux return"}, StructuralRole::aux_return},
			TrackShape{"Lead Vox", {"vox", "aux return"}, StructuralRole::normal},
			TrackShape{"", {}, StructuralRole::master},
			TrackShape{"Aux", {"the vox"}, StructuralRole::aux_return},
			TrackShape{"Drums", {}, StructuralRole::summing_folder_parent},
			TrackShape{"Vox", {"vox"}, StructuralRole::aux_return},
			TrackShape{"Return", {}, std::nullopt}
		};

		return shapes;
	}

	GeneratedProject project_from_shape_indices(const std::vector<std::size_t>& shape_indices)
	{
		GeneratedProject project;
		project.tracks.reserve(shape_indices.size());

		for (std::size_t position = 0; position < shape_indices.size(); ++position)
		{
			const TrackShape& shape = track_shapes()[shape_indices[position]];

			ResolvableTrack track;
			track.guid = guid_for(position);
			track.name = std::string{shape.name};
			track.project_index = static_cast<int>(position);
			track.structural_role = shape.structural_role;
			track.is_master_track = shape.structural_role == StructuralRole::master;

			if (track.is_master_track)
			{
				// REAPER's master is not in the indexed track list, and the resolver
				// carries that rather than inferring it.
				track.project_index = -1;
			}

			if (!shape.aliases.empty())
			{
				project.aliases.learn(track.guid, shape.aliases);
			}

			project.tracks.push_back(std::move(track));
		}

		return project;
	}

	// Every project of zero to `maximum_tracks` tracks over the shape alphabet. Zero
	// is included: an empty project is a track set, and requirement 7.5 says "any".
	std::vector<GeneratedProject> enumerate_projects(std::size_t maximum_tracks)
	{
		std::vector<GeneratedProject> projects;

		for (std::size_t track_count = 0; track_count <= maximum_tracks; ++track_count)
		{
			std::vector<std::size_t> shape_indices(track_count, 0);

			while (true)
			{
				projects.push_back(project_from_shape_indices(shape_indices));

				std::size_t position = track_count;

				while (position > 0)
				{
					--position;
					++shape_indices[position];

					if (shape_indices[position] < track_shapes().size())
					{
						break;
					}

					shape_indices[position] = 0;

					if (position == 0)
					{
						position = track_count;
						break;
					}
				}

				if (position == track_count)
				{
					break;
				}
			}
		}

		return projects;
	}

	// Deterministic byte source, as in `tests/daw/alias_store_test.cpp`.
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

		bool chance_in(std::size_t bound) { return below(bound) == 0; }

	private:
		std::uint32_t state_;
	};

	// Pools with heavy reuse, so duplicates — and therefore ambiguities much wider
	// than the exhaustive alphabet reaches — are common rather than incidental.
	const std::array<std::string_view, 10> sampled_name_pool{
		"Vox", "Lead Vox", "Vox Double", "Drums", "Drum_Bus", "Aux", "Return", "Master", "", "the vox"
	};

	const std::array<std::string_view, 7> sampled_alias_pool{
		"vox", "aux return", "master", "submix", "the drum bus", "return", "group"
	};

	GeneratedProject generate_project(
		DeterministicBytes& bytes,
		std::size_t minimum_tracks,
		std::size_t maximum_tracks)
	{
		const std::size_t track_count =
			minimum_tracks + bytes.below((maximum_tracks - minimum_tracks) + 1);

		GeneratedProject project;
		project.tracks.reserve(track_count);

		for (std::size_t position = 0; position < track_count; ++position)
		{
			ResolvableTrack track;
			track.guid = guid_for(position);
			track.name = std::string{sampled_name_pool[bytes.below(sampled_name_pool.size())]};
			track.project_index = static_cast<int>(position);

			// A role is absent a fifth of the time, which is the shape a resolver
			// running before the context builder sees.
			if (!bytes.chance_in(5))
			{
				const StructuralRole role =
					sesh_ai::context::all_structural_roles[bytes.below(
						sesh_ai::context::all_structural_roles.size())];

				track.structural_role = role;
				track.is_master_track = role == StructuralRole::master;

				if (track.is_master_track)
				{
					track.project_index = -1;
				}
			}

			const std::size_t alias_count = bytes.below(3);

			if (alias_count > 0)
			{
				std::vector<std::string> aliases;
				aliases.reserve(alias_count);

				for (std::size_t index = 0; index < alias_count; ++index)
				{
					aliases.push_back(
						std::string{sampled_alias_pool[bytes.below(sampled_alias_pool.size())]});
				}

				project.aliases.learn(track.guid, std::move(aliases));
			}

			project.tracks.push_back(std::move(track));
		}

		return project;
	}

	// -----------------------------------------------------------------------
	// The patterns
	//
	// Exact hits, containment hits, role phrases, role phrases for roles nothing
	// carries, the empty and whitespace cases, the article on its own, and the two
	// near-misses the implementation rejects on purpose.
	// -----------------------------------------------------------------------

	const std::vector<std::string>& patterns()
	{
		static const std::vector<std::string> generated{
			"",
			"   ",
			"the",
			"the ",
			"vox",
			"VOX",
			"Vox",
			"lead vox",
			"lead_vox",
			"the vox",
			"drums",
			"drum",
			"aux",
			"aux return",
			"return",
			"master",
			"master bus",
			"submix",
			"group",
			"folder",
			"bus",
			"voks",
			"the vox please",
			"{00000000-0000-0000-0000-000000000000}"
		};

		return generated;
	}

	// -----------------------------------------------------------------------
	// One evaluated case: what each source offers, and what the ladder answered
	// -----------------------------------------------------------------------

	struct ResolutionCase
	{
		std::string pattern;
		std::string normalized_pattern;
		std::array<SourceCandidates, source_count> by_precedence_order;
		TrackResolution resolution;

		std::optional<std::size_t> first_non_empty_source_position() const
		{
			for (std::size_t position = 0; position < by_precedence_order.size(); ++position)
			{
				if (!by_precedence_order[position].candidates.empty())
				{
					return position;
				}
			}

			return std::nullopt;
		}

		std::size_t non_empty_source_count() const
		{
			std::size_t counted = 0;

			for (const SourceCandidates& source : by_precedence_order)
			{
				if (!source.candidates.empty())
				{
					++counted;
				}
			}

			return counted;
		}
	};

	ResolutionCase evaluate(const GeneratedProject& project, const std::string& pattern)
	{
		ResolutionCase evaluated;
		evaluated.pattern = pattern;
		evaluated.normalized_pattern = normalize_for_matching(pattern);

		for (std::size_t position = 0; position < name_pattern_sources_in_precedence_order.size(); ++position)
		{
			evaluated.by_precedence_order[position] = collect_candidates_from_source(
				project.tracks,
				project.aliases,
				evaluated.normalized_pattern,
				name_pattern_sources_in_precedence_order[position]);
		}

		evaluated.resolution =
			resolve_track_by_name_pattern(project.tracks, project.aliases, pattern);

		return evaluated;
	}

	std::vector<std::string> guids_of(const std::vector<ResolvedTrack>& candidates)
	{
		std::vector<std::string> guids;
		guids.reserve(candidates.size());

		for (const ResolvedTrack& candidate : candidates)
		{
			guids.push_back(candidate.reference.guid);
		}

		return guids;
	}

	std::string join(const std::vector<std::string>& values)
	{
		std::string joined;

		for (const std::string& value : values)
		{
			if (!joined.empty())
			{
				joined += ", ";
			}

			joined += value;
		}

		return joined.empty() ? std::string{"none"} : joined;
	}

	std::string describe_source_candidates(const SourceCandidates& source)
	{
		return std::string{describe_resolution_source(source.source)} + " ("
			+ std::string{describe_match_precision(source.precision)} + "): "
			+ join(guids_of(source.candidates));
	}

	std::string describe_case(const GeneratedProject& project, const ResolutionCase& evaluated)
	{
		std::string description = describe_project(project)
			+ " | pattern \"" + evaluated.pattern
			+ "\" normalized \"" + evaluated.normalized_pattern + "\"";

		for (const SourceCandidates& source : evaluated.by_precedence_order)
		{
			description += " | " + describe_source_candidates(source);
		}

		return description;
	}

	std::string describe_outcome(const TrackResolution& resolution)
	{
		if (const ResolvedTrack* const resolved = std::get_if<ResolvedTrack>(&resolution))
		{
			return "resolved " + resolved->reference.guid
				+ " by " + std::string{describe_resolution_source(resolved->source)}
				+ " (" + std::string{describe_match_precision(resolved->precision)} + ")";
		}

		if (const AmbiguousTrackSelector* const ambiguous =
				std::get_if<AmbiguousTrackSelector>(&resolution))
		{
			return "ambiguous by " + std::string{describe_resolution_source(ambiguous->source)}
				+ " (" + std::string{describe_match_precision(ambiguous->precision)} + "): "
				+ join(guids_of(ambiguous->candidates));
		}

		return "unresolved";
	}

	bool same_track(const ResolvedTrack& left, const ResolvedTrack& right)
	{
		return left.reference == right.reference
			&& left.track == right.track
			&& left.project_index == right.project_index
			&& left.is_master_track == right.is_master_track
			&& left.source == right.source
			&& left.precision == right.precision;
	}

	// -----------------------------------------------------------------------
	// Near-miss classification, for property 5's coverage
	// -----------------------------------------------------------------------

	// A track name sitting inside the pattern rather than the other way round. The
	// header states reverse containment is excluded on purpose, so these cases are the
	// ones where a looser rule would have resolved and this one must refuse.
	bool a_track_name_sits_inside_the_pattern(
		const GeneratedProject& project,
		std::string_view normalized_pattern)
	{
		if (normalized_pattern.empty())
		{
			return false;
		}

		for (const ResolvableTrack& track : project.tracks)
		{
			const std::string normalized_name = normalize_for_matching(track.name);

			if (normalized_name.empty() || normalized_name == normalized_pattern)
			{
				continue;
			}

			if (normalized_pattern.find(normalized_name) != std::string_view::npos)
			{
				return true;
			}
		}

		return false;
	}

	// The pattern sitting strictly inside a role phrase of a role some track carries —
	// "bus" inside "master bus". The role source has no containment tier for exactly
	// this reason, so these cases must refuse too.
	bool the_pattern_sits_inside_a_present_role_phrase(
		const GeneratedProject& project,
		std::string_view normalized_pattern)
	{
		if (normalized_pattern.empty())
		{
			return false;
		}

		for (const ResolvableTrack& track : project.tracks)
		{
			if (!track.structural_role.has_value())
			{
				continue;
			}

			for (const std::string_view& phrase : structural_role_phrases(*track.structural_role))
			{
				if (phrase != normalized_pattern
					&& phrase.find(normalized_pattern) != std::string_view::npos)
				{
					return true;
				}
			}
		}

		return false;
	}

	// An alias sitting inside the pattern, the alias-source counterpart of reverse
	// containment.
	bool an_alias_sits_inside_the_pattern(
		const GeneratedProject& project,
		std::string_view normalized_pattern)
	{
		if (normalized_pattern.empty())
		{
			return false;
		}

		for (const ResolvableTrack& track : project.tracks)
		{
			for (const std::string& alias : project.aliases.learned_aliases_for_track(track))
			{
				const std::string normalized_alias = normalize_for_matching(alias);

				if (normalized_alias.empty() || normalized_alias == normalized_pattern)
				{
					continue;
				}

				if (normalized_pattern.find(normalized_alias) != std::string_view::npos)
				{
					return true;
				}
			}
		}

		return false;
	}

	// -----------------------------------------------------------------------
	// The corpora, built once
	// -----------------------------------------------------------------------

	constexpr std::size_t exhaustive_maximum_tracks = 4;
	constexpr std::size_t sampled_project_count = 3000;
	constexpr std::size_t sampled_minimum_tracks = 1;
	constexpr std::size_t sampled_maximum_tracks = 12;
	constexpr std::uint32_t sampled_project_seed = 0x0b1ec75u;

	const std::vector<GeneratedProject>& exhaustive_projects()
	{
		static const std::vector<GeneratedProject> projects =
			enumerate_projects(exhaustive_maximum_tracks);

		return projects;
	}

	const std::vector<GeneratedProject>& sampled_projects()
	{
		static const std::vector<GeneratedProject> projects = [] {
			DeterministicBytes bytes{sampled_project_seed};

			std::vector<GeneratedProject> generated;
			generated.reserve(sampled_project_count);

			for (std::size_t index = 0; index < sampled_project_count; ++index)
			{
				generated.push_back(
					generate_project(bytes, sampled_minimum_tracks, sampled_maximum_tracks));
			}

			return generated;
		}();

		return projects;
	}
}

// ---------------------------------------------------------------------------
// The corpus guard
//
// Every property below is a claim about "any track set and any pattern", worth
// exactly as much as the corpus behind it. A sweep that silently stopped enumerating
// would let all four pass while checking almost nothing, so the sizes are asserted
// from measured values and the collisions the properties depend on are counted.
// ---------------------------------------------------------------------------

TEST_CASE("the generated projects and patterns cover what the properties claim",
	"[daw][object_resolver][property]")
{
	SECTION("the exhaustive corpus is the whole product it claims to be")
	{
		// Eight shapes, projects of zero to four tracks: 8^0 + 8^1 + 8^2 + 8^3 + 8^4.
		REQUIRE(track_shapes().size() == 8);
		REQUIRE(exhaustive_projects().size() == 4681);
		REQUIRE(patterns().size() == 24);

		// The number of resolutions each sweep below performs.
		CHECK(exhaustive_projects().size() * patterns().size() == 112344);
	}

	SECTION("every shape reaches the corpus, and the empty project is in it")
	{
		std::vector<std::size_t> projects_containing_shape(track_shapes().size(), 0);
		std::size_t empty_project_count = 0;
		std::size_t largest_track_count = 0;

		for (const GeneratedProject& project : exhaustive_projects())
		{
			if (project.tracks.empty())
			{
				++empty_project_count;
			}

			largest_track_count = std::max(largest_track_count, project.tracks.size());

			for (std::size_t shape_index = 0; shape_index < track_shapes().size(); ++shape_index)
			{
				const TrackShape& shape = track_shapes()[shape_index];

				const bool present = std::any_of(
					project.tracks.begin(),
					project.tracks.end(),
					[&shape, &project](const ResolvableTrack& track) {
						return track.name == shape.name
							&& track.structural_role == shape.structural_role
							&& project.aliases.learned_aliases_for_track(track) == shape.aliases;
					});

				if (present)
				{
					++projects_containing_shape[shape_index];
				}
			}
		}

		CHECK(empty_project_count == 1);
		CHECK(largest_track_count == exhaustive_maximum_tracks);

		for (const std::size_t count : projects_containing_shape)
		{
			CHECK(count > 0);
		}
	}

	SECTION("the sampled corpus spans its track range and produces wide ambiguities")
	{
		const std::vector<GeneratedProject>& projects = sampled_projects();

		REQUIRE(projects.size() == sampled_project_count);

		std::size_t smallest_track_count = sampled_maximum_tracks;
		std::size_t largest_track_count = 0;
		std::size_t projects_with_a_roleless_track = 0;
		std::size_t projects_with_an_alias = 0;
		std::size_t widest_ambiguity = 0;

		for (const GeneratedProject& project : projects)
		{
			smallest_track_count = std::min(smallest_track_count, project.tracks.size());
			largest_track_count = std::max(largest_track_count, project.tracks.size());

			bool has_roleless_track = false;
			bool has_alias = false;

			for (const ResolvableTrack& track : project.tracks)
			{
				has_roleless_track = has_roleless_track || !track.structural_role.has_value();
				has_alias = has_alias
					|| !project.aliases.learned_aliases_for_track(track).empty();
			}

			projects_with_a_roleless_track += has_roleless_track ? 1 : 0;
			projects_with_an_alias += has_alias ? 1 : 0;

			for (const std::string& pattern : patterns())
			{
				const TrackResolution resolution =
					resolve_track_by_name_pattern(project.tracks, project.aliases, pattern);

				if (is_ambiguous(resolution))
				{
					widest_ambiguity = std::max(
						widest_ambiguity,
						std::get<AmbiguousTrackSelector>(resolution).candidates.size());
				}
			}
		}

		CHECK(smallest_track_count == sampled_minimum_tracks);
		CHECK(largest_track_count == sampled_maximum_tracks);
		CHECK(projects_with_a_roleless_track > 0);
		CHECK(projects_with_an_alias > 0);

		// Wider than the exhaustive alphabet can reach, which is why this corpus
		// exists — properties 6 and 7 are claims about arbitrary candidate counts.
		// Seven at this seed, against a ceiling of four over the shape alphabet. The
		// guard is the measured value rather than a round number: a generator change
		// that narrowed the widest ambiguity should be noticed here.
		CHECK(widest_ambiguity >= 7);
	}
}

// ---------------------------------------------------------------------------
// Property 4: Name pattern resolution — source precedence holds
//
// For any track set and any pattern, resolution consults track name, then learned
// alias, then structural role, and returns the first non-empty candidate set — a
// later source never overrides an earlier one that matched.
//
// **Validates: Requirements 7.2**
//
// Checked against `collect_candidates_from_source` rather than against a restatement
// of the rule: what each source offers is computed independently, and the ladder's
// answer has to be the first non-empty one, candidate for candidate, with the same
// source and the same match tier.
// ---------------------------------------------------------------------------

TEST_CASE("Property 4: the ladder's answer is the first non-empty source's answer",
	"[daw][object_resolver][property]")
{
	struct sweep_outcome
	{
		std::string first_counterexample;
		std::size_t case_count = 0;
		std::size_t wins_by_track_name = 0;
		std::size_t wins_by_learned_alias = 0;
		std::size_t wins_by_structural_role = 0;
		std::size_t several_sources_matched = 0;
		std::size_t all_three_sources_matched = 0;
		std::size_t a_later_source_named_a_different_track = 0;
	};

	const auto sweep = [](const std::vector<GeneratedProject>& projects) {
		sweep_outcome outcome;

		for (const GeneratedProject& project : projects)
		{
			for (const std::string& pattern : patterns())
			{
				const ResolutionCase evaluated = evaluate(project, pattern);
				++outcome.case_count;

				const std::optional<std::size_t> winning_position =
					evaluated.first_non_empty_source_position();

				if (!winning_position.has_value())
				{
					// No source offered anything, so the ladder has nothing to prefer
					// and the outcome is the refusal property 5 is about.
					if (!is_unresolved(evaluated.resolution) && outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | no source matched, yet the outcome was "
							+ describe_outcome(evaluated.resolution);
					}

					continue;
				}

				const SourceCandidates& winning = evaluated.by_precedence_order[*winning_position];

				switch (winning.source)
				{
					case ResolutionSource::track_name:
						++outcome.wins_by_track_name;
						break;
					case ResolutionSource::learned_alias:
						++outcome.wins_by_learned_alias;
						break;
					case ResolutionSource::structural_role:
						++outcome.wins_by_structural_role;
						break;
					case ResolutionSource::guid:
						break;
				}

				const std::size_t non_empty_count = evaluated.non_empty_source_count();

				if (non_empty_count >= 2)
				{
					++outcome.several_sources_matched;
				}

				if (non_empty_count == source_count)
				{
					++outcome.all_three_sources_matched;
				}

				// Precedence is only observable when a later source would have given a
				// different answer. Counted so the property cannot pass on a corpus
				// where every pattern was answered by one source.
				for (std::size_t later = *winning_position + 1; later < source_count; ++later)
				{
					const SourceCandidates& later_source = evaluated.by_precedence_order[later];

					if (!later_source.candidates.empty()
						&& guids_of(later_source.candidates) != guids_of(winning.candidates))
					{
						++outcome.a_later_source_named_a_different_track;
						break;
					}
				}

				if (winning.candidates.size() == 1)
				{
					if (!is_resolved(evaluated.resolution))
					{
						if (outcome.first_counterexample.empty())
						{
							outcome.first_counterexample = describe_case(project, evaluated)
								+ " | one candidate from "
								+ std::string{describe_resolution_source(winning.source)}
								+ ", yet the outcome was " + describe_outcome(evaluated.resolution);
						}

						continue;
					}

					if (!same_track(std::get<ResolvedTrack>(evaluated.resolution), winning.candidates.front())
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | resolved to a different track than the winning source offered: "
							+ describe_outcome(evaluated.resolution);
					}

					continue;
				}

				if (!is_ambiguous(evaluated.resolution))
				{
					if (outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | " + std::to_string(winning.candidates.size()) + " candidates from "
							+ std::string{describe_resolution_source(winning.source)}
							+ ", yet the outcome was " + describe_outcome(evaluated.resolution);
					}

					continue;
				}

				const AmbiguousTrackSelector& ambiguous =
					std::get<AmbiguousTrackSelector>(evaluated.resolution);

				if ((ambiguous.source != winning.source
						|| ambiguous.precision != winning.precision
						|| guids_of(ambiguous.candidates) != guids_of(winning.candidates))
					&& outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_case(project, evaluated)
						+ " | the ambiguity does not match the winning source: "
						+ describe_outcome(evaluated.resolution);
				}
			}
		}

		return outcome;
	};

	SECTION("over every project of up to four tracks, for every pattern")
	{
		const sweep_outcome outcome = sweep(exhaustive_projects());

		REQUIRE(outcome.first_counterexample == std::string{});
		CHECK(outcome.case_count == 112344);

		// All three sources win somewhere, so none of the three rungs is unreached.
		CHECK(outcome.wins_by_track_name > 0);
		CHECK(outcome.wins_by_learned_alias > 0);
		CHECK(outcome.wins_by_structural_role > 0);

		// And precedence is observable rather than incidental: many cases have two or
		// three sources with something to say, and in many of those a later source
		// would have named a different track.
		CHECK(outcome.several_sources_matched > 0);
		CHECK(outcome.all_three_sources_matched > 0);
		CHECK(outcome.a_later_source_named_a_different_track > 0);
	}

	SECTION("over sampled projects of up to twelve tracks")
	{
		// Reproducible from the seed: 0x0b1ec75.
		const sweep_outcome outcome = sweep(sampled_projects());

		REQUIRE(outcome.first_counterexample == std::string{});
		CHECK(outcome.case_count == sampled_project_count * patterns().size());

		CHECK(outcome.wins_by_track_name > 0);
		CHECK(outcome.wins_by_learned_alias > 0);
		CHECK(outcome.wins_by_structural_role > 0);
		CHECK(outcome.several_sources_matched > 0);
		CHECK(outcome.all_three_sources_matched > 0);
		CHECK(outcome.a_later_source_named_a_different_track > 0);
	}
}

// ---------------------------------------------------------------------------
// Property 4, the sharp sub-claim
//
// Requirement 7.2 implies it and the header states it: an earlier source yielding two
// candidates is ambiguous, and does not fall through to a later source that would
// have yielded one. This is the version of precedence that costs something — falling
// through would look like helpfulness and would answer a question about names with a
// fact about aliases.
//
// **Validates: Requirements 7.2, 7.4**
// ---------------------------------------------------------------------------

TEST_CASE("Property 4: an ambiguous earlier source never falls through to a later single hit",
	"[daw][object_resolver][property]")
{
	struct sweep_outcome
	{
		std::string first_counterexample;
		std::size_t name_ambiguous_with_alias_single = 0;
		std::size_t name_ambiguous_with_role_single = 0;
		std::size_t alias_ambiguous_with_role_single = 0;
	};

	const auto sweep = [](const std::vector<GeneratedProject>& projects) {
		sweep_outcome outcome;

		for (const GeneratedProject& project : projects)
		{
			for (const std::string& pattern : patterns())
			{
				const ResolutionCase evaluated = evaluate(project, pattern);

				const std::optional<std::size_t> winning_position =
					evaluated.first_non_empty_source_position();

				if (!winning_position.has_value())
				{
					continue;
				}

				const SourceCandidates& winning = evaluated.by_precedence_order[*winning_position];

				if (winning.candidates.size() < 2)
				{
					continue;
				}

				// Only the cases where falling through would have produced a clean
				// single answer — the temptation the rule exists to refuse.
				bool a_later_source_would_have_resolved = false;

				for (std::size_t later = *winning_position + 1; later < source_count; ++later)
				{
					if (evaluated.by_precedence_order[later].candidates.size() != 1)
					{
						continue;
					}

					a_later_source_would_have_resolved = true;

					if (*winning_position == 0 && later == 1)
					{
						++outcome.name_ambiguous_with_alias_single;
					}
					else if (*winning_position == 0 && later == 2)
					{
						++outcome.name_ambiguous_with_role_single;
					}
					else if (*winning_position == 1 && later == 2)
					{
						++outcome.alias_ambiguous_with_role_single;
					}
				}

				if (!a_later_source_would_have_resolved)
				{
					continue;
				}

				if (!is_ambiguous(evaluated.resolution))
				{
					if (outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | the winning source was ambiguous and a later source had a single"
							" candidate, and the outcome fell through to "
							+ describe_outcome(evaluated.resolution);
					}

					continue;
				}

				const AmbiguousTrackSelector& ambiguous =
					std::get<AmbiguousTrackSelector>(evaluated.resolution);

				if ((ambiguous.source != winning.source
						|| guids_of(ambiguous.candidates) != guids_of(winning.candidates))
					&& outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_case(project, evaluated)
						+ " | the ambiguity was reported against the wrong source: "
						+ describe_outcome(evaluated.resolution);
				}
			}
		}

		return outcome;
	};

	SECTION("over every project of up to four tracks, for every pattern")
	{
		const sweep_outcome outcome = sweep(exhaustive_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		// All three pairings are reached, so the claim is checked at each rung of the
		// ladder rather than only at the first.
		CHECK(outcome.name_ambiguous_with_alias_single > 0);
		CHECK(outcome.name_ambiguous_with_role_single > 0);
		CHECK(outcome.alias_ambiguous_with_role_single > 0);
	}

	SECTION("over sampled projects of up to twelve tracks")
	{
		const sweep_outcome outcome = sweep(sampled_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.name_ambiguous_with_alias_single > 0);
		CHECK(outcome.name_ambiguous_with_role_single > 0);
		CHECK(outcome.alias_ambiguous_with_role_single > 0);
	}
}

// ---------------------------------------------------------------------------
// Property 5: Name pattern resolution — no match never guesses
//
// For any pattern matching no candidate at any source, the outcome is
// `unresolved_track_selector`, never a guess.
//
// **Validates: Requirements 7.3**
//
// The corpus for this one has to contain more than nonsense strings, or it proves
// only that the resolver cannot match "voks". So the near-misses are counted: an
// empty or whitespace pattern, a track name sitting inside the pattern (reverse
// containment, excluded on purpose), an alias sitting inside the pattern, and a
// pattern sitting inside a role phrase some track carries ("bus" inside "master
// bus"). Each is a case a looser implementation would have resolved.
// ---------------------------------------------------------------------------

TEST_CASE("Property 5: a pattern matching nothing anywhere refuses rather than guessing",
	"[daw][object_resolver][property]")
{
	struct sweep_outcome
	{
		std::string first_counterexample;
		std::size_t no_match_case_count = 0;
		std::size_t empty_after_normalization = 0;
		std::size_t a_track_name_inside_the_pattern = 0;
		std::size_t an_alias_inside_the_pattern = 0;
		std::size_t the_pattern_inside_a_present_role_phrase = 0;
		std::size_t with_tracks_present = 0;
	};

	const auto sweep = [](const std::vector<GeneratedProject>& projects) {
		sweep_outcome outcome;

		for (const GeneratedProject& project : projects)
		{
			for (const std::string& pattern : patterns())
			{
				const ResolutionCase evaluated = evaluate(project, pattern);

				if (evaluated.first_non_empty_source_position().has_value())
				{
					continue;
				}

				++outcome.no_match_case_count;

				if (evaluated.normalized_pattern.empty())
				{
					++outcome.empty_after_normalization;
				}

				if (a_track_name_sits_inside_the_pattern(project, evaluated.normalized_pattern))
				{
					++outcome.a_track_name_inside_the_pattern;
				}

				if (an_alias_sits_inside_the_pattern(project, evaluated.normalized_pattern))
				{
					++outcome.an_alias_inside_the_pattern;
				}

				if (the_pattern_sits_inside_a_present_role_phrase(project, evaluated.normalized_pattern))
				{
					++outcome.the_pattern_inside_a_present_role_phrase;
				}

				if (!project.tracks.empty())
				{
					++outcome.with_tracks_present;
				}

				if (!is_unresolved(evaluated.resolution))
				{
					if (outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | nothing matched at any source, yet the outcome was "
							+ describe_outcome(evaluated.resolution);
					}

					continue;
				}

				// The refusal the contract names, whatever the operation's risk. An
				// unresolved selector is not something acknowledging gets past, so
				// there is no acknowledgement field and nothing is blocking.
				for (const OperationRisk risk : {OperationRisk::safe, OperationRisk::destructive})
				{
					const TrackResolutionDisposition disposition =
						dispose_of_track_resolution(evaluated.resolution, risk);

					if ((disposition.proceed
							|| disposition.resolved_track.has_value()
							|| !disposition.refusal.has_value()
							|| disposition.refusal->reason != "unresolved_track_selector"
							|| !disposition.refusal->blocking.empty()
							|| !disposition.refusal->acknowledgement_field.empty())
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | the refusal was not a bare unresolved_track_selector: "
							+ disposition.report;
					}
				}
			}
		}

		return outcome;
	};

	SECTION("over every project of up to four tracks, for every pattern")
	{
		const sweep_outcome outcome = sweep(exhaustive_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		// The no-match corpus is substantial, and it is not made only of empty
		// patterns against empty projects.
		CHECK(outcome.no_match_case_count > 0);
		CHECK(outcome.with_tracks_present > 0);
		CHECK(outcome.empty_after_normalization > 0);

		// The near-misses. Each of these would have resolved under a rule the
		// implementation rejects on purpose, so without them this property would hold
		// against a resolver that guessed generously.
		CHECK(outcome.a_track_name_inside_the_pattern > 0);
		CHECK(outcome.an_alias_inside_the_pattern > 0);
		CHECK(outcome.the_pattern_inside_a_present_role_phrase > 0);
	}

	SECTION("over sampled projects of up to twelve tracks")
	{
		const sweep_outcome outcome = sweep(sampled_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.no_match_case_count > 0);
		CHECK(outcome.with_tracks_present > 0);
		CHECK(outcome.a_track_name_inside_the_pattern > 0);
		CHECK(outcome.an_alias_inside_the_pattern > 0);
		CHECK(outcome.the_pattern_inside_a_present_role_phrase > 0);
	}
}

// ---------------------------------------------------------------------------
// Property 6: Name pattern resolution — ambiguity names every candidate
//
// For any pattern whose winning source yields more than one candidate, the outcome is
// `ambiguous_track_selector` and every candidate is named with its GUID attached.
//
// **Validates: Requirements 7.4**
//
// Two claims, deliberately separated because they are not the same one:
//
//   - The OUTCOME carries every candidate. No cap, no truncation, no deduplication
//     beyond the one-per-track rule the collector applies.
//   - The REFUSAL carries as many as the schema's `maxItems` of 512 allows, and
//     accounts for the rest rather than dropping them: 511 listed plus a count of
//     what did not fit. A truncated list that looked complete would have the agent
//     believe it had seen every candidate, which is requirement 7.4 quietly broken.
//
// The first is swept over the generated corpora; the second over candidate counts
// either side of the cap, which the corpora do not reach.
// ---------------------------------------------------------------------------

TEST_CASE("Property 6: an ambiguous outcome names every candidate with its guid",
	"[daw][object_resolver][property]")
{
	struct sweep_outcome
	{
		std::string first_counterexample;
		std::size_t ambiguity_count = 0;
		std::size_t ambiguities_by_track_name = 0;
		std::size_t ambiguities_by_learned_alias = 0;
		std::size_t ambiguities_by_structural_role = 0;
		std::size_t imperfect_tier_ambiguities = 0;
		std::size_t widest_ambiguity = 0;
		std::size_t distinct_candidate_counts_seen = 0;
	};

	const auto sweep = [](const std::vector<GeneratedProject>& projects) {
		sweep_outcome outcome;
		std::unordered_set<std::size_t> candidate_counts_seen;

		for (const GeneratedProject& project : projects)
		{
			// The name each GUID should be reported under. Requirement 7.4 is that
			// every candidate is *named* with its GUID, so both halves are checked.
			std::unordered_map<std::string, std::string> name_by_guid;

			for (const ResolvableTrack& track : project.tracks)
			{
				name_by_guid[track.guid] = track.name;
			}

			for (const std::string& pattern : patterns())
			{
				const ResolutionCase evaluated = evaluate(project, pattern);

				const std::optional<std::size_t> winning_position =
					evaluated.first_non_empty_source_position();

				if (!winning_position.has_value()
					|| evaluated.by_precedence_order[*winning_position].candidates.size() < 2)
				{
					continue;
				}

				const SourceCandidates& winning = evaluated.by_precedence_order[*winning_position];

				if (!is_ambiguous(evaluated.resolution))
				{
					if (outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | the winning source had several candidates, yet the outcome was "
							+ describe_outcome(evaluated.resolution);
					}

					continue;
				}

				const AmbiguousTrackSelector& ambiguous =
					std::get<AmbiguousTrackSelector>(evaluated.resolution);

				++outcome.ambiguity_count;
				candidate_counts_seen.insert(ambiguous.candidates.size());
				outcome.widest_ambiguity =
					std::max(outcome.widest_ambiguity, ambiguous.candidates.size());

				switch (ambiguous.source)
				{
					case ResolutionSource::track_name:
						++outcome.ambiguities_by_track_name;
						break;
					case ResolutionSource::learned_alias:
						++outcome.ambiguities_by_learned_alias;
						break;
					case ResolutionSource::structural_role:
						++outcome.ambiguities_by_structural_role;
						break;
					case ResolutionSource::guid:
						break;
				}

				if (ambiguous.precision == MatchPrecision::imperfect)
				{
					++outcome.imperfect_tier_ambiguities;
				}

				// Every candidate the winning source offered is on the outcome, in the
				// order the snapshot listed them, each with a non-empty GUID and the
				// name the snapshot gave that GUID.
				if (guids_of(ambiguous.candidates) != guids_of(winning.candidates)
					&& outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_case(project, evaluated)
						+ " | the candidates are not the ones the winning source offered: "
						+ describe_outcome(evaluated.resolution);
				}

				std::unordered_set<std::string> guids_on_the_outcome;

				for (const ResolvedTrack& candidate : ambiguous.candidates)
				{
					if (candidate.reference.guid.empty() && outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | a candidate carried no guid";
					}

					if (!guids_on_the_outcome.insert(candidate.reference.guid).second
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | the same track was listed twice: " + candidate.reference.guid;
					}

					const auto expected_name = name_by_guid.find(candidate.reference.guid);

					if ((expected_name == name_by_guid.end()
							|| expected_name->second != candidate.reference.name)
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | candidate " + candidate.reference.guid
							+ " was named \"" + candidate.reference.name + "\"";
					}
				}

				// And the refusal that carries them to the agent. Below the cap it
				// lists every one; the cap itself is swept separately.
				const TrackResolutionDisposition disposition =
					dispose_of_track_resolution(evaluated.resolution, OperationRisk::safe);

				if ((disposition.proceed
						|| !disposition.refusal.has_value()
						|| disposition.refusal->reason != "ambiguous_track_selector"
						|| disposition.refusal->acknowledgement_field != "confirmedTrackSelection")
					&& outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_case(project, evaluated)
						+ " | the refusal was not an ambiguous_track_selector: " + disposition.report;

					continue;
				}

				const std::vector<BlockingEntity>& blocking = disposition.refusal->blocking;

				if (blocking.size() != ambiguous.candidates.size()
					&& outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_case(project, evaluated)
						+ " | " + std::to_string(ambiguous.candidates.size())
						+ " candidates became " + std::to_string(blocking.size())
						+ " blocking entities";

					continue;
				}

				for (std::size_t index = 0; index < blocking.size(); ++index)
				{
					const BlockingEntity& entity = blocking[index];

					if ((entity.kind != "track"
							|| entity.description.empty()
							|| entity.guid != ambiguous.candidates[index].reference.guid)
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_case(project, evaluated)
							+ " | blocking entity " + std::to_string(index)
							+ " is kind \"" + entity.kind
							+ "\", description \"" + entity.description
							+ "\", guid \"" + entity.guid + "\"";
					}
				}
			}
		}

		outcome.distinct_candidate_counts_seen = candidate_counts_seen.size();

		return outcome;
	};

	SECTION("over every project of up to four tracks, for every pattern")
	{
		const sweep_outcome outcome = sweep(exhaustive_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.ambiguity_count > 0);

		// Every source can be the ambiguous one, and ambiguity is not only an exact
		// tier phenomenon — two tracks both matching by containment is an ambiguity
		// the imperfect tier has to report rather than pick from.
		CHECK(outcome.ambiguities_by_track_name > 0);
		CHECK(outcome.ambiguities_by_learned_alias > 0);
		CHECK(outcome.ambiguities_by_structural_role > 0);
		CHECK(outcome.imperfect_tier_ambiguities > 0);

		// Candidate counts from two up to the whole project, so "every candidate" is
		// checked at more than one width.
		CHECK(outcome.widest_ambiguity == exhaustive_maximum_tracks);
		CHECK(outcome.distinct_candidate_counts_seen == 3);
	}

	SECTION("over sampled projects of up to twelve tracks")
	{
		const sweep_outcome outcome = sweep(sampled_projects());

		REQUIRE(outcome.first_counterexample == std::string{});

		CHECK(outcome.ambiguity_count > 0);
		CHECK(outcome.ambiguities_by_track_name > 0);
		CHECK(outcome.ambiguities_by_learned_alias > 0);
		CHECK(outcome.ambiguities_by_structural_role > 0);
		CHECK(outcome.imperfect_tier_ambiguities > 0);

		// Wider ambiguities than the exhaustive alphabet can build, at more widths.
		// Measured at this seed: widest seven, and every width from two to seven seen.
		CHECK(outcome.widest_ambiguity >= 7);
		CHECK(outcome.distinct_candidate_counts_seen >= 6);
	}
}

TEST_CASE("Property 6: the refusal caps the list at the schema's maximum and counts the remainder",
	"[daw][object_resolver][property]")
{
	// Candidate counts either side of `blocking`'s `maxItems`. The generated corpora
	// cannot reach the cap, and the cap is where requirement 7.4 and the schema pull
	// in different directions — so it is swept here rather than assumed from the one
	// worked example.
	const std::vector<std::size_t> candidate_counts{
		2, 3, 5, 17, 64, 510,
		maximum_blocking_entities - 1,
		maximum_blocking_entities,
		maximum_blocking_entities + 1,
		maximum_blocking_entities + 2,
		900
	};

	std::string first_counterexample;
	std::size_t counts_below_the_cap = 0;
	std::size_t counts_above_the_cap = 0;

	for (const std::size_t candidate_count : candidate_counts)
	{
		GeneratedProject project;
		project.tracks.reserve(candidate_count);

		for (std::size_t position = 0; position < candidate_count; ++position)
		{
			ResolvableTrack track;
			track.guid = guid_for(position);
			track.name = "Take";
			track.project_index = static_cast<int>(position);
			project.tracks.push_back(std::move(track));
		}

		const TrackResolution resolution =
			resolve_track_by_name_pattern(project.tracks, project.aliases, "take");

		if (!is_ambiguous(resolution))
		{
			if (first_counterexample.empty())
			{
				first_counterexample = std::to_string(candidate_count)
					+ " identically named tracks did not produce an ambiguity: "
					+ describe_outcome(resolution);
			}

			continue;
		}

		const AmbiguousTrackSelector& ambiguous = std::get<AmbiguousTrackSelector>(resolution);

		// The claim about the outcome: uncapped, every candidate present.
		if (ambiguous.candidates.size() != candidate_count && first_counterexample.empty())
		{
			first_counterexample = std::to_string(candidate_count)
				+ " candidates became " + std::to_string(ambiguous.candidates.size())
				+ " on the outcome";

			continue;
		}

		const ToolResultRefusal refusal =
			sesh_ai::daw::build_ambiguous_track_selector_refusal(ambiguous);

		const bool above_the_cap = candidate_count > maximum_blocking_entities;
		const std::size_t expected_listed =
			above_the_cap ? maximum_blocking_entities - 1 : candidate_count;
		const std::size_t expected_blocking =
			above_the_cap ? maximum_blocking_entities : candidate_count;

		if (above_the_cap)
		{
			++counts_above_the_cap;
		}
		else
		{
			++counts_below_the_cap;
		}

		if (refusal.blocking.size() != expected_blocking && first_counterexample.empty())
		{
			first_counterexample = std::to_string(candidate_count) + " candidates produced "
				+ std::to_string(refusal.blocking.size()) + " blocking entities, expected "
				+ std::to_string(expected_blocking);

			continue;
		}

		// Every listed slot names a real candidate with its GUID, in order.
		for (std::size_t index = 0; index < expected_listed; ++index)
		{
			const BlockingEntity& entity = refusal.blocking[index];

			if ((entity.kind != "track"
					|| entity.description.empty()
					|| entity.guid != ambiguous.candidates[index].reference.guid)
				&& first_counterexample.empty())
			{
				first_counterexample = std::to_string(candidate_count) + " candidates: entity "
					+ std::to_string(index) + " is kind \"" + entity.kind
					+ "\", description \"" + entity.description + "\", guid \"" + entity.guid + "\"";
			}
		}

		if (!above_the_cap)
		{
			continue;
		}

		// And the overflow slot accounts for the rest rather than the list simply
		// ending: listed plus counted equals every candidate there was.
		const BlockingEntity& overflow = refusal.blocking.back();
		const std::string expected_description =
			std::to_string(candidate_count - expected_listed) + " further tracks match and are not listed";

		if ((!overflow.guid.empty()
				|| overflow.kind != "track"
				|| overflow.description != expected_description)
			&& first_counterexample.empty())
		{
			first_counterexample = std::to_string(candidate_count)
				+ " candidates: the overflow slot said \"" + overflow.description
				+ "\" with guid \"" + overflow.guid + "\", expected \"" + expected_description + "\"";
		}
	}

	REQUIRE(first_counterexample == std::string{});

	// Both sides of the cap were exercised, including the boundary itself.
	CHECK(counts_below_the_cap == 8);
	CHECK(counts_above_the_cap == 3);
}

// ---------------------------------------------------------------------------
// Property 7: Name pattern resolution — exactly three possible outcomes
//
// For any pattern and any track set, the outcome is exactly one of: a single resolved
// track, `unresolved_track_selector`, or `ambiguous_track_selector`.
//
// **Validates: Requirements 7.5**
//
// Half of this is structural and is asserted at the top of this file rather than
// swept for: `TrackResolution` is a `std::variant` of three alternatives, so a fourth
// outcome is not something the codebase can express and no corpus could find one.
//
// What is left is the half with content. The three predicates the rest of the
// extension reads outcomes through have to *partition* the space — exactly one true,
// never two, never none — and an ambiguous outcome has to carry at least two
// candidates, since one would have resolved and zero would be the other refusal
// wearing the wrong reason.
// ---------------------------------------------------------------------------

TEST_CASE("Property 7: the three outcome predicates partition every resolution",
	"[daw][object_resolver][property]")
{
	struct sweep_outcome
	{
		std::string first_counterexample;
		std::size_t case_count = 0;
		std::size_t resolved_count = 0;
		std::size_t unresolved_count = 0;
		std::size_t ambiguous_count = 0;
	};

	const auto sweep = [](const std::vector<GeneratedProject>& projects) {
		sweep_outcome outcome;

		for (const GeneratedProject& project : projects)
		{
			for (const std::string& pattern : patterns())
			{
				const TrackResolution resolution =
					resolve_track_by_name_pattern(project.tracks, project.aliases, pattern);

				++outcome.case_count;

				const int true_predicate_count = static_cast<int>(is_resolved(resolution))
					+ static_cast<int>(is_unresolved(resolution))
					+ static_cast<int>(is_ambiguous(resolution));

				if (true_predicate_count != 1 && outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_project(project)
						+ " | pattern \"" + pattern + "\" satisfied "
						+ std::to_string(true_predicate_count) + " of the three predicates";

					continue;
				}

				if (is_resolved(resolution))
				{
					++outcome.resolved_count;

					// A resolved track the agent cannot address again is not an
					// outcome it can act on.
					if (std::get<ResolvedTrack>(resolution).reference.guid.empty()
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_project(project)
							+ " | pattern \"" + pattern + "\" resolved to a track with no guid";
					}

					continue;
				}

				if (is_unresolved(resolution))
				{
					++outcome.unresolved_count;

					// The selector is carried so a log line or a message can name what
					// was asked for.
					if (std::get<UnresolvedSelector>(resolution).selector_description.empty()
						&& outcome.first_counterexample.empty())
					{
						outcome.first_counterexample = describe_project(project)
							+ " | pattern \"" + pattern + "\" refused without describing the selector";
					}

					continue;
				}

				++outcome.ambiguous_count;

				const AmbiguousTrackSelector& ambiguous =
					std::get<AmbiguousTrackSelector>(resolution);

				if (ambiguous.candidates.size() < 2 && outcome.first_counterexample.empty())
				{
					outcome.first_counterexample = describe_project(project)
						+ " | pattern \"" + pattern + "\" was ambiguous over "
						+ std::to_string(ambiguous.candidates.size()) + " candidates";
				}
			}
		}

		return outcome;
	};

	SECTION("over every project of up to four tracks, for every pattern")
	{
		const sweep_outcome outcome = sweep(exhaustive_projects());

		REQUIRE(outcome.first_counterexample == std::string{});
		CHECK(outcome.case_count == 112344);

		// All three outcomes occur, so the partition is checked on each of them rather
		// than on a corpus that only ever refused.
		CHECK(outcome.resolved_count > 0);
		CHECK(outcome.unresolved_count > 0);
		CHECK(outcome.ambiguous_count > 0);
		CHECK(outcome.resolved_count + outcome.unresolved_count + outcome.ambiguous_count
			== outcome.case_count);
	}

	SECTION("over sampled projects of up to twelve tracks")
	{
		const sweep_outcome outcome = sweep(sampled_projects());

		REQUIRE(outcome.first_counterexample == std::string{});
		CHECK(outcome.case_count == sampled_project_count * patterns().size());

		CHECK(outcome.resolved_count > 0);
		CHECK(outcome.unresolved_count > 0);
		CHECK(outcome.ambiguous_count > 0);
		CHECK(outcome.resolved_count + outcome.unresolved_count + outcome.ambiguous_count
			== outcome.case_count);
	}
}
