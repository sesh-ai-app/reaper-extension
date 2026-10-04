// The Stream Presenter (requirement 16, design "Stream Presenter").
//
// Two unrelated things arrive on the `stream:` namespace and both end up in front
// of the producer, which is why one component holds them.
//
// The assistant response arrives as `stream:agent_response_start`, a run of
// `_delta` envelopes, and one `_stop`. The presenter assembles them into the text
// the UI renders as it grows, and uses each delta's sequence number to know whether
// what it is showing is the whole of what was sent (requirement 16.1).
//
// The IVS publish details arrive as `stream:rtmps_url`: an RTMPS ingest endpoint and
// a publish participant token that the producer types into ReaCast by hand, because
// ReaCast's destination is a GUI-only setting. Both are presented in full plaintext
// — there is no other way for the producer to transcribe them (requirement 16.2) —
// and a reissued token replaces the displayed one (requirement 16.3). The token is a
// credential and is never logged (requirements 16.4, 26.4).
//
// ---------------------------------------------------------------------------
// No REAPER, no window, no JSON of its own
//
// This is presentation *state*, not presentation. It owns no CEF browser and calls
// no REAPER function: the UI Host (task 17.2) hands it inbound envelopes and reads
// the views below to publish across the CEF bridge, and the React UI (tasks 19.x)
// renders them. That keeps the whole of the sequencing logic — the part with the
// edge cases — unit-testable, which is the only part of it worth defending.
//
// The class is not templated, but the two envelope entry points are member
// templates, for the reason transport/transport_handler.h gives: transport/envelope.h
// pulls nlohmann/json, the suite links it only when installed, and the logic worth
// testing is reachable through the field-level overloads. stream_presenter.cpp holds
// the production instantiation, which is the compile-time proof that the real
// envelope satisfies the payload reads.
//
// ---------------------------------------------------------------------------
// The sequence number, and the four ways a delta stream goes wrong
//
// `agent-response-delta.schema.json` defines `sequence` as the zero-based index of
// the delta within the turn, and the server assigns it from its own relay counter —
// so for one turn the sequence numbers are 0, 1, 2, ... with no holes, and
// `agent-response-stop.schema.json`'s `deltaCount` is how many were sent. Anything
// else the presenter sees was introduced between there and here, by a socket that
// dropped and resumed. Four cases, all reachable, and each is answered differently
// because each has a different producer-visible failure:
//
//   **A gap** — a delta whose sequence is beyond the next one expected. The missing
//   numbers are recorded and the delta is still rendered. Freezing the bubble until
//   the hole fills would hide an answer that is mostly present, and the hole is very
//   often filled moments later by the `_stop` envelope, which carries the full
//   assembled text. So: render, remember, and report the response as possibly
//   incomplete until something repairs it.
//
//   **An out-of-order or late delta** — one whose sequence is behind the next
//   expected, in a slot still empty. It is placed at its own index rather than
//   appended, which is why fragments are held in a map keyed by sequence and the
//   rendered text is their concatenation in key order. Appending it would put a
//   fragment of a sentence in the wrong place, and text that is subtly scrambled is
//   worse than text with a visible hole in it.
//
//   **A duplicate** — a sequence already held. Ignored. A replay after a reconnect
//   is the expected cause and the text is byte-identical, so ignoring it is
//   idempotent. When the text differs for the same index the first one is kept:
//   whatever the second one means, it is not a reason to rewrite a sentence the
//   producer has already read. The two are reported separately, because identical is
//   routine and conflicting is a protocol bug.
//
//   **A delta with no preceding start** — the socket reconnected mid-turn and the
//   start was missed. The delta payload carries `conversationId`, `turnId`, and
//   `messageId`, which is everything the start carries, so the response is opened
//   from the delta and flagged as opened implicitly. Dropping it instead would show
//   the producer nothing at all for a turn the agent is answering.
//
// Two more, from the same cause. A delta for a turn that is not the open one
// supersedes it — the server streams one turn at a time per conversation, so a new
// turn means the previous one's start and stop were both missed. And a delta for the
// turn that has already stopped is ignored, because the stop's assembled text is
// authoritative and complete.
//
// ---------------------------------------------------------------------------
// The stream key is a credential, and that is enforced by the type
//
// "Never log the stream key" cannot be a rule someone remembers, because the one
// place it gets broken is the diagnostic somebody adds at 2am to find out why
// ReaCast will not connect. So StreamKey has no `operator<<` — there is a deleted
// one, so the mistake names itself at compile time rather than resolving to some
// conversion — no conversion to std::string, and no JSON serializer. The only way to
// read it is `reveal_for_display()`, whose name is the point.
//
// Plaintext is required, in the UI, by default (requirement 16.2). That is not in
// tension with the above and the two must not be conflated: the token is shown in
// full to the producer and is absent from every log line. So the revealed value
// exists in exactly one place — StreamIngestDisplay, built on demand for the CEF
// bridge — and the log-facing view is a separate type that has no field for it.
//
// The outcome structs returned from the handlers carry booleans and counts only, for
// the same reason: they are what a caller logs.

#ifndef SESH_AI_UI_STREAM_PRESENTER_H
#define SESH_AI_UI_STREAM_PRESENTER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sesh_ai::ui {

	// The five envelope types this component consumes, spelled as the server spells
	// them, so the Message Dispatcher (task 14.1) routes on the same constants the
	// presenter answers to rather than on a second copy of the strings.
	inline constexpr std::string_view agent_response_start_envelope_type{"stream:agent_response_start"};
	inline constexpr std::string_view agent_response_delta_envelope_type{"stream:agent_response_delta"};
	inline constexpr std::string_view agent_response_stop_envelope_type{"stream:agent_response_stop"};
	inline constexpr std::string_view stream_rtmps_url_envelope_type{"stream:rtmps_url"};
	inline constexpr std::string_view stream_error_envelope_type{"stream:error"};

	// The seven `error:*` types, by their shared namespace rather than one by one.
	//
	// Requirement 5.5 sends them here alongside `stream:error`, and none of them has a
	// bundled payload schema — so they are read defensively and folded into
	// `StreamErrorView`, which is design.md's "the view model half covering a protocol
	// hole rather than the UI having to". Matching on the prefix rather than
	// enumerating the seven is deliberate: an eighth from a server one version ahead is
	// an error the producer should still be shown, and the fields are the same three
	// either way.
	//
	// Spelled here rather than in `transport/message_dispatcher.h`, which is where it
	// was, for the reason the five above are: the dispatcher routes on the constant the
	// handler answers to, and a second copy that drifted is an error envelope silently
	// dropped. `message_dispatcher.h` reads this one.
	inline constexpr std::string_view error_envelope_type_namespace{"error:"};

	constexpr bool is_error_envelope_type(std::string_view envelope_type)
	{
		return envelope_type.size() > error_envelope_type_namespace.size()
			&& envelope_type.substr(0, error_envelope_type_namespace.size())
				== error_envelope_type_namespace;
	}

	// True for the five above, and for anything under `error:`.
	constexpr bool is_stream_presenter_envelope_type(std::string_view envelope_type)
	{
		return envelope_type == agent_response_start_envelope_type
			|| envelope_type == agent_response_delta_envelope_type
			|| envelope_type == agent_response_stop_envelope_type
			|| envelope_type == stream_rtmps_url_envelope_type
			|| envelope_type == stream_error_envelope_type
			|| is_error_envelope_type(envelope_type);
	}

	// The IVS publish participant token — ReaCast's stream key.
	//
	// A distinct type rather than a std::string, so that the one thing that must never
	// happen to it is not expressible. See the file comment; the short version is that
	// there is no stream insertion operator, no conversion to a string, and one
	// accessor whose name says what it is for.
	//
	// Comparison and emptiness are available because the presenter has to decide
	// whether a reissued token differs from the current one, and a test has to check
	// that it does, neither of which needs the value.
	class StreamKey {
	public:
		StreamKey() = default;

		explicit StreamKey(std::string publish_participant_token)
			: publish_participant_token_{std::move(publish_participant_token)}
		{
		}

		bool empty() const noexcept
		{
			return publish_participant_token_.empty();
		}

		// The character count. Safe to log — it is the difference between "no token
		// arrived" and "a token arrived and ReaCast still refuses it", which is the
		// actual diagnostic question, and it reveals nothing usable.
		std::size_t size() const noexcept
		{
			return publish_participant_token_.size();
		}

		bool operator==(const StreamKey& other) const noexcept
		{
			return publish_participant_token_ == other.publish_participant_token_;
		}

		bool operator!=(const StreamKey& other) const noexcept
		{
			return !(*this == other);
		}

		// The only way out. Named for its only legitimate caller: the code that puts
		// the token in front of the producer so they can paste it into ReaCast
		// (requirement 16.2). Anything else calling this is visible in review as
		// exactly what it is.
		const std::string& reveal_for_display() const noexcept
		{
			return publish_participant_token_;
		}

	private:
		std::string publish_participant_token_;
	};

	// Deleted rather than absent.
	//
	// Without an insertion operator `log_stream << stream_key` would already fail to
	// compile, since nothing converts. Declaring it and deleting it makes the error
	// name this line — and closes the door on a future implicit conversion making the
	// expression compile again by accident.
	template <typename OutputStreamType>
	OutputStreamType& operator<<(OutputStreamType&, const StreamKey&) = delete;

	// How far the current assistant response has got.
	enum class ResponseStreamState {
		// Nothing has been streamed yet, or the presenter was reset.
		idle,

		// A start (or a delta standing in for a missed one) has opened the response
		// and no stop has arrived.
		streaming,

		// A stop arrived. The text is the server's assembled version.
		ended
	};

	// What was done with one delta. Exactly one of these per delta.
	enum class DeltaDisposition {
		// Its sequence was the next one expected.
		applied_in_order,

		// Its sequence was beyond the next expected, so it revealed a gap. Rendered
		// anyway; the missing sequences are recorded.
		applied_after_gap,

		// Its sequence was behind the next expected and that slot was empty — a late
		// or out-of-order delivery. Placed at its own index, not appended.
		applied_filling_gap,

		// Its sequence was already held, with identical text. A reconnect replay.
		ignored_duplicate,

		// Its sequence was already held, with different text. A protocol bug: the
		// first text is kept.
		ignored_conflicting_duplicate,

		// It belongs to the turn that has already stopped, whose assembled text is
		// authoritative.
		ignored_after_stop,

		// `sequence` was missing, not an integer, or negative — none of which the
		// schema permits, so the Envelope Codec was bypassed or the server's protocol
		// is ahead of this build. Nothing is rendered, because there is no index to
		// render it at.
		ignored_invalid_sequence
	};

	// The result of a `stream:agent_response_start`.
	struct ResponseStartOutcome {
		std::string turn_id;

		// A new response bubble is open.
		bool opened_response = false;

		// A start for the turn already open, or already ended. Ignored, so that a
		// replayed start after a reconnect does not blank text the producer is reading.
		bool ignored_duplicate_start = false;

		// A different turn was open and streaming when this arrived.
		bool superseded_previous_turn = false;
	};

	// The result of one `stream:agent_response_delta`.
	struct ResponseDeltaOutcome {
		std::string turn_id;

		// As it arrived, including when it was not usable.
		long long sequence = -1;

		DeltaDisposition disposition = DeltaDisposition::ignored_invalid_sequence;

		// The delta's text is now part of the rendered response.
		bool applied = false;

		// No start had been seen for this turn, so the delta opened the response.
		bool opened_response_implicitly = false;

		// A different turn was open and streaming.
		bool superseded_previous_turn = false;

		// This delta revealed sequence numbers that have not arrived.
		bool gap_detected = false;

		// How many sequence numbers below the highest seen are still missing, after
		// this delta. Zero means what is rendered is everything that was sent.
		std::size_t missing_sequence_count = 0;
	};

	// The result of a `stream:agent_response_stop`.
	struct ResponseStopOutcome {
		std::string turn_id;

		bool accepted = false;

		// A second stop for a turn that already ended. Ignored.
		bool ignored_duplicate_stop = false;

		// No start and no delta had been seen for this turn — a client that joined
		// mid-stream, which is the case the stop payload's full `text` exists for.
		bool opened_response_implicitly = false;

		bool superseded_previous_turn = false;

		// The rendered text was replaced by the payload's assembled `text`.
		bool text_replaced_with_assembled_text = false;

		// The payload's `text` was empty while deltas had been applied, so the
		// assembled fragments were kept instead. The schema permits an empty string
		// here, and honouring it would blank a response the producer has read.
		bool assembled_text_was_empty = false;

		// There were missing sequence numbers and the assembled text repaired them.
		bool gaps_repaired_by_assembled_text = false;

		std::size_t missing_sequence_count_before_stop = 0;

		// `deltaCount` from the payload against what was actually applied. A mismatch
		// with no missing sequences means deltas were lost without leaving a hole —
		// which the sequence numbers alone cannot show.
		long long reported_delta_count = 0;
		std::size_t applied_delta_count = 0;
		bool delta_count_matched = false;
	};

	// The result of a `stream:rtmps_url`.
	//
	// No token material here, by construction — this is what a caller logs.
	struct StreamIngestOutcome {
		bool accepted = false;

		// A required field was empty. Refused, and any previous details are kept: a
		// blank ingest URL or stream key pasted into ReaCast fails in a way that looks
		// like ReaCast's fault, and overwriting details the producer is publishing with
		// is worse than ignoring a malformed envelope.
		bool rejected_empty_field = false;

		// Details were already held and have been replaced (requirement 16.3).
		bool replaced_previous_details = false;

		// The payload's own `reissued` flag.
		bool reissued = false;

		// `reissued` disagreed with what the presenter held: true with nothing to
		// reissue, or false when details were already present. Recorded rather than
		// acted on — the payload is applied either way, since the token it carries is
		// the one IVS will accept.
		bool reissue_flag_disagreed_with_state = false;

		bool ingest_endpoint_changed = false;
		bool stage_changed = false;

		// Whether the token itself differs from the one previously displayed. A
		// reissue that carries the same token needs no attention drawn to it.
		bool stream_key_changed = false;

		bool expiry_changed = false;
	};

	// The result of a `stream:error`.
	struct StreamErrorOutcome {
		bool recorded = false;

		// Details were held and were left alone. A failure to issue a *replacement*
		// token does not invalidate the one the producer is currently publishing with,
		// so clearing the display would pull working details out from under a live
		// stream (requirement 15.6's error is about setup, not about revocation).
		bool retained_existing_ingest_details = false;
	};

	// Which handler ran for an envelope, and what it did. Returned by the
	// envelope-level entry point; the sub-outcome present is the one matching `type`.
	struct StreamEnvelopeOutcome {
		std::string envelope_type;

		// False when the type was not one of the five. Nothing was touched.
		bool recognised = false;

		std::optional<ResponseStartOutcome> start;
		std::optional<ResponseDeltaOutcome> delta;
		std::optional<ResponseStopOutcome> stop;
		std::optional<StreamIngestOutcome> ingest;
		std::optional<StreamErrorOutcome> error;
	};

	// The assistant response as the UI should draw it.
	struct ResponseView {
		// False before anything has been streamed.
		bool active = false;

		std::string conversation_id;
		std::string turn_id;
		std::string message_id;

		ResponseStreamState state = ResponseStreamState::idle;

		// What to render. While streaming, the fragments in sequence order; once
		// stopped, the server's assembled text.
		std::string text;

		// No start envelope was seen for this turn.
		bool opened_implicitly = false;

		std::size_t applied_delta_count = 0;
		std::optional<long long> highest_sequence_seen;

		// Sequence numbers below the highest seen that never arrived, ascending. The
		// UI can say so; it does not have to.
		std::vector<long long> missing_sequences;

		// True while sequence numbers are outstanding. Requirement 16.1's detection,
		// in the one field a caller needs to read.
		bool text_may_be_incomplete = false;

		// From the stop payload. Absent while streaming, and absent after a stop whose
		// own `stopReason` was null.
		std::optional<std::string> stop_reason;

		bool reconciled_with_assembled_text = false;
	};

	// The ingest details, plaintext, for the CEF bridge and nowhere else
	// (requirement 16.2).
	//
	// Built on demand and not stored, so the revealed token lives as long as the call
	// that shows it. The field name is deliberately unpleasant to see in a log line.
	struct StreamIngestDisplay {
		std::string stage_arn;

		// ReaCast's destination field.
		std::string rtmps_ingest_url;

		// ReaCast's stream key field. Full plaintext, by default, because the producer
		// transcribes it by hand.
		std::string stream_key_plaintext;

		std::string participant_id;

		// ISO-8601, shown beside the token so an expiry is visible before it bites.
		std::string expires_at;

		// This token replaced an earlier one.
		bool reissued = false;
	};

	// Deleted for the same reason as StreamKey's, one step further out: this struct
	// exists to be serialized across the bridge, and it is the other object in this
	// header that must never reach a log.
	template <typename OutputStreamType>
	OutputStreamType& operator<<(OutputStreamType&, const StreamIngestDisplay&) = delete;

	// The same details, minus the credential — the log-facing half of the split.
	//
	// There is no field for the token and no way to reach one from here, so a log line
	// written from this type is safe by construction rather than by care.
	// `participantId` is documented in the schema as safe to log and is the identifier
	// that correlates with IVS stage events, which is what a diagnostic actually needs.
	struct StreamIngestLogFields {
		std::string stage_arn;
		std::string rtmps_ingest_url;
		std::string participant_id;
		std::string expires_at;
		bool reissued = false;

		// Length only. Distinguishes "no token" from "a token ReaCast refuses".
		std::size_t stream_key_length = 0;

		// One line, ready to log.
		std::string describe_for_log() const
		{
			std::string description;

			description.append("stage=").append(stage_arn);
			description.append(" ingest=").append(rtmps_ingest_url);
			description.append(" participant=").append(participant_id);
			description.append(" expires=").append(expires_at);
			description.append(" reissued=").append(reissued ? "true" : "false");
			description.append(" stream_key=<redacted, ")
				.append(std::to_string(stream_key_length))
				.append(" characters>");

			return description;
		}
	};

	// The last `stream:error`, for the UI to show. `message` is documented as safe to
	// display to a producer.
	struct StreamErrorView {
		std::string code;
		std::string message;

		// The envelope type that triggered it, when the error answers one.
		std::string envelope_type;

		// Rendered schema validation failures, when that was the cause.
		std::string validation_errors;
	};

	// Assembles the streamed assistant response and holds the IVS publish details.
	//
	// Single-threaded and main-thread only: every entry point is called from the tick
	// that drains the inbound queue, and every view is read from the same thread when
	// the UI Host publishes. There is no locking here because there is no second
	// thread — the queue pair is the thread boundary (requirement 22.2).
	class StreamPresenter {
	public:
		StreamPresenter() = default;

		StreamPresenter(const StreamPresenter&) = delete;
		StreamPresenter& operator=(const StreamPresenter&) = delete;
		StreamPresenter(StreamPresenter&&) = delete;
		StreamPresenter& operator=(StreamPresenter&&) = delete;

		// ------------------------------------------------------------------
		// Envelope entry points
		// ------------------------------------------------------------------

		// The entry point for the Message Dispatcher: hand it an inbound envelope it
		// routed here and it does the rest.
		//
		// A member template so the JSON-touching reads are instantiated only where the
		// real envelope is used — see the file comment. The envelope is taken by const
		// reference because nothing here keeps any part of it; every field that is
		// retained is copied out.
		template <typename InboundEnvelopeType>
		StreamEnvelopeOutcome handle(const InboundEnvelopeType& inbound_envelope)
		{
			StreamEnvelopeOutcome outcome;
			outcome.envelope_type = inbound_envelope.type;

			const std::string_view envelope_type{outcome.envelope_type};

			if (envelope_type == agent_response_start_envelope_type) {
				outcome.recognised = true;
				outcome.start = handle_agent_response_start(
					read_string(inbound_envelope.payload, "conversationId"),
					read_string(inbound_envelope.payload, "turnId"),
					read_string(inbound_envelope.payload, "messageId")
				);

				return outcome;
			}

			if (envelope_type == agent_response_delta_envelope_type) {
				outcome.recognised = true;
				outcome.delta = handle_agent_response_delta(
					read_string(inbound_envelope.payload, "conversationId"),
					read_string(inbound_envelope.payload, "turnId"),
					read_string(inbound_envelope.payload, "messageId"),
					read_integer(inbound_envelope.payload, "sequence", invalid_sequence),
					read_string(inbound_envelope.payload, "text")
				);

				return outcome;
			}

			if (envelope_type == agent_response_stop_envelope_type) {
				outcome.recognised = true;
				outcome.stop = handle_agent_response_stop(
					read_string(inbound_envelope.payload, "conversationId"),
					read_string(inbound_envelope.payload, "turnId"),
					read_string(inbound_envelope.payload, "messageId"),
					read_string(inbound_envelope.payload, "text"),
					read_optional_string(inbound_envelope.payload, "stopReason"),
					read_integer(inbound_envelope.payload, "deltaCount", 0)
				);

				return outcome;
			}

			if (envelope_type == stream_rtmps_url_envelope_type) {
				outcome.recognised = true;
				outcome.ingest = handle_stream_rtmps_url(
					read_string(inbound_envelope.payload, "stageArn"),
					read_string(inbound_envelope.payload, "rtmpsIngestUrl"),
					StreamKey{read_string(inbound_envelope.payload, "streamKey")},
					read_string(inbound_envelope.payload, "participantId"),
					read_string(inbound_envelope.payload, "expiresAt"),
					read_boolean(inbound_envelope.payload, "reissued", false)
				);

				return outcome;
			}

			if (envelope_type == stream_error_envelope_type) {
				outcome.recognised = true;
				outcome.error = handle_stream_error(
					read_string(inbound_envelope.payload, "code"),
					read_string(inbound_envelope.payload, "message"),
					read_string(inbound_envelope.payload, "envelopeType"),
					read_string(inbound_envelope.payload, "validationErrors")
				);

				return outcome;
			}

			// Any of the seven `error:*` types (requirements 5.5, 23.2). They fold into
			// the same view as `stream:error`, with two differences that are both about
			// not inventing anything.
			//
			// `envelopeType` is the arriving type rather than a field read from the
			// payload. A `stream:error` answers *another* envelope and names it inside
			// its own payload; an `error:*` envelope *is* the error, so the type the
			// producer needs to see is this one. Reading a payload field here would
			// answer empty, and the view's `envelopeType` would be blank for exactly
			// the seven types that have no schema to have put one in.
			//
			// And the payload is read defensively — no bundled schema means the codec
			// vouched for nothing, so a missing `code` or `message` is an empty string
			// rather than a reason to drop the error. An error the producer is not shown
			// is the condition requirement 23.2 exists to prevent.
			if (is_error_envelope_type(envelope_type)) {
				outcome.recognised = true;
				outcome.error = handle_stream_error(
					read_string(inbound_envelope.payload, "code"),
					read_string(inbound_envelope.payload, "message"),
					envelope_type,
					read_string(inbound_envelope.payload, "validationErrors")
				);

				return outcome;
			}

			// Not one of the five and not an error. Requirement 5.8's answer to an
			// unknown type: report it, change nothing.
			return outcome;
		}

		// ------------------------------------------------------------------
		// Field-level entry points — the same work with the payload already taken
		// apart, which is the form the sequencing rules are actually about.
		// ------------------------------------------------------------------

		ResponseStartOutcome handle_agent_response_start(
			std::string_view conversation_id,
			std::string_view turn_id,
			std::string_view message_id)
		{
			ResponseStartOutcome outcome;
			outcome.turn_id.assign(turn_id);

			// A start for the turn already open. Idempotent: a reconnect can replay it,
			// and rebuilding the response would discard deltas already rendered — or,
			// after a stop, the assembled text itself.
			if (response_active_ && matches(turn_id_, turn_id)) {
				outcome.ignored_duplicate_start = true;

				return outcome;
			}

			outcome.superseded_previous_turn = response_active_
				&& state_ == ResponseStreamState::streaming;

			open_response(conversation_id, turn_id, message_id, false);
			outcome.opened_response = true;

			return outcome;
		}

		ResponseDeltaOutcome handle_agent_response_delta(
			std::string_view conversation_id,
			std::string_view turn_id,
			std::string_view message_id,
			long long sequence,
			std::string_view text)
		{
			ResponseDeltaOutcome outcome;
			outcome.turn_id.assign(turn_id);
			outcome.sequence = sequence;

			// The schema's `sequence` is an integer with a minimum of zero, so anything
			// else means the codec was bypassed or the protocol moved. There is no index
			// to place the text at and no sensible guess — appending it would put a
			// fragment at whatever position happened to be next.
			if (sequence < 0) {
				outcome.disposition = DeltaDisposition::ignored_invalid_sequence;
				outcome.missing_sequence_count = missing_sequences_.size();

				return outcome;
			}

			if (!response_active_ || !matches(turn_id_, turn_id)) {
				outcome.superseded_previous_turn = response_active_
					&& state_ == ResponseStreamState::streaming;

				open_response(conversation_id, turn_id, message_id, true);
				outcome.opened_response_implicitly = true;
			} else if (state_ == ResponseStreamState::ended) {
				// The stop's assembled text is the complete message. A straggler cannot
				// improve on it and appending it would duplicate a fragment.
				outcome.disposition = DeltaDisposition::ignored_after_stop;
				outcome.missing_sequence_count = missing_sequences_.size();

				return outcome;
			}

			const auto existing_fragment = fragments_.find(sequence);

			if (existing_fragment != fragments_.end()) {
				outcome.disposition = matches(existing_fragment->second, text)
					? DeltaDisposition::ignored_duplicate
					: DeltaDisposition::ignored_conflicting_duplicate;
				outcome.missing_sequence_count = missing_sequences_.size();

				return outcome;
			}

			fragments_.emplace(sequence, std::string{text});
			++applied_delta_count_;

			if (!highest_sequence_seen_.has_value() || sequence > *highest_sequence_seen_) {
				highest_sequence_seen_ = sequence;
			}

			if (sequence >= next_expected_sequence_) {
				if (sequence > next_expected_sequence_) {
					// Everything between the next expected and this one is missing.
					for (long long missing = next_expected_sequence_; missing < sequence; ++missing) {
						missing_sequences_.insert(missing);
					}

					outcome.disposition = DeltaDisposition::applied_after_gap;
					outcome.gap_detected = true;
				} else {
					outcome.disposition = DeltaDisposition::applied_in_order;
				}

				next_expected_sequence_ = sequence + 1;

				// This fragment belongs at the end of everything held, so the cached text
				// can be extended rather than rebuilt. True even with earlier holes: the
				// cache is the concatenation of the fragments that are present, in order.
				rendered_text_.append(text);
			} else {
				// A late arrival, in a slot that was empty. It belongs before text
				// already rendered, so the cache is rebuilt from the map.
				missing_sequences_.erase(sequence);
				outcome.disposition = DeltaDisposition::applied_filling_gap;
				rebuild_rendered_text();
			}

			outcome.applied = true;
			outcome.missing_sequence_count = missing_sequences_.size();
			++state_revision_;

			return outcome;
		}

		ResponseStopOutcome handle_agent_response_stop(
			std::string_view conversation_id,
			std::string_view turn_id,
			std::string_view message_id,
			std::string_view assembled_text,
			const std::optional<std::string>& stop_reason,
			long long reported_delta_count)
		{
			ResponseStopOutcome outcome;
			outcome.turn_id.assign(turn_id);
			outcome.reported_delta_count = reported_delta_count;

			if (response_active_ && matches(turn_id_, turn_id)
				&& state_ == ResponseStreamState::ended) {
				outcome.ignored_duplicate_stop = true;
				outcome.applied_delta_count = applied_delta_count_;
				outcome.delta_count_matched =
					reported_delta_count == static_cast<long long>(applied_delta_count_);

				return outcome;
			}

			if (!response_active_ || !matches(turn_id_, turn_id)) {
				// A stop for a turn never seen. The payload carries the whole response,
				// which is exactly what the schema says it is for: a client that joined
				// mid-stream renders the complete message from it.
				outcome.superseded_previous_turn = response_active_
					&& state_ == ResponseStreamState::streaming;

				open_response(conversation_id, turn_id, message_id, true);
				outcome.opened_response_implicitly = true;
			}

			outcome.missing_sequence_count_before_stop = missing_sequences_.size();

			// An empty `text` with deltas applied is the one case where the payload is
			// not an improvement. The schema permits it, and honouring it would blank a
			// response the producer has been reading.
			if (!assembled_text.empty() || applied_delta_count_ == 0) {
				rendered_text_.assign(assembled_text);
				outcome.text_replaced_with_assembled_text = true;
				outcome.gaps_repaired_by_assembled_text = !missing_sequences_.empty();
				missing_sequences_.clear();
				reconciled_with_assembled_text_ = true;
			} else {
				outcome.assembled_text_was_empty = true;
			}

			if (stop_reason.has_value()) {
				stop_reason_ = stop_reason;
			}

			state_ = ResponseStreamState::ended;
			outcome.accepted = true;
			outcome.applied_delta_count = applied_delta_count_;
			outcome.delta_count_matched =
				reported_delta_count == static_cast<long long>(applied_delta_count_);
			++state_revision_;

			return outcome;
		}

		// Requirements 16.2 and 16.3. The token arrives by value because the presenter
		// keeps it.
		StreamIngestOutcome handle_stream_rtmps_url(
			std::string_view stage_arn,
			std::string_view rtmps_ingest_url,
			StreamKey stream_key,
			std::string_view participant_id,
			std::string_view expires_at,
			bool reissued)
		{
			StreamIngestOutcome outcome;
			outcome.reissued = reissued;

			// Every one of these is required and non-empty in the schema. Refusing the
			// envelope keeps whatever the producer is already publishing with, which is
			// the safer of the two failures.
			if (stage_arn.empty() || rtmps_ingest_url.empty() || stream_key.empty()
				|| participant_id.empty() || expires_at.empty()) {
				outcome.rejected_empty_field = true;

				return outcome;
			}

			outcome.replaced_previous_details = ingest_details_.has_value();
			outcome.reissue_flag_disagreed_with_state =
				reissued != outcome.replaced_previous_details;

			if (ingest_details_.has_value()) {
				outcome.stage_changed = !matches(ingest_details_->stage_arn, stage_arn);
				outcome.ingest_endpoint_changed =
					!matches(ingest_details_->rtmps_ingest_url, rtmps_ingest_url);
				outcome.stream_key_changed = ingest_details_->stream_key != stream_key;
				outcome.expiry_changed = !matches(ingest_details_->expires_at, expires_at);
			} else {
				outcome.stage_changed = true;
				outcome.ingest_endpoint_changed = true;
				outcome.stream_key_changed = true;
				outcome.expiry_changed = true;
			}

			IngestDetails details;
			details.stage_arn.assign(stage_arn);
			details.rtmps_ingest_url.assign(rtmps_ingest_url);
			details.stream_key = std::move(stream_key);
			details.participant_id.assign(participant_id);
			details.expires_at.assign(expires_at);
			details.reissued = reissued;

			ingest_details_ = std::move(details);
			outcome.accepted = true;
			++state_revision_;

			return outcome;
		}

		StreamErrorOutcome handle_stream_error(
			std::string_view code,
			std::string_view message,
			std::string_view envelope_type = {},
			std::string_view validation_errors = {})
		{
			StreamErrorOutcome outcome;

			StreamErrorView error;
			error.code.assign(code);
			error.message.assign(message);
			error.envelope_type.assign(envelope_type);
			error.validation_errors.assign(validation_errors);

			last_stream_error_ = std::move(error);
			outcome.recorded = true;

			// Deliberately not clearing the ingest details, and deliberately not
			// touching the response state — a `stream:error` is about audio streaming
			// setup, not about the assistant's answer.
			outcome.retained_existing_ingest_details = ingest_details_.has_value();
			++state_revision_;

			return outcome;
		}

		// ------------------------------------------------------------------
		// Views — what the UI Host publishes across the bridge
		// ------------------------------------------------------------------

		ResponseView current_response() const
		{
			ResponseView view;
			view.active = response_active_;

			if (!response_active_) {
				return view;
			}

			view.conversation_id = conversation_id_;
			view.turn_id = turn_id_;
			view.message_id = message_id_;
			view.state = state_;
			view.text = rendered_text_;
			view.opened_implicitly = opened_implicitly_;
			view.applied_delta_count = applied_delta_count_;
			view.highest_sequence_seen = highest_sequence_seen_;
			view.missing_sequences.assign(missing_sequences_.begin(), missing_sequences_.end());
			view.text_may_be_incomplete = !missing_sequences_.empty();
			view.stop_reason = stop_reason_;
			view.reconciled_with_assembled_text = reconciled_with_assembled_text_;

			return view;
		}

		// The text to render, without building a whole view. The UI Host reads this on
		// every delta.
		const std::string& rendered_text() const noexcept
		{
			return rendered_text_;
		}

		ResponseStreamState response_state() const noexcept
		{
			return state_;
		}

		// Requirement 16.1's detection, as one question.
		bool response_text_may_be_incomplete() const noexcept
		{
			return !missing_sequences_.empty();
		}

		// The ingest details in plaintext, for the producer (requirement 16.2).
		//
		// Absent until a `stream:rtmps_url` has been accepted. Named for what it is
		// for, and the only route to the token.
		std::optional<StreamIngestDisplay> ingest_display() const
		{
			if (!ingest_details_.has_value()) {
				return std::nullopt;
			}

			StreamIngestDisplay display;
			display.stage_arn = ingest_details_->stage_arn;
			display.rtmps_ingest_url = ingest_details_->rtmps_ingest_url;
			display.stream_key_plaintext = ingest_details_->stream_key.reveal_for_display();
			display.participant_id = ingest_details_->participant_id;
			display.expires_at = ingest_details_->expires_at;
			display.reissued = ingest_details_->reissued;

			return display;
		}

		// The same details with no field for the credential (requirements 16.4, 26.4).
		std::optional<StreamIngestLogFields> ingest_log_fields() const
		{
			if (!ingest_details_.has_value()) {
				return std::nullopt;
			}

			StreamIngestLogFields fields;
			fields.stage_arn = ingest_details_->stage_arn;
			fields.rtmps_ingest_url = ingest_details_->rtmps_ingest_url;
			fields.participant_id = ingest_details_->participant_id;
			fields.expires_at = ingest_details_->expires_at;
			fields.reissued = ingest_details_->reissued;
			fields.stream_key_length = ingest_details_->stream_key.size();

			return fields;
		}

		bool has_ingest_details() const noexcept
		{
			return ingest_details_.has_value();
		}

		const std::optional<StreamErrorView>& last_stream_error() const noexcept
		{
			return last_stream_error_;
		}

		// Increments on every accepted change. The UI Host compares it against what it
		// last published, so a tick that changed nothing sends nothing across the
		// bridge.
		std::uint64_t state_revision() const noexcept
		{
			return state_revision_;
		}

		// Clears everything, including the ingest details and the token.
		//
		// For teardown and for a session ending. Not called on a reconnect: the point
		// of the gap handling above is that a reconnect resumes a response rather than
		// restarting it, and the publish token survives a socket drop.
		void reset()
		{
			response_active_ = false;
			opened_implicitly_ = false;
			state_ = ResponseStreamState::idle;
			conversation_id_.clear();
			turn_id_.clear();
			message_id_.clear();
			rendered_text_.clear();
			fragments_.clear();
			missing_sequences_.clear();
			highest_sequence_seen_.reset();
			next_expected_sequence_ = 0;
			applied_delta_count_ = 0;
			stop_reason_.reset();
			reconciled_with_assembled_text_ = false;
			ingest_details_.reset();
			last_stream_error_.reset();
			++state_revision_;
		}

		// What `sequence` reads as when the payload had no usable one.
		static constexpr long long invalid_sequence = -1;

	private:
		// The stored details. The only place a StreamKey is held.
		struct IngestDetails {
			std::string stage_arn;
			std::string rtmps_ingest_url;
			StreamKey stream_key;
			std::string participant_id;
			std::string expires_at;
			bool reissued = false;
		};

		// std::string against std::string_view without relying on the mixed-type
		// comparison overloads, which differ in availability between standard
		// libraries at C++17.
		static bool matches(const std::string& held, std::string_view candidate) noexcept
		{
			return std::string_view{held} == candidate;
		}

		void open_response(
			std::string_view conversation_id,
			std::string_view turn_id,
			std::string_view message_id,
			bool implicitly)
		{
			response_active_ = true;
			opened_implicitly_ = implicitly;
			state_ = ResponseStreamState::streaming;
			conversation_id_.assign(conversation_id);
			turn_id_.assign(turn_id);
			message_id_.assign(message_id);
			rendered_text_.clear();
			fragments_.clear();
			missing_sequences_.clear();
			highest_sequence_seen_.reset();
			next_expected_sequence_ = 0;
			applied_delta_count_ = 0;
			stop_reason_.reset();
			reconciled_with_assembled_text_ = false;
			++state_revision_;
		}

		// The concatenation of every fragment held, in sequence order. Only needed
		// after an out-of-order insert; the common path appends.
		void rebuild_rendered_text()
		{
			rendered_text_.clear();

			for (const auto& fragment : fragments_) {
				rendered_text_.append(fragment.second);
			}
		}

		// ------------------------------------------------------------------
		// Payload reads
		//
		// Defensive rather than assuming the codec ran: a missing or wrongly typed
		// field yields the fallback, which each handler then treats as its own "not
		// usable" case. One path for bad input, rather than an exception from the JSON
		// library crossing into the main-thread tick.
		// ------------------------------------------------------------------

		template <typename PayloadType>
		static std::string read_string(const PayloadType& payload, const char* field_name)
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

		// Absent, null, or not a string all read as absent — which is what a nullable
		// schema field means to a presenter that has nothing to show for it.
		template <typename PayloadType>
		static std::optional<std::string> read_optional_string(
			const PayloadType& payload,
			const char* field_name)
		{
			if (!payload.is_object() || !payload.contains(field_name)) {
				return std::nullopt;
			}

			const auto& field = payload.at(field_name);

			if (!field.is_string()) {
				return std::nullopt;
			}

			return field.template get<std::string>();
		}

		template <typename PayloadType>
		static long long read_integer(
			const PayloadType& payload,
			const char* field_name,
			long long fallback)
		{
			if (!payload.is_object() || !payload.contains(field_name)) {
				return fallback;
			}

			const auto& field = payload.at(field_name);

			if (!field.is_number_integer()) {
				return fallback;
			}

			return field.template get<long long>();
		}

		template <typename PayloadType>
		static bool read_boolean(
			const PayloadType& payload,
			const char* field_name,
			bool fallback)
		{
			if (!payload.is_object() || !payload.contains(field_name)) {
				return fallback;
			}

			const auto& field = payload.at(field_name);

			if (!field.is_boolean()) {
				return fallback;
			}

			return field.template get<bool>();
		}

		bool response_active_ = false;
		bool opened_implicitly_ = false;
		ResponseStreamState state_ = ResponseStreamState::idle;

		std::string conversation_id_;
		std::string turn_id_;
		std::string message_id_;

		// The concatenation of fragments_, cached because the UI reads it on every
		// delta and the deltas are single tokens.
		std::string rendered_text_;

		// Keyed by sequence, so order is the container's business rather than arrival
		// order's. This is what makes an out-of-order delta placeable.
		std::map<long long, std::string> fragments_;

		// Sequence numbers below the highest seen that have not arrived.
		std::set<long long> missing_sequences_;

		std::optional<long long> highest_sequence_seen_;
		long long next_expected_sequence_ = 0;
		std::size_t applied_delta_count_ = 0;

		std::optional<std::string> stop_reason_;
		bool reconciled_with_assembled_text_ = false;

		std::optional<IngestDetails> ingest_details_;
		std::optional<StreamErrorView> last_stream_error_;

		std::uint64_t state_revision_ = 0;
	};

}

#endif
