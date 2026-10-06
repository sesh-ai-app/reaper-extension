// Track state tools — `set_track_state`, `list_tracks`, and `get_project_summary`.
//
// These three are the read-and-write pair for everything the project context snapshot
// deliberately leaves out, so a bug here is not a wrong structure the producer can see
// in their track list — it is Sesh saying something untrue about their session, or
// writing a number REAPER reads as a different number. Every test below is built around
// trying to reach one of five states:
//
//   - A fader written in the wrong units, because decibels reached
//     `SetMediaTrackInfo_Value` without being converted. REAPER would read -6 dB as a
//     gain of six, which is +15.6 dB, and nothing in its API refuses that.
//   - A track reported as answering to one name when the producer taught it four.
//     `list_tracks` is the only read path for learned vocabulary in the protocol, so a
//     dropped alias is a name that exists in the project file and is readable by
//     nothing.
//   - An alias reported in `aliasesLearned` that was rejected, or one reported as
//     learned that the track already held. Both tell the producer their vocabulary is
//     something it is not.
//   - `projectSaved` sampled after a write. Every write dirties the project, so a late
//     sample reports "unsaved" for every call that changed anything and says nothing
//     about the session the producer is in — which is requirement 8.10 answered with
//     noise.
//   - A structural role derived from a filtered track list, or from the reported FX
//     chain rather than the FX count. Both produce a role that depends on what the call
//     asked about rather than on the session, and the agent relays the role to the
//     producer as a statement about their own routing.
//
// The role assertions go through `context::derive_structural_role` the same way the
// implementation does, for the reason the structure suite gives for calling the Folder
// Invariant Keeper's own functions: a hand-written classification in the test would be a
// second implementation of the thing under test, and it would agree with the code it was
// written beside rather than with the contract.
//
// The scripted host below is what makes all of this checkable without REAPER. It records
// every write in the order it arrived and can refuse a named property, which is how the
// per-entry outcome reporting of requirements 9.4 and 9.5 is exercised.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/stored_alias_lookup.h>
#include <daw/tools/track_state_tools.h>

using sesh_ai::context::derive_structural_role;
using sesh_ai::context::StructuralRole;

using sesh_ai::daw::action_failed;
using sesh_ai::daw::action_outcome;
using sesh_ai::daw::action_succeeded;
using sesh_ai::daw::action_was_applied;
using sesh_ai::daw::AliasStorage;
using sesh_ai::daw::AliasStore;
using sesh_ai::daw::centre_reaper_pan;
using sesh_ai::daw::dispatch_outcome;
using sesh_ai::daw::encode_alias_list;
using sesh_ai::daw::handler_action_outcomes;
using sesh_ai::daw::LearnedAliasLookup;
using sesh_ai::daw::maximum_alias_length;
using sesh_ai::daw::maximum_aliases_per_track;
using sesh_ai::daw::maximum_reaper_volume;
using sesh_ai::daw::minimum_reaper_volume;
using sesh_ai::daw::pan_percent_from_reaper_pan;
using sesh_ai::daw::reaper_pan_tolerance;
using sesh_ai::daw::reaper_volume_relative_tolerance;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::ResolvedTrack;
using sesh_ai::daw::StoredAliasLookup;
using sesh_ai::daw::tool_call;
using sesh_ai::daw::tool_execution_context;
using sesh_ai::daw::tool_executor_of;
using sesh_ai::daw::tool_handler_registry;
using sesh_ai::daw::tool_registration_outcome;
using sesh_ai::daw::tool_result;
using sesh_ai::daw::tool_success;
using sesh_ai::daw::tool_undo_effect;
using sesh_ai::daw::track_alias_extension_key;
using sesh_ai::daw::TrackHandle;
using sesh_ai::daw::TrackListSource;
using sesh_ai::daw::TrackReference;
using sesh_ai::daw::undo_manager;
using sesh_ai::daw::undo_stack;
using sesh_ai::daw::unity_reaper_volume;
using sesh_ai::daw::volume_decibels_from_reaper_volume;

using sesh_ai::daw::tools::apply_get_project_summary;
using sesh_ai::daw::tools::apply_list_tracks;
using sesh_ai::daw::tools::apply_set_track_state;
using sesh_ai::daw::tools::every_track_state_tool_registered;
using sesh_ai::daw::tools::get_project_summary_request;
using sesh_ai::daw::tools::get_project_summary_tool_name;
using sesh_ai::daw::tools::list_tracks_request;
using sesh_ai::daw::tools::list_tracks_result;
using sesh_ai::daw::tools::list_tracks_tool_name;
using sesh_ai::daw::tools::maximum_reported_fx_chain_names;
using sesh_ai::daw::tools::maximum_reported_track_states;
using sesh_ai::daw::tools::project_summary_reading;
using sesh_ai::daw::tools::project_summary_result;
using sesh_ai::daw::tools::project_summary_unreadable_code;
using sesh_ai::daw::tools::register_track_state_tools;
using sesh_ai::daw::tools::reported_track_state;
using sesh_ai::daw::tools::set_track_state_request;
using sesh_ai::daw::tools::set_track_state_result;
using sesh_ai::daw::tools::set_track_state_tool_name;
using sesh_ai::daw::tools::track_alias_cap_reached_code;
using sesh_ai::daw::tools::track_fx_chain_too_long_to_report_code;
using sesh_ai::daw::tools::track_list_too_long_to_report_code;
using sesh_ai::daw::tools::track_state_change;
using sesh_ai::daw::tools::track_state_host;
using sesh_ai::daw::tools::track_state_host_unavailable_code;
using sesh_ai::daw::tools::track_state_names_no_change_code;
using sesh_ai::daw::tools::track_state_outcome;
using sesh_ai::daw::tools::track_state_reading;
using sesh_ai::daw::tools::track_state_unknown_track_code;
using sesh_ai::daw::tools::track_state_write_failed_code;

namespace
{
	// ---------------------------------------------------------------------------
	// Building a project
	// ---------------------------------------------------------------------------

	// REAPER's braced GUID form, which every output schema's `guid` pattern expects.
	std::string guid_for(int distinguishing_number)
	{
		std::string digits = std::to_string(distinguishing_number);

		while (digits.size() < 12)
		{
			digits.insert(digits.begin(), '0');
		}

		return "{00000000-0000-0000-0000-" + digits + "}";
	}

	track_state_reading track_of(std::string name, int distinguishing_number)
	{
		track_state_reading track;
		track.guid = guid_for(distinguishing_number);
		track.name = std::move(name);

		return track;
	}

	const reported_track_state* reported_track_named(
		const list_tracks_result& result,
		std::string_view name)
	{
		for (const reported_track_state& track : result.tracks)
		{
			if (track.name == name)
			{
				return &track;
			}
		}

		return nullptr;
	}

	// ---------------------------------------------------------------------------
	// The scripted host
	// ---------------------------------------------------------------------------

	// One write, as it arrived. Recorded rather than only applied, because the order the
	// properties are written in is part of the contract — the alias goes last, so an
	// entry whose fader move REAPER refused does not report a name learned.
	struct recorded_write
	{
		std::string property;
		std::string track_guid;
		double numeric_value = 0.0;
		std::string text_value;
	};

	// A project these three tools can be driven against, with no REAPER present.
	//
	// Writes are applied to the held readings as well as recorded, so a read after a
	// write sees what a read after a write would see in REAPER. `refuse_property` is what
	// makes a REAPER refusal reachable: it is the only way to exercise requirement 9.5's
	// "every failure carries a reason" on the write path.
	class scripted_project final : public track_state_host
	{
	public:
		explicit scripted_project(std::vector<track_state_reading> tracks)
			: tracks_{std::move(tracks)}
		{
		}

		bool is_usable() const override { return usable; }

		std::vector<std::string> unresolved_function_names() const override
		{
			return unresolved_names;
		}

		std::vector<track_state_reading> read_tracks_in_project_order(bool include_fx_names) override
		{
			++read_call_count;
			read_calls_asked_for_fx_names.push_back(include_fx_names);

			if (!usable)
			{
				// What an unusable host actually does: reports an empty project. The
				// handlers check `is_usable` first precisely so this never reaches a
				// result.
				return {};
			}

			std::vector<track_state_reading> tracks = tracks_;

			if (!include_fx_names)
			{
				for (track_state_reading& track : tracks)
				{
					track.fx_names.clear();
				}
			}

			return tracks;
		}

		project_summary_reading read_project_summary() override
		{
			++summary_read_call_count;

			if (!usable)
			{
				return project_summary_reading{};
			}

			project_summary_reading reading = summary;
			reading.track_count = static_cast<int>(tracks_.size());

			return reading;
		}

		bool write_track_name(const std::string& track_guid, const std::string& name) override
		{
			return write_text("name", track_guid, name, [&name](track_state_reading& track) {
				track.name = name;
			});
		}

		bool write_track_volume(const std::string& track_guid, double reaper_volume) override
		{
			return write_numeric(
				"volume",
				track_guid,
				reaper_volume,
				[reaper_volume](track_state_reading& track) { track.reaper_volume = reaper_volume; });
		}

		bool write_track_pan(const std::string& track_guid, double reaper_pan) override
		{
			return write_numeric(
				"pan",
				track_guid,
				reaper_pan,
				[reaper_pan](track_state_reading& track) { track.reaper_pan = reaper_pan; });
		}

		bool write_track_muted(const std::string& track_guid, bool muted) override
		{
			return write_numeric("muted", track_guid, muted ? 1.0 : 0.0, [muted](track_state_reading& track) {
				track.muted = muted;
			});
		}

		bool write_track_soloed(const std::string& track_guid, bool soloed) override
		{
			return write_numeric(
				"soloed",
				track_guid,
				soloed ? 1.0 : 0.0,
				[soloed](track_state_reading& track) { track.soloed = soloed; });
		}

		bool write_track_armed(const std::string& track_guid, bool armed) override
		{
			return write_numeric("armed", track_guid, armed ? 1.0 : 0.0, [armed](track_state_reading& track) {
				track.armed = armed;
			});
		}

		bool write_track_color(const std::string& track_guid, int color) override
		{
			return write_numeric(
				"color",
				track_guid,
				static_cast<double>(color),
				[color](track_state_reading& track) { track.color = color; });
		}

		const std::vector<track_state_reading>& tracks() const { return tracks_; }

		const track_state_reading* track_with_guid(const std::string& guid) const
		{
			for (const track_state_reading& track : tracks_)
			{
				if (track.guid == guid)
				{
					return &track;
				}
			}

			return nullptr;
		}

		// A property REAPER will not accept, by name as the recorder names it.
		std::string refuse_property;

		bool usable = true;
		std::vector<std::string> unresolved_names;

		project_summary_reading summary;

		std::vector<recorded_write> writes;
		int read_call_count = 0;
		int summary_read_call_count = 0;
		std::vector<bool> read_calls_asked_for_fx_names;

		// Fired by every write, so the test can model what REAPER does: changing
		// anything leaves the project dirty.
		std::function<void()> on_write;

	private:
		template <typename Apply>
		bool write_numeric(
			std::string property,
			const std::string& track_guid,
			double value,
			const Apply& apply)
		{
			if (property == refuse_property)
			{
				return false;
			}

			track_state_reading* const track = mutable_track_with_guid(track_guid);

			if (track == nullptr)
			{
				return false;
			}

			writes.push_back(recorded_write{std::move(property), track_guid, value, {}});
			apply(*track);
			note_write();

			return true;
		}

		template <typename Apply>
		bool write_text(
			std::string property,
			const std::string& track_guid,
			const std::string& value,
			const Apply& apply)
		{
			if (property == refuse_property)
			{
				return false;
			}

			track_state_reading* const track = mutable_track_with_guid(track_guid);

			if (track == nullptr)
			{
				return false;
			}

			writes.push_back(recorded_write{std::move(property), track_guid, 0.0, value});
			apply(*track);
			note_write();

			return true;
		}

		void note_write()
		{
			if (on_write)
			{
				on_write();
			}
		}

		track_state_reading* mutable_track_with_guid(const std::string& guid)
		{
			for (track_state_reading& track : tracks_)
			{
				if (track.guid == guid)
				{
					return &track;
				}
			}

			return nullptr;
		}

		std::vector<track_state_reading> tracks_;
	};

	// ---------------------------------------------------------------------------
	// The alias store, on a fake project file
	// ---------------------------------------------------------------------------

	// `AliasStorage` over a map, so the store's own accumulation, cap, and save-state
	// reporting are exercised rather than stubbed. Handles are the addresses of records
	// this object owns, which is what lets a `track_state_reading::track` point at one.
	class fake_alias_storage final : public AliasStorage
	{
	public:
		explicit fake_alias_storage(std::size_t track_count)
		{
			for (std::size_t index = 0; index < track_count; ++index)
			{
				records_.push_back(std::make_unique<record>());
			}
		}

		TrackHandle handle_for(std::size_t index) const
		{
			return static_cast<TrackHandle>(records_.at(index).get());
		}

		int track_count() const override { return static_cast<int>(records_.size()); }

		TrackHandle track_at(int index) const override
		{
			if (index < 0 || index >= track_count())
			{
				return nullptr;
			}

			return handle_for(static_cast<std::size_t>(index));
		}

		std::string track_extension_value(TrackHandle track, const std::string& key) const override
		{
			const record& held = *static_cast<const record*>(track);
			const auto found = held.values.find(key);

			return found == held.values.end() ? std::string{} : found->second;
		}

		bool set_track_extension_value(
			TrackHandle track,
			const std::string& key,
			const std::string& value) override
		{
			if (refuse_writes)
			{
				return false;
			}

			static_cast<record*>(track)->values[key] = value;
			project_has_unsaved_changes_ = true;

			return true;
		}

		std::string project_extension_value(const std::string&, const std::string&) const override
		{
			return {};
		}

		bool set_project_extension_value(
			const std::string&,
			const std::string&,
			const std::string&) override
		{
			return !refuse_writes;
		}

		bool project_has_been_saved() const override { return project_has_been_saved_; }

		bool project_has_unsaved_changes() const override { return project_has_unsaved_changes_; }

		void set_saved_with_no_pending_edits()
		{
			project_has_been_saved_ = true;
			project_has_unsaved_changes_ = false;
		}

		void mark_dirty() { project_has_unsaved_changes_ = true; }

		bool refuse_writes = false;

	private:
		struct record
		{
			std::map<std::string, std::string> values;
		};

		std::vector<std::unique_ptr<record>> records_;
		bool project_has_been_saved_ = false;
		bool project_has_unsaved_changes_ = false;
	};

	// The handle a reading carries, for a project whose tracks line up with the fake
	// storage's records.
	//
	// `TrackHandle` is `void*` and `track_state_reading::track` is `MediaTrack*`, which is
	// exactly the arrangement `stored_alias_lookup.h` describes: the store keys on
	// REAPER's handle, and nothing in the logic dereferences it.
	void attach_alias_handles(
		std::vector<track_state_reading>& tracks,
		const fake_alias_storage& storage)
	{
		for (std::size_t index = 0; index < tracks.size(); ++index)
		{
			tracks[index].track = static_cast<MediaTrack*>(storage.handle_for(index));
		}
	}

	// A lookup that answers with nothing, for the tests that are not about vocabulary.
	class no_learned_aliases final : public LearnedAliasLookup
	{
	public:
		std::vector<std::string> learned_aliases_for_track(const ResolvableTrack&) const override
		{
			return {};
		}
	};

	// ---------------------------------------------------------------------------
	// Deterministic generation, for the round-trip property
	// ---------------------------------------------------------------------------

	class deterministic_numbers
	{
	public:
		explicit deterministic_numbers(std::uint32_t seed)
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

		// A double in [0, 1].
		double unit_fraction() { return static_cast<double>(next() % 1000001u) / 1000000.0; }

	private:
		std::uint32_t state_;
	};

	// ---------------------------------------------------------------------------
	// The framework harness
	// ---------------------------------------------------------------------------

	// The payload type the framework is templated on. A plain struct, so the whole suite
	// runs with no JSON library present.
	struct test_payload
	{
		std::string description;

		// Enough of the result to check that `set_track_state`'s payload is its own
		// rather than the framework's partial.
		std::size_t action_count = 0;
		std::size_t aliases_learned_count = 0;
		bool carries_project_saved = false;
	};

	using test_registry = tool_handler_registry<test_payload>;

	// Reads a request out of the validated input. The framework holds the input as an
	// opaque pointer, so the suite binds the payload type to something it can read
	// directly.
	struct test_request_reader
	{
		bool operator()(const test_payload&, set_track_state_request& request) const
		{
			request = set_track_state;

			return readable;
		}

		bool operator()(const test_payload&, list_tracks_request& request) const
		{
			request = list_tracks;

			return readable;
		}

		bool operator()(const test_payload&, get_project_summary_request&) const { return readable; }

		set_track_state_request set_track_state;
		list_tracks_request list_tracks;
		bool readable = true;
	};

	struct test_result_writer
	{
		test_payload operator()(const set_track_state_result& result) const
		{
			return test_payload{
				"set_track_state",
				result.actions.size(),
				result.aliases_learned.size(),
				result.project_saved.has_value()};
		}

		test_payload operator()(const list_tracks_result& result) const
		{
			return test_payload{"list_tracks", result.tracks.size(), 0, false};
		}

		test_payload operator()(const project_summary_result& result) const
		{
			return test_payload{"get_project_summary " + result.project_name, 0, 0, false};
		}
	};

	// A scripted REAPER undo stack, counting everything the framework does to it.
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
		}

		void end_block(const std::string& description, int) override
		{
			++end_block_call_count;
			--open_block_depth;

			entries_.push_back(description);
			position_ = static_cast<int>(entries_.size());
		}

		bool undo_one_entry() override
		{
			if (position_ <= 0)
			{
				return false;
			}

			--position_;

			return true;
		}

		int begin_block_call_count = 0;
		int end_block_call_count = 0;
		int open_block_depth = 0;

	private:
		std::vector<std::string> entries_;
		int position_ = 0;
	};

	class empty_track_list final : public TrackListSource
	{
	public:
		std::vector<ResolvableTrack> tracks_in_project_order() const override { return {}; }
	};

	// ---------------------------------------------------------------------------
	// Assertions
	// ---------------------------------------------------------------------------

	const action_failed* failed_outcome(const std::vector<action_outcome>& actions, std::size_t index)
	{
		if (index >= actions.size())
		{
			return nullptr;
		}

		return std::get_if<action_failed>(&actions[index]);
	}

	// Requirement 9.5. A failure that names no reason is not representable in the type,
	// so this checks the reason is also not empty in practice.
	void require_every_failure_carries_a_reason(const std::vector<action_outcome>& actions)
	{
		for (std::size_t index = 0; index < actions.size(); ++index)
		{
			const action_failed* const failure = std::get_if<action_failed>(&actions[index]);

			if (failure == nullptr)
			{
				continue;
			}

			INFO("action " << index);
			CHECK_FALSE(failure->error.code().empty());
			CHECK_FALSE(failure->error.message().empty());
		}
	}
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

TEST_CASE("list_tracks reports the fader in decibels and the pan as a percentage", "[daw][tools][track-state][units]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	// Half the amplitude, which is a little over 6 dB down, and 30% left.
	tracks[0].reaper_volume = 0.5;
	tracks[0].reaper_pan = -0.3;

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, no_aliases, list_tracks_request{});

	const auto* const result = std::get_if<list_tracks_result>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE(result->tracks.size() == 1);

	// Not REAPER's stored values. A result carrying 0.5 as the volume would be read by
	// the agent as half a decibel down.
	CHECK(result->tracks[0].volume_decibels < -6.0);
	CHECK(result->tracks[0].volume_decibels > -6.1);
	CHECK(result->tracks[0].pan_percent == -30.0);
}

TEST_CASE("set_track_state converts decibels before REAPER sees them", "[daw][tools][track-state][units]")
{
	std::vector<track_state_reading> tracks{track_of("Vocal", 1)};
	scripted_project project{tracks};

	fake_alias_storage storage{1};
	AliasStore store{storage};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.volume_decibels = -6.0;
	change.pan_percent = 50.0;
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.every_change_landed());
	REQUIRE(project.writes.size() == 2);

	// The gain factor paired with -6 dB, not the number -6. Writing the decibels
	// straight through would have REAPER read this as a gain of six, which is +15.6 dB —
	// so "bring the vocal down a touch" would raise it by a factor of six.
	CHECK(project.writes[0].property == "volume");
	CHECK(project.writes[0].numeric_value > 0.50);
	CHECK(project.writes[0].numeric_value < 0.51);

	CHECK(project.writes[1].property == "pan");
	CHECK(project.writes[1].numeric_value == 0.5);
}

TEST_CASE("a level read by list_tracks writes back to the same stored value", "[daw][tools][track-state][units][property]")
{
	// The round trip the output schema promises in as many words: "a level read back can
	// be written back". Quantified over REAPER's representable range, which is what the
	// conversion's own documented bounds mean.
	deterministic_numbers numbers{0x5e5417a7u};

	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	no_learned_aliases no_aliases;

	for (int iteration = 0; iteration < 500; ++iteration)
	{
		// Logarithmic in gain, so the sample is spread across the decibel range the
		// schema reports rather than crowded at the top of the linear one.
		const double decibels_in_range = -150.0 + numbers.unit_fraction() * 162.0;
		const double stored_volume = std::pow(10.0, decibels_in_range / 20.0);
		const double stored_pan = -1.0 + numbers.unit_fraction() * 2.0;

		tracks[0].reaper_volume = stored_volume;
		tracks[0].reaper_pan = stored_pan;

		scripted_project project{tracks};

		const track_state_outcome<list_tracks_result> read =
			apply_list_tracks(project, no_aliases, list_tracks_request{});
		const auto* const reported = std::get_if<list_tracks_result>(&read);

		REQUIRE(reported != nullptr);
		REQUIRE(reported->tracks.size() == 1);

		fake_alias_storage storage{1};
		AliasStore store{storage};

		set_track_state_request request;
		track_state_change change;
		change.track_guid = tracks[0].guid;
		change.volume_decibels = reported->tracks[0].volume_decibels;
		change.pan_percent = reported->tracks[0].pan_percent;
		request.changes.push_back(change);

		const set_track_state_result written = apply_set_track_state(project, store, request);

		REQUIRE(written.every_change_landed());

		const track_state_reading* const after = project.track_with_guid(tracks[0].guid);

		REQUIRE(after != nullptr);

		INFO("stored volume " << stored_volume << " reported as "
			<< reported->tracks[0].volume_decibels << " dB, written back as "
			<< after->reaper_volume);

		const double volume_difference = std::abs(after->reaper_volume - stored_volume);

		CHECK(volume_difference
			<= reaper_volume_relative_tolerance * std::abs(stored_volume) + minimum_reaper_volume);
		CHECK(std::abs(after->reaper_pan - stored_pan) <= reaper_pan_tolerance);
	}
}

// ---------------------------------------------------------------------------
// Learned vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("list_tracks reports every alias a track answers to, in the order they were taught", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1), track_of("Snare", 2)};

	fake_alias_storage storage{2};
	attach_alias_handles(tracks, storage);

	AliasStore store{storage};

	// The case the schema's `aliases` array exists for: a producer who has been talking
	// about the same kick for months.
	REQUIRE(store.learn_track_alias(storage.handle_for(0), "the kick").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(0), "kik").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(0), "bd").learned());

	scripted_project project{tracks};
	StoredAliasLookup lookup{store};

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, lookup, list_tracks_request{});
	const auto* const result = std::get_if<list_tracks_result>(&outcome);

	REQUIRE(result != nullptr);

	const reported_track_state* const kick = reported_track_named(*result, "Kick");

	REQUIRE(kick != nullptr);

	// All three, in insertion order. Reporting only the first would leave two names in
	// the producer's project file that nothing in the protocol can read.
	CHECK(kick->aliases == std::vector<std::string>{"the kick", "kik", "bd"});

	const reported_track_state* const snare = reported_track_named(*result, "Snare");

	REQUIRE(snare != nullptr);
	CHECK(snare->aliases.empty());
}

TEST_CASE("a stored alias value past the cap is bounded to what the result can carry", "[daw][tools][track-state][aliases]")
{
	// `decode_alias_list` reports a hand-edited value as it actually is rather than
	// trimming it, because trimming in the store would be the silent dropping the cap
	// exists to refuse. So an over-cap stored value is a real input, and this read is
	// where it gets bounded: `list-tracks.schema.json` caps `aliases` at
	// `maximum_aliases_per_track` entries of `maximum_alias_length` bytes, and a value
	// somebody edited by hand has to answer rather than fail the outbound validation.
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	// Four past the cap, with the first alias also longer than a stored one can be.
	// Neither is reachable through `learn_track_alias`, which refuses both — this is a
	// `.rpp` somebody edited, a ReaScript that wrote the key itself, or an older build.
	std::vector<std::string> planted;
	planted.push_back(std::string(maximum_alias_length + 8, 'a'));

	for (std::size_t index = 1; index < maximum_aliases_per_track + 4; ++index)
	{
		planted.push_back("name " + std::to_string(index));
	}

	REQUIRE(storage.set_track_extension_value(
		storage.handle_for(0),
		track_alias_extension_key,
		encode_alias_list(planted)));

	AliasStore store{storage};

	// The store hands over all twenty, which is the input this read has to bound.
	REQUIRE(store.track_aliases(storage.handle_for(0)).size() == maximum_aliases_per_track + 4);

	scripted_project project{tracks};
	StoredAliasLookup lookup{store};

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, lookup, list_tracks_request{});
	const auto* const result = std::get_if<list_tracks_result>(&outcome);

	REQUIRE(result != nullptr);

	const reported_track_state* const kick = reported_track_named(*result, "Kick");

	REQUIRE(kick != nullptr);

	// Exactly the cap, and the names kept are the ones taught first. The schema's
	// `maxItems` met by reporting the oldest vocabulary rather than an arbitrary sixteen,
	// so the same project read twice reports the same sixteen.
	REQUIRE(kick->aliases.size() == maximum_aliases_per_track);

	std::vector<std::string> expected(
		planted.begin(),
		planted.begin() + static_cast<std::ptrdiff_t>(maximum_aliases_per_track));

	// And the schema's `maxLength` on each survivor, which the store refuses at learn
	// time and cannot refuse for a value it did not write.
	expected[0] = expected[0].substr(0, maximum_alias_length);

	CHECK(kick->aliases == expected);
	CHECK(kick->aliases.front().size() == maximum_alias_length);
}

TEST_CASE("an alias past the cap is a failed action and is not reported as learned", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	AliasStore store{storage};

	for (std::size_t index = 0; index < maximum_aliases_per_track; ++index)
	{
		REQUIRE(store.learn_track_alias(storage.handle_for(0), "name " + std::to_string(index))
			.learned());
	}

	scripted_project project{tracks};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.alias = "one more";
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.actions.size() == 1);

	const action_failed* const failure = failed_outcome(result.actions, 0);

	REQUIRE(failure != nullptr);
	CHECK(failure->error.code() == track_alias_cap_reached_code);

	// The store's own reason names the cap, so the producer can be told which of their
	// names to give up rather than having one chosen for them.
	CHECK(failure->error.message().find(std::to_string(maximum_aliases_per_track))
		!= std::string::npos);

	// Nothing learned, so nothing claimed: an alias reported here would tell the producer
	// their track answers to a name it does not.
	CHECK(result.aliases_learned.empty());
	CHECK_FALSE(result.project_saved.has_value());

	// And requirement 8.6: nothing was dropped to make room.
	CHECK(store.track_aliases(storage.handle_for(0)).size() == maximum_aliases_per_track);
}

TEST_CASE("an alias the track already holds is applied and learns nothing", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	AliasStore store{storage};

	REQUIRE(store.learn_track_alias(storage.handle_for(0), "the kick").learned());

	scripted_project project{tracks};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;

	// Same alias, said differently. The store matches on the normalised form, so this is
	// the name the track already answers to.
	change.alias = "The  Kick";
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	// Applied: the track answers to the name, which is what was asked for.
	REQUIRE(result.actions.size() == 1);
	CHECK(action_was_applied(result.actions[0]));

	// And absent from `aliasesLearned`, because nothing was written. Reporting it would
	// tell the producer a name was added that was not.
	CHECK(result.aliases_learned.empty());
	CHECK_FALSE(result.project_saved.has_value());
	CHECK(store.track_aliases(storage.handle_for(0)) == std::vector<std::string>{"the kick"});
}

TEST_CASE("projectSaved describes the session before the call changed anything", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	// A saved session with nothing pending, which is the state requirement 8.10 is about
	// distinguishing from an untitled one.
	storage.set_saved_with_no_pending_edits();

	AliasStore store{storage};
	scripted_project project{tracks};

	// What REAPER does: any change leaves the project dirty. So does writing the alias.
	project.on_write = [&storage]() { storage.mark_dirty(); };

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.volume_decibels = -3.0;
	change.alias = "the kick";
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.every_change_landed());
	REQUIRE(result.aliases_learned.size() == 1);
	CHECK(result.aliases_learned[0] == TrackReference{tracks[0].guid, "Kick"});

	// True, because the session the producer is in was saved. Sampled after the fader
	// write — or after the alias write, which dirties the project too — this would be
	// false, and the agent would tell the producer their name is about to be lost when it
	// is not.
	REQUIRE(result.project_saved.has_value());
	CHECK(*result.project_saved);
}

TEST_CASE("an unsaved session is reported as one", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	AliasStore store{storage};
	scripted_project project{tracks};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.alias = "the kick";
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.every_change_landed());
	REQUIRE(result.project_saved.has_value());

	// The thing to say to the producer, rather than reporting the alias as remembered.
	CHECK_FALSE(*result.project_saved);
}

TEST_CASE("the alias is written after the state changes it accompanies", "[daw][tools][track-state][aliases]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};

	fake_alias_storage storage{1};
	attach_alias_handles(tracks, storage);

	AliasStore store{storage};
	scripted_project project{tracks};

	// REAPER refuses the fader move, so the entry fails and nothing after it is
	// attempted.
	project.refuse_property = "volume";

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.volume_decibels = -3.0;
	change.alias = "the kick";
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.actions.size() == 1);

	const action_failed* const failure = failed_outcome(result.actions, 0);

	REQUIRE(failure != nullptr);
	CHECK(failure->error.code() == track_state_write_failed_code);

	// No alias learned beside a failed action: the result would otherwise say the
	// producer's name was remembered for a change that did not land.
	CHECK(result.aliases_learned.empty());
	CHECK(store.track_aliases(storage.handle_for(0)).empty());
}

// ---------------------------------------------------------------------------
// Per-entry outcomes
// ---------------------------------------------------------------------------

TEST_CASE("a sibling's failure leaves what landed alone, and every failure says why", "[daw][tools][track-state][outcomes]")
{
	std::vector<track_state_reading> tracks{
		track_of("Kick", 1),
		track_of("Snare", 2),
		track_of("Bass", 3)};

	scripted_project project{tracks};
	fake_alias_storage storage{3};
	AliasStore store{storage};

	// The middle entry asks for the one property REAPER will not accept.
	project.refuse_property = "muted";

	set_track_state_request request;

	track_state_change quieter;
	quieter.track_guid = tracks[0].guid;
	quieter.volume_decibels = -3.0;
	request.changes.push_back(quieter);

	track_state_change silenced;
	silenced.track_guid = tracks[1].guid;
	silenced.muted = true;
	request.changes.push_back(silenced);

	track_state_change renamed;
	renamed.track_guid = tracks[2].guid;
	renamed.name = "Sub Bass";
	request.changes.push_back(renamed);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	// One outcome per entry, in the order the call supplied them (requirement 9.4).
	REQUIRE(result.actions.size() == 3);
	CHECK(action_was_applied(result.actions[0]));
	CHECK_FALSE(action_was_applied(result.actions[1]));
	CHECK(action_was_applied(result.actions[2]));

	require_every_failure_carries_a_reason(result.actions);

	// And what succeeded is kept: the sibling's failure rolls nothing back.
	const track_state_reading* const kick = project.track_with_guid(tracks[0].guid);
	const track_state_reading* const bass = project.track_with_guid(tracks[2].guid);

	REQUIRE(kick != nullptr);
	REQUIRE(bass != nullptr);
	CHECK(kick->reaper_volume < unity_reaper_volume);
	CHECK(bass->name == "Sub Bass");
}

TEST_CASE("an entry naming a track and nothing to change is a failed action", "[daw][tools][track-state][outcomes]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};
	fake_alias_storage storage{1};
	AliasStore store{storage};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.actions.size() == 1);

	const action_failed* const failure = failed_outcome(result.actions, 0);

	REQUIRE(failure != nullptr);
	CHECK(failure->error.code() == track_state_names_no_change_code);
	CHECK(project.writes.empty());
}

TEST_CASE("an entry naming a track that is not in the project is a failed action", "[daw][tools][track-state][outcomes]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};
	fake_alias_storage storage{1};
	AliasStore store{storage};

	set_track_state_request request;
	track_state_change change;
	change.track_guid = guid_for(99);
	change.muted = true;
	request.changes.push_back(change);

	const set_track_state_result result = apply_set_track_state(project, store, request);

	REQUIRE(result.actions.size() == 1);

	const action_failed* const failure = failed_outcome(result.actions, 0);

	REQUIRE(failure != nullptr);
	CHECK(failure->error.code() == track_state_unknown_track_code);
	CHECK(project.writes.empty());
}

// ---------------------------------------------------------------------------
// Structural roles
// ---------------------------------------------------------------------------

TEST_CASE("a role is derived from the whole project, not from the tracks the call named", "[daw][tools][track-state][roles]")
{
	// A folder parent whose two children route around it, with nothing of its own. That
	// is `silent_folder_parent`: it looks like a bus in the track list and sums nothing,
	// and telling the producer so is the point of reporting the role at all.
	std::vector<track_state_reading> tracks{
		track_of("Drum Bus", 1),
		track_of("Kick", 2),
		track_of("Snare", 3)};

	tracks[0].folder_depth_delta = 1;
	tracks[1].folder_depth_delta = 0;
	tracks[2].folder_depth_delta = -1;

	tracks[1].parent_send_enabled = false;
	tracks[2].parent_send_enabled = false;
	tracks[1].item_count = 4;
	tracks[2].item_count = 4;

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	list_tracks_request request;
	request.track_guids.push_back(tracks[0].guid);

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, no_aliases, request);
	const auto* const result = std::get_if<list_tracks_result>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE(result->tracks.size() == 1);

	// Derived over the filtered list the call named, the parent would have no children
	// and would come back `normal` — a different statement about the producer's routing,
	// and a reassuring one that is not true.
	CHECK(result->tracks[0].role == StructuralRole::silent_folder_parent);
	CHECK(result->tracks[0].role_signals.child_track_count == 2);

	// The same answer the derivation itself gives for those signals, rather than a
	// second classification written here.
	CHECK(result->tracks[0].role
		== derive_structural_role(result->tracks[0].role_signals).role);

	// And the index is REAPER's track number, not the position in the reported array.
	CHECK(result->tracks[0].index == 0);
}

TEST_CASE("leaving the FX chain out does not change a role", "[daw][tools][track-state][roles]")
{
	// A folder parent with an FX chain and nothing else. FX presence is the conservative
	// half of the silent-versus-summing distinction: a chain of any kind might generate
	// rather than process, so the parent is reported as summing.
	std::vector<track_state_reading> tracks{track_of("Drum Bus", 1), track_of("Kick", 2)};

	tracks[0].folder_depth_delta = 1;
	tracks[0].fx_count = 1;
	tracks[0].fx_names = {"ReaComp"};
	tracks[1].folder_depth_delta = -1;
	tracks[1].parent_send_enabled = false;

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	list_tracks_request with_chain;
	with_chain.include_fx_chain = true;

	list_tracks_request without_chain;
	without_chain.include_fx_chain = false;

	const track_state_outcome<list_tracks_result> reported_with =
		apply_list_tracks(project, no_aliases, with_chain);
	const track_state_outcome<list_tracks_result> reported_without =
		apply_list_tracks(project, no_aliases, without_chain);

	const auto* const with = std::get_if<list_tracks_result>(&reported_with);
	const auto* const without = std::get_if<list_tracks_result>(&reported_without);

	REQUIRE(with != nullptr);
	REQUIRE(without != nullptr);
	REQUIRE(with->tracks.size() == 2);
	REQUIRE(without->tracks.size() == 2);

	// The role comes from the FX count, which is read either way. Taken from the reported
	// chain instead, the second call would classify this bus `silent_folder_parent` and
	// have the agent tell the producer their drum bus carries nothing.
	CHECK(with->tracks[0].role == StructuralRole::summing_folder_parent);
	CHECK(without->tracks[0].role == StructuralRole::summing_folder_parent);

	// Present and empty is a different answer from absent: one says the chain is empty,
	// the other says nothing was looked at.
	REQUIRE(with->tracks[0].fx_chain.has_value());
	CHECK(*with->tracks[0].fx_chain == std::vector<std::string>{"ReaComp"});
	CHECK_FALSE(without->tracks[0].fx_chain.has_value());
}

// ---------------------------------------------------------------------------
// Selection, caps, and refusals
// ---------------------------------------------------------------------------

TEST_CASE("named tracks are reported in the order they were named, once each", "[daw][tools][track-state][selection]")
{
	std::vector<track_state_reading> tracks{
		track_of("Kick", 1),
		track_of("Snare", 2),
		track_of("Bass", 3)};

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	list_tracks_request request;
	request.track_guids.push_back(tracks[2].guid);
	request.track_guids.push_back(tracks[0].guid);

	// Two selectors resolving to one track is an ordinary consequence of learned
	// vocabulary — "kick" and "the kick" are both names for it.
	request.track_guids.push_back(tracks[0].guid);

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, no_aliases, request);
	const auto* const result = std::get_if<list_tracks_result>(&outcome);

	REQUIRE(result != nullptr);
	REQUIRE(result->tracks.size() == 2);
	CHECK(result->tracks[0].name == "Bass");
	CHECK(result->tracks[1].name == "Kick");

	// REAPER's own track numbers, which is what the agent says to a producer looking at
	// the track list.
	CHECK(result->tracks[0].index == 2);
	CHECK(result->tracks[1].index == 0);
}

TEST_CASE("a project too large to report fails rather than being cut", "[daw][tools][track-state][cap]")
{
	// Requirement 23.9. `list-tracks.schema.json` caps `tracks` and carries no
	// `totalInRange` and no `truncated`, so a cut list is indistinguishable from a
	// smaller session — which is the reading the agent would act on.
	std::vector<track_state_reading> tracks;
	tracks.reserve(maximum_reported_track_states + 1);

	for (std::size_t index = 0; index <= maximum_reported_track_states; ++index)
	{
		tracks.push_back(track_of("Track " + std::to_string(index), static_cast<int>(index) + 1));
	}

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, no_aliases, list_tracks_request{});

	const auto* const reason = std::get_if<sesh_ai::daw::action_error>(&outcome);

	REQUIRE(reason != nullptr);
	CHECK(reason->code() == track_list_too_long_to_report_code);

	// The count is named, so the agent can tell the producer what to ask for instead.
	CHECK(reason->message().find(std::to_string(maximum_reported_track_states + 1))
		!= std::string::npos);
}

TEST_CASE("a chain too long to report fails, and leaving it out succeeds", "[daw][tools][track-state][cap]")
{
	// Same reasoning as the track list: `fxChain` has a cap and no way to say it was cut,
	// so a cut chain is indistinguishable from a shorter one. Unlike the track list, the
	// producer's next call has a way out, and the reason names it.
	std::vector<track_state_reading> tracks{track_of("Kick", 1), track_of("Master Bus", 2)};

	for (std::size_t index = 0; index <= maximum_reported_fx_chain_names; ++index)
	{
		tracks[1].fx_names.push_back("ReaEQ " + std::to_string(index));
	}

	tracks[1].fx_count = static_cast<int>(tracks[1].fx_names.size());

	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	const track_state_outcome<list_tracks_result> refused =
		apply_list_tracks(project, no_aliases, list_tracks_request{});
	const auto* const reason = std::get_if<sesh_ai::daw::action_error>(&refused);

	REQUIRE(reason != nullptr);
	CHECK(reason->code() == track_fx_chain_too_long_to_report_code);
	CHECK(reason->message().find(tracks[1].guid) != std::string::npos);

	// And the way out works: every track's state is still readable without the chain.
	list_tracks_request without_chain;
	without_chain.include_fx_chain = false;

	const track_state_outcome<list_tracks_result> allowed =
		apply_list_tracks(project, no_aliases, without_chain);
	const auto* const result = std::get_if<list_tracks_result>(&allowed);

	REQUIRE(result != nullptr);
	REQUIRE(result->tracks.size() == 2);
	CHECK_FALSE(result->tracks[1].fx_chain.has_value());
}

TEST_CASE("a named track that is not in the project fails rather than being skipped", "[daw][tools][track-state][selection]")
{
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};
	no_learned_aliases no_aliases;

	list_tracks_request request;
	request.track_guids.push_back(tracks[0].guid);
	request.track_guids.push_back(guid_for(99));

	const track_state_outcome<list_tracks_result> outcome =
		apply_list_tracks(project, no_aliases, request);

	const auto* const reason = std::get_if<sesh_ai::daw::action_error>(&outcome);

	REQUIRE(reason != nullptr);
	CHECK(reason->code() == track_state_unknown_track_code);
}

TEST_CASE("an unusable host says which functions REAPER did not supply", "[daw][tools][track-state][host]")
{
	// A host missing a function reports an empty project, which would have `list_tracks`
	// tell the producer they have no tracks. `is_usable` exists so that is reportable
	// instead.
	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};
	project.usable = false;
	project.unresolved_names = {"GetMediaTrackInfo_Value"};

	no_learned_aliases no_aliases;
	fake_alias_storage storage{1};
	AliasStore store{storage};

	const track_state_outcome<list_tracks_result> listed =
		apply_list_tracks(project, no_aliases, list_tracks_request{});
	const auto* const list_reason = std::get_if<sesh_ai::daw::action_error>(&listed);

	REQUIRE(list_reason != nullptr);
	CHECK(list_reason->code() == track_state_host_unavailable_code);
	CHECK(list_reason->message().find("GetMediaTrackInfo_Value") != std::string::npos);

	const track_state_outcome<project_summary_result> summarised =
		apply_get_project_summary(project, get_project_summary_request{});
	const auto* const summary_reason = std::get_if<sesh_ai::daw::action_error>(&summarised);

	REQUIRE(summary_reason != nullptr);
	CHECK(summary_reason->code() == track_state_host_unavailable_code);

	set_track_state_request request;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.muted = true;
	request.changes.push_back(change);

	const set_track_state_result written = apply_set_track_state(project, store, request);

	REQUIRE(written.actions.size() == 1);

	const action_failed* const failure = failed_outcome(written.actions, 0);

	REQUIRE(failure != nullptr);
	CHECK(failure->error.code() == track_state_host_unavailable_code);
	CHECK(project.writes.empty());
}

// ---------------------------------------------------------------------------
// get_project_summary
// ---------------------------------------------------------------------------

TEST_CASE("get_project_summary reports the session's globals and its size", "[daw][tools][track-state][summary]")
{
	scripted_project project{{track_of("Kick", 1), track_of("Snare", 2)}};

	project.summary.readable = true;
	project.summary.project_name = "midnight take 3";
	project.summary.tempo = 92.5;
	project.summary.time_signature_numerator = 6;
	project.summary.time_signature_denominator = 8;
	project.summary.sample_rate = 44100;
	project.summary.project_length = 187.25;
	project.summary.marker_count = 4;
	project.summary.region_count = 2;
	project.summary.tempo_change_count = 1;

	const track_state_outcome<project_summary_result> outcome =
		apply_get_project_summary(project, get_project_summary_request{});
	const auto* const result = std::get_if<project_summary_result>(&outcome);

	REQUIRE(result != nullptr);
	CHECK(result->project_name == "midnight take 3");
	CHECK(result->tempo == 92.5);
	CHECK(result->time_signature_numerator == 6);
	CHECK(result->time_signature_denominator == 8);
	CHECK(result->sample_rate == 44100);
	CHECK(result->track_count == 2);
	CHECK(result->project_length == 187.25);
	CHECK(result->marker_count == 4);
	CHECK(result->region_count == 2);
	CHECK(result->tempo_change_count == 1);

	// Project-level only. No per-track read was made, which is requirement 6.2's
	// separation held from this end as well.
	CHECK(project.read_call_count == 0);
}

TEST_CASE("an unreadable project is a failed action rather than a summary of defaults", "[daw][tools][track-state][summary]")
{
	scripted_project project{{}};
	project.summary.readable = false;

	const track_state_outcome<project_summary_result> outcome =
		apply_get_project_summary(project, get_project_summary_request{});
	const auto* const reason = std::get_if<sesh_ai::daw::action_error>(&outcome);

	REQUIRE(reason != nullptr);
	CHECK(reason->code() == project_summary_unreadable_code);
}

// ---------------------------------------------------------------------------
// Registration and dispatch
// ---------------------------------------------------------------------------

TEST_CASE("the three tools register through the seams their undo effects require", "[daw][tools][track-state][registration]")
{
	test_registry registry;
	scripted_project project{{track_of("Kick", 1)}};
	fake_alias_storage storage{1};
	AliasStore store{storage};
	no_learned_aliases no_aliases;

	const auto registrations = register_track_state_tools(
		registry,
		project,
		store,
		no_aliases,
		test_request_reader{},
		test_result_writer{});

	REQUIRE(every_track_state_tool_registered(registrations));
	REQUIRE(registry.registered_tool_count() == 3);

	const auto* const mutating = registry.find_tool(set_track_state_tool_name);

	REQUIRE(mutating != nullptr);
	CHECK(mutating->undo_effect() == tool_undo_effect::undo_block);

	for (const std::string_view& tool_name : {list_tracks_tool_name, get_project_summary_tool_name})
	{
		INFO("tool " << tool_name);
		const auto* const read_tool = registry.find_tool(tool_name);

		REQUIRE(read_tool != nullptr);

		// A read tool, so no block is opened and no marker captured (requirement 10.3).
		CHECK(read_tool->undo_effect() == tool_undo_effect::none);
	}
}

TEST_CASE("set_track_state's payload is always its own result", "[daw][tools][track-state][registration]")
{
	// Its output schema requires `actions` and `aliasesLearned` together, so the
	// framework's partial shape would produce a payload missing a field the schema
	// requires — and the outbound validation would refuse the tool's valid result.
	test_registry registry;

	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};

	// The one property REAPER will not accept, so the entry fails and a handler built on
	// the framework's partial would take that branch.
	project.refuse_property = "muted";

	fake_alias_storage storage{1};
	AliasStore store{storage};
	no_learned_aliases no_aliases;

	test_request_reader reader;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.muted = true;
	reader.set_track_state.changes.push_back(change);

	REQUIRE(every_track_state_tool_registered(register_track_state_tools(
		registry,
		project,
		store,
		no_aliases,
		reader,
		test_result_writer{})));

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	no_learned_aliases executor_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, executor_aliases};

	test_payload input;

	tool_call<test_payload> call;
	call.tool_name = std::string{set_track_state_tool_name};
	call.validated_input = &input;

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	// A success carrying the tool's own payload, not a partial outcome — even though one
	// entry failed.
	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	CHECK(success->fields.description == "set_track_state");
	CHECK(success->fields.action_count == 1);
	CHECK(success->fields.aliases_learned_count == 0);
	CHECK_FALSE(success->fields.carries_project_saved);

	// The only entry failed, so nothing landed and no marker is reported.
	// `set-track-state.schema.json` describes the absence of `undoPositionBefore` as what
	// tells the producer nothing changed, and the framework cannot see inside the payload
	// to work that out (requirement 4.4) — `detail::reported_track_state_result` tells it.
	CHECK_FALSE(success->undo.has_value());

	// One undo block, opened and closed (requirement 10.8). The flag withholds the report,
	// not the block.
	CHECK(stack.begin_block_call_count == 1);
	CHECK(stack.end_block_call_count == 1);
	CHECK(stack.open_block_depth == 0);
}

TEST_CASE("set_track_state reports its undo marker when a change did land", "[daw][tools][track-state][registration]")
{
	// The other half of the case above, so "no marker" is a consequence of nothing landing
	// rather than of this tool never reporting one.
	test_registry registry;

	std::vector<track_state_reading> tracks{track_of("Kick", 1)};
	scripted_project project{tracks};

	fake_alias_storage storage{1};
	AliasStore store{storage};
	no_learned_aliases no_aliases;

	test_request_reader reader;
	track_state_change change;
	change.track_guid = tracks[0].guid;
	change.muted = true;
	reader.set_track_state.changes.push_back(change);

	REQUIRE(every_track_state_tool_registered(register_track_state_tools(
		registry,
		project,
		store,
		no_aliases,
		reader,
		test_result_writer{})));

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	no_learned_aliases executor_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, executor_aliases};

	undo.begin_turn();

	test_payload input;

	tool_call<test_payload> call;
	call.tool_name = std::string{set_track_state_tool_name};
	call.validated_input = &input;

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	CHECK(success->fields.action_count == 1);
	CHECK(success->undo.has_value());
}

TEST_CASE("input the handler cannot read reports no undo marker either", "[daw][tools][track-state][registration]")
{
	// Every outright failure of `set_track_state` — an unusable host, unreadable input, an
	// array over the cap — leaves as a result whose `actions` array holds one failed
	// action, because its output schema requires `actions` and `aliasesLearned` together.
	// So every one of them is a call in which nothing changed, and none may report a
	// marker. Checked through the seam every path shares rather than per path.
	test_registry registry;
	scripted_project project{{track_of("Kick", 1)}};
	fake_alias_storage storage{1};
	AliasStore store{storage};
	no_learned_aliases no_aliases;

	test_request_reader reader;
	reader.readable = false;

	REQUIRE(every_track_state_tool_registered(register_track_state_tools(
		registry,
		project,
		store,
		no_aliases,
		reader,
		test_result_writer{})));

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	no_learned_aliases executor_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, executor_aliases};

	undo.begin_turn();

	test_payload input;

	tool_call<test_payload> call;
	call.tool_name = std::string{set_track_state_tool_name};
	call.validated_input = &input;

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	const auto* const success = std::get_if<tool_success<test_payload>>(result);

	REQUIRE(success != nullptr);
	CHECK(success->fields.action_count == 1);
	CHECK_FALSE(success->undo.has_value());
}

TEST_CASE("input the handler cannot read is reported rather than crashed on", "[daw][tools][track-state][registration]")
{
	test_registry registry;
	scripted_project project{{track_of("Kick", 1)}};
	fake_alias_storage storage{1};
	AliasStore store{storage};
	no_learned_aliases no_aliases;

	test_request_reader reader;
	reader.readable = false;

	REQUIRE(every_track_state_tool_registered(register_track_state_tools(
		registry,
		project,
		store,
		no_aliases,
		reader,
		test_result_writer{})));

	scripted_undo_stack stack;
	undo_manager undo{stack};
	empty_track_list track_list;
	no_learned_aliases executor_aliases;

	tool_executor_of<test_payload> executor{registry, undo, track_list, executor_aliases};

	test_payload input;

	tool_call<test_payload> call;
	call.tool_name = std::string{list_tracks_tool_name};
	call.validated_input = &input;

	const dispatch_outcome<test_payload> outcome = executor.execute(call);
	const auto* const result = std::get_if<tool_result<test_payload>>(&outcome);

	REQUIRE(result != nullptr);

	// A read tool, so nothing was opened and there is no block to have been left open.
	CHECK(stack.begin_block_call_count == 0);

	const auto* const partial =
		std::get_if<sesh_ai::daw::tool_partial_outcome<test_payload>>(result);

	REQUIRE(partial != nullptr);
	require_every_failure_carries_a_reason(partial->actions);
}
