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
// are resolved from the registration table's `GetFunc` at construction rather than
// imported through REAPERAPI_IMPLEMENT; see reaper_alias_storage.h for why. The
// checks are the same ones that were always here — they read a member now instead
// of a global.
//
// And the string calls write into a caller-supplied buffer. There is no "how big?"
// query, so the size is derived from the alias bound the store publishes rather
// than guessed.
//
// `daw/reaper_alias_storage.h` comes before the SDK, which is load-bearing rather
// than tidy: on macOS and Linux the SDK reaches for WDL's swell, whose `min` and
// `max` function-like macros eat a qualified `std::min` parsed after them.

#include <daw/reaper_alias_storage.h>

#include <reaper_plugin.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace sesh_ai {
namespace daw {

// The seven functions this seam needs, and only those. Resolved once at
// construction; none of them is required, because every call below already has an
// answer for the one it cannot make.
struct ReaperAliasStorage::reaper_alias_api {
	int (*CountTracks)(ReaProject*) = nullptr;
	MediaTrack* (*GetTrack)(ReaProject*, int) = nullptr;
	bool (*GetSetMediaTrackInfo_String)(MediaTrack*, const char*, char*, bool) = nullptr;

	int (*GetProjExtState)(ReaProject*, const char*, const char*, char*, int) = nullptr;
	int (*SetProjExtState)(ReaProject*, const char*, const char*, const char*) = nullptr;

	void (*GetProjectName)(ReaProject*, char*, int) = nullptr;
	int (*IsProjectDirty)(ReaProject*) = nullptr;
};

namespace {

// Resolves one function and records the name when it is missing, so a storage that
// degraded can say which functions REAPER did not supply rather than only that an
// alias went missing.
template <typename FunctionPointer>
void resolve_function(
	reaper_plugin_info_t* plugin_info,
	const char* name,
	FunctionPointer& destination,
	std::vector<std::string>& unresolved_names)
{
	void* const resolved = plugin_info->GetFunc(name);

	if (resolved == nullptr) {
		unresolved_names.emplace_back(name);
		return;
	}

	destination = reinterpret_cast<FunctionPointer>(resolved);
}

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

ReaperAliasStorage::ReaperAliasStorage(reaper_plugin_info_t* plugin_info, ReaProject* project)
	: api_{std::make_unique<reaper_alias_api>()}
	, project_{project}
{
	if (plugin_info == nullptr || plugin_info->GetFunc == nullptr) {
		// Every pointer stays null, so the store reads no aliases and refuses every
		// write — which is what it already did on a REAPER that exposed none of
		// these, and the direction worth failing in: an alias reported as learned
		// that was not written is the failure the return values exist to prevent.
		unresolved_function_names_.emplace_back("GetFunc");
		return;
	}

	resolve_function(plugin_info, "CountTracks", api_->CountTracks, unresolved_function_names_);
	resolve_function(plugin_info, "GetTrack", api_->GetTrack, unresolved_function_names_);
	resolve_function(plugin_info, "GetSetMediaTrackInfo_String", api_->GetSetMediaTrackInfo_String,
		unresolved_function_names_);

	resolve_function(plugin_info, "GetProjExtState", api_->GetProjExtState, unresolved_function_names_);
	resolve_function(plugin_info, "SetProjExtState", api_->SetProjExtState, unresolved_function_names_);

	resolve_function(plugin_info, "GetProjectName", api_->GetProjectName, unresolved_function_names_);
	resolve_function(plugin_info, "IsProjectDirty", api_->IsProjectDirty, unresolved_function_names_);
}

ReaperAliasStorage::~ReaperAliasStorage() = default;

int ReaperAliasStorage::track_count() const
{
	if (api_->CountTracks == nullptr) {
		return 0;
	}

	return api_->CountTracks(project_);
}

TrackHandle ReaperAliasStorage::track_at(int index) const
{
	if (api_->GetTrack == nullptr || index < 0) {
		return nullptr;
	}

	// GetTrack answers null for an out-of-range index, which is the behaviour the
	// seam's contract promises for a track list that changed mid-enumeration.
	return api_->GetTrack(project_, index);
}

std::string ReaperAliasStorage::track_extension_value(TrackHandle track, const std::string& key) const
{
	if (api_->GetSetMediaTrackInfo_String == nullptr || track == nullptr) {
		return std::string{};
	}

	std::vector<char> buffer = make_read_buffer();

	// False means the key is absent, which REAPER does not distinguish from an empty
	// value — and neither does the seam's contract.
	if (!api_->GetSetMediaTrackInfo_String(as_media_track(track), key.c_str(), buffer.data(), false)) {
		return std::string{};
	}

	return std::string{buffer.data()};
}

bool ReaperAliasStorage::set_track_extension_value(
	TrackHandle track, const std::string& key, const std::string& value)
{
	if (api_->GetSetMediaTrackInfo_String == nullptr || track == nullptr) {
		return false;
	}

	// The parameter is `char*` in both directions even when setting, so the value
	// has to be handed over as a mutable buffer. REAPER copies it into the track
	// chunk; nothing reads back out of this one.
	std::vector<char> writable_value(value.begin(), value.end());
	writable_value.push_back('\0');

	return api_->GetSetMediaTrackInfo_String(
		as_media_track(track), key.c_str(), writable_value.data(), true);
}

std::string ReaperAliasStorage::project_extension_value(
	const std::string& section, const std::string& key) const
{
	if (api_->GetProjExtState == nullptr) {
		return std::string{};
	}

	std::vector<char> buffer = make_read_buffer();

	// What comes back is the value as of the last save, not the value set in this
	// session. That is REAPER's documented behaviour and the entire reason
	// requirement 8.10 exists — the store reports the project's save state beside
	// every learn so the producer hears about it at the right moment instead of
	// finding the alias missing next week.
	api_->GetProjExtState(project_, section.c_str(), key.c_str(), buffer.data(),
		static_cast<int>(buffer.size()));

	return std::string{buffer.data()};
}

bool ReaperAliasStorage::set_project_extension_value(
	const std::string& section, const std::string& key, const std::string& value)
{
	if (api_->SetProjExtState == nullptr) {
		return false;
	}

	// Returns the size of the section's state afterwards, not a success flag — and
	// zero is the correct answer to the delete an empty value performs, so treating
	// it as one would report every forget as a failure. The only refusal this seam
	// can detect is the missing function pointer checked above.
	api_->SetProjExtState(project_, section.c_str(), key.c_str(), value.c_str());

	return true;
}

bool ReaperAliasStorage::project_has_been_saved() const
{
	if (api_->GetProjectName == nullptr) {
		return false;
	}

	std::vector<char> buffer = make_read_buffer();

	api_->GetProjectName(project_, buffer.data(), static_cast<int>(buffer.size()));

	// A project that has never been saved has no name. This is the reliable half of
	// the save-state answer.
	return buffer[0] != '\0';
}

bool ReaperAliasStorage::project_has_unsaved_changes() const
{
	if (api_->IsProjectDirty == nullptr) {
		return false;
	}

	// Reports clean unconditionally when the producer has turned off "undo/prompt to
	// save" in REAPER's preferences. Answering false there is the same thing REAPER
	// answers, which is why the store treats this as a signal that can sharpen its
	// save-state report rather than one that can carry it.
	return api_->IsProjectDirty(project_) != 0;
}

}  // namespace daw
}  // namespace sesh_ai
