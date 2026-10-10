// The lock-protected queue pair.
//
// Two queues plus a REAPER-registered timer are the extension's entire
// concurrency mechanism (requirement 22.1). Inbound carries envelopes the
// network thread has already parsed and validated, so the main thread receives
// trusted structures rather than text to work on (requirement 22.2). Outbound
// carries payloads the main thread produced, on their way to the network thread.
// Nothing else crosses a thread boundary.
//
// Deliberately not a blocking queue. The consumer is a REAPER timer callback
// running on REAPER's own main thread — the one thread in the process that must
// never wait for anything, because waiting there is a frozen DAW. So every read
// is a try, an empty queue is the ordinary case rather than a condition to
// signal, and there is no condition variable or notify/wait pair to get wrong.
//
// Templated on the item type rather than fixed to the envelope structures. The
// queue's job — hand items across a thread boundary in order, under a lock — does
// not depend on what an item is, and parameterising it means the suite exercises
// the real queue with a type it fully controls rather than a stand-in.
//
// Not bounded. A main thread starved long enough would let the inbound queue grow
// without limit, which is a backpressure question no requirement answers yet;
// size() is exposed so whoever answers it has the number to act on.

#ifndef SESH_AI_TRANSPORT_QUEUE_PAIR_H
#define SESH_AI_TRANSPORT_QUEUE_PAIR_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace sesh_ai::transport {

	// A first-in-first-out queue guarded by one mutex.
	template <typename Item>
	class ConcurrentQueue {
	public:
		using ItemType = Item;

		ConcurrentQueue() = default;

		// Copying a queue would copy items out from under one of the two threads
		// sharing it, and the mutex is not copyable anyway. Moving has the same
		// problem with a longer explanation, so neither is offered.
		ConcurrentQueue(const ConcurrentQueue&) = delete;
		ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;
		ConcurrentQueue(ConcurrentQueue&&) = delete;
		ConcurrentQueue& operator=(ConcurrentQueue&&) = delete;

		void push(Item item)
		{
			const std::lock_guard<std::mutex> lock{mutex_};
			items_.push_back(std::move(item));
		}

		// Empty when the queue is.
		std::optional<Item> try_pop()
		{
			const std::lock_guard<std::mutex> lock{mutex_};

			if (items_.empty()) {
				return std::nullopt;
			}

			Item item{std::move(items_.front())};
			items_.pop_front();

			return item;
		}

		// Removes up to maximum_item_count items and returns them, oldest first.
		//
		// Returning the items rather than taking a callback is the load-bearing
		// choice here: the lock is released before the caller touches anything it
		// got back. A handler that pushes a result onto a queue while the drain of
		// that same queue still held its lock would deadlock on a non-recursive
		// mutex, and outbound results produced while routing inbound envelopes are
		// exactly that shape. The cost is one vector per drain, against a handler
		// that will go on to call into REAPER.
		std::vector<Item> drain_up_to(std::size_t maximum_item_count)
		{
			std::vector<Item> drained_items;

			if (maximum_item_count == 0) {
				return drained_items;
			}

			const std::lock_guard<std::mutex> lock{mutex_};

			const std::size_t item_count_to_drain =
				items_.size() < maximum_item_count ? items_.size() : maximum_item_count;

			drained_items.reserve(item_count_to_drain);

			for (std::size_t drained_item_index = 0; drained_item_index < item_count_to_drain; ++drained_item_index) {
				drained_items.push_back(std::move(items_.front()));
				items_.pop_front();
			}

			return drained_items;
		}

		// A snapshot, true only at the moment it was taken — the other thread may
		// push before the caller reads it. Useful for reporting queue depth, not for
		// deciding whether a pop will succeed.
		std::size_t size() const
		{
			const std::lock_guard<std::mutex> lock{mutex_};
			return items_.size();
		}

		bool empty() const
		{
			const std::lock_guard<std::mutex> lock{mutex_};
			return items_.empty();
		}

	private:
		mutable std::mutex mutex_;
		std::deque<Item> items_;
	};

	// The two queues, held together so "exactly two lock-protected queues" is a
	// type rather than a convention somebody has to remember.
	template <typename InboundItem, typename OutboundItem>
	class QueuePair {
	public:
		using InboundQueue = ConcurrentQueue<InboundItem>;
		using OutboundQueue = ConcurrentQueue<OutboundItem>;

		QueuePair() = default;

		QueuePair(const QueuePair&) = delete;
		QueuePair& operator=(const QueuePair&) = delete;
		QueuePair(QueuePair&&) = delete;
		QueuePair& operator=(QueuePair&&) = delete;

		InboundQueue& inbound() { return inbound_; }
		const InboundQueue& inbound() const { return inbound_; }

		OutboundQueue& outbound() { return outbound_; }
		const OutboundQueue& outbound() const { return outbound_; }

	private:
		InboundQueue inbound_;
		OutboundQueue outbound_;
	};

}

#endif
