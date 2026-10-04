// The REAPER side of the Alias Store's seam.
//
// SDK-free, like `entry/reaper_timer_registrar.h` and for the same reason:
// `ReaProject` is forward-declared and only ever held as a pointer, so the Tool
// Executor can construct a REAPER-backed storage without pulling
// reaper_plugin_functions.h — which the test target has no include path for, by
// design.
//
// `reaper_alias_storage.cpp` is the only translation unit here that sees the SDK.

#ifndef SESH_AI_DAW_REAPER_ALIAS_STORAGE_H
#define SESH_AI_DAW_REAPER_ALIAS_STORAGE_H

#include <string>

#include <daw/alias_store.h>

// Declared by the SDK as `class ReaProject;`, so naming it here refers to that
// type rather than introducing a second one.
class ReaProject;

namespace sesh_ai {
namespace daw {

// Alias storage backed by the live project.
//
// Every TrackHandle crossing this boundary is a `MediaTrack*`. The cast lives in
// the implementation and nowhere else, which is the whole point of the handle
// being opaque in alias_store.h.
class ReaperAliasStorage final : public AliasStorage {
public:
	// Null means REAPER's active project, which is the API's own convention for a
	// null `ReaProject*` — so the default is "whatever the producer is looking at"
	// without this class having to resolve that itself.
	explicit ReaperAliasStorage(ReaProject* project = nullptr);

	int track_count() const override;
	TrackHandle track_at(int index) const override;

	std::string track_extension_value(TrackHandle track, const std::string& key) const override;
	bool set_track_extension_value(TrackHandle track, const std::string& key, const std::string& value) override;

	std::string project_extension_value(const std::string& section, const std::string& key) const override;
	bool set_project_extension_value(const std::string& section, const std::string& key, const std::string& value) override;

	bool project_has_been_saved() const override;
	bool project_has_unsaved_changes() const override;

private:
	ReaProject* project_;
};

}  // namespace daw
}  // namespace sesh_ai

#endif  // SESH_AI_DAW_REAPER_ALIAS_STORAGE_H
