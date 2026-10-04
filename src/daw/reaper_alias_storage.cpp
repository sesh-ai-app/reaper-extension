// The eight REAPER calls the Alias Store needs, and nothing else.
//
// Keeping them alone in a translation unit is what makes the key naming, the
// encoding, and the save-state reporting in alias_store.h testable without
// REAPER: the suite substitutes an AliasStorage and never links this.
//
// Two things about the REAPER API shape show up in every function below.
//
// The API arrives as function pointers resolved at load, and the SDK is explicit
// that a caller must never assume one exists — an older REAPER may not carry it.
// So each call is null-checked, and a missing function reads as "no alias" or
// "write refused" rather than as a crash in the producer's session. The pointers
// themselves get their storage from the one translation unit that defines
// REAPERAPI_IMPLEMENT, which belongs with the plugin entry (task 17.1); this file
// includes the header the way the SDK says most files should.
//
// And the string calls write into a caller-supplied buffer. There is no "how big?"
// query, so the size is derived from the alias bound the store publishes rather
// than guessed.

#include <daw/reaper_alias_storage.h>

#include <cstddef>
#include <vector>

#include <reaper_plugin_functions.h>

namespace sesh_ai {
namespace daw {

namespace {

// Comfortably past anything the store can produce, and derived from the store's own
// published bound rather than picked: a track holds up to maximum_aliases_per_track
// aliases, each at most maximum_alias_length bytes, each of those at most three
// bytes once percent-encoded, plus a separator apiece —
// maximum_stored_alias_value_length, which alias_store.h works out so that this
// buffer and that encoding cannot drift apart. A buffer too small for a full set
// would truncate the value and lose aliases, which is the one failure the whole
// accumulating design is arranged around.
//
// The slack on top covers the terminator, a project path (this buffer reads those
// too), and a longer value that arrived from somewhere other than Sesh under a key
// that looks like one of ours.
//
// A value longer than this is truncated on the way in. That is not reachable
// through the store, and the alternative — a growing re-read driven by a return
// value the SDK header does not document — would be guessing at the API to handle
// a case Sesh does not create.
constexpr std::size_t extension_value_buffer_size = maximum_stored_alias_value_length + 4096;

// Zero-filled, and the string is built from the C string REAPER leaves behind, so
// a call that writes nothing reads as empty rather than as whatever was in memory.
std::vector<char> make_read_buffer()
{
	return std::vector<char>(extension_value_buffer_size, '\0');
}

MediaTrack* as_media_track(TrackHandle track)
{
	return static_cast<MediaTrack*>(track);
}

}  // namespace

ReaperAliasStorage::ReaperAliasStorage(ReaProject* project)
	: project_{project}
{
}

int ReaperAliasStorage::track_count() const
{
	if (CountTracks == nullptr) {
		return 0;
	}

	return CountTracks(project_);
}

TrackHandle ReaperAliasStorage::track_at(int index) const
{
	if (GetTrack == nullptr || index < 0) {
		return nullptr;
	}

	// GetTrack answers null for an out-of-range index, which is the behaviour the
	// seam's contract promises for a track list that changed mid-enumeration.
	return GetTrack(project_, index);
}

std::string ReaperAliasStorage::track_extension_value(TrackHandle track, const std::string& key) const
{
	if (GetSetMediaTrackInfo_String == nullptr || track == nullptr) {
		return std::string{};
	}

	std::vector<char> buffer = make_read_buffer();

	// False means the key is absent, which REAPER does not distinguish from an empty
	// value — and neither does the seam's contract.
	if (!GetSetMediaTrackInfo_String(as_media_track(track), key.c_str(), buffer.data(), false)) {
		return std::string{};
	}

	return std::string{buffer.data()};
}

bool ReaperAliasStorage::set_track_extension_value(
	TrackHandle track, const std::string& key, const std::string& value)
{
	if (GetSetMediaTrackInfo_String == nullptr || track == nullptr) {
		return false;
	}

	// The parameter is `char*` in both directions even when setting, so the value
	// has to be handed over as a mutable buffer. REAPER copies it into the track
	// chunk; nothing reads back out of this one.
	std::vector<char> writable_value(value.begin(), value.end());
	writable_value.push_back('\0');

	return GetSetMediaTrackInfo_String(as_media_track(track), key.c_str(), writable_value.data(), true);
}

std::string ReaperAliasStorage::project_extension_value(
	const std::string& section, const std::string& key) const
{
	if (GetProjExtState == nullptr) {
		return std::string{};
	}

	std::vector<char> buffer = make_read_buffer();

	// What comes back is the value as of the last save, not the value set in this
	// session. That is REAPER's documented behaviour and the entire reason
	// requirement 8.10 exists — the store reports the project's save state beside
	// every learn so the producer hears about it at the right moment instead of
	// finding the alias missing next week.
	GetProjExtState(project_, section.c_str(), key.c_str(), buffer.data(),
		static_cast<int>(buffer.size()));

	return std::string{buffer.data()};
}

bool ReaperAliasStorage::set_project_extension_value(
	const std::string& section, const std::string& key, const std::string& value)
{
	if (SetProjExtState == nullptr) {
		return false;
	}

	// Returns the size of the section's state afterwards, not a success flag — and
	// zero is the correct answer to the delete an empty value performs, so treating
	// it as one would report every forget as a failure. The only refusal this seam
	// can detect is the missing function pointer checked above.
	SetProjExtState(project_, section.c_str(), key.c_str(), value.c_str());

	return true;
}

bool ReaperAliasStorage::project_has_been_saved() const
{
	if (GetProjectName == nullptr) {
		return false;
	}

	std::vector<char> buffer = make_read_buffer();

	GetProjectName(project_, buffer.data(), static_cast<int>(buffer.size()));

	// A project that has never been saved has no name. This is the reliable half of
	// the save-state answer.
	return buffer[0] != '\0';
}

bool ReaperAliasStorage::project_has_unsaved_changes() const
{
	if (IsProjectDirty == nullptr) {
		return false;
	}

	// Reports clean unconditionally when the producer has turned off "undo/prompt to
	// save" in REAPER's preferences. Answering false there is the same thing REAPER
	// answers, which is why the store treats this as a signal that can sharpen its
	// save-state report rather than one that can carry it.
	return IsProjectDirty(project_) != 0;
}

}  // namespace daw
}  // namespace sesh_ai
