// The Plugin Entry (requirements 1.1, 1.2, 1.3).
//
// Three things here are worth defending, and they are covered in that order.
//
// **Registration and removal are symmetric.** A registration REAPER still holds after
// the extension's memory is gone is a callback REAPER invokes into freed memory. So the
// cases below check the balance rather than the calls: nothing outstanding after a
// teardown, nothing outstanding after a load that failed halfway, and removal in the
// reverse of the order things were taken.
//
// **Teardown order is the whole task.** Unregister the timer, destroy the queues, shut
// down CEF, close the socket, exit. Out of order, REAPER crashes on quit, which
// producers report as Sesh losing their session. `ShutdownSequence` already refuses a
// step that is not next — `tests/ui/ui_host_test.cpp` holds it to that, and the UI Host
// to refusing its own step. What is new here is the *driving*: that all five happen,
// that each one's component is actually called, that the recorded order is the required
// one, and that a step whose component fails stops the sequence instead of letting the
// steps ordered against it run anyway.
//
// **The panel is where the browser goes** (requirement 1.3), so the browser cannot be
// created before REAPER has a panel to put it in, and must not be after CEF is gone.
//
// What this cannot reach: `ReaperPluginEntry` itself. It is an exported C symbol in a
// translation unit that includes the SDK, and the test target has no SDK on its include
// path — which is the enforcement that keeps the logic above out of it. The one decision
// that would otherwise hide in there is `reaper_plugin_entry_result`, pulled out into
// the header precisely so it is assertable: returning 0 on a successful load is an
// extension that registers everything correctly and is then unloaded, and nothing about
// the symptom would point at the return statement.

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/plugin_entry.h>
#include <entry/shutdown_sequence.h>

using sesh_ai::entry::ExtensionRegistrar;
using sesh_ai::entry::LoadReport;
using sesh_ai::entry::PanelRefusalReason;
using sesh_ai::entry::PluginEntry;
using sesh_ai::entry::RegistrationKind;
using sesh_ai::entry::RegistrationRequest;
using sesh_ai::entry::RegistrationSet;
using sesh_ai::entry::ShutdownReport;
using sesh_ai::entry::ShutdownSequence;
using sesh_ai::entry::ShutdownStep;
using sesh_ai::entry::StartupPlan;
using sesh_ai::entry::TeardownSteps;
using sesh_ai::entry::describe;
using sesh_ai::entry::reaper_plugin_entry_result;
using sesh_ai::entry::reaper_plugin_entry_unload_result;

namespace {

	// Stands in for REAPER's registration table. Records every call in order, tracks
	// what is still registered so the balance can be asserted rather than inferred, and
	// lets a test make REAPER refuse one kind.
	class recording_registrar final : public ExtensionRegistrar {
	public:
		bool register_item(RegistrationKind kind, void* payload) override
		{
			call_log.push_back("register " + std::string{describe(kind)});

			if (refused_kind.has_value() && *refused_kind == kind) {
				return false;
			}

			outstanding.emplace_back(kind, payload);

			return true;
		}

		void unregister_item(RegistrationKind kind, void* payload) override
		{
			call_log.push_back("unregister " + std::string{describe(kind)});

			for (std::size_t index = 0; index < outstanding.size(); ++index) {
				if (outstanding[index].first == kind && outstanding[index].second == payload) {
					outstanding.erase(outstanding.begin() + static_cast<std::ptrdiff_t>(index));
					return;
				}
			}

			// Unregistering something that was never registered, or registered twice —
			// both are the bug this fake exists to catch.
			unmatched_unregister_count += 1;
		}

		std::optional<RegistrationKind> refused_kind;
		std::vector<std::pair<RegistrationKind, void*>> outstanding;
		std::vector<std::string> call_log;
		int unmatched_unregister_count = 0;
	};

	// `required_order` returns its array by value, so both iterators have to come from
	// one object — taking them from two calls is two temporaries and an unrelated pair
	// of iterators.
	std::vector<ShutdownStep> required_shutdown_order()
	{
		const auto order = ShutdownSequence::required_order();

		return std::vector<ShutdownStep>{order.begin(), order.end()};
	}

	// Payloads. REAPER only ever holds the address, so what they point at does not
	// matter — only that each is distinct and outlives the registration.
	int action_payload = 0;
	int action_hook_payload = 0;
	int menu_item_payload = 0;
	int panel_payload = 0;

	std::vector<RegistrationRequest> three_registrations()
	{
		return {
			RegistrationRequest{RegistrationKind::action, &action_payload, "Sesh AI: Show the chat panel"},
			RegistrationRequest{RegistrationKind::action_hook, &action_hook_payload, "the action hook"},
			RegistrationRequest{RegistrationKind::menu_item, &menu_item_payload, "the menu entry"}
		};
	}

	// Records what each teardown step's component was asked to do, so the order the
	// components were *called* in can be asserted and not only the order the sequence
	// recorded.
	struct recording_components {
		std::vector<std::string> call_log;

		bool timer_running = true;
		bool queues_alive = true;
		bool cef_up = true;
		bool socket_open = true;

		bool fail_timer_unregister = false;
		bool fail_queue_destruction = false;
		bool fail_cef_shutdown = false;
		bool fail_socket_close = false;

		// A CEF step that does the work but never claims the step from the sequence.
		// Nothing then proves CEF is down, which is what the entry has to notice.
		bool cef_step_skips_the_sequence = false;
	};

	TeardownSteps teardown_over(recording_components& components)
	{
		TeardownSteps teardown;

		teardown.unregister_timer = [&components] {
			components.call_log.emplace_back("unregister_timer");

			if (components.fail_timer_unregister) {
				return false;
			}

			components.timer_running = false;

			return true;
		};

		teardown.destroy_queues = [&components] {
			components.call_log.emplace_back("destroy_queues");

			if (components.fail_queue_destruction) {
				return false;
			}

			components.queues_alive = false;

			return true;
		};

		// Shaped like `UiHost::shut_down`: it claims the step from the sequence itself,
		// before it touches CEF, so an out-of-order teardown is refused while the
		// browser is still alive.
		teardown.shut_down_cef = [&components](ShutdownSequence& sequence) {
			components.call_log.emplace_back("shut_down_cef");

			if (!components.cef_step_skips_the_sequence && !sequence.record(ShutdownStep::shut_down_cef)) {
				return false;
			}

			if (components.fail_cef_shutdown) {
				return false;
			}

			components.cef_up = false;

			return true;
		};

		teardown.close_socket_io_connection = [&components] {
			components.call_log.emplace_back("close_socket_io_connection");

			if (components.fail_socket_close) {
				return false;
			}

			components.socket_open = false;

			return true;
		};

		return teardown;
	}

	StartupPlan plan_with(
		std::vector<RegistrationRequest> registrations,
		bool timer_starts = true,
		bool browser_creation_succeeds = true,
		std::vector<std::string>* call_log = nullptr)
	{
		StartupPlan plan;
		plan.registrations = std::move(registrations);

		plan.start_timer = [timer_starts, call_log] {
			if (call_log != nullptr) {
				call_log->emplace_back("start_timer");
			}

			return timer_starts;
		};

		plan.create_panel_browser = [browser_creation_succeeds, call_log](void*) {
			if (call_log != nullptr) {
				call_log->emplace_back("create_panel_browser");
			}

			return browser_creation_succeeds;
		};

		return plan;
	}

}

// ---------------------------------------------------------------------------
// Load (requirement 1.1)
// ---------------------------------------------------------------------------

TEST_CASE("loading registers everything and then starts the timer", "[entry][load]")
{
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;
	std::vector<std::string> startup_log;

	PluginEntry entry{
		registrations,
		plan_with(three_registrations(), true, true, &startup_log),
		teardown_over(components)
	};

	const LoadReport report = entry.load();

	CHECK(report.loaded);
	CHECK(report.timer_started);
	CHECK(report.registrations_taken == 3);
	CHECK_FALSE(report.rolled_back);
	CHECK(report.failures.empty());
	CHECK(entry.loaded());
	CHECK(entry.timer_started());

	// The timer goes last. A tick arriving before the panel is registered has a
	// destination that is not there yet.
	REQUIRE(startup_log.size() == 1);
	CHECK(startup_log.front() == "start_timer");
	CHECK(registrar.outstanding.size() == 3);
	CHECK(registrar.call_log == std::vector<std::string>{
		"register action",
		"register action hook",
		"register menu item"
	});
}

TEST_CASE("a refused registration leaves nothing registered", "[entry][load]")
{
	// REAPER returning 1 for a 0 is the condition. The extension then returns 0 from
	// its entry point and REAPER unloads the module — without calling back to tear
	// down, because the load never completed. So anything still registered at that
	// point is a callback into freed memory.
	recording_registrar registrar;
	registrar.refused_kind = RegistrationKind::menu_item;

	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	const LoadReport report = entry.load();

	CHECK_FALSE(report.loaded);
	CHECK(report.rolled_back);
	CHECK_FALSE(report.timer_started);
	CHECK_FALSE(entry.loaded());

	// The two it had taken went back, in the reverse of the order they were taken.
	CHECK(registrar.outstanding.empty());
	CHECK(registrations.empty());
	CHECK(registrar.unmatched_unregister_count == 0);
	CHECK(registrar.call_log == std::vector<std::string>{
		"register action",
		"register action hook",
		"register menu item",
		"unregister action hook",
		"unregister action"
	});

	// Named, because "a registration failed" does not tell a producer which feature
	// they are missing.
	REQUIRE(report.failures.size() == 1);
	CHECK(report.failures.front()
		== "reaper refused to register the menu item 'the menu entry'");
}

TEST_CASE("a refused timer leaves nothing registered either", "[entry][load]")
{
	// An extension that loaded, registered its panel, and processes no message is
	// worse than one that did not load: the producer can see it and it does nothing.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{
		registrations,
		plan_with(three_registrations(), false),
		teardown_over(components)
	};

	const LoadReport report = entry.load();

	CHECK_FALSE(report.loaded);
	CHECK(report.rolled_back);
	CHECK(registrar.outstanding.empty());
	REQUIRE(report.failures.size() == 1);
	CHECK(report.failures.front().find("refused the timer registration") != std::string::npos);
}

TEST_CASE("an extension with no timer wired does not load", "[entry][load]")
{
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	StartupPlan plan;
	plan.registrations = three_registrations();

	PluginEntry entry{registrations, std::move(plan), teardown_over(components)};

	const LoadReport report = entry.load();

	CHECK_FALSE(report.loaded);
	CHECK(report.rolled_back);
	CHECK(registrar.outstanding.empty());
	REQUIRE(report.failures.size() == 1);
	CHECK(report.failures.front().find("no timer was wired") != std::string::npos);
}

TEST_CASE("what the entry point returns is a decision, not a formality", "[entry][load]")
{
	// The one piece of the entry point the suite can reach. REAPER reads 1 as
	// compatible and anything else as "unload this" — so the inverse of this is an
	// extension that does everything right and is then unloaded, with nothing in the
	// symptom to point at a return statement.
	STATIC_REQUIRE(reaper_plugin_entry_result(true) == 1);
	STATIC_REQUIRE(reaper_plugin_entry_result(false) == 0);
	STATIC_REQUIRE(reaper_plugin_entry_unload_result() == 0);
}

// ---------------------------------------------------------------------------
// The dockable panel (requirement 1.3)
// ---------------------------------------------------------------------------

TEST_CASE("the browser is created in the panel, once", "[entry][panel]")
{
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;
	std::vector<std::string> startup_log;

	PluginEntry entry{
		registrations,
		plan_with(three_registrations(), true, true, &startup_log),
		teardown_over(components)
	};

	// Stands in for REAPER's dockable panel — an `HWND` on Windows and a SWELL `HWND`
	// elsewhere, which the entry only ever holds as an opaque pointer.
	int panel_window = 0;

	SECTION("not before the extension has loaded")
	{
		const auto outcome = entry.open_panel(&panel_window);

		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::extension_not_loaded);
		CHECK_FALSE(entry.panel_browser_created());
	}

	SECTION("once loaded, opening the panel creates it")
	{
		REQUIRE(entry.load().loaded);

		const auto outcome = entry.open_panel(&panel_window);

		CHECK(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::none);
		CHECK(entry.panel_browser_created());
		CHECK(startup_log.back() == "create_panel_browser");
	}

	SECTION("opening it again finds the browser already there")
	{
		REQUIRE(entry.load().loaded);
		REQUIRE(entry.open_panel(&panel_window).browser_created);

		const auto outcome = entry.open_panel(&panel_window);

		// CEF is initialized once per process and holds one browser per panel. A second
		// create would orphan the first.
		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.already_open);
		CHECK(outcome.refusal == PanelRefusalReason::none);
	}

	SECTION("with no panel window there is nothing to create it in")
	{
		REQUIRE(entry.load().loaded);

		const auto outcome = entry.open_panel(nullptr);

		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::no_panel_window);
	}

	SECTION("not after the extension has torn down")
	{
		REQUIRE(entry.load().loaded);

		ShutdownSequence sequence;
		REQUIRE(entry.shut_down(sequence).completed);

		const auto outcome = entry.open_panel(&panel_window);

		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::extension_shut_down);
	}
}

TEST_CASE("a browser that cannot be created says which half failed", "[entry][panel]")
{
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	int panel_window = 0;

	SECTION("cef refused")
	{
		PluginEntry entry{
			registrations,
			plan_with(three_registrations(), true, false),
			teardown_over(components)
		};

		REQUIRE(entry.load().loaded);

		const auto outcome = entry.open_panel(&panel_window);

		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::browser_creation_refused);
		CHECK_FALSE(entry.panel_browser_created());
	}

	SECTION("nothing is wired to create one")
	{
		// The state a build with no CEF distribution is in. Reported as itself rather
		// than as CEF refusing, because the two call for different answers.
		StartupPlan plan = plan_with(three_registrations());
		plan.create_panel_browser = nullptr;

		PluginEntry entry{registrations, std::move(plan), teardown_over(components)};

		REQUIRE(entry.load().loaded);

		const auto outcome = entry.open_panel(&panel_window);

		CHECK_FALSE(outcome.browser_created);
		CHECK(outcome.refusal == PanelRefusalReason::no_browser_creator);
	}
}

// ---------------------------------------------------------------------------
// Teardown (requirement 1.2)
// ---------------------------------------------------------------------------

TEST_CASE("the entry performs all five teardown steps in the required order", "[entry][teardown]")
{
	// The case this task exists for. `ShutdownSequence` refuses a step that is not
	// next, and the UI Host refuses its own — but until now nothing performed all five.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	REQUIRE(entry.load().loaded);
	REQUIRE(registrar.outstanding.size() == 3);

	ShutdownSequence sequence;
	const ShutdownReport report = entry.shut_down(sequence);

	CHECK(report.completed);
	CHECK_FALSE(report.step_failed);
	CHECK_FALSE(report.already_shut_down);

	// The order, recorded rather than reasoned about. A step moved, dropped, or
	// swapped shows up here.
	const std::vector<ShutdownStep> required_order = required_shutdown_order();

	CHECK(report.steps_performed == required_order);
	CHECK(sequence.taken_steps() == required_order);
	CHECK_FALSE(sequence.broken());
	CHECK(sequence.violations().empty());

	// And each step's component was actually called, in the same order. The sequence
	// alone would be satisfied by five `record` calls that tore nothing down.
	CHECK(components.call_log == std::vector<std::string>{
		"unregister_timer",
		"destroy_queues",
		"shut_down_cef",
		"close_socket_io_connection"
	});

	CHECK_FALSE(components.timer_running);
	CHECK_FALSE(components.queues_alive);
	CHECK_FALSE(components.cef_up);
	CHECK_FALSE(components.socket_open);
	CHECK(report.steps_with_nothing_to_tear_down.empty());
}

TEST_CASE("the exit step is where the registrations go back", "[entry][teardown]")
{
	// Requirement 1.1's registrations, matched one for one. Asserted while the entry is
	// still alive, so the destructor is not what is being credited: a registration
	// released only by the destructor is one REAPER still holds for as long as anything
	// returns early.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	REQUIRE(entry.load().loaded);
	REQUIRE(registrar.outstanding.size() == 3);

	ShutdownSequence sequence;
	const ShutdownReport report = entry.shut_down(sequence);

	CHECK(report.completed);
	CHECK(report.registrations_released == 3);
	CHECK(registrar.outstanding.empty());
	CHECK(registrations.empty());
	CHECK(registrar.unmatched_unregister_count == 0);

	// Reverse order: the panel is registered last and removed first, because the action
	// that shows it must not outlive it.
	const std::vector<std::string> expected_calls{
		"register action",
		"register action hook",
		"register menu item",
		"unregister menu item",
		"unregister action hook",
		"unregister action"
	};

	CHECK(registrar.call_log == expected_calls);
}

TEST_CASE("a step whose component fails stops the teardown", "[entry][teardown][errors]")
{
	// Not logged and continued. Every step after the failed one is ordered *against*
	// it, so performing them anyway is the out-of-order teardown the order exists to
	// prevent.
	SECTION("the timer refuses to unregister")
	{
		recording_registrar registrar;
		RegistrationSet registrations{registrar};
		recording_components components;
		components.fail_timer_unregister = true;

		PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

		REQUIRE(entry.load().loaded);

		ShutdownSequence sequence;
		const ShutdownReport report = entry.shut_down(sequence);

		CHECK_FALSE(report.completed);
		CHECK(report.step_failed);
		CHECK(report.failed_step == ShutdownStep::unregister_timer);
		CHECK(report.failure_description.find("unregister the timer") != std::string::npos);

		// Nothing after it ran: the queues are intact, CEF is up, the socket is open,
		// and the registrations are still REAPER's — which is the correct answer when
		// a tick may still be arriving.
		CHECK(components.call_log == std::vector<std::string>{"unregister_timer"});
		CHECK(components.queues_alive);
		CHECK(components.cef_up);
		CHECK(components.socket_open);
		CHECK(registrar.outstanding.size() == 3);
		CHECK(report.steps_performed.empty());
	}

	SECTION("the queues refuse to be destroyed")
	{
		recording_registrar registrar;
		RegistrationSet registrations{registrar};
		recording_components components;
		components.fail_queue_destruction = true;

		PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

		REQUIRE(entry.load().loaded);

		ShutdownSequence sequence;
		const ShutdownReport report = entry.shut_down(sequence);

		CHECK_FALSE(report.completed);
		CHECK(report.failed_step == ShutdownStep::destroy_queues);
		CHECK(report.steps_performed == std::vector<ShutdownStep>{ShutdownStep::unregister_timer});

		// CEF is still up, which is the point: a queue that could not be destroyed is
		// a queue that may still hold work whose destination would otherwise be gone.
		CHECK(components.cef_up);
		CHECK(registrar.outstanding.size() == 3);
	}

	SECTION("cef refuses to shut down")
	{
		recording_registrar registrar;
		RegistrationSet registrations{registrar};
		recording_components components;
		components.fail_cef_shutdown = true;

		PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

		REQUIRE(entry.load().loaded);

		ShutdownSequence sequence;
		const ShutdownReport report = entry.shut_down(sequence);

		CHECK_FALSE(report.completed);
		CHECK(report.failed_step == ShutdownStep::shut_down_cef);
		CHECK(components.cef_up);
		CHECK(components.socket_open);
		CHECK(registrar.outstanding.size() == 3);
		CHECK(report.failure_description.find("live browser") != std::string::npos);
	}

	SECTION("the socket refuses to close")
	{
		recording_registrar registrar;
		RegistrationSet registrations{registrar};
		recording_components components;
		components.fail_socket_close = true;

		PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

		REQUIRE(entry.load().loaded);

		ShutdownSequence sequence;
		const ShutdownReport report = entry.shut_down(sequence);

		CHECK_FALSE(report.completed);
		CHECK(report.failed_step == ShutdownStep::close_socket_io_connection);

		// The registrations are REAPER's until the exit step, which never ran. The
		// destructor is the backstop for this case, not the mechanism.
		CHECK(registrar.outstanding.size() == 3);
	}
}

TEST_CASE("a cef step that claims nothing is not a cef step", "[entry][teardown][errors]")
{
	// Step three is the UI Host's to claim, because the refusal has to happen inside
	// the component that would otherwise touch CEF. The cost of delegating it is that
	// the entry has to check: a step that reported success without claiming anything
	// leaves nothing proving CEF is down, and the socket would then close on top of a
	// live browser.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;
	components.cef_step_skips_the_sequence = true;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	REQUIRE(entry.load().loaded);

	ShutdownSequence sequence;
	const ShutdownReport report = entry.shut_down(sequence);

	CHECK_FALSE(report.completed);
	CHECK(report.step_failed);
	CHECK(report.failed_step == ShutdownStep::shut_down_cef);
	CHECK(report.failure_description.find("did not claim") != std::string::npos);

	// The sequence is still waiting for the step, so nothing after it could run.
	CHECK(sequence.is_next(ShutdownStep::shut_down_cef));
	CHECK(components.socket_open);
	CHECK(registrar.outstanding.size() == 3);
}

TEST_CASE("a step with nothing behind it advances, and says so", "[entry][teardown]")
{
	// During the build-out the Socket.IO connection does not exist. "There is no socket
	// to close" has to advance the sequence — otherwise CEF never shuts down and the
	// registrations never go back — while "the socket refused to close" must not.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};

	PluginEntry entry{registrations, plan_with(three_registrations()), TeardownSteps{}};

	REQUIRE(entry.load().loaded);

	ShutdownSequence sequence;
	const ShutdownReport report = entry.shut_down(sequence);

	CHECK(report.completed);
	CHECK_FALSE(report.step_failed);
	CHECK_FALSE(sequence.broken());

	CHECK(report.steps_performed == required_shutdown_order());

	// All four delegated steps had nothing behind them. The exit step is the entry's
	// own, so it is not among them — the registrations really did go back.
	CHECK(report.steps_with_nothing_to_tear_down == std::vector<ShutdownStep>{
		ShutdownStep::unregister_timer,
		ShutdownStep::destroy_queues,
		ShutdownStep::shut_down_cef,
		ShutdownStep::close_socket_io_connection
	});

	CHECK(report.registrations_released == 3);
	CHECK(registrar.outstanding.empty());
}

TEST_CASE("a second teardown is reported, not performed again", "[entry][teardown]")
{
	// REAPER calls the entry point with a null registration table on unload, and an
	// error path may already have torn down. Unregistering everything a second time
	// would hand REAPER a removal for a registration it no longer holds.
	recording_registrar registrar;
	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	REQUIRE(entry.load().loaded);

	ShutdownSequence first_sequence;
	REQUIRE(entry.shut_down(first_sequence).completed);

	const std::size_t calls_after_first_teardown = registrar.call_log.size();

	ShutdownSequence second_sequence;
	const ShutdownReport report = entry.shut_down(second_sequence);

	CHECK(report.already_shut_down);
	CHECK_FALSE(report.completed);
	CHECK(report.steps_performed.empty());
	CHECK(registrar.call_log.size() == calls_after_first_teardown);
	CHECK(registrar.unmatched_unregister_count == 0);

	// The second sequence was never walked, so it is not broken either — a second
	// unload is not a violation.
	CHECK(second_sequence.taken_steps().empty());
	CHECK_FALSE(second_sequence.broken());

	// And the components were each torn down exactly once.
	CHECK(components.call_log.size() == 4);
}

TEST_CASE("tearing down an extension that never loaded is harmless", "[entry][teardown]")
{
	// The path a refused registration takes: `load` rolled back, and REAPER unloads the
	// module. Nothing is registered, nothing is up, and the teardown must not invent
	// work to do.
	recording_registrar registrar;
	registrar.refused_kind = RegistrationKind::action;

	RegistrationSet registrations{registrar};
	recording_components components;

	PluginEntry entry{registrations, plan_with(three_registrations()), teardown_over(components)};

	REQUIRE_FALSE(entry.load().loaded);

	ShutdownSequence sequence;
	const ShutdownReport report = entry.shut_down(sequence);

	CHECK(report.completed);
	CHECK_FALSE(report.step_failed);
	CHECK(report.registrations_released == 0);
	CHECK(registrar.outstanding.empty());
	CHECK(registrar.unmatched_unregister_count == 0);
}

// ---------------------------------------------------------------------------
// The registration set on its own
// ---------------------------------------------------------------------------

TEST_CASE("the registration set holds one list, walked both ways", "[entry][registrations]")
{
	recording_registrar registrar;

	SECTION("a refused registration is not held, so it is not released")
	{
		registrar.refused_kind = RegistrationKind::menu_item;

		RegistrationSet registrations{registrar};

		CHECK(registrations.add(
			RegistrationRequest{RegistrationKind::action, &action_payload, "an action"}
		));
		CHECK_FALSE(registrations.add(
			RegistrationRequest{RegistrationKind::menu_item, &menu_item_payload, "a menu item"}
		));

		CHECK(registrations.size() == 1);

		registrations.release_all();

		CHECK(registrar.outstanding.empty());

		// The refused one was never handed back, which is what keeps REAPER from being
		// asked to remove a registration it never took.
		CHECK(registrar.unmatched_unregister_count == 0);
	}

	SECTION("a registration with no payload is refused before REAPER is asked")
	{
		RegistrationSet registrations{registrar};

		CHECK_FALSE(registrations.add(
			RegistrationRequest{RegistrationKind::dockable_panel, nullptr, "a panel with no window"}
		));
		CHECK(registrations.empty());
		CHECK(registrar.call_log.empty());
	}

	SECTION("releasing twice releases once")
	{
		RegistrationSet registrations{registrar};

		REQUIRE(registrations.add(
			RegistrationRequest{RegistrationKind::dockable_panel, &panel_payload, "the panel"}
		));

		registrations.release_all();
		registrations.release_all();

		CHECK(registrations.empty());
		CHECK(registrar.unmatched_unregister_count == 0);
	}

	SECTION("the destructor is the backstop")
	{
		{
			RegistrationSet registrations{registrar};

			REQUIRE(registrations.add(
				RegistrationRequest{RegistrationKind::action, &action_payload, "an action"}
			));
			REQUIRE(registrar.outstanding.size() == 1);
		}

		// Leaving the scope gave it back. An early return between the registration and
		// the teardown is then a leak of nothing.
		CHECK(registrar.outstanding.empty());
		CHECK(registrar.unmatched_unregister_count == 0);
	}
}
