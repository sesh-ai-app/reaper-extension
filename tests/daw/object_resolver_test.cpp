// Object resolution.
//
// The component's job is to refuse rather than guess, so most of this file is about
// the two refusals and the exact shape of the precedence rule that produces them.
// Three things get the most attention, because they are the three that are easy to
// get subtly wrong and impossible to notice afterwards:
//
//   - A later source never overrides an earlier match (requirement 7.2).
//   - An earlier source matching two candidates is ambiguous and does not fall
//     through to a later source that would have matched one (requirement 7.4).
//   - Whatever the pattern and whatever the track set, there are exactly three
//     outcomes (requirement 7.5).
//
// Task 5.2 adds the formal property tests over generated inputs. These are the
// worked examples underneath them.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/structural_role.h>
#include <daw/object_resolver.h>

using sesh_ai::context::StructuralRole;
using sesh_ai::daw::AmbiguousTrackSelector;
using sesh_ai::daw::LearnedAliasLookup;
using sesh_ai::daw::MarkerOrRegionResolution;
using sesh_ai::daw::MatchPrecision;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::OperationRisk;
using sesh_ai::daw::ResolutionSource;
using sesh_ai::daw::ResolvableMarkerOrRegion;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::ResolvedMarkerOrRegion;
using sesh_ai::daw::ResolvedTrack;
using sesh_ai::daw::TrackGuidSelector;
using sesh_ai::daw::TrackNamePatternSelector;
using sesh_ai::daw::TrackResolution;
using sesh_ai::daw::TrackSelector;
using sesh_ai::daw::UnresolvedSelector;

namespace
{

	// Stands in for the Alias Store. The seam takes the whole `ResolvableTrack`, so a
	// substitute is free to key on whichever of its fields it actually has: the real
	// store keys on the `MediaTrack*`, and a synthetic track list has GUIDs and no
	// handles, so this keys on the GUID.
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

	// REAPER's braced GUID form, from an ordinal, so a test reads as "track 3" rather
	// than as a wall of hex. Matches the schemas' pattern: braces around 36
	// hex-and-hyphen characters.
	std::string guid_for(int ordinal)
	{
		std::string digits = std::to_string(ordinal);

		while (digits.size() < 12)
		{
			digits.insert(digits.begin(), '0');
		}

		return "{00000000-0000-0000-0000-" + digits + "}";
	}

	ResolvableTrack track_named(
		int project_index,
		std::string name,
		std::optional<StructuralRole> structural_role = std::nullopt)
	{
		ResolvableTrack track;
		track.guid = guid_for(project_index);
		track.name = std::move(name);
		track.project_index = project_index;
		track.structural_role = structural_role;

		return track;
	}

	ResolvableMarkerOrRegion marker_named(int ordinal, std::string name, double position_seconds)
	{
		ResolvableMarkerOrRegion marker;
		marker.guid = guid_for(ordinal);
		marker.name = std::move(name);
		marker.is_region = false;
		marker.start_seconds = position_seconds;
		marker.end_seconds = position_seconds;
		marker.internal_index = ordinal;
		marker.displayed_number = ordinal;

		return marker;
	}

	ResolvableMarkerOrRegion region_named(
		int ordinal,
		std::string name,
		double start_seconds,
		double end_seconds)
	{
		ResolvableMarkerOrRegion region = marker_named(ordinal, std::move(name), start_seconds);
		region.is_region = true;
		region.end_seconds = end_seconds;

		return region;
	}

}

// ---------------------------------------------------------------------------
// GUID resolution (requirement 7.1)
// ---------------------------------------------------------------------------

TEST_CASE("a guid resolves by walking the track list and comparing", "[object_resolver][guid]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick"),
		track_named(1, "Snare"),
		track_named(2, "Bass")
	};

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_guid(tracks, guid_for(1));

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Snare");
	CHECK(resolved.reference.guid == guid_for(1));
	CHECK(resolved.project_index == 1);
	CHECK(resolved.source == ResolutionSource::guid);
	CHECK(resolved.precision == MatchPrecision::exact);
}

TEST_CASE("a guid that names no track is unresolved rather than nearest", "[object_resolver][guid]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick"),
		track_named(1, "Snare")
	};

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_guid(tracks, guid_for(9));

	REQUIRE(sesh_ai::daw::is_unresolved(resolution));
}

TEST_CASE("a guid is compared case-insensitively but not loosely", "[object_resolver][guid]")
{
	std::vector<ResolvableTrack> tracks{track_named(0, "Kick")};
	tracks[0].guid = "{ABCDEF01-2345-6789-ABCD-EF0123456789}";

	SECTION("the same guid in the other case resolves")
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_guid(tracks, "{abcdef01-2345-6789-abcd-ef0123456789}");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
	}

	SECTION("a prefix of the guid does not resolve")
	{
		const TrackResolution resolution = sesh_ai::daw::resolve_track_by_guid(tracks, "{ABCDEF01");

		REQUIRE(sesh_ai::daw::is_unresolved(resolution));
	}
}

TEST_CASE("a duplicated guid is ambiguous rather than resolved to the first", "[object_resolver][guid]")
{
	std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick"),
		track_named(1, "Kick copy")
	};
	tracks[1].guid = tracks[0].guid;

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_guid(tracks, guid_for(0));

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));
	CHECK(std::get<AmbiguousTrackSelector>(resolution).candidates.size() == 2);
}

// ---------------------------------------------------------------------------
// Source precedence (requirement 7.2)
// ---------------------------------------------------------------------------

TEST_CASE("the precedence order is track name, then learned alias, then structural role",
	"[object_resolver][precedence]")
{
	const auto& order = sesh_ai::daw::name_pattern_sources_in_precedence_order;

	REQUIRE(order.size() == 3);
	CHECK(order[0] == ResolutionSource::track_name);
	CHECK(order[1] == ResolutionSource::learned_alias);
	CHECK(order[2] == ResolutionSource::structural_role);
}

TEST_CASE("a track name match wins over an alias pointing elsewhere", "[object_resolver][precedence]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Drum Bus"),
		track_named(1, "Guitars")
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(1), {"drum bus"});

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "drum bus");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Drum Bus");
	CHECK(resolved.source == ResolutionSource::track_name);
}

TEST_CASE("an alias match wins over a structural role that would also match",
	"[object_resolver][precedence]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Verb", StructuralRole::aux_return),
		track_named(1, "Delay", StructuralRole::aux_return)
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(0), {"aux return"});

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "aux return");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Verb");
	CHECK(resolved.source == ResolutionSource::learned_alias);
}

TEST_CASE("the structural role source is reached only when the first two match nothing",
	"[object_resolver][precedence]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Verb", StructuralRole::aux_return),
		track_named(1, "Kick", StructuralRole::normal)
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "aux");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Verb");
	CHECK(resolved.source == ResolutionSource::structural_role);
}

TEST_CASE("an ambiguous earlier source does not fall through to a later single hit",
	"[object_resolver][precedence]")
{
	// Two tracks named the same thing, and an alias on a third that would have
	// resolved cleanly. The name source wins because it matched, and it matched twice
	// — so this is a question for the producer, not an opportunity for the alias
	// store to answer a question about names.
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Bass"),
		track_named(1, "Bass"),
		track_named(2, "Sub")
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(2), {"bass"});

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "Bass");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const AmbiguousTrackSelector& ambiguous = std::get<AmbiguousTrackSelector>(resolution);

	CHECK(ambiguous.source == ResolutionSource::track_name);
	REQUIRE(ambiguous.candidates.size() == 2);
	CHECK(ambiguous.candidates[0].reference.guid == guid_for(0));
	CHECK(ambiguous.candidates[1].reference.guid == guid_for(1));
}

TEST_CASE("an ambiguous alias source does not fall through to the structural role source",
	"[object_resolver][precedence]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Verb", StructuralRole::normal),
		track_named(1, "Delay", StructuralRole::normal),
		track_named(2, "Plate", StructuralRole::aux_return)
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(0), {"aux return"});
	aliases.learn(guid_for(1), {"aux return"});

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "aux return");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));
	CHECK(std::get<AmbiguousTrackSelector>(resolution).source == ResolutionSource::learned_alias);
}

TEST_CASE("the ladder's answer is the first non-empty source's answer",
	"[object_resolver][precedence]")
{
	// Stated against the per-source collector rather than against a second copy of
	// the intent: whatever the sources individually produce, the ladder returns the
	// first non-empty one.
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Reverb Return", StructuralRole::aux_return),
		track_named(1, "Drums", StructuralRole::summing_folder_parent)
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(1), {"return"});

	const std::string pattern{"return"};
	const std::string normalized = sesh_ai::daw::normalize_for_matching(pattern);

	const sesh_ai::daw::SourceCandidates from_name = sesh_ai::daw::collect_candidates_from_source(
		tracks, aliases, normalized, ResolutionSource::track_name);
	const sesh_ai::daw::SourceCandidates from_alias = sesh_ai::daw::collect_candidates_from_source(
		tracks, aliases, normalized, ResolutionSource::learned_alias);
	const sesh_ai::daw::SourceCandidates from_role = sesh_ai::daw::collect_candidates_from_source(
		tracks, aliases, normalized, ResolutionSource::structural_role);

	// All three sources have something to say about "return" here, which is what
	// makes precedence observable.
	REQUIRE(from_name.candidates.size() == 1);
	REQUIRE(from_alias.candidates.size() == 1);
	REQUIRE(from_role.candidates.size() == 1);
	CHECK(from_name.candidates[0].reference.guid != from_alias.candidates[0].reference.guid);

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, pattern);

	REQUIRE(sesh_ai::daw::is_resolved(resolution));
	CHECK(std::get<ResolvedTrack>(resolution).reference.guid == from_name.candidates[0].reference.guid);
}

// ---------------------------------------------------------------------------
// Match tiers
// ---------------------------------------------------------------------------

TEST_CASE("matching folds case and separators", "[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Drum_Bus")};

	NoLearnedAliases aliases;

	for (const std::string pattern : {"drum bus", "DRUM-BUS", "  Drum   Bus  ", "drum_bus"})
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, pattern);

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
		CHECK(std::get<ResolvedTrack>(resolution).precision == MatchPrecision::exact);
	}
}

TEST_CASE("a leading definite article is dropped symmetrically", "[object_resolver][matching]")
{
	NoLearnedAliases aliases;

	SECTION("an articled pattern matches an unarticled name exactly")
	{
		const std::vector<ResolvableTrack> tracks{track_named(0, "Drum Bus")};

		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the drum bus");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
		CHECK(std::get<ResolvedTrack>(resolution).precision == MatchPrecision::exact);
	}

	SECTION("an articled name matches an articled pattern exactly")
	{
		const std::vector<ResolvableTrack> tracks{track_named(0, "The Bus")};

		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the bus");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
		CHECK(std::get<ResolvedTrack>(resolution).precision == MatchPrecision::exact);
	}

	SECTION("a pattern of nothing but the article matches nothing")
	{
		const std::vector<ResolvableTrack> tracks{
			track_named(0, "Kick"),
			track_named(1, "Snare")
		};

		CHECK(sesh_ai::daw::is_unresolved(
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the")));
		CHECK(sesh_ai::daw::is_unresolved(
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the ")));
	}

	SECTION("only the leading article goes")
	{
		CHECK(sesh_ai::daw::normalize_for_matching("The the") == "the");
		CHECK(sesh_ai::daw::normalize_for_matching("Bus the") == "bus the");
		CHECK(sesh_ai::daw::normalize_for_matching("Theremin") == "theremin");
	}
}

TEST_CASE("an exact match is preferred over a containment match in the same source",
	"[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Vox Double"),
		track_named(1, "Vox"),
		track_named(2, "Lead Vox")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "vox");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Vox");
	CHECK(resolved.precision == MatchPrecision::exact);
}

TEST_CASE("containment is the imperfect tier and is reported as such", "[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Lead Vox"),
		track_named(1, "Kick")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "vox");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.reference.name == "Lead Vox");
	CHECK(resolved.precision == MatchPrecision::imperfect);
}

TEST_CASE("two exact matches are ambiguous and do not fall through to containment",
	"[object_resolver][matching]")
{
	// Containment would match all three. The exact tier matched two, so the answer is
	// those two and the tier ladder stops there for the same reason the source ladder
	// does.
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Vox"),
		track_named(1, "vox"),
		track_named(2, "Lead Vox")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "VOX");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const AmbiguousTrackSelector& ambiguous = std::get<AmbiguousTrackSelector>(resolution);

	CHECK(ambiguous.precision == MatchPrecision::exact);
	CHECK(ambiguous.candidates.size() == 2);
}

TEST_CASE("containment runs pattern-inside-name and not the reverse", "[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Bus")};

	NoLearnedAliases aliases;

	SECTION("the pattern inside the name matches")
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "bu");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
	}

	SECTION("the name inside a longer pattern does not")
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the bus please");

		REQUIRE(sesh_ai::daw::is_unresolved(resolution));
	}
}

TEST_CASE("an unnamed track matches no pattern", "[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "")};

	NoLearnedAliases aliases;

	CHECK(sesh_ai::daw::is_unresolved(sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "kick")));
	CHECK(sesh_ai::daw::is_unresolved(sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "")));
}

TEST_CASE("a whitespace-only pattern matches nothing rather than everything",
	"[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick"),
		track_named(1, "Snare")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "   ");

	REQUIRE(sesh_ai::daw::is_unresolved(resolution));
}

TEST_CASE("a track matching a pattern through two aliases is listed once",
	"[object_resolver][matching]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Bus A")};

	RecordedAliases aliases;
	aliases.learn(guid_for(0), {"drum bus", "the drum bus thing"});

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "drum bus");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));
	CHECK(std::get<ResolvedTrack>(resolution).source == ResolutionSource::learned_alias);
}

// ---------------------------------------------------------------------------
// The structural role source
// ---------------------------------------------------------------------------

TEST_CASE("the schema spelling of every role is a phrase for it", "[object_resolver][role]")
{
	for (const StructuralRole role : sesh_ai::context::all_structural_roles)
	{
		const std::string normalized_schema_spelling =
			sesh_ai::daw::normalize_for_matching(sesh_ai::context::to_schema_string(role));

		const std::optional<StructuralRole> recognised =
			sesh_ai::daw::structural_role_from_producer_phrase(normalized_schema_spelling);

		REQUIRE(recognised.has_value());
		CHECK(*recognised == role);
	}
}

TEST_CASE("a role names a class of tracks, so several of them is ambiguous",
	"[object_resolver][role]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Drums", StructuralRole::summing_folder_parent),
		track_named(1, "Guitars", StructuralRole::summing_folder_parent),
		track_named(2, "Kick", StructuralRole::normal)
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "submix");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const AmbiguousTrackSelector& ambiguous = std::get<AmbiguousTrackSelector>(resolution);

	CHECK(ambiguous.source == ResolutionSource::structural_role);
	REQUIRE(ambiguous.candidates.size() == 2);
	CHECK(ambiguous.candidates[0].reference.name == "Drums");
	CHECK(ambiguous.candidates[1].reference.name == "Guitars");
}

TEST_CASE("a track with no derived role is invisible to the role source", "[object_resolver][role]")
{
	// A resolver running before the context builder has derived roles refuses rather
	// than answering from a role nobody computed.
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Drums"),
		track_named(1, "Guitars")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "master");

	REQUIRE(sesh_ai::daw::is_unresolved(resolution));
}

TEST_CASE("the role source has no containment tier", "[object_resolver][role]")
{
	// "bus" sits inside "master bus". Containment here would resolve a request for a
	// bus to the master track, which is the failure this component exists to prevent.
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Master", StructuralRole::master),
		track_named(1, "Drums", StructuralRole::summing_folder_parent)
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "bus");

	REQUIRE(sesh_ai::daw::is_unresolved(resolution));
}

TEST_CASE("the master track resolves from the producer's words for it", "[object_resolver][role]")
{
	std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick", StructuralRole::normal),
		track_named(1, "", StructuralRole::master)
	};
	tracks[1].is_master_track = true;
	tracks[1].project_index = -1;

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "the master");

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

	CHECK(resolved.is_master_track);
	CHECK(resolved.source == ResolutionSource::structural_role);
}

// ---------------------------------------------------------------------------
// The selector oneOf
// ---------------------------------------------------------------------------

TEST_CASE("resolving dispatches on which half of the selector arrived", "[object_resolver][selector]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Kick"),
		track_named(1, "Snare")
	};

	NoLearnedAliases aliases;

	SECTION("a guid selector takes the guid walk")
	{
		const TrackSelector selector = TrackGuidSelector{guid_for(1)};
		const TrackResolution resolution = sesh_ai::daw::resolve_track(tracks, aliases, selector);

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
		CHECK(std::get<ResolvedTrack>(resolution).source == ResolutionSource::guid);
		CHECK(sesh_ai::daw::describe_track_selector(selector) == "guid " + guid_for(1));
	}

	SECTION("a name pattern selector takes the precedence ladder")
	{
		const TrackSelector selector = TrackNamePatternSelector{"kick"};
		const TrackResolution resolution = sesh_ai::daw::resolve_track(tracks, aliases, selector);

		REQUIRE(sesh_ai::daw::is_resolved(resolution));
		CHECK(std::get<ResolvedTrack>(resolution).source == ResolutionSource::track_name);
		CHECK(sesh_ai::daw::describe_track_selector(selector) == "name pattern \"kick\"");
	}
}

// ---------------------------------------------------------------------------
// Exactly three outcomes (requirement 7.5)
// ---------------------------------------------------------------------------

TEST_CASE("every pattern over every track set lands on exactly one of three outcomes",
	"[object_resolver][outcomes]")
{
	const std::vector<std::vector<ResolvableTrack>> track_sets{
		{},
		{track_named(0, "Kick", StructuralRole::normal)},
		{track_named(0, "Kick"), track_named(1, "Kick")},
		{
			track_named(0, "Drums", StructuralRole::summing_folder_parent),
			track_named(1, "Guitars", StructuralRole::summing_folder_parent),
			track_named(2, "Lead Vox", StructuralRole::normal),
			track_named(3, "", StructuralRole::master)
		}
	};

	const std::vector<std::string> patterns{
		"", "   ", "kick", "KICK", "vox", "drum", "submix", "master",
		"aux return", "nothing like this", "{not-a-guid}"
	};

	RecordedAliases aliases;
	aliases.learn(guid_for(0), {"the thing"});
	aliases.learn(guid_for(1), {"the thing"});

	for (const std::vector<ResolvableTrack>& tracks : track_sets)
	{
		for (const std::string& pattern : patterns)
		{
			const TrackResolution resolution =
				sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, pattern);

			const int outcome_count = static_cast<int>(sesh_ai::daw::is_resolved(resolution))
				+ static_cast<int>(sesh_ai::daw::is_unresolved(resolution))
				+ static_cast<int>(sesh_ai::daw::is_ambiguous(resolution));

			// The variant makes this structural rather than checked, so the assertion
			// is really that the three predicates cover it and do not overlap.
			CHECK(outcome_count == 1);
			CHECK(resolution.index() < 3);

			if (sesh_ai::daw::is_ambiguous(resolution))
			{
				CHECK(std::get<AmbiguousTrackSelector>(resolution).candidates.size() >= 2);
			}
		}
	}
}

TEST_CASE("an empty track list resolves nothing", "[object_resolver][outcomes]")
{
	const std::vector<ResolvableTrack> tracks;

	NoLearnedAliases aliases;

	CHECK(sesh_ai::daw::is_unresolved(sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "kick")));
	CHECK(sesh_ai::daw::is_unresolved(sesh_ai::daw::resolve_track_by_guid(tracks, guid_for(0))));
}

// ---------------------------------------------------------------------------
// Refusals (requirements 7.3, 7.4)
// ---------------------------------------------------------------------------

TEST_CASE("an unresolved selector refuses with the contract reason and no acknowledgement",
	"[object_resolver][refusal]")
{
	const sesh_ai::daw::ToolResultRefusal refusal =
		sesh_ai::daw::build_unresolved_track_selector_refusal();

	CHECK(sesh_ai::daw::ToolResultRefusal::refused);
	CHECK(refusal.reason == "unresolved_track_selector");
	CHECK(refusal.blocking.empty());

	// There is nothing to acknowledge about a track that does not exist, and the
	// schema leaves the field optional for exactly this.
	CHECK(refusal.acknowledgement_field.empty());
}

TEST_CASE("an ambiguous selector names every candidate with its guid", "[object_resolver][refusal]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Bass"),
		track_named(1, "Bass"),
		track_named(2, "Bass")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "bass");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const sesh_ai::daw::ToolResultRefusal refusal =
		sesh_ai::daw::build_ambiguous_track_selector_refusal(std::get<AmbiguousTrackSelector>(resolution));

	CHECK(refusal.reason == "ambiguous_track_selector");
	CHECK(refusal.acknowledgement_field == "confirmedTrackSelection");
	REQUIRE(refusal.blocking.size() == 3);

	for (std::size_t candidate_index = 0; candidate_index < refusal.blocking.size(); ++candidate_index)
	{
		const sesh_ai::daw::BlockingEntity& entity = refusal.blocking[candidate_index];

		CHECK(entity.kind == "track");
		CHECK(entity.description == "Bass");
		CHECK(entity.guid == guid_for(static_cast<int>(candidate_index)));
	}
}

TEST_CASE("an unnamed candidate is still described", "[object_resolver][refusal]")
{
	std::vector<ResolvableTrack> tracks{
		track_named(0, ""),
		track_named(1, "")
	};
	tracks[0].structural_role = StructuralRole::aux_return;
	tracks[1].structural_role = StructuralRole::aux_return;

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "aux return");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const sesh_ai::daw::ToolResultRefusal refusal =
		sesh_ai::daw::build_ambiguous_track_selector_refusal(std::get<AmbiguousTrackSelector>(resolution));

	REQUIRE(refusal.blocking.size() == 2);

	// The schema requires a non-empty description on a blocking entity, so an unnamed
	// track needs words rather than an empty string.
	for (const sesh_ai::daw::BlockingEntity& entity : refusal.blocking)
	{
		CHECK_FALSE(entity.description.empty());
		CHECK_FALSE(entity.guid.empty());
	}
}

TEST_CASE("more candidates than blocking can carry are counted rather than dropped silently",
	"[object_resolver][refusal]")
{
	// The schema caps `blocking` at 512 entries. A truncated list that looked complete
	// would have the agent believe it had seen every candidate, so the last slot says
	// how many did not fit.
	const int candidate_count = 600;

	std::vector<ResolvableTrack> tracks;
	tracks.reserve(static_cast<std::size_t>(candidate_count));

	for (int ordinal = 0; ordinal < candidate_count; ++ordinal)
	{
		tracks.push_back(track_named(ordinal, "Take", StructuralRole::normal));
	}

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "take");

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));

	const AmbiguousTrackSelector& ambiguous = std::get<AmbiguousTrackSelector>(resolution);

	// Requirement 7.4 holds inside the component: every candidate is on the outcome.
	CHECK(ambiguous.candidates.size() == static_cast<std::size_t>(candidate_count));

	const sesh_ai::daw::ToolResultRefusal refusal =
		sesh_ai::daw::build_ambiguous_track_selector_refusal(ambiguous);

	REQUIRE(refusal.blocking.size() == sesh_ai::daw::maximum_blocking_entities);
	CHECK(refusal.blocking.back().guid.empty());
	CHECK(refusal.blocking.back().description == "89 further tracks match and are not listed");
}

// ---------------------------------------------------------------------------
// Safe and destructive dispositions (requirements 7.6, 7.7)
// ---------------------------------------------------------------------------

TEST_CASE("a safe operation proceeds on an imperfect match and reports the track it used",
	"[object_resolver][disposition]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Lead Vox")};

	NoLearnedAliases aliases;

	const TrackResolution resolution = sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "vox");

	const sesh_ai::daw::TrackResolutionDisposition disposition =
		sesh_ai::daw::dispose_of_track_resolution(resolution, OperationRisk::safe);

	CHECK(disposition.proceed);
	CHECK_FALSE(disposition.confirmation_required);
	CHECK_FALSE(disposition.refusal.has_value());
	REQUIRE(disposition.resolved_track.has_value());
	CHECK(disposition.resolved_track->name == "Lead Vox");
	CHECK(disposition.resolved_track->guid == guid_for(0));

	// The report is what requirement 7.6 asks for: which track was used, and honestly
	// how well it fitted.
	CHECK(disposition.report.find("Lead Vox") != std::string::npos);
	CHECK(disposition.report.find(guid_for(0)) != std::string::npos);
	CHECK(disposition.report.find("imperfect") != std::string::npos);
}

TEST_CASE("a destructive operation proceeds to a confirmation naming the track",
	"[object_resolver][disposition]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Lead Vox")};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "lead vox");

	const sesh_ai::daw::TrackResolutionDisposition disposition =
		sesh_ai::daw::dispose_of_track_resolution(resolution, OperationRisk::destructive);

	CHECK(disposition.proceed);
	CHECK(disposition.confirmation_required);
	REQUIRE(disposition.resolved_track.has_value());
	CHECK(disposition.report.find("Lead Vox") != std::string::npos);
	CHECK(disposition.report.find(guid_for(0)) != std::string::npos);
}

TEST_CASE("ambiguity refuses whatever the operation's risk", "[object_resolver][disposition]")
{
	const std::vector<ResolvableTrack> tracks{
		track_named(0, "Bass"),
		track_named(1, "Bass")
	};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "bass");

	for (const OperationRisk risk : {OperationRisk::safe, OperationRisk::destructive})
	{
		const sesh_ai::daw::TrackResolutionDisposition disposition =
			sesh_ai::daw::dispose_of_track_resolution(resolution, risk);

		CHECK_FALSE(disposition.proceed);
		CHECK_FALSE(disposition.confirmation_required);
		CHECK_FALSE(disposition.resolved_track.has_value());
		REQUIRE(disposition.refusal.has_value());
		CHECK(disposition.refusal->reason == "ambiguous_track_selector");
	}
}

TEST_CASE("an unresolved selector refuses whatever the operation's risk",
	"[object_resolver][disposition]")
{
	const std::vector<ResolvableTrack> tracks{track_named(0, "Kick")};

	NoLearnedAliases aliases;

	const TrackResolution resolution =
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, aliases, "nothing like this");

	for (const OperationRisk risk : {OperationRisk::safe, OperationRisk::destructive})
	{
		const sesh_ai::daw::TrackResolutionDisposition disposition =
			sesh_ai::daw::dispose_of_track_resolution(resolution, risk);

		CHECK_FALSE(disposition.proceed);
		REQUIRE(disposition.refusal.has_value());
		CHECK(disposition.refusal->reason == "unresolved_track_selector");
	}
}

// ---------------------------------------------------------------------------
// Markers and regions (requirement 7.8)
// ---------------------------------------------------------------------------

TEST_CASE("a marker guid resolves by enumeration and comparison", "[object_resolver][marker]")
{
	const std::vector<ResolvableMarkerOrRegion> markers_and_regions{
		marker_named(0, "Intro", 0.0),
		region_named(1, "Chorus", 16.0, 32.0),
		marker_named(2, "Bridge", 48.0)
	};

	const MarkerOrRegionResolution resolution =
		sesh_ai::daw::resolve_marker_or_region_by_guid(markers_and_regions, guid_for(1));

	REQUIRE(sesh_ai::daw::is_resolved(resolution));

	const ResolvedMarkerOrRegion& resolved = std::get<ResolvedMarkerOrRegion>(resolution);

	CHECK(resolved.object.name == "Chorus");
	CHECK(resolved.object.is_region);
	CHECK(resolved.object.start_seconds == 16.0);
	CHECK(resolved.object.end_seconds == 32.0);
	CHECK(resolved.source == ResolutionSource::guid);
}

TEST_CASE("a marker guid that names nothing is unresolved", "[object_resolver][marker]")
{
	const std::vector<ResolvableMarkerOrRegion> markers_and_regions{marker_named(0, "Intro", 0.0)};

	CHECK(sesh_ai::daw::is_unresolved(
		sesh_ai::daw::resolve_marker_or_region_by_guid(markers_and_regions, guid_for(7))));
	CHECK(sesh_ai::daw::is_unresolved(sesh_ai::daw::resolve_marker_or_region_by_guid({}, guid_for(0))));
}

TEST_CASE("markers and regions get the same three outcomes as tracks", "[object_resolver][marker]")
{
	std::vector<ResolvableMarkerOrRegion> markers_and_regions{
		marker_named(0, "Intro", 0.0),
		marker_named(1, "Intro", 4.0)
	};
	markers_and_regions[1].guid = markers_and_regions[0].guid;

	const MarkerOrRegionResolution resolution =
		sesh_ai::daw::resolve_marker_or_region_by_guid(markers_and_regions, guid_for(0));

	REQUIRE(sesh_ai::daw::is_ambiguous(resolution));
	CHECK(std::get<sesh_ai::daw::AmbiguousMarkerOrRegionSelector>(resolution).candidates.size() == 2);

	// Three alternatives, same as the track resolution.
	CHECK(std::variant_size_v<MarkerOrRegionResolution> == 3);
	CHECK(std::variant_size_v<TrackResolution> == 3);
}

TEST_CASE("a marker name is not a resolution source", "[object_resolver][marker]")
{
	// The tool schemas address markers and regions by GUID, and there is no refusal
	// reason in the taxonomy for an unresolvable marker pattern — so a name is carried
	// for reporting, not matched on.
	const std::vector<ResolvableMarkerOrRegion> markers_and_regions{marker_named(0, "Intro", 0.0)};

	CHECK(sesh_ai::daw::is_unresolved(
		sesh_ai::daw::resolve_marker_or_region_by_guid(markers_and_regions, "Intro")));
}
