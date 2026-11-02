// The dockable panel's native window, and what creating it can report (task 25.1,
// requirements 1.1, 1.3).
//
// `DockWindowAddEx` takes an `HWND`, and this is where the extension gets one. The
// window is a dialog built from `entry/panel_window.rc`: compiled directly on Windows,
// and turned into SWELL's form on macOS and Linux by WDL's generator at build time.
// `CMakeLists.txt` records what the generator is, where it lives at the pinned commit,
// and why it runs when it does.
//
// ---------------------------------------------------------------------------
// The split
//
// This header is the decision logic and owns no platform type. The window travels as
// `void*`, the same way `PanelBrowserCreator` takes it, so the suite can compile this
// without `windows.h` or swell. `entry/reaper_panel_window.cpp` is the other half: the
// one translation unit that calls `CreateDialogParam` and `DestroyWindow`, reached
// through `PanelWindowPlatform`.
//
// What lives here rather than there is everything with a branch in it: refusing a
// second window, refusing a missing parent before the platform sees it, telling "no
// platform was wired" apart from "the platform refused", and destroying exactly once.
//
// ---------------------------------------------------------------------------
// What this does not do
//
// It does not dock the window. Task 25.2 hands `native_window()` to the panel
// registration — `LoadedExtension::register_panel_window` in `entry/plugin_entry.cpp`,
// which `DockWindowAddEx` sits behind — and to the `PanelBrowserCreator`.
//
// Nor does it decide when the window is destroyed relative to the rest of teardown.
// A docked window has to leave REAPER's docker before it is destroyed, and the
// registration set releases the dock at the very end of `plugin_entry.h`'s sequence,
// after CEF has shut down. Where a `PanelWindow` sits among `LoadedExtension`'s members
// is therefore 25.2's decision. The destructor here is a backstop, the same way the
// member order in `LoadedExtension` is, not the mechanism.

#ifndef SESH_AI_ENTRY_PANEL_WINDOW_H
#define SESH_AI_ENTRY_PANEL_WINDOW_H

#include <functional>
#include <string_view>
#include <utility>

namespace sesh_ai::entry {

	// What `PanelWindow::create` did. Named outcomes rather than a bool, because the
	// three failures have three different next steps for whoever reads the log line.
	enum class PanelWindowCreationOutcome {
		// A window now exists, and `native_window()` returns it.
		created,

		// A window already existed. Nothing was created, and the existing window is
		// still the one `native_window()` returns. Not a failure: the panel action can
		// fire many times while the window lives, and 25.2 docks it once.
		already_created,

		// No platform was wired, so nothing could be created. A build fault rather than
		// a runtime condition: `entry/reaper_panel_window.cpp` always fills both seams.
		no_window_platform,

		// The parent window was null. REAPER supplies its main window in the
		// registration table as `hwnd_main`; a dialog created without it would float
		// outside REAPER's window hierarchy, where the docker cannot reach it. Refused
		// before the platform is called rather than left for the platform to decide,
		// because Win32 would accept a null parent and create a top-level window.
		no_parent_window,

		// The platform was called and returned no window. On Windows this is
		// `CreateDialogParam` not finding the resource in the module it was given. On
		// macOS and Linux it is also what an unresolved SWELL looks like: the module
		// stub replaces every SWELL function REAPER did not supply with one that returns
		// zero, so a missing `SWELL_CreateDialog` arrives here as a null window rather
		// than as a crash.
		platform_refused,
	};

	// The line the plugin entry logs for an outcome. Every enumerator has its own text,
	// which the suite asserts.
	constexpr std::string_view describe_panel_window_creation_outcome(
		PanelWindowCreationOutcome outcome) noexcept
	{
		switch (outcome) {
			case PanelWindowCreationOutcome::created:
				return "the panel window was created";
			case PanelWindowCreationOutcome::already_created:
				return "the panel window already exists";
			case PanelWindowCreationOutcome::no_window_platform:
				return "no platform was wired to create the panel window";
			case PanelWindowCreationOutcome::no_parent_window:
				return "REAPER supplied no main window to parent the panel window to";
			case PanelWindowCreationOutcome::platform_refused:
				return "the platform did not create the panel window from its dialog resource";
		}

		return "the panel window creation outcome is not recognised";
	}

	// The two platform calls a panel window needs, as seams.
	//
	// `create_window` takes the parent and returns the new window or null. It is where
	// `CreateDialogParam` is called, and the module handle it needs on Windows is
	// captured when the platform is built rather than passed here, because nothing on
	// this side of the seam has any use for it.
	//
	// `destroy_window` is handed a window `create_window` returned, exactly once. It must
	// not throw: it runs from `PanelWindow`'s destructor.
	struct PanelWindowPlatform {
		std::function<void*(void* parent_window)> create_window;
		std::function<void(void* window)> destroy_window;

		bool complete() const { return static_cast<bool>(create_window) && static_cast<bool>(destroy_window); }
	};

	// Owns the panel's native window: at most one at a time, destroyed exactly once.
	//
	// Neither copyable nor movable. The window handle is about to be held by REAPER's
	// docker and by CEF, and an owner that could be copied would be two owners of one
	// window, both of which destroy it.
	class PanelWindow {
	public:
		explicit PanelWindow(PanelWindowPlatform platform)
			: platform_(std::move(platform))
		{
		}

		~PanelWindow()
		{
			destroy();
		}

		PanelWindow(const PanelWindow&) = delete;
		PanelWindow& operator=(const PanelWindow&) = delete;
		PanelWindow(PanelWindow&&) = delete;
		PanelWindow& operator=(PanelWindow&&) = delete;

		// Creates the window, owned by `parent_window`, unless one already exists.
		//
		// The checks run in this order on purpose. An existing window is reported first,
		// so a second call cannot be turned into a failure by its arguments; the platform
		// is checked before the parent, because a build with no platform cannot create a
		// window whatever REAPER passes it.
		PanelWindowCreationOutcome create(void* parent_window)
		{
			if (native_window_ != nullptr) {
				return PanelWindowCreationOutcome::already_created;
			}

			if (!platform_.complete()) {
				return PanelWindowCreationOutcome::no_window_platform;
			}

			if (parent_window == nullptr) {
				return PanelWindowCreationOutcome::no_parent_window;
			}

			void* const created_window = platform_.create_window(parent_window);

			if (created_window == nullptr) {
				return PanelWindowCreationOutcome::platform_refused;
			}

			native_window_ = created_window;

			return PanelWindowCreationOutcome::created;
		}

		// Destroys the window if there is one. Idempotent, so an explicit teardown and
		// the destructor can both call it.
		//
		// The handle is cleared before the platform is called, so a re-entrant call —
		// `DestroyWindow` sends `WM_DESTROY` synchronously, and 25.2's dialog procedure
		// may answer it — finds nothing left to destroy.
		void destroy()
		{
			if (native_window_ == nullptr) {
				return;
			}

			void* const window_being_destroyed = native_window_;
			native_window_ = nullptr;

			platform_.destroy_window(window_being_destroyed);
		}

		// The window, or null when there is none. What 25.2 hands to `DockWindowAddEx`
		// and to the `PanelBrowserCreator`.
		void* native_window() const noexcept { return native_window_; }

		bool exists() const noexcept { return native_window_ != nullptr; }

	private:
		PanelWindowPlatform platform_;
		void* native_window_ = nullptr;
	};

}

#endif
