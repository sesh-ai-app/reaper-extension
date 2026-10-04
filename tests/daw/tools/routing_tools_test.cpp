// The seven routing tools (task 10.5, requirements 9.1, 9.2, 9.7).
//
// Two things are worth trying to break here, and the suite is built around them.
//
// **A cycle check that never refuses.** Every one of these checks is asked about the
// *projected* graph — the graph as it would be after the change. Ask about the current
// graph instead and every test below still compiles, every handler still runs, and
// nothing is ever refused. So the refusal cases are paired with negative controls on
// the same graphs: an acyclic session must not be refused either, and property 27 is
// the harder half of requirement 9.7 because a false refusal blocks work the producer
// is entitled to do.
//
// **A cycle check that only reads the sends list.** Folder summing is invisible to
// send enumeration — `B_MAINSEND` is a flag on the child, not a send object — so a
// loop can close entirely through folder routing. `set_parent_send` and `create_bus`
// are the two handlers that can create one without an explicit send being involved,
// and both have a case here whose loop exists *only* because a folder parent's own
// parent send closes it. A handler wired to look at sends alone passes every other
// test in this file.
//
// The scripted host below is a small working model of REAPER's routing rather than a
// recorder: it creates and renumbers sends, moves tracks, writes folder depths, and
// deletes tracks. That is what makes "the successes survive a sibling failure"
// (requirement 9.4) an assertion about the session rather than a restatement of the
// payload describing it.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/tools/routing_tools.h>

using sesh_ai::context::StructuralRole;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_target;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::bus_type;
using sesh_ai::daw::count_applied_actions;
using sesh_ai::daw::count_failed_actions;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::LearnedAliasLookup;
using sesh_ai::daw::NoLearnedAliases;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::routing_graph;
using sesh_ai::daw::send_edge;
using sesh_ai::daw::signal_cycle_acknowledgement_field;
using sesh_ai::daw::signal_cycle_refusal_reason;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_partial_outcome;
using sesh_ai::daw::tool_refusal;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::TrackFolderDepth;
using sesh_ai::daw::TrackGuidSelector;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::TrackReference;
using sesh_ai::daw::TrackSelector;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::tools::bus_construction_request;
using sesh_ai::daw::tools::create_bus_result;
using sesh_ai::daw::tools::create_send_request;
using sesh_ai::daw::tools::create_send_result;
using sesh_ai::daw::tools::decide_on_projected_routing_graph;
using sesh_ai::daw::tools::find_silenced_folder_parents;
using sesh_ai::daw::tools::get_routing_result;
using sesh_ai::daw::tools::invalid_send_index_code;
using sesh_ai::daw::tools::not_a_bus_code;
using sesh_ai::daw::tools::plan_summing_bus_dissolution;
using sesh_ai::daw::tools::plan_summing_bus_layout;
using sesh_ai::daw::tools::projected_graph_with_parent_send;
using sesh_ai::daw::tools::register_routing_tools;
using sesh_ai::daw::tools::remove_bus_result;
using sesh_ai::daw::tools::remove_send_request;
using sesh_ai::daw::tools::remove_send_result;
using sesh_ai::daw::tools::routing_fx_placement;
using sesh_ai::daw::tools::routing_graph_from;
using sesh_ai::daw::tools::routing_host;
using sesh_ai::daw::tools::routing_payload_codec;
using sesh_ai::daw::tools::routing_send;
using sesh_ai::daw::tools::routing_send_mode;
using sesh_ai::daw::tools::routing_snapshot;
using sesh_ai::daw::tools::routing_tool_registration;
using sesh_ai::daw::tools::routing_track;
using sesh_ai::daw::tools::send_state_change;
using sesh_ai::daw::tools::set_parent_send_request;
using sesh_ai::daw::tools::set_parent_send_result;
using sesh_ai::daw::tools::set_send_state_request;
using sesh_ai::daw::tools::set_send_state_result;
using sesh_ai::daw::tools::summing_bus_dissolution;
using sesh_ai::daw::tools::summing_bus_layout;

namespace
{
	// ---------------------------------------------------------------------------
	// The payload type, and somewhere to read the results back from
	// ---------------------------------------------------------------------------

	// A plain struct rather than nlohmann::json. The framework never reads a field of
	// one and neither does `routing_tools.h` — the codec is the only thing that touches
	// the payload type, which is what lets the whole of both files be driven with no
	// JSON library present.
	struct test_payload
	{
		std::string label;
	};

	// Where the codec's writers put what they were given, so a test asserts against the
	// result struct the handler built rather than against a serialisation of it.
	struct recorded_results
	{
		std::optional<create_send_result> create_send;
		std::optional<remove_send_result> remove_send;
		std::optional<set_send_state_result> set_send_state;
		std::optional<set_parent_send_result> set_parent_send;
		std::optional<create_bus_result> create_bus;
		std::optional<remove_bus_result> remove_bus;
		std::optional<get_routing_result> get_routing;
	};

	// What the codec's readers extract from the (already validated) input.
	struct scripted_requests
	{
		create_send_request create_send;
		remove_send_request remove_send;
		set_send_state_request set_send_state;
		set_parent_send_request set_parent_send;
		bus_construction_request create_bus;
	};

	routing_payload_codec<test_payload> codec_over(
		scripted_requests& requests,
		recorded_results& recorded)
	{
		routing_payload_codec<test_payload> codec;

		codec.read_create_send_request = [&requests](const test_payload&) { return requests.create_send; };
		codec.read_remove_send_request = [&requests](const test_payload&) { return requests.remove_send; };
		codec.read_set_send_state_request = [&requests](const test_payload&) {
			return requests.set_send_state;
		};
		codec.read_set_parent_send_request = [&requests](const test_payload&) {
			return requests.set_parent_send;
		};
		codec.read_create_bus_request = [&requests](const test_payload&) { return requests.create_bus; };

		codec.write_create_send_result = [&recorded](const create_send_result& result) {
			recorded.create_send = result;

			return test_payload{"create_send"};
		};
		codec.write_remove_send_result = [&recorded](const remove_send_result& result) {
			recorded.remove_send = result;

			return test_payload{"remove_send"};
		};
		codec.write_set_send_state_result = [&recorded](const set_send_state_result& result) {
			recorded.set_send_state = result;

			return test_payload{"set_send_state"};
		};
		codec.write_set_parent_send_result = [&recorded](const set_parent_send_result& result) {
			recorded.set_parent_send = result;

			return test_payload{"set_parent_send"};
		};
		codec.write_create_bus_result = [&recorded](const create_bus_result& result) {
			recorded.create_bus = result;

			return test_payload{"create_bus"};
		};
		codec.write_remove_bus_result = [&recorded](const remove_bus_result& result) {
			recorded.remove_bus = result;

			return test_payload{"remove_bus"};
		};
		codec.write_get_routing_result = [&recorded](const get_routing_result& result) {
			recorded.get_routing = result;

			return test_payload{"get_routing"};
		};

		return codec;
	}

	// ---------------------------------------------------------------------------
	// A scripted REAPER undo stack, counting everything the framework does to it
	// ---------------------------------------------------------------------------

	class scripted_undo_stack final : public undo_stack
	{
	public:
		int current_position() override { return position_; }

		std::string entry_description_at(int position) override
		{
			if (position < 0 || static_cast<std::size_t>(position) >= entries_.size())
			{
				return {};
			}

			return entries_[static_cast<std::size_t>(position)];
		}

		void begin_block() override
		{
			++begin_block_call_count;
			++open_block_depth;

			if (open_block_depth > deepest_open_block_depth)
			{
				deepest_open_block_depth = open_block_depth;
			}
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;

			entries_.resize(static_cast<std::size_t>(position_));
			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			++undo_call_count;

			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int undo_call_count = 0;
		int open_block_depth = 0;
		int deepest_open_block_depth = 0;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class scripted_track_list final : public TrackListSource
	{
	public:
		explicit scripted_track_list(std::vector<ResolvableTrack> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		std::vector<ResolvableTrack> tracks_in_project_order() const override { return tracks_; }

	private:
		std::vector<ResolvableTrack> tracks_;
	};

	// ---------------------------------------------------------------------------
	// Synthetic sessions
	// ---------------------------------------------------------------------------

	// REAPER's braced GUID form, which the schemas' `guid` pattern expects.
	std::string guid_for(char distinguishing_character)
	{
		std::string guid{"{00000000-0000-0000-0000-0000000000"};
		guid.push_back(distinguishing_character);
		guid.push_back(distinguishing_character);
		guid.push_back('}');

		return guid;
	}

	routing_track track_of(
		const std::string& name,
		char distinguishing_character,
		int folder_depth_delta,
		bool parent_send_enabled)
	{
		routing_track track;
		track.guid = guid_for(distinguishing_character);
		track.name = name;
		track.folder_depth_delta = folder_depth_delta;
		track.parent_send_enabled = parent_send_enabled;
		track.item_count = 1;
		track.fx_count = 0;

		return track;
	}

	// One explicit send, wired onto both ends the way REAPER reports it: on the source
	// track's sends and on the destination track's receives.
	void wire_send(routing_snapshot& snapshot, const std::string& source_guid, const std::string& destination_guid)
	{
		routing_track* source = nullptr;
		routing_track* destination = nullptr;

		for (routing_track& track : snapshot.tracks_in_project_order)
		{
			if (track.guid == source_guid)
			{
				source = &track;
			}

			if (track.guid == destination_guid)
			{
				destination = &track;
			}
		}

		REQUIRE(source != nullptr);
		REQUIRE(destination != nullptr);

		routing_send outgoing;
		outgoing.index = static_cast<int>(source->sends.size());
		outgoing.track_guid = destination->guid;
		outgoing.track_name = destination->name;

		routing_send incoming = outgoing;
		incoming.track_guid = source->guid;
		incoming.track_name = source->name;

		source->sends.push_back(outgoing);
		destination->receives.push_back(incoming);
	}

	std::vector<ResolvableTrack> resolvable_from(const routing_snapshot& snapshot)
	{
		std::vector<ResolvableTrack> tracks;
		tracks.reserve(snapshot.tracks_in_project_order.size());

		for (std::size_t index = 0; index < snapshot.tracks_in_project_order.size(); ++index)
		{
			ResolvableTrack track;
			track.guid = snapshot.tracks_in_project_order[index].guid;
			track.name = snapshot.tracks_in_project_order[index].name;
			track.project_index = static_cast<int>(index);

			tracks.push_back(std::move(track));
		}

		return tracks;
	}

	// ---------------------------------------------------------------------------
	// A working model of REAPER's routing
	// ---------------------------------------------------------------------------

	class scripted_routing_host final : public routing_host
	{
	public:
		routing_snapshot snapshot;

		// Guids the host refuses to act on, for the sibling-failure cases.
		std::vector<std::string> tracks_refusing_parent_send;
		std::vector<std::string> tracks_refusing_send_creation;
		bool refuse_track_insertion = false;
		bool refuse_fx = false;

		// What was actually asked of REAPER.
		int snapshot_read_count = 0;
		std::vector<std::string> track_order_applications;
		std::vector<std::pair<std::string, int>> folder_depth_writes;
		std::vector<std::string> deleted_tracks;
		std::vector<std::string> added_fx;

		routing_snapshot read_routing_snapshot() override
		{
			++snapshot_read_count;

			return snapshot;
		}

		std::optional<routing_send> read_send(const std::string& source_track_guid, int send_index) override
		{
			routing_track* const source = find(source_track_guid);

			if (source == nullptr || send_index < 0
				|| static_cast<std::size_t>(send_index) >= source->sends.size())
			{
				return std::nullopt;
			}

			return source->sends[static_cast<std::size_t>(send_index)];
		}

		std::optional<int> create_send(
			const std::string& source_track_guid,
			const std::string& destination_track_guid) override
		{
			for (const std::string& refused : tracks_refusing_send_creation)
			{
				if (refused == source_track_guid)
				{
					return std::nullopt;
				}
			}

			routing_track* const source = find(source_track_guid);
			routing_track* const destination = find(destination_track_guid);

			if (source == nullptr || destination == nullptr)
			{
				return std::nullopt;
			}

			routing_send outgoing;
			outgoing.index = static_cast<int>(source->sends.size());
			outgoing.track_guid = destination->guid;
			outgoing.track_name = destination->name;

			routing_send incoming = outgoing;
			incoming.track_guid = source->guid;
			incoming.track_name = source->name;

			source->sends.push_back(outgoing);
			destination->receives.push_back(incoming);

			return outgoing.index;
		}

		bool remove_send(const std::string& source_track_guid, int send_index) override
		{
			routing_track* const source = find(source_track_guid);

			if (source == nullptr || send_index < 0
				|| static_cast<std::size_t>(send_index) >= source->sends.size())
			{
				return false;
			}

			const routing_send removed = source->sends[static_cast<std::size_t>(send_index)];

			source->sends.erase(source->sends.begin() + send_index);

			// REAPER renumbers the sends after the removed one.
			for (std::size_t index = 0; index < source->sends.size(); ++index)
			{
				source->sends[index].index = static_cast<int>(index);
			}

			routing_track* const destination = find(removed.track_guid);

			if (destination != nullptr)
			{
				for (std::size_t index = 0; index < destination->receives.size(); ++index)
				{
					if (destination->receives[index].track_guid == source_track_guid)
					{
						destination->receives.erase(destination->receives.begin()
							+ static_cast<std::ptrdiff_t>(index));
						break;
					}
				}
			}

			return true;
		}

		bool write_send_state(
			const std::string& source_track_guid,
			int send_index,
			const send_state_change& change) override
		{
			routing_track* const source = find(source_track_guid);

			if (source == nullptr || send_index < 0
				|| static_cast<std::size_t>(send_index) >= source->sends.size())
			{
				return false;
			}

			routing_send& send = source->sends[static_cast<std::size_t>(send_index)];

			send.volume_decibels = change.volume_decibels.value_or(send.volume_decibels);
			send.pan_percent = change.pan_percent.value_or(send.pan_percent);
			send.muted = change.muted.value_or(send.muted);
			send.mode = change.mode.value_or(send.mode);

			return true;
		}

		bool write_parent_send(const std::string& track_guid, bool enabled) override
		{
			for (const std::string& refused : tracks_refusing_parent_send)
			{
				if (refused == track_guid)
				{
					return false;
				}
			}

			routing_track* const track = find(track_guid);

			if (track == nullptr)
			{
				return false;
			}

			track->parent_send_enabled = enabled;

			return true;
		}

		std::optional<TrackReference> insert_track(int project_index, const std::string& name) override
		{
			if (refuse_track_insertion)
			{
				return std::nullopt;
			}

			routing_track inserted;
			inserted.guid = guid_for('z');
			inserted.name = name;
			inserted.folder_depth_delta = 0;
			inserted.parent_send_enabled = true;
			inserted.item_count = 0;
			inserted.fx_count = 0;

			const std::size_t at = project_index < 0
				|| static_cast<std::size_t>(project_index) > snapshot.tracks_in_project_order.size()
					? snapshot.tracks_in_project_order.size()
					: static_cast<std::size_t>(project_index);

			snapshot.tracks_in_project_order.insert(
				snapshot.tracks_in_project_order.begin() + static_cast<std::ptrdiff_t>(at),
				inserted);

			return TrackReference{inserted.guid, inserted.name};
		}

		bool delete_track(const std::string& track_guid) override
		{
			for (std::size_t index = 0; index < snapshot.tracks_in_project_order.size(); ++index)
			{
				if (snapshot.tracks_in_project_order[index].guid != track_guid)
				{
					continue;
				}

				snapshot.tracks_in_project_order.erase(
					snapshot.tracks_in_project_order.begin() + static_cast<std::ptrdiff_t>(index));

				deleted_tracks.push_back(track_guid);

				return true;
			}

			return false;
		}

		bool apply_track_order(const std::vector<std::string>& track_guids_in_project_order) override
		{
			std::vector<routing_track> reordered;
			reordered.reserve(track_guids_in_project_order.size());

			for (const std::string& track_guid : track_guids_in_project_order)
			{
				const routing_track* const track = find(track_guid);

				if (track == nullptr)
				{
					return false;
				}

				reordered.push_back(*track);
			}

			if (reordered.size() != snapshot.tracks_in_project_order.size())
			{
				return false;
			}

			snapshot.tracks_in_project_order = std::move(reordered);
			track_order_applications.push_back("applied");

			return true;
		}

		bool write_folder_depth(const std::string& track_guid, int folder_depth_delta) override
		{
			routing_track* const track = find(track_guid);

			if (track == nullptr)
			{
				return false;
			}

			track->folder_depth_delta = folder_depth_delta;
			folder_depth_writes.emplace_back(track_guid, folder_depth_delta);

			return true;
		}

		std::optional<routing_fx_placement> add_fx(
			const std::string& track_guid,
			const std::string& fx_name) override
		{
			if (refuse_fx)
			{
				return std::nullopt;
			}

			routing_track* const track = find(track_guid);

			if (track == nullptr)
			{
				return std::nullopt;
			}

			routing_fx_placement placement;
			placement.index = track->fx_count;
			placement.name = fx_name;
			placement.bypassed = false;

			++track->fx_count;
			added_fx.push_back(fx_name);

			return placement;
		}

		routing_track* find(const std::string& track_guid)
		{
			for (routing_track& track : snapshot.tracks_in_project_order)
			{
				if (track.guid == track_guid)
				{
					return &track;
				}
			}

			return nullptr;
		}
	};

	// ---------------------------------------------------------------------------
	// One executor, wired the way the extension wires it
	// ---------------------------------------------------------------------------

	using test_registry = tool_handler_registry<test_payload>;
	using test_executor = tool_executor_of<test_payload>;
	using test_call = tool_call<test_payload>;
	using test_outcome = dispatch_outcome<test_payload>;
	using test_result = tool_result<test_payload>;

	struct routing_fixture
	{
		explicit routing_fixture(routing_snapshot snapshot)
			: host{}, tracks{resolvable_from(snapshot)}
		{
			host.snapshot = std::move(snapshot);
			host.snapshot.readable = true;

			registration = register_routing_tools<test_payload>(registry, host, codec_over(requests, recorded));

			executor = std::make_unique<test_executor>(registry, undo, tracks, aliases);
			undo.begin_turn();
		}

		test_outcome execute(std::string_view tool_name, const std::vector<std::string>& target_guids)
		{
			test_call call;
			call.tool_name = std::string{tool_name};
			call.validated_input = &input;
			call.confirmed_signal_cycle = confirmed_signal_cycle;

			for (const std::string& target_guid : target_guids)
			{
				call.track_selectors.push_back(TrackSelector{TrackGuidSelector{target_guid}});
			}

			return executor->execute(call);
		}

		scripted_routing_host host;
		scripted_requests requests;
		recorded_results recorded;
		routing_tool_registration registration;

		scripted_undo_stack stack;
		undo_manager undo{stack};
		scripted_track_list tracks;
		NoLearnedAliases aliases;
		test_registry registry;
		std::unique_ptr<test_executor> executor;

		test_payload input{"tool input"};
		bool confirmed_signal_cycle = false;
	};

	const test_result& result_of(const test_outcome& outcome)
	{
		REQUIRE(std::holds_alternative<test_result>(outcome));

		return std::get<test_result>(outcome);
	}

	const tool_refusal& refusal_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_refusal>(result));

		return std::get<tool_refusal>(result);
	}

	const tool_success<test_payload>& success_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_success<test_payload>>(result));

		return std::get<tool_success<test_payload>>(result);
	}

	const tool_partial_outcome<test_payload>& partial_of(const test_outcome& outcome)
	{
		const test_result& result = result_of(outcome);

		REQUIRE(std::holds_alternative<tool_partial_outcome<test_payload>>(result));

		return std::get<tool_partial_outcome<test_payload>>(result);
	}

	// A flat session: three tracks, no folders, no sends.
	routing_snapshot flat_session()
	{
		routing_snapshot snapshot;
		snapshot.readable = true;
		snapshot.tracks_in_project_order.push_back(track_of("Kick", 'a', 0, true));
		snapshot.tracks_in_project_order.push_back(track_of("Snare", 'b', 0, true));
		snapshot.tracks_in_project_order.push_back(track_of("Bass", 'c', 0, true));

		snapshot.master_track.present = true;
		snapshot.master_track.guid = guid_for('m');
		snapshot.master_track.name = "MASTER";

		return snapshot;
	}

	// A folder: Drums summing Kick and Snare, both children feeding it. No explicit
	// sends anywhere — every bit of summing in this session is folder summing.
	routing_snapshot folder_session()
	{
		routing_snapshot snapshot;
		snapshot.readable = true;

		routing_track drums = track_of("Drums", 'd', 1, true);
		drums.item_count = 0;
		snapshot.tracks_in_project_order.push_back(drums);

		snapshot.tracks_in_project_order.push_back(track_of("Kick", 'a', 0, true));
		snapshot.tracks_in_project_order.push_back(track_of("Snare", 'b', -1, true));

		snapshot.master_track.present = true;
		snapshot.master_track.guid = guid_for('m');
		snapshot.master_track.name = "MASTER";

		return snapshot;
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST_CASE("all seven routing tools register through the executor's seam", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	CHECK(fixture.registration.every_tool_registered());
	REQUIRE(fixture.registration.registered_tool_names.size() == 7);

	// The registration call decides the undo effect, and the split is the point:
	// `get_routing` opens no block and records no marker (requirement 10.3), the other
	// six open exactly one each.
	CHECK(fixture.registry.find_tool("get_routing")->undo_effect() == tool_undo_effect::none);
	CHECK(fixture.registry.find_tool("create_send")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(fixture.registry.find_tool("remove_send")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(fixture.registry.find_tool("set_send_state")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(fixture.registry.find_tool("set_parent_send")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(fixture.registry.find_tool("create_bus")->undo_effect() == tool_undo_effect::undo_block);
	CHECK(fixture.registry.find_tool("remove_bus")->undo_effect() == tool_undo_effect::undo_block);
}

// ---------------------------------------------------------------------------
// The graph the checks run over
// ---------------------------------------------------------------------------

TEST_CASE("the graph carries folder state, not just the sends list", "[routing_tools]")
{
	const routing_snapshot snapshot = folder_session();
	const routing_graph graph = routing_graph_from(snapshot);

	// No explicit send exists in this session at all — everything is folder summing,
	// which is exactly what send enumeration cannot see.
	CHECK(graph.explicit_sends.empty());
	REQUIRE(graph.tracks_in_project_order.size() == 3);
	CHECK(graph.tracks_in_project_order[0].folder_depth_delta == 1);
	CHECK(graph.tracks_in_project_order[2].folder_depth_delta == -1);
	CHECK(graph.tracks_in_project_order[1].parent_send_enabled);
}

TEST_CASE("an acyclic graph is not refused", "[routing_tools]")
{
	// Property 27, and the harder direction of requirement 9.7: a false refusal blocks
	// routing the producer is entitled to build.
	const routing_snapshot snapshot = folder_session();
	const routing_graph graph = routing_graph_from(snapshot);

	CHECK(decide_on_projected_routing_graph(graph, false).permitted);

	// And still not refused once every parent send is on, which is REAPER's own default
	// for a new track.
	const routing_graph everything_feeding_its_parent = projected_graph_with_parent_send(
		graph,
		{guid_for('a'), guid_for('b'), guid_for('d')},
		true);

	CHECK(decide_on_projected_routing_graph(everything_feeding_its_parent, false).permitted);
}

// ---------------------------------------------------------------------------
// create_send — requirement 9.7 through an explicit send
// ---------------------------------------------------------------------------

TEST_CASE("create_send refuses a send that would close a loop, and opens no undo block", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();
	wire_send(snapshot, guid_for('b'), guid_for('a'));

	routing_fixture fixture{std::move(snapshot)};

	const test_outcome outcome = fixture.execute("create_send", {guid_for('a'), guid_for('b')});

	// Requirements 9.3 and 10.7: the refusal is a precondition, so no block was opened
	// and no undo position marker was recorded. And nothing reached REAPER. Asserted
	// before the payload, so the ordering is checked even when the shape is right.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.stack.end_block_call_count == 0);
	CHECK(fixture.undo.has_undo_position_marker() == false);
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].sends.empty());

	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{signal_cycle_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{signal_cycle_acknowledgement_field});
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].description.empty() == false);
}

TEST_CASE("create_send creates the send when nothing loops", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.create_send.volume_decibels = -6.0;
	fixture.requests.create_send.pan_percent = 25.0;
	fixture.requests.create_send.mode = routing_send_mode::pre_fx;

	const test_outcome outcome = fixture.execute("create_send", {guid_for('a'), guid_for('b')});

	CHECK(success_of(outcome).fields.label == "create_send");

	REQUIRE(fixture.recorded.create_send.has_value());
	CHECK(fixture.recorded.create_send->source_track.name == "Kick");
	CHECK(fixture.recorded.create_send->destination_track.name == "Snare");
	CHECK(fixture.recorded.create_send->send_index == 0);
	CHECK(fixture.recorded.create_send->volume_decibels == -6.0);
	CHECK(fixture.recorded.create_send->pan_percent == 25.0);
	CHECK(fixture.recorded.create_send->mode == routing_send_mode::pre_fx);

	// One identifiable block, and the send actually landed.
	CHECK(fixture.stack.begin_block_call_count == 1);
	CHECK(fixture.stack.end_block_call_count == 1);
	CHECK(fixture.stack.open_block_depth == 0);
	REQUIRE(fixture.host.snapshot.tracks_in_project_order[0].sends.size() == 1);
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].sends[0].volume_decibels == -6.0);
}

TEST_CASE("create_send proceeds when the producer confirmed the loop", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();
	wire_send(snapshot, guid_for('b'), guid_for('a'));

	routing_fixture fixture{std::move(snapshot)};
	fixture.confirmed_signal_cycle = true;

	const test_outcome outcome = fixture.execute("create_send", {guid_for('a'), guid_for('b')});

	// A deliberate feedback path is a real thing to build, so the acknowledgement
	// permits rather than overriding.
	CHECK(success_of(outcome).fields.label == "create_send");
	CHECK(fixture.stack.begin_block_call_count == 1);
}

TEST_CASE("create_send with one target is a failed action, not a refusal", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	const test_outcome outcome = fixture.execute("create_send", {guid_for('a')});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// An incomplete call names no acknowledgement the producer could confirm, so it is
	// reported as a failed action carrying a reason — the same reasoning requirement 9.6
	// gives for an inverted time range.
	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(count_failed_actions(partial.actions) == 1);
}

// ---------------------------------------------------------------------------
// set_parent_send — the folder-closed cycle
// ---------------------------------------------------------------------------

namespace
{
	// A session whose only possible loop closes through folder routing.
	//
	//   Group   opens a folder, at top level
	//     Bus     opens a nested folder, parent send OFF — so it feeds nothing yet
	//       Child closes both folders, parent send ON — so Child -> Bus
	//   Group -> Child, as an explicit send.
	//
	// As it stands: Child -> Bus is a dead end, and Group -> Child goes nowhere else.
	// Acyclic. Switch Bus's parent send on and the loop closes:
	// Child -> Bus -> Group -> Child. Two of those three hops are parent sends, which
	// appear in no send enumeration, so a check reading the sends list sees one send and
	// no loop.
	routing_snapshot folder_closed_loop_session()
	{
		routing_snapshot snapshot;
		snapshot.readable = true;

		routing_track group = track_of("Group", 'g', 1, false);
		group.item_count = 0;
		snapshot.tracks_in_project_order.push_back(group);

		routing_track bus = track_of("Bus", 'p', 1, false);
		bus.item_count = 0;
		snapshot.tracks_in_project_order.push_back(bus);

		snapshot.tracks_in_project_order.push_back(track_of("Child", 'c', -2, true));

		snapshot.master_track.present = true;
		snapshot.master_track.guid = guid_for('m');
		snapshot.master_track.name = "MASTER";

		wire_send(snapshot, guid_for('g'), guid_for('c'));

		return snapshot;
	}
}

TEST_CASE("the folder-closed loop session is acyclic before the change", "[routing_tools]")
{
	// The negative control the refusal below depends on. Without this, a check that
	// refuses everything would pass the next test.
	const routing_graph graph = routing_graph_from(folder_closed_loop_session());

	CHECK(decide_on_projected_routing_graph(graph, false).permitted);
}

TEST_CASE("set_parent_send refuses a loop that closes only through folder routing", "[routing_tools]")
{
	routing_fixture fixture{folder_closed_loop_session()};

	fixture.requests.set_parent_send.enabled = true;

	const test_outcome outcome = fixture.execute("set_parent_send", {guid_for('p')});

	// Requirements 9.3 and 10.7, asserted before the payload is inspected so that a
	// check moved inside the undo block is caught here rather than only by the shape of
	// the result. The flag was not written either.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.stack.end_block_call_count == 0);
	CHECK(fixture.undo.has_undo_position_marker() == false);
	CHECK(fixture.host.snapshot.tracks_in_project_order[1].parent_send_enabled == false);

	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{signal_cycle_refusal_reason});
	CHECK(refusal.acknowledgement_field == std::string{signal_cycle_acknowledgement_field});

	// The description names the mechanism at each hop, which is what makes the refusal
	// actionable: folder summing is the edge the producer cannot see in the routing
	// window.
	REQUIRE(refusal.blocking.size() == 1);
	CHECK(refusal.blocking[0].description.find("folder parent send") != std::string::npos);
}

TEST_CASE("the folder-closed loop is found as a cycle through folder routing", "[routing_tools]")
{
	// Stated directly against the detector as well as through the handler, so a failure
	// says which of the two is wrong.
	const routing_graph projected = projected_graph_with_parent_send(
		routing_graph_from(folder_closed_loop_session()),
		{guid_for('p')},
		true);

	const sesh_ai::daw::routing_change_decision decision =
		decide_on_projected_routing_graph(projected, false);

	CHECK(decision.permitted == false);
	CHECK(decision.cycle.cycle_detected);
	CHECK(decision.cycle.closes_through_folder_routing());
}

TEST_CASE("switching a parent send off is never refused", "[routing_tools]")
{
	// Nothing that removes an edge can close a loop, so the check does not run at all.
	routing_fixture fixture{folder_closed_loop_session()};

	fixture.requests.set_parent_send.enabled = false;

	const test_outcome outcome = fixture.execute("set_parent_send", {guid_for('c')});

	CHECK(success_of(outcome).fields.label == "set_parent_send");
	CHECK(fixture.host.snapshot.tracks_in_project_order[2].parent_send_enabled == false);
}

TEST_CASE("set_parent_send names what each track's flag routes to", "[routing_tools]")
{
	routing_fixture fixture{folder_session()};

	fixture.requests.set_parent_send.enabled = false;

	// Kick sits in the Drums folder; Drums itself sits at top level, so its flag targets
	// the master track. The target is context-dependent, which is why it is reported
	// rather than left for the agent to infer.
	const test_outcome outcome = fixture.execute("set_parent_send", {guid_for('a'), guid_for('d')});

	CHECK(success_of(outcome).fields.label == "set_parent_send");

	REQUIRE(fixture.recorded.set_parent_send.has_value());
	const set_parent_send_result& result = *fixture.recorded.set_parent_send;

	CHECK(result.enabled == false);
	REQUIRE(result.tracks.size() == 2);

	CHECK(result.tracks[0].track.name == "Kick");
	CHECK(result.tracks[0].previous_enabled);
	CHECK(result.tracks[0].targets_folder_parent);
	CHECK(result.tracks[0].target_guid == guid_for('d'));

	CHECK(result.tracks[1].track.name == "Drums");
	CHECK(result.tracks[1].targets_folder_parent == false);
	CHECK(result.tracks[1].target_guid == guid_for('m'));
}

TEST_CASE("set_parent_send names a folder parent it just silenced", "[routing_tools]")
{
	routing_fixture fixture{folder_session()};

	fixture.requests.set_parent_send.enabled = false;

	// Both of Drums's children routed around it. Drums holds no items, no receives and
	// no FX, so it now looks like a summing bus and sums nothing — named here rather
	// than discovered at render time.
	const test_outcome outcome = fixture.execute("set_parent_send", {guid_for('a'), guid_for('b')});

	CHECK(success_of(outcome).fields.label == "set_parent_send");

	REQUIRE(fixture.recorded.set_parent_send.has_value());
	REQUIRE(fixture.recorded.set_parent_send->silenced_folder_parents.size() == 1);
	CHECK(fixture.recorded.set_parent_send->silenced_folder_parents[0].name == "Drums");
}

TEST_CASE("set_parent_send keeps the successes when a sibling fails", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.set_parent_send.enabled = false;
	fixture.host.tracks_refusing_parent_send.push_back(guid_for('b'));

	const test_outcome outcome =
		fixture.execute("set_parent_send", {guid_for('a'), guid_for('b'), guid_for('c')});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// Requirement 9.4: the whole array ran in one block, and the sibling failure did not
	// roll anything back.
	REQUIRE(partial.actions.size() == 3);
	CHECK(count_applied_actions(partial.actions) == 2);
	CHECK(count_failed_actions(partial.actions) == 1);
	CHECK(action_target(partial.actions[1]) == guid_for('b'));

	CHECK(fixture.stack.begin_block_call_count == 1);
	CHECK(fixture.stack.end_block_call_count == 1);
	CHECK(fixture.stack.undo_call_count == 0);

	// The session, not the payload describing it: the two writes that landed are still
	// there.
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].parent_send_enabled == false);
	CHECK(fixture.host.snapshot.tracks_in_project_order[1].parent_send_enabled);
	CHECK(fixture.host.snapshot.tracks_in_project_order[2].parent_send_enabled == false);

	// Requirement 9.5: a reasonless failure is not representable, and the one reported
	// here carries its reason.
	const sesh_ai::daw::action_failed& failed =
		std::get<sesh_ai::daw::action_failed>(partial.actions[1]);

	CHECK(failed.error.code().empty() == false);
	CHECK(failed.error.message().empty() == false);
}

// ---------------------------------------------------------------------------
// create_bus — two different builds, and the other folder-closed cycle
// ---------------------------------------------------------------------------

namespace
{
	// A folder whose parent sends down into one of its own children.
	//
	//   Group  opens a folder, top level
	//     Lead   parent send OFF, so it currently feeds nothing
	//     Backing parent send ON, closes the folder
	//   Group -> Lead, as an explicit send.
	//
	// Acyclic as it stands. Build a summing bus over Lead and the bus is inserted in
	// Group with Lead beneath it, so Lead -> bus -> Group -> Lead closes — and
	// `create_bus` for a summing bus creates no sends at all, so nothing in the sends
	// list changed.
	routing_snapshot folder_parent_feeding_its_child_session()
	{
		routing_snapshot snapshot;
		snapshot.readable = true;

		routing_track group = track_of("Group", 'g', 1, false);
		group.item_count = 0;
		snapshot.tracks_in_project_order.push_back(group);

		snapshot.tracks_in_project_order.push_back(track_of("Lead", 'l', 0, false));
		snapshot.tracks_in_project_order.push_back(track_of("Backing", 'k', -1, true));

		snapshot.master_track.present = true;
		snapshot.master_track.guid = guid_for('m');
		snapshot.master_track.name = "MASTER";

		wire_send(snapshot, guid_for('g'), guid_for('l'));

		return snapshot;
	}
}

TEST_CASE("a summing bus is refused when re-parenting closes a loop", "[routing_tools]")
{
	routing_fixture fixture{folder_parent_feeding_its_child_session()};

	fixture.requests.create_bus.type = bus_type::summing;
	fixture.requests.create_bus.bus_name = "Vocals";

	const test_outcome outcome = fixture.execute("create_bus", {guid_for('l')});

	// Nothing was opened and no track was inserted. Asserted first, so a check that had
	// drifted into the mutating body fails here on the block count.
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(fixture.stack.end_block_call_count == 0);
	CHECK(fixture.undo.has_undo_position_marker() == false);
	CHECK(fixture.host.snapshot.tracks_in_project_order.size() == 3);

	const tool_refusal& refusal = refusal_of(outcome);

	CHECK(refusal.reason == std::string{signal_cycle_refusal_reason});
	REQUIRE(refusal.blocking.size() == 1);

	// The loop passes through folder summing, which is the whole reason a check reading
	// the sends list would have permitted this.
	CHECK(refusal.blocking[0].description.find("folder parent send") != std::string::npos);
}

TEST_CASE("an aux bus over the same tracks is not refused", "[routing_tools]")
{
	// The negative control for the case above, and it separates the two mechanisms: an
	// aux bus re-parents nothing, so the loop the summing bus would have closed does not
	// exist.
	routing_fixture fixture{folder_parent_feeding_its_child_session()};

	fixture.requests.create_bus.type = bus_type::aux;
	fixture.requests.create_bus.bus_name = "Verb";

	const test_outcome outcome = fixture.execute("create_bus", {guid_for('l')});

	CHECK(success_of(outcome).fields.label == "create_bus");
	REQUIRE(fixture.recorded.create_bus.has_value());
	CHECK(fixture.recorded.create_bus->type == bus_type::aux);
}

TEST_CASE("a summing bus is built as a folder, with the Keeper's deltas", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.create_bus.type = bus_type::summing;
	fixture.requests.create_bus.bus_name = "Drums";

	const test_outcome outcome = fixture.execute("create_bus", {guid_for('a'), guid_for('b')});

	CHECK(success_of(outcome).fields.label == "create_bus");

	REQUIRE(fixture.recorded.create_bus.has_value());
	const create_bus_result& result = *fixture.recorded.create_bus;

	CHECK(result.type == bus_type::summing);
	CHECK(result.bus_track.name == "Drums");

	// A folder parent must precede its children, so the bus lands immediately above the
	// first source.
	CHECK(result.index == 0);

	// A summing bus carries signal through folder summing, so it appears in no send
	// enumeration at all.
	CHECK(result.sends.empty());

	// The structure REAPER ends up with: the bus opens a folder, the last source closes
	// it, and the deltas sum to zero.
	const std::vector<routing_track>& tracks = fixture.host.snapshot.tracks_in_project_order;

	REQUIRE(tracks.size() == 4);
	CHECK(tracks[0].name == "Drums");
	CHECK(tracks[0].folder_depth_delta == 1);
	CHECK(tracks[1].name == "Kick");
	CHECK(tracks[1].folder_depth_delta == 0);
	CHECK(tracks[2].name == "Snare");
	CHECK(tracks[2].folder_depth_delta == -1);
	CHECK(tracks[3].name == "Bass");
	CHECK(tracks[3].folder_depth_delta == 0);

	// And the delta the call did not name is reported as a repair, so a structural side
	// effect is something the agent can tell the producer about.
	REQUIRE(result.folder_depth_repairs.size() == 1);
	CHECK(result.folder_depth_repairs[0].name == "Snare");
	CHECK(result.folder_depth_repairs[0].previous_folder_depth == 0);
	CHECK(result.folder_depth_repairs[0].folder_depth == -1);
}

TEST_CASE("a summing bus gathers sources that were not next to each other", "[routing_tools]")
{
	routing_snapshot snapshot;
	snapshot.readable = true;
	snapshot.tracks_in_project_order.push_back(track_of("Kick", 'a', 0, true));
	snapshot.tracks_in_project_order.push_back(track_of("Lead Vox", 'v', 0, true));
	snapshot.tracks_in_project_order.push_back(track_of("Snare", 'b', 0, true));
	snapshot.master_track.present = true;
	snapshot.master_track.guid = guid_for('m');
	snapshot.master_track.name = "MASTER";

	routing_fixture fixture{std::move(snapshot)};

	fixture.requests.create_bus.type = bus_type::summing;
	fixture.requests.create_bus.bus_name = "Drums";

	const test_outcome outcome = fixture.execute("create_bus", {guid_for('a'), guid_for('b')});

	CHECK(success_of(outcome).fields.label == "create_bus");

	// A folder is a position, not a track type, so the sources have to end up
	// contiguous beneath the parent — an untouched track cannot be left in the middle of
	// the folder.
	const std::vector<routing_track>& tracks = fixture.host.snapshot.tracks_in_project_order;

	REQUIRE(tracks.size() == 4);
	CHECK(tracks[0].name == "Drums");
	CHECK(tracks[1].name == "Kick");
	CHECK(tracks[2].name == "Snare");
	CHECK(tracks[3].name == "Lead Vox");
	CHECK(fixture.host.track_order_applications.size() == 1);

	// Lead Vox rose out of nothing and sits at top level, unchanged.
	CHECK(tracks[3].folder_depth_delta == 0);
}

TEST_CASE("an aux bus is a return track fed by one send per source", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.create_bus.type = bus_type::aux;
	fixture.requests.create_bus.bus_name = "Plate Verb";
	fixture.requests.create_bus.fx_names.push_back("ReaVerbate");
	fixture.requests.create_bus.send_volume_decibels = -12.0;

	const test_outcome outcome = fixture.execute("create_bus", {guid_for('a'), guid_for('b')});

	CHECK(success_of(outcome).fields.label == "create_bus");

	REQUIRE(fixture.recorded.create_bus.has_value());
	const create_bus_result& result = *fixture.recorded.create_bus;

	CHECK(result.type == bus_type::aux);
	REQUIRE(result.sends.size() == 2);
	CHECK(result.sends[0].source_track_guid == guid_for('a'));
	CHECK(result.sends[0].send_index == 0);
	CHECK(result.sends[0].volume_decibels == -12.0);
	CHECK(result.sends[1].source_track_guid == guid_for('b'));

	// The return is a track, not a folder, so it disturbs no structure.
	CHECK(result.folder_depth_repairs.empty());
	CHECK(fixture.host.folder_depth_writes.empty());

	REQUIRE(result.fx.size() == 1);
	CHECK(result.fx[0].name == "ReaVerbate");

	// And the sends really exist on the sources, which is the only mechanism an aux bus
	// carries signal by.
	const std::vector<routing_track>& tracks = fixture.host.snapshot.tracks_in_project_order;

	REQUIRE(tracks[0].sends.size() == 1);
	CHECK(tracks[0].sends[0].track_name == "Plate Verb");
	CHECK(tracks[0].sends[0].volume_decibels == -12.0);
}

TEST_CASE("an aux bus keeps the sends that landed when a sibling send fails", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.create_bus.type = bus_type::aux;
	fixture.requests.create_bus.bus_name = "Verb";
	fixture.host.tracks_refusing_send_creation.push_back(guid_for('b'));

	const test_outcome outcome =
		fixture.execute("create_bus", {guid_for('a'), guid_for('b'), guid_for('c')});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// One outcome for the bus track plus one per source.
	REQUIRE(partial.actions.size() == 4);
	CHECK(count_failed_actions(partial.actions) == 1);

	// Requirement 9.4: nothing rolled back. The bus exists and the two sends that
	// REAPER accepted are still on their tracks.
	CHECK(fixture.host.snapshot.tracks_in_project_order.size() == 4);
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].sends.size() == 1);
	CHECK(fixture.host.snapshot.tracks_in_project_order[1].sends.empty());
	CHECK(fixture.host.snapshot.tracks_in_project_order[2].sends.size() == 1);
	CHECK(fixture.stack.begin_block_call_count == 1);
	CHECK(fixture.stack.undo_call_count == 0);
}

// ---------------------------------------------------------------------------
// remove_bus — the same split, dissolution included
// ---------------------------------------------------------------------------

TEST_CASE("remove_bus dissolves a summing folder and reports who rose out of it", "[routing_tools]")
{
	routing_fixture fixture{folder_session()};

	const test_outcome outcome = fixture.execute("remove_bus", {guid_for('d')});

	CHECK(success_of(outcome).fields.label == "remove_bus");

	REQUIRE(fixture.recorded.remove_bus.has_value());
	const remove_bus_result& result = *fixture.recorded.remove_bus;

	// The type is derived from what was found rather than taken from the call.
	CHECK(result.type == bus_type::summing);
	CHECK(result.removed_bus.name == "Drums");
	REQUIRE(result.reparented_tracks.size() == 2);
	CHECK(result.reparented_tracks[0].name == "Kick");
	CHECK(result.reparented_tracks[1].name == "Snare");
	CHECK(result.removed_sends.empty());

	// Requirement 11.3: the former children rise to the parent's own depth, and no
	// child is promoted into the parent's folder role.
	const std::vector<routing_track>& tracks = fixture.host.snapshot.tracks_in_project_order;

	REQUIRE(tracks.size() == 2);
	CHECK(tracks[0].name == "Kick");
	CHECK(tracks[0].folder_depth_delta == 0);
	CHECK(tracks[1].name == "Snare");
	CHECK(tracks[1].folder_depth_delta == 0);

	REQUIRE(result.folder_depth_repairs.size() == 1);
	CHECK(result.folder_depth_repairs[0].name == "Snare");
	CHECK(result.folder_depth_repairs[0].previous_folder_depth == -1);
	CHECK(result.folder_depth_repairs[0].folder_depth == 0);
}

TEST_CASE("remove_bus deletes an aux return and names what stopped being wet", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();

	routing_track verb = track_of("Plate Verb", 'v', 0, true);
	verb.item_count = 0;
	verb.fx_count = 2;
	snapshot.tracks_in_project_order.push_back(verb);

	wire_send(snapshot, guid_for('a'), guid_for('v'));
	wire_send(snapshot, guid_for('b'), guid_for('v'));

	routing_fixture fixture{std::move(snapshot)};

	const test_outcome outcome = fixture.execute("remove_bus", {guid_for('v')});

	CHECK(success_of(outcome).fields.label == "remove_bus");

	REQUIRE(fixture.recorded.remove_bus.has_value());
	const remove_bus_result& result = *fixture.recorded.remove_bus;

	CHECK(result.type == bus_type::aux);
	CHECK(result.removed_fx_count == 2);
	CHECK(result.reparented_tracks.empty());
	REQUIRE(result.removed_sends.size() == 2);
	CHECK(result.removed_sends[0].name == "Kick");
	CHECK(result.removed_sends[1].name == "Snare");

	// An aux return is not a folder, so nothing structural was touched.
	CHECK(result.folder_depth_repairs.empty());
	CHECK(fixture.host.folder_depth_writes.empty());
}

TEST_CASE("remove_bus on a plain track is a failed action, not a refusal", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	const test_outcome outcome = fixture.execute("remove_bus", {guid_for('a')});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);

	const sesh_ai::daw::action_failed& failed =
		std::get<sesh_ai::daw::action_failed>(partial.actions[0]);

	CHECK(failed.error.code() == std::string{not_a_bus_code});

	// Deleting a track because the agent called it a bus is not a mistake to make
	// quietly.
	CHECK(fixture.host.deleted_tracks.empty());
	CHECK(fixture.host.snapshot.tracks_in_project_order.size() == 3);
}

// ---------------------------------------------------------------------------
// remove_send and set_send_state
// ---------------------------------------------------------------------------

TEST_CASE("remove_send names the far end and what is left", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();
	wire_send(snapshot, guid_for('a'), guid_for('b'));
	wire_send(snapshot, guid_for('a'), guid_for('c'));

	routing_fixture fixture{std::move(snapshot)};

	fixture.requests.remove_send.send_index = 0;

	const test_outcome outcome = fixture.execute("remove_send", {guid_for('a')});

	CHECK(success_of(outcome).fields.label == "remove_send");

	REQUIRE(fixture.recorded.remove_send.has_value());
	CHECK(fixture.recorded.remove_send->destination_track.name == "Snare");
	CHECK(fixture.recorded.remove_send->send_index == 0);
	CHECK(fixture.recorded.remove_send->remaining_send_count == 1);

	// REAPER renumbers the sends after the removed one, which is why the result says so.
	REQUIRE(fixture.host.snapshot.tracks_in_project_order[0].sends.size() == 1);
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].sends[0].index == 0);
	CHECK(fixture.host.snapshot.tracks_in_project_order[0].sends[0].track_name == "Bass");
}

TEST_CASE("a send index past the end is a failed action carrying a reason", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.requests.remove_send.send_index = 4;

	const test_outcome outcome = fixture.execute("remove_send", {guid_for('a')});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);

	const sesh_ai::daw::action_failed& failed =
		std::get<sesh_ai::daw::action_failed>(partial.actions[0]);

	CHECK(failed.error.code() == std::string{invalid_send_index_code});
	CHECK(failed.error.message().empty() == false);
}

TEST_CASE("set_send_state changes only what the call named", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();
	wire_send(snapshot, guid_for('a'), guid_for('b'));
	snapshot.tracks_in_project_order[0].sends[0].pan_percent = 50.0;

	routing_fixture fixture{std::move(snapshot)};

	fixture.requests.set_send_state.send_index = 0;
	fixture.requests.set_send_state.change.volume_decibels = -3.0;
	fixture.requests.set_send_state.change.muted = true;

	const test_outcome outcome = fixture.execute("set_send_state", {guid_for('a')});

	CHECK(success_of(outcome).fields.label == "set_send_state");

	REQUIRE(fixture.recorded.set_send_state.has_value());
	const set_send_state_result& result = *fixture.recorded.set_send_state;

	CHECK(result.destination_track.name == "Snare");
	CHECK(result.send.volume_decibels == -3.0);
	CHECK(result.send.muted);

	// The pan the producer had is not a property this call named, so it stands.
	CHECK(result.send.pan_percent == 50.0);
}

TEST_CASE("a level outside the schema's range is clamped rather than rejected", "[routing_tools]")
{
	routing_snapshot snapshot = flat_session();
	wire_send(snapshot, guid_for('a'), guid_for('b'));

	routing_fixture fixture{std::move(snapshot)};

	fixture.requests.set_send_state.send_index = 0;
	fixture.requests.set_send_state.change.volume_decibels = 400.0;

	const test_outcome outcome = fixture.execute("set_send_state", {guid_for('a')});

	CHECK(success_of(outcome).fields.label == "set_send_state");

	REQUIRE(fixture.recorded.set_send_state.has_value());

	// The output schemas bound the level at +12 dB, and a payload outside that would
	// fail the extension's own outbound validation rather than the producer's ear.
	CHECK(fixture.recorded.set_send_state->send.volume_decibels == 12.0);
}

// ---------------------------------------------------------------------------
// get_routing — both mechanisms, or the session reads as unrouted
// ---------------------------------------------------------------------------

TEST_CASE("get_routing reports folder summing, which no send enumeration can see", "[routing_tools]")
{
	routing_fixture fixture{folder_session()};

	const test_outcome outcome = fixture.execute("get_routing", {});

	CHECK(success_of(outcome).fields.label == "get_routing");

	// A read tool: no block, no marker (requirement 10.3).
	CHECK(fixture.stack.begin_block_call_count == 0);
	CHECK(success_of(outcome).undo.has_value() == false);
	CHECK(fixture.undo.has_undo_position_marker() == false);

	REQUIRE(fixture.recorded.get_routing.has_value());
	const get_routing_result& result = *fixture.recorded.get_routing;

	REQUIRE(result.tracks.size() == 3);

	// Not one explicit send exists in this session, so a result carrying sends alone
	// would report a fully routed session as unrouted.
	for (const sesh_ai::daw::tools::track_routing_report& report : result.tracks)
	{
		CHECK(report.sends.empty());
		CHECK(report.receives.empty());
	}

	CHECK(result.tracks[0].track.name == "Drums");
	CHECK(result.tracks[0].role == StructuralRole::summing_folder_parent);
	CHECK(result.tracks[0].folder_depth_delta == 1);
	CHECK(result.tracks[0].accumulated_folder_depth == 0);
	CHECK(result.tracks[0].folder_parent_guid.has_value() == false);

	CHECK(result.tracks[1].track.name == "Kick");
	CHECK(result.tracks[1].accumulated_folder_depth == 1);
	REQUIRE(result.tracks[1].folder_parent_guid.has_value());
	CHECK(*result.tracks[1].folder_parent_guid == guid_for('d'));
	CHECK(result.tracks[1].parent_send_enabled);

	CHECK(result.tracks[2].track.name == "Snare");
	CHECK(result.tracks[2].folder_depth_delta == -1);

	// The master is where a stem set is checked against what actually reaches the mix,
	// so it is present when the call read the whole project.
	CHECK(result.master_track.present);
	CHECK(result.master_track.role == StructuralRole::master);
}

TEST_CASE("get_routing reports a silent folder parent as silent", "[routing_tools]")
{
	routing_snapshot snapshot = folder_session();
	snapshot.tracks_in_project_order[1].parent_send_enabled = false;
	snapshot.tracks_in_project_order[2].parent_send_enabled = false;

	routing_fixture fixture{std::move(snapshot)};

	const test_outcome outcome = fixture.execute("get_routing", {});

	CHECK(success_of(outcome).fields.label == "get_routing");

	REQUIRE(fixture.recorded.get_routing.has_value());
	REQUIRE(fixture.recorded.get_routing->tracks.size() == 3);

	// Every child routes around the parent, which looks like a summing bus and sums
	// nothing. Only visible through folder depth plus the parent send flag.
	CHECK(fixture.recorded.get_routing->tracks[0].role == StructuralRole::silent_folder_parent);
}

TEST_CASE("get_routing reads only the tracks the call asked about", "[routing_tools]")
{
	routing_fixture fixture{folder_session()};

	const test_outcome outcome = fixture.execute("get_routing", {guid_for('a')});

	CHECK(success_of(outcome).fields.label == "get_routing");

	REQUIRE(fixture.recorded.get_routing.has_value());
	REQUIRE(fixture.recorded.get_routing->tracks.size() == 1);
	CHECK(fixture.recorded.get_routing->tracks[0].track.name == "Kick");

	// The master is reported where the whole project was read, since that is where a
	// stem set is checked. A partial read is not that.
	CHECK(fixture.recorded.get_routing->master_track.present == false);
}

// ---------------------------------------------------------------------------
// The folder arithmetic, stated on its own
// ---------------------------------------------------------------------------

TEST_CASE("a summing bus layout keeps the deltas well formed", "[routing_tools]")
{
	const routing_snapshot snapshot = folder_session();

	// Bussing two tracks that are already inside a folder: the new bus sits inside
	// Drums, and the closing delta Snare carried has to keep closing Drums as well as
	// the new folder.
	const summing_bus_layout layout =
		plan_summing_bus_layout(snapshot, {guid_for('a'), guid_for('b')}, "(bus)");

	CHECK(layout.insert_index == 1);
	REQUIRE(layout.track_order.size() == 4);
	CHECK(layout.track_order[0] == guid_for('d'));
	CHECK(layout.track_order[1] == "(bus)");
	CHECK(layout.track_order[2] == guid_for('a'));
	CHECK(layout.track_order[3] == guid_for('b'));

	CHECK(sesh_ai::daw::folder_depth_deltas_are_well_formed(layout.depths_after));

	// Two folders are open at Snare, and it is the last track, so it closes both.
	REQUIRE(layout.depths_after.size() == 4);
	CHECK(layout.depths_after[1].folder_depth_delta == 1);
	CHECK(layout.depths_after[3].folder_depth_delta == -2);
}

TEST_CASE("dissolving a folder promotes no child into the parent's role", "[routing_tools]")
{
	const routing_snapshot snapshot = folder_session();

	const summing_bus_dissolution dissolution = plan_summing_bus_dissolution(snapshot, guid_for('d'));

	REQUIRE(dissolution.depths_after.size() == 2);

	// The mistake requirement 11.3 names explicitly: carrying the opening delta onto
	// the first child would make it the new folder parent and silently re-parent its
	// siblings underneath it.
	CHECK(dissolution.depths_after[0].folder_depth_delta == 0);
	CHECK(dissolution.depths_after[1].folder_depth_delta == 0);
	CHECK(sesh_ai::daw::folder_depth_deltas_are_well_formed(dissolution.depths_after));

	REQUIRE(dissolution.reparented_tracks.size() == 2);
}

TEST_CASE("an unreadable project is a failed action rather than an empty session", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	fixture.host.snapshot.readable = false;

	const test_outcome outcome = fixture.execute("get_routing", {});
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	// A snapshot that failed reads as a session with no tracks, and an agent told the
	// session has no routing will act on that.
	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(fixture.recorded.get_routing.has_value() == false);
}

TEST_CASE("a tool whose input could not be read fails rather than guessing", "[routing_tools]")
{
	routing_fixture fixture{flat_session()};

	// A `create_bus` call whose type nobody extracted would otherwise build a folder,
	// because `bus_type` happens to default to summing — a different session than the
	// producer asked for.
	test_call call;
	call.tool_name = "create_bus";
	call.validated_input = nullptr;
	call.track_selectors.push_back(TrackSelector{TrackGuidSelector{guid_for('a')}});

	const test_outcome outcome = fixture.executor->execute(call);
	const tool_partial_outcome<test_payload>& partial = partial_of(outcome);

	REQUIRE(partial.actions.size() == 1);
	CHECK(action_was_applied(partial.actions[0]) == false);
	CHECK(fixture.host.snapshot.tracks_in_project_order.size() == 3);
}

TEST_CASE("find_silenced_folder_parents names only folder parents nothing reaches", "[routing_tools]")
{
	routing_snapshot snapshot = folder_session();

	CHECK(find_silenced_folder_parents(snapshot).empty());

	snapshot.tracks_in_project_order[1].parent_send_enabled = false;

	// One child still feeds it, so it is not silent.
	CHECK(find_silenced_folder_parents(snapshot).empty());

	snapshot.tracks_in_project_order[2].parent_send_enabled = false;

	REQUIRE(find_silenced_folder_parents(snapshot).size() == 1);
	CHECK(find_silenced_folder_parents(snapshot)[0].name == "Drums");
}
