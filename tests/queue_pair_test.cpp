// The lock-protected queue pair.
//
// What matters about this queue is not that it stores things — it is that one
// thread can push while REAPER's main thread drains, that the drain stops where it
// was told to, and that no handler runs while the lock is held. The last one is the
// subtle one: a handler pushing a result onto a queue whose drain still held its
// mutex would deadlock a DAW, and a deadlock inside a timer callback is a REAPER
// that never repaints again.

#include <atomic>
#include <cstddef>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/queue_pair.h>

using sesh_ai::transport::ConcurrentQueue;
using sesh_ai::transport::QueuePair;

namespace {

	// Stands in for a parsed envelope. Move-only, so the tests fail to compile if the
	// queue ever copies what it was handed rather than moving it.
	struct StubEnvelope {
		explicit StubEnvelope(int identifier_value = 0)
			: identifier{identifier_value}
		{
		}

		StubEnvelope(const StubEnvelope&) = delete;
		StubEnvelope& operator=(const StubEnvelope&) = delete;
		StubEnvelope(StubEnvelope&&) = default;
		StubEnvelope& operator=(StubEnvelope&&) = default;

		int identifier;
	};

}

TEST_CASE("the queue hands items across in the order they arrived", "[queue]")
{
	ConcurrentQueue<StubEnvelope> queue;

	queue.push(StubEnvelope{1});
	queue.push(StubEnvelope{2});
	queue.push(StubEnvelope{3});

	REQUIRE(queue.size() == 3);
	REQUIRE_FALSE(queue.empty());

	const auto drained_envelopes = queue.drain_up_to(3);

	REQUIRE(drained_envelopes.size() == 3);
	REQUIRE(drained_envelopes[0].identifier == 1);
	REQUIRE(drained_envelopes[1].identifier == 2);
	REQUIRE(drained_envelopes[2].identifier == 3);
	REQUIRE(queue.empty());
}

TEST_CASE("an empty queue reports nothing rather than blocking", "[queue]")
{
	ConcurrentQueue<StubEnvelope> queue;

	REQUIRE_FALSE(queue.try_pop().has_value());
	REQUIRE(queue.drain_up_to(8).empty());
	REQUIRE(queue.size() == 0);
}

TEST_CASE("a drain takes no more than it was asked for and leaves the rest", "[queue]")
{
	ConcurrentQueue<StubEnvelope> queue;

	for (int envelope_identifier = 0; envelope_identifier < 10; ++envelope_identifier) {
		queue.push(StubEnvelope{envelope_identifier});
	}

	SECTION("a limit below the queue depth leaves the remainder queued, oldest first")
	{
		const auto first_drain = queue.drain_up_to(4);

		REQUIRE(first_drain.size() == 4);
		REQUIRE(first_drain.front().identifier == 0);
		REQUIRE(first_drain.back().identifier == 3);
		REQUIRE(queue.size() == 6);

		const auto second_drain = queue.drain_up_to(4);

		REQUIRE(second_drain.front().identifier == 4);
		REQUIRE(queue.size() == 2);
	}

	SECTION("a limit above the queue depth takes what is there and no more")
	{
		REQUIRE(queue.drain_up_to(100).size() == 10);
		REQUIRE(queue.empty());
	}

	SECTION("a limit of zero takes nothing")
	{
		REQUIRE(queue.drain_up_to(0).empty());
		REQUIRE(queue.size() == 10);
	}
}

TEST_CASE("items are returned by the drain, so no lock is held while they are handled", "[queue]")
{
	// The deadlock this guards against: handle an item while the drain still holds
	// the mutex, have that handler push, and a non-recursive mutex stops the process
	// dead. Routing an inbound envelope that produces an outbound result is exactly
	// that shape, and so is a handler that re-queues its own work.
	ConcurrentQueue<StubEnvelope> queue;

	queue.push(StubEnvelope{1});
	queue.push(StubEnvelope{2});

	const auto drained_envelopes = queue.drain_up_to(2);

	for (const auto& drained_envelope : drained_envelopes) {
		queue.push(StubEnvelope{drained_envelope.identifier + 100});
	}

	REQUIRE(queue.size() == 2);
	REQUIRE(queue.drain_up_to(2)[0].identifier == 101);
}

TEST_CASE("the two directions are independent queues", "[queue]")
{
	QueuePair<StubEnvelope, std::string> queues;

	queues.inbound().push(StubEnvelope{7});
	queues.outbound().push("response:transport");

	REQUIRE(queues.inbound().size() == 1);
	REQUIRE(queues.outbound().size() == 1);

	const auto drained_inbound = queues.inbound().drain_up_to(4);

	REQUIRE(drained_inbound.size() == 1);
	REQUIRE(drained_inbound[0].identifier == 7);

	// Draining one direction leaves the other untouched.
	REQUIRE(queues.inbound().empty());
	REQUIRE(queues.outbound().size() == 1);

	const auto drained_outbound = queues.outbound().drain_up_to(4);

	REQUIRE(drained_outbound.size() == 1);
	REQUIRE(drained_outbound[0] == "response:transport");
}

TEST_CASE("concurrent producers and a draining consumer lose and duplicate nothing", "[queue][threads]")
{
	// The real arrangement is one network thread pushing and REAPER's main thread
	// draining. Four producers is the same contract under more pressure, and the
	// assertion is the one that matters: every identifier arrives exactly once.
	constexpr int producer_thread_count = 4;
	constexpr int envelopes_per_producer = 500;
	constexpr int total_envelope_count = producer_thread_count * envelopes_per_producer;

	ConcurrentQueue<StubEnvelope> queue;
	std::atomic<int> pushed_envelope_count{0};

	std::vector<std::thread> producer_threads;
	producer_threads.reserve(producer_thread_count);

	for (int producer_index = 0; producer_index < producer_thread_count; ++producer_index) {
		producer_threads.emplace_back([&queue, &pushed_envelope_count, producer_index] {
			const int first_identifier = producer_index * envelopes_per_producer;

			for (int envelope_offset = 0; envelope_offset < envelopes_per_producer; ++envelope_offset) {
				queue.push(StubEnvelope{first_identifier + envelope_offset});
				pushed_envelope_count.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	std::set<int> received_identifiers;

	// Drains in bounded batches the way the dispatcher does, rather than waiting for
	// the producers to finish, so the drain really is interleaved with the pushes.
	while (static_cast<int>(received_identifiers.size()) < total_envelope_count) {
		for (const auto& drained_envelope : queue.drain_up_to(8)) {
			const auto insertion = received_identifiers.insert(drained_envelope.identifier);
			REQUIRE(insertion.second);
		}
	}

	for (auto& producer_thread : producer_threads) {
		producer_thread.join();
	}

	REQUIRE(pushed_envelope_count.load() == total_envelope_count);
	REQUIRE(received_identifiers.size() == static_cast<std::size_t>(total_envelope_count));
	REQUIRE(queue.empty());
	REQUIRE(*received_identifiers.begin() == 0);
	REQUIRE(*received_identifiers.rbegin() == total_envelope_count - 1);
}

TEST_CASE("a queue shared by pushing and popping threads stays consistent", "[queue][threads]")
{
	// try_pop rather than drain_up_to — the same queue under the other read path.
	constexpr int envelope_count = 2000;

	ConcurrentQueue<StubEnvelope> queue;

	std::thread producer_thread{[&queue] {
		for (int envelope_identifier = 0; envelope_identifier < envelope_count; ++envelope_identifier) {
			queue.push(StubEnvelope{envelope_identifier});
		}
	}};

	long long received_identifier_sum = 0;
	int received_envelope_count = 0;

	while (received_envelope_count < envelope_count) {
		if (auto popped_envelope = queue.try_pop()) {
			received_identifier_sum += popped_envelope->identifier;
			++received_envelope_count;
		}
	}

	producer_thread.join();

	constexpr long long expected_identifier_sum =
		static_cast<long long>(envelope_count - 1) * envelope_count / 2;

	REQUIRE(received_envelope_count == envelope_count);
	REQUIRE(received_identifier_sum == expected_identifier_sum);
	REQUIRE(queue.empty());
}
