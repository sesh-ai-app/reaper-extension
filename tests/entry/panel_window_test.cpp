// The panel window's ownership and what creating it reports (task 25.1, requirements
// 1.1, 1.3).
//
// `entry/reaper_panel_window.cpp` is two platform calls and cannot run here, so what is
// pinned is everything around them: that the platform is called only when a window can
// honestly be created, that each refusal says which one it was, and that a window is
// destroyed exactly once whoever asks first. The platform below records what it was
// handed and returns addresses of its own, which is all `PanelWindow` ever does with a
// window.

#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/panel_window.h>

using sesh_ai::entry::PanelWindow;
using sesh_ai::entry::PanelWindowCreationOutcome;
using sesh_ai::entry::PanelWindowPlatform;
using sesh_ai::entry::describe_panel_window_creation_outcome;

namespace {

	// Stands in for `CreateDialogParam` and `DestroyWindow`. The windows it creates are
	// the addresses of its own slots, so a destroyed window can be told apart from a
	// created one without a real handle.
	struct RecordingWindowPlatform {
		bool refuses_creation = false;
		int window_slots[4] = {};
		int created_window_count = 0;
		std::vector<void*> parents_given;
		std::vector<void*> windows_destroyed;

		PanelWindowPlatform seams()
		{
			PanelWindowPlatform platform;

			platform.create_window = [this](void* parent_window) -> void* {
				parents_given.push_back(parent_window);

				if (refuses_creation) {
					return nullptr;
				}

				return &window_slots[created_window_count++];
			};

			platform.destroy_window = [this](void* window) {
				windows_destroyed.push_back(window);
			};

			return platform;
		}
	};

	int reaper_main_window = 0;

	void* const parent_window = &reaper_main_window;

}

TEST_CASE("creating the panel window hands the platform REAPER's main window and keeps what it returns", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;
	PanelWindow panel_window{platform.seams()};

	REQUIRE_FALSE(panel_window.exists());
	REQUIRE(panel_window.native_window() == nullptr);

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);

	REQUIRE(platform.parents_given == std::vector<void*>{parent_window});
	REQUIRE(panel_window.exists());
	REQUIRE(panel_window.native_window() == &platform.window_slots[0]);
}

TEST_CASE("a second create keeps the first window and does not reach the platform", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;
	PanelWindow panel_window{platform.seams()};

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);

	// Reported before the arguments are looked at, so a null parent on the second call
	// cannot turn an existing window into a failure.
	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::already_created);
	REQUIRE(panel_window.create(nullptr) == PanelWindowCreationOutcome::already_created);

	REQUIRE(platform.parents_given.size() == 1);
	REQUIRE(panel_window.native_window() == &platform.window_slots[0]);
}

TEST_CASE("a missing parent is refused before the platform could create an ownerless window", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;
	PanelWindow panel_window{platform.seams()};

	REQUIRE(panel_window.create(nullptr) == PanelWindowCreationOutcome::no_parent_window);

	REQUIRE(platform.parents_given.empty());
	REQUIRE_FALSE(panel_window.exists());
}

TEST_CASE("an unwired platform is its own refusal, whatever REAPER passed", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;

	SECTION("with no seams at all")
	{
		PanelWindow panel_window{PanelWindowPlatform{}};

		REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::no_window_platform);
		REQUIRE(panel_window.create(nullptr) == PanelWindowCreationOutcome::no_window_platform);
		REQUIRE_FALSE(panel_window.exists());
	}

	SECTION("with a creator but no destroyer, which would leak the window it made")
	{
		PanelWindowPlatform half_wired = platform.seams();
		half_wired.destroy_window = nullptr;

		PanelWindow panel_window{half_wired};

		REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::no_window_platform);
		REQUIRE(platform.parents_given.empty());
	}
}

TEST_CASE("a platform that returns no window leaves nothing held, and a later attempt can still succeed", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;
	platform.refuses_creation = true;

	PanelWindow panel_window{platform.seams()};

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::platform_refused);
	REQUIRE_FALSE(panel_window.exists());

	// Nothing was created, so there is nothing to destroy.
	panel_window.destroy();
	REQUIRE(platform.windows_destroyed.empty());

	platform.refuses_creation = false;

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);
	REQUIRE(platform.parents_given.size() == 2);
}

TEST_CASE("the panel window is destroyed exactly once, by whichever of destroy and the destructor runs first", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;

	SECTION("an explicit destroy, then the destructor")
	{
		{
			PanelWindow panel_window{platform.seams()};
			REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);

			panel_window.destroy();

			REQUIRE_FALSE(panel_window.exists());
			REQUIRE(panel_window.native_window() == nullptr);

			panel_window.destroy();
		}

		REQUIRE(platform.windows_destroyed == std::vector<void*>{&platform.window_slots[0]});
	}

	SECTION("the destructor alone, as the backstop")
	{
		{
			PanelWindow panel_window{platform.seams()};
			REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);
		}

		REQUIRE(platform.windows_destroyed == std::vector<void*>{&platform.window_slots[0]});
	}

	SECTION("a window that was never created is never destroyed")
	{
		{
			PanelWindow panel_window{platform.seams()};
		}

		REQUIRE(platform.windows_destroyed.empty());
	}
}

TEST_CASE("a destroyed panel window can be created again, and the new one is what is held", "[entry][panel_window]")
{
	RecordingWindowPlatform platform;
	PanelWindow panel_window{platform.seams()};

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);
	panel_window.destroy();

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);
	REQUIRE(panel_window.native_window() == &platform.window_slots[1]);
}

TEST_CASE("the window is released before the platform destroys it, so a re-entrant destroy finds nothing", "[entry][panel_window]")
{
	// `DestroyWindow` sends `WM_DESTROY` synchronously. A dialog procedure that answers
	// it by asking the owner to destroy the window again must not produce a second
	// `DestroyWindow` on a handle that is already going away.
	PanelWindow* owner = nullptr;
	std::vector<void*> windows_destroyed;
	int window_slot = 0;

	PanelWindowPlatform platform;
	platform.create_window = [&window_slot](void*) -> void* { return &window_slot; };
	platform.destroy_window = [&owner, &windows_destroyed](void* window) {
		windows_destroyed.push_back(window);
		REQUIRE_FALSE(owner->exists());
		owner->destroy();
	};

	PanelWindow panel_window{platform};
	owner = &panel_window;

	REQUIRE(panel_window.create(parent_window) == PanelWindowCreationOutcome::created);
	panel_window.destroy();

	REQUIRE(windows_destroyed == std::vector<void*>{&window_slot});
}

TEST_CASE("every panel window creation outcome has its own line", "[entry][panel_window]")
{
	const std::vector<PanelWindowCreationOutcome> outcomes{
		PanelWindowCreationOutcome::created,
		PanelWindowCreationOutcome::already_created,
		PanelWindowCreationOutcome::no_window_platform,
		PanelWindowCreationOutcome::no_parent_window,
		PanelWindowCreationOutcome::platform_refused,
	};

	std::vector<std::string_view> descriptions;

	for (const PanelWindowCreationOutcome outcome : outcomes) {
		const std::string_view description = describe_panel_window_creation_outcome(outcome);

		REQUIRE_FALSE(description.empty());

		for (const std::string_view earlier : descriptions) {
			REQUIRE(description != earlier);
		}

		descriptions.push_back(description);
	}
}
