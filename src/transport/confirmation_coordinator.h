// The Confirmation Coordinator.
//
// A destructive operation pauses the agent's turn until the producer answers. The
// server broadcasts `confirm:request` to every client in the session, waits up to
// 120 seconds, and broadcasts `confirm:resolved` the moment the confirmation stops
// being pending — because one client answered, or because the window closed. This
// component is the extension's half of that exchange: it presents the request,
// sends the producer's answer, and takes the prompt down again (requirement 13).
//
// Four things about the shape of this file.
//
// **This is the one place in the extension that keeps a pending-request table.**
// Design.md is explicit that `requestId` correlation is the server's concern and the
// extension keeps no pending-request table for tool calls — it echoes the identifier
// it was handed onto the response and forgets it. Confirmations are the stated
// exception, and the reason is that nothing here is answered by code: the producer
// answers, minutes may pass, and the answer arrives through the UI rather than
// through the call that delivered the request. Something has to hold the request in
// between, and it has to be keyed by `requestId` because that is the only thing
// tying an answer back to the operation it permits (requirement 23.7).
//
// **The 120-second window is displayed, not enforced.** The server owns the timer
// and expiry is its conclusion, broadcast as `confirm:resolved` with decision
// `expired` — no client votes on it. But the number goes to the UI anyway
// (requirement 13.4), because the failure mode without it is a producer watching a
// prompt they did not answer vanish, or not vanish, with no sense of a deadline. A
// confirmation that quietly expires looks like the assistant ignoring them.
//
// **Dismissal is keyed by `requestId`, never by "the current prompt".** A
// `confirm:resolved` can arrive for something this extension is not tracking: a late
// or duplicated broadcast, a re-delivery after a reconnect, or a confirmation that
// was already answered here. Dismissing whatever happens to be showing would take
// down a live prompt for a different, still-pending destructive operation — which
// then expires with the producer never having seen it. So resolution looks the
// identifier up, and finding nothing is an ordinary outcome that changes no state.
//
// **The coordinator is templated on the outbound envelope type.**
// transport/envelope.h pulls nlohmann/json and the test target links it only when it
// is installed, so the logic worth testing — track, present, pair a verdict with its
// envelope type, dismiss — is written against the operations it actually performs on
// an envelope: assigning `type` and `request_id`, and writing two strings through
// `payload[...]`. Both nlohmann::json and a std::map<std::string, std::string>
// support that, and the production instantiation in confirmation_coordinator.cpp is
// the compile-time proof that the real envelope does. Same arrangement as
// transport/transport_handler.h, for the same reason.
//
// Nothing here touches REAPER. A confirmation is a protocol and UI concern from end
// to end — the operation it authorises runs later, in the Tool Executor — so there
// is no SDK seam in this component and no REAPER-facing half of it.

#ifndef SESH_AI_TRANSPORT_CONFIRMATION_COORDINATOR_H
#define SESH_AI_TRANSPORT_CONFIRMATION_COORDINATOR_H

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <transport/queue_pair.h>

namespace sesh_ai::transport {

	// The four envelope types of the confirmation exchange. Spelled once so the
	// Message Dispatcher (task 14.1) routes on the same constants the coordinator
	// answers with, rather than on a second copy of the strings.
	//
	// Two inbound, two outbound, and the asymmetry is the protocol: the extension
	// receives requests and resolutions, and sends only the producer's answer.
	inline constexpr std::string_view confirmation_request_envelope_type{"confirm:request"};
	inline constexpr std::string_view confirmation_resolved_envelope_type{"confirm:resolved"};
	inline constexpr std::string_view confirmation_approve_envelope_type{"confirm:approve"};
	inline constexpr std::string_view confirmation_reject_envelope_type{"confirm:reject"};

	// The server's confirmation window, in seconds.
	//
	// Not a timer. The extension runs no countdown of its own and never concludes
	// that a confirmation expired — that conclusion arrives as `confirm:resolved`
	// with decision `expired`. This is a number handed to the UI so the producer can
	// see a deadline (requirement 13.4), and the countdown the UI renders from it
	// (task 19.3) is a display of the server's clock rather than a second one.
	inline constexpr int confirmation_window_seconds = 120;

	// How damaging the operation would be if unintended.
	// confirmation-request.schema.json's `risk_level` enum, closed.
	enum class ConfirmationRiskLevel {
		low,
		medium,
		high
	};

	inline constexpr std::array<ConfirmationRiskLevel, 3> all_confirmation_risk_levels{
		ConfirmationRiskLevel::low,
		ConfirmationRiskLevel::medium,
		ConfirmationRiskLevel::high
	};

	// What a producer can answer.
	//
	// Two values, not three, and that is the contract rather than an omission.
	// confirm-decision.schema.json's `decision` enum is `approved` and `rejected`:
	// an expiry is the server's own conclusion and is never sent by a client. Making
	// it unrepresentable here means no path through this component can send one.
	enum class ProducerVerdict {
		approved,
		rejected
	};

	inline constexpr std::array<ProducerVerdict, 2> all_producer_verdicts{
		ProducerVerdict::approved,
		ProducerVerdict::rejected
	};

	// How a confirmation ended. confirm-resolved.schema.json's `decision` enum —
	// the producer's two answers plus the server's one.
	enum class ConfirmationResolution {
		approved,
		rejected,
		expired
	};

	inline constexpr std::array<ConfirmationResolution, 3> all_confirmation_resolutions{
		ConfirmationResolution::approved,
		ConfirmationResolution::rejected,
		ConfirmationResolution::expired
	};

	// Which of the producer's clients answered.
	// confirm-resolved.schema.json's `answeringClient` enum, and the same two values
	// the Socket.IO handshake's `clientType` carries.
	enum class AnsweringClient {
		extension,
		pwa
	};

	inline constexpr std::array<AnsweringClient, 2> all_answering_clients{
		AnsweringClient::extension,
		AnsweringClient::pwa
	};

	// The schema spellings. Total over each enum, so there is no fallback string
	// that could be compared against a payload and match something nobody wrote.

	constexpr std::string_view to_schema_string(ConfirmationRiskLevel risk_level)
	{
		switch (risk_level) {
			case ConfirmationRiskLevel::low:
				return "low";
			case ConfirmationRiskLevel::medium:
				return "medium";
			case ConfirmationRiskLevel::high:
				return "high";
		}

		return "";
	}

	constexpr std::string_view to_schema_string(ProducerVerdict verdict)
	{
		switch (verdict) {
			case ProducerVerdict::approved:
				return "approved";
			case ProducerVerdict::rejected:
				return "rejected";
		}

		return "";
	}

	constexpr std::string_view to_schema_string(ConfirmationResolution resolution)
	{
		switch (resolution) {
			case ConfirmationResolution::approved:
				return "approved";
			case ConfirmationResolution::rejected:
				return "rejected";
			case ConfirmationResolution::expired:
				return "expired";
		}

		return "";
	}

	constexpr std::string_view to_schema_string(AnsweringClient answering_client)
	{
		switch (answering_client) {
			case AnsweringClient::extension:
				return "extension";
			case AnsweringClient::pwa:
				return "pwa";
		}

		return "";
	}

	// The envelope type that carries a verdict.
	//
	// The pairing lives here, in one switch over the same enum value the payload's
	// `decision` is written from, so the two cannot come apart.
	// confirm-decision.schema.json refuses a payload that disagrees with its own
	// envelope — `confirm:approve` carrying `rejected` — rather than resolving it by
	// precedence, because neither reading is safe to guess at for an operation the
	// producer was asked about. Deriving both sides from one value is what makes that
	// disagreement unconstructible rather than merely tested for.
	constexpr std::string_view envelope_type_for_verdict(ProducerVerdict verdict)
	{
		switch (verdict) {
			case ProducerVerdict::approved:
				return confirmation_approve_envelope_type;
			case ProducerVerdict::rejected:
				return confirmation_reject_envelope_type;
		}

		return "";
	}

	// The resolution a verdict corresponds to, for comparing what this client sent
	// against what the server concluded.
	constexpr ConfirmationResolution resolution_for_verdict(ProducerVerdict verdict)
	{
		switch (verdict) {
			case ProducerVerdict::approved:
				return ConfirmationResolution::approved;
			case ProducerVerdict::rejected:
				return ConfirmationResolution::rejected;
		}

		return ConfirmationResolution::rejected;
	}

	// The inverses. Empty for anything outside the schema's enum — there is no
	// nearest match, because every candidate default here either overstates or
	// understates what a producer is being asked to authorise.

	constexpr std::optional<ConfirmationRiskLevel> confirmation_risk_level_from_schema_string(
		std::string_view risk_level_name
	)
	{
		for (const ConfirmationRiskLevel candidate : all_confirmation_risk_levels) {
			if (to_schema_string(candidate) == risk_level_name) {
				return candidate;
			}
		}

		return std::nullopt;
	}

	constexpr std::optional<ConfirmationResolution> confirmation_resolution_from_schema_string(
		std::string_view resolution_name
	)
	{
		for (const ConfirmationResolution candidate : all_confirmation_resolutions) {
			if (to_schema_string(candidate) == resolution_name) {
				return candidate;
			}
		}

		return std::nullopt;
	}

	constexpr std::optional<AnsweringClient> answering_client_from_schema_string(
		std::string_view answering_client_name
	)
	{
		for (const AnsweringClient candidate : all_answering_clients) {
			if (to_schema_string(candidate) == answering_client_name) {
				return candidate;
			}
		}

		return std::nullopt;
	}

	// What the UI needs in order to render the prompt (requirement 13.1).
	//
	// A flat structure of already-decoded values rather than the payload, because
	// everything downstream of here — the UI Host bridge, then React — works in
	// serialized messages and has no business re-reading a JSON payload the
	// coordinator already read.
	struct PendingConfirmationView {
		// The correlation identifier. Never empty in a tracked confirmation: an
		// unaddressable one is refused before it becomes a prompt, because
		// confirm-decision.schema.json requires a `requestId` of at least one
		// character and a prompt whose buttons cannot produce a valid answer is worse
		// than none.
		std::string request_id;

		// One line of plain language, as the agent wrote it.
		std::string action_summary;

		// Longer prose, including which tracks, items, or files are affected. The
		// blocking entities of a precondition refusal are enumerated in here rather
		// than in a structured field — confirmation-request.schema.json says why.
		std::string details;

		// Defaults to the most damaging reading on purpose. When the payload's
		// `risk_level` is missing or is a value this build does not know, the
		// conservative direction is the safe one: understating the risk of a
		// destructive operation is what gets a producer to approve something they
		// would have read twice.
		ConfirmationRiskLevel risk_level = ConfirmationRiskLevel::high;

		// False when risk_level above is this component's conservative default rather
		// than the agent's own assessment, so the UI can say so instead of presenting
		// a guess as a fact.
		bool risk_level_reported = false;

		// The server's window, for the countdown. Carried on the view rather than
		// left for the UI to reach back for, so the prompt and its deadline travel
		// together across the bridge.
		int confirmation_window_seconds = sesh_ai::transport::confirmation_window_seconds;

		// False when any of the three required fields was missing or unreadable.
		// confirmation-request.schema.json requires all three and the Envelope Codec
		// refuses a payload that fails it, so this is false only on codec bypass or
		// protocol drift — and when it is, the producer is being asked to authorise
		// something the extension could not fully describe. What to do about that is
		// the UI's call (task 19.3); the coordinator's job is to say so rather than
		// to quietly render a blank prompt.
		bool description_complete = false;
	};

	// What the UI needs in order to take the prompt down and say how it ended
	// (requirements 13.5, 23.7).
	struct ConfirmationResolutionView {
		std::string request_id;

		// Empty when the resolution string was not one of the three this build knows.
		// The prompt still comes down — it is demonstrably no longer pending — but
		// naming an outcome would be inventing one, so the UI is told the confirmation
		// ended without being told how.
		std::optional<ConfirmationResolution> resolution;

		// Which client answered, when the server said. Absent for an expiry, where
		// nothing answered, and absent on drift where an approved or rejected
		// resolution arrived without it.
		std::optional<AnsweringClient> answering_client;

		// True when this extension sent the answer, so the UI can say "you approved
		// this" rather than "approved on your phone".
		bool answered_by_this_client = false;

		// What this extension sent, when it sent anything. Present for a resolution
		// this client answered, whether or not the server agreed.
		std::optional<ProducerVerdict> local_verdict;

		// True when this extension sent a verdict and the server concluded something
		// else — the producer tapping Approve as the window closed is the case that
		// produces it. The outcome the producer needs to hear is the server's, and
		// they need to hear that theirs did not take effect.
		bool contradicts_local_verdict = false;

		// True only when the resolution establishes that the session was not touched:
		// rejected, or expired (requirement 23.7). False covers both "approved" and
		// "this build could not read the resolution", so it is a licence to tell the
		// producer nothing changed, not a claim that something did.
		bool nothing_was_changed = false;
	};

	// What the extension needs from the UI: put a prompt up, take a prompt down.
	//
	// The narrowest seam this component could have, and the same per-component
	// interface convention entry/timer_registration.h and
	// transport/transport_handler.h follow. The implementation is the UI Host (task
	// 19.x), which serializes the view across the CEF bridge — so the coordinator is
	// testable without CEF, and no REAPER or browser handle is visible from here.
	//
	// Both calls are made on REAPER's main thread, inside the tick.
	class ConfirmationPresenter {
	public:
		virtual ~ConfirmationPresenter() = default;

		// Show the prompt. Called once per tracked confirmation, and again for the
		// same `requestId` only when the server re-delivers it — in which case the UI
		// should refresh the prompt it already has rather than stack a second one.
		virtual void present_confirmation(const PendingConfirmationView& pending_confirmation) = 0;

		// Take the prompt for this `requestId` down, and say how it ended. Keyed by
		// identifier, not "the current prompt": another confirmation may be showing
		// and still be answerable.
		virtual void dismiss_confirmation(const ConfirmationResolutionView& resolution) = 0;
	};

	// How a `confirm:request` was handled.
	enum class ConfirmationRequestDisposition {
		// New, tracked, and presented to the producer.
		presented,

		// A `requestId` already tracked. The entry is refreshed in place and the
		// prompt re-presented rather than a second one added, so a re-delivered
		// broadcast does not give the producer two prompts for one operation.
		redelivered,

		// No `requestId`. Not tracked and not presented — see
		// PendingConfirmationView::request_id.
		unaddressable
	};

	// How a producer's answer was handled.
	enum class ConfirmationDecisionDisposition {
		// Queued as `confirm:approve` or `confirm:reject`, and the prompt dismissed.
		sent,

		// Nothing is tracked under this `requestId`. Either it was never seen, or it
		// already resolved — and a decision for a resolved confirmation is a
		// decision the server would apply to nothing or refuse, so it is not sent.
		not_tracked,

		// This client already answered this confirmation and is waiting on the
		// server's resolution. A second answer is not sent: the first one stands, and
		// two decisions for one `requestId` is a race the server should not be asked
		// to arbitrate.
		already_answered
	};

	// How a `confirm:resolved` was handled.
	enum class ConfirmationResolvedDisposition {
		// It was awaiting the producer here. The prompt is dismissed and the outcome
		// shown — the PWA answered first, or the window closed.
		dismissed,

		// This client answered it and the server agrees. The prompt came down when
		// the producer tapped, so there is nothing to dismiss and nothing to correct.
		confirmed_local_answer,

		// This client answered it and the server concluded otherwise. The UI is told,
		// because the producer believes their answer took effect.
		contradicted_local_answer,

		// Nothing is tracked under this `requestId` — a late or duplicated broadcast,
		// or a resolution for a confirmation this extension never saw. No prompt is
		// dismissed and no state changes.
		not_tracked
	};

	struct ConfirmationRequestOutcome {
		ConfirmationRequestDisposition disposition = ConfirmationRequestDisposition::unaddressable;

		// As it arrived, including when it was empty — which is the value worth having
		// in the log line.
		std::string request_id;

		bool tracked = false;
		bool presented = false;

		// Mirrors the view that was presented, so a caller that logs rather than
		// renders still has the two facts worth logging.
		bool description_complete = false;
		bool risk_level_reported = false;
		ConfirmationRiskLevel risk_level = ConfirmationRiskLevel::high;
	};

	struct ConfirmationDecisionOutcome {
		ConfirmationDecisionDisposition disposition = ConfirmationDecisionDisposition::not_tracked;

		std::string request_id;
		ProducerVerdict verdict = ProducerVerdict::rejected;

		// The envelope type that was queued, empty when nothing was. Carried so a log
		// line names what went out rather than re-deriving it from the verdict.
		std::string envelope_type;

		bool decision_queued = false;
		bool prompt_dismissed = false;
	};

	struct ConfirmationResolvedOutcome {
		ConfirmationResolvedDisposition disposition = ConfirmationResolvedDisposition::not_tracked;

		std::string request_id;

		// Empty when the `decision` field was not one of the three.
		std::optional<ConfirmationResolution> resolution;

		bool prompt_dismissed = false;
		bool stopped_tracking = false;

		// True when the payload named an answering client for an expiry, which
		// confirm-resolved.schema.json forbids via its if/then/else. The `decision` is
		// trusted and the client dropped: telling the producer somebody approved an
		// expired confirmation is the worse of the two readings.
		bool answering_client_contradicted_expiry = false;
	};

	// Presents confirmations, sends the producer's answer, and dismisses prompts.
	//
	// Templated on the outbound envelope type — see the file comment for why. The
	// production instantiation lives in confirmation_coordinator.cpp.
	template <typename OutboundEnvelopeType>
	class ConfirmationCoordinator {
	public:
		using OutboundQueue = ConcurrentQueue<OutboundEnvelopeType>;

		ConfirmationCoordinator(ConfirmationPresenter& presenter, OutboundQueue& outbound_queue)
			: presenter_{presenter},
			outbound_queue_{outbound_queue}
		{
		}

		ConfirmationCoordinator(const ConfirmationCoordinator&) = delete;
		ConfirmationCoordinator& operator=(const ConfirmationCoordinator&) = delete;
		ConfirmationCoordinator(ConfirmationCoordinator&&) = delete;
		ConfirmationCoordinator& operator=(ConfirmationCoordinator&&) = delete;

		// The Message Dispatcher's entry point for `confirm:request` (task 14.1).
		//
		// A member template for the same dependency reason handle() in
		// transport_handler.h is one: the JSON-touching half is instantiated only
		// where the real envelope is used. The envelope is taken by const reference
		// because what is kept is a decoded copy, not any part of it.
		template <typename InboundEnvelopeType>
		ConfirmationRequestOutcome handle_confirmation_request(const InboundEnvelopeType& inbound_envelope)
		{
			const std::string action_summary = read_string_field(inbound_envelope.payload, "action_summary");
			const std::string risk_level_name = read_string_field(inbound_envelope.payload, "risk_level");
			const std::string details = read_string_field(inbound_envelope.payload, "details");

			return present_confirmation_request(
				inbound_envelope.request_id,
				action_summary,
				risk_level_name,
				details
			);
		}

		// The Message Dispatcher's entry point for `confirm:resolved` (task 14.1).
		template <typename InboundEnvelopeType>
		ConfirmationResolvedOutcome handle_confirmation_resolved(const InboundEnvelopeType& inbound_envelope)
		{
			// The identifier rides on the envelope and is repeated in the payload so
			// that a payload handed over on its own is still self-describing. The
			// envelope's copy is the one the server correlates on, so it wins; the
			// payload's is the fallback for a broadcast that carried only the body.
			std::string request_id = inbound_envelope.request_id;

			if (request_id.empty()) {
				request_id = read_string_field(inbound_envelope.payload, "requestId");
			}

			const std::string resolution_name = read_string_field(inbound_envelope.payload, "decision");
			const std::string answering_client_name =
				read_string_field(inbound_envelope.payload, "answeringClient");

			return resolve_confirmation(request_id, resolution_name, answering_client_name);
		}

		// The same work with the payload already taken apart, which is the form the
		// suite drives and the form the sequence is actually about.
		//
		// `risk_level_name` is the schema string rather than the enum on purpose: an
		// unrecognised one has to be distinguishable from a missing one nowhere in
		// this component, and both land on the same conservative default, so passing
		// the string keeps that decision in one place.
		ConfirmationRequestOutcome present_confirmation_request(
			std::string_view request_id,
			std::string_view action_summary,
			std::string_view risk_level_name,
			std::string_view details
		)
		{
			ConfirmationRequestOutcome outcome;
			outcome.request_id.assign(request_id);

			const std::optional<ConfirmationRiskLevel> risk_level =
				confirmation_risk_level_from_schema_string(risk_level_name);

			outcome.risk_level_reported = risk_level.has_value();
			outcome.risk_level = risk_level.value_or(ConfirmationRiskLevel::high);
			outcome.description_complete =
				!action_summary.empty() && !details.empty() && risk_level.has_value();

			// Unaddressable. confirm-decision.schema.json requires a `requestId` of at
			// least one character, so neither answer could be sent — approving would
			// build a payload the extension's own outbound validation refuses
			// (requirement 4.1), which is where that failure belongs rather than at the
			// server.
			//
			// So the prompt is not raised at all. This is the one case where declining
			// to show something is better than showing it: a confirmation prompt whose
			// buttons cannot produce a valid answer would have the producer tap
			// Approve and watch the operation stall for 120 seconds and expire, which
			// is precisely the appearance requirement 13.4 exists to prevent. The
			// outcome reports it so the Message Dispatcher logs it.
			if (request_id.empty()) {
				outcome.disposition = ConfirmationRequestDisposition::unaddressable;

				return outcome;
			}

			PendingConfirmationView view;
			view.request_id.assign(request_id);
			view.action_summary.assign(action_summary);
			view.details.assign(details);
			view.risk_level = outcome.risk_level;
			view.risk_level_reported = outcome.risk_level_reported;
			view.confirmation_window_seconds = sesh_ai::transport::confirmation_window_seconds;
			view.description_complete = outcome.description_complete;

			TrackedConfirmation* const existing = find_tracked(request_id);

			if (existing != nullptr) {
				// A re-delivery. Refreshed in place rather than appended, so one
				// operation never produces two prompts — and the local verdict is left
				// alone, because a producer who already answered has not unanswered by
				// the server repeating itself.
				existing->view = std::move(view);

				outcome.disposition = ConfirmationRequestDisposition::redelivered;
			} else {
				TrackedConfirmation tracked;
				tracked.view = std::move(view);

				tracked_confirmations_.push_back(std::move(tracked));

				outcome.disposition = ConfirmationRequestDisposition::presented;
			}

			outcome.tracked = true;

			// Presented after the table is updated, so the UI cannot answer into a
			// coordinator that does not yet know about the confirmation. The call is
			// synchronous but the answer is not, and ordering it this way costs nothing.
			presenter_.present_confirmation(find_tracked(request_id)->view);

			outcome.presented = true;

			return outcome;
		}

		// The producer approved. Requirement 13.2.
		ConfirmationDecisionOutcome approve(std::string_view request_id)
		{
			return answer(request_id, ProducerVerdict::approved);
		}

		// The producer rejected. Requirement 13.3.
		ConfirmationDecisionOutcome reject(std::string_view request_id)
		{
			return answer(request_id, ProducerVerdict::rejected);
		}

		// What the UI Host calls when the producer taps one of the two buttons (task
		// 19.3). Requirements 13.2 and 13.3.
		//
		// Refuses anything it is not tracking, which is the load-bearing check. An
		// answer for a `requestId` that already resolved is the producer tapping a
		// prompt the PWA answered a moment earlier or that expired while they read
		// it — and sending it would have the extension authorising a destructive
		// operation whose window has closed.
		ConfirmationDecisionOutcome answer(std::string_view request_id, ProducerVerdict verdict)
		{
			ConfirmationDecisionOutcome outcome;
			outcome.request_id.assign(request_id);
			outcome.verdict = verdict;

			TrackedConfirmation* const tracked = find_tracked(request_id);

			if (tracked == nullptr) {
				outcome.disposition = ConfirmationDecisionDisposition::not_tracked;

				return outcome;
			}

			if (tracked->local_verdict.has_value()) {
				outcome.disposition = ConfirmationDecisionDisposition::already_answered;

				return outcome;
			}

			queue_decision(request_id, verdict);

			tracked->local_verdict = verdict;

			outcome.disposition = ConfirmationDecisionDisposition::sent;
			outcome.envelope_type.assign(envelope_type_for_verdict(verdict));
			outcome.decision_queued = true;

			// The prompt comes down now rather than when the server's resolution
			// arrives. The producer tapped a button and a prompt that lingers through a
			// network round trip reads as one that did not register. The entry stays in
			// the table, marked, so the resolution can be recognised as the answer to
			// this client's own decision — and so a second tap cannot send a second one.
			ConfirmationResolutionView dismissal;
			dismissal.request_id.assign(request_id);
			dismissal.resolution = resolution_for_verdict(verdict);
			dismissal.answering_client = AnsweringClient::extension;
			dismissal.answered_by_this_client = true;
			dismissal.local_verdict = verdict;
			dismissal.nothing_was_changed = verdict == ProducerVerdict::rejected;

			presenter_.dismiss_confirmation(dismissal);

			outcome.prompt_dismissed = true;

			return outcome;
		}

		// The server says this confirmation is no longer pending. Requirements 13.5
		// and 23.7.
		//
		// `resolution_name` and `answering_client_name` are the schema strings, with
		// an empty `answering_client_name` meaning the field was absent — which is
		// what confirm-resolved.schema.json requires for an expiry and forbids for
		// the other two.
		ConfirmationResolvedOutcome resolve_confirmation(
			std::string_view request_id,
			std::string_view resolution_name,
			std::string_view answering_client_name
		)
		{
			ConfirmationResolvedOutcome outcome;
			outcome.request_id.assign(request_id);
			outcome.resolution = confirmation_resolution_from_schema_string(resolution_name);

			const std::optional<AnsweringClient> reported_answering_client =
				answering_client_from_schema_string(answering_client_name);

			// An expiry that names an answering client contradicts itself: nothing
			// answered it. The `decision` field is required by the schema and the
			// `answeringClient` is the one the if/then/else forbids here, so `decision`
			// is trusted and the client dropped. The alternative — telling the producer
			// their phone approved a confirmation that in fact expired — is the reading
			// that misinforms them about whether their session was touched.
			std::optional<AnsweringClient> answering_client = reported_answering_client;

			if (outcome.resolution == ConfirmationResolution::expired && reported_answering_client.has_value()) {
				outcome.answering_client_contradicted_expiry = true;
				answering_client.reset();
			}

			const std::optional<std::size_t> tracked_index = find_tracked_index(request_id);

			// Not tracked. A late or duplicated broadcast, a resolution for a
			// confirmation that arrived without a usable `requestId`, or one this
			// extension never saw at all. Nothing is dismissed — dismissing whatever
			// prompt happens to be showing would take down a live one for a different
			// destructive operation, which would then expire unseen.
			if (!tracked_index.has_value()) {
				outcome.disposition = ConfirmationResolvedDisposition::not_tracked;

				return outcome;
			}

			const std::optional<ProducerVerdict> local_verdict =
				tracked_confirmations_[*tracked_index].local_verdict;

			// Stop tracking before notifying, so a presenter that calls straight back
			// into the coordinator sees a table that is already settled.
			tracked_confirmations_.erase(tracked_confirmations_.begin() + static_cast<std::ptrdiff_t>(*tracked_index));

			outcome.stopped_tracking = true;

			ConfirmationResolutionView view;
			view.request_id.assign(request_id);
			view.resolution = outcome.resolution;
			view.answering_client = answering_client;
			view.local_verdict = local_verdict;
			view.answered_by_this_client = local_verdict.has_value();
			view.nothing_was_changed = outcome.resolution == ConfirmationResolution::rejected
				|| outcome.resolution == ConfirmationResolution::expired;

			// This client never answered: the prompt is still up and comes down now.
			// Requirement 13.5, and the reason it says "immediately".
			if (!local_verdict.has_value()) {
				outcome.disposition = ConfirmationResolvedDisposition::dismissed;

				presenter_.dismiss_confirmation(view);

				outcome.prompt_dismissed = true;

				return outcome;
			}

			// This client answered and the server agrees. The prompt came down when the
			// producer tapped, and the schema's own note says the answering client
			// already knows and ignores this broadcast — so the UI is not called again.
			// Re-announcing an outcome the producer just chose is noise, and noise
			// around destructive operations trains people to stop reading.
			if (outcome.resolution == resolution_for_verdict(*local_verdict)) {
				outcome.disposition = ConfirmationResolvedDisposition::confirmed_local_answer;

				return outcome;
			}

			// This client answered and the server concluded otherwise — the producer
			// tapping Approve as the window closed, or answering here while the PWA
			// answered differently. They believe their answer took effect, so they are
			// told the outcome that actually holds. This is requirement 23.7's "indicate
			// that nothing was changed" in the case where it is least obvious and most
			// needed.
			view.contradicts_local_verdict = true;

			outcome.disposition = ConfirmationResolvedDisposition::contradicted_local_answer;

			presenter_.dismiss_confirmation(view);

			outcome.prompt_dismissed = true;

			return outcome;
		}

		// Drops everything tracked and takes the prompts down, reporting how many.
		//
		// For the reconnect path (requirement 23.1): the resolution broadcasts for
		// anything pending across a dropped socket are gone, so without this the table
		// holds entries that will never resolve and the producer keeps prompts that
		// can no longer be answered. Each dismissal carries no resolution, because
		// what happened to those confirmations server-side is not known here.
		std::size_t abandon_all_tracked_confirmations()
		{
			// Moved out first, so the table is empty before any presenter call. A
			// presenter that answers or resolves from inside the loop then finds
			// nothing tracked, which is a defined outcome rather than an iterator
			// invalidated mid-traversal.
			const std::vector<TrackedConfirmation> abandoned = std::move(tracked_confirmations_);

			tracked_confirmations_.clear();

			for (const TrackedConfirmation& tracked : abandoned) {
				ConfirmationResolutionView view;
				view.request_id = tracked.view.request_id;
				view.local_verdict = tracked.local_verdict;
				view.answered_by_this_client = tracked.local_verdict.has_value();

				presenter_.dismiss_confirmation(view);
			}

			return abandoned.size();
		}

		// Everything in the table, answered or not.
		std::size_t tracked_confirmation_count() const { return tracked_confirmations_.size(); }

		// Those the producer has not answered here — the ones a prompt is showing for.
		std::size_t awaiting_producer_count() const
		{
			std::size_t awaiting_count = 0;

			for (const TrackedConfirmation& tracked : tracked_confirmations_) {
				if (!tracked.local_verdict.has_value()) {
					++awaiting_count;
				}
			}

			return awaiting_count;
		}

		bool is_tracked(std::string_view request_id) const
		{
			return find_tracked_index(request_id).has_value();
		}

		bool awaits_producer(std::string_view request_id) const
		{
			const TrackedConfirmation* const tracked = find_tracked_const(request_id);

			return tracked != nullptr && !tracked->local_verdict.has_value();
		}

		// What this client answered under this identifier, when it answered.
		std::optional<ProducerVerdict> local_verdict_for(std::string_view request_id) const
		{
			const TrackedConfirmation* const tracked = find_tracked_const(request_id);

			if (tracked == nullptr) {
				return std::nullopt;
			}

			return tracked->local_verdict;
		}

		// The view as it was last presented, for a UI that reconnects its bridge and
		// needs to redraw prompts it has lost.
		const PendingConfirmationView* pending_confirmation_view(std::string_view request_id) const
		{
			const TrackedConfirmation* const tracked = find_tracked_const(request_id);

			if (tracked == nullptr) {
				return nullptr;
			}

			return &tracked->view;
		}

		// Tracked identifiers, oldest first.
		std::vector<std::string> tracked_request_ids() const
		{
			std::vector<std::string> request_ids;
			request_ids.reserve(tracked_confirmations_.size());

			for (const TrackedConfirmation& tracked : tracked_confirmations_) {
				request_ids.push_back(tracked.view.request_id);
			}

			return request_ids;
		}

	private:
		// One row of the pending-request table.
		struct TrackedConfirmation {
			PendingConfirmationView view;

			// Set when this client answered and is waiting on the server's resolution.
			std::optional<ProducerVerdict> local_verdict;
		};

		// Builds the `confirm:approve` or `confirm:reject` envelope and hands it to the
		// network thread through the outbound queue.
		//
		// The payload is `requestId` and `decision`, which is the whole of
		// confirm-decision.schema.json — `additionalProperties` is false. Both the
		// envelope type and the decision string come from the same verdict, so the
		// disagreement the schema refuses cannot be constructed here.
		//
		// The identifier goes on the envelope as well as into the payload: the
		// envelope's copy is what the server correlates on, the payload's is what lets
		// the payload identify its own subject when it is logged or persisted apart
		// from its envelope.
		void queue_decision(std::string_view request_id, ProducerVerdict verdict)
		{
			OutboundEnvelopeType decision_envelope;

			decision_envelope.type = std::string{envelope_type_for_verdict(verdict)};
			decision_envelope.request_id = std::string{request_id};
			decision_envelope.payload["requestId"] = std::string{request_id};
			decision_envelope.payload["decision"] = std::string{to_schema_string(verdict)};

			outbound_queue_.push(std::move(decision_envelope));
		}

		// The table is a vector rather than a map, and searched linearly.
		//
		// Confirmations are destructive operations a producer is asked about one at a
		// time; the table holds one entry in practice and a handful at worst. Arrival
		// order is worth keeping — it is the order the prompts went up — and a hash
		// map would lose it to buy a lookup nobody can measure.
		std::optional<std::size_t> find_tracked_index(std::string_view request_id) const
		{
			if (request_id.empty()) {
				return std::nullopt;
			}

			for (std::size_t tracked_index = 0; tracked_index < tracked_confirmations_.size(); ++tracked_index) {
				if (tracked_confirmations_[tracked_index].view.request_id == request_id) {
					return tracked_index;
				}
			}

			return std::nullopt;
		}

		TrackedConfirmation* find_tracked(std::string_view request_id)
		{
			const std::optional<std::size_t> tracked_index = find_tracked_index(request_id);

			if (!tracked_index.has_value()) {
				return nullptr;
			}

			return &tracked_confirmations_[*tracked_index];
		}

		const TrackedConfirmation* find_tracked_const(std::string_view request_id) const
		{
			const std::optional<std::size_t> tracked_index = find_tracked_index(request_id);

			if (!tracked_index.has_value()) {
				return nullptr;
			}

			return &tracked_confirmations_[*tracked_index];
		}

		// Reads a string field out of an already-validated payload, defensively rather
		// than by assuming the codec ran.
		//
		// Empty for anything missing or not a string, which every caller treats as
		// absent — one path for "no usable value" rather than an exception from the
		// JSON library crossing into the main-thread tick. None of the fields read
		// here can legitimately be the empty string: the two prose fields have
		// `minLength` 1 and the rest are closed enums.
		template <typename PayloadType>
		static std::string read_string_field(const PayloadType& payload, const char* field_name)
		{
			if (!payload.is_object() || !payload.contains(field_name)) {
				return std::string{};
			}

			const auto& field = payload.at(field_name);

			if (!field.is_string()) {
				return std::string{};
			}

			return field.template get<std::string>();
		}

		ConfirmationPresenter& presenter_;
		OutboundQueue& outbound_queue_;

		// The extension's one pending-request table. Requirement 23.7, and design.md's
		// stated exception to keeping none.
		std::vector<TrackedConfirmation> tracked_confirmations_;
	};

}

#endif
