// The REAPER side of the Alias Store's seam.
//
// SDK-free, like `entry/reaper_timer_registrar.h` and for the same reason:
// `ReaProject` and `reaper_plugin_info_t` are forward-declared and only ever held
// as pointers, so the Tool Executor can construct a REAPER-backed storage without
// pulling the SDK headers — which the test target has no include path for, by
// design.
//
// `reaper_alias_storage.cpp` is the only translation unit here that sees the SDK.
//
// The seven API functions are resolved from the registration table's `GetFunc` at
// construction rather than through the SDK's REAPERAPI_IMPLEMENT import table,
// following `reaper_render_host.h` and the other REAPER-backed adapters.
// REAPERAPI_IMPLEMENT would need exactly one translation unit providing storage for
// every imported pointer — global mutable state that has to be initialised before
// anything else in the library runs, and that turns a function an older REAPER does
// not expose into a null call at the point of use. Resolving here keeps that a value
// this object checks, which is what every null check in the implementation already
// did — they read a member now rather than a global, and nothing else moved.

#ifndef SESH_AI_DAW_REAPER_ALIAS_STORAGE_H
#define SESH_AI_DAW_REAPER_ALIAS_STORAGE_H

#include <memory>
#include <string>
#include <vector>

#include <daw/alias_store.h>

// Declared by the SDK as `class ReaProject;`, so naming it here refers to that
// type rather than introducing a second one.
class ReaProject;

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`, so naming the struct here is a forward declaration of the
// same type rather than a second one.
struct reaper_plugin_info_t;

namespace sesh_ai {
namespace daw {

// Alias storage backed by the live project.
//
// Every TrackHandle crossing this boundary is a `MediaTrack*`. The cast lives in
// the implementation and nowhere else, which is the whole point of the handle
// being opaque in alias_store.h.
class ReaperAliasStorage final : public AliasStorage {
public:
	// `plugin_info` is the pointer REAPER passed to ReaperPluginEntry; it stays
	// valid for the lifetime of the loaded extension. A null project means REAPER's
	// active project, which is the API's own convention for a null `ReaProject*` —
	// so the default is "whatever the producer is looking at" without this class
	// having to resolve that itself.
	explicit ReaperAliasStorage(reaper_plugin_info_t* plugin_info, ReaProject* project = nullptr);
	~ReaperAliasStorage() override;

	int track_count() const override;
	TrackHandle track_at(int index) const override;

	std::string track_extension_value(TrackHandle track, const std::string& key) const override;
	bool set_track_extension_value(TrackHandle track, const std::string& key, const std::string& value) override;

	std::string project_extension_value(const std::string& section, const std::string& key) const override;
	bool set_project_extension_value(const std::string& section, const std::string& key, const std::string& value) override;

	bool project_has_been_saved() const override;
	bool project_has_unsaved_changes() const override;

	// The resolved function pointers. Declared but not defined here: the definition
	// is in the translation unit, so the SDK types its members are made of never
	// reach this header. Public only so that translation unit can name the type;
	// nothing outside can do anything with an incomplete struct.
	struct reaper_alias_api;

private:
	std::unique_ptr<reaper_alias_api> api_;
	ReaProject* project_;

	// The names REAPER did not supply. Private: `AliasStorage` carries no usability
	// report, and the store already treats a missing function as "no alias" or
	// "write refused", so widening the interface would reach every test double for
	// no behaviour this changes.
	std::vector<std::string> unresolved_function_names_;
};

}  // namespace daw
}  // namespace sesh_ai

#endif  // SESH_AI_DAW_REAPER_ALIAS_STORAGE_H
