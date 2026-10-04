// Alias Store — learned producer vocabulary in the producer's project file.
//
// The component's two mechanisms fail in different ways, so the suite is built
// around the difference rather than around the methods.
//
// A track's aliases live on the track, and what that buys — surviving a reorder,
// travelling with the project — is checked by doing those things to the fake and
// reading the aliases back, not by asserting that a particular API was called.
//
// A project-state mapping is read as of the last save, so the fake models exactly
// that: a write lands in the live session, a read answers from the last snapshot.
// Requirement 8.10 is the consequence, and the thing under test is that the store
// tells the producer about it at the moment the alias is learned.
//
// Then the accumulation. A track answers to several names, and every way a set can
// be quietly damaged gets its own case: a second learn that overwrites the first, a
// duplicate that becomes a second entry, a cap that drops the oldest to make room,
// a forget-one that takes the others with it. Each of those is a plausible
// implementation and each loses vocabulary the producer cannot know has gone, so
// each is asserted against directly rather than left to follow from a happy path.
//
// And then the encoding, which now has two jobs: make one alias safe for a
// line-oriented, space-delimited `.rpp`, and make several aliases into one value
// under one key. So the fake serialises its state through a line-oriented format and
// parses it back, with a negative control that shows the same round trip destroying
// an unencoded value; and the set round trip is swept over generated vocabulary that
// deliberately includes the separator and the escape character, which are the two
// bytes a join-and-split scheme can be broken by.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <daw/alias_store.h>
#include <daw/object_resolver.h>
#include <daw/stored_alias_lookup.h>

using sesh_ai::daw::alias_separator;
using sesh_ai::daw::AliasStorage;
using sesh_ai::daw::AliasStore;
using sesh_ai::daw::AliasWriteOutcome;
using sesh_ai::daw::decode_alias_list;
using sesh_ai::daw::decode_ext_state_text;
using sesh_ai::daw::encode_alias_list;
using sesh_ai::daw::encode_ext_state_text;
using sesh_ai::daw::ext_state_byte_needs_encoding;
using sesh_ai::daw::LearnedAlias;
using sesh_ai::daw::maximum_alias_length;
using sesh_ai::daw::maximum_aliases_per_track;
using sesh_ai::daw::maximum_stored_alias_value_length;
using sesh_ai::daw::normalize_alias_for_matching;
using sesh_ai::daw::project_extension_section;
using sesh_ai::daw::ResolvableTrack;
using sesh_ai::daw::StoredAliasLookup;
using sesh_ai::daw::track_alias_extension_key;
using sesh_ai::daw::TrackHandle;
using sesh_ai::daw::trim_alias;

namespace {

	// Stands in for the project: tracks that carry extension values, a project
	// extension section, and the save state the store has to report.
	//
	// The one modelling decision that matters is the split between `live_state` and
	// `saved_state`. REAPER's GetProjExtState answers from the last save, so a fake
	// with one map would make requirement 8.10 untestable by construction — the
	// mapping would read back immediately and the loss the producer is being warned
	// about would never happen here.
	class FakeAliasStorage final : public AliasStorage {
	public:
		struct Track {
			std::string name;
			std::map<std::string, std::string> extension_values;
		};

		Track& add_track(std::string name)
		{
			auto track = std::make_unique<Track>();
			track->name = std::move(name);

			tracks_.push_back(std::move(track));

			return *tracks_.back();
		}

		TrackHandle handle_for(int index) const
		{
			return static_cast<TrackHandle>(tracks_.at(static_cast<std::size_t>(index)).get());
		}

		static const Track& track_of(TrackHandle handle)
		{
			return *static_cast<const Track*>(handle);
		}

		void swap_tracks(int first, int second)
		{
			std::swap(tracks_.at(static_cast<std::size_t>(first)), tracks_.at(static_cast<std::size_t>(second)));
		}

		// What saving does: the session's state becomes the state a reader sees, and
		// the project stops being dirty.
		void save_project(std::string file_name = "session.rpp")
		{
			file_name_ = std::move(file_name);
			saved_state_ = live_state_;
			has_unsaved_changes_ = false;
		}

		int track_count() const override { return static_cast<int>(tracks_.size()); }

		TrackHandle track_at(int index) const override
		{
			if (index < 0 || index >= track_count()) {
				return nullptr;
			}

			return handle_for(index);
		}

		std::string track_extension_value(TrackHandle track, const std::string& key) const override
		{
			const Track& record = track_of(track);
			const auto found = record.extension_values.find(key);

			return found == record.extension_values.end() ? std::string{} : found->second;
		}

		bool set_track_extension_value(TrackHandle track, const std::string& key, const std::string& value) override
		{
			keys_written.push_back(key);

			if (refuse_writes) {
				return false;
			}

			Track& record = *static_cast<Track*>(track);

			// REAPER cannot represent an empty extension value distinctly from an absent
			// one, so an empty write erases.
			if (value.empty()) {
				record.extension_values.erase(key);
			} else {
				record.extension_values[key] = value;
			}

			has_unsaved_changes_ = true;

			return true;
		}

		std::string project_extension_value(const std::string& section, const std::string& key) const override
		{
			const auto found = saved_state_.find(section + '\n' + key);

			return found == saved_state_.end() ? std::string{} : found->second;
		}

		bool set_project_extension_value(
			const std::string& section, const std::string& key, const std::string& value) override
		{
			keys_written.push_back(key);

			if (refuse_writes) {
				return false;
			}

			if (value.empty()) {
				live_state_.erase(section + '\n' + key);
			} else {
				live_state_[section + '\n' + key] = value;
			}

			has_unsaved_changes_ = true;

			return true;
		}

		bool project_has_been_saved() const override { return !file_name_.empty(); }
		bool project_has_unsaved_changes() const override { return has_unsaved_changes_; }

		// The live session's project state, keyed section-then-key — what a save would
		// commit. Exposed so a test can assert the section a mapping landed in.
		const std::map<std::string, std::string>& live_state() const { return live_state_; }

		void mark_unsaved_changes(bool unsaved) { has_unsaved_changes_ = unsaved; }
		void set_file_name(std::string file_name) { file_name_ = std::move(file_name); }

		// A project file, as far as this suite needs one: one `key value` line per
		// entry. Line-oriented and space-delimited, which are the two properties of a
		// `.rpp` the encoding has to survive.
		std::string serialise_project_state() const
		{
			std::ostringstream serialised;

			serialised << "<SESH\n";

			for (const auto& entry : live_state_) {
				const std::size_t separator = entry.first.find('\n');
				const std::string key = entry.first.substr(separator + 1);

				serialised << "  " << key << ' ' << entry.second << '\n';
			}

			serialised << ">\n";

			return serialised.str();
		}

		// And reading it back. Everything before the first space on a line is the key,
		// everything after is the value — a value carrying a space or a newline does not
		// survive this, which is the point.
		static std::map<std::string, std::string> parse_project_state(const std::string& serialised)
		{
			std::map<std::string, std::string> parsed;
			std::istringstream lines{serialised};
			std::string line;

			while (std::getline(lines, line)) {
				const std::size_t first_visible = line.find_first_not_of(' ');

				if (first_visible == std::string::npos) {
					continue;
				}

				line = line.substr(first_visible);

				if (line == "<SESH" || line == ">") {
					continue;
				}

				const std::size_t separator = line.find(' ');

				if (separator == std::string::npos) {
					parsed[line] = std::string{};
					continue;
				}

				parsed[line.substr(0, separator)] = line.substr(separator + 1);
			}

			return parsed;
		}

		bool refuse_writes = false;
		std::vector<std::string> keys_written;

	private:
		// unique_ptr so a handle stays valid across a reorder — the property the whole
		// per-track mechanism exists for would be unfalsifiable if reordering the
		// vector moved the tracks.
		std::vector<std::unique_ptr<Track>> tracks_;
		std::map<std::string, std::string> live_state_;
		std::map<std::string, std::string> saved_state_;
		std::string file_name_;
		bool has_unsaved_changes_ = false;
	};

	// Deterministic byte source. The suite needs a few thousand adversarial strings
	// and has to produce the same ones on every machine, so a failure is a failure
	// anybody can reproduce from the seed in the assertion message.
	class DeterministicBytes {
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

	// The vocabulary a producer might actually type, weighted towards everything that
	// breaks a line-oriented project file: the quote characters REAPER quotes with,
	// the chunk delimiters, spaces, newlines, the percent sign, control bytes, and
	// multi-byte UTF-8 from the languages Sesh localises into.
	const std::vector<std::string>& hostile_alphabet()
	{
		static const std::vector<std::string> pieces{
			"a", "b", "z", "0", "9", "_", "-", ".", "/", "\\", "|", "*", ":", ";", ",",
			" ", "  ", "\t", "\n", "\r", "\r\n", "\v", "\f",
			"\"", "'", "`", "<", ">", "%", "%20", "%%", "%zz",
			std::string(1, '\x01'), std::string(1, '\x1f'), std::string(1, '\x7f'),
			"ä", "ß", "é", "ü", "ドラム", "バス", "São", "batería", "guitare", "chitarra",
			"the drum bus", "don't call it \"the drums\"", "kick <1>", "100% wet",
		};

		return pieces;
	}

	std::string generate_hostile_text(DeterministicBytes& bytes, std::size_t maximum_pieces)
	{
		const std::vector<std::string>& pieces = hostile_alphabet();
		const std::size_t piece_count = bytes.below(maximum_pieces + 1);

		std::string text;

		for (std::size_t index = 0; index < piece_count; ++index) {
			text += pieces[bytes.below(pieces.size())];
		}

		return text;
	}

	// Whether one encoded alias is safe to put on a line of a project file. Every
	// byte the encoder escapes must be gone, except the `%` that does the escaping.
	//
	// The separator is *not* allowed through here, and that is the assertion that
	// makes joining sound: if an encoded alias could contain a separator, splitting
	// the joined value would cut an alias in half.
	bool is_safe_for_project_file(const std::string& stored)
	{
		for (const char character : stored) {
			if (ext_state_byte_needs_encoding(static_cast<unsigned char>(character))
				&& static_cast<unsigned char>(character) != '%') {
				return false;
			}
		}

		return true;
	}

	// And whether a whole stored set is. Same rule, plus the separator this component
	// put between the aliases itself.
	bool is_safe_alias_list_for_project_file(const std::string& stored)
	{
		for (const char character : stored) {
			const unsigned char byte = static_cast<unsigned char>(character);

			if (ext_state_byte_needs_encoding(byte) && byte != '%'
				&& byte != static_cast<unsigned char>(alias_separator)) {
				return false;
			}
		}

		return true;
	}

	// What the track actually holds, read straight out of the fake rather than
	// through the store, for the cases that are about the stored form.
	std::string stored_alias_value(TrackHandle track)
	{
		const FakeAliasStorage::Track& record = FakeAliasStorage::track_of(track);
		const auto found = record.extension_values.find("P_EXT:sesh_alias");

		return found == record.extension_values.end() ? std::string{} : found->second;
	}

	using Aliases = std::vector<std::string>;

}

// ---------------------------------------------------------------------------
// Requirement 8.1 — the alias lives on the track
// ---------------------------------------------------------------------------

TEST_CASE("a learned alias is written to the track under the namespaced P_EXT key", "[alias]")
{
	FakeAliasStorage storage;
	storage.add_track("Track 1");

	AliasStore store{storage};

	const TrackHandle track = storage.handle_for(0);
	const LearnedAlias learned = store.learn_track_alias(track, "the drum bus");

	REQUIRE(learned.learned());
	REQUIRE(learned.alias == "the drum bus");
	REQUIRE(learned.aliases_held == Aliases{"the drum bus"});
	REQUIRE(learned.reason.empty());

	// The key REAPER persists it under, verbatim. Requirement 8.1 names the
	// mechanism, and requirement 26.6 wants the key namespaced so it is invisible in
	// REAPER's own UI — both of which are this string.
	REQUIRE(std::string{track_alias_extension_key} == "P_EXT:sesh_alias");
	REQUIRE(storage.keys_written == std::vector<std::string>{"P_EXT:sesh_alias"});

	// What actually lands in the `.rpp`: one safe token, not a quoted sentence.
	const FakeAliasStorage::Track& record = FakeAliasStorage::track_of(track);
	REQUIRE(record.extension_values.at("P_EXT:sesh_alias") == "the%20drum%20bus");

	// And nothing else on the track was touched.
	REQUIRE(record.extension_values.size() == 1);
	REQUIRE(record.name == "Track 1");

	REQUIRE(store.track_aliases(track) == Aliases{"the drum bus"});
}

TEST_CASE("an alias survives track reordering, because it is part of the track", "[alias]")
{
	// The reason the alias is not kept in a side table. A table keyed by index goes
	// wrong the moment the producer drags a track; a table keyed by GUID needs
	// maintaining against deletes. Living on the track, there is nothing to keep in
	// step.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.add_track("Drum Bus");
	storage.add_track("Bass");

	AliasStore store{storage};

	const TrackHandle drum_bus = storage.handle_for(1);

	REQUIRE(store.learn_track_alias(drum_bus, "the drum bus").learned());
	REQUIRE(store.tracks_with_alias("the drum bus") == std::vector<TrackHandle>{drum_bus});

	storage.swap_tracks(0, 2);
	storage.swap_tracks(1, 2);

	// Same track, now last in the list, same alias — and the lookup finds it where it
	// now is rather than where it was.
	REQUIRE(storage.handle_for(2) == drum_bus);
	REQUIRE(store.track_aliases(drum_bus) == Aliases{"the drum bus"});
	REQUIRE(store.tracks_with_alias("the drum bus") == std::vector<TrackHandle>{drum_bus});

	// And a name taught after the reorder joins the one taught before it, in order.
	// Nothing about accumulation depends on where the track sits, because the set is
	// read back off the track rather than out of anything indexed by position.
	REQUIRE(store.learn_track_alias(drum_bus, "the glue").learned());
	REQUIRE(store.track_aliases(drum_bus) == Aliases{"the drum bus", "the glue"});
	REQUIRE(store.tracks_with_alias("the glue") == std::vector<TrackHandle>{drum_bus});
}

TEST_CASE("an alias travels with the project to another machine", "[alias]")
{
	// Opening the same project elsewhere: the track chunks arrive with their
	// extension values, and nothing about the alias depended on the machine that
	// learned it.
	FakeAliasStorage original;
	original.add_track("Kick");
	original.add_track("Drum Bus");

	AliasStore original_store{original};

	REQUIRE(original_store.learn_track_alias(original.handle_for(1), "the drum bus").learned());
	REQUIRE(original_store.learn_track_alias(original.handle_for(1), "the glue").learned());
	REQUIRE(original_store.learn_track_alias(original.handle_for(1), "バス").learned());

	FakeAliasStorage elsewhere;

	for (int index = 0; index < original.track_count(); ++index) {
		const FakeAliasStorage::Track& source = FakeAliasStorage::track_of(original.handle_for(index));
		FakeAliasStorage::Track& copy = elsewhere.add_track(source.name);
		copy.extension_values = source.extension_values;
	}

	AliasStore store_elsewhere{elsewhere};

	// Every alias, and in the order they were taught on the machine that taught them.
	// Requirement 8.4 says the order is reproducible *across machines*, and this is
	// the only case that can show it: the order has to be carried by the stored value
	// rather than reconstructed from anything local.
	REQUIRE(store_elsewhere.track_aliases(elsewhere.handle_for(1))
		== Aliases{"the drum bus", "the glue", "バス"});
	REQUIRE(store_elsewhere.tracks_with_alias("the drum bus")
		== std::vector<TrackHandle>{elsewhere.handle_for(1)});
	REQUIRE(store_elsewhere.tracks_with_alias("バス")
		== std::vector<TrackHandle>{elsewhere.handle_for(1)});
}

// ---------------------------------------------------------------------------
// Requirements 8.2, 8.3, 8.4 — learning accumulates, in order, without duplicates
// ---------------------------------------------------------------------------

TEST_CASE("a second learned alias joins the first rather than replacing it", "[alias][accumulate]")
{
	// Requirement 8.2, and the failure the amendment exists to fix. A producer who
	// says "call it the kick" and later "also kik" has taught two names; a store that
	// wrote the second over the first would leave them calling it something Sesh had
	// silently stopped knowing.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the kick").learned());
	REQUIRE(store.learn_track_alias(track, "kik").learned());
	REQUIRE(store.learn_track_alias(track, "bd").learned());

	REQUIRE(store.track_aliases(track) == Aliases{"the kick", "kik", "bd"});

	// And every one of them resolves. Holding three names is only worth anything if
	// all three answer.
	REQUIRE(store.tracks_with_alias("the kick") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("kik") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("bd") == std::vector<TrackHandle>{track});

	// All three under the one namespaced key (requirement 8.1 and 26.6), not three
	// keys — and the report from the last learn names the whole set, not just what it
	// added.
	REQUIRE(FakeAliasStorage::track_of(track).extension_values.size() == 1);
	REQUIRE(stored_alias_value(track) == "the%20kick|kik|bd");
}

TEST_CASE("the set held after each learn is reported back", "[alias][accumulate]")
{
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the kick").aliases_held == Aliases{"the kick"});
	REQUIRE(store.learn_track_alias(track, "kik").aliases_held == Aliases{"the kick", "kik"});
}

TEST_CASE("learning an alias the track already holds changes nothing and says so", "[alias][accumulate]")
{
	// Requirement 8.3. Compared on the normalized form, because that is what
	// resolution matches on: if "The Kick" and "the kick" were stored as two aliases,
	// one name would have become an ambiguity the producer could not resolve, since
	// both candidates are the same track under names they cannot tell apart.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the kick").learned());
	REQUIRE(store.learn_track_alias(track, "kik").learned());

	const std::size_t writes_before = storage.keys_written.size();

	for (const std::string& same : {"the kick", "The Kick", "  THE   KICK  ", "the kick"}) {
		const LearnedAlias learned = store.learn_track_alias(track, same);

		INFO("relearning " << same);

		CHECK(learned.outcome == AliasWriteOutcome::already_held);
		CHECK_FALSE(learned.learned());

		// It is held, whether or not this call is what taught it — the question most
		// callers actually have.
		CHECK(learned.held());

		// The alias reported back is the one the track holds, as the producer first
		// typed it, rather than the casing they just used.
		CHECK(learned.alias == "the kick");
		CHECK(learned.aliases_held == Aliases{"the kick", "kik"});
	}

	// No change means no write. Dirtying the producer's project to store what is
	// already there is not free: it is the difference between their session being
	// saved and not.
	REQUIRE(storage.keys_written.size() == writes_before);
	REQUIRE(store.track_aliases(track) == Aliases{"the kick", "kik"});
}

TEST_CASE("aliases are held in the order they were learned", "[alias][accumulate]")
{
	// Requirement 8.4. The order is what makes the candidate list resolution reports
	// reproducible: a set that came back in hash order would reshuffle between reads,
	// so an ambiguity the producer was asked to choose from would list its options
	// differently each time.
	FakeAliasStorage storage;
	storage.add_track("Drum Bus");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	const Aliases taught{"zz", "the drum bus", "a", "glue", "バス", "Bus"};

	for (const std::string& alias : taught) {
		REQUIRE(store.learn_track_alias(track, alias).learned());
	}

	// Not sorted, not reversed, not hash order — taught order, and the same on every
	// read.
	REQUIRE(store.track_aliases(track) == taught);
	REQUIRE(store.track_aliases(track) == taught);
	REQUIRE(store.all_track_aliases().at(0).second == taught);
}

// ---------------------------------------------------------------------------
// Requirements 8.5, 8.6 — the cap, and what happens at it
// ---------------------------------------------------------------------------

TEST_CASE("a track holds at most the per-track cap of aliases", "[alias][cap]")
{
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	Aliases taught;

	for (std::size_t index = 0; index < maximum_aliases_per_track; ++index) {
		const std::string alias = "name " + std::to_string(index);

		REQUIRE(store.learn_track_alias(track, alias).learned());
		taught.push_back(alias);
	}

	REQUIRE(store.track_aliases(track).size() == maximum_aliases_per_track);
	REQUIRE(store.track_aliases(track) == taught);
}

TEST_CASE("an alias learned past the cap is rejected, and nothing already held is dropped",
	"[alias][cap]")
{
	// Requirement 8.6, and the reason the cap is a rejection rather than an eviction.
	// Dropping the oldest would take away a name the producer still uses and never
	// tell them — they would find out by saying it and being asked which track they
	// meant.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	Aliases taught;

	for (std::size_t index = 0; index < maximum_aliases_per_track; ++index) {
		const std::string alias = "name " + std::to_string(index);

		REQUIRE(store.learn_track_alias(track, alias).learned());
		taught.push_back(alias);
	}

	const std::string stored_before = stored_alias_value(track);
	const std::size_t writes_before = storage.keys_written.size();

	const LearnedAlias refused = store.learn_track_alias(track, "one too many");

	REQUIRE(refused.outcome == AliasWriteOutcome::rejected_cap_reached);
	REQUIRE_FALSE(refused.learned());
	REQUIRE_FALSE(refused.held());

	// The reason names the cap, so the producer can be told which of their names to
	// give up rather than having one chosen for them.
	REQUIRE(refused.reason.find(std::to_string(maximum_aliases_per_track)) != std::string::npos);

	// Every alias already held, untouched and in order — the first one especially,
	// which is what an eviction policy would have taken.
	REQUIRE(refused.aliases_held == taught);
	REQUIRE(store.track_aliases(track) == taught);
	REQUIRE(store.tracks_with_alias("name 0") == std::vector<TrackHandle>{track});

	// And no write happened at all, so the stored value is byte-for-byte what it was.
	REQUIRE(storage.keys_written.size() == writes_before);
	REQUIRE(stored_alias_value(track) == stored_before);

	// The new name is not held, which is the other half of the rejection: a caller
	// that believed it had been learned would stop asking.
	REQUIRE(store.tracks_with_alias("one too many").empty());

	// Relearning one the track already holds still works at the cap. It needs no
	// room, so refusing it would be refusing to say "yes, already".
	REQUIRE(store.learn_track_alias(track, "name 3").outcome == AliasWriteOutcome::already_held);

	// And once one is forgotten there is room again.
	REQUIRE(store.forget_track_alias(track, "name 0") == AliasWriteOutcome::forgotten);
	REQUIRE(store.learn_track_alias(track, "one too many").learned());
	REQUIRE(store.track_aliases(track).size() == maximum_aliases_per_track);
}

// ---------------------------------------------------------------------------
// Requirements 8.7, 8.8 — the two shapes of forgetting
// ---------------------------------------------------------------------------

TEST_CASE("forgetting one alias leaves the rest of the track's vocabulary in order",
	"[alias][forget]")
{
	// Requirement 8.7. The other half of accumulation: a set you cannot remove one
	// item from is a set the producer can only clear.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	for (const std::string& alias : {"the kick", "kik", "bd", "boom"}) {
		REQUIRE(store.learn_track_alias(track, alias).learned());
	}

	// Matched on the same normalized form learning compares on, so the producer can
	// forget "kik" without remembering which casing they taught it in.
	REQUIRE(store.forget_track_alias(track, "  KIK ") == AliasWriteOutcome::forgotten);

	REQUIRE(store.track_aliases(track) == Aliases{"the kick", "bd", "boom"});
	REQUIRE(store.tracks_with_alias("kik").empty());
	REQUIRE(store.tracks_with_alias("the kick") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("boom") == std::vector<TrackHandle>{track});

	// Removing from the middle does not reorder what is left, and removing the first
	// or the last is not a special case.
	REQUIRE(store.forget_track_alias(track, "the kick") == AliasWriteOutcome::forgotten);
	REQUIRE(store.track_aliases(track) == Aliases{"bd", "boom"});

	REQUIRE(store.forget_track_alias(track, "boom") == AliasWriteOutcome::forgotten);
	REQUIRE(store.track_aliases(track) == Aliases{"bd"});

	// Down to nothing, the key goes rather than an empty value staying behind.
	REQUIRE(store.forget_track_alias(track, "bd") == AliasWriteOutcome::forgotten);
	REQUIRE(store.track_aliases(track).empty());
	REQUIRE(FakeAliasStorage::track_of(track).extension_values.empty());
}

TEST_CASE("forgetting an alias the track never held changes nothing", "[alias][forget]")
{
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the kick").learned());

	const std::size_t writes_before = storage.keys_written.size();

	// The post-condition asked for is "this track does not answer to that name", and
	// it already holds. Writing anyway would dirty the producer's project to change
	// nothing.
	REQUIRE(store.forget_track_alias(track, "the snare") == AliasWriteOutcome::forgotten);

	REQUIRE(storage.keys_written.size() == writes_before);
	REQUIRE(store.track_aliases(track) == Aliases{"the kick"});

	// And an empty name is neither "forget this one" nor "forget them all", so the
	// store does not guess which was meant — guessing would mean deleting vocabulary
	// on a hunch.
	REQUIRE(store.forget_track_alias(track, "   ") == AliasWriteOutcome::rejected_empty);
	REQUIRE(store.forget_track_alias(track, "") == AliasWriteOutcome::rejected_empty);
	REQUIRE(store.track_aliases(track) == Aliases{"the kick"});
}

TEST_CASE("forgetting every alias removes the key rather than storing an empty one",
	"[alias][forget]")
{
	// Requirement 8.8.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	for (const std::string& alias : {"the kick", "kik", "bd"}) {
		REQUIRE(store.learn_track_alias(track, alias).learned());
	}

	REQUIRE(store.forget_all_track_aliases(track) == AliasWriteOutcome::forgotten);

	// The track is left exactly as it was before Sesh ever saw it.
	REQUIRE(FakeAliasStorage::track_of(track).extension_values.empty());
	REQUIRE(store.track_aliases(track).empty());
	REQUIRE(store.tracks_with_alias("the kick").empty());

	// Clearing a track that held nothing is not an error, for the same reason as
	// above: the post-condition holds.
	REQUIRE(store.forget_all_track_aliases(track) == AliasWriteOutcome::forgotten);

	// And learning after a clear starts a fresh set rather than resurrecting the old
	// one.
	REQUIRE(store.learn_track_alias(track, "boom").learned());
	REQUIRE(store.track_aliases(track) == Aliases{"boom"});
}

// ---------------------------------------------------------------------------
// Requirement 8.9 — mappings that are not about one track
// ---------------------------------------------------------------------------

TEST_CASE("a non-track mapping is written to project state under the sesh section", "[alias]")
{
	FakeAliasStorage storage;
	AliasStore store{storage};

	REQUIRE(std::string{project_extension_section} == "sesh");

	const LearnedAlias learned = store.learn_mapping("my mix bus setup", "drum bus + glue");

	REQUIRE(learned.learned());
	REQUIRE(learned.alias == "my mix bus setup");

	// Encoded on both sides. The key is producer vocabulary too, and a key with a
	// space in it is a key GetProjExtState will not find again.
	REQUIRE(storage.keys_written == std::vector<std::string>{"my%20mix%20bus%20setup"});
	REQUIRE(storage.live_state().at(std::string{"sesh"} + '\n' + "my%20mix%20bus%20setup")
		== "drum%20bus%20+%20glue");

	// Not readable yet — see the save-state cases below. Readable once saved.
	REQUIRE(store.mapping("my mix bus setup").empty());

	storage.save_project();

	REQUIRE(store.mapping("my mix bus setup") == "drum bus + glue");
}

TEST_CASE("a mapping is forgotten by writing an empty value", "[alias]")
{
	FakeAliasStorage storage;
	AliasStore store{storage};

	REQUIRE(store.learn_mapping("my mix bus setup", "drum bus + glue").learned());
	storage.save_project();

	REQUIRE(store.forget_mapping("my mix bus setup") == AliasWriteOutcome::forgotten);
	REQUIRE(storage.live_state().empty());

	// An empty value passed to learn is the same instruction, and reports itself as
	// such rather than as a learn.
	REQUIRE(store.learn_mapping("my mix bus setup", "drum bus + glue").learned());
	REQUIRE(store.learn_mapping("my mix bus setup", "").outcome == AliasWriteOutcome::forgotten);
	REQUIRE(storage.live_state().empty());

	// A name that is nothing but whitespace is neither a learn nor a forget, and the
	// store does not guess which was meant.
	REQUIRE(store.forget_mapping("   ") == AliasWriteOutcome::rejected_empty);
	REQUIRE(store.mapping("   ").empty());
}

// ---------------------------------------------------------------------------
// Requirement 8.10 — telling the producer the alias may not survive
// ---------------------------------------------------------------------------

TEST_CASE("an alias learned in a never-saved session reports the project as unsaved", "[alias]")
{
	// The case requirement 8.10 is about. Project state is read as of the last save,
	// so in a session with no file on disk the alias is certain to be gone next
	// launch, and the producer is told now rather than discovering it next week.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};

	REQUIRE_FALSE(store.project_saved());

	const LearnedAlias track_alias = store.learn_track_alias(storage.handle_for(0), "the kick");

	REQUIRE(track_alias.learned());
	REQUIRE_FALSE(track_alias.project_saved);

	const LearnedAlias mapping = store.learn_mapping("my mix bus setup", "drum bus + glue");

	REQUIRE(mapping.learned());
	REQUIRE_FALSE(mapping.project_saved);
}

TEST_CASE("the save state is sampled before the write, not after it", "[alias]")
{
	// Writing an alias dirties the project, so a save state read afterwards reports
	// "unsaved" for every learn — technically true and useless, because it says
	// nothing about the session the producer is in. Sampling first is what makes the
	// report mean something.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.save_project();

	AliasStore store{storage};

	REQUIRE(store.project_saved());

	const LearnedAlias learned = store.learn_track_alias(storage.handle_for(0), "the kick");

	REQUIRE(learned.learned());
	REQUIRE(learned.project_saved);

	// The write itself is what made the project dirty, which is exactly why the
	// sample had to happen before it.
	REQUIRE(storage.project_has_unsaved_changes());
	REQUIRE_FALSE(store.project_saved());
}

TEST_CASE("a saved project with pending edits still reports unsaved", "[alias]")
{
	// Both signals, and the AND is the conservative direction: a file on disk that
	// does not yet contain this session's edits will not contain the alias either.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.save_project();
	storage.mark_unsaved_changes(true);

	AliasStore store{storage};

	REQUIRE_FALSE(store.learn_track_alias(storage.handle_for(0), "the kick").project_saved);

	// And a dirty flag REAPER declines to report — which is what happens when the
	// producer turns off "undo/prompt to save" — degrades to the file check rather
	// than to a crash or a guess.
	storage.mark_unsaved_changes(false);

	REQUIRE(store.project_saved());

	storage.set_file_name("");

	REQUIRE_FALSE(store.project_saved());
}

TEST_CASE("a refused write is reported as refused rather than as learned", "[alias][errors]")
{
	// An alias reported as remembered that was never written is worse than one
	// reported as failed: the producer stops saying the track's real name.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.refuse_writes = true;

	AliasStore store{storage};

	const LearnedAlias learned = store.learn_track_alias(storage.handle_for(0), "the kick");

	REQUIRE(learned.outcome == AliasWriteOutcome::storage_refused);
	REQUIRE_FALSE(learned.learned());
	REQUIRE(learned.alias.empty());

	REQUIRE(learned.aliases_held.empty());

	REQUIRE(store.forget_all_track_aliases(storage.handle_for(0)) == AliasWriteOutcome::storage_refused);
	REQUIRE(store.learn_mapping("my setup", "value").outcome == AliasWriteOutcome::storage_refused);
	REQUIRE(store.forget_mapping("my setup") == AliasWriteOutcome::storage_refused);
}

TEST_CASE("a refused write on a forget-one leaves the aliases as they were", "[alias][errors]")
{
	// The write a forget-one makes is the whole remaining set, so a refusal has to
	// report itself: a caller told the alias was forgotten would stop offering it back
	// to the producer while the track still answered to it.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the kick").learned());
	REQUIRE(store.learn_track_alias(track, "kik").learned());

	storage.refuse_writes = true;

	REQUIRE(store.forget_track_alias(track, "kik") == AliasWriteOutcome::storage_refused);

	storage.refuse_writes = false;

	REQUIRE(store.track_aliases(track) == Aliases{"the kick", "kik"});
}

TEST_CASE("an alias that is whitespace, empty, or too long is refused", "[alias][errors]")
{
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	SECTION("whitespace only")
	{
		REQUIRE(store.learn_track_alias(track, " \t\n ").outcome == AliasWriteOutcome::rejected_empty);
		REQUIRE(storage.keys_written.empty());
	}

	SECTION("empty is a refusal, not an instruction to forget")
	{
		// Under a single-alias store, clearing the value *was* forgetting, so an empty
		// alias could reasonably mean either. With two forget operations of its own,
		// reading an empty string as "forget everything this track answers to" would be
		// the store deleting vocabulary nobody asked it to.
		REQUIRE(store.learn_track_alias(track, "the kick").learned());
		REQUIRE(store.learn_track_alias(track, "").outcome == AliasWriteOutcome::rejected_empty);
		REQUIRE(store.track_aliases(track) == Aliases{"the kick"});
	}

	SECTION("longer than the bound is refused rather than truncated")
	{
		// Half an alias is a different alias, and it would resolve to the wrong track
		// quietly.
		const std::string too_long(maximum_alias_length + 1, 'a');

		REQUIRE(store.learn_track_alias(track, too_long).outcome == AliasWriteOutcome::rejected_too_long);
		REQUIRE(store.track_aliases(track).empty());

		const std::string at_the_bound(maximum_alias_length, 'a');

		REQUIRE(store.learn_track_alias(track, at_the_bound).learned());
		REQUIRE(store.track_aliases(track) == Aliases{at_the_bound});
	}

	SECTION("a mapping name or value past the bound is refused")
	{
		const std::string too_long(maximum_alias_length + 1, 'a');

		REQUIRE(store.learn_mapping(too_long, "value").outcome == AliasWriteOutcome::rejected_too_long);
		REQUIRE(store.learn_mapping("name", too_long).outcome == AliasWriteOutcome::rejected_too_long);
		REQUIRE(store.learn_mapping("", "value").outcome == AliasWriteOutcome::rejected_empty);
	}

	SECTION("no track is a refusal, not a crash")
	{
		REQUIRE(store.learn_track_alias(nullptr, "the kick").outcome == AliasWriteOutcome::storage_refused);
		REQUIRE(store.forget_track_alias(nullptr, "the kick") == AliasWriteOutcome::storage_refused);
		REQUIRE(store.forget_all_track_aliases(nullptr) == AliasWriteOutcome::storage_refused);
		REQUIRE(store.track_aliases(nullptr).empty());
	}
}

// ---------------------------------------------------------------------------
// The reads the Object Resolver needs
// ---------------------------------------------------------------------------

TEST_CASE("every track answering to an alias is returned, in track order", "[alias]")
{
	// Requirement 7.4 wants a tie reported as ambiguous with every candidate named,
	// which it cannot do if the alias source hands back the first match. Two tracks
	// carrying the same alias is a state a producer reaches by duplicating a track.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.add_track("Drum Bus");
	storage.add_track("Drum Bus copy");
	storage.add_track("Bass");

	AliasStore store{storage};

	REQUIRE(store.learn_track_alias(storage.handle_for(1), "the drum bus").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(2), "the drum bus").learned());

	REQUIRE(store.tracks_with_alias("the drum bus")
		== std::vector<TrackHandle>{storage.handle_for(1), storage.handle_for(2)});

	// A pattern nothing answers to returns nothing — the resolver turns that into
	// `unresolved_track_selector` rather than this component guessing.
	REQUIRE(store.tracks_with_alias("the guitars").empty());
	REQUIRE(store.tracks_with_alias("").empty());
	REQUIRE(store.tracks_with_alias("   ").empty());

	// A track matches on any one of the names it holds, which is the point of holding
	// several — and it is one candidate however many of them matched, because a track
	// listed twice is an ambiguity between a track and itself.
	REQUIRE(store.learn_track_alias(storage.handle_for(1), "the glue").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(1), "bus").learned());

	REQUIRE(store.tracks_with_alias("bus") == std::vector<TrackHandle>{storage.handle_for(1)});
	REQUIRE(store.tracks_with_alias("the drum bus")
		== std::vector<TrackHandle>{storage.handle_for(1), storage.handle_for(2)});
}

TEST_CASE("alias matching ignores case and surrounding whitespace, storage does not", "[alias]")
{
	FakeAliasStorage storage;
	storage.add_track("Drum Bus");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	// Stored as typed, because it gets read back and shown to the producer. The
	// trimming is the one exception: a trailing space is a typing artefact.
	REQUIRE(store.learn_track_alias(track, "  The Drum   Bus  ").learned());
	REQUIRE(store.track_aliases(track) == Aliases{"The Drum   Bus"});

	// Matched on a normalised form, so the producer saying it back differently still
	// lands.
	REQUIRE(store.tracks_with_alias("the drum bus") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("THE DRUM BUS") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("  the   drum bus ") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("the drums").empty());

	// ASCII case folding only, and deliberately: folding the rest of Unicode
	// correctly needs a table this extension has no reason to carry, and folding it
	// with whatever locale REAPER is in produces matches that depend on the machine.
	REQUIRE(normalize_alias_for_matching("Bühne") == "bühne");
	REQUIRE(normalize_alias_for_matching("ドラム バス") == "ドラム バス");
}

TEST_CASE("every aliased track is listed in one walk, unaliased tracks omitted", "[alias]")
{
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.add_track("Drum Bus");
	storage.add_track("Bass");

	AliasStore store{storage};

	REQUIRE(store.learn_track_alias(storage.handle_for(0), "the kick").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(0), "kik").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(2), "the low end").learned());

	const std::vector<std::pair<TrackHandle, Aliases>> aliases = store.all_track_aliases();

	REQUIRE(aliases.size() == 2);
	REQUIRE(aliases[0].first == storage.handle_for(0));
	REQUIRE(aliases[0].second == Aliases{"the kick", "kik"});
	REQUIRE(aliases[1].first == storage.handle_for(2));
	REQUIRE(aliases[1].second == Aliases{"the low end"});
}

TEST_CASE("nothing is cached, so a change made outside Sesh is visible", "[alias]")
{
	// The producer can clear these keys with a ReaScript, or load a different
	// project. A cache would keep answering with vocabulary that is no longer in the
	// file.
	FakeAliasStorage storage;
	storage.add_track("Drum Bus");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	REQUIRE(store.learn_track_alias(track, "the drum bus").learned());

	static_cast<FakeAliasStorage::Track*>(track)->extension_values["P_EXT:sesh_alias"] = "the%20glue|bus";

	REQUIRE(store.track_aliases(track) == Aliases{"the glue", "bus"});
	REQUIRE(store.tracks_with_alias("the drum bus").empty());
	REQUIRE(store.tracks_with_alias("the glue") == std::vector<TrackHandle>{track});
	REQUIRE(store.tracks_with_alias("bus") == std::vector<TrackHandle>{track});
}

TEST_CASE("a value written outside Sesh is read as best it can be, not refused", "[alias]")
{
	// The project file is the producer's, and these keys can be hand-edited, written
	// by a ReaScript, or left by an older build. Reading such a value as well as it
	// can be read beats reporting that the producer never taught anything.
	FakeAliasStorage storage;
	storage.add_track("Drum Bus");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	FakeAliasStorage::Track& record = *static_cast<FakeAliasStorage::Track*>(track);

	// A single-alias value, which is what an earlier build of this component wrote.
	// No separator means one alias, so those read correctly with nothing to migrate.
	record.extension_values["P_EXT:sesh_alias"] = "the%20drum%20bus";

	REQUIRE(store.track_aliases(track) == Aliases{"the drum bus"});

	// And learning against one accumulates onto it rather than starting over.
	REQUIRE(store.learn_track_alias(track, "glue").learned());
	REQUIRE(store.track_aliases(track) == Aliases{"the drum bus", "glue"});

	// Leading, trailing, and doubled separators are skipped rather than read as empty
	// aliases. Nothing here writes those; an empty alias is not vocabulary.
	record.extension_values["P_EXT:sesh_alias"] = "|kick||the%20kick|";

	REQUIRE(store.track_aliases(track) == Aliases{"kick", "the kick"});

	// More aliases than the cap allows are all reported rather than quietly trimmed —
	// trimming on read is the silent dropping the cap exists to refuse. No write can
	// make it worse, because learning is rejected until the count is back under.
	std::string overfull;

	for (std::size_t index = 0; index < maximum_aliases_per_track + 4; ++index) {
		overfull += (index == 0 ? "" : "|") + std::string{"name"} + std::to_string(index);
	}

	record.extension_values["P_EXT:sesh_alias"] = overfull;

	REQUIRE(store.track_aliases(track).size() == maximum_aliases_per_track + 4);
	REQUIRE(store.learn_track_alias(track, "one more").outcome
		== AliasWriteOutcome::rejected_cap_reached);
	REQUIRE(store.track_aliases(track).size() == maximum_aliases_per_track + 4);
}

// ---------------------------------------------------------------------------
// The Object Resolver's seam
// ---------------------------------------------------------------------------

TEST_CASE("the store answers the resolver's alias lookup, keyed on the track it is holding",
	"[alias][resolver]")
{
	// `LearnedAliasLookup` takes the `ResolvableTrack` the resolver already has, which
	// carries both the GUID and the handle. The store is keyed by handle because
	// requirement 8.1 keeps the aliases on the track, so the adapter is a forward
	// rather than a GUID-to-handle search of its own.
	FakeAliasStorage storage;
	storage.add_track("Kick");
	storage.add_track("Bass");

	AliasStore store{storage};
	const StoredAliasLookup lookup{store};

	REQUIRE(store.learn_track_alias(storage.handle_for(0), "the kick").learned());
	REQUIRE(store.learn_track_alias(storage.handle_for(0), "kik").learned());

	ResolvableTrack kick;
	kick.guid = "{00000000-0000-0000-0000-000000000001}";
	kick.name = "Kick";
	kick.track = static_cast<MediaTrack*>(storage.handle_for(0));
	kick.project_index = 0;

	// Every alias, in the order taught — the resolver's candidate order, and the
	// vector return finally carrying more than one thing.
	REQUIRE(lookup.learned_aliases_for_track(kick) == Aliases{"the kick", "kik"});

	ResolvableTrack bass;
	bass.guid = "{00000000-0000-0000-0000-000000000002}";
	bass.name = "Bass";
	bass.track = static_cast<MediaTrack*>(storage.handle_for(1));
	bass.project_index = 1;

	REQUIRE(lookup.learned_aliases_for_track(bass).empty());

	// A synthetic track carrying no handle answers with nothing rather than reading
	// through a null pointer.
	ResolvableTrack handleless;
	handleless.guid = kick.guid;
	handleless.name = "Kick";

	REQUIRE(lookup.learned_aliases_for_track(handleless).empty());

	// Nothing is cached: forgetting is visible through the lookup immediately.
	REQUIRE(store.forget_track_alias(storage.handle_for(0), "the kick")
		== AliasWriteOutcome::forgotten);
	REQUIRE(lookup.learned_aliases_for_track(kick) == Aliases{"kik"});
}

// ---------------------------------------------------------------------------
// Encoding — the round trip through a line-oriented, space-delimited file
// ---------------------------------------------------------------------------

TEST_CASE("the characters that would break a project file are encoded", "[alias][encoding]")
{
	REQUIRE(encode_ext_state_text("the drum bus") == "the%20drum%20bus");
	REQUIRE(encode_ext_state_text("line\nbreak") == "line%0Abreak");
	REQUIRE(encode_ext_state_text("carriage\rreturn") == "carriage%0Dreturn");
	REQUIRE(encode_ext_state_text("tab\there") == "tab%09here");

	// All three characters REAPER quotes with, in one value. A scheme that picks one
	// of them as a delimiter cannot represent this, which is the case that corrupts
	// quietly rather than failing.
	REQUIRE(encode_ext_state_text("\"'`") == "%22%27%60");

	// The chunk delimiters, and the escape character itself.
	REQUIRE(encode_ext_state_text("<SESH>") == "%3CSESH%3E");
	REQUIRE(encode_ext_state_text("100%") == "100%25");

	// And the separator that joins several aliases into one value. This is the
	// assertion the whole join-and-split scheme rests on: because a pipe inside an
	// alias is escaped, a pipe in the stored value can only be one this component put
	// there, so splitting needs no quoting rules and cannot cut an alias in half.
	REQUIRE(alias_separator == '|');
	REQUIRE(encode_ext_state_text("kick|snare") == "kick%7Csnare");

	// UTF-8 passes through: REAPER stores it in project files as-is, and encoding a
	// Japanese alias into nine times its length would buy nothing.
	REQUIRE(encode_ext_state_text("ドラム") == "ドラム");
	REQUIRE(encode_ext_state_text("Bühne") == "Bühne");
}

TEST_CASE("several aliases become one value, and one value becomes them again",
	"[alias][encoding]")
{
	REQUIRE(encode_alias_list({}).empty());
	REQUIRE(encode_alias_list({"kick"}) == "kick");
	REQUIRE(encode_alias_list({"kick", "the kick", "bd"}) == "kick|the%20kick|bd");

	// An alias containing the separator, which is the case a naive join loses.
	REQUIRE(encode_alias_list({"a|b", "c"}) == "a%7Cb|c");
	REQUIRE(decode_alias_list("a%7Cb|c") == Aliases{"a|b", "c"});

	// A value with no separator is one alias — which is exactly what an earlier
	// single-alias build wrote, so those degrade gracefully with nothing to migrate.
	REQUIRE(decode_alias_list("the%20drum%20bus") == Aliases{"the drum bus"});

	// Nothing at all is no aliases, not one empty one.
	REQUIRE(decode_alias_list("").empty());

	// Leading, trailing, and doubled separators are skipped. This component does not
	// write them; a hand-edited file can.
	REQUIRE(decode_alias_list("|a||b|") == Aliases{"a", "b"});
}

TEST_CASE("a set of aliases round-trips exactly, separator and escape character included",
	"[alias][encoding]")
{
	// The property the encoding exists for, at the level the set is stored. The
	// generated aliases are drawn from an alphabet that deliberately includes the
	// separator and the percent sign that escapes it, because those two bytes are the
	// only way a join-and-split scheme can be broken: a separator that survived
	// encoding would split an alias, and an escape character that did not would let a
	// producer's text spell one.
	DeterministicBytes bytes{0x71c0de5eu};

	int swept_sets = 0;
	int sets_containing_separator = 0;

	for (int iteration = 0; iteration < 4000; ++iteration) {
		const std::size_t count = bytes.below(maximum_aliases_per_track + 1);

		Aliases original;

		for (std::size_t index = 0; index < count; ++index) {
			// Non-empty, because an empty alias is not vocabulary and the store never
			// stores one — the decode side drops empties by design, so including them
			// would be asserting against a contract nobody holds.
			std::string alias = generate_hostile_text(bytes, 4);

			if (alias.empty()) {
				alias = "a";
			}

			original.push_back(alias);
		}

		const std::string encoded = encode_alias_list(original);

		INFO("iteration " << iteration << ", " << original.size() << " aliases");

		CHECK(decode_alias_list(encoded) == original);
		CHECK(is_safe_alias_list_for_project_file(encoded));

		// No raw newline in the stored value, ever. A `.rpp` is line-based, so one
		// newline in an extension value corrupts the producer's project file — this is
		// the assertion that says the separator did not smuggle one past the encoder.
		CHECK(encoded.find('\n') == std::string::npos);
		CHECK(encoded.find('\r') == std::string::npos);

		++swept_sets;

		for (const std::string& alias : original) {
			if (alias.find(alias_separator) != std::string::npos) {
				++sets_containing_separator;
				break;
			}
		}
	}

	// Negative controls on the sweep. A generator that never produced a set containing
	// the separator would pass every assertion above while testing nothing about the
	// case this encoding was chosen for.
	REQUIRE(swept_sets == 4000);
	REQUIRE(sets_containing_separator > 200);
}

TEST_CASE("a full set of aliases fits the value length the store publishes", "[alias][encoding]")
{
	// The bound `reaper_alias_storage.cpp` sizes its read buffer from. A buffer too
	// small for a full set would truncate the value on the way back in and lose
	// aliases — silently, and on the read path, where nothing would report it.
	Aliases worst_case;

	for (std::size_t index = 0; index < maximum_aliases_per_track; ++index) {
		// Every byte needing three to encode: a full-length alias of control bytes.
		worst_case.push_back(std::string(maximum_alias_length, '\n'));
	}

	REQUIRE(encode_alias_list(worst_case).size() <= maximum_stored_alias_value_length);
}

TEST_CASE("an encoded value decodes back to exactly what went in", "[alias][encoding]")
{
	REQUIRE(decode_ext_state_text("the%20drum%20bus") == "the drum bus");
	REQUIRE(decode_ext_state_text("%22%27%60") == "\"'`");

	// Lower-case hexadecimal too, since a hand-edited project file or another tool
	// may have written it that way.
	REQUIRE(decode_ext_state_text("the%20drum%0abus") == "the drum\nbus");
}

TEST_CASE("a malformed escape decodes to a literal percent rather than nothing", "[alias][encoding]")
{
	// Nothing this component writes produces these. The project file is the
	// producer's, though, and a value can have been hand-edited or written by
	// something else under a key that looks like ours. Returning the alias slightly
	// wrong beats returning nothing and having the resolver decide it was never
	// taught.
	REQUIRE(decode_ext_state_text("100%") == "100%");
	REQUIRE(decode_ext_state_text("%2") == "%2");
	REQUIRE(decode_ext_state_text("%zz") == "%zz");
	REQUIRE(decode_ext_state_text("%2zdrum") == "%2zdrum");
	REQUIRE(decode_ext_state_text("a%%20b") == "a% b");
}

TEST_CASE("encoding round-trips arbitrary producer vocabulary exactly", "[alias][encoding]")
{
	// The generated strings are built from spaces, newlines, every quote character,
	// the chunk delimiters, control bytes, percent signs, malformed escapes, and
	// multi-byte UTF-8 — the alphabet of things a producer types and a project file
	// cannot hold raw.
	DeterministicBytes bytes{0x5e58a11au};

	for (int iteration = 0; iteration < 4000; ++iteration) {
		const std::string original = generate_hostile_text(bytes, 8);
		const std::string encoded = encode_ext_state_text(original);

		INFO("iteration " << iteration);
		CHECK(decode_ext_state_text(encoded) == original);
		CHECK(is_safe_for_project_file(encoded));
	}
}

TEST_CASE("an encoded alias survives a line-oriented project file, and a raw one does not",
	"[alias][encoding]")
{
	// The negative control, and the reason the encoding exists at all. The same round
	// trip is applied to the raw value and to the encoded one: the raw value loses
	// everything after its first space and everything after its first newline, the
	// encoded value comes back intact.
	FakeAliasStorage storage;
	AliasStore store{storage};

	const std::string hostile = "don't call it \"the drums\"\nthe drum bus 100%";

	REQUIRE(store.learn_mapping("the drum bus", hostile).learned());

	const std::map<std::string, std::string> reparsed =
		FakeAliasStorage::parse_project_state(storage.serialise_project_state());

	REQUIRE(reparsed.size() == 1);
	REQUIRE(decode_ext_state_text(reparsed.at(encode_ext_state_text("the drum bus"))) == hostile);

	// What would have happened without it. The value is one line in the file, so a
	// newline ends the value and a space ends the key.
	std::ostringstream raw_file;
	raw_file << "<SESH\n  the drum bus " << hostile << "\n>\n";

	const std::map<std::string, std::string> raw_reparsed =
		FakeAliasStorage::parse_project_state(raw_file.str());

	// The key is now "the" — everything after the first space was taken for the
	// value — and the value is a fragment ending at the first newline.
	REQUIRE(raw_reparsed.find(hostile) == raw_reparsed.end());
	REQUIRE(raw_reparsed.find("the drum bus") == raw_reparsed.end());
	REQUIRE(raw_reparsed.find("the") != raw_reparsed.end());
	REQUIRE(raw_reparsed.at("the") != hostile);
}

TEST_CASE("an alias round-trips through the store for arbitrary vocabulary", "[alias][encoding]")
{
	// The same property one level up: whatever the producer says, what comes back out
	// of the store is what went in, modulo the documented trim.
	FakeAliasStorage storage;
	storage.add_track("Kick");

	AliasStore store{storage};
	const TrackHandle track = storage.handle_for(0);

	DeterministicBytes bytes{0xd12e57u};

	int learned_count = 0;

	for (int iteration = 0; iteration < 2000; ++iteration) {
		const std::string original = generate_hostile_text(bytes, 4);
		const std::string expected = trim_alias(original);

		const LearnedAlias learned = store.learn_track_alias(track, original);

		INFO("iteration " << iteration);

		if (expected.empty()) {
			// Whitespace only, or nothing at all. Refused, never stored.
			CHECK_FALSE(learned.learned());
			CHECK(store.track_aliases(track).empty());
			continue;
		}

		if (expected.size() > maximum_alias_length) {
			CHECK(learned.outcome == AliasWriteOutcome::rejected_too_long);
			continue;
		}

		++learned_count;

		CHECK(learned.learned());
		CHECK(learned.alias == expected);
		CHECK(store.track_aliases(track) == Aliases{expected});

		// And it is findable by what the producer would say back.
		CHECK(store.tracks_with_alias(expected) == std::vector<TrackHandle>{track});

		// Whatever went into the project file is safe to put on a line of one.
		CHECK(is_safe_alias_list_for_project_file(stored_alias_value(track)));

		REQUIRE(store.forget_all_track_aliases(track) == AliasWriteOutcome::forgotten);
	}

	// The negative control on the sweep itself: a generator that only produced
	// whitespace would pass every assertion above and check nothing.
	REQUIRE(learned_count > 1000);
}

TEST_CASE("an accumulated set round-trips through the store for arbitrary vocabulary",
	"[alias][encoding][accumulate]")
{
	// The property that matters most under the amendment, and the one a single-alias
	// sweep cannot state: teach a track a whole vocabulary of adversarial strings, one
	// at a time, and every name it was taught is still there, in order, exactly as
	// typed.
	//
	// The counts are asserted at the end so that a generator drifting into producing
	// only duplicates, or only rejections, fails rather than passing vacuously.
	DeterministicBytes bytes{0x4a11a5e7u};

	int filled_tracks = 0;
	int already_held_count = 0;
	int cap_rejections = 0;

	for (int iteration = 0; iteration < 600; ++iteration) {
		FakeAliasStorage storage;
		storage.add_track("Kick");

		AliasStore store{storage};
		const TrackHandle track = storage.handle_for(0);

		// Well past the cap at the top of the range, so a good share of iterations fill
		// the set and go on learning into a full one — that is where an eviction policy
		// would show itself. The rest stay under it, because a sweep that only ever ran
		// at the boundary would say nothing about the ordinary case.
		//
		// The range has to be this wide because the generator rejects freely: roughly a
		// third of what it produces is whitespace, too long, or a duplicate, so reaching
		// sixteen *accepted* aliases takes noticeably more than sixteen attempts. The
		// coverage guards at the end are what hold this honest.
		const std::size_t attempts = 1 + bytes.below(3 * maximum_aliases_per_track);

		Aliases expected;

		for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
			const std::string typed = generate_hostile_text(bytes, 3);
			const std::string trimmed = trim_alias(typed);

			const LearnedAlias learned = store.learn_track_alias(track, typed);

			INFO("iteration " << iteration << ", attempt " << attempt);

			if (trimmed.empty()) {
				CHECK(learned.outcome == AliasWriteOutcome::rejected_empty);
			} else if (trimmed.size() > maximum_alias_length) {
				CHECK(learned.outcome == AliasWriteOutcome::rejected_too_long);
			} else {
				bool duplicate = false;

				for (const std::string& held : expected) {
					if (normalize_alias_for_matching(held) == normalize_alias_for_matching(trimmed)) {
						duplicate = true;
						break;
					}
				}

				if (duplicate) {
					CHECK(learned.outcome == AliasWriteOutcome::already_held);
					++already_held_count;
				} else if (expected.size() >= maximum_aliases_per_track) {
					CHECK(learned.outcome == AliasWriteOutcome::rejected_cap_reached);
					CHECK(learned.reason.find(std::to_string(maximum_aliases_per_track))
						!= std::string::npos);
					++cap_rejections;
				} else {
					CHECK(learned.learned());
					expected.push_back(trimmed);
				}
			}

			// After every single call, whatever the outcome: the set is exactly what was
			// successfully taught, in taught order. An overwrite, an eviction, or a
			// duplicate entry all fail here on the call that caused them.
			CHECK(learned.aliases_held == expected);
			CHECK(store.track_aliases(track) == expected);
		}

		if (expected.size() == maximum_aliases_per_track) {
			++filled_tracks;
		}

		INFO("iteration " << iteration);

		CHECK(store.track_aliases(track).size() <= maximum_aliases_per_track);
		CHECK(is_safe_alias_list_for_project_file(stored_alias_value(track)));
		CHECK(stored_alias_value(track).find('\n') == std::string::npos);

		// Every name the track was taught answers, not just the first or the last.
		for (const std::string& held : expected) {
			CHECK(store.tracks_with_alias(held) == std::vector<TrackHandle>{track});
		}

		// And forgetting one at a time leaves the rest in order the whole way down.
		Aliases remaining = expected;

		while (!remaining.empty()) {
			const std::size_t victim = bytes.below(remaining.size());
			const std::string forgotten = remaining.at(victim);

			REQUIRE(store.forget_track_alias(track, forgotten) == AliasWriteOutcome::forgotten);

			remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(victim));

			CHECK(store.track_aliases(track) == remaining);
			CHECK(store.tracks_with_alias(forgotten).empty());
		}

		CHECK(FakeAliasStorage::track_of(track).extension_values.empty());
	}

	// Negative controls: the sweep has to have actually reached a full track, actually
	// met the cap, and actually met a duplicate.
	REQUIRE(filled_tracks > 20);
	REQUIRE(cap_rejections > 20);
	REQUIRE(already_held_count > 20);
}

TEST_CASE("a mapping round-trips through project state for arbitrary vocabulary", "[alias][encoding]")
{
	DeterministicBytes bytes{0x9a5e17u};

	int learned_count = 0;

	for (int iteration = 0; iteration < 2000; ++iteration) {
		FakeAliasStorage storage;
		AliasStore store{storage};

		const std::string name = generate_hostile_text(bytes, 3);
		const std::string value = generate_hostile_text(bytes, 3);

		const std::string expected_name = trim_alias(name);

		const LearnedAlias learned = store.learn_mapping(name, value);

		INFO("iteration " << iteration);

		if (expected_name.empty() || expected_name.size() > maximum_alias_length
			|| value.size() > maximum_alias_length) {
			CHECK_FALSE(learned.learned());
			continue;
		}

		if (value.empty()) {
			CHECK(learned.outcome == AliasWriteOutcome::forgotten);
			continue;
		}

		++learned_count;

		CHECK(learned.learned());

		// Not readable before the save, which is requirement 8.10's whole point, and
		// exactly what the store warned about through project_saved.
		CHECK(store.mapping(name).empty());
		CHECK_FALSE(learned.project_saved);

		storage.save_project();

		CHECK(store.mapping(name) == value);
		CHECK(store.mapping(expected_name) == value);
	}

	REQUIRE(learned_count > 500);
}
