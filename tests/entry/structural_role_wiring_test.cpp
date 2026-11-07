// The Object Resolver's structural roles, as the composition root wires them
// (task 30.1, requirements 6.6, 7.2, 18.3).
//
// The agent reads each track's role from the project context snapshot and then
// addresses tracks by it: "the submix", "the silent folder", "the return". So the
// claim worth testing is agreement. For the same project state, the role the resolver
// matches a track on is the role the snapshot reports for it. If the two disagreed,
// the agent would name a track by the role it was told and the resolver would refuse
// it, or resolve a different one.
//
// The agreement is checked against the payload the Project Context Builder actually
// publishes, not against `build_project_context_snapshot` called directly. The payload
// is what the agent reads, and comparing against it catches a role that was derived
// alike but serialised differently.
//
// The resolver side is `bind_structural_role_reader`, the function
// `extension_composition.cpp` passes to `ReaperTrackListSource`, over the same source
// the builder reads. That source's walk of REAPER is replaced by a reading the case
// wrote by hand. `ReaperTrackListSource`'s own walk is in a `.cpp` the suite does not
// compile, so the tracks it would report are built here the way that walk builds them:
// GUID, name, and index per indexed track, then the master with its role already set.
// What the walk then does with the reader's answer is `assign_structural_roles`, which
// is called here exactly as it is called there.
//
// The roles are read at call time, not taken from the last snapshot. One case changes
// the routing after a snapshot went out and checks that the resolver follows the
// project, and that the next snapshot reports what the resolver used.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <context/project_context_builder.h>
#include <context/structural_role.h>
#include <daw/object_resolver.h>
#include <entry/extension_composition.h>
#include <transport/queue_pair.h>

#include "../context/project_context_test_doubles.h"

using sesh_ai::context::ProjectContextBuilder;
using sesh_ai::context::ProjectContextReading;
using sesh_ai::context::StructuralRole;
using sesh_ai::context::TrackReading;
using sesh_ai::context::to_schema_string;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolutionSource;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::ResolvedTrack;
using sesh_ai::daw::TrackResolution;
using sesh_ai::entry::bind_structural_role_reader;
using sesh_ai_tests::StubDocument;
using sesh_ai_tests::StubOutboundEnvelope;
using sesh_ai_tests::StubProjectContextSource;
using sesh_ai_tests::plain_globals;
using sesh_ai_tests::plain_track;
using sesh_ai_tests::test_guid;

namespace
{
	using Builder = ProjectContextBuilder<StubDocument, StubOutboundEnvelope>;
	using OutboundQueue = sesh_ai::transport::ConcurrentQueue<StubOutboundEnvelope>;

	// Positions in `project_with_every_role`, named so a case reads as routing rather
	// than as indices.
	constexpr std::size_t drums_folder_position = 0;
	constexpr std::size_t pads_folder_position = 3;
	constexpr std::size_t pad_left_position = 4;

	TrackReading folder_track(int ordinal, std::string name, int folder_depth_delta)
	{
		TrackReading track = plain_track(test_guid(ordinal), std::move(name));
		track.folder_depth_delta = folder_depth_delta;

		return track;
	}

	TrackReading track_with_items(int ordinal, std::string name, int folder_depth_delta, bool parent_send_enabled)
	{
		TrackReading track = folder_track(ordinal, std::move(name), folder_depth_delta);
		track.item_count = 1;
		track.parent_send_enabled = parent_send_enabled;

		return track;
	}

	// One project in which every role appears:
	//
	//   Drums   folder parent, both children feed it         summing_folder_parent
	//     Kick
	//     Snare
	//   Pads    folder parent, neither child feeds it        silent_folder_parent
	//     Pad L parent send off
	//     Pad R parent send off
	//   Verb    two receives, no items                       aux_return
	//   master                                               master
	ProjectContextReading project_with_every_role()
	{
		ProjectContextReading reading;

		reading.readable = true;
		reading.globals = plain_globals();

		reading.tracks_in_project_order.push_back(folder_track(1, "Drums", 1));
		reading.tracks_in_project_order.push_back(track_with_items(2, "Kick", 0, true));
		reading.tracks_in_project_order.push_back(track_with_items(3, "Snare", -1, true));
		reading.tracks_in_project_order.push_back(folder_track(4, "Pads", 1));
		reading.tracks_in_project_order.push_back(track_with_items(5, "Pad L", 0, false));
		reading.tracks_in_project_order.push_back(track_with_items(6, "Pad R", -1, false));

		TrackReading verb = plain_track(test_guid(7), "Verb");
		verb.receive_count = 2;
		reading.tracks_in_project_order.push_back(verb);

		reading.master_track.present = true;
		reading.master_track.guid = test_guid(0);
		reading.master_track.name = "MASTER";

		return reading;
	}

	// What `ReaperTrackListSource`'s walk reports before it asks for roles: the indexed
	// tracks with no role, then the master with its own. GUIDs are lower-cased here
	// because the walk and the context source read them through different REAPER
	// calls, and the join between them must not depend on the two agreeing on case.
	std::vector<ResolvableTrack> tracks_as_the_track_list_walk_reports_them(const ProjectContextReading& reading)
	{
		std::vector<ResolvableTrack> tracks;

		for (std::size_t position = 0; position < reading.tracks_in_project_order.size(); ++position)
		{
			const TrackReading& track_reading = reading.tracks_in_project_order[position];

			ResolvableTrack track;
			track.guid = track_reading.guid;

			for (char& character : track.guid)
			{
				if (character >= 'A' && character <= 'F')
				{
					character = static_cast<char>(character - 'A' + 'a');
				}
			}

			track.name = track_reading.name;
			track.project_index = static_cast<int>(position);
			track.is_master_track = false;

			tracks.push_back(track);
		}

		ResolvableTrack master;
		master.guid = reading.master_track.guid;
		master.name = reading.master_track.name;
		master.project_index = -1;
		master.is_master_track = true;
		master.structural_role = StructuralRole::master;

		tracks.push_back(master);

		return tracks;
	}

	// The tracks as the resolver sees them during a tool call: walked, then given the
	// roles the composition root's reader supplies.
	std::vector<ResolvableTrack> tracks_as_the_resolver_sees_them(StubProjectContextSource& source)
	{
		std::vector<ResolvableTrack> tracks = tracks_as_the_track_list_walk_reports_them(source.reading);

		const sesh_ai::daw::ReaperTrackListSource::StructuralRoleReader reader =
			bind_structural_role_reader(source);

		sesh_ai::daw::assign_structural_roles(tracks, reader());

		return tracks;
	}

	// GUID to role, as the published payload states it, the master's included.
	std::map<std::string, std::string> roles_in_published_payload(const StubDocument& payload)
	{
		std::map<std::string, std::string> roles;

		for (const StubDocument& track : payload.at("tracks").elements())
		{
			roles[track.at("guid").string_value()] = track.at("role").string_value();
		}

		if (payload.contains("masterTrack"))
		{
			const StubDocument& master = payload.at("masterTrack");
			roles[master.at("guid").string_value()] = master.at("role").string_value();
		}

		return roles;
	}

	StubDocument publish_and_take_payload(Builder& builder, OutboundQueue& outbound_queue)
	{
		builder.publish_on_request(outbound_queue, "request-1");

		auto envelopes = outbound_queue.drain_up_to(4);

		REQUIRE(envelopes.size() == 1);

		return envelopes.front().payload;
	}

	std::string upper_case(std::string guid)
	{
		for (char& character : guid)
		{
			if (character >= 'a' && character <= 'f')
			{
				character = static_cast<char>(character - 'a' + 'A');
			}
		}

		return guid;
	}

	// Every track's role on the resolver side equals the payload's role for its GUID.
	void require_roles_agree(
		const std::vector<ResolvableTrack>& resolver_tracks,
		const std::map<std::string, std::string>& payload_roles)
	{
		REQUIRE(resolver_tracks.size() == payload_roles.size());

		for (const ResolvableTrack& track : resolver_tracks)
		{
			INFO("track " << track.name << " guid " << track.guid);

			const auto reported = payload_roles.find(upper_case(track.guid));

			REQUIRE(reported != payload_roles.end());
			REQUIRE(track.structural_role.has_value());
			CHECK(std::string{to_schema_string(*track.structural_role)} == reported->second);
		}
	}
}

TEST_CASE("for the same project state, the resolver's role for every track is the snapshot's",
	"[entry][composition][structural_role]")
{
	StubProjectContextSource source;
	source.reading = project_with_every_role();

	OutboundQueue outbound_queue;
	Builder builder{source};

	const std::map<std::string, std::string> payload_roles =
		roles_in_published_payload(publish_and_take_payload(builder, outbound_queue));

	const std::vector<ResolvableTrack> resolver_tracks = tracks_as_the_resolver_sees_them(source);

	require_roles_agree(resolver_tracks, payload_roles);

	// The agreement is only worth something if the project exercises the derivation,
	// so every role has to have appeared.
	for (const StructuralRole role : sesh_ai::context::all_structural_roles)
	{
		bool role_appears = false;

		for (const ResolvableTrack& track : resolver_tracks)
		{
			role_appears = role_appears || track.structural_role == role;
		}

		INFO("role " << to_schema_string(role));
		CHECK(role_appears);
	}
}

TEST_CASE("the resolver reads roles at call time, so it follows a routing change the last snapshot missed",
	"[entry][composition][structural_role]")
{
	StubProjectContextSource source;
	source.reading = project_with_every_role();

	OutboundQueue outbound_queue;
	Builder builder{source};

	const std::map<std::string, std::string> stale_payload_roles =
		roles_in_published_payload(publish_and_take_payload(builder, outbound_queue));

	const std::string pads_guid = source.reading.tracks_in_project_order[pads_folder_position].guid;

	REQUIRE(stale_payload_roles.at(pads_guid) == "silent_folder_parent");

	// The producer turns Pad L's parent send back on. Pads now sums, and no snapshot
	// has gone out since.
	source.reading.tracks_in_project_order[pad_left_position].parent_send_enabled = true;

	const std::size_t reads_before_the_call = source.project_reads;
	const std::vector<ResolvableTrack> resolver_tracks = tracks_as_the_resolver_sees_them(source);

	// One project read per call, however many tracks there are.
	CHECK(source.project_reads == reads_before_the_call + 1);

	CHECK(resolver_tracks[pads_folder_position].structural_role == StructuralRole::summing_folder_parent);

	// The next snapshot reports the role the resolver already used.
	const std::map<std::string, std::string> fresh_payload_roles =
		roles_in_published_payload(publish_and_take_payload(builder, outbound_queue));

	CHECK(fresh_payload_roles.at(pads_guid) == "summing_folder_parent");

	require_roles_agree(resolver_tracks, fresh_payload_roles);
}

TEST_CASE("a producer's role phrase resolves through the wired roles",
	"[entry][composition][structural_role]")
{
	StubProjectContextSource source;
	source.reading = project_with_every_role();

	const std::vector<ResolvableTrack> tracks = tracks_as_the_resolver_sees_them(source);
	NoLearnedAliases no_aliases;

	SECTION("the silent folder is Pads")
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, no_aliases, "the silent folder");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));

		const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

		CHECK(resolved.reference.name == "Pads");
		CHECK(resolved.source == ResolutionSource::structural_role);
	}

	SECTION("the submix is Drums, the only folder that sums")
	{
		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(tracks, no_aliases, "the submix");

		REQUIRE(sesh_ai::daw::is_resolved(resolution));

		const ResolvedTrack& resolved = std::get<ResolvedTrack>(resolution);

		CHECK(resolved.reference.name == "Drums");
		CHECK(resolved.source == ResolutionSource::structural_role);
	}

	SECTION("a folder parent once its children stop feeding it is no longer the submix")
	{
		source.reading.tracks_in_project_order[drums_folder_position + 1].parent_send_enabled = false;
		source.reading.tracks_in_project_order[drums_folder_position + 2].parent_send_enabled = false;

		const std::vector<ResolvableTrack> rerouted = tracks_as_the_resolver_sees_them(source);

		const TrackResolution resolution =
			sesh_ai::daw::resolve_track_by_name_pattern(rerouted, no_aliases, "the submix");

		CHECK(std::holds_alternative<sesh_ai::daw::UnresolvedSelector>(resolution));
	}
}

TEST_CASE("an unreadable project gives the resolver no roles, so a role phrase is refused",
	"[entry][composition][structural_role]")
{
	StubProjectContextSource source;
	source.reading = project_with_every_role();
	source.reading.readable = false;

	const std::vector<ResolvableTrack> tracks = tracks_as_the_resolver_sees_them(source);

	for (const ResolvableTrack& track : tracks)
	{
		INFO("track " << track.name);

		// The master's role is the walk's own, not the reader's, so it survives.
		if (track.is_master_track)
		{
			CHECK(track.structural_role == StructuralRole::master);
			continue;
		}

		CHECK_FALSE(track.structural_role.has_value());
	}

	NoLearnedAliases no_aliases;

	CHECK(std::holds_alternative<sesh_ai::daw::UnresolvedSelector>(
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, no_aliases, "the submix")));
}

TEST_CASE("a track the snapshot leaves out carries no role", "[entry][composition][structural_role]")
{
	StubProjectContextSource source;
	source.reading = project_with_every_role();

	// The snapshot drops a track whose GUID does not fit the schema, so the agent is
	// never told its role. The resolver must not answer to that role either.
	source.reading.tracks_in_project_order[pads_folder_position].guid = "not-a-guid";

	const std::vector<ResolvableTrack> tracks = tracks_as_the_resolver_sees_them(source);

	CHECK_FALSE(tracks[pads_folder_position].structural_role.has_value());

	NoLearnedAliases no_aliases;

	CHECK(std::holds_alternative<sesh_ai::daw::UnresolvedSelector>(
		sesh_ai::daw::resolve_track_by_name_pattern(tracks, no_aliases, "the silent folder")));
}
