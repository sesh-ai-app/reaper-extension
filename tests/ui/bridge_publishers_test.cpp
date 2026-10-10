// The two real presenters, and the one object that publishes UI state from the tick.
//
// Four things are worth testing here.
//
// **A tick that changed nothing publishes nothing.** This is the component's whole
// reason to exist. REAPER's timer runs about thirty times a second on the thread that
// draws REAPER's own UI, so a publisher that pushed the current state every tick would
// be thirty bridge crossings a second for a session where the producer is doing
// nothing. The revision comparison is checked, and so is the narrower per-view
// comparison under it — one revision bump does not mean all three of the Stream
// Presenter's views moved, and a delta must not re-send the ingest details.
//
// **A new publish token reaches the panel.** The ingest view is compared by digest
// rather than by payload, because the payload carries a credential this object has no
// business keeping. A digest comparison that got this wrong would leave the producer
// looking at a key that no longer works, so the case that matters is a token replaced
// by a *different token of the same length* — the one a field-by-field comparison on
// the log-safe fields would miss.
//
// **The connection status is produced on one thread and published on another.** The
// Transport Client presents from the network thread and requirement 2.3 puts every
// write to CEF inside the tick, so the presenter holds and the publisher takes. Driven
// with real threads rather than reasoned about, following `tests/queue_pair_test.cpp`:
// the handoff is the only thing in this header whose contract is about threads.
//
// **A refused publish is not treated as delivered.** CEF refusing is the case where
// remembering what was sent is wrong — the next tick has to try again, or a panel that
// was busy for one tick never catches up.
//
// No CEF. `UiHost` holds a `BrowserHost`, and the recorder below is it.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ui/bridge_publishers.h>
#include <ui/deferred_browser_host.h>
#include <ui/ui_host.h>

using sesh_ai::transport::ConfirmationResolution;
using sesh_ai::transport::ConfirmationResolutionView;
using sesh_ai::transport::ConfirmationRiskLevel;
using sesh_ai::transport::PendingConfirmationView;
using sesh_ai::transport::connection_state;
using sesh_ai::transport::connection_status;
using sesh_ai::ui::BridgeConfirmationPresenter;
using sesh_ai::ui::BridgeRefusalReason;
using sesh_ai::ui::BridgeUiStatePublisher;
using sesh_ai::ui::BrowserHost;
using sesh_ai::ui::DeferredBrowserHost;
using sesh_ai::ui::DeferredConnectionStatePresenter;
using sesh_ai::ui::LocalAssetUrl;
using sesh_ai::ui::StreamPresenter;
using sesh_ai::ui::UiHost;
using sesh_ai::ui::UiStatePublicationReport;
using sesh_ai::ui::agent_response_bridge_message_name;
using sesh_ai::ui::build_local_asset_url;
using sesh_ai::ui::confirmation_resolution_bridge_message_name;
using sesh_ai::ui::connection_state_bridge_message_name;
using sesh_ai::ui::pending_confirmation_bridge_message_name;
using sesh_ai::ui::stream_ingest_bridge_message_name;

namespace {

	// What CEF looks like when it is a recorder. Every published message, in order.
	class RecordingBrowserHost final : public BrowserHost {
	public:
		struct PublishedMessage {
			std::string message_name;
			std::string serialized_json;
		};

		bool create_browser(const std::string& local_asset_url) override
		{
			loaded_url = local_asset_url;

			return true;
		}

		bool navigate(const std::string& absolute_url) override
		{
			loaded_url = absolute_url;

			return true;
		}

		bool post_message_to_javascript(
			const std::string& message_name,
			const std::string& serialized_json) override
		{
			if (refuse_everything) {
				return false;
			}

			published.push_back(PublishedMessage{message_name, serialized_json});

			return true;
		}

		void close_browser() override {}
		void shut_down_cef() override {}

		std::size_t count_of(std::string_view message_name) const
		{
			std::size_t count = 0;

			for (const PublishedMessage& message : published) {
				if (message.message_name == message_name) {
					++count;
				}
			}

			return count;
		}

		std::optional<std::string> last_payload_of(std::string_view message_name) const
		{
			std::optional<std::string> payload;

			for (const PublishedMessage& message : published) {
				if (message.message_name == message_name) {
					payload = message.serialized_json;
				}
			}

			return payload;
		}

		std::vector<PublishedMessage> published;
		std::string loaded_url;
		bool refuse_everything = false;
	};

	// The dispatcher side of `UiHost`, which none of these cases exercises — they are
	// all about the outbound half.
	class RefusingDispatcherSink final : public sesh_ai::ui::DispatcherSink {
	public:
		bool accept_message_from_javascript(const sesh_ai::ui::BridgeMessage&) override
		{
			return false;
		}
	};

	// A `UiHost` with its browser created, which is the state every publish needs —
	// `publish_to_javascript` refuses with `browser_not_created` otherwise, and that
	// refusal is `ui_host_test.cpp`'s to cover rather than this file's.
	struct OpenPanel {
		RecordingBrowserHost browser_host;
		RefusingDispatcherSink dispatcher_sink;
		UiHost ui_host{browser_host, dispatcher_sink};

		OpenPanel()
		{
			const auto application_entry_point =
				build_local_asset_url("/opt/sesh-ai/ui", "index.html");

			REQUIRE(application_entry_point.valid);
			REQUIRE(ui_host.create_browser(application_entry_point.url).created);
		}
	};

	PendingConfirmationView a_pending_confirmation()
	{
		PendingConfirmationView view;
		view.request_id = "confirm-7";
		view.action_summary = "Delete the track";
		view.details = "One track.";
		view.risk_level = ConfirmationRiskLevel::high;
		view.risk_level_reported = true;
		view.description_complete = true;

		return view;
	}

	// A `stream:rtmps_url` payload, in the shape the presenter's envelope entry point
	// reads. The stub document is the minimum this one envelope type needs.
	class StubJson {
	public:
		StubJson() = default;

		static StubJson string_value(std::string value)
		{
			StubJson json;
			json.is_string_ = true;
			json.string_ = std::move(value);

			return json;
		}

		static StubJson object_with(std::vector<std::pair<std::string, StubJson>> properties)
		{
			StubJson json;
			json.is_object_ = true;
			json.properties_ = std::move(properties);

			return json;
		}

		bool is_object() const { return is_object_; }
		bool is_string() const { return is_string_; }
		bool is_boolean() const { return false; }
		bool is_number_integer() const { return false; }
		bool is_null() const { return !is_object_ && !is_string_; }

		bool contains(const std::string& name) const { return find(name) != nullptr; }

		const StubJson& at(const std::string& name) const
		{
			const StubJson* const found = find(name);
			REQUIRE(found != nullptr);

			return *found;
		}

		// Only the string case is reachable from these envelopes — the ingest and start
		// payloads are strings throughout — but the presenter's reader is instantiated
		// for the numeric and boolean cases a delta and a stop carry, so they have to
		// compile.
		template <typename ValueType>
		ValueType get() const
		{
			if constexpr (std::is_same_v<ValueType, std::string>) {
				return string_;
			} else {
				return ValueType{};
			}
		}

	private:
		const StubJson* find(const std::string& name) const
		{
			for (const auto& property : properties_) {
				if (property.first == name) {
					return &property.second;
				}
			}

			return nullptr;
		}

		bool is_object_ = false;
		bool is_string_ = false;
		std::string string_;
		std::vector<std::pair<std::string, StubJson>> properties_;
	};

	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubJson payload;
	};

	StubInboundEnvelope an_ingest_envelope(const std::string& stream_key)
	{
		StubInboundEnvelope envelope;
		envelope.type = std::string{sesh_ai::ui::stream_rtmps_url_envelope_type};
		envelope.payload = StubJson::object_with({
			{"stageArn", StubJson::string_value("arn:aws:ivs:us-east-1:1:stage/abc")},
			{"rtmpsIngestUrl", StubJson::string_value("rtmps://1.example.net:443/app/")},
			{"streamKey", StubJson::string_value(stream_key)},
			{"participantId", StubJson::string_value("participant-1")},
			{"expiresAt", StubJson::string_value("2026-01-01T00:00:00Z")}
		});

		return envelope;
	}

}

TEST_CASE("the confirmation presenter publishes the prompt and its dismissal", "[bridge][presenter]")
{
	OpenPanel panel;
	BridgeConfirmationPresenter presenter{panel.ui_host};

	SECTION("raising the prompt crosses the bridge under the pending confirmation's name")
	{
		presenter.present_confirmation(a_pending_confirmation());

		REQUIRE(panel.browser_host.count_of(pending_confirmation_bridge_message_name) == 1);
		REQUIRE(presenter.tally().published == 1);
		REQUIRE_FALSE(presenter.tally().anything_was_refused());
	}

	SECTION("taking it down is a different message, not a repeat of the first")
	{
		// Two different operations on one prompt, and collapsing them would leave the UI
		// unable to say how it ended — which is requirements 13.5 and 23.7.
		presenter.present_confirmation(a_pending_confirmation());

		ConfirmationResolutionView resolution;
		resolution.request_id = "confirm-7";
		resolution.resolution = ConfirmationResolution::expired;
		resolution.nothing_was_changed = true;

		presenter.dismiss_confirmation(resolution);

		REQUIRE(panel.browser_host.count_of(pending_confirmation_bridge_message_name) == 1);
		REQUIRE(panel.browser_host.count_of(confirmation_resolution_bridge_message_name) == 1);
		REQUIRE(presenter.tally().published == 2);
	}

	SECTION("a refusal is counted and named, because the seam returns nothing")
	{
		// The coordinator cannot hear that CEF said no, and a prompt that never reached
		// the panel is a producer watching an operation stall for 120 seconds. So the
		// refusal has to be readable from somewhere.
		panel.browser_host.refuse_everything = true;

		presenter.present_confirmation(a_pending_confirmation());

		REQUIRE(presenter.tally().published == 0);
		REQUIRE(presenter.tally().refused == 1);
		REQUIRE(presenter.tally().last_refusal == BridgeRefusalReason::browser_host_refused);
		REQUIRE(presenter.tally().last_refused_message_name
			== pending_confirmation_bridge_message_name);
	}
}

TEST_CASE("the connection state presenter defers the publish to the tick", "[bridge][presenter]")
{
	SECTION("presenting asks for a publish and touches nothing else")
	{
		std::size_t publication_requests = 0;
		DeferredConnectionStatePresenter presenter{[&publication_requests] {
			++publication_requests;
		}};

		connection_status status;
		status.state = connection_state::reconnecting;

		presenter.present_connection_status(status);

		REQUIRE(publication_requests == 1);
		REQUIRE(presenter.has_pending_status());
		REQUIRE(presenter.presented_count() == 1);
	}

	SECTION("only the latest status survives, because a backoff replay is not useful")
	{
		// Requirement 3.2's schedule walks 500 ms to 30 seconds, one status per attempt.
		// The producer needs where the connection is, not how it got there — so a queue
		// here would render one outage several times over.
		DeferredConnectionStatePresenter presenter;

		for (std::size_t attempt = 1; attempt <= 5; ++attempt) {
			connection_status status;
			status.state = connection_state::reconnecting;
			status.consecutive_failed_attempts = attempt;

			presenter.present_connection_status(status);
		}

		REQUIRE(presenter.presented_count() == 5);

		const std::optional<connection_status> taken = presenter.take_pending_status();

		REQUIRE(taken.has_value());
		REQUIRE(taken->consecutive_failed_attempts == 5);

		// And taking it empties the slot, so the next tick publishes nothing.
		REQUIRE_FALSE(presenter.take_pending_status().has_value());
	}

	SECTION("a status presented from another thread is taken on this one")
	{
		// The handoff is the only thing in this header whose contract is about threads,
		// so it is driven with one rather than reasoned about.
		std::atomic<std::size_t> publication_requests{0};
		DeferredConnectionStatePresenter presenter{[&publication_requests] {
			publication_requests.fetch_add(1, std::memory_order_relaxed);
		}};

		constexpr std::size_t presentation_count = 200;

		std::thread network_thread{[&presenter] {
			for (std::size_t attempt = 1; attempt <= presentation_count; ++attempt) {
				connection_status status;
				status.state = connection_state::reconnecting;
				status.consecutive_failed_attempts = attempt;

				presenter.present_connection_status(status);
			}

			connection_status connected;
			connected.state = connection_state::connected;
			presenter.present_connection_status(connected);
		}};

		// The main thread takes while the other presents, which is what a tick does.
		std::size_t taken_count = 0;
		std::optional<connection_status> last_taken;

		while (!last_taken.has_value() || last_taken->state != connection_state::connected) {
			if (std::optional<connection_status> taken = presenter.take_pending_status()) {
				last_taken = std::move(taken);
				++taken_count;
			}
		}

		network_thread.join();

		REQUIRE(presenter.presented_count() == presentation_count + 1);
		REQUIRE(publication_requests.load(std::memory_order_relaxed) == presentation_count + 1);

		// Fewer takes than presentations is the designed behaviour, not a loss: the
		// replaced ones are intermediate backoff steps. What must hold is that the last
		// state presented is the state the panel ends up showing.
		REQUIRE(taken_count <= presentation_count + 1);
		REQUIRE(last_taken->state == connection_state::connected);
	}
}

TEST_CASE("the publish step sends nothing when nothing changed", "[bridge][publisher]")
{
	OpenPanel panel;
	DeferredConnectionStatePresenter connection_state;
	StreamPresenter stream_presenter;
	BridgeUiStatePublisher publisher{panel.ui_host, connection_state, stream_presenter};

	SECTION("the first step publishes the stream's own idle view and nothing else")
	{
		// A revision of 0 is a presenter nothing has happened to, so the first step has
		// to consider the views rather than compare against a default-constructed
		// revision and skip.
		const UiStatePublicationReport report = publisher.publish();

		REQUIRE_FALSE(report.stream_state_unchanged);
		REQUIRE(report.agent_response_published);
		REQUIRE_FALSE(report.connection_state_published);
		REQUIRE_FALSE(report.stream_ingest_published);
		REQUIRE_FALSE(report.stream_error_published);
		REQUIRE(report.published_message_count() == 1);
	}

	SECTION("a second step with nothing changed publishes nothing at all")
	{
		publisher.publish();

		const UiStatePublicationReport report = publisher.publish();

		REQUIRE(report.stream_state_unchanged);
		REQUIRE(report.published_message_count() == 0);
		REQUIRE(panel.browser_host.published.size() == 1);
	}

	SECTION("thirty quiet ticks cross the bridge once")
	{
		// The number that matters: the timer runs at about 30 Hz, so this is one
		// second of a session where the producer is doing nothing.
		for (std::size_t tick = 0; tick < 30; ++tick) {
			publisher.publish();
		}

		REQUIRE(panel.browser_host.published.size() == 1);
	}
}

TEST_CASE("the publish step sends each view only when that view moved", "[bridge][publisher]")
{
	OpenPanel panel;
	DeferredConnectionStatePresenter connection_state;
	StreamPresenter stream_presenter;
	BridgeUiStatePublisher publisher{panel.ui_host, connection_state, stream_presenter};

	// The first step, so the comparisons below start from a published baseline.
	publisher.publish();

	SECTION("a connection status crosses once per status, not once per tick")
	{
		connection_status status;
		status.state = connection_state::connected;
		connection_state.present_connection_status(status);

		REQUIRE(publisher.publish().connection_state_published);
		REQUIRE(panel.browser_host.count_of(connection_state_bridge_message_name) == 1);

		// No new status, so nothing to publish — the slot was emptied by the take.
		REQUIRE_FALSE(publisher.publish().connection_state_published);
		REQUIRE(panel.browser_host.count_of(connection_state_bridge_message_name) == 1);
	}

	SECTION("ingest details cross once, and a delta afterwards does not re-send them")
	{
		// This is the case the per-view comparison exists for. A revision bump means
		// *something* changed; publishing all three views on it would re-send a publish
		// token on every delta of a streaming turn.
		REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_one")).recognised);

		const UiStatePublicationReport after_ingest = publisher.publish();

		REQUIRE(after_ingest.stream_ingest_published);
		REQUIRE(panel.browser_host.count_of(stream_ingest_bridge_message_name) == 1);

		// Now move the response. The revision changes, the ingest details do not.
		StubInboundEnvelope start;
		start.type = std::string{sesh_ai::ui::agent_response_start_envelope_type};
		start.payload = StubJson::object_with({
			{"conversationId", StubJson::string_value("conversation-1")},
			{"turnId", StubJson::string_value("turn-1")},
			{"messageId", StubJson::string_value("message-1")}
		});

		REQUIRE(stream_presenter.handle(start).recognised);

		const UiStatePublicationReport after_start = publisher.publish();

		REQUIRE(after_start.agent_response_published);
		REQUIRE_FALSE(after_start.stream_ingest_published);
		REQUIRE(panel.browser_host.count_of(stream_ingest_bridge_message_name) == 1);
	}

	SECTION("a reissued token of the same length still reaches the panel")
	{
		// The case a comparison on the log-safe fields would miss, and the one that
		// matters: everything but the token identical, the token itself different and
		// the same length. Missing it would leave the producer transcribing a key that
		// no longer works.
		REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_one")).recognised);
		REQUIRE(publisher.publish().stream_ingest_published);

		const std::optional<std::string> first_payload =
			panel.browser_host.last_payload_of(stream_ingest_bridge_message_name);
		REQUIRE(first_payload.has_value());

		REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_two")).recognised);
		REQUIRE(publisher.publish().stream_ingest_published);

		REQUIRE(panel.browser_host.count_of(stream_ingest_bridge_message_name) == 2);

		const std::optional<std::string> second_payload =
			panel.browser_host.last_payload_of(stream_ingest_bridge_message_name);
		REQUIRE(second_payload.has_value());
		REQUIRE(*second_payload != *first_payload);
		REQUIRE(second_payload->find("sk_token_two") != std::string::npos);
	}

	SECTION("an identical re-delivery of the same ingest details is not re-published")
	{
		REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_one")).recognised);
		REQUIRE(publisher.publish().stream_ingest_published);

		// The same envelope again. The presenter accepts it and bumps its revision, so
		// the digest comparison is the only thing standing between that and a second
		// crossing.
		REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_one")).recognised);

		REQUIRE_FALSE(publisher.publish().stream_ingest_published);
		REQUIRE(panel.browser_host.count_of(stream_ingest_bridge_message_name) == 1);
	}
}

TEST_CASE("a refused publish is retried rather than remembered", "[bridge][publisher]")
{
	OpenPanel panel;
	DeferredConnectionStatePresenter connection_state;
	StreamPresenter stream_presenter;
	BridgeUiStatePublisher publisher{panel.ui_host, connection_state, stream_presenter};

	panel.browser_host.refuse_everything = true;

	const UiStatePublicationReport refused = publisher.publish();

	REQUIRE_FALSE(refused.agent_response_published);
	REQUIRE(refused.tally.refused == 1);
	REQUIRE(publisher.tally().refused == 1);

	// The revision has been noted, so the stream views are not reconsidered until
	// something moves — which is correct. What must not have happened is the refused
	// payload being remembered as sent.
	//
	// The envelope below is what makes that observable, and the choice of envelope is
	// the whole case: a `stream:rtmps_url` bumps the revision and leaves the response
	// view byte for byte identical to the one CEF refused. So the only thing that can
	// make the response cross on this step is the publisher having declined to
	// remember a refusal. A `stream:agent_response_start` would have changed the
	// payload too, and would have passed either way.
	REQUIRE(stream_presenter.handle(an_ingest_envelope("sk_token_one")).recognised);

	panel.browser_host.refuse_everything = false;

	const UiStatePublicationReport retried = publisher.publish();

	REQUIRE(retried.agent_response_published);
	REQUIRE(retried.stream_ingest_published);
	REQUIRE(panel.browser_host.count_of(agent_response_bridge_message_name) == 1);
}

TEST_CASE("the deferred browser host refuses until a browser is bound", "[bridge][browser]")
{
	DeferredBrowserHost deferred_browser_host;
	RefusingDispatcherSink dispatcher_sink;
	UiHost ui_host{deferred_browser_host, dispatcher_sink};

	const auto application_entry_point = build_local_asset_url("/opt/sesh-ai/ui", "index.html");
	REQUIRE(application_entry_point.valid);

	SECTION("a build with no CEF gets an unbound host and refuses rather than pretending")
	{
		REQUIRE_FALSE(deferred_browser_host.is_bound());

		const auto outcome = ui_host.create_browser(application_entry_point.url);

		REQUIRE_FALSE(outcome.created);
		REQUIRE(outcome.refusal == BridgeRefusalReason::browser_host_refused);
		REQUIRE(deferred_browser_host.refused_operation_count() == 1);
	}

	SECTION("binding makes the same call succeed, through the bound host")
	{
		RecordingBrowserHost browser_host;

		REQUIRE(deferred_browser_host.bind(&browser_host));
		REQUIRE(deferred_browser_host.is_bound());
		REQUIRE(ui_host.create_browser(application_entry_point.url).created);
		REQUIRE(browser_host.loaded_url == application_entry_point.url.value());
		REQUIRE(deferred_browser_host.refused_operation_count() == 0);
	}

	SECTION("a second bind is refused rather than replacing a running browser")
	{
		RecordingBrowserHost first;
		RecordingBrowserHost second;

		REQUIRE(deferred_browser_host.bind(&first));
		REQUIRE_FALSE(deferred_browser_host.bind(&second));

		// Still the first, which is the point — a replaced host would be a CEF browser
		// nothing can reach but which is still running.
		REQUIRE(ui_host.create_browser(application_entry_point.url).created);
		REQUIRE_FALSE(first.loaded_url.empty());
		REQUIRE(second.loaded_url.empty());
	}

	SECTION("a null host is refused")
	{
		REQUIRE_FALSE(deferred_browser_host.bind(nullptr));
		REQUIRE_FALSE(deferred_browser_host.is_bound());
	}

	SECTION("tearing down a session that never opened the panel is not a refusal")
	{
		// Counting it would make every clean shutdown of a session where the producer
		// did not use Sesh look like something went wrong.
		sesh_ai::entry::ShutdownSequence sequence;

		REQUIRE(sequence.record(sesh_ai::entry::ShutdownStep::unregister_timer));
		REQUIRE(sequence.record(sesh_ai::entry::ShutdownStep::destroy_queues));

		REQUIRE(ui_host.shut_down(sequence).cef_shut_down);
		REQUIRE(deferred_browser_host.refused_operation_count() == 0);
	}
}
