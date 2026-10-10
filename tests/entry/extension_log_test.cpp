// What the entry writes down (requirements 5.8, 2.2, 2.4).
//
// Three things are defended here.
//
// **Requirement 5.8's line exists and names the type.** `message_dispatcher.h` reports
// an unrecognised envelope type in its outcome and says the plugin entry writes the
// line. Nothing proved the line was written until now — an outcome whose `ignored()`
// nobody reads is the requirement silently unmet, and the symptom is a producer whose
// server is a version ahead getting no explanation for a feature that does nothing.
//
// **The binding fits the real Message Dispatcher, and the real tick drives it.** The
// last case goes the whole way: a queue, a `MainThreadDispatcher` whose router slot is
// `bind_envelope_router`, and the real `transport::MessageDispatcher` over all seven of
// its seams. Templates are only checked where they are instantiated, so this is the
// check that the adapter actually fits the two shapes it sits between — and it is also
// the arrangement `plugin_entry.cpp` makes, so a change to either shape fails here
// rather than in a translation unit the suite cannot compile.
//
// **The 30 Hz flood is not traded for silence.** REAPER's timer runs about thirty times
// a second. A line per tick buries everything; a repeating failure reported once and
// never again hides that it is still happening. So the cases pin both ends: the first
// failing tick is written immediately, the repeats are suppressed, and the suppressed
// count is accounted for rather than dropped.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/extension_log.h>
#include <entry/main_thread_dispatcher.h>
#include <transport/message_dispatcher.h>
#include <transport/queue_pair.h>

using sesh_ai::entry::ExtensionLog;
using sesh_ai::entry::LogSeverity;
using sesh_ai::entry::MainThreadDispatcher;
using sesh_ai::entry::TickBudget;
using sesh_ai::entry::TickLogSettings;
using sesh_ai::entry::TickReport;
using sesh_ai::entry::TickReportLog;
using sesh_ai::entry::bind_envelope_router;
using sesh_ai::entry::describe;
using sesh_ai::entry::ignored_envelope_type_log_line;
using sesh_ai::entry::log_routing_outcome;
using sesh_ai::transport::ConfirmationRouter;
using sesh_ai::transport::MessageDispatcher;
using sesh_ai::transport::ProjectContextRouter;
using sesh_ai::transport::QueuePair;
using sesh_ai::transport::RoutingOutcome;
using sesh_ai::transport::ScriptDownloadRouter;
using sesh_ai::transport::StreamRouter;
using sesh_ai::transport::ToolCallResponse;
using sesh_ai::transport::ToolCallRouter;
using sesh_ai::transport::ToolResultResponse;
using sesh_ai::transport::ToolResultSink;
using sesh_ai::transport::TransportCommandRouter;
using sesh_ai::transport::confirmation_request_envelope_type;
using sesh_ai::transport::transport_request_envelope_type;

namespace {

	struct recorded_line {
		LogSeverity severity = LogSeverity::information;
		std::string text;
	};

	// Stands in for whatever this build eventually writes to. `plugin_entry.cpp`
	// installs one that writes to the process's standard error; what the cases need is
	// to read back what would have gone there.
	class recording_log {
	public:
		recording_log()
		{
			log.install_sink([this](LogSeverity severity, std::string_view line) {
				lines.push_back(recorded_line{severity, std::string{line}});
			});
		}

		bool mentions(std::string_view fragment) const
		{
			for (const recorded_line& line : lines) {
				if (line.text.find(fragment) != std::string::npos) {
					return true;
				}
			}

			return false;
		}

		ExtensionLog log;
		std::vector<recorded_line> lines;
	};

	// The envelope and payload types, stubbed. Routing does not read a payload, so a
	// JSON library is not needed to drive any of this — the same arrangement
	// `message_dispatcher.h` and `main_thread_dispatcher.h` are templated for.
	struct stub_envelope {
		std::string type;
		std::string request_id;
		int payload = 0;
	};

	struct stub_payload {
		std::string note;
	};

	// The seven destinations. Each records that it was reached, because "the line was
	// not written" and "the envelope went somewhere" are different answers and the
	// unrouted case has to be neither.
	struct destination_log {
		std::vector<std::string> reached;
	};

	class recording_tool_calls final : public ToolCallRouter<stub_envelope, stub_payload> {
	public:
		explicit recording_tool_calls(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		ToolCallResponse<stub_payload> execute_tool_call(
			std::string_view tool_name,
			const stub_envelope&) override
		{
			destinations_.reached.push_back("tool_executor:" + std::string{tool_name});

			ToolCallResponse<stub_payload> response;
			response.payload.note = "ran";
			response.result_schema_path = "mcp-tools/outputs/some-tool.schema.json";

			return response;
		}

	private:
		destination_log& destinations_;
	};

	class recording_tool_results final : public ToolResultSink<stub_payload> {
	public:
		explicit recording_tool_results(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		bool send_tool_result(const ToolResultResponse<stub_payload>& response) override
		{
			destinations_.reached.push_back("tool_result:" + response.type());

			return true;
		}

	private:
		destination_log& destinations_;
	};

	class recording_confirmations final : public ConfirmationRouter<stub_envelope> {
	public:
		explicit recording_confirmations(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		void handle_confirmation_request(const stub_envelope&) override
		{
			destinations_.reached.emplace_back("confirmation_request");
		}

		void handle_confirmation_resolved(const stub_envelope&) override
		{
			destinations_.reached.emplace_back("confirmation_resolved");
		}

	private:
		destination_log& destinations_;
	};

	class recording_project_context final : public ProjectContextRouter {
	public:
		explicit recording_project_context(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		void answer_project_context_request(std::string_view) override
		{
			destinations_.reached.emplace_back("context_builder");
		}

	private:
		destination_log& destinations_;
	};

	class recording_transport_commands final : public TransportCommandRouter<stub_envelope> {
	public:
		explicit recording_transport_commands(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		void execute_transport_command(const stub_envelope&) override
		{
			destinations_.reached.emplace_back("transport_handler");
		}

	private:
		destination_log& destinations_;
	};

	class recording_streams final : public StreamRouter<stub_envelope> {
	public:
		explicit recording_streams(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		void present_stream_envelope(const stub_envelope&) override
		{
			destinations_.reached.emplace_back("stream_presenter");
		}

	private:
		destination_log& destinations_;
	};

	class recording_script_downloads final : public ScriptDownloadRouter<stub_envelope> {
	public:
		explicit recording_script_downloads(destination_log& destinations)
			: destinations_{destinations}
		{
		}

		void present_script_download(const stub_envelope&) override
		{
			destinations_.reached.emplace_back("script_download");
		}

	private:
		destination_log& destinations_;
	};

	// The real Message Dispatcher over all seven, assembled once. This is the shape
	// `plugin_entry.cpp` builds, with recorders where the real components go.
	struct wired_message_dispatcher {
		explicit wired_message_dispatcher(destination_log& destinations)
			: tool_calls{destinations},
			tool_results{destinations},
			confirmations{destinations},
			project_context{destinations},
			transport_commands{destinations},
			streams{destinations},
			script_downloads{destinations},
			dispatcher{
				tool_calls,
				tool_results,
				confirmations,
				project_context,
				transport_commands,
				streams,
				script_downloads
			}
		{
		}

		recording_tool_calls tool_calls;
		recording_tool_results tool_results;
		recording_confirmations confirmations;
		recording_project_context project_context;
		recording_transport_commands transport_commands;
		recording_streams streams;
		recording_script_downloads script_downloads;

		MessageDispatcher<stub_envelope, stub_payload> dispatcher;
	};

	TickReport clean_tick(std::size_t routed = 3)
	{
		TickReport report;
		report.inbound_envelopes_routed = routed;

		return report;
	}

	TickReport tick_with_inbound_failures(std::size_t failures, std::size_t routed = 0)
	{
		TickReport report;
		report.inbound_envelopes_routed = routed;
		report.inbound_routing_failures = failures;

		return report;
	}

}

// ---------------------------------------------------------------------------
// The sink
// ---------------------------------------------------------------------------

TEST_CASE("the log counts what it could not write", "[entry][log]")
{
	// "The extension logged nothing" and "the extension logged into a sink nobody
	// installed" are the same observation without this, and they are a routing bug and
	// a composition bug respectively.
	SECTION("with no sink installed, lines are counted as dropped")
	{
		ExtensionLog log;

		CHECK_FALSE(log.has_sink());

		log.write(LogSeverity::information, "something happened");

		CHECK(log.lines_written() == 0);
		CHECK(log.lines_dropped() == 1);
	}

	SECTION("a sink that throws does not take the tick with it")
	{
		// Called from inside the REAPER timer callback. An exception unwinding from
		// there crosses a C function pointer, which is undefined behaviour and in
		// practice a REAPER crash.
		ExtensionLog log{[](LogSeverity, std::string_view) {
			throw std::runtime_error{"the diagnostics surface went away"};
		}};

		CHECK_NOTHROW(log.write(LogSeverity::warning, "a handler threw"));
		CHECK(log.lines_written() == 0);
		CHECK(log.lines_dropped() == 1);
	}

	SECTION("severities are named, not numbered")
	{
		CHECK(describe(LogSeverity::information) == "information");
		CHECK(describe(LogSeverity::warning) == "warning");
	}
}

// ---------------------------------------------------------------------------
// Requirement 5.8
// ---------------------------------------------------------------------------

TEST_CASE("an unrecognised envelope type is logged, and named", "[entry][log][routing]")
{
	// The type is the only thing that makes the line actionable: it says whether the
	// server is emitting something this build predates, or whether a handler's spelling
	// drifted from the dispatcher's.
	const std::string line = ignored_envelope_type_log_line("state:transcription_started");

	CHECK(line.find("state:transcription_started") != std::string::npos);

	// And it must not read as a fault. The expected cause is a producer who updated
	// the server first, and nothing is wrong with their session.
	CHECK(line.find("ignored") != std::string::npos);
	CHECK(line.find("ahead of the extension") != std::string::npos);
}

TEST_CASE("only the unrouted envelope produces a line", "[entry][log][routing]")
{
	// Instantiated against the real `transport::RoutingOutcome`, which is the check
	// that this reads the members the dispatcher actually fills in — a template is
	// only checked where it is instantiated.
	recording_log recording;

	SECTION("an ignored envelope is written at information level")
	{
		RoutingOutcome outcome;
		outcome.envelope_type = "state:something_new";
		outcome.route = sesh_ai::transport::EnvelopeRoute::unknown;

		REQUIRE(outcome.ignored());
		CHECK(log_routing_outcome(outcome, recording.log));

		REQUIRE(recording.lines.size() == 1);
		CHECK(recording.lines.front().severity == LogSeverity::information);
		CHECK(recording.mentions("state:something_new"));
	}

	SECTION("a routed envelope writes nothing")
	{
		// A line per routed envelope is the 30 Hz flood, arriving on the thread that
		// draws REAPER's UI. Every other route answers the server itself.
		RoutingOutcome outcome;
		outcome.envelope_type = "request:set_track_state";
		outcome.route = sesh_ai::transport::EnvelopeRoute::tool_executor;
		outcome.routed = true;

		CHECK_FALSE(log_routing_outcome(outcome, recording.log));
		CHECK(recording.lines.empty());
	}
}

// ---------------------------------------------------------------------------
// The TickReport
// ---------------------------------------------------------------------------

TEST_CASE("a tick that worked writes nothing", "[entry][log][tick]")
{
	recording_log recording;
	TickReportLog tick_log{recording.log};

	for (int tick = 0; tick < 100; ++tick) {
		CHECK_FALSE(tick_log.note(clean_tick()));
	}

	CHECK(recording.lines.empty());
	CHECK(tick_log.ticks_seen() == 100);
}

TEST_CASE("the yield and the re-entrant skip are not faults", "[entry][log][tick]")
{
	// `inbound_work_remains` is requirement 2.2 working: the budget was reached and the
	// next tick continues. `reentrant_tick_skipped` is the designed answer to a REAPER
	// call that pumped the message loop. Reporting either as a problem would mean a
	// burst of tool calls, or any ordinary REAPER call, looked like a fault.
	recording_log recording;
	TickReportLog tick_log{recording.log};

	TickReport yielded;
	yielded.inbound_envelopes_routed = 8;
	yielded.inbound_work_remains = true;
	yielded.outbound_work_remains = true;

	TickReport skipped;
	skipped.reentrant_tick_skipped = true;

	CHECK_FALSE(tick_log.note(yielded));
	CHECK_FALSE(tick_log.note(skipped));
	CHECK(recording.lines.empty());
}

TEST_CASE("a dropped envelope is written the first time it happens", "[entry][log][tick]")
{
	// A handler that threw is an envelope the producer experiences as the assistant not
	// answering. The first line is the diagnosis, so it is never batched or delayed.
	recording_log recording;
	TickReportLog tick_log{recording.log};

	CHECK(tick_log.note(tick_with_inbound_failures(1, 2)));

	REQUIRE(recording.lines.size() == 1);
	CHECK(recording.lines.front().severity == LogSeverity::warning);
	CHECK(recording.mentions("a handler threw while routing 1 envelope"));
	CHECK(recording.mentions("unanswered"));
	CHECK(tick_log.total_failures() == 1);
}

TEST_CASE("a failure that cannot leave is written as its own line", "[entry][log][tick]")
{
	recording_log recording;
	TickReportLog tick_log{recording.log};

	TickReport report;
	report.outbound_send_failures = 2;

	CHECK(tick_log.note(report));

	CHECK(recording.mentions("failed to hand 2 envelopes to the network thread"));
	CHECK(recording.mentions("did not leave"));
}

TEST_CASE("a repeating failure is reported once and then summarised", "[entry][log][tick]")
{
	// The case the suppression exists for. A tool that throws on every call writes
	// thirty lines a second, which buries the first one — the only line anybody wanted.
	recording_log recording;
	TickReportLog tick_log{recording.log, TickLogSettings{4}};

	CHECK(tick_log.note(tick_with_inbound_failures(1)));
	REQUIRE(recording.lines.size() == 1);

	// The next three are suppressed, not forgotten.
	CHECK_FALSE(tick_log.note(tick_with_inbound_failures(1)));
	CHECK_FALSE(tick_log.note(tick_with_inbound_failures(1)));
	CHECK_FALSE(tick_log.note(tick_with_inbound_failures(1)));
	CHECK(recording.lines.size() == 1);
	CHECK(tick_log.suppressed_failing_ticks() == 3);

	// The fourth reaches the interval and the summary names what was held back, so a
	// developer watching sees the failure is ongoing rather than resolved.
	CHECK(tick_log.note(tick_with_inbound_failures(1)));

	REQUIRE(recording.lines.size() == 2);
	CHECK(recording.lines.back().severity == LogSeverity::warning);
	CHECK(recording.mentions("suppressed 4 further ticks"));
	CHECK(tick_log.total_failures() == 5);
	CHECK(tick_log.suppressed_failing_ticks() == 0);
}

TEST_CASE("a burst that stops is accounted for, not forgotten", "[entry][log][tick]")
{
	// Without this, a failure that repeats a few times and then clears leaves the
	// suppressed count unwritten — and the log says one envelope was dropped when
	// several were.
	recording_log recording;
	TickReportLog tick_log{recording.log, TickLogSettings{1000}};

	REQUIRE(tick_log.note(tick_with_inbound_failures(1)));
	CHECK_FALSE(tick_log.note(tick_with_inbound_failures(1)));
	CHECK_FALSE(tick_log.note(tick_with_inbound_failures(1)));

	CHECK(tick_log.note(clean_tick()));

	REQUIRE(recording.lines.size() == 2);
	CHECK(recording.mentions("suppressed 2 further ticks"));

	// And the window is closed, so the next failure is a first occurrence again rather
	// than a repeat of one from before the session went quiet.
	CHECK(tick_log.note(tick_with_inbound_failures(1)));
	CHECK(recording.lines.size() == 3);
	CHECK(recording.lines.back().text.find("a handler threw") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The binding, driven by the real tick through the real dispatcher
// ---------------------------------------------------------------------------

TEST_CASE("the timer tick routes through the Message Dispatcher", "[entry][log][routing]")
{
	// The whole path, with nothing stood in for between the queue and the destinations:
	// a `MainThreadDispatcher` whose router slot is `bind_envelope_router`, and the real
	// `transport::MessageDispatcher` over all seven of its seams. This is the
	// arrangement `plugin_entry.cpp` makes, so a change to either shape fails here
	// rather than in a translation unit the suite cannot compile.
	destination_log destinations;
	wired_message_dispatcher wiring{destinations};
	recording_log recording;

	QueuePair<stub_envelope, int> queues;

	MainThreadDispatcher<stub_envelope, int> main_thread_dispatcher{
		queues,
		bind_envelope_router<stub_envelope>(wiring.dispatcher, recording.log),
		[](int) {},
		[] {}
	};

	SECTION("a recognised envelope reaches its destination and writes nothing")
	{
		queues.inbound().push(stub_envelope{std::string{transport_request_envelope_type}, "r-1", 0});
		queues.inbound().push(stub_envelope{std::string{confirmation_request_envelope_type}, "r-2", 0});

		const TickReport report = main_thread_dispatcher.tick();

		CHECK(report.inbound_envelopes_routed == 2);
		CHECK(report.inbound_routing_failures == 0);
		CHECK(destinations.reached == std::vector<std::string>{
			"transport_handler",
			"confirmation_request"
		});
		CHECK(recording.lines.empty());
	}

	SECTION("an envelope with no route in this build is logged and goes nowhere")
	{
		// Requirement 5.8, end to end. Not an error response, not a refusal, and not
		// silence: nothing is called and one line is written naming the type.
		queues.inbound().push(stub_envelope{"state:transcription_started", "r-3", 0});

		const TickReport report = main_thread_dispatcher.tick();

		CHECK(report.inbound_envelopes_routed == 1);
		CHECK(report.inbound_routing_failures == 0);
		CHECK(destinations.reached.empty());

		REQUIRE(recording.lines.size() == 1);
		CHECK(recording.lines.front().severity == LogSeverity::information);
		CHECK(recording.mentions("state:transcription_started"));
	}

	SECTION("a tool call still reaches the executor and is answered")
	{
		// The route that produces a response, included so the binding is not only shown
		// on the paths that return nothing.
		queues.inbound().push(stub_envelope{"request:set_track_state", "r-4", 0});

		REQUIRE(main_thread_dispatcher.tick().inbound_envelopes_routed == 1);

		CHECK(destinations.reached == std::vector<std::string>{
			"tool_executor:set_track_state",
			"tool_result:response:set_track_state"
		});
		CHECK(recording.lines.empty());
	}
}
