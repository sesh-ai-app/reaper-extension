// The one translation unit in this task that includes the REAPER SDK.
//
// Everything the extension does with REAPER's timer is these two calls. Keeping
// them alone in a file is what makes the dispatcher's draining and bounding logic
// testable without REAPER: the suite substitutes a TimerRegistrar and never links
// this.
//
// Note for the plugin entry (task 17.1): reaper_plugin_functions.h needs exactly
// one translation unit to define REAPERAPI_IMPLEMENT, which is where the imported
// API function pointers get their storage. That belongs with the entry point, not
// here — this file needs only the registration table REAPER passes in, so it
// includes reaper_plugin.h and stays out of the way of that decision.

#include <entry/reaper_timer_registrar.h>

#include <reaper_plugin.h>

namespace sesh_ai::entry {

	ReaperTimerRegistrar::ReaperTimerRegistrar(reaper_plugin_info_t* plugin_info)
		: plugin_info_{plugin_info}
	{
	}

	bool ReaperTimerRegistrar::register_timer(TimerCallback callback)
	{
		if (plugin_info_ == nullptr || plugin_info_->Register == nullptr || callback == nullptr) {
			return false;
		}

		// Register takes void*, so the function pointer has to be cast. Casting a
		// function pointer to an object pointer is conditionally supported by the
		// standard; it is well defined on all three platforms the extension targets,
		// and it is how REAPER's own SDK examples register a timer.
		const int registered_timer_count = plugin_info_->Register("timer", reinterpret_cast<void*>(callback));

		// REAPER returns the number of registered timers, so zero means the
		// registration did not take.
		return registered_timer_count > 0;
	}

	void ReaperTimerRegistrar::unregister_timer(TimerCallback callback)
	{
		if (plugin_info_ == nullptr || plugin_info_->Register == nullptr || callback == nullptr) {
			return;
		}

		// The leading minus is REAPER's convention for removing a registration.
		plugin_info_->Register("-timer", reinterpret_cast<void*>(callback));
	}

}
