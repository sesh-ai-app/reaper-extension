// The Plugin Entry (requirements 1.1, 1.2, 1.3; design "Plugin Entry").
//
// What REAPER loads. It registers the extension's actions, its menu item, and its
// dockable panel, starts the timer that drives the Main-Thread Dispatcher, creates the
// CEF browser when the panel is opened, and owns the order everything comes down in.
//
// `ReaperPluginEntry` itself is not here. It is the one symbol REAPER looks up by name,
// its signature is the SDK's, and an exported C symbol has to live in a translation
// unit — so it is in `plugin_entry.cpp`, as a thin shell over this header. Everything
// that decides anything is here, where the suite can reach it: the test target has no
// SDK and no CEF distribution on its include path, which is what enforces the split
// rather than anyone remembering it. Same arrangement as `entry/timer_registration.h`
// and `reaper_timer_registrar.h`, `daw/render_coordinator.h` and
// `daw/reaper_render_host.h`, `ui/ui_host.h` and `ui/cef_browser_host.h`.
//
// ---------------------------------------------------------------------------
// Registration and removal are one list, not two
//
// A registration REAPER still holds after the extension's memory is gone is a callback
// REAPER invokes into freed memory. The usual way that happens is two lists — the
// things registered at load, and the things unregistered at unload — which agree until
// somebody adds to one of them.
//
// So there is one list. `RegistrationSet::add` is the only way a registration is taken,
// `release_all` is the only way one is given back, and it walks the same list backwards.
// Nothing can be added without its removal being added with it, because they are the
// same entry. The destructor calls `release_all`, so the symmetry survives an early
// return and a throw as well as a normal teardown.
//
// Reverse order on release for the ordinary reason: the dockable panel is registered
// last and removed first, because the action that shows it must not be invocable after
// the panel it shows is gone.
//
// ---------------------------------------------------------------------------
// A load that cannot complete leaves nothing behind
//
// `ReaperPluginEntry` returns 1 to stay loaded and 0 to be unloaded, and REAPER does
// not call a plugin back to tear down one it never finished loading. So a load that
// fails halfway and returns 0 would be unloaded *with* whatever it had already
// registered — the dangling-callback case, reached by the error path rather than by
// forgetting.
//
// `PluginEntry::load` therefore rolls back: the first refusal releases everything taken
// so far and reports which registration REAPER refused, by name. Partially loaded is
// not a state this component has.
//
// ---------------------------------------------------------------------------
// Teardown (requirement 1.2)
//
// The order is `ShutdownSequence`'s, and the reasoning for it is in
// `entry/shutdown_sequence.h`. This component is what walks it: unregister the timer,
// destroy the queues, shut down CEF, close the Socket.IO connection, exit.
//
// Three things about how `shut_down` walks it are deliberate.
//
// **Each step is claimed before the work it names.** `record` comes first, the component
// is called second. A violation is then caught while the thing it would have broken is
// still intact — the same choice `UiHost::shut_down` makes, for the same reason.
//
// **Step three is claimed by the UI Host, not here.** `UiHost::shut_down` takes the
// sequence and records `shut_down_cef` itself, because the refusal has to happen inside
// the component that would otherwise touch CEF. So the CEF step is handed the sequence
// rather than called behind it, and this component checks afterwards that the step was
// actually claimed. A CEF step that neither shut CEF down nor advanced the sequence
// stops the teardown rather than letting the socket close on top of a live browser.
//
// **A step whose component fails stops the sequence.** Not logged and continued: the
// steps after it are ordered *against* the one that failed, and performing them anyway
// is the out-of-order teardown the order exists to prevent. The report says which step
// failed and the sequence is left incomplete, which is what the caller returns to
// REAPER.
//
// A step with nothing wired behind it is a different thing from a step that failed, and
// is recorded as such. During the build-out the Socket.IO connection does not exist
// yet; "there is no socket to close" must advance the sequence, while "the socket
// refused to close" must not.

#ifndef SESH_AI_ENTRY_PLUGIN_ENTRY_H
#define SESH_AI_ENTRY_PLUGIN_ENTRY_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <entry/shutdown_sequence.h>

namespace sesh_ai::entry {

	// ---------------------------------------------------------------------------
	// Registrations
	// ---------------------------------------------------------------------------

	// The things the extension asks REAPER to hold on its behalf.
	//
	// Named after the REAPER registration each one maps to rather than after the
	// product feature, because the mapping is the part that has to be right and the
	// part a reviewer cannot otherwise check. `plugin_entry.cpp` is where the strings
	// are.
	enum class RegistrationKind {
		// "gaccel" — puts the action in the main section's action list, with a
		// description the producer can rebind.
		action,

		// "hookcommand" — what actually runs when one of those actions is invoked. An
		// action registered without this is in the list and does nothing.
		action_hook,

		// "hookcustommenu" — the hook REAPER calls while building a customizable menu,
		// which is how a menu item is contributed.
		menu_item,

		// `DockWindowAddEx` / `DockWindowRemove` rather than `Register`. The panel is a
		// window REAPER docks, not an entry in a table.
		dockable_panel
	};

	inline constexpr std::string_view describe(RegistrationKind kind)
	{
		switch (kind) {
		case RegistrationKind::action:
			return "action";
		case RegistrationKind::action_hook:
			return "action hook";
		case RegistrationKind::menu_item:
			return "menu item";
		case RegistrationKind::dockable_panel:
			return "dockable panel";
		}

		return "unknown registration";
	}

	// What REAPER's registration facility looks like from this side.
	//
	// Two calls, matching the two directions, with the payload erased to `void*`
	// because that is already what REAPER's own `Register` takes. A seam per component
	// as narrow as that component's need — see `entry/timer_registration.h` for why
	// that is the shape rather than one interface listing everything REAPER can do.
	class ExtensionRegistrar {
	public:
		virtual ~ExtensionRegistrar() = default;

		// False when REAPER refused. Reported rather than assumed: a menu item that
		// silently did not register is a feature the producer cannot find, and a panel
		// that silently did not dock is an extension that loaded and shows nothing.
		virtual bool register_item(RegistrationKind kind, void* payload) = 0;

		virtual void unregister_item(RegistrationKind kind, void* payload) = 0;
	};

	// One registration the extension asked for.
	//
	// `payload` is whatever REAPER's registration for that kind expects — a
	// `gaccel_register_t*`, a function pointer, a window handle. It must outlive the
	// registration, which in practice means it is a static in `plugin_entry.cpp`.
	struct RegistrationRequest {
		RegistrationKind kind = RegistrationKind::action;
		void* payload = nullptr;

		// For the log line when REAPER refuses this one. "Sesh AI: Show chat panel" is
		// what a producer's log needs; "a registration failed" is not.
		std::string description;
	};

	// Every registration the extension holds while it is loaded, and the only way it
	// gives one back.
	//
	// See the file comment: one list, walked forwards to take and backwards to release,
	// so a registration without its matching removal is not expressible.
	class RegistrationSet {
	public:
		explicit RegistrationSet(ExtensionRegistrar& registrar)
			: registrar_{registrar}
		{
		}

		~RegistrationSet()
		{
			release_all();
		}

		// Holds registrations REAPER has a pointer into, so it cannot be copied — two
		// sets releasing one registration would unregister it twice — and it is
		// referenced by the entry, so it must not move either.
		RegistrationSet(const RegistrationSet&) = delete;
		RegistrationSet& operator=(const RegistrationSet&) = delete;
		RegistrationSet(RegistrationSet&&) = delete;
		RegistrationSet& operator=(RegistrationSet&&) = delete;

		// False when REAPER refused, in which case nothing was added — so a refused
		// registration is not something `release_all` will later hand back a second
		// time.
		bool add(const RegistrationRequest& request)
		{
			if (request.payload == nullptr) {
				return false;
			}

			if (!registrar_.register_item(request.kind, request.payload)) {
				return false;
			}

			held_.push_back(request);

			return true;
		}

		// Idempotent, so the destructor can run after an explicit teardown already did,
		// and so a second `shut_down` does not unregister anything twice.
		void release_all()
		{
			// Backwards: the panel is registered last and goes first, because the action
			// that shows it must not outlive it.
			while (!held_.empty()) {
				const RegistrationRequest& held = held_.back();
				registrar_.unregister_item(held.kind, held.payload);
				held_.pop_back();
			}
		}

		std::size_t size() const noexcept { return held_.size(); }
		bool empty() const noexcept { return held_.empty(); }

		const std::vector<RegistrationRequest>& held() const noexcept { return held_; }

	private:
		ExtensionRegistrar& registrar_;
		std::vector<RegistrationRequest> held_;
	};

	// ---------------------------------------------------------------------------
	// What the entry is made of
	// ---------------------------------------------------------------------------

	// Creating the CEF browser inside REAPER's dockable panel (requirement 1.3).
	//
	// `parent_window_handle` is the panel — an `HWND` on Windows and a SWELL `HWND` on
	// macOS and Linux, held as `void*` so this header needs neither `windows.h` nor
	// swell. False when CEF refused.
	//
	// A seam rather than a direct call into `ui::UiHost`, for the reason
	// `CMakeLists.txt` records: `src/ui/cef_browser_host.cpp` is the only translation
	// unit allowed to need a CEF distribution, and a build without one drops it and
	// keeps going. Naming the CEF-backed browser host from here would make the
	// extension's library unbuildable without CEF rather than merely CEF-less.
	using PanelBrowserCreator = std::function<bool(void* parent_window_handle)>;

	// What the extension registers at load, and the timer that drives the Main-Thread
	// Dispatcher.
	//
	// `start_timer` is deliberately not a `TimerRegistration` and not a
	// `TimerRegistrar`: the registration owns a process-wide static and the tick it
	// runs reaches the dispatcher, the Message Dispatcher, and the UI Host. Composing
	// that is `plugin_entry.cpp`'s job. What belongs here is the ordering — the timer
	// starts after the registrations are taken, because a tick arriving before the
	// panel exists has a destination that is not there yet.
	struct StartupPlan {
		std::vector<RegistrationRequest> registrations;

		// False when REAPER refused the timer registration. A silently unregistered
		// timer is an extension that loads, shows its panel, and never processes a
		// message.
		std::function<bool()> start_timer;

		PanelBrowserCreator create_panel_browser;
	};

	// The four teardown steps this component performs itself, and what performs each.
	//
	// A callable per step rather than an interface with four methods: these reach four
	// unrelated types, two of which do not exist yet, and what this component is for is
	// the ordering, not their shapes. An empty callable means there is nothing wired
	// behind that step — recorded as such rather than as a failure, because during the
	// build-out "there is no socket to close" and "the socket refused to close" must
	// not be the same answer.
	//
	// The fifth step, exit, is this component's own: it releases the registrations it
	// took at load. There is no callable for it because there is nothing to delegate —
	// the symmetry is the point, and handing half of it to a caller is how the two
	// lists get out of step again.
	struct TeardownSteps {
		std::function<bool()> unregister_timer;
		std::function<bool()> destroy_queues;

		// Step three takes the sequence because the UI Host claims the step itself —
		// `UiHost::shut_down(ShutdownSequence&)`. See the file comment.
		std::function<bool(ShutdownSequence&)> shut_down_cef;

		std::function<bool()> close_socket_io_connection;

		// Anything the exit step has to do beyond releasing the registrations. Optional.
		std::function<bool()> on_exit;
	};

	// ---------------------------------------------------------------------------
	// Outcomes
	// ---------------------------------------------------------------------------

	struct LoadReport {
		// What `ReaperPluginEntry` returns 1 for.
		bool loaded = false;

		bool timer_started = false;
		std::size_t registrations_taken = 0;

		// True when a refusal partway through sent everything already taken back. The
		// alternative was staying loaded with half the extension registered.
		bool rolled_back = false;

		// What REAPER refused, named. Empty on a successful load.
		std::vector<std::string> failures;
	};

	enum class PanelRefusalReason {
		none,

		// The panel cannot be opened before the extension finished loading, or after it
		// tore down. The second is the one that matters: CEF is gone.
		extension_not_loaded,
		extension_shut_down,

		// REAPER handed over no panel window. Creating a browser with no parent is the
		// crash, not the empty panel.
		no_panel_window,

		// Nothing is wired to create a browser. Distinct from CEF refusing, so a build
		// with no CEF distribution reads as what it is.
		no_browser_creator,

		// CEF said no. `UiHost`'s own refusal reason says which.
		browser_creation_refused
	};

	struct PanelOutcome {
		bool browser_created = false;

		// The panel was opened again and the browser is already in it. CEF is
		// initialized once per process and holds one browser per panel, so the second
		// open finds the first one rather than creating another.
		bool already_open = false;

		PanelRefusalReason refusal = PanelRefusalReason::none;
	};

	struct ShutdownReport {
		// In the order they were performed, which is the order requirement 1.2 fixes.
		// Asserted rather than inferred — the whole task is this list.
		std::vector<ShutdownStep> steps_performed;

		// Steps that were this component's to claim but had nothing behind them. They
		// advance the sequence; they are reported so that "nothing was torn down" does
		// not read as "everything was".
		std::vector<ShutdownStep> steps_with_nothing_to_tear_down;

		bool completed = false;

		// A second teardown. REAPER calls the entry point with a null registration
		// table on unload, and an error path may already have torn down.
		bool already_shut_down = false;

		// The step that stopped the teardown, and why. Nothing after it ran.
		bool step_failed = false;
		ShutdownStep failed_step = ShutdownStep::unregister_timer;
		std::string failure_description;

		std::size_t registrations_released = 0;
	};

	// What `ReaperPluginEntry` returns, as REAPER reads it: 1 means compatible and the
	// extension stays loaded, anything else means it is unloaded.
	//
	// A named function rather than a `return loaded ? 1 : 0` inside the entry point,
	// because the entry point is the one thing in this component the suite cannot reach
	// — it is an exported C symbol in the translation unit that includes the SDK. The
	// inverse of this is an extension that registers everything correctly and is then
	// unloaded, and nothing in that symptom points at a return statement. Pulled out
	// here, it is one assertion.
	inline constexpr int reaper_plugin_entry_result(bool loaded)
	{
		return loaded ? 1 : 0;
	}

	// REAPER's second call, with a null registration table, is the unload. The return
	// value is not read — the module is going either way — and 0 is the convention.
	inline constexpr int reaper_plugin_entry_unload_result()
	{
		return 0;
	}

	// ---------------------------------------------------------------------------
	// The Plugin Entry
	// ---------------------------------------------------------------------------

	// Main-thread only. REAPER calls the entry point on the thread that loads the
	// extension, the panel is opened from REAPER's UI, and every teardown step is
	// ordered against a timer tick that runs on the same thread — so there is nothing
	// here to lock and nothing here that may be reached from the network thread.
	class PluginEntry {
	public:
		PluginEntry(RegistrationSet& registrations, StartupPlan plan, TeardownSteps teardown)
			: registrations_{registrations},
			plan_{std::move(plan)},
			teardown_{std::move(teardown)}
		{
		}

		// REAPER holds pointers into what this owns, and the teardown order depends on
		// the declaration order of whatever holds it. Neither copying nor moving it is
		// meaningful.
		PluginEntry(const PluginEntry&) = delete;
		PluginEntry& operator=(const PluginEntry&) = delete;
		PluginEntry(PluginEntry&&) = delete;
		PluginEntry& operator=(PluginEntry&&) = delete;

		// Requirement 1.1. Takes the registrations, then starts the timer.
		//
		// Registrations first and the timer last, because a tick that arrives before
		// the panel is registered has nowhere to publish. On any refusal, everything
		// taken so far goes back — see the file comment on why partially loaded is not
		// a state this has.
		LoadReport load()
		{
			LoadReport report;

			if (loaded_ || shut_down_) {
				report.loaded = loaded_;
				return report;
			}

			for (const RegistrationRequest& registration : plan_.registrations) {
				if (registrations_.add(registration)) {
					++report.registrations_taken;
					continue;
				}

				report.failures.push_back(
					"reaper refused to register the " + std::string{describe(registration.kind)}
						+ " '" + registration.description + "'"
				);

				return roll_back(report);
			}

			if (!plan_.start_timer) {
				report.failures.emplace_back(
					"no timer was wired, so nothing would drive the main-thread dispatcher"
				);

				return roll_back(report);
			}

			if (!plan_.start_timer()) {
				report.failures.emplace_back(
					"reaper refused the timer registration, so nothing would drive the "
					"main-thread dispatcher"
				);

				return roll_back(report);
			}

			report.timer_started = true;
			timer_started_ = true;
			loaded_ = true;
			report.loaded = true;

			return report;
		}

		// Requirement 1.3. The browser goes inside REAPER's panel, so it cannot be
		// created until REAPER has a panel to put it in.
		PanelOutcome open_panel(void* parent_window_handle)
		{
			PanelOutcome outcome;

			if (shut_down_) {
				outcome.refusal = PanelRefusalReason::extension_shut_down;
				return outcome;
			}

			if (!loaded_) {
				outcome.refusal = PanelRefusalReason::extension_not_loaded;
				return outcome;
			}

			if (panel_browser_created_) {
				outcome.already_open = true;
				return outcome;
			}

			if (parent_window_handle == nullptr) {
				outcome.refusal = PanelRefusalReason::no_panel_window;
				return outcome;
			}

			if (!plan_.create_panel_browser) {
				outcome.refusal = PanelRefusalReason::no_browser_creator;
				return outcome;
			}

			if (!plan_.create_panel_browser(parent_window_handle)) {
				outcome.refusal = PanelRefusalReason::browser_creation_refused;
				return outcome;
			}

			panel_browser_created_ = true;
			outcome.browser_created = true;

			return outcome;
		}

		// Requirement 1.2. All five steps, in order, stopping at the first one whose
		// component failed.
		ShutdownReport shut_down(ShutdownSequence& sequence)
		{
			ShutdownReport report;

			if (shut_down_) {
				report.already_shut_down = true;
				return report;
			}

			shut_down_ = true;

			// One: the timer. First so that no tick can reach anything the later steps
			// are taking apart.
			if (!perform_step(sequence, ShutdownStep::unregister_timer, teardown_.unregister_timer, report)) {
				return report;
			}

			timer_started_ = false;

			// Two: the queues. After the timer, so nothing is draining them; before CEF
			// and the socket, so nothing is left holding work whose destination is gone.
			if (!perform_step(sequence, ShutdownStep::destroy_queues, teardown_.destroy_queues, report)) {
				return report;
			}

			// Three: CEF, claimed by the UI Host rather than here.
			if (!perform_cef_step(sequence, report)) {
				return report;
			}

			panel_browser_created_ = false;

			// Four: the socket.
			if (!perform_step(
					sequence,
					ShutdownStep::close_socket_io_connection,
					teardown_.close_socket_io_connection,
					report)) {
				return report;
			}

			// Five: exit, which is where the registrations taken at load go back.
			if (!perform_exit_step(sequence, report)) {
				return report;
			}

			report.completed = sequence.completed();

			return report;
		}

		bool loaded() const noexcept { return loaded_; }
		bool timer_started() const noexcept { return timer_started_; }
		bool was_shut_down() const noexcept { return shut_down_; }
		bool panel_browser_created() const noexcept { return panel_browser_created_; }

	private:
		LoadReport roll_back(LoadReport& report)
		{
			registrations_.release_all();

			report.loaded = false;
			report.rolled_back = true;

			return report;
		}

		// Claim the step, then do the work. False when the teardown must stop here.
		bool perform_step(
			ShutdownSequence& sequence,
			ShutdownStep step,
			const std::function<bool()>& perform,
			ShutdownReport& report)
		{
			if (!sequence.record(step)) {
				return fail_step(step, sequence, report);
			}

			if (!perform) {
				report.steps_with_nothing_to_tear_down.push_back(step);
				report.steps_performed.push_back(step);
				return true;
			}

			if (!perform()) {
				report.step_failed = true;
				report.failed_step = step;
				report.failure_description =
					"teardown stopped: the component for '" + std::string{describe(step)}
						+ "' reported a failure, and every later step is ordered against it";

				return false;
			}

			report.steps_performed.push_back(step);

			return true;
		}

		// Step three. The UI Host records `shut_down_cef` itself, so this hands over the
		// sequence and then checks that the step was actually claimed — a CEF step that
		// advanced nothing must not let the socket close on top of a live browser.
		bool perform_cef_step(ShutdownSequence& sequence, ShutdownReport& report)
		{
			if (!teardown_.shut_down_cef) {
				if (!sequence.record(ShutdownStep::shut_down_cef)) {
					return fail_step(ShutdownStep::shut_down_cef, sequence, report);
				}

				report.steps_with_nothing_to_tear_down.push_back(ShutdownStep::shut_down_cef);
				report.steps_performed.push_back(ShutdownStep::shut_down_cef);

				return true;
			}

			const bool cef_shut_down = teardown_.shut_down_cef(sequence);

			// Claimed by the component, so "did the step happen" is a question about the
			// sequence rather than about the return value alone.
			const bool step_claimed = !sequence.is_next(ShutdownStep::shut_down_cef);

			if (!cef_shut_down || !step_claimed) {
				report.step_failed = true;
				report.failed_step = ShutdownStep::shut_down_cef;
				report.failure_description = step_claimed
					? "teardown stopped: cef refused to shut down, and the socket must not "
						"close on top of a live browser"
					: "teardown stopped: the cef step did not claim 'shut down cef' from the "
						"sequence, so nothing proves cef is down";

				return false;
			}

			report.steps_performed.push_back(ShutdownStep::shut_down_cef);

			return true;
		}

		bool perform_exit_step(ShutdownSequence& sequence, ShutdownReport& report)
		{
			if (!sequence.record(ShutdownStep::exit_extension)) {
				return fail_step(ShutdownStep::exit_extension, sequence, report);
			}

			// Requirement 1.1's registrations, given back. Counted before the release so
			// the report says what went rather than what is left.
			report.registrations_released = registrations_.size();
			registrations_.release_all();

			if (teardown_.on_exit && !teardown_.on_exit()) {
				report.step_failed = true;
				report.failed_step = ShutdownStep::exit_extension;
				report.failure_description =
					"teardown stopped: the exit step reported a failure after the "
					"registrations were released";

				return false;
			}

			report.steps_performed.push_back(ShutdownStep::exit_extension);

			return true;
		}

		// The sequence refused the step. That means something already walked it out of
		// order, so the only safe thing left is to stop and say so.
		bool fail_step(ShutdownStep step, const ShutdownSequence& sequence, ShutdownReport& report)
		{
			report.step_failed = true;
			report.failed_step = step;
			report.failure_description = sequence.violations().empty()
				? "teardown stopped: '" + std::string{describe(step)} + "' was refused"
				: sequence.violations().back().description;

			return false;
		}

		RegistrationSet& registrations_;
		StartupPlan plan_;
		TeardownSteps teardown_;

		bool loaded_ = false;
		bool timer_started_ = false;
		bool shut_down_ = false;
		bool panel_browser_created_ = false;
	};

}

#endif
