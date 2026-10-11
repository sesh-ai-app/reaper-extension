// Alias Store — learned producer vocabulary, written into the producer's project.
//
// Two REAPER mechanisms, chosen for different reasons, and the difference is the
// whole design.
//
// A track's learned aliases are written to the track itself, through
// `GetSetMediaTrackInfo_String` with a `P_EXT:` key. REAPER persists that in the
// `.rpp` inside the track's own chunk, which buys three properties no side table
// can offer: the aliases survive track reordering (there is no index to go stale),
// they travel with the project to another machine, and they cannot drift out of
// sync with the track they describe because they *are* part of the track.
// Requirement 8.1.
//
// A mapping that is not about one particular track — "my mix bus setup", a
// vocabulary entry with no `MediaTrack*` to hang from — goes to project extension
// state under the `sesh` section, via `SetProjExtState` / `GetProjExtState`.
// Requirement 8.9. This one has a sharp edge: REAPER's own documentation is
// explicit that `GetProjExtState` returns the value "the last time the project was
// saved". A mapping learned in a session the producer never saves is simply gone
// on the next launch. Requirement 8.10 is that the producer hears this at the
// moment the alias is learned rather than discovering it next week, which is why
// every learn call reports the project's save state alongside the outcome — see
// LearnedAlias below, and `aliasesLearned` / `projectSaved` in
// `schemas/mcp-tools/outputs/set-track-state.schema.json`, which is the contract
// this component exists to be able to fill in.
//
// Requirement 26.6 is why the keys are namespaced: `sesh_alias` and the `sesh`
// section are invisible in REAPER's UI, so the producer's track panel looks
// exactly as they left it. It is still their file, and uninstalling Sesh leaves
// these keys behind — the README says so, because the alternative is Sesh deleting
// things from a project file at uninstall time.
//
// ---------------------------------------------------------------------------
// A track answers to several aliases, not to one
//
// The producer calls the same track "kick", "the kick", "kik", and "bd". Learning
// the fourth must not forget the first, because nobody experiences teaching a name
// as replacing one — a store that overwrote would lose vocabulary the producer has
// no way of knowing had gone. So learning accumulates (requirement 8.2), and
// teaching a name the track already answers to is a no-op that says so rather than
// a second copy (requirement 8.3, compared on `normalize_alias_for_matching` —
// the same normalized form the Object Resolver matches on, so "the Kick" and "the
// kick" are one alias here exactly as they are to resolution).
//
// Insertion order is held (requirement 8.4). That is what makes the candidate
// order resolution sees reproducible: the same project read twice, or read on
// another machine, offers the aliases in the same order, so an ambiguity reported
// to the producer does not reshuffle between calls.
//
// The set is capped at `maximum_aliases_per_track`, because a list with no bound
// grows the project file without one (requirement 8.5). An alias learned past the
// cap is rejected with the cap named in the reason, and nothing already held is
// touched (requirement 8.6). Dropping the oldest to make room is the failure this
// design exists to avoid, so it is not the overflow behaviour: the producer would
// lose a name they still use and never be told.
//
// Forgetting comes in two shapes, because the two things a producer means are
// different: forget this one name (requirement 8.7, matched on the same normalized
// form, the rest keeping their order), or forget everything this track answers to
// (requirement 8.8).
//
// ---------------------------------------------------------------------------
// Encoding
//
// The one genuinely subtle thing here, and it has two jobs now: make a single
// alias safe to put in a project file, and make several aliases into one value.
//
// A `.rpp` is line-oriented and space-delimited, and REAPER quotes a token
// containing spaces by picking a quote character from `"`, `'`, and a backtick.
// Which means a value carrying a newline terminates its own line, a value carrying
// a space relies on that quoting, and a value carrying all three quote characters
// cannot be quoted faithfully at all. None of that is hypothetical for this
// component: the values are *producer vocabulary* in seven languages, typed by a
// person, and "don't call it 'the drums'" is a perfectly ordinary thing for a
// producer to say.
//
// So nothing is trusted to REAPER's quoting. Text is percent-encoded on the way in
// and decoded on the way out, leaving a stored form that is a single token of safe
// bytes.
//
// Several aliases are then joined with `alias_separator`, and the separator is
// chosen from the characters the encoder already escapes. That is the whole trick,
// and it is worth stating plainly: because `|` is escaped inside an alias, a `|`
// in the stored value can only ever be one this component put there. Nothing has
// to be quoted, no lookahead is needed to split, and an alias containing a pipe
// round-trips exactly like any other. The escape character `%` is escaped for the
// same reason, so neither the delimiter nor the escape can be forged by anything a
// producer types.
//
// Two consequences worth having on purpose. A stored value with no separator in it
// reads back as exactly one alias, which is what a value written by an earlier
// single-alias build looks like — so those degrade gracefully instead of needing a
// migration. And the format stays legible: a producer who opens their `.rpp` in a
// text editor sees `sesh_alias` saying `kick|the%20kick|bd` and recognises their
// own vocabulary. A JSON array or a base64 blob would be shorter to write here and
// would turn "what did Sesh put in my project" into a question with no answer.
//
// No requirement names a delimiter or an encoding, and none should: no other
// component reads this value. It is decoded on the way out of the seam below, and
// every caller — including the Object Resolver — sees a `std::vector<std::string>`.
//
// ---------------------------------------------------------------------------
// Shape
//
// No SDK header, no `MediaTrack*`, nothing that calls REAPER. The API lives behind
// AliasStorage — the narrowest interface this component can state its needs in,
// following the seam-per-component convention set by
// `entry/timer_registration.h`. `daw/reaper_alias_storage.h` is the REAPER-backed
// implementation and `reaper_alias_storage.cpp` is the only translation unit that
// includes the SDK. The suite substitutes a fake, which is what makes the key
// naming, the encoding, and the save-state reporting testable outside REAPER.
//
// `daw/stored_alias_lookup.h` adapts this store to the Object Resolver's
// `LearnedAliasLookup` seam. It lives in its own header so that this one stays
// free of anything resolution owns.
//
// The definitions are `inline` in the header for the same build reason as
// `folder_invariant_keeper.h`: the Catch2 target compiles `tests/` and not `src/`,
// so logic the suite exercises has to be visible through the header.

#ifndef SESH_AI_DAW_ALIAS_STORE_H
#define SESH_AI_DAW_ALIAS_STORE_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sesh_ai {
namespace daw {

// A track, as this component needs to talk about one.
//
// Opaque on purpose. In the extension every value is a `MediaTrack*` and
// `reaper_alias_storage.cpp` casts it back; the suite fabricates handles. Holding
// it as `void*` is what keeps this header — and so the Object Resolver that
// consumes it — off the SDK include path.
using TrackHandle = void*;

// The `P_EXT:` key every alias a track holds lives under, and the project
// extension section for everything that is not about one track. Both namespaced
// (requirement 26.6).
//
// One key for the whole set, rather than `sesh_alias_1`, `sesh_alias_2`. A key per
// alias would make "forget the second one" a renumbering problem across keys, and
// would leave a half-written set behind if one of sixteen writes were refused.
inline constexpr const char* track_alias_extension_key = "P_EXT:sesh_alias";
inline constexpr const char* project_extension_section = "sesh";

// The longest single alias that will be stored, in bytes of UTF-8 before encoding.
//
// A bound is needed because the read side has to size a buffer, and it is stated
// here rather than in the REAPER implementation so that the limit is part of the
// contract the suite checks instead of a surprise from a buffer somewhere. 256
// bytes is roughly 85 characters of the most expensive script Sesh localises
// into — far past anything a producer says out loud to name a track.
inline constexpr std::size_t maximum_alias_length = 256;

// How many learned aliases one track holds at most. Requirement 8.5.
//
// Sixteen is chosen to be past what a producer plausibly needs and short of what
// bloats a project file. Four or five names for one track is an ordinary session;
// sixteen is a producer who has been talking to Sesh about the same kick for
// months. Past it, the honest answer is to say so — see
// `AliasWriteOutcome::rejected_cap_reached` — because the alternative, dropping
// the oldest, takes away a name the producer still uses without telling them.
inline constexpr std::size_t maximum_aliases_per_track = 16;

// What separates one encoded alias from the next in the stored value.
//
// Legible, meaningless to a `.rpp`, and — the part that matters — escaped by
// `ext_state_byte_needs_encoding`, so it cannot occur inside an encoded alias. A
// separator that could occur inside an encoded alias would need quoting rules of
// its own, which is how a round trip stops being exact.
inline constexpr char alias_separator = '|';

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Whether a byte has to be escaped before it goes into project state.
//
// Everything at or below `0x20` — every C0 control, tab, carriage return,
// newline, and space. Newline and carriage return would end the line the value
// sits on; space would leave the value dependent on REAPER's quoting; the rest
// have no business in a project file at all.
//
// `DEL`, for the same reason as the controls.
//
// The three characters REAPER quotes with, plus the two that delimit a chunk, plus
// the escape character itself, plus the separator. A value containing `"`, `'`,
// and a backtick cannot be quoted by a scheme that picks one of those three as its
// delimiter, which is the case that would silently corrupt rather than fail. And
// escaping the separator is what makes joining reversible: a producer is entitled
// to type a pipe.
//
// Bytes at or above `0x80` pass through. In valid UTF-8 those are lead and
// continuation bytes, REAPER stores UTF-8 in project files as-is, and
// percent-encoding a Japanese alias into nine times its length would be a cost
// with nothing bought.
inline bool ext_state_byte_needs_encoding(unsigned char byte)
{
	if (byte <= 0x20 || byte == 0x7f) {
		return true;
	}

	switch (byte) {
		case '%':
		case '"':
		case '\'':
		case '`':
		case '<':
		case '>':
			return true;
		default:
			return byte == static_cast<unsigned char>(alias_separator);
	}
}

// Percent-encodes the bytes that would break a project file, leaving the rest
// legible.
//
// Legibility is not decoration. These keys land in a producer's `.rpp`, and a
// producer who opens theirs in a text editor should be able to see that
// `sesh_alias` says `the%20drum%20bus` and recognise it. A base64 blob would be
// shorter to write and would turn "what did Sesh put in my project" into a
// question with no answer.
inline std::string encode_ext_state_text(const std::string& text)
{
	static constexpr char hexadecimal_digits[] = "0123456789ABCDEF";

	std::string encoded;
	encoded.reserve(text.size());

	for (const char character : text) {
		const unsigned char byte = static_cast<unsigned char>(character);

		if (!ext_state_byte_needs_encoding(byte)) {
			encoded.push_back(character);
			continue;
		}

		encoded.push_back('%');
		encoded.push_back(hexadecimal_digits[byte >> 4]);
		encoded.push_back(hexadecimal_digits[byte & 0x0f]);
	}

	return encoded;
}

// The value of a hexadecimal digit, or -1 when it is not one.
inline int hexadecimal_digit_value(unsigned char byte)
{
	if (byte >= '0' && byte <= '9') {
		return byte - '0';
	}

	if (byte >= 'a' && byte <= 'f') {
		return byte - 'a' + 10;
	}

	if (byte >= 'A' && byte <= 'F') {
		return byte - 'A' + 10;
	}

	return -1;
}

// Reverses encode_ext_state_text.
//
// A `%` that is not followed by two hexadecimal digits is kept as a literal `%`
// rather than dropped or treated as an error. Nothing this component writes
// produces that, but the project file is the producer's and a stored value can
// have been hand-edited, arrived from an older build, or been written by
// something else entirely under a key that looks like ours. Returning the alias
// slightly wrong beats returning nothing and having the resolver decide the
// producer never taught it.
inline std::string decode_ext_state_text(const std::string& stored)
{
	std::string decoded;
	decoded.reserve(stored.size());

	for (std::size_t index = 0; index < stored.size(); ++index) {
		if (stored[index] != '%' || index + 2 >= stored.size()) {
			decoded.push_back(stored[index]);
			continue;
		}

		const int high = hexadecimal_digit_value(static_cast<unsigned char>(stored[index + 1]));
		const int low = hexadecimal_digit_value(static_cast<unsigned char>(stored[index + 2]));

		if (high < 0 || low < 0) {
			decoded.push_back(stored[index]);
			continue;
		}

		decoded.push_back(static_cast<char>((high << 4) | low));
		index += 2;
	}

	return decoded;
}

// The longest value this component can write under the track alias key.
//
// Percent-encoding is at most three bytes per input byte, so a single alias
// encodes to at most `3 * maximum_alias_length`, and the set adds one separator
// per alias. Published here rather than worked out again in
// `reaper_alias_storage.cpp`, because the read buffer over there has to hold this
// and a buffer that silently truncated the value would lose aliases — which is the
// one failure this whole design is arranged around.
inline constexpr std::size_t maximum_stored_alias_value_length =
	maximum_aliases_per_track * (3 * maximum_alias_length + 1);

// Joins encoded aliases into the one value that goes under the key.
//
// Insertion order in, insertion order out. An empty list produces an empty value,
// which is how REAPER is told to drop the key entirely.
inline std::string encode_alias_list(const std::vector<std::string>& aliases)
{
	std::string encoded;

	for (const std::string& alias : aliases) {
		if (!encoded.empty()) {
			encoded.push_back(alias_separator);
		}

		encoded += encode_ext_state_text(alias);
	}

	return encoded;
}

// Splits a stored value back into the aliases it holds, in the order they were
// written.
//
// A value with no separator is one alias — which is also what a value written by
// an earlier single-alias build is, so it reads correctly with nothing to migrate.
//
// Empty pieces are skipped rather than returned as empty aliases. Nothing this
// component writes produces a leading, trailing, or doubled separator; a
// hand-edited file can, and an empty alias is not vocabulary.
//
// More than `maximum_aliases_per_track` pieces are all returned. The cap governs
// what this component writes, and a value that arrived from somewhere else is
// reported as it actually is — trimming it here would be the silent dropping the
// cap exists to refuse, and no write can push the count any higher because
// learning past the cap is rejected.
inline std::vector<std::string> decode_alias_list(const std::string& stored)
{
	std::vector<std::string> aliases;

	std::size_t piece_start = 0;

	while (piece_start <= stored.size()) {
		std::size_t separator = stored.find(alias_separator, piece_start);

		if (separator == std::string::npos) {
			separator = stored.size();
		}

		if (separator > piece_start) {
			aliases.push_back(
				decode_ext_state_text(stored.substr(piece_start, separator - piece_start)));
		}

		piece_start = separator + 1;
	}

	return aliases;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// Whether a byte is whitespace for trimming purposes. ASCII only — a UTF-8
// no-break space is a byte sequence above 0x7f and is left alone, since it is
// part of the alias rather than padding around it.
inline bool is_alias_whitespace(unsigned char byte)
{
	return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f';
}

// Strips leading and trailing whitespace. What gets stored is the trimmed text:
// a trailing space is a typing artefact, not vocabulary.
inline std::string trim_alias(const std::string& alias)
{
	std::size_t first = 0;
	std::size_t last = alias.size();

	while (first < last && is_alias_whitespace(static_cast<unsigned char>(alias[first]))) {
		++first;
	}

	while (last > first && is_alias_whitespace(static_cast<unsigned char>(alias[last - 1]))) {
		--last;
	}

	return alias.substr(first, last - first);
}

// The form two aliases are compared in: trimmed, internal whitespace runs
// collapsed to one space, ASCII letters lowercased.
//
// This is a matching key and never a storage form — the alias is stored as the
// producer typed it, because it gets read back and shown to them.
//
// It is the form the duplicate check in `learn_track_alias` and the target of
// `forget_track_alias` are compared in as well (requirements 8.3 and 8.7), which
// is not a convenience: comparing on anything else would let a track hold two
// aliases the Object Resolver cannot tell apart, so that teaching "The Kick" to a
// track that answers to "the kick" produced an ambiguity out of one name.
//
// Deliberately ASCII-only in its case folding. Case folding the rest of Unicode
// correctly needs a table this extension has no reason to carry, and folding it
// incorrectly — byte-wise, or with `tolower` under whatever locale REAPER
// happens to be in — is worse than not folding it, because it produces matches
// that depend on the machine. A producer working in German gets exact matching on
// the non-ASCII part of their vocabulary, which is honest; they do not get a
// match that works on their laptop and fails in the studio.
inline std::string normalize_alias_for_matching(const std::string& alias)
{
	const std::string trimmed = trim_alias(alias);

	std::string normalized;
	normalized.reserve(trimmed.size());

	bool previous_was_whitespace = false;

	for (const char character : trimmed) {
		const unsigned char byte = static_cast<unsigned char>(character);

		if (is_alias_whitespace(byte)) {
			if (!previous_was_whitespace) {
				normalized.push_back(' ');
			}

			previous_was_whitespace = true;
			continue;
		}

		previous_was_whitespace = false;

		if (byte >= 'A' && byte <= 'Z') {
			normalized.push_back(static_cast<char>(byte - 'A' + 'a'));
			continue;
		}

		normalized.push_back(character);
	}

	return normalized;
}

// ---------------------------------------------------------------------------
// The REAPER seam
// ---------------------------------------------------------------------------

// What this component needs from REAPER, and nothing else: read and write a
// string extension value on a track, read and write project extension state, walk
// the track list, and ask whether the project has a saved state on disk.
//
// Eight calls. The track walk is here because the Object Resolver's second
// resolution source is "which tracks answer to this alias", and a lookup by alias
// has to enumerate — REAPER offers no index from extension value back to track,
// for the same reason it offers none from GUID back to track.
class AliasStorage {
public:
	virtual ~AliasStorage() = default;

	virtual int track_count() const = 0;

	// Null when the index is out of range, which is how a track list that changed
	// under an enumeration reports itself rather than as a crash.
	virtual TrackHandle track_at(int index) const = 0;

	// The stored, still-encoded value. Empty when the key is absent — REAPER draws
	// no distinction between an absent extension key and an empty one, so neither
	// does this.
	virtual std::string track_extension_value(TrackHandle track, const std::string& key) const = 0;

	// False when REAPER refused the write. An alias reported as learned that was
	// not written is the failure this return value exists to prevent.
	virtual bool set_track_extension_value(TrackHandle track, const std::string& key, const std::string& value) = 0;

	virtual std::string project_extension_value(const std::string& section, const std::string& key) const = 0;

	virtual bool set_project_extension_value(const std::string& section, const std::string& key, const std::string& value) = 0;

	// Whether the project exists as a file on disk. False for an untitled session
	// that has never been saved — the case where a learned alias is certain not to
	// survive the next launch.
	virtual bool project_has_been_saved() const = 0;

	// Whether the project has edits not yet on disk.
	//
	// Supplementary rather than sufficient, and the reason is REAPER's: the API
	// this comes from always reports clean when the producer has turned off
	// "undo/prompt to save" in preferences. So it can sharpen the answer but
	// cannot carry it — see AliasStore::project_saved.
	virtual bool project_has_unsaved_changes() const = 0;
};

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

// What happened to a write. A single bool would collapse "the producer sent
// whitespace" into "REAPER refused", and those want different things said to the
// producer.
enum class AliasWriteOutcome {
	// Written, and the track now answers to one more name than it did. For a
	// project-state mapping this means written into the in-memory project, which
	// reaches disk on the next save.
	learned,

	// The track already answered to this name, under the normalized form the Object
	// Resolver matches on, so nothing was written. Requirement 8.3. Distinct from
	// `learned` because it is worth saying back: the producer learns that Sesh
	// already knew, rather than being told a name was added that was not.
	already_held,

	// Removed — one named alias, every alias on a track, or a mapping.
	forgotten,

	// Nothing but whitespace. There is no alias here to learn and no instruction to
	// forget one either, so the store does not guess between them.
	rejected_empty,

	// Longer than maximum_alias_length. Refused rather than truncated: half an
	// alias is a different alias, and it would resolve to the wrong track quietly.
	rejected_too_long,

	// The track already holds maximum_aliases_per_track aliases. Requirement 8.6.
	// The aliases already held are untouched, and LearnedAlias::reason names the
	// cap so the producer can be told what to do about it.
	rejected_cap_reached,

	// REAPER refused the write.
	storage_refused,
};

// The cap, in words a producer can act on. Built from the constant rather than
// written out, so the number in the message cannot drift from the number enforced.
inline std::string per_track_alias_cap_reason()
{
	return "this track already holds the maximum of " + std::to_string(maximum_aliases_per_track)
		+ " learned aliases, so nothing was changed — forget one before teaching another";
}

// The outcome of a learn, with the save state that has to be reported beside it.
struct LearnedAlias {
	AliasWriteOutcome outcome{AliasWriteOutcome::rejected_empty};

	// The alias this call was about, as stored — trimmed, otherwise exactly what the
	// producer said. Empty for a refusal.
	//
	// For `already_held` this is the alias the track *already* holds rather than the
	// text just typed. The two differ only in case and internal spacing, and the
	// stored one is the more useful thing to say back: it is what the producer will
	// see if they ask what the track answers to.
	std::string alias;

	// Every alias the track holds after this call, in the order they were learned.
	// Requirement 8.4, and the thing to show a producer who has just been told their
	// alias was rejected or already known.
	//
	// Unchanged from before the call for every outcome except `learned` — which for
	// `rejected_cap_reached` is requirement 8.6's "no alias is dropped to make room",
	// stated in the return value rather than only in a comment.
	std::vector<std::string> aliases_held;

	// Why, when the outcome needs more than its name. Populated for
	// `rejected_cap_reached`, whose requirement asks for the cap to be named; empty
	// otherwise, because an outcome that explains itself does not need prose.
	std::string reason;

	// Requirement 8.10, and `projectSaved` in the `set_track_state` output schema.
	//
	// Captured *before* the write, which is not a detail: writing an alias dirties
	// the project, so a save state sampled afterwards would report "unsaved" for
	// every learn and say nothing about the session the producer is actually in.
	bool project_saved{false};

	bool learned() const { return outcome == AliasWriteOutcome::learned; }

	// True when the track answers to the alias as a result of this call, whether or
	// not this call is what taught it. The question most callers actually have.
	bool held() const
	{
		return outcome == AliasWriteOutcome::learned || outcome == AliasWriteOutcome::already_held;
	}
};

// Reads and writes learned vocabulary. Owns no state of its own — every read goes
// to the project, because the project is where the truth is and a cache would go
// stale the first time the producer renamed something in REAPER's own UI.
class AliasStore {
public:
	explicit AliasStore(AliasStorage& storage)
		: storage_{storage}
	{
	}

	// Whether the alias the producer just taught will still be there next launch.
	//
	// Both signals, and the AND is the conservative direction on purpose. A session
	// with no file on disk loses the alias for certain. A session with a file but
	// pending edits loses it too, and that is the case the dirty check catches —
	// when REAPER is willing to report it. When it is not (the producer disabled
	// the save prompt, so the API always answers clean) this degrades to the file
	// check, which is the strongest thing left and is stated rather than hidden.
	//
	// So a false is always worth acting on, and a true means "saved session, and
	// this alias joins it on the next save" — which is why the schema words the
	// false case as the thing to tell the producer.
	bool project_saved() const
	{
		return storage_.project_has_been_saved() && !storage_.project_has_unsaved_changes();
	}

	// Adds an alias to the ones the track already holds. Requirements 8.1 through
	// 8.6.
	//
	// The order of the checks is the order of the requirements, and the two reads
	// before the write are what make accumulation safe: the existing set is read,
	// the new alias is appended to it, and the whole set is written back. Writing
	// only the new alias is the bug this method is a rewrite of.
	//
	// An empty alias is a refusal rather than an instruction to forget. Under a
	// single-alias store the two were indistinguishable — clearing the value was
	// forgetting — but with two forget operations of its own, guessing that an empty
	// string means "forget everything" would be this component deciding to delete
	// vocabulary nobody asked it to.
	LearnedAlias learn_track_alias(TrackHandle track, const std::string& alias)
	{
		LearnedAlias learned;

		// Before the write, for the reason on LearnedAlias::project_saved.
		learned.project_saved = project_saved();

		if (track == nullptr) {
			learned.outcome = AliasWriteOutcome::storage_refused;
			return learned;
		}

		const std::string trimmed = trim_alias(alias);

		if (trimmed.empty()) {
			learned.outcome = AliasWriteOutcome::rejected_empty;
			learned.aliases_held = track_aliases(track);
			return learned;
		}

		if (trimmed.size() > maximum_alias_length) {
			learned.outcome = AliasWriteOutcome::rejected_too_long;
			learned.aliases_held = track_aliases(track);
			return learned;
		}

		std::vector<std::string> held = track_aliases(track);

		// Requirement 8.3. Compared on the normalized form, so the producer does not
		// end up with "The Kick" and "the kick" as two aliases resolution cannot
		// choose between.
		const std::string wanted = normalize_alias_for_matching(trimmed);

		for (const std::string& existing : held) {
			if (normalize_alias_for_matching(existing) != wanted) {
				continue;
			}

			learned.outcome = AliasWriteOutcome::already_held;
			learned.alias = existing;
			learned.aliases_held = std::move(held);

			return learned;
		}

		// Requirement 8.6. Nothing is dropped to make room, and the reason names the
		// cap so the producer can be told which of their names to give up rather than
		// having one chosen for them.
		if (held.size() >= maximum_aliases_per_track) {
			learned.outcome = AliasWriteOutcome::rejected_cap_reached;
			learned.reason = per_track_alias_cap_reason();
			learned.aliases_held = std::move(held);

			return learned;
		}

		held.push_back(trimmed);

		if (!storage_.set_track_extension_value(
				track, track_alias_extension_key, encode_alias_list(held))) {
			learned.outcome = AliasWriteOutcome::storage_refused;
			return learned;
		}

		learned.outcome = AliasWriteOutcome::learned;
		learned.alias = trimmed;
		learned.aliases_held = std::move(held);

		return learned;
	}

	// Removes one named alias and leaves the rest of the track's vocabulary alone,
	// in the order it was already in. Requirement 8.7.
	//
	// Matched on the normalized form, the same one `learn_track_alias` compares on,
	// so a producer can forget "the kick" by saying "The Kick" — they should not have
	// to remember which casing they taught.
	//
	// Forgetting one that was never there succeeds without a write: the
	// post-condition asked for is "this track does not answer to that name", and it
	// already holds. Writing anyway would dirty the producer's project to change
	// nothing.
	//
	// Every match is removed, not the first. Only a value written outside Sesh can
	// hold the same normalized alias twice, and leaving the second behind would mean
	// the track still answered to a name the producer just forgot.
	AliasWriteOutcome forget_track_alias(TrackHandle track, const std::string& alias)
	{
		if (track == nullptr) {
			return AliasWriteOutcome::storage_refused;
		}

		const std::string wanted = normalize_alias_for_matching(alias);

		// No name to forget, and not an instruction to forget them all either — that
		// is `forget_all_track_aliases`, and guessing between the two would be this
		// component deleting vocabulary on a hunch.
		if (wanted.empty()) {
			return AliasWriteOutcome::rejected_empty;
		}

		const std::vector<std::string> held = track_aliases(track);

		std::vector<std::string> remaining;
		remaining.reserve(held.size());

		for (const std::string& existing : held) {
			if (normalize_alias_for_matching(existing) != wanted) {
				remaining.push_back(existing);
			}
		}

		if (remaining.size() == held.size()) {
			return AliasWriteOutcome::forgotten;
		}

		if (!storage_.set_track_extension_value(
				track, track_alias_extension_key, encode_alias_list(remaining))) {
			return AliasWriteOutcome::storage_refused;
		}

		return AliasWriteOutcome::forgotten;
	}

	// Removes every alias the track holds. Requirement 8.8.
	//
	// An empty value is how REAPER is told to drop the key, so the track is left
	// exactly as it was before Sesh ever saw it rather than carrying an empty one.
	// Doing this to a track that held nothing succeeds, for the same reason as above.
	AliasWriteOutcome forget_all_track_aliases(TrackHandle track)
	{
		if (track == nullptr) {
			return AliasWriteOutcome::storage_refused;
		}

		if (!storage_.set_track_extension_value(track, track_alias_extension_key, std::string{})) {
			return AliasWriteOutcome::storage_refused;
		}

		return AliasWriteOutcome::forgotten;
	}

	// What this track answers to, in the order it was taught, or empty when it
	// answers to nothing. Requirement 8.4, and one of the two reads the Object
	// Resolver needs.
	std::vector<std::string> track_aliases(TrackHandle track) const
	{
		if (track == nullptr) {
			return std::vector<std::string>{};
		}

		return decode_alias_list(
			storage_.track_extension_value(track, track_alias_extension_key));
	}

	// Every track carrying aliases, in track order, paired with them. The Object
	// Resolver's alias source in one walk rather than a walk plus a lookup per
	// track.
	std::vector<std::pair<TrackHandle, std::vector<std::string>>> all_track_aliases() const
	{
		std::vector<std::pair<TrackHandle, std::vector<std::string>>> aliases;

		const int count = storage_.track_count();

		for (int index = 0; index < count; ++index) {
			const TrackHandle track = storage_.track_at(index);

			if (track == nullptr) {
				continue;
			}

			std::vector<std::string> held = track_aliases(track);

			if (held.empty()) {
				continue;
			}

			aliases.emplace_back(track, std::move(held));
		}

		return aliases;
	}

	// Which tracks answer to this alias — the resolver's other read, and the reason
	// the seam can walk the track list.
	//
	// A track matches when any one of the aliases it holds does, which is the point
	// of holding several.
	//
	// Every match, in track order, not the first. Two tracks carrying the same
	// alias is a real state a producer can reach, and requirement 7.4 wants it
	// reported as ambiguous with both candidates named. Returning one would make
	// this component the place a wrong track got chosen. Each track appears once
	// however many of its aliases matched — a track is one candidate.
	std::vector<TrackHandle> tracks_with_alias(const std::string& alias) const
	{
		std::vector<TrackHandle> matches;

		const std::string wanted = normalize_alias_for_matching(alias);

		if (wanted.empty()) {
			return matches;
		}

		const int count = storage_.track_count();

		for (int index = 0; index < count; ++index) {
			const TrackHandle track = storage_.track_at(index);

			if (track == nullptr) {
				continue;
			}

			for (const std::string& held : track_aliases(track)) {
				if (normalize_alias_for_matching(held) == wanted) {
					matches.push_back(track);
					break;
				}
			}
		}

		return matches;
	}

	// Requirement 8.9 — a mapping with no track to hang from. Same encoding on the
	// key as on the value: the key is producer vocabulary too, and a key with a
	// space in it is a key `GetProjExtState` will not find again.
	//
	// One name, one value, and no accumulation: a mapping is a name for a thing
	// rather than one of several names for a track, so learning it again replaces
	// what it meant, which is what the producer asked for.
	LearnedAlias learn_mapping(const std::string& name, const std::string& value)
	{
		LearnedAlias learned;
		learned.project_saved = project_saved();

		const std::string trimmed_name = trim_alias(name);

		if (trimmed_name.empty()) {
			learned.outcome = AliasWriteOutcome::rejected_empty;
			return learned;
		}

		if (trimmed_name.size() > maximum_alias_length || value.size() > maximum_alias_length) {
			learned.outcome = AliasWriteOutcome::rejected_too_long;
			return learned;
		}

		if (value.empty()) {
			learned.outcome = storage_.set_project_extension_value(
					project_extension_section, encode_ext_state_text(trimmed_name), std::string{})
				? AliasWriteOutcome::forgotten
				: AliasWriteOutcome::storage_refused;

			return learned;
		}

		if (!storage_.set_project_extension_value(project_extension_section,
				encode_ext_state_text(trimmed_name), encode_ext_state_text(value))) {
			learned.outcome = AliasWriteOutcome::storage_refused;
			return learned;
		}

		learned.outcome = AliasWriteOutcome::learned;
		learned.alias = trimmed_name;

		return learned;
	}

	// The value last *saved* under this name. Empty for a name that was never
	// learned — and also for one learned in this session and not yet saved, which
	// is exactly the loss requirement 8.10 makes the store announce up front.
	std::string mapping(const std::string& name) const
	{
		const std::string trimmed_name = trim_alias(name);

		if (trimmed_name.empty()) {
			return std::string{};
		}

		return decode_ext_state_text(storage_.project_extension_value(
			project_extension_section, encode_ext_state_text(trimmed_name)));
	}

	AliasWriteOutcome forget_mapping(const std::string& name)
	{
		const std::string trimmed_name = trim_alias(name);

		if (trimmed_name.empty()) {
			return AliasWriteOutcome::rejected_empty;
		}

		if (!storage_.set_project_extension_value(
				project_extension_section, encode_ext_state_text(trimmed_name), std::string{})) {
			return AliasWriteOutcome::storage_refused;
		}

		return AliasWriteOutcome::forgotten;
	}

private:
	AliasStorage& storage_;
};

}  // namespace daw
}  // namespace sesh_ai

#endif  // SESH_AI_DAW_ALIAS_STORE_H
