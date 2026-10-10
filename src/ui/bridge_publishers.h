// The real `ConfirmationPresenter` and `ConnectionStatePresenter`, and the one object
// that publishes UI state from the tick (requirements 2.3, 3.4, 13.1, 13.5, 16.1, 16.2,
// 23.2, 23.7).
//
// `confirmation_coordinator.h` and `transport_client.h` each declare a narrow presenter
// seam and name the UI Host as what implements it. Until now both existed only as
// recorders in `tests/`, which means the coordinator and the client were each tested
// against a surface nothing produced. These are the implementations, and they are here
// rather than in the composition's `.cpp` for the reason the whole codebase is arranged
// this way: `tests/CMakeLists.txt` does not compile `src/`, so logic in a `src/*.cpp` is
// logic nothing can reach.
//
// Everything below is decision logic over `BridgeMessage`s. CEF is behind
// `BrowserHost`, which `UiHost` already holds, so the suite drives all of this against
// a recording browser.
//
// ---------------------------------------------------------------------------
// The two presenters are called on different threads, and that is the whole
// difference between them
//
// `ConfirmationPresenter`'s two calls happen on REAPER's main thread, inside the tick
// that routed the envelope — `confirmation_coordinator.h` says so outright. So
// `BridgeConfirmationPresenter` publishes immediately. There is no deferral to arrange
// and no lock to take.
//
// `ConnectionStatePresenter` is the opposite. `TransportClient` runs on the network
// thread (requirement 22.2) and presents a status from there, while requirement 2.3
// puts every write to CEF inside the tick. So `DeferredConnectionStatePresenter` holds
// the latest status under a mutex, asks the Main-Thread Dispatcher to publish, and
// `BridgeUiStatePublisher` is what actually crosses the bridge, on the main thread,
// later.
//
// The design document describes this as handing the status "to the outbound side of the
// queue pair", and that is the one place this implementation departs from it — because
// the outbound queue carries `OutboundEnvelope`, which goes to the *server*.
// `ui_host.h` is explicit that the `view:` namespace exists so that a view model pushed
// onto the outbound queue is refused by `envelope.schema.json` rather than sent to a
// server with no handler for it, so pushing a connection status there would be the
// exact mistake that namespace is load-bearing against. A dedicated single-slot
// handoff is the same arrangement — produced on the network thread, consumed on the
// main thread — without a view model ever being addressable to the server.
//
// Only the latest status is kept, and intermediate ones are dropped on purpose. A
// backoff schedule that walks 500 ms to 30 seconds produces a status per attempt, and
// the producer needs to see where the connection *is*, not a replay of how it got
// there. A queue here would render the same outage several times over.
//
// ---------------------------------------------------------------------------
// A tick that changed nothing publishes nothing
//
// `StreamPresenter::state_revision()` exists for this, and says so: it increments on
// every accepted change so that the publisher can compare it against what it last
// published. `BridgeUiStatePublisher` does that, and then asks the narrower question
// per view, because one revision bump does not mean all three of the presenter's views
// moved — a delta changes the response and leaves the ingest details and the last error
// exactly as they were.
//
// For the response view and the error view that question is answered by comparing the
// serialised payload, which is the only comparison that cannot disagree with what was
// sent.
//
// The ingest display is compared by the SHA-256 of its payload instead, and that is a
// deliberate exception rather than an inconsistency. Its payload carries the publish
// token (requirement 16.2), and `stream_presenter.h` is careful that the token is held
// in exactly one place; remembering the payload here to compare against would make this
// object a second holder of a credential for the life of the process, for no purpose
// but equality. A digest answers the same question — including the case a token is
// replaced by a different one of the same length, which comparing
// `StreamIngestLogFields` would miss and which matters because the producer would
// otherwise be left looking at a key that no longer works.

#ifndef SESH_AI_UI_BRIDGE_PUBLISHERS_H
#define SESH_AI_UI_BRIDGE_PUBLISHERS_H

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <auth/sha256.h>
#include <transport/confirmation_coordinator.h>
#include <transport/transport_client.h>
#include <ui/bridge_serialisation.h>
#include <ui/stream_presenter.h>
#include <ui/ui_host.h>

namespace sesh_ai::ui {

	// ---------------------------------------------------------------------------
	// What a publish did
	// ---------------------------------------------------------------------------

	// Both presenter seams return `void`, so neither the Confirmation Coordinator nor
	// the Transport Client can hear that a publish was refused. That is right for them
	// — a coordinator cannot do anything useful about CEF saying no — but it leaves the
	// refusal needing somewhere to go, because a prompt that never reached the panel is
	// a producer watching an operation stall for 120 seconds.
	//
	// So the presenters count, and name the last refusal. The plugin entry reads these
	// and writes the line, which is the same arrangement `RoutingOutcome` and
	// `TickReport` use.
	struct BridgePublicationTally {
		std::size_t published = 0;
		std::size_t refused = 0;

		// The reason the most recent refusal gave. `BridgeRefusalReason::none` while
		// nothing has been refused.
		BridgeRefusalReason last_refusal = BridgeRefusalReason::none;

		// The message name the most recent refusal was carrying. Empty while nothing
		// has been refused.
		std::string last_refused_message_name;

		void note(const PublishOutcome& outcome)
		{
			if (outcome.published) {
				++published;
				return;
			}

			++refused;
			last_refusal = outcome.refusal;
			last_refused_message_name = outcome.message_name;
		}

		bool anything_was_refused() const noexcept { return refused > 0; }
	};

	// ---------------------------------------------------------------------------
	// The Confirmation Coordinator's presenter
	// ---------------------------------------------------------------------------

	// Publishes `view:confirmation_pending` and `view:confirmation_resolved`.
	//
	// Both views are already decoded by the coordinator, which is the half of the
	// bridge contract design.md calls deliberate: `riskLevelReported`,
	// `descriptionComplete`, `answeredByThisClient`, `contradictsLocalVerdict`, and
	// `nothingWasChanged` are the coordinator's conclusions and no protocol schema has
	// them. So there is nothing to decide here — the view goes across as it stands,
	// under the name `ui_host.h` declares, with the casing the serialiser applies.
	class BridgeConfirmationPresenter final : public transport::ConfirmationPresenter {
	public:
		explicit BridgeConfirmationPresenter(UiHost& ui_host)
			: ui_host_{ui_host}
		{
		}

		BridgeConfirmationPresenter(const BridgeConfirmationPresenter&) = delete;
		BridgeConfirmationPresenter& operator=(const BridgeConfirmationPresenter&) = delete;
		BridgeConfirmationPresenter(BridgeConfirmationPresenter&&) = delete;
		BridgeConfirmationPresenter& operator=(BridgeConfirmationPresenter&&) = delete;

		void present_confirmation(
			const transport::PendingConfirmationView& pending_confirmation) override
		{
			tally_.note(ui_host_.publish_to_javascript(bridge_message_for(pending_confirmation)));
		}

		void dismiss_confirmation(
			const transport::ConfirmationResolutionView& resolution) override
		{
			tally_.note(ui_host_.publish_to_javascript(bridge_message_for(resolution)));
		}

		const BridgePublicationTally& tally() const noexcept { return tally_; }

	private:
		UiHost& ui_host_;
		BridgePublicationTally tally_;
	};

	// ---------------------------------------------------------------------------
	// The Transport Client's presenter
	// ---------------------------------------------------------------------------

	// Asks the Main-Thread Dispatcher to publish UI state on its next tick.
	//
	// `MainThreadDispatcher::request_ui_state_publication` is documented safe from any
	// thread and is the one operation needed here, so it is taken as a callable rather
	// than by naming the dispatcher — which would pull its template parameters, and
	// with them the envelope types, into a header that has no business knowing either.
	using UiStatePublicationRequest = std::function<void()>;

	// Holds the latest connection status for the tick to publish.
	//
	// Written from the network thread, read from the main thread, which is what the
	// mutex is for — and it is the only lock in this header, because it is the only
	// place in the UI half of the extension where two threads meet.
	class DeferredConnectionStatePresenter final : public transport::ConnectionStatePresenter {
	public:
		explicit DeferredConnectionStatePresenter(
			UiStatePublicationRequest request_ui_state_publication = {})
			: request_ui_state_publication_{std::move(request_ui_state_publication)}
		{
		}

		DeferredConnectionStatePresenter(const DeferredConnectionStatePresenter&) = delete;
		DeferredConnectionStatePresenter& operator=(const DeferredConnectionStatePresenter&) = delete;
		DeferredConnectionStatePresenter(DeferredConnectionStatePresenter&&) = delete;
		DeferredConnectionStatePresenter& operator=(DeferredConnectionStatePresenter&&) = delete;

		// Network thread. Nothing here touches CEF, which is requirement 2.3's whole
		// point.
		void present_connection_status(const transport::connection_status& status) override
		{
			{
				const std::lock_guard<std::mutex> held{pending_status_mutex_};

				// Replaced rather than appended. See the file comment: a producer needs
				// where the connection is, not a replay of every backoff step.
				pending_status_ = status;
				++presented_count_;
			}

			// Outside the lock. The request is an atomic store on the dispatcher, but a
			// callable supplied by a caller is not this object's to make promises about,
			// and holding a lock across it would put the network thread's progress behind
			// something the main thread could be inside.
			if (request_ui_state_publication_) {
				request_ui_state_publication_();
			}
		}

		// Main thread, from the publisher below. Empty when nothing has arrived since
		// the last take, which is what makes a tick with no connection change publish
		// nothing.
		std::optional<transport::connection_status> take_pending_status()
		{
			const std::lock_guard<std::mutex> held{pending_status_mutex_};

			std::optional<transport::connection_status> taken = std::move(pending_status_);
			pending_status_.reset();

			return taken;
		}

		// How many statuses the client presented, including the ones a later status
		// replaced before the tick got to them. Exposed so "nothing was published"
		// and "nothing happened" can be told apart.
		std::size_t presented_count() const
		{
			const std::lock_guard<std::mutex> held{pending_status_mutex_};

			return presented_count_;
		}

		bool has_pending_status() const
		{
			const std::lock_guard<std::mutex> held{pending_status_mutex_};

			return pending_status_.has_value();
		}

	private:
		UiStatePublicationRequest request_ui_state_publication_;

		mutable std::mutex pending_status_mutex_;
		std::optional<transport::connection_status> pending_status_;
		std::size_t presented_count_ = 0;
	};

	// ---------------------------------------------------------------------------
	// The tick's publish step
	// ---------------------------------------------------------------------------

	// What one publish step did. Returned rather than logged, like everything else
	// here.
	struct UiStatePublicationReport {
		bool connection_state_published = false;
		bool agent_response_published = false;
		bool stream_ingest_published = false;
		bool stream_error_published = false;

		// True when the Stream Presenter's revision was unchanged, so none of its three
		// views was even considered. Not a problem — it is the common case on a quiet
		// tick, and the reason this component exists.
		bool stream_state_unchanged = false;

		BridgePublicationTally tally;

		std::size_t published_message_count() const
		{
			return static_cast<std::size_t>(connection_state_published)
				+ static_cast<std::size_t>(agent_response_published)
				+ static_cast<std::size_t>(stream_ingest_published)
				+ static_cast<std::size_t>(stream_error_published);
		}
	};

	// Publishes everything the panel is shown, from inside the tick and nowhere else.
	//
	// This is what `MainThreadDispatcher::UiStatePublisher` is bound to. The dispatcher
	// calls it at most once per tick however many envelopes it routed, which is the
	// budget that makes a turn's worth of streaming deltas one repaint instead of
	// forty.
	//
	// The Confirmation Coordinator's two views are *not* published here, and that is
	// not an omission. A confirmation is raised and dismissed by the coordinator at the
	// moment it decides, on this same thread, through
	// `BridgeConfirmationPresenter` — so publishing it again from here would be a
	// second copy of a prompt the panel already has, and polling for a state nobody
	// said had changed.
	class BridgeUiStatePublisher {
	public:
		BridgeUiStatePublisher(
			UiHost& ui_host,
			DeferredConnectionStatePresenter& connection_state,
			const StreamPresenter& stream_presenter)
			: ui_host_{ui_host},
			connection_state_{connection_state},
			stream_presenter_{stream_presenter}
		{
		}

		BridgeUiStatePublisher(const BridgeUiStatePublisher&) = delete;
		BridgeUiStatePublisher& operator=(const BridgeUiStatePublisher&) = delete;
		BridgeUiStatePublisher(BridgeUiStatePublisher&&) = delete;
		BridgeUiStatePublisher& operator=(BridgeUiStatePublisher&&) = delete;

		UiStatePublicationReport publish()
		{
			UiStatePublicationReport report;

			publish_connection_state(report);
			publish_stream_state(report);

			tally_.published += report.tally.published;
			tally_.refused += report.tally.refused;

			if (report.tally.anything_was_refused()) {
				tally_.last_refusal = report.tally.last_refusal;
				tally_.last_refused_message_name = report.tally.last_refused_message_name;
			}

			return report;
		}

		const BridgePublicationTally& tally() const noexcept { return tally_; }

	private:
		void publish_connection_state(UiStatePublicationReport& report)
		{
			const std::optional<transport::connection_status> status =
				connection_state_.take_pending_status();

			if (!status.has_value()) {
				return;
			}

			const PublishOutcome outcome = ui_host_.publish_to_javascript(bridge_message_for(*status));

			report.tally.note(outcome);
			report.connection_state_published = outcome.published;
		}

		void publish_stream_state(UiStatePublicationReport& report)
		{
			const std::uint64_t revision = stream_presenter_.state_revision();

			if (published_stream_revision_.has_value() && *published_stream_revision_ == revision) {
				report.stream_state_unchanged = true;
				return;
			}

			published_stream_revision_ = revision;

			publish_agent_response(report);
			publish_stream_ingest(report);
			publish_stream_error(report);
		}

		void publish_agent_response(UiStatePublicationReport& report)
		{
			// The inactive view is published too, on the one tick that first reports it.
			// `ResponseView::active` is a field the UI reads — a response that ended and
			// was reset leaves a panel showing the previous turn forever if the only
			// thing that ever crosses is an active one.
			std::string payload = serialise_response_view(stream_presenter_.current_response());

			if (published_agent_response_payload_.has_value()
				&& *published_agent_response_payload_ == payload) {
				return;
			}

			const PublishOutcome outcome = ui_host_.publish_to_javascript(
				bridge_message(std::string{agent_response_bridge_message_name}, payload)
			);

			report.tally.note(outcome);
			report.agent_response_published = outcome.published;

			if (outcome.published) {
				published_agent_response_payload_ = std::move(payload);
			}
		}

		void publish_stream_ingest(UiStatePublicationReport& report)
		{
			const std::optional<StreamIngestDisplay> ingest = stream_presenter_.ingest_display();

			if (!ingest.has_value()) {
				return;
			}

			std::string payload = serialise_stream_ingest_display(*ingest);

			// Compared by digest rather than by payload. See the file comment — the
			// payload carries the publish token and this object has no business keeping
			// one.
			const auth::sha256_digest digest = auth::sha256(payload);

			if (published_stream_ingest_digest_.has_value()
				&& *published_stream_ingest_digest_ == digest) {
				return;
			}

			const PublishOutcome outcome = ui_host_.publish_to_javascript(
				bridge_message(std::string{stream_ingest_bridge_message_name}, std::move(payload))
			);

			report.tally.note(outcome);
			report.stream_ingest_published = outcome.published;

			if (outcome.published) {
				published_stream_ingest_digest_ = digest;
			}
		}

		void publish_stream_error(UiStatePublicationReport& report)
		{
			const std::optional<StreamErrorView>& error = stream_presenter_.last_stream_error();

			if (!error.has_value()) {
				return;
			}

			std::string payload = serialise_stream_error_view(*error);

			if (published_stream_error_payload_.has_value()
				&& *published_stream_error_payload_ == payload) {
				return;
			}

			const PublishOutcome outcome = ui_host_.publish_to_javascript(
				bridge_message(std::string{stream_error_bridge_message_name}, payload)
			);

			report.tally.note(outcome);
			report.stream_error_published = outcome.published;

			if (outcome.published) {
				published_stream_error_payload_ = std::move(payload);
			}
		}

		UiHost& ui_host_;
		DeferredConnectionStatePresenter& connection_state_;
		const StreamPresenter& stream_presenter_;

		// Absent until the first publish step, so the first tick always considers the
		// stream views — a revision of 0 is a presenter nothing has happened to, and
		// comparing against a default-constructed 0 would skip it.
		std::optional<std::uint64_t> published_stream_revision_;

		// What was last sent, so a revision bump that did not move a given view does not
		// re-send it. A refused publish leaves these alone, so the next tick tries
		// again rather than treating a message CEF rejected as delivered.
		std::optional<std::string> published_agent_response_payload_;
		std::optional<std::string> published_stream_error_payload_;
		std::optional<auth::sha256_digest> published_stream_ingest_digest_;

		BridgePublicationTally tally_;
	};

}

#endif
