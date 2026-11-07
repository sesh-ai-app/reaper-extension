// Opening, docking, and tearing down the chat panel (task 25.2, requirements 1.1, 1.2,
// 1.3, 15.1).
//
// `entry/plugin_entry.cpp` fills the seams with `CreateDialogParam`, `DockWindowAddEx`,
// `DockWindowRemove`, `DockWindowActivate`, and `DestroyWindow`, none of which can run
// here. Everything below shares one call log across the window platform, the registrar,
// the browser creator, and the teardown steps, so what is asserted is the order those
// calls happen in across components — which is what this task is about:
//
//   - opening goes window, dock, raise, browser, and each refusal stops at its own step;
//   - teardown shuts CEF down while the window is docked and alive, then undocks it
//     first of the registrations, then destroys it last of everything.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/dockable_panel.h>
#include <entry/extension_log.h>
#include <entry/panel_window.h>
#include <entry/plugin_entry.h>
#include <entry/shutdown_sequence.h>

using sesh_ai::entry::DockablePanel;
using sesh_ai::entry::ExtensionLog;
using sesh_ai::entry::ExtensionRegistrar;
using sesh_ai::entry::LogSeverity;
using sesh_ai::entry::PanelOpeningRefusal;
using sesh_ai::entry::PanelOpeningReport;
using sesh_ai::entry::PanelRefusalReason;
using sesh_ai::entry::PanelWindow;
using sesh_ai::entry::PanelWindowCreationOutcome;
using sesh_ai::entry::PanelWindowDestructionOutcome;
using sesh_ai::entry::PanelWindowPlatform;
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
using sesh_ai::entry::describe_panel_opening_refusal;
using sesh_ai::entry::describe_panel_refusal_reason;
using sesh_ai::entry::describe_panel_window_destruction_outcome;
using sesh_ai::entry::write_panel_opening_lines;

namespace {

	using CallLog = std::vector<std::string>;

	// REAPER's registration table, recording into the shared log. A refused kind is how
	// an unresolved `DockWindowAddEx` looks from this side.
	class RecordingRegistrar final : public ExtensionRegistrar {
	public:
		explicit RecordingRegistrar(CallLog& call_log)
			: call_log_(call_log)
		{
		}

		bool register_item(RegistrationKind kind, void* payload) override
		{
			call_log_.push_back("register " + std::string{describe(kind)});

			if (refused_kind.has_value() && *refused_kind == kind) {
				return false;
			}

			outstanding.emplace_back(kind, payload);

			return true;
		}

		void unregister_item(RegistrationKind kind, void* payload) override
		{
			call_log_.push_back("unregister " + std::string{describe(kind)});

			for (std::size_t index = 0; index < outstanding.size(); ++index) {
				if (outstanding[index].first == kind && outstanding[index].second == payload) {
					outstanding.erase(outstanding.begin() + static_cast<std::ptrdiff_t>(index));
					return;
				}
			}
		}

		std::optional<RegistrationKind> refused_kind;
		std::vector<std::pair<RegistrationKind, void*>> outstanding;

	private:
		CallLog& call_log_;
	};

	// `CreateDialogParam` and `DestroyWindow`, recording into the shared log. The
	// windows are addresses of its own slots.
	struct RecordingWindowPlatform {
		explicit RecordingWindowPlatform(CallLog& call_log)
			: call_log(call_log)
		{
		}

		CallLog& call_log;
		bool refuses_creation = false;
		int window_slots[4] = {};
		int created_window_count = 0;
		std::vector<void*> parents_given;
		std::vector<void*> windows_destroyed;

		PanelWindowPlatform seams()
		{
			PanelWindowPlatform platform;

			platform.create_window = [this](void* parent_window) -> void* {
				call_log.emplace_back("create window");
				parents_given.push_back(parent_window);

				if (refuses_creation) {
					return nullptr;
				}

				return &window_slots[created_window_count++];
			};

			platform.destroy_window = [this](void* window) {
				call_log.emplace_back("destroy window");
				windows_destroyed.push_back(window);
			};

			return platform;
		}
	};

	int menu_item_payload = 0;
	int action_payload = 0;
	int reaper_main_window = 0;

	void* const parent_window = &reaper_main_window;

	// The whole arrangement `LoadedExtension` builds, with every REAPER call recorded.
	//
	// Declared in the order `LoadedExtension` declares its members, for the same
	// reason: the window outlives the registration set, which outlives the entry.
	struct PanelFixture {
		CallLog call_log;
		RecordingWindowPlatform window_platform{call_log};
		PanelWindow window{window_platform.seams()};
		RecordingRegistrar registrar{call_log};
		RegistrationSet registrations{registrar};

		PanelFixture()
			: PanelFixture(true)
		{
		}

		// False is an installation missing a file the browser needs. A constructor
		// argument rather than a flag set afterwards, because the startup plan is fixed
		// when `entry` is constructed.
		explicit PanelFixture(bool installs_browser_creator)
			: has_browser_creator(installs_browser_creator)
		{
		}

		std::vector<void*> browser_parents_given;
		std::vector<void*> windows_activated;
		bool browser_creation_succeeds = true;
		bool has_browser_creator = true;
		bool cef_shutdown_fails = false;

		// Set by the fixture's own `on_exit`, so a test can read what the teardown's
		// last action reported.
		std::optional<PanelWindowDestructionOutcome> destruction_at_exit;

		PluginEntry entry{registrations, build_plan(), build_teardown()};

		DockablePanel panel{
			window,
			registrations,
			entry,
			[this](void* window_to_activate) {
				call_log.emplace_back("activate window");
				windows_activated.push_back(window_to_activate);
			},
			"Sesh AI"
		};

		StartupPlan build_plan()
		{
			StartupPlan plan;

			plan.registrations = {
				RegistrationRequest{RegistrationKind::action, &action_payload, "Sesh AI: Show the chat panel"},
				RegistrationRequest{RegistrationKind::menu_item, &menu_item_payload, "the menu entry"}
			};

			plan.start_timer = [] { return true; };

			if (!has_browser_creator) {
				return plan;
			}

			plan.create_panel_browser = [this](void* panel_window) {
				call_log.emplace_back("create browser");
				browser_parents_given.push_back(panel_window);

				return browser_creation_succeeds;
			};

			return plan;
		}

		TeardownSteps build_teardown()
		{
			TeardownSteps teardown;

			teardown.unregister_timer = [this] {
				call_log.emplace_back("unregister timer");
				return true;
			};

			teardown.destroy_queues = [this] {
				call_log.emplace_back("destroy queues");
				return true;
			};

			// Shaped like `UiHost::shut_down`, and records whether the panel window was
			// still alive and docked at the moment CEF went down — the browser is its
			// child, so both must be true.
			teardown.shut_down_cef = [this](ShutdownSequence& sequence) {
				if (!sequence.record(ShutdownStep::shut_down_cef)) {
					return false;
				}

				if (cef_shutdown_fails) {
					return false;
				}

				call_log.emplace_back(
					std::string{"shut down cef with the window "}
						+ (window.exists() ? "alive" : "gone")
						+ (panel.docked() ? " and docked" : " and undocked")
				);

				return true;
			};

			teardown.on_exit = [this] {
				destruction_at_exit = panel.destroy_window();

				return *destruction_at_exit == PanelWindowDestructionOutcome::destroyed
					|| *destruction_at_exit == PanelWindowDestructionOutcome::no_window;
			};

			return teardown;
		}
	};

	// The fixture's position in the log once `load` has run, so a test can assert on
	// what came after.
	CallLog calls_after(const CallLog& call_log, std::size_t offset)
	{
		return CallLog{call_log.begin() + static_cast<std::ptrdiff_t>(offset), call_log.end()};
	}

	struct RecordedLine {
		LogSeverity severity;
		std::string text;
	};

	struct RecordingLog {
		std::vector<RecordedLine> lines;
		ExtensionLog log{[this](LogSeverity severity, std::string_view line) {
			lines.push_back(RecordedLine{severity, std::string{line}});
		}};
	};

}

// ---------------------------------------------------------------------------
// Opening (requirements 1.1, 1.3)
// ---------------------------------------------------------------------------

TEST_CASE("opening the panel creates the window, docks it, raises it, and puts the browser in it, in that order", "[entry][dockable_panel]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);

	const std::size_t after_load = fixture.call_log.size();

	const PanelOpeningReport report = fixture.panel.open(parent_window);

	CHECK(report.panel_shown());
	CHECK(report.window_outcome == PanelWindowCreationOutcome::created);
	CHECK(report.window_created);
	CHECK(report.docked);
	CHECK(report.activated);
	CHECK(report.browser_attempted);
	CHECK(report.browser.browser_created);

	CHECK(calls_after(fixture.call_log, after_load) == CallLog{
		"create window",
		"register dockable panel",
		"activate window",
		"create browser"
	});

	// One window, everywhere: REAPER's main window parents it, and it is what was
	// docked, raised, and handed to the browser creator.
	void* const panel_window = fixture.window.native_window();

	CHECK(fixture.window_platform.parents_given == std::vector<void*>{parent_window});
	CHECK(fixture.registrar.outstanding.back() == std::make_pair(RegistrationKind::dockable_panel, panel_window));
	CHECK(fixture.windows_activated == std::vector<void*>{panel_window});
	CHECK(fixture.browser_parents_given == std::vector<void*>{panel_window});
	CHECK(fixture.panel.docked());
}

TEST_CASE("opening the panel again raises the same window and creates nothing", "[entry][dockable_panel]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	REQUIRE(fixture.panel.open(parent_window).browser.browser_created);

	const std::size_t after_first_open = fixture.call_log.size();

	const PanelOpeningReport report = fixture.panel.open(parent_window);

	CHECK(report.panel_shown());
	CHECK_FALSE(report.window_created);
	CHECK_FALSE(report.docked);
	CHECK(report.activated);
	CHECK(report.browser.already_open);

	// The action is how a producer gets a closed panel back, so it raises every time —
	// and does nothing else.
	CHECK(calls_after(fixture.call_log, after_first_open) == CallLog{"activate window"});
	CHECK(fixture.window_platform.created_window_count == 1);
}

TEST_CASE("the panel cannot be opened before the extension loads or after it shut down", "[entry][dockable_panel]")
{
	PanelFixture fixture;

	SECTION("before load")
	{
		const PanelOpeningReport report = fixture.panel.open(parent_window);

		CHECK(report.refusal == PanelOpeningRefusal::extension_not_loaded);
		CHECK(fixture.call_log.empty());
	}

	SECTION("after teardown, when nothing could ever undock a new window")
	{
		REQUIRE(fixture.entry.load().loaded);

		ShutdownSequence sequence;
		REQUIRE(fixture.entry.shut_down(sequence).completed);

		const std::size_t after_teardown = fixture.call_log.size();
		const PanelOpeningReport report = fixture.panel.open(parent_window);

		CHECK(report.refusal == PanelOpeningRefusal::extension_shut_down);
		CHECK(fixture.call_log.size() == after_teardown);
	}

	CHECK_FALSE(fixture.window.exists());
}

TEST_CASE("a window that cannot be created stops the opening before anything is docked", "[entry][dockable_panel]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	fixture.window_platform.refuses_creation = true;

	SECTION("the platform returned no window")
	{
		const PanelOpeningReport report = fixture.panel.open(parent_window);

		CHECK(report.refusal == PanelOpeningRefusal::window_not_created);
		CHECK(report.window_outcome == PanelWindowCreationOutcome::platform_refused);
	}

	SECTION("REAPER supplied no main window")
	{
		fixture.window_platform.refuses_creation = false;

		const PanelOpeningReport report = fixture.panel.open(nullptr);

		CHECK(report.refusal == PanelOpeningRefusal::window_not_created);
		CHECK(report.window_outcome == PanelWindowCreationOutcome::no_parent_window);
	}

	CHECK_FALSE(fixture.panel.docked());
	CHECK(fixture.registrar.outstanding.size() == 2);
	CHECK(fixture.browser_parents_given.empty());
}

TEST_CASE("a dock REAPER refuses destroys the window again, and the next opening starts over", "[entry][dockable_panel]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	fixture.registrar.refused_kind = RegistrationKind::dockable_panel;

	const PanelOpeningReport refused = fixture.panel.open(parent_window);

	CHECK(refused.refusal == PanelOpeningRefusal::dock_refused);
	CHECK_FALSE(refused.browser_attempted);

	// No panel window the docker does not know about.
	CHECK_FALSE(fixture.window.exists());
	CHECK(fixture.window_platform.windows_destroyed == std::vector<void*>{&fixture.window_platform.window_slots[0]});
	CHECK(fixture.browser_parents_given.empty());

	fixture.registrar.refused_kind.reset();

	const PanelOpeningReport opened = fixture.panel.open(parent_window);

	CHECK(opened.panel_shown());
	CHECK(opened.window_created);
	CHECK(opened.docked);
	CHECK(fixture.browser_parents_given == std::vector<void*>{&fixture.window_platform.window_slots[1]});
}

TEST_CASE("a panel with no browser creator is still docked and shown, and says why it is empty", "[entry][dockable_panel]")
{
	// An installation missing a file the browser needs. The panel exists; there is just
	// nothing to draw in it.
	PanelFixture fixture{false};
	REQUIRE(fixture.entry.load().loaded);

	const PanelOpeningReport report = fixture.panel.open(parent_window);

	CHECK(report.panel_shown());
	CHECK(report.docked);
	CHECK(report.browser_attempted);
	CHECK(report.browser.refusal == PanelRefusalReason::no_browser_creator);
	CHECK(fixture.panel.docked());
}

// ---------------------------------------------------------------------------
// Teardown (requirement 1.2)
// ---------------------------------------------------------------------------

TEST_CASE("teardown shuts cef down inside a docked window, then undocks it, then destroys it last", "[entry][dockable_panel][teardown]")
{
	// The ordering task 25.2 settles. CEF's browser is the window's child, so CEF goes
	// first while the window is alive and docked; the dock is the last registration
	// taken, so it is the first released; and the window is destroyed after every
	// registration is back.
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	REQUIRE(fixture.panel.open(parent_window).browser.browser_created);

	const std::size_t after_open = fixture.call_log.size();

	ShutdownSequence sequence;
	const ShutdownReport report = fixture.entry.shut_down(sequence);

	CHECK(report.completed);
	CHECK_FALSE(report.step_failed);
	CHECK(report.registrations_released == 3);
	REQUIRE(fixture.destruction_at_exit.has_value());
	CHECK(*fixture.destruction_at_exit == PanelWindowDestructionOutcome::destroyed);

	CHECK(calls_after(fixture.call_log, after_open) == CallLog{
		"unregister timer",
		"destroy queues",
		"shut down cef with the window alive and docked",
		"unregister dockable panel",
		"unregister menu item",
		"unregister action",
		"destroy window"
	});

	CHECK(fixture.registrar.outstanding.empty());
	CHECK_FALSE(fixture.window.exists());
	CHECK(fixture.window_platform.windows_destroyed.size() == 1);
}

TEST_CASE("teardown of a panel that was never opened has no window to destroy", "[entry][dockable_panel][teardown]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);

	ShutdownSequence sequence;
	const ShutdownReport report = fixture.entry.shut_down(sequence);

	CHECK(report.completed);
	REQUIRE(fixture.destruction_at_exit.has_value());
	CHECK(*fixture.destruction_at_exit == PanelWindowDestructionOutcome::no_window);
	CHECK(fixture.window_platform.windows_destroyed.empty());
}

TEST_CASE("the window is not destroyed out of order, whoever asks", "[entry][dockable_panel][teardown]")
{
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	REQUIRE(fixture.panel.open(parent_window).browser.browser_created);

	SECTION("while it is still in REAPER's docker")
	{
		CHECK(fixture.panel.destroy_window() == PanelWindowDestructionOutcome::still_docked);
	}

	SECTION("undocked, but with the browser still inside it")
	{
		fixture.registrations.release_all();

		REQUIRE_FALSE(fixture.panel.docked());
		CHECK(fixture.panel.destroy_window() == PanelWindowDestructionOutcome::browser_still_open);
	}

	CHECK(fixture.window.exists());
	CHECK(fixture.window_platform.windows_destroyed.empty());
}

TEST_CASE("a teardown that stopped at cef leaves the window docked and alive", "[entry][dockable_panel][teardown]")
{
	// CEF refusing to shut down means the browser may still be alive inside the window.
	// The exit step never runs, so nothing undocks or destroys the window on the
	// explicit path — destroying it here would be destroying a live browser's parent.
	// What is left is `LoadedExtension`'s declaration order, which puts the window
	// after the dock.
	PanelFixture fixture;
	REQUIRE(fixture.entry.load().loaded);
	REQUIRE(fixture.panel.open(parent_window).browser.browser_created);
	fixture.cef_shutdown_fails = true;

	ShutdownSequence sequence;
	const ShutdownReport report = fixture.entry.shut_down(sequence);

	CHECK(report.step_failed);
	CHECK(report.failed_step == ShutdownStep::shut_down_cef);
	CHECK_FALSE(fixture.destruction_at_exit.has_value());
	CHECK(fixture.window.exists());
	CHECK(fixture.panel.docked());
}

// ---------------------------------------------------------------------------
// What is written down
// ---------------------------------------------------------------------------

TEST_CASE("a panel that opened with its browser writes nothing, and every other opening writes one line", "[entry][dockable_panel][log]")
{
	RecordingLog recording;

	SECTION("opened with the browser")
	{
		PanelOpeningReport report;
		report.browser_attempted = true;
		report.browser.browser_created = true;

		write_panel_opening_lines(report, "", "", recording.log);

		CHECK(recording.lines.empty());
	}

	SECTION("opened again, with the browser already in it")
	{
		PanelOpeningReport report;
		report.browser_attempted = true;
		report.browser.already_open = true;

		write_panel_opening_lines(report, "", "", recording.log);

		CHECK(recording.lines.empty());
	}

	SECTION("not opened, naming the window's own refusal")
	{
		PanelOpeningReport report;
		report.refusal = PanelOpeningRefusal::window_not_created;
		report.window_outcome = PanelWindowCreationOutcome::platform_refused;

		write_panel_opening_lines(report, "", "", recording.log);

		REQUIRE(recording.lines.size() == 1);
		CHECK(recording.lines.front().severity == LogSeverity::warning);
		CHECK(recording.lines.front().text.find("the panel window could not be created") != std::string::npos);
		CHECK(recording.lines.front().text.find("dialog resource") != std::string::npos);
	}

	SECTION("no browser creator is information, carrying the installation's reason")
	{
		PanelOpeningReport report;
		report.browser_attempted = true;
		report.browser.refusal = PanelRefusalReason::no_browser_creator;

		write_panel_opening_lines(report, "the CEF subprocess helper was not found: '/opt/sesh-ai/sesh_ai_cef_helper' does not exist", "", recording.log);

		REQUIRE(recording.lines.size() == 1);
		CHECK(recording.lines.front().severity == LogSeverity::information);
		CHECK(recording.lines.front().text.find("/opt/sesh-ai/sesh_ai_cef_helper") != std::string::npos);
	}

	SECTION("a refused browser is a warning, carrying the creator's reason")
	{
		PanelOpeningReport report;
		report.browser_attempted = true;
		report.browser.refusal = PanelRefusalReason::browser_creation_refused;

		write_panel_opening_lines(report, "", "no cef subprocess executable path was supplied", recording.log);

		REQUIRE(recording.lines.size() == 1);
		CHECK(recording.lines.front().severity == LogSeverity::warning);
		CHECK(recording.lines.front().text.find("no cef subprocess executable path") != std::string::npos);
	}
}

TEST_CASE("every panel opening, refusal, and destruction outcome has its own line", "[entry][dockable_panel][log]")
{
	const auto all_distinct_and_present = [](const std::vector<std::string_view>& descriptions) {
		for (std::size_t index = 0; index < descriptions.size(); ++index) {
			REQUIRE_FALSE(descriptions[index].empty());

			for (std::size_t earlier = 0; earlier < index; ++earlier) {
				REQUIRE(descriptions[index] != descriptions[earlier]);
			}
		}
	};

	all_distinct_and_present({
		describe_panel_opening_refusal(PanelOpeningRefusal::none),
		describe_panel_opening_refusal(PanelOpeningRefusal::extension_not_loaded),
		describe_panel_opening_refusal(PanelOpeningRefusal::extension_shut_down),
		describe_panel_opening_refusal(PanelOpeningRefusal::window_not_created),
		describe_panel_opening_refusal(PanelOpeningRefusal::dock_refused),
	});

	all_distinct_and_present({
		describe_panel_refusal_reason(PanelRefusalReason::none),
		describe_panel_refusal_reason(PanelRefusalReason::extension_not_loaded),
		describe_panel_refusal_reason(PanelRefusalReason::extension_shut_down),
		describe_panel_refusal_reason(PanelRefusalReason::no_panel_window),
		describe_panel_refusal_reason(PanelRefusalReason::no_browser_creator),
		describe_panel_refusal_reason(PanelRefusalReason::browser_creation_refused),
	});

	all_distinct_and_present({
		describe_panel_window_destruction_outcome(PanelWindowDestructionOutcome::destroyed),
		describe_panel_window_destruction_outcome(PanelWindowDestructionOutcome::no_window),
		describe_panel_window_destruction_outcome(PanelWindowDestructionOutcome::still_docked),
		describe_panel_window_destruction_outcome(PanelWindowDestructionOutcome::browser_still_open),
	});
}
