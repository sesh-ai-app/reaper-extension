// A `BrowserHost` that exists before the browser does.
//
// ---------------------------------------------------------------------------
// The problem this solves, which is an ordering one
//
// `UiHost` holds its `BrowserHost` by reference from construction, and that is right:
// there is one browser per panel for the life of the extension, and a host that could
// be swapped at runtime would be a panel that could be pointed at a different browser
// mid-session.
//
// But the two do not come into existence at the same time. The UI Host is part of the
// object graph built at load, when REAPER calls the extension's entry point. The
// CEF-backed browser host needs the panel's native window handle, which does not exist
// until the producer opens the panel — `PluginEntry::open_panel` is what has it, and it
// may never be called at all in a session where the producer does not use Sesh.
//
// Three ways out, and this is the third:
//
//   - Construct the UI Host when the panel opens. That moves the whole graph behind an
//     `optional` and makes every component that holds a reference into it conditional,
//     for one object's sake.
//   - Give `UiHost` a settable host. That is the swap this component's reference
//     deliberately prevents, and it would make "is there a browser yet" a question
//     every one of `UiHost`'s entry points has to ask twice.
//   - A `BrowserHost` that is real from the start and empty until the browser arrives.
//     Which is this.
//
// ---------------------------------------------------------------------------
// Empty is a refusal, and `UiHost` already knows what to do with one
//
// Every operation on an unbound host answers the way CEF would if it had refused:
// `false` from the three that report, and nothing from the two that do not. `UiHost`
// reads that as `BridgeRefusalReason::browser_host_refused` and reports it, which is
// already the answer for "CEF said no" — so no new refusal reason is needed and no
// caller learns a new state.
//
// That matters for the case it is most likely to meet: a build with no CEF
// distribution, where `make_panel_browser_creator` installs nothing and this host is
// never bound. The panel reports `no_browser_creator`, every publish is refused, and
// the extension otherwise runs — the transport connects, tools execute, results go
// back. Nothing pretends a browser is there.
//
// ---------------------------------------------------------------------------
// Binding is once, and unbinding is not an operation
//
// `bind` refuses a second host rather than replacing the first, for the reason
// `UiHost::create_browser` refuses a second browser: whichever won would be decided by
// call order, and the loser would be a CEF browser nothing can reach but which is still
// running. There is no `unbind`, because the only thing that would legitimately want
// one is teardown, and teardown is `shut_down_cef`'s — after which the bound host
// refuses everything on its own account.

#ifndef SESH_AI_UI_DEFERRED_BROWSER_HOST_H
#define SESH_AI_UI_DEFERRED_BROWSER_HOST_H

#include <cstddef>
#include <string>

#include <ui/ui_host.h>

namespace sesh_ai::ui {

	class DeferredBrowserHost final : public BrowserHost {
	public:
		DeferredBrowserHost() = default;

		DeferredBrowserHost(const DeferredBrowserHost&) = delete;
		DeferredBrowserHost& operator=(const DeferredBrowserHost&) = delete;
		DeferredBrowserHost(DeferredBrowserHost&&) = delete;
		DeferredBrowserHost& operator=(DeferredBrowserHost&&) = delete;

		// Installs the real host. False for a null host and for a second bind — see the
		// file comment on why replacing is not the answer.
		//
		// The host is held by pointer and not owned: it outlives this object in the
		// extension, where both are members of the one object the plugin entry keeps,
		// destroyed in declaration order.
		bool bind(BrowserHost* browser_host)
		{
			if (browser_host == nullptr || browser_host_ != nullptr) {
				return false;
			}

			browser_host_ = browser_host;

			return true;
		}

		bool is_bound() const noexcept { return browser_host_ != nullptr; }

		// How many operations were refused for want of a browser. Exposed so a startup
		// log line can tell "the panel was never opened" from "the panel was opened and
		// CEF refused" — two conditions a producer experiences identically and which
		// call for different answers.
		std::size_t refused_operation_count() const noexcept { return refused_operation_count_; }

		bool create_browser(const std::string& local_asset_url) override
		{
			if (browser_host_ == nullptr) {
				++refused_operation_count_;
				return false;
			}

			return browser_host_->create_browser(local_asset_url);
		}

		bool navigate(const std::string& absolute_url) override
		{
			if (browser_host_ == nullptr) {
				++refused_operation_count_;
				return false;
			}

			return browser_host_->navigate(absolute_url);
		}

		bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json) override
		{
			if (browser_host_ == nullptr) {
				++refused_operation_count_;
				return false;
			}

			return browser_host_->post_message_to_javascript(message_name, serialized_json);
		}

		void close_browser() override
		{
			if (browser_host_ == nullptr) {
				++refused_operation_count_;
				return;
			}

			browser_host_->close_browser();
		}

		// Not counted as a refusal, and that is the one asymmetry here on purpose.
		// `UiHost::shut_down` calls this as its step in the teardown order, and a
		// teardown of a session where the panel was never opened is a teardown that
		// succeeded. Counting it would make every clean shutdown look like a refusal.
		void shut_down_cef() override
		{
			if (browser_host_ == nullptr) {
				return;
			}

			browser_host_->shut_down_cef();
		}

	private:
		BrowserHost* browser_host_ = nullptr;
		std::size_t refused_operation_count_ = 0;
	};

}

#endif
