// The one translation unit that includes the REAPER SDK for the plugin entry, and the
// one place the extension's exported C symbol lives.
//
// `ReaperPluginEntry` is the only symbol REAPER looks up by name, and its contract is
// the SDK's, not ours: it takes the module instance and the registration table, returns
// 1 to stay loaded and anything else to be unloaded, and is called a second time with a
// null table when it is time to tear down. The name and the export attribute come from
// the SDK's own `REAPER_PLUGIN_ENTRYPOINT` and `REAPER_PLUGIN_DLL_EXPORT` rather than
// being written out here, so there is no spelling of it to get wrong.
//
// Everything below the entry point is translation: a `PluginEntry` call in, one or two
// REAPER calls out. The ordering, the rollback, the registration symmetry, and the
// teardown sequence are all in `plugin_entry.h`, which is what makes them testable
// without REAPER. If a change here needs a branch on anything other than "did REAPER
// give me a value", the logic belongs on the other side of the seam.
//
// The function pointers for the dockable panel are resolved from the registration
// table's `GetFunc`, not through `REAPERAPI_IMPLEMENT`. Same two reasons as
// `daw/reaper_render_host.cpp` and `entry/reaper_timer_registrar.cpp`: the import
// mechanism needs one translation unit to provide storage for every pointer in the SDK,
// which is a large decision to make on behalf of a file that needs three of them; and
// resolving locally turns a function REAPER did not supply into something this object
// can report rather than a null call inside a producer's session.
//
// ---------------------------------------------------------------------------
// What is wired, and what is not
//
// Task 24.3 filled the router slot. `compose_extension`
// (`entry/composed_extension.h`, defined in `entry/extension_composition.cpp`) builds
// the whole object graph against nlohmann/json and the nine REAPER-backed adapters, and
// `bind_envelope_router` binds its Message Dispatcher into the slot below — which, until
// that landed, was a default-constructed router that the Main-Thread Dispatcher read as
// "nothing routes yet". So an inbound envelope now reaches one of seven destinations,
// all 42 tools are registered, and `load` writes the line that says how many.
//
// Two pieces of requirements 1.1 and 1.3 are still open, and both are about the window
// rather than the graph:
//
//   - **The panel window itself.** `DockWindowAddEx` takes an `HWND`. Task 25.1 built
//     the way to get one: the dialog resource `entry/panel_window.rc`, the SWELL
//     resource-generation step `CMakeLists.txt` runs on macOS and Linux, and
//     `entry/panel_window.h` with `entry/reaper_panel_window.h` to create the window.
//     Nothing here creates it yet. Doing that when the panel action fires, and passing
//     the result to `register_panel_window`, is task 25.2. The registrar's half is
//     complete and symmetric; the call into it is not.
//
//   - **The CEF-backed browser.** `entry/panel_browser_creator.h` declares
//     `make_panel_browser_creator`, and `entry/cef_panel_browser_creator.cpp` —
//     compiled only when a CEF distribution was located, exactly as
//     `src/ui/cef_browser_host.cpp` is — is the one translation unit outside that file
//     which names `CefPanelBrowserHost`. Both references it takes are now reachable, on
//     `ComposedExtension`. What it still needs is the panel's `HWND` and the resolved
//     asset directory, which arrive with the window above: task 25.2.
//
// Neither is faked. `open_panel` reports `no_browser_creator` and the panel registration
// is simply not taken, which is what the log says rather than something claiming to have
// happened. The UI state publish step *is* wired, and publishes through the
// `DeferredBrowserHost` — which reports that it has no browser rather than losing the
// state, so the presenters' accumulation is correct the moment there is one.

#include <entry/plugin_entry.h>

// Project headers before the SDK, and that ordering is load-bearing rather than tidy.
// On macOS and Linux the SDK header reaches for WDL's swell, which defines `min` and
// `max` as function-like macros; a header parsed after it that calls `std::min` or
// `std::max` has the qualified call eaten before the compiler sees it. Parsed before,
// there is nothing to eat. The same hazard is why nothing below this line calls either.
// See `daw/tools/reaper_track_state_host.cpp`, where it was first hit, and
// `reaper_sdk.h`, which is the only route to the SDK in this tree.
#include <entry/composed_extension.h>
#include <entry/extension_log.h>
#include <entry/main_thread_dispatcher.h>
#include <entry/reaper_timer_registrar.h>
#include <entry/shutdown_sequence.h>
#include <entry/timer_registration.h>
#include <transport/envelope.h>

#include <reaper_sdk.h>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

	using sesh_ai::entry::ExtensionLog;
	using sesh_ai::entry::ExtensionRegistrar;
	using sesh_ai::entry::LoadReport;
	using sesh_ai::entry::LogSeverity;
	using sesh_ai::entry::PluginEntry;
	using sesh_ai::entry::RegistrationKind;
	using sesh_ai::entry::RegistrationRequest;
	using sesh_ai::entry::RegistrationSet;
	using sesh_ai::entry::ShutdownReport;
	using sesh_ai::entry::ShutdownSequence;
	using sesh_ai::entry::StartupPlan;
	using sesh_ai::entry::TeardownSteps;
	using sesh_ai::entry::TickReport;
	using sesh_ai::entry::TickReportLog;
	using sesh_ai::entry::TimerRegistration;
	using sesh_ai::entry::ReaperTimerRegistrar;

	using Queues = sesh_ai::transport::QueuePair<
		sesh_ai::transport::InboundEnvelope,
		sesh_ai::transport::OutboundEnvelope
	>;

	using Dispatcher = sesh_ai::entry::MainThreadDispatcher<
		sesh_ai::transport::InboundEnvelope,
		sesh_ai::transport::OutboundEnvelope
	>;

	// What the producer sees in the action list, and the identifier REAPER keys the
	// command on. The identifier is persisted in the producer's key bindings and
	// toolbars, so renaming it silently breaks both — it is a value, not a string
	// literal at a call site.
	constexpr const char* show_panel_command_identifier = "SESHAI_SHOW_CHAT_PANEL";
	constexpr const char* show_panel_action_description = "Sesh AI: Show the chat panel";

	// The panel's name in REAPER's docker, and the identifier REAPER stores its dock
	// position under. Same reasoning as above: the second one is persisted state.
	constexpr const char* panel_display_name = "Sesh AI";
	constexpr const char* panel_dock_identifier = "sesh_ai_chat_panel";

	// ---------------------------------------------------------------------------
	// Where a log line goes in this build
	// ---------------------------------------------------------------------------

	// Standard error, and deliberately not `ShowConsoleMsg`.
	//
	// REAPER's console is a window that pops up in front of whatever the producer is
	// doing. Requirement 5.8's line is the common case — the server's protocol being
	// one version ahead of the extension is expected, not a fault — and interrupting a
	// mixing session to announce it would make the extension feel broken for a
	// condition in which nothing is wrong. Standard error is where a developer running
	// REAPER from a terminal looks, and it costs a producer who is not looking nothing.
	//
	// Anything the *producer* needs to be told goes to the panel instead, which is the
	// UI Host's surface and not a log.
	//
	// The seam is what matters here rather than the destination: `ExtensionLog` holds a
	// sink, so when there is a diagnostics surface worth having — a file, a page in the
	// panel — it replaces this one function and nothing else moves.
	void write_log_line_to_standard_error(LogSeverity severity, std::string_view line)
	{
		// Built into one string and written once. Two `fputs` calls can interleave with
		// another thread's line between them, and a half-written line is worse than no
		// line.
		std::string formatted_line{"[Sesh AI] "};
		formatted_line.append(describe(severity));
		formatted_line.append(": ");
		formatted_line.append(line);
		formatted_line.push_back('\n');

		std::fputs(formatted_line.c_str(), stderr);

		// Flushed, because the line that matters most is the last one before REAPER
		// went down, and that is exactly the one a buffer loses.
		std::fflush(stderr);
	}

	// ---------------------------------------------------------------------------
	// The REAPER side of the registration seam
	// ---------------------------------------------------------------------------

	// The registration string REAPER's `Register` takes for each kind, and the same
	// string with the `-` prefix REAPER uses to remove a registration.
	//
	// `dockable_panel` has none, because the panel is not a table entry — it goes
	// through `DockWindowAddEx` and `DockWindowRemove`.
	const char* reaper_registration_name(RegistrationKind kind)
	{
		switch (kind) {
		case RegistrationKind::action:
			return "gaccel";
		case RegistrationKind::action_hook:
			return "hookcommand";
		case RegistrationKind::menu_item:
			return "hookcustommenu";
		case RegistrationKind::dockable_panel:
			return nullptr;
		}

		return nullptr;
	}

	const char* reaper_unregistration_name(RegistrationKind kind)
	{
		switch (kind) {
		case RegistrationKind::action:
			return "-gaccel";
		case RegistrationKind::action_hook:
			return "-hookcommand";
		case RegistrationKind::menu_item:
			return "-hookcustommenu";
		case RegistrationKind::dockable_panel:
			return nullptr;
		}

		return nullptr;
	}

	// Registers through the table REAPER hands the extension at load.
	//
	// `Register` and `GetFunc` are the two entries of that table, and it stays valid
	// for the lifetime of the loaded extension — which outlives this object.
	class ReaperExtensionRegistrar final : public ExtensionRegistrar {
	public:
		explicit ReaperExtensionRegistrar(reaper_plugin_info_t* plugin_info)
			: plugin_info_{plugin_info}
		{
			if (plugin_info_ == nullptr || plugin_info_->GetFunc == nullptr) {
				unresolved_function_names_.emplace_back("GetFunc");
				return;
			}

			resolve_function("DockWindowAddEx", dock_window_add_);
			resolve_function("DockWindowRemove", dock_window_remove_);
			resolve_function("DockWindowActivate", dock_window_activate_);
		}

		// False when a function the panel needs could not be resolved. The actions and
		// the menu item go through `Register`, which is in the table itself, so they do
		// not depend on this.
		bool can_register_panel() const
		{
			return dock_window_add_ != nullptr && dock_window_remove_ != nullptr;
		}

		// The names REAPER did not supply, for the log line that explains why the panel
		// is unavailable. Reported rather than inferred, the same way
		// `ReaperRenderHost::unresolved_function_names` is.
		const std::vector<std::string>& unresolved_function_names() const
		{
			return unresolved_function_names_;
		}

		bool register_item(RegistrationKind kind, void* payload) override
		{
			if (plugin_info_ == nullptr || payload == nullptr) {
				return false;
			}

			if (kind == RegistrationKind::dockable_panel) {
				if (!can_register_panel()) {
					return false;
				}

				dock_window_add_(
					static_cast<HWND>(payload),
					panel_display_name,
					panel_dock_identifier,
					true
				);

				// `DockWindowAddEx` reports nothing. Docking the window is the whole
				// operation and there is no state to read back, so a resolved function
				// and a non-null window is as much as can honestly be checked.
				return true;
			}

			if (plugin_info_->Register == nullptr) {
				return false;
			}

			const char* const registration_name = reaper_registration_name(kind);

			if (registration_name == nullptr) {
				return false;
			}

			// REAPER returns the number of registrations of that kind it now holds, so
			// zero means the registration did not take.
			return plugin_info_->Register(registration_name, payload) > 0;
		}

		void unregister_item(RegistrationKind kind, void* payload) override
		{
			if (plugin_info_ == nullptr || payload == nullptr) {
				return;
			}

			if (kind == RegistrationKind::dockable_panel) {
				if (dock_window_remove_ != nullptr) {
					dock_window_remove_(static_cast<HWND>(payload));
				}

				return;
			}

			if (plugin_info_->Register == nullptr) {
				return;
			}

			const char* const unregistration_name = reaper_unregistration_name(kind);

			if (unregistration_name == nullptr) {
				return;
			}

			plugin_info_->Register(unregistration_name, payload);
		}

		// Brings the panel to the front once it is docked. Not a registration, so it is
		// not in the set.
		void activate_panel(HWND panel_window) const
		{
			if (dock_window_activate_ != nullptr && panel_window != nullptr) {
				dock_window_activate_(panel_window);
			}
		}

	private:
		template <typename FunctionPointer>
		void resolve_function(const char* name, FunctionPointer& destination)
		{
			void* const resolved = plugin_info_->GetFunc(name);

			if (resolved == nullptr) {
				unresolved_function_names_.emplace_back(name);
				return;
			}

			destination = reinterpret_cast<FunctionPointer>(resolved);
		}

		reaper_plugin_info_t* plugin_info_;

		void (*dock_window_add_)(HWND, const char*, const char*, bool) = nullptr;
		void (*dock_window_remove_)(HWND) = nullptr;
		void (*dock_window_activate_)(HWND) = nullptr;

		std::vector<std::string> unresolved_function_names_;
	};

	// ---------------------------------------------------------------------------
	// The loaded extension
	// ---------------------------------------------------------------------------

	class LoadedExtension;

	// One extension instance per REAPER process, which is the actual cardinality rather
	// than a limitation being accepted — REAPER's own callbacks carry no context, so
	// `hookcommand` and `hookcustommenu` have to find the instance the same way
	// `TimerRegistration` does.
	LoadedExtension* loaded_extension = nullptr;

	// Everything the extension owns while it is loaded, in the order it must be
	// destroyed.
	//
	// Member declaration order is load-bearing, and it is the backstop for requirement
	// 1.2 rather than the mechanism: `shut_down` is what walks the five steps in order,
	// and these destructors are what happens if something returned early before it got
	// there. C++ destroys members in reverse declaration order, so reading the list
	// bottom-up gives the teardown: the entry, then the timer registration (the timer
	// stops), then the registrations REAPER holds, then the registrar, then the
	// dispatcher, then the queues, and the log last of all — it is written to by the
	// tick, by `load`, and by `shut_down`, so it has to outlive every one of them.
	// Timer before queues, which is the constraint `entry/timer_registration.h`
	// describes.
	class LoadedExtension {
	public:
		explicit LoadedExtension(reaper_plugin_info_t* plugin_info)
			: plugin_info_{plugin_info},
			log_{&write_log_line_to_standard_error},
			tick_log_{log_},
			queues_{std::make_unique<Queues>()},
			registrar_{plugin_info},
			registrations_{registrar_},
			timer_registrar_{plugin_info},
			timer_registration_{timer_registrar_},
			entry_{registrations_, build_startup_plan(), build_teardown_steps()}
		{
			// The graph, before the dispatcher that routes into it.
			//
			// `request_ui_state_publication` is the one edge that runs the other way:
			// the Transport Client presents a connection status from the network thread
			// and the publish has to reach the tick (requirement 2.3). The dispatcher
			// does not exist yet, so the callable finds it through this object the same
			// way the tick does — and `request_ui_state_publication` is documented safe
			// from any thread, which is the property that makes a lambda here correct
			// rather than convenient.
			composed_ = sesh_ai::entry::compose_extension(
				sesh_ai::entry::ComposedExtensionDependencies{
					plugin_info,
					&queues_->outbound(),
					[this] {
						if (dispatcher_ != nullptr) {
							dispatcher_->request_ui_state_publication();
						}
					}
				}
			);

			dispatcher_ = std::make_unique<Dispatcher>(
				*queues_,

				// The Message Dispatcher's slot (requirement 2.4), filled.
				//
				// Task 24.3's deliverable. Before it, this was a default-constructed
				// router that the Main-Thread Dispatcher read as "nothing routes yet"
				// and answered by leaving the inbound queue alone — because the graph
				// could be built but had nothing to give its Tool Executor, so all 42
				// tools were unregistered and a filled slot would have been an extension
				// that looked like it worked and refused everything.
				//
				// `bind_envelope_router` is the adapter between the slot's
				// `void(InboundEnvelope)` and the Message Dispatcher's `RoutingOutcome`,
				// and it is also where requirement 5.8's line is written: an envelope
				// type this build has no route for is logged once and ignored, because
				// the server's protocol may be a version ahead.
				//
				// An empty handle keeps the old behaviour rather than crashing on load,
				// and `load` writes the line that says so.
				composed_ == nullptr
					? Dispatcher::EnvelopeRouter{}
					: sesh_ai::entry::bind_envelope_router<sesh_ai::transport::InboundEnvelope>(
						composed_->message_dispatcher(),
						log_
					),

				// The handoff to the network thread, which arrives with the Transport
				// Client.
				Dispatcher::OutboundEnvelopeSender{},

				// Publishing to CEF, inside the tick and nowhere else (requirement 2.3).
				// A turn's forty stream deltas are one repaint because the presenters
				// accumulate and this step takes — so it is wired now even though the
				// browser is not, and publishes through the `DeferredBrowserHost`, which
				// reports that it has no browser rather than dropping the state.
				composed_ == nullptr
					? Dispatcher::UiStatePublisher{}
					: composed_->ui_state_publisher()
			);
		}

		LoadedExtension(const LoadedExtension&) = delete;
		LoadedExtension& operator=(const LoadedExtension&) = delete;
		LoadedExtension(LoadedExtension&&) = delete;
		LoadedExtension& operator=(LoadedExtension&&) = delete;

		LoadReport load()
		{
			LoadReport report = entry_.load();

			// REAPER reads a 0 from the entry point and unloads the module without
			// saying why. `LoadReport::failures` already names which registration was
			// refused; written down here, that name is the difference between "Sesh AI
			// did not load" and a line pointing at the registration REAPER declined.
			for (const std::string& failure : report.failures) {
				log_.write(LogSeverity::warning, failure);
			}

			write_tool_registry_lines();

			return report;
		}

		// REAPER's unload call. The sequence is created here rather than held as a
		// member so that a second unload cannot walk a sequence that is already
		// complete — `PluginEntry::shut_down` reports the second call as already shut
		// down before the sequence is touched.
		void shut_down()
		{
			ShutdownSequence sequence;
			const ShutdownReport report = entry_.shut_down(sequence);

			// A teardown that stopped partway is the condition requirement 1.2 exists
			// to prevent, and the crash it turns into happens after this returns — so
			// the line has to be written now, while there is still a process to write
			// it from.
			if (report.step_failed) {
				log_.write(LogSeverity::warning, report.failure_description);
			}
		}

		// What `hookcommand` runs. True when this extension handled the action, which
		// stops REAPER passing it on.
		bool run_action(int command_id)
		{
			if (show_panel_command_id_ == 0 || command_id != show_panel_command_id_) {
				return false;
			}

			// Nothing creates the panel window yet — see the file comment. Task 25.2
			// creates it here through `entry/panel_window.h`, after which this shows it
			// and `open_panel` creates the browser inside it.
			if (panel_window_ == nullptr) {
				return true;
			}

			registrar_.activate_panel(panel_window_);
			entry_.open_panel(panel_window_);

			return true;
		}

		// Called by the panel window's creation, once there is one. Takes the panel
		// registration into the same set everything else is in, so it is released by the
		// same `release_all`.
		bool register_panel_window(HWND panel_window)
		{
			if (panel_window == nullptr || panel_window_ != nullptr) {
				return false;
			}

			RegistrationRequest panel_registration;
			panel_registration.kind = RegistrationKind::dockable_panel;
			panel_registration.payload = panel_window;
			panel_registration.description = panel_display_name;

			if (!registrations_.add(panel_registration)) {
				return false;
			}

			panel_window_ = panel_window;

			return true;
		}

		int show_panel_command_id() const { return show_panel_command_id_; }

	private:
		// What the Tool Executor's registry ended up holding, as one line at load.
		//
		// This is the line task 24 exists to make possible. The registry declines a name
		// that is not one of the 42, a second handler for one tool, and an empty handler
		// — and every `register_*_tools` declines an incomplete codec rather than
		// registering a handler that would throw. All three of those produce the same
		// symptom in a producer's session: the agent is told a tool it can see in its own
		// tool list does not exist. Written here, the gap is a line at startup instead.
		//
		// A complete registry writes one informational line and nothing else, because
		// "all 42 registered" is worth confirming once and is not worth a warning.
		void write_tool_registry_lines()
		{
			if (composed_ == nullptr) {
				log_.write(
					LogSeverity::warning,
					"the extension's object graph was not composed, so no tool is registered and "
					"no envelope is routed"
				);

				return;
			}

			const sesh_ai::entry::ToolRegistryCompositionReport& report =
				composed_->tool_registry_report();

			std::string line{"registered "};
			line.append(std::to_string(report.registered_tool_names.size()));
			line.append(" tools");

			if (report.every_constrained_tool_registered() && !report.anything_was_refused()) {
				log_.write(LogSeverity::information, line);

				return;
			}

			// The two halves are different failures and are named separately. A refusal
			// is a name this composition offered and the registry would not take; a name
			// left out is one nothing even offered, which is what an absent codec looks
			// like.
			for (const sesh_ai::entry::ToolRegistrationFailure& failure : report.failures) {
				line.append("; refused ");
				line.append(failure.tool_name);
				line.append(" (");
				line.append(
					sesh_ai::daw::describe_tool_registration_outcome(failure.outcome)
				);
				line.append(")");
			}

			for (const std::string& tool_name : report.unregistered_constrained_tool_names) {
				line.append("; unregistered ");
				line.append(tool_name);
			}

			log_.write(LogSeverity::warning, line);
		}

		StartupPlan build_startup_plan()
		{
			StartupPlan plan;

			// The command ID comes first: a `gaccel` registration carries it, so there
			// is nothing to register until REAPER has allocated one. Zero means REAPER
			// has run out of command IDs, and the registration below then fails on a
			// null payload rather than registering an action nothing can invoke.
			show_panel_command_id_ = allocate_command_id(show_panel_command_identifier);

			if (show_panel_command_id_ != 0) {
				show_panel_action_.accel.cmd =
					static_cast<decltype(show_panel_action_.accel.cmd)>(show_panel_command_id_);
				show_panel_action_.desc = show_panel_action_description;

				RegistrationRequest action;
				action.kind = RegistrationKind::action;
				action.payload = &show_panel_action_;
				action.description = show_panel_action_description;
				plan.registrations.push_back(std::move(action));

				RegistrationRequest action_hook;
				action_hook.kind = RegistrationKind::action_hook;
				action_hook.payload = reinterpret_cast<void*>(&dispatch_action_to_loaded_extension);
				action_hook.description = "the hook that runs Sesh AI's actions";
				plan.registrations.push_back(std::move(action_hook));
			}

			RegistrationRequest menu_item;
			menu_item.kind = RegistrationKind::menu_item;
			menu_item.payload = reinterpret_cast<void*>(&populate_menu_for_loaded_extension);
			menu_item.description = "the Sesh AI entry in REAPER's customizable menus";
			plan.registrations.push_back(std::move(menu_item));

			// The dockable panel is deliberately not here. It needs an `HWND`, which is
			// created when the panel action fires rather than at load —
			// `register_panel_window` takes it into the same set then. Task 25.1 built
			// the window; 25.2 creates and docks it. See the file comment.

			plan.start_timer = [this] {
				// The tick. One of these is the only legal window in the extension for a
				// REAPER C API call, and the dispatcher is what bounds the work it does
				// — see `entry/main_thread_dispatcher.h`.
				return timer_registration_.start([this] {
					if (dispatcher_ != nullptr) {
						const TickReport report = dispatcher_->tick();

						// Returned rather than logged by the dispatcher, because this is
						// the component that knows how this build reports things. What it
						// decides is mostly what *not* to write: the timer runs about
						// thirty times a second on the thread that draws REAPER's UI, so
						// a clean tick writes nothing, the per-tick budget being reached
						// writes nothing, and a dropped envelope writes one line the
						// first time and a summary if it keeps happening. The reasoning
						// is in `entry/extension_log.h`.
						tick_log_.note(report);
					}
				});
			};

			// `create_panel_browser` is left unset, and what is missing has narrowed to
			// one thing. `make_panel_browser_creator` — behind
			// `entry/panel_browser_creator.h`, defined in the one translation unit
			// `CMakeLists.txt` lets need CEF — takes the UI Host and the deferred
			// browser host, and both are now reachable on `composed_`. What it also
			// takes is `PanelBrowserSettings`: the directory Vite's output was installed
			// into, and the CEF runtime layout. Resolving those needs REAPER's resource
			// path and the window they are rendered into, which is task 25.2's.
			//
			// So `open_panel` reports `no_browser_creator`, which is the honest answer
			// rather than a creator that would be handed a null window.

			return plan;
		}

		TeardownSteps build_teardown_steps()
		{
			TeardownSteps teardown;

			// One. Idempotent, and the destructor calls it again.
			teardown.unregister_timer = [this] {
				timer_registration_.stop();
				return !timer_registration_.is_running();
			};

			// Two. The dispatcher goes with the queues: it holds a reference to them and
			// exists only to drain them, so leaving it behind would leave a reference to
			// something destroyed.
			//
			// The graph is *not* destroyed here, and its outbound-queue reference is
			// therefore dead from this point until step three. That is deliberate and it
			// is the only arrangement requirement 1.2's order allows: CEF shuts down at
			// step three through the UI Host, which is a member of the graph, so the
			// graph has to outlive this step. Nothing reads the reference in between —
			// the timer was unregistered at step one, so the tick that routes and the
			// publish that pushes are both already gone, and `UiHost::shut_down` touches
			// the browser and nothing else.
			teardown.destroy_queues = [this] {
				dispatcher_.reset();
				queues_.reset();
				return true;
			};

			// Three. The UI Host claims the step from the sequence itself, before it
			// touches CEF, so an out-of-order teardown is refused while the browser is
			// still alive rather than detected after REAPER has crashed.
			//
			// `already_shut_down` is reported rather than treated as a failure — an
			// error path may have torn down before this ran — so the step succeeds for
			// both of the outcomes in which CEF is, in fact, down.
			teardown.shut_down_cef = [this](ShutdownSequence& sequence) {
				if (composed_ == nullptr) {
					// Nothing composed, so there is no UI Host and no browser. The step
					// still has to be claimed, or the sequence refuses step four.
					return sequence.record(sesh_ai::entry::ShutdownStep::shut_down_cef);
				}

				const sesh_ai::ui::ShutdownOutcome outcome =
					composed_->ui_host().shut_down(sequence);

				// After the UI Host's step, because this is what destroys the UI Host.
				// Its queue reference has been dead since step two — see there.
				composed_.reset();

				return outcome.cef_shut_down || outcome.already_shut_down;
			};

			// Four is the Transport Client's, left unset — which `PluginEntry` records
			// as a step with nothing to tear down rather than as a step that failed. The
			// distinction matters: a socket that refused to close must stop the teardown
			// and a socket that does not exist must not.

			return teardown;
		}

		int allocate_command_id(const char* command_identifier)
		{
			if (plugin_info_ == nullptr || plugin_info_->Register == nullptr) {
				return 0;
			}

			// `command_id` takes a unique string and returns the ID REAPER assigned, or
			// 0 when it has none left. The cast away from const is REAPER's signature,
			// not a choice: `Register` takes `void*` for every registration kind.
			return plugin_info_->Register(
				"command_id",
				const_cast<char*>(command_identifier)
			);
		}

		// REAPER's `hookcommand` and `hookcustommenu` callbacks carry no context, so
		// they find the one loaded instance the same way the timer trampoline does.
		static bool dispatch_action_to_loaded_extension(int command_id, int /*flag*/)
		{
			if (loaded_extension == nullptr) {
				return false;
			}

			// REAPER calls this through a C function pointer, and an exception unwinding
			// across that boundary is undefined behaviour — in practice a REAPER crash,
			// which producers experience as losing their session.
			try {
				return loaded_extension->run_action(command_id);
			} catch (...) {
				return false;
			}
		}

		// Flag 0 is REAPER initialising the default menu, which is when items may be
		// added; flag 1 is each time it is shown, which is when dynamic state may be
		// set. Populating the menu needs the platform menu API and belongs with the
		// panel window work — the registration is real and symmetric, its body is not
		// written yet.
		static void populate_menu_for_loaded_extension(
			const char* /*menu_identifier*/,
			void* /*menu*/,
			int /*flag*/)
		{
		}

		// The plain data first, so it is constructed before anything that reads it and
		// destroyed after anything that holds a pointer into it. `show_panel_action_` is
		// the one that matters: REAPER keeps the address of it for as long as the
		// `gaccel` registration stands.
		reaper_plugin_info_t* plugin_info_ = nullptr;
		gaccel_register_t show_panel_action_{};
		int show_panel_command_id_ = 0;
		HWND panel_window_ = nullptr;

		// Before everything below, so it is destroyed after all of it. The tick writes
		// to it, `load` writes to it, and `shut_down` writes to it — the last of those
		// runs while the members below are being taken apart, so a log destroyed with
		// them would be one the teardown's own failure line could not reach.
		//
		// `tick_log_` holds a reference to `log_`, hence the order of these two.
		ExtensionLog log_;
		TickReportLog tick_log_;

		// Declaration order from here down is the teardown backstop. See the class
		// comment: reading it bottom-up gives the order the members are destroyed in,
		// and that order is requirement 1.2's.
		std::unique_ptr<Queues> queues_;

		// Between the queues and the dispatcher, which is both constraints at once: the
		// graph holds the outbound queue by reference, so the queues must be constructed
		// first and destroyed last of the three; and the dispatcher's router is bound
		// into the graph, so the dispatcher must be destroyed first. Reading the three
		// bottom-up gives exactly that.
		std::unique_ptr<sesh_ai::entry::ComposedExtension> composed_;

		std::unique_ptr<Dispatcher> dispatcher_;
		ReaperExtensionRegistrar registrar_;
		RegistrationSet registrations_;
		ReaperTimerRegistrar timer_registrar_;
		TimerRegistration timer_registration_;
		PluginEntry entry_;
	};

}

// ---------------------------------------------------------------------------
// The entry point
// ---------------------------------------------------------------------------

extern "C" {

	// REAPER's contract, from `reaper_plugin.h`:
	//
	//   int ReaperPluginEntry(HINSTANCE hInstance, reaper_plugin_info_t *rec);
	//   return 1 if you are compatible (anything else will result in plugin being unloaded)
	//   if rec == NULL, then time to unload
	//
	// Both the name and the export attribute are the SDK's macros, so neither is spelled
	// out here and neither can drift.
	REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
		REAPER_PLUGIN_HINSTANCE instance,
		reaper_plugin_info_t* plugin_info)
	{
		// The module handle is REAPER's to keep. Nothing here uses it yet. On Windows
		// it is where the dialog resource lives, and task 25.2 passes it to
		// `make_reaper_panel_window_platform` (`entry/reaper_panel_window.h`).
		(void)instance;

		// REAPER calls this through a C function pointer. An exception unwinding across
		// that boundary is undefined behaviour, and on load it would be a crash before
		// the producer has seen anything at all.
		try {
			if (plugin_info == nullptr) {
				// Time to unload. The teardown walks the five steps in order; the
				// destructor is the backstop for anything that returned early.
				if (loaded_extension != nullptr) {
					loaded_extension->shut_down();

					delete loaded_extension;
					loaded_extension = nullptr;
				}

				return sesh_ai::entry::reaper_plugin_entry_unload_result();
			}

			// A second load of an extension that is already up. Returning 0 would tell
			// REAPER to unload a working extension, so the already-loaded answer is the
			// loaded one.
			if (loaded_extension != nullptr) {
				return sesh_ai::entry::reaper_plugin_entry_result(true);
			}

			// The two entries of the registration table are what every seam in the
			// extension resolves through. Without them there is nothing to register and
			// nothing to resolve, so being unloaded is the correct outcome.
			if (plugin_info->caller_version != REAPER_PLUGIN_VERSION
				|| plugin_info->Register == nullptr
				|| plugin_info->GetFunc == nullptr) {
				return sesh_ai::entry::reaper_plugin_entry_result(false);
			}

			auto extension = std::make_unique<LoadedExtension>(plugin_info);

			// Set before `load`, because the registrations `load` takes are callbacks
			// REAPER may invoke the moment it holds them, and each one finds the
			// instance through this pointer.
			loaded_extension = extension.get();

			const LoadReport report = extension->load();

			if (!report.loaded) {
				// `load` already released everything it had taken — see
				// `plugin_entry.h` on why partially loaded is not a state it has. REAPER
				// unloads the module on anything but 1, and it does not call back to
				// tear down a load that never completed, so there must be nothing left
				// registered at this point.
				loaded_extension = nullptr;

				return sesh_ai::entry::reaper_plugin_entry_result(false);
			}

			// Ownership moves to the process for as long as the extension is loaded.
			// Released rather than held in a static `unique_ptr` so that teardown
			// happens when REAPER says to, not during static destruction — which on
			// some platforms runs after REAPER's own window is gone.
			extension.release();

			return sesh_ai::entry::reaper_plugin_entry_result(report.loaded);
		} catch (...) {
			loaded_extension = nullptr;

			return sesh_ai::entry::reaper_plugin_entry_result(false);
		}
	}

}
