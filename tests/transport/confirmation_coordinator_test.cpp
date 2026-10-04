// The Confirmation Coordinator.
//
// Every case here is about one of two harms, and they pull in opposite directions.
//
// **A destructive operation must not be authorised by anything but the producer.** So
// no path may send `confirm:approve` on its own initiative, a decision must agree
// with the envelope carrying it, and an answer must not be sent for a confirmation
// that has already resolved — a producer tapping Approve on a prompt the window
// closed under is the case that produces that, and sending it would have the
// extension authorising an operation nobody is still asking about.
//
// **A producer must never be left unable to tell what happened.** So a resolution
// dismisses the prompt immediately whoever answered, an expiry says nothing was
// changed, and a resolution that contradicts what this client sent is reported rather
// than swallowed. The window goes to the UI for the same reason: a confirmation that
// quietly expires looks like the assistant ignoring them.
//
// The one case that sits between the two is dismissal by identifier. Taking down
// "the current prompt" on a `confirm:resolved` would satisfy the second harm for one
// confirmation and cause it for another — a live prompt for a different destructive
// operation, removed, then expiring unseen. Late and duplicated broadcasts are both
// reachable, so several cases here are about a resolution that matches nothing
// leaving the table exactly as it was.
//
// No REAPER and no CEF. The presenter is substituted, which is what the seam in
// confirmation_coordinator.h exists for.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <transport/confirmation_coordinator.h>
#include <transport/queue_pair.h>

using sesh_ai::transport::AnsweringClient;
using sesh_ai::transport::ConfirmationCoordinator;
using sesh_ai::transport::ConfirmationDecisionDisposition;
using sesh_ai::transport::ConfirmationDecisionOutcome;
using sesh_ai::transport::ConfirmationPresenter;
using sesh_ai::transport::ConfirmationRequestDisposition;
using sesh_ai::transport::ConfirmationRequestOutcome;
using sesh_ai::transport::ConfirmationResolution;
using sesh_ai::transport::ConfirmationResolutionView;
using sesh_ai::transport::ConfirmationResolvedDisposition;
using sesh_ai::transport::ConfirmationResolvedOutcome;
using sesh_ai::transport::ConfirmationRiskLevel;
using sesh_ai::transport::PendingConfirmationView;
using sesh_ai::transport::ProducerVerdict;
using sesh_ai::transport::all_answering_clients;
using sesh_ai::transport::all_confirmation_resolutions;
using sesh_ai::transport::all_confirmation_risk_levels;
using sesh_ai::transport::all_producer_verdicts;
using sesh_ai::transport::answering_client_from_schema_string;
using sesh_ai::transport::confirmation_approve_envelope_type;
using sesh_ai::transport::confirmation_reject_envelope_type;
using sesh_ai::transport::confirmation_request_envelope_type;
using sesh_ai::transport::confirmation_resolution_from_schema_string;
using sesh_ai::transport::confirmation_resolved_envelope_type;
using sesh_ai::transport::confirmation_risk_level_from_schema_string;
using sesh_ai::transport::confirmation_window_seconds;
using sesh_ai::transport::envelope_type_for_verdict;
using sesh_ai::transport::resolution_for_verdict;
using sesh_ai::transport::to_schema_string;

namespace {

	// Stands in for the UI Host: records every prompt raised and every prompt taken
	// down, in order, so a case can assert what the producer would have seen.
	class RecordingConfirmationPresenter final : public ConfirmationPresenter {
	public:
		void present_confirmation(const PendingConfirmationView& pending_confirmation) override
		{
			presented.push_back(pending_confirmation);
		}

		void dismiss_confirmation(const ConfirmationResolutionView& resolution) override
		{
			dismissed.push_back(resolution);
		}

		std::vector<PendingConfirmationView> presented;
		std::vector<ConfirmationResolutionView> dismissed;
	};

	// Stands in for transport/envelope.h's OutboundEnvelope.
	//
	// The real one carries an nlohmann::json payload, and nlohmann/json is an optional
	// dependency of this suite — so the coordinator is templated on the envelope type
	// and this is what the suite instantiates it with. A std::map supports the two
	// payload writes the coordinator performs, and the explicit instantiation in
	// confirmation_coordinator.cpp is what proves the real envelope does too.
	struct StubOutboundEnvelope {
		std::string type;
		std::string request_id;
		std::map<std::string, std::string> payload;
	};

	using StubOutboundQueue = sesh_ai::transport::ConcurrentQueue<StubOutboundEnvelope>;
	using StubConfirmationCoordinator = ConfirmationCoordinator<StubOutboundEnvelope>;

	// Stands in for the nlohmann::json payload of an inbound envelope, for the same
	// dependency reason as StubOutboundEnvelope.
	//
	// It provides only what reading a string field out of a payload uses — is it an
	// object, does it have the field, is that field a string, and its value — so what
	// the cases below exercise is which branch the coordinator takes, not a
	// reimplementation of a JSON library.
	class StubPayload {
	public:
		static StubPayload object_value(std::map<std::string, StubPayload> fields)
		{
			StubPayload payload;
			payload.kind_ = Kind::object_value;
			payload.fields_ = std::move(fields);

			return payload;
		}

		static StubPayload string_value(std::string value)
		{
			StubPayload payload;
			payload.kind_ = Kind::string_value;
			payload.value_ = std::move(value);

			return payload;
		}

		// A field that is present but not a string — what a client sending an enum's
		// index rather than its name would produce.
		static StubPayload number_value()
		{
			StubPayload payload;
			payload.kind_ = Kind::number_value;

			return payload;
		}

		bool is_object() const { return kind_ == Kind::object_value; }
		bool is_string() const { return kind_ == Kind::string_value; }

		bool contains(const std::string& field_name) const
		{
			return fields_.find(field_name) != fields_.end();
		}

		const StubPayload& at(const std::string& field_name) const { return fields_.at(field_name); }

		template <typename ValueType>
		ValueType get() const
		{
			return value_;
		}

	private:
		enum class Kind { object_value, string_value, number_value };

		Kind kind_ = Kind::object_value;
		std::string value_;
		std::map<std::string, StubPayload> fields_;
	};

	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubPayload payload;
	};

	// A well-formed confirm:request as confirmation-request.schema.json spells it:
	// snake_case field names, all three required.
	StubInboundEnvelope confirmation_request(
		std::string request_id,
		std::string action_summary,
		std::string risk_level_name,
		std::string details
	)
	{
		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{confirmation_request_envelope_type};
		inbound_envelope.request_id = std::move(request_id);
		inbound_envelope.payload = StubPayload::object_value({
			{"action_summary", StubPayload::string_value(std::move(action_summary))},
			{"risk_level", StubPayload::string_value(std::move(risk_level_name))},
			{"details", StubPayload::string_value(std::move(details))}
		});

		return inbound_envelope;
	}

	// A confirm:resolved carrying an answering client — what the schema requires for
	// approved and rejected.
	StubInboundEnvelope confirmation_resolved(
		std::string request_id,
		std::string resolution_name,
		std::string answering_client_name
	)
	{
		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{confirmation_resolved_envelope_type};
		inbound_envelope.request_id = request_id;
		inbound_envelope.payload = StubPayload::object_value({
			{"requestId", StubPayload::string_value(std::move(request_id))},
			{"decision", StubPayload::string_value(std::move(resolution_name))},
			{"answeringClient", StubPayload::string_value(std::move(answering_client_name))}
		});

		return inbound_envelope;
	}

	// A confirm:resolved with no answering client — what the schema requires for an
	// expiry, since nothing answered it.
	StubInboundEnvelope confirmation_expired(std::string request_id)
	{
		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{confirmation_resolved_envelope_type};
		inbound_envelope.request_id = request_id;
		inbound_envelope.payload = StubPayload::object_value({
			{"requestId", StubPayload::string_value(std::move(request_id))},
			{"decision", StubPayload::string_value("expired")}
		});

		return inbound_envelope;
	}

	// The three-part fixture every behavioural case needs.
	struct CoordinatorFixture {
		RecordingConfirmationPresenter presenter;
		StubOutboundQueue outbound_queue;
		StubConfirmationCoordinator coordinator{presenter, outbound_queue};
	};

}

TEST_CASE("the confirmation envelope types are the ones the protocol names", "[confirmation]")
{
	// Two inbound and two outbound, and the asymmetry is the protocol: the extension
	// receives requests and resolutions and sends only the producer's answer. The
	// Message Dispatcher routes on the inbound pair and the server matches the
	// outbound pair as strings, so a spelling drift here is a confirmation nobody
	// answers.
	REQUIRE(confirmation_request_envelope_type == "confirm:request");
	REQUIRE(confirmation_resolved_envelope_type == "confirm:resolved");
	REQUIRE(confirmation_approve_envelope_type == "confirm:approve");
	REQUIRE(confirmation_reject_envelope_type == "confirm:reject");

	// envelope.schema.json's `type` pattern is `namespace:action`, with the namespace
	// enumerated. All four are in the `confirm` namespace.
	const std::vector<std::string_view> confirmation_envelope_types{
		confirmation_request_envelope_type,
		confirmation_resolved_envelope_type,
		confirmation_approve_envelope_type,
		confirmation_reject_envelope_type
	};

	for (const std::string_view envelope_type : confirmation_envelope_types) {
		REQUIRE(envelope_type.substr(0, 8) == "confirm:");
	}
}

TEST_CASE("a verdict and the envelope that carries it cannot disagree", "[confirmation]")
{
	// confirm-decision.schema.json refuses a payload that disagrees with itself —
	// confirm:approve carrying decision `rejected` — rather than resolving it by
	// precedence, because neither reading is safe to guess at for an operation the
	// producer was asked about. Both sides come from one switch over the same value,
	// so this pins the pairing rather than hoping for it.
	REQUIRE(envelope_type_for_verdict(ProducerVerdict::approved) == "confirm:approve");
	REQUIRE(to_schema_string(ProducerVerdict::approved) == "approved");

	REQUIRE(envelope_type_for_verdict(ProducerVerdict::rejected) == "confirm:reject");
	REQUIRE(to_schema_string(ProducerVerdict::rejected) == "rejected");

	// And the two verdicts do not collapse onto one envelope type.
	REQUIRE(envelope_type_for_verdict(ProducerVerdict::approved)
		!= envelope_type_for_verdict(ProducerVerdict::rejected));

	// A verdict maps onto the resolution the server will report for it, which is what
	// lets a resolution be compared against what this client sent.
	REQUIRE(resolution_for_verdict(ProducerVerdict::approved) == ConfirmationResolution::approved);
	REQUIRE(resolution_for_verdict(ProducerVerdict::rejected) == ConfirmationResolution::rejected);
}

TEST_CASE("only the two answers a producer can give are representable", "[confirmation]")
{
	// Not an omission: an expiry is the server's own conclusion, broadcast as
	// confirm:resolved and never sent by a client. confirm-decision.schema.json's
	// enum has two values and confirm-resolved.schema.json's has three, and keeping
	// them as separate types is what makes "the extension sends an expiry"
	// unwriteable rather than merely untested.
	REQUIRE(all_producer_verdicts.size() == 2);
	REQUIRE(all_confirmation_resolutions.size() == 3);

	for (const ProducerVerdict verdict : all_producer_verdicts) {
		REQUIRE(to_schema_string(verdict) != "expired");
	}
}

TEST_CASE("the confirmation vocabularies round-trip through their schema spellings", "[confirmation]")
{
	SECTION("risk level")
	{
		for (const ConfirmationRiskLevel risk_level : all_confirmation_risk_levels) {
			const std::optional<ConfirmationRiskLevel> parsed =
				confirmation_risk_level_from_schema_string(to_schema_string(risk_level));

			REQUIRE(parsed.has_value());
			REQUIRE(*parsed == risk_level);
		}

		REQUIRE(to_schema_string(ConfirmationRiskLevel::low) == "low");
		REQUIRE(to_schema_string(ConfirmationRiskLevel::medium) == "medium");
		REQUIRE(to_schema_string(ConfirmationRiskLevel::high) == "high");

		// Nothing outside the closed enum, including the near misses a client
		// paraphrasing rather than quoting the schema would produce.
		REQUIRE_FALSE(confirmation_risk_level_from_schema_string("").has_value());
		REQUIRE_FALSE(confirmation_risk_level_from_schema_string("HIGH").has_value());
		REQUIRE_FALSE(confirmation_risk_level_from_schema_string("critical").has_value());
		REQUIRE_FALSE(confirmation_risk_level_from_schema_string("severe").has_value());
		REQUIRE_FALSE(confirmation_risk_level_from_schema_string("high ").has_value());
	}

	SECTION("resolution")
	{
		for (const ConfirmationResolution resolution : all_confirmation_resolutions) {
			const std::optional<ConfirmationResolution> parsed =
				confirmation_resolution_from_schema_string(to_schema_string(resolution));

			REQUIRE(parsed.has_value());
			REQUIRE(*parsed == resolution);
		}

		REQUIRE(to_schema_string(ConfirmationResolution::approved) == "approved");
		REQUIRE(to_schema_string(ConfirmationResolution::rejected) == "rejected");
		REQUIRE(to_schema_string(ConfirmationResolution::expired) == "expired");

		REQUIRE_FALSE(confirmation_resolution_from_schema_string("").has_value());
		REQUIRE_FALSE(confirmation_resolution_from_schema_string("approve").has_value());
		REQUIRE_FALSE(confirmation_resolution_from_schema_string("timeout").has_value());
		REQUIRE_FALSE(confirmation_resolution_from_schema_string("cancelled").has_value());
	}

	SECTION("answering client")
	{
		for (const AnsweringClient answering_client : all_answering_clients) {
			const std::optional<AnsweringClient> parsed =
				answering_client_from_schema_string(to_schema_string(answering_client));

			REQUIRE(parsed.has_value());
			REQUIRE(*parsed == answering_client);
		}

		// The same two values the Socket.IO handshake's clientType carries, spelled
		// the same way.
		REQUIRE(to_schema_string(AnsweringClient::extension) == "extension");
		REQUIRE(to_schema_string(AnsweringClient::pwa) == "pwa");

		REQUIRE_FALSE(answering_client_from_schema_string("").has_value());
		REQUIRE_FALSE(answering_client_from_schema_string("PWA").has_value());
		REQUIRE_FALSE(answering_client_from_schema_string("mobile").has_value());
		REQUIRE_FALSE(answering_client_from_schema_string("reaper").has_value());
	}
}

TEST_CASE("the server's confirmation window is displayed, not enforced", "[confirmation]")
{
	// Requirement 13.4. The number travels on the view rather than being left for the
	// UI to reach back for, so the prompt and its deadline cross the bridge together.
	// The extension runs no timer of its own — the only thing that ends a
	// confirmation here is a confirm:resolved arriving.
	REQUIRE(confirmation_window_seconds == 120);

	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1",
		"Delete the two takes on Lead Vocal",
		"high",
		"Removes takes 2 and 3 from Lead Vocal. This cannot be undone from REAPER's history."
	);

	REQUIRE(fixture.presenter.presented.size() == 1);
	REQUIRE(fixture.presenter.presented.front().confirmation_window_seconds == 120);
}

TEST_CASE("a confirm:request renders the summary, risk level, and details", "[confirmation]")
{
	// Requirement 13.1, and the whole of confirmation-request.schema.json — three
	// required fields, additionalProperties false.
	CoordinatorFixture fixture;

	const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
		"confirmation-1",
		"Delete four items on Drum Bus",
		"medium",
		"Removes the four items between 1:04 and 1:32 on Drum Bus."
	);

	REQUIRE(outcome.disposition == ConfirmationRequestDisposition::presented);
	REQUIRE(outcome.tracked);
	REQUIRE(outcome.presented);
	REQUIRE(outcome.description_complete);
	REQUIRE(outcome.risk_level_reported);
	REQUIRE(outcome.risk_level == ConfirmationRiskLevel::medium);
	REQUIRE(outcome.request_id == "confirmation-1");

	REQUIRE(fixture.presenter.presented.size() == 1);

	const PendingConfirmationView& view = fixture.presenter.presented.front();

	REQUIRE(view.request_id == "confirmation-1");
	REQUIRE(view.action_summary == "Delete four items on Drum Bus");
	REQUIRE(view.risk_level == ConfirmationRiskLevel::medium);
	REQUIRE(view.risk_level_reported);
	REQUIRE(view.details == "Removes the four items between 1:04 and 1:32 on Drum Bus.");
	REQUIRE(view.description_complete);

	// Tracked by requestId, which is the pending-request table design.md makes this
	// component's stated exception to keeping none (requirement 23.7).
	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 1);
	REQUIRE(fixture.coordinator.awaiting_producer_count() == 1);
	REQUIRE(fixture.coordinator.is_tracked("confirmation-1"));
	REQUIRE(fixture.coordinator.awaits_producer("confirmation-1"));
	REQUIRE_FALSE(fixture.coordinator.local_verdict_for("confirmation-1").has_value());

	// Nothing goes out until the producer answers. A confirmation the extension
	// replied to on its own initiative would be a destructive operation authorised by
	// code.
	REQUIRE(fixture.outbound_queue.empty());
}

TEST_CASE("every risk level reaches the UI as the agent assessed it", "[confirmation]")
{
	// The risk level is the one field that changes how carefully a producer reads the
	// rest, so a level that arrives as `low` and renders as anything else is a
	// producer misinformed about their own session.
	for (const ConfirmationRiskLevel risk_level : all_confirmation_risk_levels) {
		CoordinatorFixture fixture;

		const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
			"confirmation-1",
			"Rename the Guitar folder",
			to_schema_string(risk_level),
			"Renames the folder track and nothing inside it."
		);

		REQUIRE(outcome.risk_level == risk_level);
		REQUIRE(outcome.risk_level_reported);
		REQUIRE(fixture.presenter.presented.front().risk_level == risk_level);
		REQUIRE(fixture.presenter.presented.front().risk_level_reported);
	}
}

TEST_CASE("an unreadable risk level is presented as the most damaging one", "[confirmation][errors]")
{
	// confirmation-request.schema.json's `risk_level` enum is closed and the Envelope
	// Codec refuses a payload that fails it, so arriving here means the codec was
	// bypassed or the server's protocol is ahead of this build.
	//
	// The conservative direction is the safe one. Understating the risk of a
	// destructive operation is what gets a producer to approve something they would
	// otherwise have read twice — so an unknown level presents as `high`, flagged as
	// this component's default rather than the agent's assessment so the UI can say so
	// instead of presenting a guess as a fact.
	const std::vector<std::string> unreadable_risk_levels{"", "critical", "HIGH", "severe", "none"};

	for (const std::string& unreadable_risk_level : unreadable_risk_levels) {
		CoordinatorFixture fixture;

		const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
			"confirmation-1",
			"Delete the Reverb Bus track",
			unreadable_risk_level,
			"Removes the track and the three sends routed into it."
		);

		REQUIRE(outcome.risk_level == ConfirmationRiskLevel::high);
		REQUIRE_FALSE(outcome.risk_level_reported);
		REQUIRE_FALSE(outcome.description_complete);

		// Still presented. Withholding the prompt would leave the producer with a
		// turn that stalls for 120 seconds and expires — the appearance requirement
		// 13.4 exists to prevent.
		REQUIRE(outcome.presented);
		REQUIRE(fixture.presenter.presented.size() == 1);
		REQUIRE(fixture.presenter.presented.front().risk_level == ConfirmationRiskLevel::high);
		REQUIRE_FALSE(fixture.presenter.presented.front().risk_level_reported);
		REQUIRE_FALSE(fixture.presenter.presented.front().description_complete);
	}
}

TEST_CASE("an incomplete description is presented and reported as incomplete", "[confirmation][errors]")
{
	// All three fields are required with minLength 1, so none of these should reach a
	// handler. When one does, the producer is being asked to authorise something the
	// extension could not fully describe — which the UI has to be told about rather
	// than left to render a blank prompt with a working Approve button (task 19.3).
	//
	// The coordinator does not answer on the producer's behalf in any of these cases.
	// Auto-rejecting would be the extension deciding, and deciding is the one thing a
	// confirmation exists to stop it doing.
	SECTION("no action summary")
	{
		CoordinatorFixture fixture;

		const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
			"confirmation-1", "", "high", "Removes the four items on Drum Bus."
		);

		REQUIRE(outcome.presented);
		REQUIRE_FALSE(outcome.description_complete);
		REQUIRE(fixture.presenter.presented.front().action_summary.empty());
		REQUIRE(fixture.outbound_queue.empty());
	}

	SECTION("no details")
	{
		CoordinatorFixture fixture;

		const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "high", ""
		);

		REQUIRE(outcome.presented);
		REQUIRE_FALSE(outcome.description_complete);
		REQUIRE(fixture.presenter.presented.front().details.empty());
		REQUIRE(fixture.outbound_queue.empty());
	}
}

TEST_CASE("a confirmation with no correlation identifier raises no prompt", "[confirmation][errors]")
{
	// The one case where declining to show something beats showing it.
	// confirm-decision.schema.json requires a requestId of at least one character, so
	// neither answer could be sent — a prompt whose buttons cannot produce a valid
	// answer would have the producer tap Approve and watch the operation stall for 120
	// seconds and expire.
	CoordinatorFixture fixture;

	const ConfirmationRequestOutcome outcome = fixture.coordinator.present_confirmation_request(
		"", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	REQUIRE(outcome.disposition == ConfirmationRequestDisposition::unaddressable);
	REQUIRE_FALSE(outcome.tracked);
	REQUIRE_FALSE(outcome.presented);
	REQUIRE(outcome.request_id.empty());

	REQUIRE(fixture.presenter.presented.empty());
	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
	REQUIRE(fixture.outbound_queue.empty());

	// And nothing can be answered under an empty identifier afterwards, so the
	// unaddressable request cannot be reached by an answer that happens to carry no
	// identifier either.
	const ConfirmationDecisionOutcome decision = fixture.coordinator.approve("");

	REQUIRE(decision.disposition == ConfirmationDecisionDisposition::not_tracked);
	REQUIRE_FALSE(decision.decision_queued);
	REQUIRE(fixture.outbound_queue.empty());
}

TEST_CASE("approving sends confirm:approve with the correlating requestId", "[confirmation]")
{
	// Requirement 13.2. The payload is requestId and decision and nothing else, which
	// is the whole of confirm-decision.schema.json — additionalProperties is false.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);

	const ConfirmationDecisionOutcome outcome = fixture.coordinator.approve("confirmation-1");

	REQUIRE(outcome.disposition == ConfirmationDecisionDisposition::sent);
	REQUIRE(outcome.verdict == ProducerVerdict::approved);
	REQUIRE(outcome.decision_queued);
	REQUIRE(outcome.envelope_type == "confirm:approve");
	REQUIRE(outcome.request_id == "confirmation-1");

	REQUIRE(fixture.outbound_queue.size() == 1);

	const std::optional<StubOutboundEnvelope> decision_envelope = fixture.outbound_queue.try_pop();

	REQUIRE(decision_envelope.has_value());
	REQUIRE(decision_envelope->type == "confirm:approve");
	REQUIRE(decision_envelope->request_id == "confirmation-1");
	REQUIRE(decision_envelope->payload.size() == 2);
	REQUIRE(decision_envelope->payload.at("requestId") == "confirmation-1");

	// Agreeing with the envelope type, which is what the schema refuses to resolve by
	// precedence.
	REQUIRE(decision_envelope->payload.at("decision") == "approved");

	// The prompt comes down on the tap rather than on the server's resolution. A
	// prompt that lingers through a network round trip reads as one that did not
	// register.
	REQUIRE(outcome.prompt_dismissed);
	REQUIRE(fixture.presenter.dismissed.size() == 1);
	REQUIRE(fixture.presenter.dismissed.front().request_id == "confirmation-1");
	REQUIRE(fixture.presenter.dismissed.front().answered_by_this_client);
	REQUIRE(fixture.presenter.dismissed.front().answering_client == AnsweringClient::extension);
	REQUIRE(fixture.presenter.dismissed.front().local_verdict == ProducerVerdict::approved);
	REQUIRE(fixture.presenter.dismissed.front().resolution == ConfirmationResolution::approved);

	// An approval is the one outcome that does not let the UI say nothing changed.
	REQUIRE_FALSE(fixture.presenter.dismissed.front().nothing_was_changed);

	// Still tracked, so the resolution can be recognised as the answer to this
	// client's own decision.
	REQUIRE(fixture.coordinator.is_tracked("confirmation-1"));
	REQUIRE_FALSE(fixture.coordinator.awaits_producer("confirmation-1"));
	REQUIRE(fixture.coordinator.local_verdict_for("confirmation-1") == ProducerVerdict::approved);
	REQUIRE(fixture.coordinator.awaiting_producer_count() == 0);
}

TEST_CASE("rejecting sends confirm:reject with the correlating requestId", "[confirmation]")
{
	// Requirement 13.3.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-7", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	const ConfirmationDecisionOutcome outcome = fixture.coordinator.reject("confirmation-7");

	REQUIRE(outcome.disposition == ConfirmationDecisionDisposition::sent);
	REQUIRE(outcome.verdict == ProducerVerdict::rejected);
	REQUIRE(outcome.envelope_type == "confirm:reject");

	const std::optional<StubOutboundEnvelope> decision_envelope = fixture.outbound_queue.try_pop();

	REQUIRE(decision_envelope.has_value());
	REQUIRE(decision_envelope->type == "confirm:reject");
	REQUIRE(decision_envelope->request_id == "confirmation-7");
	REQUIRE(decision_envelope->payload.size() == 2);
	REQUIRE(decision_envelope->payload.at("requestId") == "confirmation-7");
	REQUIRE(decision_envelope->payload.at("decision") == "rejected");

	// A rejection establishes that the session was not touched, which is what the UI
	// tells the producer.
	REQUIRE(fixture.presenter.dismissed.size() == 1);
	REQUIRE(fixture.presenter.dismissed.front().nothing_was_changed);
	REQUIRE(fixture.presenter.dismissed.front().local_verdict == ProducerVerdict::rejected);
}

TEST_CASE("an answer for a confirmation nobody is tracking is not sent", "[confirmation][errors]")
{
	// The load-bearing refusal. A requestId that is not tracked either was never seen
	// or has already resolved — and a decision for a resolved confirmation would have
	// the extension authorising a destructive operation whose window has closed.
	CoordinatorFixture fixture;

	SECTION("never seen")
	{
		const ConfirmationDecisionOutcome outcome = fixture.coordinator.approve("confirmation-nobody-sent");

		REQUIRE(outcome.disposition == ConfirmationDecisionDisposition::not_tracked);
		REQUIRE_FALSE(outcome.decision_queued);
		REQUIRE_FALSE(outcome.prompt_dismissed);
		REQUIRE(outcome.envelope_type.empty());
		REQUIRE(fixture.outbound_queue.empty());
		REQUIRE(fixture.presenter.dismissed.empty());
	}

	SECTION("already resolved, and the producer tapped as the window closed")
	{
		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		REQUIRE(fixture.coordinator.resolve_confirmation("confirmation-1", "expired", "").stopped_tracking);

		const ConfirmationDecisionOutcome outcome = fixture.coordinator.approve("confirmation-1");

		REQUIRE(outcome.disposition == ConfirmationDecisionDisposition::not_tracked);
		REQUIRE_FALSE(outcome.decision_queued);

		// Nothing on the wire. This is the case that matters: the producer believes
		// they approved, and the extension must not tell the server they did.
		REQUIRE(fixture.outbound_queue.empty());
	}
}

TEST_CASE("a second tap on the same confirmation sends one decision", "[confirmation]")
{
	// Two decisions for one requestId is a race the server should not be asked to
	// arbitrate, and the second tap is ordinary — a producer who did not see the
	// prompt come down.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);

	REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);

	const ConfirmationDecisionOutcome second_tap = fixture.coordinator.approve("confirmation-1");

	REQUIRE(second_tap.disposition == ConfirmationDecisionDisposition::already_answered);
	REQUIRE_FALSE(second_tap.decision_queued);
	REQUIRE_FALSE(second_tap.prompt_dismissed);
	REQUIRE(fixture.outbound_queue.size() == 1);
	REQUIRE(fixture.presenter.dismissed.size() == 1);

	SECTION("and a contradicting second tap does not overturn the first")
	{
		const ConfirmationDecisionOutcome contradicting_tap = fixture.coordinator.reject("confirmation-1");

		REQUIRE(contradicting_tap.disposition == ConfirmationDecisionDisposition::already_answered);
		REQUIRE_FALSE(contradicting_tap.decision_queued);
		REQUIRE(fixture.outbound_queue.size() == 1);
		REQUIRE(fixture.coordinator.local_verdict_for("confirmation-1") == ProducerVerdict::approved);
	}
}

TEST_CASE("a second confirmation while one is pending leaves both answerable", "[confirmation]")
{
	// Reachable: two turns, a reconnect that re-delivers, or a server that asks about
	// two operations. Replacing the first would leave a confirmation pending
	// server-side that the producer can no longer answer, which then expires — looking
	// exactly like the assistant ignoring them, the appearance requirement 13.4 exists
	// to prevent. So the table holds both, in arrival order, and each is answered under
	// its own identifier.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);
	fixture.coordinator.present_confirmation_request(
		"confirmation-2", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 2);
	REQUIRE(fixture.coordinator.awaiting_producer_count() == 2);
	REQUIRE(fixture.coordinator.tracked_request_ids()
		== std::vector<std::string>{"confirmation-1", "confirmation-2"});
	REQUIRE(fixture.presenter.presented.size() == 2);

	// Answering the second must not touch the first.
	REQUIRE(fixture.coordinator.reject("confirmation-2").decision_queued);

	REQUIRE(fixture.coordinator.awaits_producer("confirmation-1"));
	REQUIRE_FALSE(fixture.coordinator.awaits_producer("confirmation-2"));

	const std::optional<StubOutboundEnvelope> decision_envelope = fixture.outbound_queue.try_pop();

	REQUIRE(decision_envelope.has_value());
	REQUIRE(decision_envelope->type == "confirm:reject");
	REQUIRE(decision_envelope->request_id == "confirmation-2");
	REQUIRE(decision_envelope->payload.at("requestId") == "confirmation-2");

	REQUIRE(fixture.presenter.dismissed.size() == 1);
	REQUIRE(fixture.presenter.dismissed.front().request_id == "confirmation-2");

	// And the first is still answerable, with its own identifier on the wire.
	REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);

	const std::optional<StubOutboundEnvelope> second_decision = fixture.outbound_queue.try_pop();

	REQUIRE(second_decision.has_value());
	REQUIRE(second_decision->type == "confirm:approve");
	REQUIRE(second_decision->request_id == "confirmation-1");
}

TEST_CASE("a re-delivered confirmation refreshes its prompt rather than stacking one", "[confirmation]")
{
	// Same requestId, same operation. A second entry would give the producer two
	// prompts for one destructive operation, and answering one would leave the other
	// showing.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);

	const ConfirmationRequestOutcome redelivery = fixture.coordinator.present_confirmation_request(
		"confirmation-1",
		"Delete four items on Drum Bus",
		"high",
		"Removes the four items between 1:04 and 1:32 on Drum Bus."
	);

	REQUIRE(redelivery.disposition == ConfirmationRequestDisposition::redelivered);
	REQUIRE(redelivery.tracked);
	REQUIRE(redelivery.presented);

	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 1);
	REQUIRE(fixture.coordinator.awaiting_producer_count() == 1);

	// Refreshed in place: the UI is handed the newer description, and one identifier
	// remains.
	REQUIRE(fixture.presenter.presented.size() == 2);
	REQUIRE(fixture.presenter.presented.back().risk_level == ConfirmationRiskLevel::high);
	REQUIRE(fixture.presenter.presented.back().details
		== "Removes the four items between 1:04 and 1:32 on Drum Bus.");

	const PendingConfirmationView* const tracked_view =
		fixture.coordinator.pending_confirmation_view("confirmation-1");

	REQUIRE(tracked_view != nullptr);
	REQUIRE(tracked_view->risk_level == ConfirmationRiskLevel::high);

	SECTION("and a re-delivery after the producer answered does not unanswer it")
	{
		REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);

		const ConfirmationRequestOutcome late_redelivery = fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "high", "Removes four items."
		);

		REQUIRE(late_redelivery.disposition == ConfirmationRequestDisposition::redelivered);
		REQUIRE(fixture.coordinator.local_verdict_for("confirmation-1") == ProducerVerdict::approved);
		REQUIRE(fixture.coordinator.awaiting_producer_count() == 0);

		// And no second decision goes out on the strength of the repeat.
		REQUIRE(fixture.outbound_queue.size() == 1);
	}
}

TEST_CASE("a confirmation the phone answered is dismissed immediately", "[confirmation]")
{
	// Requirement 13.5, and the reason it says immediately. The confirmation goes to
	// every client in the session, so the PWA may answer first — and the producer
	// standing at REAPER has to see their prompt go away and be told who answered,
	// rather than watching a prompt for a decision that has already been made.
	for (const ConfirmationResolution resolution :
		{ConfirmationResolution::approved, ConfirmationResolution::rejected}) {
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		const ConfirmationResolvedOutcome outcome = fixture.coordinator.resolve_confirmation(
			"confirmation-1", to_schema_string(resolution), "pwa"
		);

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::dismissed);
		REQUIRE(outcome.resolution == resolution);
		REQUIRE(outcome.prompt_dismissed);
		REQUIRE(outcome.stopped_tracking);
		REQUIRE_FALSE(outcome.answering_client_contradicted_expiry);

		REQUIRE(fixture.presenter.dismissed.size() == 1);

		const ConfirmationResolutionView& view = fixture.presenter.dismissed.front();

		REQUIRE(view.request_id == "confirmation-1");
		REQUIRE(view.resolution == resolution);
		REQUIRE(view.answering_client == AnsweringClient::pwa);
		REQUIRE_FALSE(view.answered_by_this_client);
		REQUIRE_FALSE(view.local_verdict.has_value());
		REQUIRE_FALSE(view.contradicts_local_verdict);
		REQUIRE(view.nothing_was_changed == (resolution == ConfirmationResolution::rejected));

		// Tracking stops, so the prompt cannot be answered afterwards.
		REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
		REQUIRE_FALSE(fixture.coordinator.is_tracked("confirmation-1"));
		REQUIRE(fixture.outbound_queue.empty());
	}
}

TEST_CASE("an expired confirmation is cleared and reported as changing nothing", "[confirmation][errors]")
{
	// Requirements 13.5 and 23.7. The expiry is the server's conclusion — the
	// extension runs no timer and reaches it only by being told — and what the producer
	// needs to hear is that their session was not touched.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	const ConfirmationResolvedOutcome outcome =
		fixture.coordinator.resolve_confirmation("confirmation-1", "expired", "");

	REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::dismissed);
	REQUIRE(outcome.resolution == ConfirmationResolution::expired);
	REQUIRE(outcome.prompt_dismissed);
	REQUIRE(outcome.stopped_tracking);

	REQUIRE(fixture.presenter.dismissed.size() == 1);

	const ConfirmationResolutionView& view = fixture.presenter.dismissed.front();

	REQUIRE(view.resolution == ConfirmationResolution::expired);
	REQUIRE(view.nothing_was_changed);

	// No answering client, because nothing answered — which is what
	// confirm-resolved.schema.json's if/then/else forbids the server from sending.
	REQUIRE_FALSE(view.answering_client.has_value());
	REQUIRE_FALSE(view.answered_by_this_client);
}

TEST_CASE("an expiry that names an answering client has the client dropped", "[confirmation][errors]")
{
	// confirm-resolved.schema.json forbids answeringClient when the decision is
	// expired, via if/then/else, so the two cannot come apart on the wire. If one
	// arrives anyway, `decision` is the required field and is trusted, and the client
	// is dropped — telling the producer their phone approved a confirmation that in
	// fact expired is the reading that misinforms them about whether their session was
	// touched.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	const ConfirmationResolvedOutcome outcome =
		fixture.coordinator.resolve_confirmation("confirmation-1", "expired", "pwa");

	REQUIRE(outcome.resolution == ConfirmationResolution::expired);
	REQUIRE(outcome.answering_client_contradicted_expiry);
	REQUIRE(outcome.prompt_dismissed);

	const ConfirmationResolutionView& view = fixture.presenter.dismissed.front();

	REQUIRE(view.resolution == ConfirmationResolution::expired);
	REQUIRE_FALSE(view.answering_client.has_value());
	REQUIRE(view.nothing_was_changed);
}

TEST_CASE("a resolution missing its answering client still dismisses the prompt", "[confirmation][errors]")
{
	// confirm-resolved.schema.json requires answeringClient for approved and rejected,
	// so this is protocol drift. Dismissal must not depend on the payload being
	// perfect: the confirmation is demonstrably no longer pending, and a prompt left
	// up for it is a prompt whose buttons the server will refuse.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);

	const ConfirmationResolvedOutcome outcome =
		fixture.coordinator.resolve_confirmation("confirmation-1", "approved", "");

	REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::dismissed);
	REQUIRE(outcome.resolution == ConfirmationResolution::approved);
	REQUIRE(outcome.prompt_dismissed);
	REQUIRE(outcome.stopped_tracking);
	REQUIRE_FALSE(outcome.answering_client_contradicted_expiry);

	// The outcome is reported; who answered is not claimed.
	const ConfirmationResolutionView& view = fixture.presenter.dismissed.front();

	REQUIRE(view.resolution == ConfirmationResolution::approved);
	REQUIRE_FALSE(view.answering_client.has_value());
}

TEST_CASE("a resolution this build cannot read dismisses without naming an outcome", "[confirmation][errors]")
{
	// An unknown `decision` means the server's protocol is ahead of this build. The
	// prompt still comes down, because the confirmation is no longer pending whatever
	// the word means — but naming an outcome would be inventing one, so the UI is told
	// the confirmation ended without being told how, and nothing_was_changed stays
	// false because that would be a claim rather than a reading.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);

	const ConfirmationResolvedOutcome outcome =
		fixture.coordinator.resolve_confirmation("confirmation-1", "superseded", "pwa");

	REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::dismissed);
	REQUIRE_FALSE(outcome.resolution.has_value());
	REQUIRE(outcome.prompt_dismissed);
	REQUIRE(outcome.stopped_tracking);

	const ConfirmationResolutionView& view = fixture.presenter.dismissed.front();

	REQUIRE_FALSE(view.resolution.has_value());
	REQUIRE_FALSE(view.nothing_was_changed);
	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
}

TEST_CASE("a resolution for something untracked changes nothing", "[confirmation][errors]")
{
	// Both halves of this are reachable, and the second is the one that would do
	// damage. A late or duplicated broadcast finds nothing to dismiss; dismissing
	// whatever prompt happens to be showing instead would take down a live one for a
	// different destructive operation, which would then expire with the producer never
	// having seen it.
	SECTION("nothing is tracked at all")
	{
		CoordinatorFixture fixture;

		const ConfirmationResolvedOutcome outcome =
			fixture.coordinator.resolve_confirmation("confirmation-nobody-sent", "expired", "");

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::not_tracked);
		REQUIRE_FALSE(outcome.prompt_dismissed);
		REQUIRE_FALSE(outcome.stopped_tracking);

		// The resolution is still decoded, so a log line can say what arrived.
		REQUIRE(outcome.resolution == ConfirmationResolution::expired);
		REQUIRE(fixture.presenter.dismissed.empty());
	}

	SECTION("a different confirmation is pending and must survive")
	{
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		const ConfirmationResolvedOutcome outcome =
			fixture.coordinator.resolve_confirmation("confirmation-2", "approved", "pwa");

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::not_tracked);
		REQUIRE_FALSE(outcome.prompt_dismissed);
		REQUIRE(fixture.presenter.dismissed.empty());

		// Untouched, and still answerable.
		REQUIRE(fixture.coordinator.tracked_confirmation_count() == 1);
		REQUIRE(fixture.coordinator.awaits_producer("confirmation-1"));
		REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);
	}

	SECTION("a duplicated broadcast dismisses once")
	{
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		REQUIRE(fixture.coordinator.resolve_confirmation("confirmation-1", "rejected", "pwa").prompt_dismissed);

		const ConfirmationResolvedOutcome duplicate =
			fixture.coordinator.resolve_confirmation("confirmation-1", "rejected", "pwa");

		REQUIRE(duplicate.disposition == ConfirmationResolvedDisposition::not_tracked);
		REQUIRE_FALSE(duplicate.prompt_dismissed);
		REQUIRE(fixture.presenter.dismissed.size() == 1);
	}

	SECTION("a resolution with no identifier at all")
	{
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		const ConfirmationResolvedOutcome outcome =
			fixture.coordinator.resolve_confirmation("", "expired", "");

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::not_tracked);
		REQUIRE_FALSE(outcome.prompt_dismissed);
		REQUIRE(fixture.coordinator.awaits_producer("confirmation-1"));
		REQUIRE(fixture.presenter.dismissed.empty());
	}
}

TEST_CASE("the server agreeing with this client's answer is not re-announced", "[confirmation]")
{
	// confirm-resolved.schema.json's own note: the client that answered already knows
	// and ignores the broadcast. The prompt came down when the producer tapped, so
	// calling the UI again would re-announce an outcome they just chose — and noise
	// around destructive operations trains people to stop reading. Tracking still
	// stops, which is what settles the table.
	for (const ProducerVerdict verdict : all_producer_verdicts) {
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		REQUIRE(fixture.coordinator.answer("confirmation-1", verdict).decision_queued);
		REQUIRE(fixture.presenter.dismissed.size() == 1);

		const ConfirmationResolvedOutcome outcome = fixture.coordinator.resolve_confirmation(
			"confirmation-1", to_schema_string(resolution_for_verdict(verdict)), "extension"
		);

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::confirmed_local_answer);
		REQUIRE_FALSE(outcome.prompt_dismissed);
		REQUIRE(outcome.stopped_tracking);

		// Still one dismissal — the one the tap produced.
		REQUIRE(fixture.presenter.dismissed.size() == 1);
		REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
	}
}

TEST_CASE("a resolution contradicting this client's answer is reported", "[confirmation][errors]")
{
	// The producer tapping Approve as the window closed. They believe the operation is
	// running; it is not, and nobody else is going to tell them. Requirement 23.7's
	// "indicate that nothing was changed" in the case where it is least obvious and
	// most needed.
	SECTION("approved here, expired server-side")
	{
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);

		const ConfirmationResolvedOutcome outcome =
			fixture.coordinator.resolve_confirmation("confirmation-1", "expired", "");

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::contradicted_local_answer);
		REQUIRE(outcome.resolution == ConfirmationResolution::expired);
		REQUIRE(outcome.prompt_dismissed);
		REQUIRE(outcome.stopped_tracking);

		// Two calls to the UI: the dismissal on the tap, then the correction.
		REQUIRE(fixture.presenter.dismissed.size() == 2);

		const ConfirmationResolutionView& correction = fixture.presenter.dismissed.back();

		REQUIRE(correction.request_id == "confirmation-1");
		REQUIRE(correction.resolution == ConfirmationResolution::expired);
		REQUIRE(correction.contradicts_local_verdict);
		REQUIRE(correction.local_verdict == ProducerVerdict::approved);
		REQUIRE(correction.answered_by_this_client);
		REQUIRE(correction.nothing_was_changed);
		REQUIRE_FALSE(correction.answering_client.has_value());
	}

	SECTION("approved here, rejected on the phone")
	{
		CoordinatorFixture fixture;

		fixture.coordinator.present_confirmation_request(
			"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
		);

		REQUIRE(fixture.coordinator.approve("confirmation-1").decision_queued);

		const ConfirmationResolvedOutcome outcome =
			fixture.coordinator.resolve_confirmation("confirmation-1", "rejected", "pwa");

		REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::contradicted_local_answer);
		REQUIRE(fixture.presenter.dismissed.size() == 2);

		const ConfirmationResolutionView& correction = fixture.presenter.dismissed.back();

		REQUIRE(correction.resolution == ConfirmationResolution::rejected);
		REQUIRE(correction.answering_client == AnsweringClient::pwa);
		REQUIRE(correction.contradicts_local_verdict);
		REQUIRE(correction.local_verdict == ProducerVerdict::approved);
		REQUIRE(correction.nothing_was_changed);
	}
}

TEST_CASE("abandoning the table takes every prompt down", "[confirmation][errors]")
{
	// The reconnect path (requirement 23.1). Resolution broadcasts for anything
	// pending across a dropped socket are gone, so without this the table holds
	// entries that never resolve and the producer keeps prompts that can no longer be
	// answered. No resolution is claimed, because what happened to those
	// confirmations server-side is not known here.
	CoordinatorFixture fixture;

	fixture.coordinator.present_confirmation_request(
		"confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items."
	);
	fixture.coordinator.present_confirmation_request(
		"confirmation-2", "Delete the Reverb Bus track", "high", "Removes the track and its three sends."
	);

	REQUIRE(fixture.coordinator.approve("confirmation-2").decision_queued);
	REQUIRE(fixture.presenter.dismissed.size() == 1);

	REQUIRE(fixture.coordinator.abandon_all_tracked_confirmations() == 2);

	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
	REQUIRE(fixture.coordinator.awaiting_producer_count() == 0);
	REQUIRE(fixture.coordinator.tracked_request_ids().empty());

	// One dismissal for the tap, then one per abandoned entry, oldest first.
	REQUIRE(fixture.presenter.dismissed.size() == 3);
	REQUIRE(fixture.presenter.dismissed[1].request_id == "confirmation-1");
	REQUIRE_FALSE(fixture.presenter.dismissed[1].resolution.has_value());
	REQUIRE_FALSE(fixture.presenter.dismissed[1].answered_by_this_client);
	REQUIRE(fixture.presenter.dismissed[2].request_id == "confirmation-2");
	REQUIRE_FALSE(fixture.presenter.dismissed[2].resolution.has_value());
	REQUIRE(fixture.presenter.dismissed[2].answered_by_this_client);

	// And nothing is answerable afterwards.
	REQUIRE(fixture.coordinator.approve("confirmation-1").disposition
		== ConfirmationDecisionDisposition::not_tracked);
	REQUIRE(fixture.outbound_queue.size() == 1);
	REQUIRE(fixture.coordinator.abandon_all_tracked_confirmations() == 0);
}

TEST_CASE("the confirm:request entry point reads the payload the schema defines", "[confirmation]")
{
	// What the Message Dispatcher (task 14.1) calls. The field names are snake_case,
	// matching the harness inline function input schema that authors this payload —
	// unlike the rest of the protocol, which is camelCase.
	CoordinatorFixture fixture;

	const ConfirmationRequestOutcome outcome = fixture.coordinator.handle_confirmation_request(
		confirmation_request(
			"confirmation-1",
			"Delete the two takes on Lead Vocal",
			"high",
			"Removes takes 2 and 3 from Lead Vocal."
		)
	);

	REQUIRE(outcome.disposition == ConfirmationRequestDisposition::presented);
	REQUIRE(outcome.request_id == "confirmation-1");
	REQUIRE(outcome.risk_level == ConfirmationRiskLevel::high);
	REQUIRE(outcome.risk_level_reported);
	REQUIRE(outcome.description_complete);

	REQUIRE(fixture.presenter.presented.size() == 1);

	const PendingConfirmationView& view = fixture.presenter.presented.front();

	REQUIRE(view.action_summary == "Delete the two takes on Lead Vocal");
	REQUIRE(view.details == "Removes takes 2 and 3 from Lead Vocal.");
	REQUIRE(view.confirmation_window_seconds == 120);

	SECTION("and a malformed payload takes the incomplete-description path")
	{
		// The Envelope Codec refuses a payload failing
		// confirmation-request.schema.json before it reaches here (requirement 4.3),
		// so reading defensively anyway is what keeps a malformed one from throwing
		// out of the main-thread tick.
		CoordinatorFixture malformed_fixture;

		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{confirmation_request_envelope_type};
		inbound_envelope.request_id = "confirmation-2";
		inbound_envelope.payload = StubPayload::object_value({
			{"action_summary", StubPayload::number_value()},
			{"details", StubPayload::string_value("Removes four items.")}
		});

		const ConfirmationRequestOutcome malformed_outcome =
			malformed_fixture.coordinator.handle_confirmation_request(inbound_envelope);

		REQUIRE(malformed_outcome.presented);
		REQUIRE_FALSE(malformed_outcome.description_complete);
		REQUIRE_FALSE(malformed_outcome.risk_level_reported);
		REQUIRE(malformed_outcome.risk_level == ConfirmationRiskLevel::high);
	}

	SECTION("and a payload that is not an object at all")
	{
		CoordinatorFixture scalar_fixture;

		StubInboundEnvelope inbound_envelope;
		inbound_envelope.type = std::string{confirmation_request_envelope_type};
		inbound_envelope.request_id = "confirmation-3";
		inbound_envelope.payload = StubPayload::string_value("delete everything");

		const ConfirmationRequestOutcome scalar_outcome =
			scalar_fixture.coordinator.handle_confirmation_request(inbound_envelope);

		REQUIRE(scalar_outcome.presented);
		REQUIRE_FALSE(scalar_outcome.description_complete);
		REQUIRE(scalar_fixture.presenter.presented.front().action_summary.empty());
		REQUIRE(scalar_fixture.outbound_queue.empty());
	}
}

TEST_CASE("the confirm:resolved entry point reads the payload the schema defines", "[confirmation]")
{
	// The other half of what the Message Dispatcher calls. These field names are
	// camelCase, like the rest of the protocol — the payload is assembled by the
	// server rather than authored by the model.
	CoordinatorFixture fixture;

	fixture.coordinator.handle_confirmation_request(
		confirmation_request("confirmation-1", "Delete four items on Drum Bus", "medium", "Removes four items.")
	);

	const ConfirmationResolvedOutcome outcome = fixture.coordinator.handle_confirmation_resolved(
		confirmation_resolved("confirmation-1", "approved", "pwa")
	);

	REQUIRE(outcome.disposition == ConfirmationResolvedDisposition::dismissed);
	REQUIRE(outcome.resolution == ConfirmationResolution::approved);
	REQUIRE(outcome.prompt_dismissed);
	REQUIRE(fixture.presenter.dismissed.size() == 1);
	REQUIRE(fixture.presenter.dismissed.front().answering_client == AnsweringClient::pwa);

	SECTION("an expiry arrives with no answeringClient field at all")
	{
		CoordinatorFixture expiry_fixture;

		expiry_fixture.coordinator.handle_confirmation_request(
			confirmation_request("confirmation-9", "Delete the Reverb Bus track", "high", "Removes the track.")
		);

		const ConfirmationResolvedOutcome expiry_outcome =
			expiry_fixture.coordinator.handle_confirmation_resolved(confirmation_expired("confirmation-9"));

		REQUIRE(expiry_outcome.disposition == ConfirmationResolvedDisposition::dismissed);
		REQUIRE(expiry_outcome.resolution == ConfirmationResolution::expired);
		REQUIRE_FALSE(expiry_outcome.answering_client_contradicted_expiry);
		REQUIRE(expiry_fixture.presenter.dismissed.front().nothing_was_changed);
		REQUIRE_FALSE(expiry_fixture.presenter.dismissed.front().answering_client.has_value());
	}

	SECTION("the identifier falls back to the payload when the envelope carried none")
	{
		// confirm-resolved.schema.json repeats requestId in the payload so that a
		// payload handed over on its own is still self-describing. The envelope's copy
		// is what the server correlates on and wins; this is the fallback.
		CoordinatorFixture fallback_fixture;

		fallback_fixture.coordinator.handle_confirmation_request(
			confirmation_request("confirmation-4", "Delete four items", "low", "Removes four items.")
		);

		StubInboundEnvelope inbound_envelope =
			confirmation_resolved("confirmation-4", "rejected", "pwa");
		inbound_envelope.request_id.clear();

		const ConfirmationResolvedOutcome fallback_outcome =
			fallback_fixture.coordinator.handle_confirmation_resolved(inbound_envelope);

		REQUIRE(fallback_outcome.request_id == "confirmation-4");
		REQUIRE(fallback_outcome.disposition == ConfirmationResolvedDisposition::dismissed);
		REQUIRE(fallback_outcome.prompt_dismissed);
		REQUIRE(fallback_fixture.presenter.dismissed.front().nothing_was_changed);
	}
}

TEST_CASE("the whole exchange runs end to end without the extension deciding anything", "[confirmation]")
{
	// The sequence a producer actually sees: the prompt goes up, they answer, the
	// answer goes out correlated, the server confirms it. Nothing leaves the extension
	// before the producer acts, and exactly one decision leaves after.
	CoordinatorFixture fixture;

	fixture.coordinator.handle_confirmation_request(confirmation_request(
		"confirmation-1",
		"Delete the two takes on Lead Vocal",
		"high",
		"Removes takes 2 and 3 from Lead Vocal."
	));

	REQUIRE(fixture.outbound_queue.empty());
	REQUIRE(fixture.presenter.presented.size() == 1);
	REQUIRE(fixture.presenter.dismissed.empty());

	REQUIRE(fixture.coordinator.reject("confirmation-1").decision_queued);

	const std::vector<StubOutboundEnvelope> sent = fixture.outbound_queue.drain_up_to(16);

	REQUIRE(sent.size() == 1);
	REQUIRE(sent.front().type == "confirm:reject");
	REQUIRE(sent.front().request_id == "confirmation-1");
	REQUIRE(sent.front().payload.at("decision") == "rejected");

	REQUIRE(fixture.coordinator.handle_confirmation_resolved(
		confirmation_resolved("confirmation-1", "rejected", "extension")
	).disposition == ConfirmationResolvedDisposition::confirmed_local_answer);

	REQUIRE(fixture.coordinator.tracked_confirmation_count() == 0);
	REQUIRE(fixture.presenter.dismissed.size() == 1);
	REQUIRE(fixture.outbound_queue.empty());
}
