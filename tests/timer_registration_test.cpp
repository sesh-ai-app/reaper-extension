// The REAPER timer seam.
//
// Substituting a TimerRegistrar is what makes the rest of this suite possible, so
// the substitution itself is worth testing: that starting registers exactly one
// callback, that stopping unregisters the same one, and that the destructor does it
// too. The last is how requirement 1.2's ordering constraint — unregister the timer
// before destroying the queues — becomes a declaration-order property instead of a
// teardown sequence somebody has to call correctly.

#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/timer_registration.h>
#include <transport/queue_pair.h>

using sesh_ai::entry::TimerCallback;
using sesh_ai::entry::TimerRegistrar;
using sesh_ai::entry::TimerRegistration;

namespace {

	// Stands in for REAPER's registration table: records what was registered and lets
	// the test fire the callback the way REAPER's timer would.
	class RecordingTimerRegistrar final : public TimerRegistrar {
	public:
		bool register_timer(TimerCallback callback) override
		{
			++register_call_count;

			if (refuse_registration) {
				return false;
			}

			registered_callback = callback;

			return true;
		}

		void unregister_timer(TimerCallback callback) override
		{
			++unregister_call_count;
			unregistered_callback = callback;
			registered_callback = nullptr;
		}

		void fire_timer() const
		{
			if (registered_callback != nullptr) {
				registered_callback();
			}
		}

		bool refuse_registration = false;
		TimerCallback registered_callback = nullptr;
		TimerCallback unregistered_callback = nullptr;
		int register_call_count = 0;
		int unregister_call_count = 0;
	};

}

TEST_CASE("starting registers one callback and firing it reaches the tick", "[timer]")
{
	RecordingTimerRegistrar registrar;
	TimerRegistration registration{registrar};

	int tick_count = 0;

	REQUIRE(registration.start([&tick_count] { ++tick_count; }));
	REQUIRE(registration.is_running());
	REQUIRE(registrar.register_call_count == 1);
	REQUIRE(registrar.registered_callback == TimerRegistration::callback_given_to_reaper());

	registrar.fire_timer();
	registrar.fire_timer();

	REQUIRE(tick_count == 2);
}

TEST_CASE("stopping unregisters the same callback that was registered", "[timer]")
{
	RecordingTimerRegistrar registrar;
	int tick_count = 0;

	{
		TimerRegistration registration{registrar};

		REQUIRE(registration.start([&tick_count] { ++tick_count; }));

		registration.stop();

		REQUIRE_FALSE(registration.is_running());
		REQUIRE(registrar.unregister_call_count == 1);
		REQUIRE(registrar.unregistered_callback == TimerRegistration::callback_given_to_reaper());

		// Stopping twice is not an error — the destructor will try again.
		registration.stop();
		REQUIRE(registrar.unregister_call_count == 1);
	}

	// And the destructor did not unregister a second time either.
	REQUIRE(registrar.unregister_call_count == 1);
	REQUIRE(tick_count == 0);
}

TEST_CASE("a fired timer after stopping does not reach the tick", "[timer]")
{
	// REAPER unregisters on its own schedule, so a callback can fire once more after
	// the unregister call returns. It must find nothing to do.
	RecordingTimerRegistrar registrar;
	TimerRegistration registration{registrar};

	int tick_count = 0;

	REQUIRE(registration.start([&tick_count] { ++tick_count; }));

	const TimerCallback callback_reaper_holds = registrar.registered_callback;

	registration.stop();

	REQUIRE_NOTHROW(callback_reaper_holds());
	REQUIRE(tick_count == 0);
}

TEST_CASE("the destructor unregisters, so the queues can safely be declared first", "[timer]")
{
	// Requirement 1.2. The registration is declared after the queues, so it is
	// destroyed before them, so the timer cannot fire into a destroyed queue. This
	// test is the shape of that arrangement rather than a mock of it.
	using Queues = sesh_ai::transport::QueuePair<int, int>;

	RecordingTimerRegistrar registrar;
	std::vector<std::string> teardown_order;

	{
		Queues queues;
		TimerRegistration registration{registrar};

		REQUIRE(registration.start([&queues, &teardown_order] {
			queues.inbound().push(1);
			teardown_order.push_back("tick");
		}));

		registrar.fire_timer();

		REQUIRE(queues.inbound().size() == 1);
		REQUIRE(registrar.unregister_call_count == 0);
	}

	// Leaving the scope destroyed the registration first and the queues second.
	REQUIRE(registrar.unregister_call_count == 1);
	REQUIRE(teardown_order == std::vector<std::string>{"tick"});
}

TEST_CASE("a refused registration is reported rather than assumed", "[timer]")
{
	// REAPER's Register call can fail. A timer that silently never started is an
	// extension that loads, shows its panel, and processes no messages at all.
	RecordingTimerRegistrar registrar;
	registrar.refuse_registration = true;

	TimerRegistration registration{registrar};

	REQUIRE_FALSE(registration.start([] {}));
	REQUIRE_FALSE(registration.is_running());
	REQUIRE(registrar.register_call_count == 1);

	// A refused start left nothing behind, so a later attempt is allowed to succeed.
	registrar.refuse_registration = false;

	REQUIRE(registration.start([] {}));
	REQUIRE(registration.is_running());
}

TEST_CASE("starting twice, or without a tick function, is refused", "[timer]")
{
	RecordingTimerRegistrar registrar;
	TimerRegistration registration{registrar};

	SECTION("no tick function")
	{
		REQUIRE_FALSE(registration.start(nullptr));
		REQUIRE_FALSE(registration.is_running());
		REQUIRE(registrar.register_call_count == 0);
	}

	SECTION("already running")
	{
		REQUIRE(registration.start([] {}));
		REQUIRE_FALSE(registration.start([] {}));
		REQUIRE(registrar.register_call_count == 1);
	}
}

TEST_CASE("a second registration cannot claim the process-wide slot", "[timer]")
{
	// REAPER's timer callback carries no context, so the active registration is a
	// static — one extension instance per REAPER process. A second claim is a bug in
	// the caller and is refused rather than silently redirecting the first one's
	// callback.
	RecordingTimerRegistrar first_registrar;
	RecordingTimerRegistrar second_registrar;

	TimerRegistration first_registration{first_registrar};
	TimerRegistration second_registration{second_registrar};

	int first_tick_count = 0;
	int second_tick_count = 0;

	REQUIRE(first_registration.start([&first_tick_count] { ++first_tick_count; }));
	REQUIRE_FALSE(second_registration.start([&second_tick_count] { ++second_tick_count; }));
	REQUIRE(second_registrar.register_call_count == 0);

	first_registrar.fire_timer();

	REQUIRE(first_tick_count == 1);
	REQUIRE(second_tick_count == 0);

	// Once the first releases the slot, the second can take it.
	first_registration.stop();

	REQUIRE(second_registration.start([&second_tick_count] { ++second_tick_count; }));

	second_registrar.fire_timer();

	REQUIRE(second_tick_count == 1);
	REQUIRE(first_tick_count == 1);
}

TEST_CASE("an exception from the tick never escapes into REAPER", "[timer][errors]")
{
	// REAPER calls this through a C function pointer. Unwinding across that boundary
	// is undefined behaviour, and in practice the crash a producer reads as Sesh
	// losing their session.
	RecordingTimerRegistrar registrar;
	TimerRegistration registration{registrar};

	int tick_count = 0;

	REQUIRE(registration.start([&tick_count] {
		++tick_count;
		throw std::runtime_error{"tick failed"};
	}));

	REQUIRE_NOTHROW(registrar.fire_timer());
	REQUIRE_NOTHROW(registrar.fire_timer());
	REQUIRE(tick_count == 2);
}
