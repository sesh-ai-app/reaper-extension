// `make_panel_browser_creator` in a build with no CEF distribution.
//
// One of two definitions; `entry/cef_panel_browser_creator.cpp` is the other, and
// `CMakeLists.txt` compiles exactly one. This one when no distribution was located,
// which is the same condition that already drops `src/ui/cef_browser_host.cpp`.
//
// The point of it existing at all is that the alternative is a link error. The
// composition root calls `make_panel_browser_creator` unconditionally, because
// "whether this build has a browser" is not a question the graph should be shaped
// around — every other component is wired the same way either way, and the transport
// connects, the tools execute, and results go back regardless of whether a panel can be
// drawn. So the function is always there, and in this build it returns nothing and says
// why.
//
// The honest outcome follows from there without anything pretending: the creator is
// empty, `PluginEntry::open_panel` reports `no_browser_creator`, the
// `DeferredBrowserHost` is never bound, and every publish is refused with
// `browser_host_refused`. That is four components each reporting what is true rather
// than one component faking a browser.
//
// Unlike its counterpart, this file is compiled by the project build on a machine with
// no CEF — it is the one that is reached. It names no CEF type.

#include <entry/panel_browser_creator.h>

#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

#include <string>

namespace sesh_ai::entry {

	PanelBrowserInstallation make_panel_browser_creator(
		ui::UiHost& ui_host,
		ui::DeferredBrowserHost& deferred_browser_host,
		const PanelBrowserSettings& settings)
	{
		// All three are the signature's, not this build's. Named and discarded rather
		// than left unnamed, so the two definitions read as the same function.
		(void)ui_host;
		(void)deferred_browser_host;
		(void)settings;

		PanelBrowserInstallation installation;

		// Information rather than a fault, in the phrasing `extension_log.h` reserves
		// for that distinction: a developer build without a CEF distribution is an
		// expected state, and nothing about the producer's session is broken by it.
		installation.unavailable_reason =
			"this build was configured without a CEF distribution, so the chat panel cannot be "
			"shown. Everything else — the transport, the tools, the project context — is "
			"unaffected. Configure with -DSESH_AI_DOWNLOAD_CEF=ON or -DSESH_AI_CEF_ROOT=<path>.";

		return installation;
	}

}
