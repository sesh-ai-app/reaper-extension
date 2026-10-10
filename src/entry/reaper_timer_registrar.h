// The REAPER side of the timer seam.
//
// This header deliberately does not include the SDK. reaper_plugin_info_t is
// forward-declared and only ever held as a pointer, so everything except
// reaper_timer_registrar.cpp can talk about a REAPER-backed registrar without
// pulling reaper_plugin.h — which on macOS and Linux drags in WDL's swell, and
// which the test target has no include path for.
//
// That is the convention for every REAPER dependency that follows: the interface
// and the header stay SDK-free, and one `*_reaper_*.cpp` per component is where the
// SDK lives.

#ifndef SESH_AI_ENTRY_REAPER_TIMER_REGISTRAR_H
#define SESH_AI_ENTRY_REAPER_TIMER_REGISTRAR_H

#include <entry/timer_registration.h>

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`, so naming the struct here is a forward declaration of the
// same type rather than a second one.
struct reaper_plugin_info_t;

namespace sesh_ai::entry {

	// Registers the timer through the registration table REAPER hands the extension
	// at load. `Register("timer", callback)` and `Register("-timer", callback)` are
	// the same facility the SDK exposes as plugin_register once the API imports are
	// resolved; going through the table avoids depending on that resolution having
	// happened, which matters because the timer has to be running before most of it
	// is needed.
	class ReaperTimerRegistrar final : public TimerRegistrar {
	public:
		// The pointer REAPER passed to ReaperPluginEntry. It stays valid for the
		// lifetime of the loaded extension, which outlives this object.
		explicit ReaperTimerRegistrar(reaper_plugin_info_t* plugin_info);

		bool register_timer(TimerCallback callback) override;
		void unregister_timer(TimerCallback callback) override;

	private:
		reaper_plugin_info_t* plugin_info_;
	};

}

#endif
