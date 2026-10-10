// The Main-Thread Dispatcher.
//
// Everything here runs with no REAPER, no socket, and no JSON dependency, which is
// the point of the seam: the dispatcher is templated on the envelope types and its
// collaborators are callables, so the suite substitutes stubs for all of them and
// exercises the real draining and bounding logic.
//
// Requirement 2.2 is the one worth reading the assertions for. "Yield after the
// budget and continue on the next tick" is two claims — that a tick stops, and that
// nothing is lost when it does — and they are tested separately.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <entry/main_thread_dispatcher.h>
#include <transport/queue_pair.h>

using sesh_ai::entry::MainThreadDispatcher;
using sesh_ai::entry::TickBudget;
using sesh_ai::entry::TickReport;

namespace {

	// The envelope shapes the dispatcher moves around. Deliberately not the
	// production ones — what is under test does not depend on what an envelope
	// contains, and using a stub keeps the suite free of the JSON dependency.
	struct StubInboundEnvelope {
		std::string type;
		int identifier = 0;
	};

	struct StubOutboundEnvelope {
		std::string type;
		int identifier = 0;
	};

	using StubQueues = sesh_ai::transport::QueuePair<StubInboundEnvelope, StubOutboundEnvelope>;
	using StubDispatcher = MainThreadDispatcher<StubInboundEnvelope, StubOutboundEnvelope>;

	void queue_inbound_envelopes(StubQueues& queues, int envelope_count)
	{
		for (int envelope_identifier = 0; envelope_identifier < envelope_count; ++envelope_identifier) {
			queues.inbound().push(StubInboundEnvelope{"request:list_tracks", envelope_identifier});
		}
	}

	void queue_outbound_envelopes(StubQueues& queues, int envelope_count)
	{
		for (int envelope_identifier = 0; envelope_identifier < envelope_count; ++envelope_identifier) {
			queues.outbound().push(StubOutboundEnvelope{"response:list_tracks", envelope_identifier});
		}
	}

}

TEST_CASE("a tick drains both directions", "[dispatcher]")
{
	// Requirement 2.1: both queues, every tick.
	StubQueues queues;
	std::vector<int> routed_identifiers;
	std::vector<int> sent_identifiers;

	StubDispatcher dispatcher{
		queues,
		[&routed_identifiers](StubInboundEnvelope envelope) {
			routed_identifiers.push_back(envelope.identifier);
		},
		[&sent_identifiers](StubOutboundEnvelope envelope) {
			sent_identifiers.push_back(envelope.identifier);
		},
		[] {}
	};

	queue_inbound_envelopes(queues, 3);
	queue_outbound_envelopes(queues, 2);

	const TickReport report = dispatcher.tick();

	REQUIRE(report.inbound_envelopes_routed == 3);
	REQUIRE(report.outbound_envelopes_sent == 2);
	REQUIRE(routed_identifiers == std::vector<int>{0, 1, 2});
	REQUIRE(sent_identifiers == std::vector<int>{0, 1});
	REQUIRE_FALSE(report.inbound_work_remains);
	REQUIRE_FALSE(report.outbound_work_remains);
}

TEST_CASE("each inbound envelope reaches the router with its type intact", "[dispatcher]")
{
	// Requirement 2.4: the dispatcher does not interpret envelopes, it hands each one
	// to the Message Dispatcher for type-based routing.
	StubQueues queues;
	std::vector<std::string> routed_types;

	StubDispatcher dispatcher{
		queues,
		[&routed_types](StubInboundEnvelope envelope) { routed_types.push_back(envelope.type); },
		[](StubOutboundEnvelope) {},
		[] {}
	};

	queues.inbound().push(StubInboundEnvelope{"request:project_context", 1});
	queues.inbound().push(StubInboundEnvelope{"confirm:request", 2});
	queues.inbound().push(StubInboundEnvelope{"stream:delta", 3});

	dispatcher.tick();

	REQUIRE(routed_types == std::vector<std::string>{"request:project_context", "confirm:request", "stream:delta"});
}

TEST_CASE("a tick stops at the inbound budget and reports that work remains", "[dispatcher][budget]")
{
	// Requirement 2.2, first half: forty queued tool calls must not be drained in one
	// tick, because that tick is REAPER's UI thread.
	StubQueues queues;
	int routed_envelope_count = 0;

	TickBudget budget;
	budget.inbound_envelopes = 8;

	StubDispatcher dispatcher{
		queues,
		[&routed_envelope_count](StubInboundEnvelope) { ++routed_envelope_count; },
		[](StubOutboundEnvelope) {},
		[] {},
		budget
	};

	queue_inbound_envelopes(queues, 40);

	const TickReport report = dispatcher.tick();

	REQUIRE(routed_envelope_count == 8);
	REQUIRE(report.inbound_envelopes_routed == 8);
	REQUIRE(report.inbound_work_remains);
	REQUIRE(queues.inbound().size() == 32);
}

TEST_CASE("draining continues on the following ticks until the queue is empty", "[dispatcher][budget]")
{
	// Requirement 2.2, second half: yielding must not drop anything, and the order
	// across ticks is still the order the envelopes arrived in.
	StubQueues queues;
	std::vector<int> routed_identifiers;

	TickBudget budget;
	budget.inbound_envelopes = 8;

	StubDispatcher dispatcher{
		queues,
		[&routed_identifiers](StubInboundEnvelope envelope) {
			routed_identifiers.push_back(envelope.identifier);
		},
		[](StubOutboundEnvelope) {},
		[] {},
		budget
	};

	queue_inbound_envelopes(queues, 40);

	int tick_count = 0;

	while (true) {
		const TickReport report = dispatcher.tick();
		++tick_count;

		if (!report.inbound_work_remains) {
			break;
		}

		REQUIRE(tick_count < 100);
	}

	REQUIRE(tick_count == 5);
	REQUIRE(routed_identifiers.size() == 40);

	for (int envelope_identifier = 0; envelope_identifier < 40; ++envelope_identifier) {
		REQUIRE(routed_identifiers[static_cast<std::size_t>(envelope_identifier)] == envelope_identifier);
	}
}

TEST_CASE("the outbound budget is enforced separately from the inbound one", "[dispatcher][budget]")
{
	// The two directions are not the same work: inbound means a tool call against
	// REAPER's API, outbound means handing a built payload to the network thread. The
	// budgets reflect that and are checked independently.
	StubQueues queues;
	int routed_envelope_count = 0;
	int sent_envelope_count = 0;

	TickBudget budget;
	budget.inbound_envelopes = 2;
	budget.outbound_envelopes = 5;

	StubDispatcher dispatcher{
		queues,
		[&routed_envelope_count](StubInboundEnvelope) { ++routed_envelope_count; },
		[&sent_envelope_count](StubOutboundEnvelope) { ++sent_envelope_count; },
		[] {},
		budget
	};

	queue_inbound_envelopes(queues, 10);
	queue_outbound_envelopes(queues, 10);

	const TickReport report = dispatcher.tick();

	REQUIRE(routed_envelope_count == 2);
	REQUIRE(sent_envelope_count == 5);
	REQUIRE(report.inbound_work_remains);
	REQUIRE(report.outbound_work_remains);
	REQUIRE(queues.inbound().size() == 8);
	REQUIRE(queues.outbound().size() == 5);
}

TEST_CASE("the default budget rations inbound work more tightly than outbound", "[dispatcher][budget]")
{
	// Not a tautology: the asymmetry is the design decision, and a later edit that
	// quietly equalises the two numbers should have to change this test and say why.
	const TickBudget default_budget;

	REQUIRE(default_budget.inbound_envelopes == 8);
	REQUIRE(default_budget.outbound_envelopes == 32);
	REQUIRE(default_budget.inbound_envelopes < default_budget.outbound_envelopes);
}

TEST_CASE("results produced while routing leave on the same tick", "[dispatcher]")
{
	// Inbound is drained before outbound on purpose. A tool result produced while
	// routing goes out this tick rather than waiting 33 ms for the next one, which
	// across a turn of tool calls is the difference between a responsive assistant
	// and a sluggish one.
	StubQueues queues;
	std::vector<int> sent_identifiers;

	StubDispatcher dispatcher{
		queues,
		[&queues](StubInboundEnvelope envelope) {
			queues.outbound().push(StubOutboundEnvelope{"response:list_tracks", envelope.identifier});
		},
		[&sent_identifiers](StubOutboundEnvelope envelope) {
			sent_identifiers.push_back(envelope.identifier);
		},
		[] {}
	};

	queue_inbound_envelopes(queues, 3);

	const TickReport report = dispatcher.tick();

	REQUIRE(report.inbound_envelopes_routed == 3);
	REQUIRE(report.outbound_envelopes_sent == 3);
	REQUIRE(sent_identifiers == std::vector<int>{0, 1, 2});
}

TEST_CASE("UI state is published at most once per tick, and only when asked", "[dispatcher][ui]")
{
	// Requirement 2.3. A turn's worth of streaming deltas is one repaint, not one per
	// envelope, and a tick with nothing to say does not repaint at all.
	StubQueues queues;
	int ui_state_publication_count = 0;

	StubDispatcher dispatcher{
		queues,
		[&dispatcher](StubInboundEnvelope) { dispatcher.request_ui_state_publication(); },
		[](StubOutboundEnvelope) {},
		[&ui_state_publication_count] { ++ui_state_publication_count; }
	};

	SECTION("a tick with no request publishes nothing")
	{
		const TickReport report = dispatcher.tick();

		REQUIRE(ui_state_publication_count == 0);
		REQUIRE_FALSE(report.ui_state_published);
	}

	SECTION("many requests in one tick coalesce into one publish")
	{
		queue_inbound_envelopes(queues, 5);

		const TickReport report = dispatcher.tick();

		REQUIRE(report.inbound_envelopes_routed == 5);
		REQUIRE(ui_state_publication_count == 1);
		REQUIRE(report.ui_state_published);
	}

	SECTION("the request does not persist into the next tick")
	{
		queue_inbound_envelopes(queues, 1);

		dispatcher.tick();
		const TickReport second_report = dispatcher.tick();

		REQUIRE(ui_state_publication_count == 1);
		REQUIRE_FALSE(second_report.ui_state_published);
	}
}

TEST_CASE("a publication request from another thread is honoured on the main thread", "[dispatcher][ui][threads]")
{
	// The network thread needs to say "connection state changed" without touching
	// CEF. Flagging is safe from anywhere; publishing still happens inside the tick.
	StubQueues queues;
	int ui_state_publication_count = 0;

	StubDispatcher dispatcher{
		queues,
		[](StubInboundEnvelope) {},
		[](StubOutboundEnvelope) {},
		[&ui_state_publication_count] { ++ui_state_publication_count; }
	};

	std::thread network_thread{[&dispatcher] { dispatcher.request_ui_state_publication(); }};
	network_thread.join();

	REQUIRE(dispatcher.ui_state_publication_pending());

	const TickReport report = dispatcher.tick();

	REQUIRE(report.ui_state_published);
	REQUIRE(ui_state_publication_count == 1);
	REQUIRE_FALSE(dispatcher.ui_state_publication_pending());
}

TEST_CASE("a throwing handler is contained and the rest of the tick still runs", "[dispatcher][errors]")
{
	// The tick is invoked through a C function pointer. An exception leaving it is
	// undefined behaviour and in practice a REAPER crash, which a producer
	// experiences as losing their session. One bad envelope must cost one envelope.
	StubQueues queues;
	std::vector<int> routed_identifiers;
	int sent_envelope_count = 0;

	StubDispatcher dispatcher{
		queues,
		[&routed_identifiers](StubInboundEnvelope envelope) {
			if (envelope.identifier == 1) {
				throw std::runtime_error{"handler failed"};
			}

			routed_identifiers.push_back(envelope.identifier);
		},
		[&sent_envelope_count](StubOutboundEnvelope envelope) {
			if (envelope.identifier == 0) {
				throw std::runtime_error{"send failed"};
			}

			++sent_envelope_count;
		},
		[] {}
	};

	queue_inbound_envelopes(queues, 3);
	queue_outbound_envelopes(queues, 2);

	TickReport report;
	REQUIRE_NOTHROW(report = dispatcher.tick());

	REQUIRE(report.inbound_envelopes_routed == 2);
	REQUIRE(report.inbound_routing_failures == 1);
	REQUIRE(routed_identifiers == std::vector<int>{0, 2});

	REQUIRE(report.outbound_envelopes_sent == 1);
	REQUIRE(report.outbound_send_failures == 1);
	REQUIRE(sent_envelope_count == 1);
}

TEST_CASE("a nested tick declines rather than draining the queues twice", "[dispatcher]")
{
	// Several REAPER API calls pump the message loop, so a handler can cause REAPER
	// to fire the timer again before the first call returned. The inner call has to
	// decline: two interleaved drains would process envelopes out of order and spend
	// twice the budget without either call knowing.
	StubQueues queues;
	std::vector<int> routed_identifiers;
	TickReport nested_report;
	bool nested_tick_attempted = false;

	StubDispatcher* dispatcher_pointer = nullptr;

	StubDispatcher dispatcher{
		queues,
		[&](StubInboundEnvelope envelope) {
			routed_identifiers.push_back(envelope.identifier);

			if (!nested_tick_attempted) {
				nested_tick_attempted = true;
				nested_report = dispatcher_pointer->tick();
			}
		},
		[](StubOutboundEnvelope) {},
		[] {}
	};

	dispatcher_pointer = &dispatcher;

	queue_inbound_envelopes(queues, 3);

	const TickReport report = dispatcher.tick();

	REQUIRE(nested_tick_attempted);
	REQUIRE(nested_report.reentrant_tick_skipped);
	REQUIRE(nested_report.inbound_envelopes_routed == 0);

	// The outer tick finished its own batch, in order, undisturbed.
	REQUIRE_FALSE(report.reentrant_tick_skipped);
	REQUIRE(report.inbound_envelopes_routed == 3);
	REQUIRE(routed_identifiers == std::vector<int>{0, 1, 2});
}

TEST_CASE("an unwired dispatcher leaves the queues alone rather than dropping them", "[dispatcher]")
{
	// A dispatcher ticking before the Message Dispatcher is wired up is a startup
	// ordering mistake. Reporting work as remaining makes it recoverable; silently
	// discarding envelopes would not be.
	StubQueues queues;

	StubDispatcher dispatcher{queues, nullptr, nullptr, nullptr};

	queue_inbound_envelopes(queues, 2);
	queue_outbound_envelopes(queues, 2);

	const TickReport report = dispatcher.tick();

	REQUIRE(report.inbound_envelopes_routed == 0);
	REQUIRE(report.outbound_envelopes_sent == 0);
	REQUIRE(report.inbound_work_remains);
	REQUIRE(report.outbound_work_remains);
	REQUIRE(queues.inbound().size() == 2);
	REQUIRE(queues.outbound().size() == 2);
}

TEST_CASE("a tick on empty queues does nothing and says so", "[dispatcher]")
{
	// The common case, thirty times a second, for as long as REAPER is open.
	StubQueues queues;
	int routed_envelope_count = 0;
	int sent_envelope_count = 0;

	StubDispatcher dispatcher{
		queues,
		[&routed_envelope_count](StubInboundEnvelope) { ++routed_envelope_count; },
		[&sent_envelope_count](StubOutboundEnvelope) { ++sent_envelope_count; },
		[] {}
	};

	const TickReport report = dispatcher.tick();

	REQUIRE(routed_envelope_count == 0);
	REQUIRE(sent_envelope_count == 0);
	REQUIRE_FALSE(report.inbound_work_remains);
	REQUIRE_FALSE(report.outbound_work_remains);
	REQUIRE_FALSE(report.ui_state_published);
	REQUIRE_FALSE(report.reentrant_tick_skipped);
}

TEST_CASE("envelopes pushed from another thread are routed on the next tick", "[dispatcher][threads]")
{
	// The whole arrangement end to end: a network thread pushing parsed envelopes, a
	// main thread ticking, and nothing shared but the queue pair.
	constexpr int envelope_count = 200;

	StubQueues queues;
	std::vector<int> routed_identifiers;

	StubDispatcher dispatcher{
		queues,
		[&routed_identifiers](StubInboundEnvelope envelope) {
			routed_identifiers.push_back(envelope.identifier);
		},
		[](StubOutboundEnvelope) {},
		[] {}
	};

	std::thread network_thread{[&queues] {
		for (int envelope_identifier = 0; envelope_identifier < envelope_count; ++envelope_identifier) {
			queues.inbound().push(StubInboundEnvelope{"stream:delta", envelope_identifier});
		}
	}};

	while (static_cast<int>(routed_identifiers.size()) < envelope_count) {
		dispatcher.tick();
	}

	network_thread.join();

	REQUIRE(routed_identifiers.size() == envelope_count);

	for (int envelope_identifier = 0; envelope_identifier < envelope_count; ++envelope_identifier) {
		REQUIRE(routed_identifiers[static_cast<std::size_t>(envelope_identifier)] == envelope_identifier);
	}
}
