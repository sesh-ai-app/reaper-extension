// What the extension writes down, and the one requirement that asks for it.
//
// Nothing in this codebase logs. Every component returns what it did and the caller
// decides — `RoutingOutcome`, `TickReport`, `TransportRequestOutcome`, `ShutdownReport`
// are all that same arrangement, and each of their headers says so in its own words.
// The caller they mean is this one. `transport/message_dispatcher.h` names it
// outright: requirement 5.8 asks for an unrecognised envelope type to be logged, the
// dispatcher names the type in its outcome rather than writing a line, and "the plugin
// entry writes the line".
//
// So this is where the lines are built and where the sink is held. The sink itself is
// installed in `plugin_entry.cpp`, which is the only place that knows what this build
// has to write to.
//
// ---------------------------------------------------------------------------
// Why the bindings live here too
//
// `bind_envelope_router` and `TickReportLog` are the two things that carry a returned
// outcome to the log, and they are the whole reason the log exists. Separating them
// would leave a header that can write a line nobody calls and a header that calls
// nothing. They are one concern: what the entry does with what came back.
//
// `bind_envelope_router` is also the answer to "where does the Message Dispatcher get
// wired in". `MainThreadDispatcher`'s router slot takes
// `std::function<void(InboundEnvelope)>` and the Message Dispatcher's `route` returns a
// `RoutingOutcome` — the binding is the adapter between those two shapes, and the
// place the log line is written is inside it, because that is the only point where a
// routed envelope and the log are both in scope.
//
// ---------------------------------------------------------------------------
// `TickReport` is taken by its own type; `RoutingOutcome` is not
//
// `TickReport` is declared next door in `entry/main_thread_dispatcher.h`, which costs
// one more header in this component's own directory. `RoutingOutcome` is declared
// behind `transport/message_dispatcher.h`, which pulls the envelope codec, the schema
// validator, the Context Builder, the Confirmation Coordinator, the Transport Handler,
// and the Stream Presenter — six headers across three other directories, to read two
// members off a plain struct. Templating is what every component here does for exactly
// that reason, so `log_routing_outcome` is a template over anything with
// `envelope_type` and `ignored()`.
//
// The cost of templating is that the binding is checked only where it is instantiated.
// `tests/entry/extension_log_test.cpp` instantiates it against the real
// `transport::RoutingOutcome`, so the check is a compiled one rather than a comment.
//
// ---------------------------------------------------------------------------
// The tick runs at 30 Hz, so most of what it reports must not be written down
//
// REAPER runs registered timers at roughly 30 Hz on the thread that draws REAPER's own
// UI. A line per tick is 1800 lines a minute and time the DAW is not repainting, so the
// decision about `TickReport` is mostly a decision about what *not* to write:
//
//   - **A tick that did work and nothing went wrong writes nothing.** There is no
//     reader for "routed 3 envelopes" thirty times a second.
//   - **`inbound_work_remains` is not a problem.** It is requirement 2.2's yield
//     working as designed — the budget was reached and the next tick continues. Logging
//     it would report the feature as a fault every time a turn arrives in a burst.
//   - **`reentrant_tick_skipped` is not a problem either.** Several REAPER API calls
//     pump the message loop, so a nested tick is expected inside a DAW; the inner call
//     declining is the designed behaviour.
//   - **A routing failure is a problem, and is written.** A handler that threw is an
//     envelope the producer will experience as the assistant not answering. That is
//     worth a line.
//
// Which leaves the flood in the one case that matters: a failure that repeats. A tool
// that throws on every call would write thirty lines a second, which buries the first
// one — the only line anybody wanted. So `TickReportLog` writes the first occurrence
// immediately and then suppresses, emitting a summary at most once per interval that
// names how many were suppressed. The first line is never delayed, because the first
// line is the diagnosis.

#ifndef SESH_AI_ENTRY_EXTENSION_LOG_H
#define SESH_AI_ENTRY_EXTENSION_LOG_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <entry/main_thread_dispatcher.h>

namespace sesh_ai::entry {

	// ---------------------------------------------------------------------------
	// The sink
	// ---------------------------------------------------------------------------

	// Two levels, because there are two kinds of line and no reader for a finer
	// distinction.
	enum class LogSeverity {
		// Something the extension declined to act on, where declining was correct.
		// Requirement 5.8's unrecognised envelope type is the whole of this category:
		// the server's protocol being ahead of this build is expected, not a fault, and
		// a producer must not be told their session is broken because of it.
		information,

		// Something that did not work. A dropped envelope is the producer's turn going
		// unanswered.
		warning
	};

	inline constexpr std::string_view describe(LogSeverity severity)
	{
		switch (severity) {
		case LogSeverity::information:
			return "information";
		case LogSeverity::warning:
			return "warning";
		}

		return "unknown severity";
	}

	// Where a line goes. A callable rather than an interface, for the reason
	// `TeardownSteps` is callables: there is exactly one operation, and what is behind
	// it changes as the build grows a real diagnostics surface.
	using LogSink = std::function<void(LogSeverity, std::string_view)>;

	// Holds the sink, and counts what went through it.
	//
	// The counts are not decoration. Without them, "the extension logged nothing" and
	// "the extension logged into a sink nobody installed" are the same observation, and
	// they call for different answers — the first is a routing bug and the second is a
	// composition one. They are also what lets the suite assert that a line was written
	// without asserting its exact wording everywhere.
	//
	// An unwired sink is not an error. During the build-out there is no diagnostics
	// surface, and a log that threw or asserted on that would be worse than one that
	// counts.
	class ExtensionLog {
	public:
		ExtensionLog() = default;

		explicit ExtensionLog(LogSink sink)
			: sink_{std::move(sink)}
		{
		}

		// Held by reference by the bindings, so neither copying nor moving it is
		// meaningful — two logs counting one stream of lines is not a state worth
		// having.
		ExtensionLog(const ExtensionLog&) = delete;
		ExtensionLog& operator=(const ExtensionLog&) = delete;
		ExtensionLog(ExtensionLog&&) = delete;
		ExtensionLog& operator=(ExtensionLog&&) = delete;

		// Installing a sink after construction, which is the order the composition
		// actually happens in: the entry exists before the thing that can display a
		// line does.
		void install_sink(LogSink sink)
		{
			sink_ = std::move(sink);
		}

		void write(LogSeverity severity, std::string_view line)
		{
			if (!sink_) {
				++lines_dropped_;
				return;
			}

			// A sink that throws must not take the tick with it. This is called from
			// inside the REAPER timer callback, and an exception unwinding from there
			// reaches REAPER through a C function pointer.
			try {
				sink_(severity, line);
				++lines_written_;
			} catch (...) {
				++lines_dropped_;
			}
		}

		bool has_sink() const noexcept { return static_cast<bool>(sink_); }

		std::size_t lines_written() const noexcept { return lines_written_; }
		std::size_t lines_dropped() const noexcept { return lines_dropped_; }

	private:
		LogSink sink_;
		std::size_t lines_written_ = 0;
		std::size_t lines_dropped_ = 0;
	};

	// ---------------------------------------------------------------------------
	// The lines
	// ---------------------------------------------------------------------------

	// Requirement 5.8's line.
	//
	// The type is named, because the type is the only thing that makes the line
	// actionable: a developer reading it needs to know whether the server is emitting
	// something this build predates or whether a handler's spelling drifted from the
	// dispatcher's. Phrased so it does not read as a fault — "ignored" rather than
	// "failed", and the expected cause stated — because the expected cause is a
	// producer who updated the server first, and nothing is wrong with their session.
	inline std::string ignored_envelope_type_log_line(std::string_view envelope_type)
	{
		std::string line{"ignored an envelope of type '"};
		line.append(envelope_type);
		line.append(
			"': this build has no route for it. Expected when the server's protocol is "
			"ahead of the extension."
		);

		return line;
	}

	inline std::string routing_failures_log_line(
		std::size_t failure_count,
		std::size_t envelopes_routed)
	{
		std::string line{"a handler threw while routing "};
		line.append(std::to_string(failure_count));
		line.append(failure_count == 1 ? " envelope" : " envelopes");
		line.append(" this tick (");
		line.append(std::to_string(envelopes_routed));
		line.append(" routed): the envelope was dropped, so whatever it asked for is "
			"unanswered.");

		return line;
	}

	inline std::string outbound_send_failures_log_line(std::size_t failure_count)
	{
		std::string line{"failed to hand "};
		line.append(std::to_string(failure_count));
		line.append(failure_count == 1 ? " envelope" : " envelopes");
		line.append(" to the network thread this tick: the response did not leave.");

		return line;
	}

	inline std::string suppressed_tick_failures_log_line(
		std::size_t suppressed_ticks,
		std::size_t suppressed_failures)
	{
		std::string line{"suppressed "};
		line.append(std::to_string(suppressed_ticks));
		line.append(suppressed_ticks == 1 ? " further tick" : " further ticks");
		line.append(" reporting failures (");
		line.append(std::to_string(suppressed_failures));
		line.append(" in total). The timer runs about thirty times a second, so a "
			"repeating failure is reported once and then summarised.");

		return line;
	}

	// ---------------------------------------------------------------------------
	// Binding the Message Dispatcher into the router slot
	// ---------------------------------------------------------------------------

	// Writes requirement 5.8's line, and nothing for an envelope that reached a
	// destination.
	//
	// True when a line was written. Only the unrouted case produces one: every other
	// route answers the server itself, and the components behind them report their own
	// outcomes to their own callers. A line per routed envelope would be the 30 Hz
	// flood described in the file comment, arriving on the main thread.
	template <typename RoutingOutcomeType>
	bool log_routing_outcome(const RoutingOutcomeType& outcome, ExtensionLog& log)
	{
		if (!outcome.ignored()) {
			return false;
		}

		log.write(
			LogSeverity::information,
			ignored_envelope_type_log_line(outcome.envelope_type)
		);

		return true;
	}

	// The Message Dispatcher, in the shape `MainThreadDispatcher` takes for its router
	// slot (requirement 2.4).
	//
	// Two shapes have to meet here. The tick hands an envelope over by value and wants
	// nothing back; `MessageDispatcher::route` takes a const reference and returns the
	// outcome requirement 5.8's line is built from. This is that adapter, and the log
	// write happens inside it because this is the only point at which a routed envelope
	// and the log are both in scope.
	//
	// Both arguments are captured by reference and must outlive the returned callable —
	// which in `plugin_entry.cpp` means both are members of the object that owns the
	// timer registration, destroyed after it.
	//
	// Templated on the dispatcher rather than naming `transport::MessageDispatcher`, so
	// this header does not acquire that header's include closure. See the file comment.
	template <typename InboundEnvelopeType, typename MessageDispatcherType>
	std::function<void(InboundEnvelopeType)> bind_envelope_router(
		MessageDispatcherType& message_dispatcher,
		ExtensionLog& log)
	{
		return [&message_dispatcher, &log](InboundEnvelopeType inbound_envelope) {
			log_routing_outcome(message_dispatcher.route(inbound_envelope), log);
		};
	}

	// ---------------------------------------------------------------------------
	// What the entry does with a TickReport
	// ---------------------------------------------------------------------------

	struct TickLogSettings {
		// How long a repeating failure stays suppressed before a summary. About ten
		// seconds at REAPER's timer rate, which is short enough that a developer
		// watching sees the failure is ongoing and long enough that it is not the
		// flood it is suppressing.
		std::size_t summary_interval_ticks = 300;
	};

	// Reads every tick's report and writes down the part worth writing.
	//
	// Main-thread only — it is called from inside the timer callback and holds plain
	// counters for that reason.
	class TickReportLog {
	public:
		explicit TickReportLog(ExtensionLog& log, TickLogSettings settings = TickLogSettings{})
			: log_{log}, settings_{settings}
		{
		}

		TickReportLog(const TickReportLog&) = delete;
		TickReportLog& operator=(const TickReportLog&) = delete;
		TickReportLog(TickReportLog&&) = delete;
		TickReportLog& operator=(TickReportLog&&) = delete;

		// True when this tick wrote a line.
		bool note(const TickReport& report)
		{
			++ticks_seen_;

			const std::size_t failures =
				report.inbound_routing_failures + report.outbound_send_failures;

			if (failures == 0) {
				// A clean tick ends the suppression window, so the next failure is
				// reported immediately rather than folded into a summary about an
				// earlier one. Any suppressed count is flushed first — otherwise a
				// burst that stops would be silently forgotten.
				return flush_suppressed_failures();
			}

			total_failures_ += failures;

			if (consecutive_failing_ticks_ > 0) {
				++consecutive_failing_ticks_;
				suppressed_failing_ticks_ += 1;
				suppressed_failures_ += failures;

				if (suppressed_failing_ticks_ >= settings_.summary_interval_ticks) {
					return flush_suppressed_failures();
				}

				return false;
			}

			// The first failing tick after a clean one. Written immediately: the first
			// line is the diagnosis, and delaying it to batch it with repeats is how the
			// useful line gets lost.
			consecutive_failing_ticks_ = 1;

			bool wrote_a_line = false;

			if (report.inbound_routing_failures > 0) {
				log_.write(
					LogSeverity::warning,
					routing_failures_log_line(
						report.inbound_routing_failures,
						report.inbound_envelopes_routed
					)
				);
				wrote_a_line = true;
			}

			if (report.outbound_send_failures > 0) {
				log_.write(
					LogSeverity::warning,
					outbound_send_failures_log_line(report.outbound_send_failures)
				);
				wrote_a_line = true;
			}

			return wrote_a_line;
		}

		std::size_t ticks_seen() const noexcept { return ticks_seen_; }
		std::size_t total_failures() const noexcept { return total_failures_; }

		// Failing ticks not individually reported, since the last summary. Exposed so
		// that "nothing was written" can be told apart from "nothing happened".
		std::size_t suppressed_failing_ticks() const noexcept
		{
			return suppressed_failing_ticks_;
		}

	private:
		bool flush_suppressed_failures()
		{
			consecutive_failing_ticks_ = 0;

			if (suppressed_failing_ticks_ == 0) {
				return false;
			}

			log_.write(
				LogSeverity::warning,
				suppressed_tick_failures_log_line(suppressed_failing_ticks_, suppressed_failures_)
			);

			suppressed_failing_ticks_ = 0;
			suppressed_failures_ = 0;

			return true;
		}

		ExtensionLog& log_;
		TickLogSettings settings_;

		std::size_t ticks_seen_ = 0;
		std::size_t total_failures_ = 0;

		// Zero while the last tick was clean, which is what makes the next failure a
		// first occurrence rather than a repeat.
		std::size_t consecutive_failing_ticks_ = 0;

		std::size_t suppressed_failing_ticks_ = 0;
		std::size_t suppressed_failures_ = 0;
	};

}

#endif
