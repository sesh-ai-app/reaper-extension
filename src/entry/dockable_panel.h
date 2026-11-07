// Opening, docking, and tearing down the chat panel (task 25.2, requirements 1.1, 1.3,
// 15.1).
//
// The panel action is the one way in. When it fires, the panel window is created if
// there is none, docked into REAPER's docker if it is not docked, brought to the front,
// and handed to `PluginEntry::open_panel`, which puts the browser in it. Each of those
// can refuse, and each refusal is reported by name rather than leaving the producer
// with an action that did nothing.
//
// The decisions are here and the platform is not, the same split as
// `entry/panel_window.h`: `entry/plugin_entry.cpp` supplies the seams —
// `DockWindowAddEx` and `DockWindowRemove` behind the `RegistrationSet`,
// `DockWindowActivate` behind `PanelWindowActivation`, `CreateDialogParam` behind
// `PanelWindow` — and the suite drives everything below against recorders.
//
// ---------------------------------------------------------------------------
// Docking is a registration
//
// `DockWindowAddEx` goes into the same `RegistrationSet` as the actions, the action
// hook, and the menu item, as `RegistrationKind::dockable_panel`. So `DockWindowRemove`
// is not a call anyone has to remember: it is that registration's release, made by the
// same `release_all` that gives back everything else, at the exit step of the teardown.
// The panel is docked after load, so it is the last registration taken and the first
// released — before the action that shows it, which is the order `plugin_entry.h`
// already wanted.
//
// ---------------------------------------------------------------------------
// Where the window goes in the teardown
//
// Three things constrain it:
//
//   - CEF's browser is a child of the panel window. Destroying a parent under a live
//     browser is the crash CEF documents, so the window outlives step three.
//   - A docked window has to leave the docker before it is destroyed, or REAPER keeps
//     a handle to a window that no longer exists.
//   - Undocking is the dock registration's release, which is step five's.
//
// So the window is destroyed at the very end of step five, after `release_all`, as the
// exit step's `on_exit`. `destroy_window` checks both preconditions rather than trusting
// the wiring, and refuses by name when either does not hold — a window left alive at
// unload is a leak, and one destroyed out of order is a crash in a producer's session.
// The documented order did not change; step five gained a defined last action.

#ifndef SESH_AI_ENTRY_DOCKABLE_PANEL_H
#define SESH_AI_ENTRY_DOCKABLE_PANEL_H

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <entry/extension_log.h>
#include <entry/panel_window.h>
#include <entry/plugin_entry.h>

namespace sesh_ai::entry {

	// Why `DockablePanel::open` stopped before the panel was shown.
	enum class PanelOpeningRefusal {
		none,

		// The action can only reach an extension that loaded and has not torn down. The
		// second is the one that matters: the registrations are going and CEF is gone.
		extension_not_loaded,
		extension_shut_down,

		// The window could not be created. `PanelOpeningReport::window_outcome` says
		// which of `PanelWindow`'s refusals it was.
		window_not_created,

		// REAPER would not take the dock registration — in practice, `DockWindowAddEx`
		// or `DockWindowRemove` was not supplied. The window is destroyed again rather
		// than kept, so the next attempt starts from nothing and there is never a panel
		// window the docker does not know about.
		dock_refused,
	};

	constexpr std::string_view describe_panel_opening_refusal(PanelOpeningRefusal refusal) noexcept
	{
		switch (refusal) {
			case PanelOpeningRefusal::none:
				return "the chat panel was opened";
			case PanelOpeningRefusal::extension_not_loaded:
				return "the extension has not finished loading";
			case PanelOpeningRefusal::extension_shut_down:
				return "the extension has already shut down";
			case PanelOpeningRefusal::window_not_created:
				return "the panel window could not be created";
			case PanelOpeningRefusal::dock_refused:
				return "REAPER did not dock the panel window";
		}

		return "the panel opening refusal is not recognised";
	}

	// `PluginEntry::open_panel`'s refusals, for the log line. Every enumerator has its
	// own text, which the suite asserts.
	constexpr std::string_view describe_panel_refusal_reason(PanelRefusalReason reason) noexcept
	{
		switch (reason) {
			case PanelRefusalReason::none:
				return "the browser is in the panel";
			case PanelRefusalReason::extension_not_loaded:
				return "the extension has not finished loading";
			case PanelRefusalReason::extension_shut_down:
				return "the extension has already shut down";
			case PanelRefusalReason::no_panel_window:
				return "there is no panel window to put the browser in";
			case PanelRefusalReason::no_browser_creator:
				return "the extension has nothing that can create a browser";
			case PanelRefusalReason::browser_creation_refused:
				return "the browser could not be created";
		}

		return "the panel refusal reason is not recognised";
	}

	struct PanelOpeningReport {
		PanelOpeningRefusal refusal = PanelOpeningRefusal::none;

		// What `PanelWindow::create` returned, or `already_created` when there was a
		// window before this call and it was not asked.
		PanelWindowCreationOutcome window_outcome = PanelWindowCreationOutcome::already_created;

		// What this call did, as opposed to what was already true before it.
		bool window_created = false;
		bool docked = false;
		bool activated = false;

		// Whether `PluginEntry::open_panel` was reached, and what it said. The panel is
		// shown either way once it is reached; an empty panel with a line explaining why
		// is the honest result when a file the browser needs is missing.
		bool browser_attempted = false;
		PanelOutcome browser;

		bool panel_shown() const noexcept { return refusal == PanelOpeningRefusal::none; }
	};

	// What `DockablePanel::destroy_window` did.
	enum class PanelWindowDestructionOutcome {
		destroyed,

		// There was no window. Not a failure: the panel may never have been opened.
		no_window,

		// The dock registration is still held, so REAPER's docker still has the window.
		// Destroying it now would leave the docker a dangling handle.
		still_docked,

		// The plugin entry still has a browser in the panel, so CEF's teardown has not
		// run. Destroying the parent under it is the crash.
		browser_still_open,
	};

	constexpr std::string_view describe_panel_window_destruction_outcome(
		PanelWindowDestructionOutcome outcome) noexcept
	{
		switch (outcome) {
			case PanelWindowDestructionOutcome::destroyed:
				return "the panel window was destroyed";
			case PanelWindowDestructionOutcome::no_window:
				return "there was no panel window to destroy";
			case PanelWindowDestructionOutcome::still_docked:
				return "the panel window was not destroyed because it is still in REAPER's docker";
			case PanelWindowDestructionOutcome::browser_still_open:
				return "the panel window was not destroyed because the browser inside it has not "
					"been shut down";
		}

		return "the panel window destruction outcome is not recognised";
	}

	// Brings a docked panel to the front — `DockWindowActivate`, and showing a window the
	// producer had closed. Optional: an unwired one means the panel is docked and the
	// browser created, just not raised.
	using PanelWindowActivation = std::function<void(void* window)>;

	// The panel action's sequence, and the window's end.
	//
	// Holds references only. The window, the registration set, and the entry are all
	// members of the plugin entry's `LoadedExtension`, whose declaration order is what
	// keeps them alive for as long as this is used; this object adds no lifetime of its
	// own, which is why it has no destructor that does anything.
	//
	// Main-thread only, for the reason `PluginEntry` is: it is reached from the action
	// hook and from the unload call, both REAPER's own UI callbacks.
	class DockablePanel {
	public:
		DockablePanel(
			PanelWindow& window,
			RegistrationSet& registrations,
			PluginEntry& entry,
			PanelWindowActivation activate_window,
			std::string dock_description)
			: window_(window),
			registrations_(registrations),
			entry_(entry),
			activate_window_(std::move(activate_window)),
			dock_description_(std::move(dock_description))
		{
		}

		DockablePanel(const DockablePanel&) = delete;
		DockablePanel& operator=(const DockablePanel&) = delete;
		DockablePanel(DockablePanel&&) = delete;
		DockablePanel& operator=(DockablePanel&&) = delete;

		// Creates, docks, raises, and fills the panel, doing only the parts not already
		// done. `parent_window` is REAPER's main window, `hwnd_main`.
		//
		// The order is the dependency order. A window before the dock, because the dock
		// takes the window. The dock before the browser, so the browser is created inside
		// a window already at its docked size rather than resized into it. The browser
		// last, through `PluginEntry::open_panel`, which owns the refusals about CEF and
		// answers a second open with `already_open`.
		PanelOpeningReport open(void* parent_window)
		{
			PanelOpeningReport report;

			// Before anything is created: a window made now could never be docked, and
			// would be destroyed by nothing but the destructor.
			if (entry_.was_shut_down()) {
				report.refusal = PanelOpeningRefusal::extension_shut_down;
				return report;
			}

			if (!entry_.loaded()) {
				report.refusal = PanelOpeningRefusal::extension_not_loaded;
				return report;
			}

			if (!window_.exists()) {
				report.window_outcome = window_.create(parent_window);

				if (report.window_outcome != PanelWindowCreationOutcome::created) {
					report.refusal = PanelOpeningRefusal::window_not_created;
					return report;
				}

				report.window_created = true;
			}

			if (!docked()) {
				RegistrationRequest dock_registration;
				dock_registration.kind = RegistrationKind::dockable_panel;
				dock_registration.payload = window_.native_window();
				dock_registration.description = dock_description_;

				if (!registrations_.add(dock_registration)) {
					window_.destroy();

					report.refusal = PanelOpeningRefusal::dock_refused;
					return report;
				}

				report.docked = true;
			}

			if (activate_window_) {
				activate_window_(window_.native_window());
				report.activated = true;
			}

			report.browser_attempted = true;
			report.browser = entry_.open_panel(window_.native_window());

			return report;
		}

		// The window's end, for the exit step's `on_exit` — after `release_all` has
		// undocked it and after step three has shut the browser down. See the file
		// comment for why it is there.
		//
		// Refuses rather than proceeding when either precondition does not hold. The
		// window then survives to `PanelWindow`'s destructor, which is the backstop and
		// which runs after the registrations' own.
		PanelWindowDestructionOutcome destroy_window()
		{
			if (!window_.exists()) {
				return PanelWindowDestructionOutcome::no_window;
			}

			if (docked()) {
				return PanelWindowDestructionOutcome::still_docked;
			}

			if (entry_.panel_browser_created()) {
				return PanelWindowDestructionOutcome::browser_still_open;
			}

			window_.destroy();

			return PanelWindowDestructionOutcome::destroyed;
		}

		// Whether REAPER's docker holds the window, read from the registration set rather
		// than tracked here. The set is what `release_all` empties, so a flag kept beside
		// it would be a second list that could disagree with the first.
		bool docked() const
		{
			const void* const window = window_.native_window();

			if (window == nullptr) {
				return false;
			}

			for (const RegistrationRequest& held : registrations_.held()) {
				if (held.kind == RegistrationKind::dockable_panel && held.payload == window) {
					return true;
				}
			}

			return false;
		}

		void* native_window() const noexcept { return window_.native_window(); }

	private:
		PanelWindow& window_;
		RegistrationSet& registrations_;
		PluginEntry& entry_;
		PanelWindowActivation activate_window_;
		std::string dock_description_;
	};

	// The log lines for one opening of the panel.
	//
	// A panel that opened with its browser writes nothing — the producer can see it
	// worked. Everything else writes one line, because every other outcome looks the
	// same from REAPER: an action that did nothing, or an empty panel.
	//
	// `browser_unavailable_reason` is `PanelBrowserInstallation::unavailable_reason` and
	// `browser_failure_reason` is `PanelBrowserState::last_failure_reason`: the first
	// explains `no_browser_creator`, the second `browser_creation_refused`.
	//
	// "No browser creator" is information rather than a warning, because the case it
	// meets most is an expected state: a development build loaded from the build tree,
	// where the React application's assets are not beside the binary. Nothing
	// else about the producer's session is broken by it, and the line names what to
	// put in place.
	inline void write_panel_opening_lines(
		const PanelOpeningReport& report,
		std::string_view browser_unavailable_reason,
		std::string_view browser_failure_reason,
		ExtensionLog& log)
	{
		if (!report.panel_shown()) {
			std::string line{"the chat panel was not opened: "};
			line.append(describe_panel_opening_refusal(report.refusal));

			if (report.refusal == PanelOpeningRefusal::window_not_created) {
				line.append(" (");
				line.append(describe_panel_window_creation_outcome(report.window_outcome));
				line.append(")");
			}

			log.write(LogSeverity::warning, line);

			return;
		}

		if (!report.browser_attempted
			|| report.browser.browser_created
			|| report.browser.already_open) {
			return;
		}

		std::string line{"the chat panel opened without a browser: "};
		line.append(describe_panel_refusal_reason(report.browser.refusal));

		if (report.browser.refusal == PanelRefusalReason::no_browser_creator) {
			if (!browser_unavailable_reason.empty()) {
				line.append(". ");
				line.append(browser_unavailable_reason);
			}

			log.write(LogSeverity::information, line);

			return;
		}

		if (report.browser.refusal == PanelRefusalReason::browser_creation_refused
			&& !browser_failure_reason.empty()) {
			line.append(": ");
			line.append(browser_failure_reason);
		}

		log.write(LogSeverity::warning, line);
	}

}

#endif
