// The REAPER timer seam.
//
// This is the first place the extension needs REAPER itself, so it is also where
// the shape of every later REAPER dependency gets decided. The shape is: the
// component declares the smallest interface it needs, the production
// implementation of that interface is the only thing that includes the SDK
// headers, and the suite substitutes a fake. Not one ReaperApi interface listing
// everything REAPER can do — a seam per component, as narrow as that component's
// actual need. This component's need is two calls.
//
// The enforcement is structural rather than a rule anyone has to follow: the test
// target does not have the SDK on its include path, so a header that reaches the
// SDK cannot be included from a test and the build says so. That is why
// ReaperTimerRegistrar's header forward-declares reaper_plugin_info_t instead of
// including reaper_plugin.h.
//
// REAPER's timer callback takes no argument, which is the awkward part. A plain
// function pointer cannot carry a `this`, so TimerRegistration keeps the active
// registration in a static and hands REAPER a trampoline that finds it. One
// extension instance is loaded per REAPER process, so one static is not a
// limitation being accepted, it is the actual cardinality.

#ifndef SESH_AI_ENTRY_TIMER_REGISTRATION_H
#define SESH_AI_ENTRY_TIMER_REGISTRATION_H

#include <functional>
#include <utility>

namespace sesh_ai::entry {

	// What REAPER's timer facility looks like from this side: a function pointer
	// goes in, and the same function pointer comes back out to stop it.
	using TimerCallback = void (*)();

	class TimerRegistrar {
	public:
		virtual ~TimerRegistrar() = default;

		// False when the registration was refused. REAPER's own Register call
		// reports this, and a silently unregistered timer is an extension that
		// loads, shows its panel, and never processes a message.
		virtual bool register_timer(TimerCallback callback) = 0;

		virtual void unregister_timer(TimerCallback callback) = 0;
	};

	// Owns the registration for the process and routes REAPER's argument-free
	// callback to a std::function.
	//
	// The destructor unregisters. That turns requirement 1.2's ordering constraint —
	// the timer must be unregistered before the queues are destroyed — into a
	// declaration-order concern in whatever owns both: declare the queues first and
	// the registration after them, and C++ destroys them in the order the
	// requirement asks for. An explicit teardown sequence that has to be called in
	// the right order is the version of this that breaks when somebody returns early.
	class TimerRegistration {
	public:
		explicit TimerRegistration(TimerRegistrar& registrar)
			: registrar_{registrar}
		{
		}

		~TimerRegistration()
		{
			stop();
		}

		// The trampoline holds this object's address, so it must not move, and
		// copying would mean two objects claiming one registration.
		TimerRegistration(const TimerRegistration&) = delete;
		TimerRegistration& operator=(const TimerRegistration&) = delete;
		TimerRegistration(TimerRegistration&&) = delete;
		TimerRegistration& operator=(TimerRegistration&&) = delete;

		// False when this registration is already running, when another
		// registration already holds the process-wide slot, when there is no
		// callback to run, or when REAPER refused.
		bool start(std::function<void()> on_tick)
		{
			if (running_ || !on_tick) {
				return false;
			}

			if (active_registration_ != nullptr && active_registration_ != this) {
				return false;
			}

			on_tick_ = std::move(on_tick);
			active_registration_ = this;

			if (!registrar_.register_timer(&dispatch_active_tick)) {
				active_registration_ = nullptr;
				on_tick_ = nullptr;
				return false;
			}

			running_ = true;

			return true;
		}

		// Idempotent, so the destructor can call it after an explicit shutdown
		// already did.
		void stop()
		{
			if (!running_) {
				return;
			}

			running_ = false;
			registrar_.unregister_timer(&dispatch_active_tick);

			if (active_registration_ == this) {
				active_registration_ = nullptr;
			}

			on_tick_ = nullptr;
		}

		bool is_running() const { return running_; }

		// The function pointer REAPER is given. Exposed so a test can invoke what
		// REAPER would invoke rather than a stand-in for it.
		static TimerCallback callback_given_to_reaper() { return &dispatch_active_tick; }

	private:
		static void dispatch_active_tick()
		{
			TimerRegistration* const registration = active_registration_;

			if (registration == nullptr || !registration->on_tick_) {
				return;
			}

			// REAPER calls this through a C function pointer, and an exception
			// unwinding across that boundary is undefined behaviour — in practice a
			// REAPER crash, which producers experience as losing their session. The
			// tick reports its own failures; anything that still escapes stops here.
			try {
				registration->on_tick_();
			} catch (...) {
			}
		}

		inline static TimerRegistration* active_registration_ = nullptr;

		TimerRegistrar& registrar_;
		std::function<void()> on_tick_;
		bool running_ = false;
	};

}

#endif
