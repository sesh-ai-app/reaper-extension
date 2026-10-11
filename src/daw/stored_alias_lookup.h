// The Alias Store, seen through the seam object resolution declares.
//
// `LearnedAliasLookup` lives in `daw/object_resolver.h` and asks one question: what
// has the producer called this track. `AliasStore` answers it. This file is the two
// sentences that connect them, and it is a separate header so that neither of those
// components has to include the other — the store stays free of anything resolution
// owns, and resolution stays free of `P_EXT:` keys and project extension state.
//
// The lookup is keyed on the `ResolvableTrack` rather than on a GUID, and that is
// the whole reason this adapter is three lines instead of a track walk.
// `ResolvableTrack` already carries both the `guid` and the `MediaTrack*`, the
// resolver already has one in hand at the moment it asks, and `AliasStore` is keyed
// by handle because REAPER stores the aliases on the track. Keying the seam on the
// GUID would mean throwing away the handle the caller had and rebuilding a
// GUID-to-handle mapping by enumeration — a second walk of the track list, per
// track, to recover something that was never lost.
//
// Passing the whole `ResolvableTrack` rather than the bare handle costs nothing and
// buys the suite something real: a substitute lookup can key on the `guid`, which is
// what a synthetic track list has, while this implementation keys on the handle,
// which is what REAPER has. Neither has to pretend to be the other.

#ifndef SESH_AI_DAW_STORED_ALIAS_LOOKUP_H
#define SESH_AI_DAW_STORED_ALIAS_LOOKUP_H

#include <string>
#include <vector>

#include <daw/alias_store.h>
#include <daw/object_resolver.h>

namespace sesh_ai {
namespace daw {

// Resolution's alias source, backed by the producer's project file.
//
// Holds the store by reference and caches nothing, which is the store's own
// position: the project is where the vocabulary is, and a copy would go stale the
// first time the producer cleared a key with a ReaScript.
class StoredAliasLookup final : public LearnedAliasLookup {
public:
	explicit StoredAliasLookup(const AliasStore& store)
		: store_{store}
	{
	}

	// Every alias the track holds, in the order the producer taught them —
	// requirement 8.4, which is what makes the candidate order resolution reports
	// reproducible across reads and across machines.
	//
	// A synthetic track with a null handle answers with nothing rather than reading
	// from a null, which is `AliasStore::track_aliases`' own behaviour and means a
	// track list assembled outside REAPER cannot fault this.
	std::vector<std::string> learned_aliases_for_track(const ResolvableTrack& track) const override
	{
		return store_.track_aliases(track.track);
	}

private:
	const AliasStore& store_;
};

}  // namespace daw
}  // namespace sesh_ai

#endif  // SESH_AI_DAW_STORED_ALIAS_LOOKUP_H
