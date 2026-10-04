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
// What is not here yet, named so it is not mistaken for done
//
// Three pieces of requirements 1.1, 1.3, and 2.4 are still open, and the three are at
// different distances from done. Task 22.1 built the composition root
// (`entry/extension_composition.h`), so the shape of what is missing has changed since
// this comment was first written — what remains is one body of work, not three.
//
//   - **The panel window itself.** Unchanged, and deliberately out of 22.1's scope.
//     `DockWindowAddEx` takes an `HWND`, and producing one needs a dialog resource plus
//     the SWELL resource-generation step on macOS and Linux — a `CMakeLists.txt` change
//     and packaging work (task 21.x). So the panel registration is taken by
//     `register_panel_window`, which the window creation calls once it exists. The
//     registrar's half of it is complete and symmetric; the window is not.
//
//   - **The CEF-backed browser.** The seam is now filled in the right place.
//     `entry/panel_browser_creator.h` declares `make_panel_browser_creator`, and
//     `entry/cef_panel_browser_creator.cpp` — compiled only when a CEF distribution was
//     located, exactly as `src/ui/cef_browser_host.cpp` is — is the one translation
//     unit outside that file which names `CefPanelBrowserHost`. It is still not called
//     from here, because it takes a `ui::UiHost&` and a `ui::DeferredBrowserHost&`, both
//     of which are members of the composed graph; calling it means constructing the
//     graph, which is the third item.
//
//   - **The graph, bound to nlohmann/json and the REAPER C API.**
//     `ExtensionObjectGraph` wires all seven of the Message Dispatcher's destinations,
//     composes the Tool Executor's registry over the nine REAPER-backed adapters, and
//     hands back both the Message Dispatcher for `bind_envelope_router` and the publish
//     step for the UI state slot. The suite drives the whole of it against stubs in
//     `tests/entry/extension_composition_test.cpp`, including all 42 tools registering
//     and an envelope routed through the bound router reaching its destination.
//
//     What it has to be *given* is the JSON half: a reader and a writer per tool across
//     the 42 (the per-family `*_payload_codec` structs under `src/daw/tools/`), a
//     `read_tool_call` and a `write_dispatch_outcome` for the executor seam, and a
//     payload parser for the bridge. None of those exists yet, and none of them is
//     written here: they are nlohmann/json code, and
//     `ToolRegistryCompositionReport::unregistered_constrained_tool_names` is what
//     reports the gap at load rather than one unknown-tool error at a time.
//
// None of the three is faked. `open_panel` reports `no_browser_creator`, the panel
// registration is simply not taken, and the router slot is empty — which the
// Main-Thread Dispatcher reads as "nothing routes yet" and answers by leaving the
// inbound queue alone. All three are what the log says, rather than something that
// claims to have happened.

#include <entry/plugin_entry.h>

// Project headers before `reaper_plugin.h`, and that ordering is load-bearing rather
// than tidy. On macOS and Linux the SDK header reaches for WDL's swell, which defines
// `min` and `max` as function-like macros; a header parsed after it that calls
// `std::min` or `std::max` has the qualified call eaten before the compiler sees it.
// Parsed before, there is nothing to eat. The same hazard is why nothing below this
// line calls either. See `daw/tools/reaper_track_state_host.cpp`, where it was first
// hit.
#include <entry/extension_log.h>
#include <entry/main_thread_dispatcher.h>
#include <entry/reaper_timer_registrar.h>
#include <entry/shutdown_sequence.h>
#include <entry/timer_registration.h>
#include <transport/envelope.h>

#include <reaper_plugin.h>

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
			dispatcher_ = std::make_unique<Dispatcher>(
				*queues_,

				// The Message Dispatcher's slot (requirement 2.4). Still empty, which
				// the Main-Thread Dispatcher reads as "nothing routes yet" and answers
				// by leaving the inbound queue alone and reporting that work remains —
				// envelopes wait rather than being drained into nowhere.
				//
				// The one call that fills it is
				// `bind_envelope_router<InboundEnvelope>(graph.message_dispatcher(), log_)`.
				// Both halves of that now exist and both are tested:
				// `bind_envelope_router` against the real `transport::MessageDispatcher`
				// in `tests/entry/extension_log_test.cpp`, and the graph — all seven
				// destinations, the registry over the nine REAPER-backed adapters —
				// against stubs in `tests/entry/extension_composition_test.cpp`.
				//
				// What is missing is the JSON half the graph has to be given: a reader
				// and a writer per tool across the 42, the executor seam's
				// `read_tool_call` and `write_dispatch_outcome`, and a payload parser
				// for the bridge. See the file comment.
				//
				// So the slot stays empty rather than being filled with a dispatcher
				// whose Tool Executor answers "unknown tool" to all 42 tools. That would
				// not be a partially wired extension, it would be a working-looking one
				// that refuses everything.
				Dispatcher::EnvelopeRouter{},

				// The handoff to the network thread, which arrives with the Transport
				// Client.
				Dispatcher::OutboundEnvelopeSender{},

				// Publishing UI state to CEF, which arrives with the browser host.
				Dispatcher::UiStatePublisher{}
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

			// The panel window does not exist yet — see the file comment. Once it does,
			// this shows it and `open_panel` creates the browser inside it.
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

			// The dockable panel is deliberately not here. It needs an `HWND`, and the
			// panel window does not exist yet — `register_panel_window` takes it into
			// the same set when it does. See the file comment.

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

			// `create_panel_browser` is left unset. The creator itself now exists —
			// `make_panel_browser_creator`, behind `entry/panel_browser_creator.h`,
			// defined in the one translation unit `CMakeLists.txt` lets need CEF — but
			// it takes the UI Host and the deferred browser host out of the composed
			// graph, so installing it here means constructing the graph. See the file
			// comment.

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
			teardown.destroy_queues = [this] {
				dispatcher_.reset();
				queues_.reset();
				return true;
			};

			// Three is the UI Host's, and four is the Transport Client's. Both are left
			// unset, which `PluginEntry` records as a step with nothing to tear down
			// rather than as a step that failed — the distinction matters, because a
			// socket that refused to close must stop the teardown and a socket that does
			// not exist must not.

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
		// The module handle is REAPER's to keep. Nothing here needs it — the panel
		// window will, once there is a dialog resource to load.
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
