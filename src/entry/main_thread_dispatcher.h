// The Main-Thread Dispatcher.
//
// One tick of this is the only legal window in the whole extension for a REAPER C
// API call (requirement 22.3). Each tick drains the inbound queue, routes what it
// drained, drains the outbound queue, and publishes UI state — in that order, so
// a result produced while routing this tick leaves on this tick rather than
// waiting for the next one (requirement 2.1).
//
// The budget is the point of the component. REAPER's timer runs on the same thread
// that draws REAPER's UI, so time spent here is time REAPER is not repainting.
// Forty queued tool calls drained unconditionally is a visibly frozen DAW, so the
// dispatcher stops at the budget and picks up where it left off next tick
// (requirement 2.2). See TickBudget for the numbers and why they differ between
// the two directions.
//
// Templated on the envelope types. Bounded moves from a queue to a handler do not
// depend on what an envelope contains, and parameterising it is what lets the
// suite exercise this logic with a stub envelope — no REAPER, no JSON dependency,
// no socket. The production instantiation lives in main_thread_dispatcher.cpp.

#ifndef SESH_AI_ENTRY_MAIN_THREAD_DISPATCHER_H
#define SESH_AI_ENTRY_MAIN_THREAD_DISPATCHER_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <utility>

#include <transport/queue_pair.h>

namespace sesh_ai::entry {

	// How much work one tick may do before yielding.
	//
	// REAPER runs registered timers at roughly 30 Hz, so a tick has on the order of
	// 30 ms before it is competing with REAPER's own frame. The budget is set to
	// keep the dispatcher's share a small fraction of that.
	//
	// The two directions get different numbers because they are not the same kind of
	// work. Routing an inbound envelope means executing a tool call against the
	// REAPER C API — enumerating tracks, instantiating FX, opening undo blocks —
	// which is the expensive, variable-cost side and the one that must be rationed.
	// Draining an outbound envelope means handing an already-built payload to the
	// network thread, which touches no REAPER API at all.
	//
	// Eight inbound per tick is about 240 tool calls a second, which no plausible
	// turn approaches, while a burst of forty clears in five ticks — fast enough
	// that a producer reads it as immediate, bounded enough that a slow tool cannot
	// take the frame. Thirty-two outbound per tick keeps results from pooling behind
	// an inbound burst when one tool call produces many.
	//
	// Overridable rather than hard-coded, because the right number depends on how
	// expensive tool calls turn out to be in a real session, and that is measured,
	// not reasoned about.
	struct TickBudget {
		std::size_t inbound_envelopes = 8;
		std::size_t outbound_envelopes = 32;
	};

	// What one tick did. Returned rather than logged so the caller — the plugin
	// entry, which knows how this build reports things — decides what to do with it.
	struct TickReport {
		std::size_t inbound_envelopes_routed = 0;
		std::size_t outbound_envelopes_sent = 0;

		// A handler that threw. Counted rather than propagated: one malformed
		// envelope must not take down the rest of the tick, and must never unwind
		// into REAPER.
		std::size_t inbound_routing_failures = 0;
		std::size_t outbound_send_failures = 0;

		// True when the budget was reached with items still queued, which is the
		// yield in requirement 2.2. The next tick continues from there.
		bool inbound_work_remains = false;
		bool outbound_work_remains = false;

		bool ui_state_published = false;

		// True when the tick was skipped because another tick was already in
		// progress on this thread. See tick().
		bool reentrant_tick_skipped = false;
	};

	template <typename InboundEnvelope, typename OutboundEnvelope>
	class MainThreadDispatcher {
	public:
		using Queues = transport::QueuePair<InboundEnvelope, OutboundEnvelope>;

		// The Message Dispatcher (task 14.1). Takes the envelope by value so
		// ownership moves out of the queue and the dispatcher keeps nothing.
		using EnvelopeRouter = std::function<void(InboundEnvelope)>;

		// The handoff to the network thread. Must not block: serializing and
		// validating an outbound payload belongs on the network thread
		// (requirement 22.2), and anything that waits here waits on REAPER's main
		// thread.
		using OutboundEnvelopeSender = std::function<void(OutboundEnvelope)>;

		// Publishing UI state to CEF happens here and nowhere else
		// (requirement 2.3). The state itself arrives with the UI Host (task 17.2) —
		// what this component fixes now is who publishes and how often.
		using UiStatePublisher = std::function<void()>;

		MainThreadDispatcher(
			Queues& queues,
			EnvelopeRouter router,
			OutboundEnvelopeSender sender,
			UiStatePublisher ui_state_publisher,
			TickBudget budget = TickBudget{}
		)
			: queues_{queues},
			router_{std::move(router)},
			sender_{std::move(sender)},
			ui_state_publisher_{std::move(ui_state_publisher)},
			budget_{budget}
		{
		}

		MainThreadDispatcher(const MainThreadDispatcher&) = delete;
		MainThreadDispatcher& operator=(const MainThreadDispatcher&) = delete;
		MainThreadDispatcher(MainThreadDispatcher&&) = delete;
		MainThreadDispatcher& operator=(MainThreadDispatcher&&) = delete;

		// Call from the REAPER timer callback and from nowhere else.
		TickReport tick()
		{
			TickReport report;

			// Re-entrancy is not hypothetical inside a DAW. Several REAPER API calls
			// pump the message loop, so a handler can cause REAPER to call the timer
			// again before the first call returned. Draining the same queues twice
			// through nested calls would process envelopes out of order and blow the
			// budget silently, so the inner call declines and the outer one finishes.
			if (tick_in_progress_) {
				report.reentrant_tick_skipped = true;
				return report;
			}

			tick_in_progress_ = true;

			route_inbound_envelopes(report);
			send_outbound_envelopes(report);
			publish_ui_state_if_requested(report);

			tick_in_progress_ = false;

			return report;
		}

		// Safe from any thread. Marks UI state as needing a publish; the publish
		// itself still happens on the main thread inside tick(), which is what
		// requirement 2.3 is about. Lets the network thread flag a connection state
		// change without touching CEF.
		void request_ui_state_publication()
		{
			ui_state_publication_requested_.store(true, std::memory_order_release);
		}

		bool ui_state_publication_pending() const
		{
			return ui_state_publication_requested_.load(std::memory_order_acquire);
		}

		const TickBudget& budget() const { return budget_; }

	private:
		void route_inbound_envelopes(TickReport& report)
		{
			if (!router_) {
				report.inbound_work_remains = !queues_.inbound().empty();
				return;
			}

			// Drained first, then handled with no lock held — a handler pushing a
			// result onto the outbound queue is the common case, and a handler
			// pushing back onto the inbound queue must not deadlock either.
			auto inbound_envelopes = queues_.inbound().drain_up_to(budget_.inbound_envelopes);

			for (auto& inbound_envelope : inbound_envelopes) {
				try {
					router_(std::move(inbound_envelope));
					++report.inbound_envelopes_routed;
				} catch (...) {
					++report.inbound_routing_failures;
				}
			}

			report.inbound_work_remains = !queues_.inbound().empty();
		}

		void send_outbound_envelopes(TickReport& report)
		{
			if (!sender_) {
				report.outbound_work_remains = !queues_.outbound().empty();
				return;
			}

			auto outbound_envelopes = queues_.outbound().drain_up_to(budget_.outbound_envelopes);

			for (auto& outbound_envelope : outbound_envelopes) {
				try {
					sender_(std::move(outbound_envelope));
					++report.outbound_envelopes_sent;
				} catch (...) {
					++report.outbound_send_failures;
				}
			}

			report.outbound_work_remains = !queues_.outbound().empty();
		}

		void publish_ui_state_if_requested(TickReport& report)
		{
			// Cleared before publishing, so a request arriving from another thread
			// during the publish is kept for the next tick rather than lost.
			const bool publication_requested =
				ui_state_publication_requested_.exchange(false, std::memory_order_acq_rel);

			if (!publication_requested || !ui_state_publisher_) {
				return;
			}

			// At most one publish per tick however many envelopes were routed. A
			// turn's worth of streaming deltas is one repaint, not forty.
			try {
				ui_state_publisher_();
				report.ui_state_published = true;
			} catch (...) {
			}
		}

		Queues& queues_;
		EnvelopeRouter router_;
		OutboundEnvelopeSender sender_;
		UiStatePublisher ui_state_publisher_;
		TickBudget budget_;

		// Only ever touched on the main thread, so a plain bool is correct.
		bool tick_in_progress_ = false;

		std::atomic<bool> ui_state_publication_requested_{false};
	};

}

#endif
