// The Stream Presenter.
//
// Two things here are worth real effort, and they are the two things the component
// exists to get right.
//
// **The delta stream, when it misbehaves.** The server emits sequence numbers 0, 1,
// 2, ... with no holes, so every anomaly the presenter can see was introduced by a
// socket that dropped and resumed — which makes all four of them reachable and none
// of them exercised by the happy path. A gap, a late delta, a duplicate, and a delta
// with no preceding start each get a case, and so do the two turn-boundary variants:
// a delta for a new turn while one is streaming, and a delta for a turn that already
// stopped. The failure being defended against is not a crash. It is a producer
// reading an answer that is quietly missing a clause, or has one in the wrong place.
//
// **The stream key never reaching a log.** Requirement 16.4 is a prohibition, and a
// test that asserts one log line is clean proves nothing about the next one somebody
// adds. So what is tested is the type: that `log << stream_key` does not compile,
// that the token does not convert to a string, and that the log-facing view has no
// field capable of carrying it. Alongside that, the inverse — requirement 16.2 wants
// the token displayed in full plaintext, because the producer types it into ReaCast
// by hand, so a case asserts the display carries it verbatim. The two are easy to
// conflate and the component is only correct if both hold at once.
//
// No REAPER and no CEF. The presenter is state; the UI Host publishes it and the
// React UI draws it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ui/stream_presenter.h>

using sesh_ai::ui::DeltaDisposition;
using sesh_ai::ui::ResponseDeltaOutcome;
using sesh_ai::ui::ResponseStartOutcome;
using sesh_ai::ui::ResponseStopOutcome;
using sesh_ai::ui::ResponseStreamState;
using sesh_ai::ui::ResponseView;
using sesh_ai::ui::StreamEnvelopeOutcome;
using sesh_ai::ui::StreamIngestDisplay;
using sesh_ai::ui::StreamIngestLogFields;
using sesh_ai::ui::StreamIngestOutcome;
using sesh_ai::ui::StreamKey;
using sesh_ai::ui::StreamPresenter;
using sesh_ai::ui::agent_response_delta_envelope_type;
using sesh_ai::ui::agent_response_start_envelope_type;
using sesh_ai::ui::agent_response_stop_envelope_type;
using sesh_ai::ui::is_stream_presenter_envelope_type;
using sesh_ai::ui::stream_error_envelope_type;
using sesh_ai::ui::stream_rtmps_url_envelope_type;

namespace {

	constexpr std::string_view conversation_id{"01HZK8QW0RCONVERSATION"};
	constexpr std::string_view turn_id{"01HZK8QW0RTURNONE"};
	constexpr std::string_view other_turn_id{"01HZK8QW0RTURNTWO"};
	constexpr std::string_view message_id{"01HZK8QW0RMESSAGE"};

	// A representative IVS publish participant token — long, opaque, and the thing
	// that must not appear in a log line. Distinctive enough that a substring search
	// for it means something.
	constexpr std::string_view publish_token{
		"eyJhbGciOiJLTVMiLCJ0eXAiOiJKV1QifQ.PUBLISH-PARTICIPANT-TOKEN-a7f3c91e"
	};

	constexpr std::string_view reissued_publish_token{
		"eyJhbGciOiJLTVMiLCJ0eXAiOiJKV1QifQ.PUBLISH-PARTICIPANT-TOKEN-b208d4f6"
	};

	constexpr std::string_view rtmps_ingest_url{
		"rtmps://a1b2c3d4e5f6.global-contribute.live-video.net:443/app/"
	};

	constexpr std::string_view stage_arn{
		"arn:aws:ivs:us-east-1:111122223333:stage/AbCdEf123456"
	};

	constexpr std::string_view participant_id{"xYz987ParticipantId"};
	constexpr std::string_view expires_at{"2026-06-23T18:45:00Z"};

	ResponseDeltaOutcome push_delta(
		StreamPresenter& presenter,
		long long sequence,
		std::string_view text,
		std::string_view turn = turn_id)
	{
		return presenter.handle_agent_response_delta(
			conversation_id,
			turn,
			message_id,
			sequence,
			text
		);
	}

	ResponseStopOutcome push_stop(
		StreamPresenter& presenter,
		std::string_view assembled_text,
		long long reported_delta_count,
		std::string_view turn = turn_id)
	{
		return presenter.handle_agent_response_stop(
			conversation_id,
			turn,
			message_id,
			assembled_text,
			std::optional<std::string>{"end_turn"},
			reported_delta_count
		);
	}

	StreamIngestOutcome push_rtmps_url(
		StreamPresenter& presenter,
		std::string_view token,
		bool reissued,
		std::string_view expiry = expires_at)
	{
		return presenter.handle_stream_rtmps_url(
			stage_arn,
			rtmps_ingest_url,
			StreamKey{std::string{token}},
			participant_id,
			expiry,
			reissued
		);
	}

	// Whether `stream << value` is a valid expression.
	//
	// SFINAE rather than a compile-failure comment, so the prohibition is an assertion
	// in the suite rather than a claim in a file header. A deleted overload makes the
	// expression ill-formed in the immediate context, which is a substitution failure,
	// which is what this detects.
	template <typename StreamType, typename ValueType, typename = void>
	struct is_stream_insertable : std::false_type {
	};

	template <typename StreamType, typename ValueType>
	struct is_stream_insertable<
		StreamType,
		ValueType,
		std::void_t<decltype(std::declval<StreamType&>() << std::declval<const ValueType&>())>
	> : std::true_type {
	};

	// Stands in for a parsed nlohmann::json payload.
	//
	// nlohmann/json is an optional dependency of this suite, so the presenter's
	// envelope entry point is a member template and this is what the suite instantiates
	// it with. It implements exactly the operations the payload reads perform —
	// is_object, contains, at, is_string, is_number_integer, is_boolean, and a
	// templated get — and the explicit instantiation in stream_presenter.cpp is what
	// proves the real nlohmann::json satisfies the same reads.
	struct StubJsonValue {
		enum class Kind {
			object,
			string,
			integer,
			boolean
		};

		Kind kind = Kind::object;
		std::string string_value;
		long long integer_value = 0;
		bool boolean_value = false;
		std::map<std::string, StubJsonValue> members;

		static StubJsonValue of_string(std::string_view value)
		{
			StubJsonValue json;
			json.kind = Kind::string;
			json.string_value.assign(value);

			return json;
		}

		static StubJsonValue of_integer(long long value)
		{
			StubJsonValue json;
			json.kind = Kind::integer;
			json.integer_value = value;

			return json;
		}

		static StubJsonValue of_boolean(bool value)
		{
			StubJsonValue json;
			json.kind = Kind::boolean;
			json.boolean_value = value;

			return json;
		}

		bool is_object() const
		{
			return kind == Kind::object;
		}

		bool is_string() const
		{
			return kind == Kind::string;
		}

		bool is_number_integer() const
		{
			return kind == Kind::integer;
		}

		bool is_boolean() const
		{
			return kind == Kind::boolean;
		}

		bool contains(const std::string& field_name) const
		{
			return members.find(field_name) != members.end();
		}

		const StubJsonValue& at(const std::string& field_name) const
		{
			return members.at(field_name);
		}

		template <typename ValueType>
		ValueType get() const
		{
			if constexpr (std::is_same_v<ValueType, std::string>) {
				return string_value;
			} else if constexpr (std::is_same_v<ValueType, bool>) {
				return boolean_value;
			} else {
				return static_cast<ValueType>(integer_value);
			}
		}
	};

	// Stands in for transport/envelope.h's InboundEnvelope.
	struct StubInboundEnvelope {
		std::string type;
		std::string request_id;
		StubJsonValue payload;
	};

}

TEST_CASE("the presenter names the five stream envelope types it consumes", "[ui][stream]")
{
	STATIC_REQUIRE(agent_response_start_envelope_type == "stream:agent_response_start");
	STATIC_REQUIRE(agent_response_delta_envelope_type == "stream:agent_response_delta");
	STATIC_REQUIRE(agent_response_stop_envelope_type == "stream:agent_response_stop");
	STATIC_REQUIRE(stream_rtmps_url_envelope_type == "stream:rtmps_url");
	STATIC_REQUIRE(stream_error_envelope_type == "stream:error");

	STATIC_REQUIRE(is_stream_presenter_envelope_type(agent_response_start_envelope_type));
	STATIC_REQUIRE(is_stream_presenter_envelope_type(agent_response_delta_envelope_type));
	STATIC_REQUIRE(is_stream_presenter_envelope_type(agent_response_stop_envelope_type));
	STATIC_REQUIRE(is_stream_presenter_envelope_type(stream_rtmps_url_envelope_type));
	STATIC_REQUIRE(is_stream_presenter_envelope_type(stream_error_envelope_type));

	// The PWA's half of the IVS exchange. The extension publishes and the PWA
	// subscribes, so the subscribe token is deliberately not in the extension's schema
	// bundle and not a type this component answers to.
	STATIC_REQUIRE(!is_stream_presenter_envelope_type("stream:subscribe_token"));
	STATIC_REQUIRE(!is_stream_presenter_envelope_type("stream:request_audio"));
	STATIC_REQUIRE(!is_stream_presenter_envelope_type("request:transport"));
}

TEST_CASE("nothing is presented before a response starts", "[ui][stream]")
{
	const StreamPresenter presenter;

	const ResponseView view = presenter.current_response();

	REQUIRE(!view.active);
	REQUIRE(view.state == ResponseStreamState::idle);
	REQUIRE(view.text.empty());
	REQUIRE(view.applied_delta_count == 0);
	REQUIRE(!view.text_may_be_incomplete);
	REQUIRE(!presenter.has_ingest_details());
	REQUIRE(!presenter.ingest_display().has_value());
	REQUIRE(!presenter.last_stream_error().has_value());
}

TEST_CASE("an in-order delta stream renders progressively", "[ui][stream]")
{
	StreamPresenter presenter;

	const ResponseStartOutcome start = presenter.handle_agent_response_start(
		conversation_id,
		turn_id,
		message_id
	);

	REQUIRE(start.opened_response);
	REQUIRE(!start.ignored_duplicate_start);
	REQUIRE(!start.superseded_previous_turn);
	REQUIRE(presenter.response_state() == ResponseStreamState::streaming);
	REQUIRE(presenter.rendered_text().empty());

	REQUIRE(push_delta(presenter, 0, "Bouncing ").disposition == DeltaDisposition::applied_in_order);
	REQUIRE(presenter.rendered_text() == "Bouncing ");

	REQUIRE(push_delta(presenter, 1, "the drum ").disposition == DeltaDisposition::applied_in_order);
	REQUIRE(push_delta(presenter, 2, "bus.").disposition == DeltaDisposition::applied_in_order);

	const ResponseView view = presenter.current_response();

	REQUIRE(view.active);
	REQUIRE(view.state == ResponseStreamState::streaming);
	REQUIRE(view.text == "Bouncing the drum bus.");
	REQUIRE(view.conversation_id == std::string{conversation_id});
	REQUIRE(view.turn_id == std::string{turn_id});
	REQUIRE(view.message_id == std::string{message_id});
	REQUIRE(!view.opened_implicitly);
	REQUIRE(view.applied_delta_count == 3);
	REQUIRE(view.highest_sequence_seen.has_value());
	REQUIRE(*view.highest_sequence_seen == 2);
	REQUIRE(view.missing_sequences.empty());
	REQUIRE(!view.text_may_be_incomplete);
	REQUIRE(!view.stop_reason.has_value());
}

TEST_CASE("a gap in the sequence is detected and still rendered", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);

	push_delta(presenter, 0, "one ");

	const ResponseDeltaOutcome after_gap = push_delta(presenter, 3, "four");

	REQUIRE(after_gap.applied);
	REQUIRE(after_gap.disposition == DeltaDisposition::applied_after_gap);
	REQUIRE(after_gap.gap_detected);
	REQUIRE(after_gap.missing_sequence_count == 2);

	// Rendered rather than withheld: an answer that is mostly present beats a frozen
	// bubble, and the hole is usually closed moments later.
	REQUIRE(presenter.rendered_text() == "one four");

	const ResponseView view = presenter.current_response();

	REQUIRE(view.text_may_be_incomplete);
	REQUIRE(view.missing_sequences == std::vector<long long>{1, 2});
	REQUIRE(view.applied_delta_count == 2);
	REQUIRE(*view.highest_sequence_seen == 3);
}

TEST_CASE("a late delta is placed at its own index rather than appended", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);

	push_delta(presenter, 0, "Solo ");
	push_delta(presenter, 2, "bus.");

	REQUIRE(presenter.rendered_text() == "Solo bus.");
	REQUIRE(presenter.response_text_may_be_incomplete());

	const ResponseDeltaOutcome late = push_delta(presenter, 1, "the drum ");

	REQUIRE(late.applied);
	REQUIRE(late.disposition == DeltaDisposition::applied_filling_gap);
	REQUIRE(!late.gap_detected);
	REQUIRE(late.missing_sequence_count == 0);

	// Appending would have produced "Solo bus.the drum ". Text that is subtly
	// scrambled is worse than text with a visible hole in it.
	REQUIRE(presenter.rendered_text() == "Solo the drum bus.");
	REQUIRE(!presenter.response_text_may_be_incomplete());
	REQUIRE(presenter.current_response().missing_sequences.empty());
}

TEST_CASE("several out-of-order deltas still render in sequence order", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);

	push_delta(presenter, 4, "e");
	push_delta(presenter, 0, "a");
	push_delta(presenter, 3, "d");
	push_delta(presenter, 1, "b");
	push_delta(presenter, 2, "c");

	REQUIRE(presenter.rendered_text() == "abcde");
	REQUIRE(presenter.current_response().applied_delta_count == 5);
	REQUIRE(!presenter.response_text_may_be_incomplete());
}

TEST_CASE("a duplicate delta is ignored", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);

	push_delta(presenter, 0, "Raising ");
	push_delta(presenter, 1, "the vocal.");

	SECTION("an identical replay changes nothing")
	{
		const ResponseDeltaOutcome replay = push_delta(presenter, 1, "the vocal.");

		REQUIRE(!replay.applied);
		REQUIRE(replay.disposition == DeltaDisposition::ignored_duplicate);
		REQUIRE(presenter.rendered_text() == "Raising the vocal.");
		REQUIRE(presenter.current_response().applied_delta_count == 2);
	}

	SECTION("a conflicting one keeps the text already shown")
	{
		const ResponseDeltaOutcome conflicting = push_delta(presenter, 1, "the drum bus.");

		REQUIRE(!conflicting.applied);
		REQUIRE(conflicting.disposition == DeltaDisposition::ignored_conflicting_duplicate);
		REQUIRE(presenter.rendered_text() == "Raising the vocal.");
		REQUIRE(presenter.current_response().applied_delta_count == 2);
	}
}

TEST_CASE("a delta with no preceding start opens the response", "[ui][stream][gap]")
{
	StreamPresenter presenter;

	// The socket reconnected mid-turn and the start was missed. The delta carries the
	// same three identifiers the start does, so there is nothing to wait for.
	const ResponseDeltaOutcome first = push_delta(presenter, 0, "Reconnected ");

	REQUIRE(first.applied);
	REQUIRE(first.opened_response_implicitly);
	REQUIRE(first.disposition == DeltaDisposition::applied_in_order);
	REQUIRE(!first.superseded_previous_turn);

	const ResponseDeltaOutcome second = push_delta(presenter, 1, "mid-turn.");

	REQUIRE(second.applied);
	REQUIRE(!second.opened_response_implicitly);

	const ResponseView view = presenter.current_response();

	REQUIRE(view.active);
	REQUIRE(view.opened_implicitly);
	REQUIRE(view.state == ResponseStreamState::streaming);
	REQUIRE(view.text == "Reconnected mid-turn.");
	REQUIRE(view.turn_id == std::string{turn_id});
}

TEST_CASE("a delta arriving after the stop is ignored", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Done.");
	push_stop(presenter, "Done.", 1);

	const ResponseDeltaOutcome straggler = push_delta(presenter, 1, " Or not.");

	REQUIRE(!straggler.applied);
	REQUIRE(straggler.disposition == DeltaDisposition::ignored_after_stop);
	REQUIRE(presenter.rendered_text() == "Done.");
	REQUIRE(presenter.response_state() == ResponseStreamState::ended);
}

TEST_CASE("a delta for a new turn supersedes the one streaming", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Half an answer");

	// The server streams one turn at a time per conversation, so a delta for another
	// turn means this turn's stop and the next turn's start were both missed.
	const ResponseDeltaOutcome next_turn = push_delta(presenter, 0, "A new answer", other_turn_id);

	REQUIRE(next_turn.applied);
	REQUIRE(next_turn.superseded_previous_turn);
	REQUIRE(next_turn.opened_response_implicitly);

	const ResponseView view = presenter.current_response();

	REQUIRE(view.turn_id == std::string{other_turn_id});
	REQUIRE(view.text == "A new answer");
	REQUIRE(view.applied_delta_count == 1);
}

TEST_CASE("a delta with no usable sequence renders nothing", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Kept.");

	const ResponseDeltaOutcome negative = push_delta(presenter, -1, "Unplaceable.");

	REQUIRE(!negative.applied);
	REQUIRE(negative.disposition == DeltaDisposition::ignored_invalid_sequence);
	REQUIRE(presenter.rendered_text() == "Kept.");
	REQUIRE(presenter.current_response().applied_delta_count == 1);
}

TEST_CASE("a replayed start does not blank the response", "[ui][stream]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Already read by the producer.");

	const ResponseStartOutcome replay = presenter.handle_agent_response_start(
		conversation_id,
		turn_id,
		message_id
	);

	REQUIRE(replay.ignored_duplicate_start);
	REQUIRE(!replay.opened_response);
	REQUIRE(presenter.rendered_text() == "Already read by the producer.");
	REQUIRE(presenter.current_response().applied_delta_count == 1);
}

TEST_CASE("a start for a new turn supersedes the one streaming", "[ui][stream]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Half an answer");

	const ResponseStartOutcome next_turn = presenter.handle_agent_response_start(
		conversation_id,
		other_turn_id,
		message_id
	);

	REQUIRE(next_turn.opened_response);
	REQUIRE(next_turn.superseded_previous_turn);
	REQUIRE(presenter.rendered_text().empty());
	REQUIRE(presenter.current_response().turn_id == std::string{other_turn_id});
}

TEST_CASE("the stop's assembled text repairs a gap", "[ui][stream][gap]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Soloed ");
	push_delta(presenter, 2, "bus.");

	REQUIRE(presenter.response_text_may_be_incomplete());

	const ResponseStopOutcome stop = push_stop(presenter, "Soloed the drum bus.", 3);

	REQUIRE(stop.accepted);
	REQUIRE(stop.text_replaced_with_assembled_text);
	REQUIRE(stop.gaps_repaired_by_assembled_text);
	REQUIRE(stop.missing_sequence_count_before_stop == 1);
	REQUIRE(stop.reported_delta_count == 3);
	REQUIRE(stop.applied_delta_count == 2);
	REQUIRE(!stop.delta_count_matched);

	const ResponseView view = presenter.current_response();

	REQUIRE(view.state == ResponseStreamState::ended);
	REQUIRE(view.text == "Soloed the drum bus.");
	REQUIRE(!view.text_may_be_incomplete);
	REQUIRE(view.missing_sequences.empty());
	REQUIRE(view.reconciled_with_assembled_text);
	REQUIRE(view.stop_reason.has_value());
	REQUIRE(*view.stop_reason == "end_turn");
}

TEST_CASE("the stop reports a delta count that matches what was applied", "[ui][stream]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "a");
	push_delta(presenter, 1, "b");

	const ResponseStopOutcome stop = push_stop(presenter, "ab", 2);

	REQUIRE(stop.delta_count_matched);
	REQUIRE(stop.applied_delta_count == 2);
	REQUIRE(!stop.gaps_repaired_by_assembled_text);
}

TEST_CASE("a stop with empty text keeps the assembled fragments", "[ui][stream]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Read by the producer.");

	// The schema permits an empty string here, and honouring it would blank a response
	// that is already on screen.
	const ResponseStopOutcome stop = push_stop(presenter, "", 1);

	REQUIRE(stop.accepted);
	REQUIRE(stop.assembled_text_was_empty);
	REQUIRE(!stop.text_replaced_with_assembled_text);
	REQUIRE(presenter.rendered_text() == "Read by the producer.");
	REQUIRE(presenter.response_state() == ResponseStreamState::ended);
}

TEST_CASE("a stop for a turn never seen renders the complete message", "[ui][stream][gap]")
{
	StreamPresenter presenter;

	// A client that joined mid-stream. The stop payload's full text is exactly what
	// this case exists for.
	const ResponseStopOutcome stop = push_stop(presenter, "The whole answer, assembled.", 7);

	REQUIRE(stop.accepted);
	REQUIRE(stop.opened_response_implicitly);
	REQUIRE(stop.text_replaced_with_assembled_text);
	REQUIRE(!stop.delta_count_matched);

	const ResponseView view = presenter.current_response();

	REQUIRE(view.active);
	REQUIRE(view.opened_implicitly);
	REQUIRE(view.state == ResponseStreamState::ended);
	REQUIRE(view.text == "The whole answer, assembled.");
}

TEST_CASE("a duplicate stop is ignored", "[ui][stream]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "a");
	push_stop(presenter, "assembled", 1);

	const ResponseStopOutcome replay = push_stop(presenter, "something else", 1);

	REQUIRE(!replay.accepted);
	REQUIRE(replay.ignored_duplicate_stop);
	REQUIRE(presenter.rendered_text() == "assembled");
}

TEST_CASE("the ingest endpoint and publish token are presented in full plaintext", "[ui][stream][ivs]")
{
	StreamPresenter presenter;

	const StreamIngestOutcome outcome = push_rtmps_url(presenter, publish_token, false);

	REQUIRE(outcome.accepted);
	REQUIRE(!outcome.replaced_previous_details);
	REQUIRE(!outcome.reissued);
	REQUIRE(!outcome.reissue_flag_disagreed_with_state);
	REQUIRE(outcome.ingest_endpoint_changed);
	REQUIRE(outcome.stream_key_changed);

	const std::optional<StreamIngestDisplay> display = presenter.ingest_display();

	REQUIRE(display.has_value());

	// Requirement 16.2: full plaintext by default. The producer transcribes both of
	// these into ReaCast by hand, so anything masked or elided is unusable.
	REQUIRE(display->rtmps_ingest_url == std::string{rtmps_ingest_url});
	REQUIRE(display->stream_key_plaintext == std::string{publish_token});
	REQUIRE(display->expires_at == std::string{expires_at});
	REQUIRE(display->stage_arn == std::string{stage_arn});
	REQUIRE(display->participant_id == std::string{participant_id});
	REQUIRE(!display->reissued);
}

TEST_CASE("a reissued token replaces the displayed one", "[ui][stream][ivs]")
{
	StreamPresenter presenter;
	push_rtmps_url(presenter, publish_token, false);

	const StreamIngestOutcome reissue = push_rtmps_url(
		presenter,
		reissued_publish_token,
		true,
		"2026-06-24T06:45:00Z"
	);

	REQUIRE(reissue.accepted);
	REQUIRE(reissue.replaced_previous_details);
	REQUIRE(reissue.reissued);
	REQUIRE(reissue.stream_key_changed);
	REQUIRE(reissue.expiry_changed);
	REQUIRE(!reissue.ingest_endpoint_changed);
	REQUIRE(!reissue.stage_changed);
	REQUIRE(!reissue.reissue_flag_disagreed_with_state);

	// Requirement 16.3. The producer re-pastes this into ReaCast, so the previous
	// token must not still be on screen.
	const std::optional<StreamIngestDisplay> display = presenter.ingest_display();

	REQUIRE(display.has_value());
	REQUIRE(display->stream_key_plaintext == std::string{reissued_publish_token});
	REQUIRE(display->expires_at == "2026-06-24T06:45:00Z");
	REQUIRE(display->reissued);
}

TEST_CASE("a reissue that carries the same token is reported as unchanged", "[ui][stream][ivs]")
{
	StreamPresenter presenter;
	push_rtmps_url(presenter, publish_token, false);

	const StreamIngestOutcome reissue = push_rtmps_url(presenter, publish_token, true);

	REQUIRE(reissue.accepted);
	REQUIRE(reissue.replaced_previous_details);
	REQUIRE(!reissue.stream_key_changed);
	REQUIRE(!reissue.expiry_changed);
}

TEST_CASE("the reissued flag disagreeing with what is held is reported", "[ui][stream][ivs]")
{
	SECTION("reissued on a first issue")
	{
		StreamPresenter presenter;

		const StreamIngestOutcome outcome = push_rtmps_url(presenter, publish_token, true);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.reissue_flag_disagreed_with_state);
	}

	SECTION("not reissued when details are already held")
	{
		StreamPresenter presenter;
		push_rtmps_url(presenter, publish_token, false);

		const StreamIngestOutcome outcome = push_rtmps_url(presenter, reissued_publish_token, false);

		REQUIRE(outcome.accepted);
		REQUIRE(outcome.reissue_flag_disagreed_with_state);

		// Applied regardless: the token in hand is the one IVS will accept.
		REQUIRE(presenter.ingest_display()->stream_key_plaintext
			== std::string{reissued_publish_token});
	}
}

TEST_CASE("an rtmps_url with an empty required field keeps the working details", "[ui][stream][ivs]")
{
	StreamPresenter presenter;
	push_rtmps_url(presenter, publish_token, false);

	SECTION("an empty stream key")
	{
		const StreamIngestOutcome outcome = push_rtmps_url(presenter, "", true);

		REQUIRE(!outcome.accepted);
		REQUIRE(outcome.rejected_empty_field);
	}

	SECTION("an empty ingest URL")
	{
		const StreamIngestOutcome outcome = presenter.handle_stream_rtmps_url(
			stage_arn,
			"",
			StreamKey{std::string{reissued_publish_token}},
			participant_id,
			expires_at,
			true
		);

		REQUIRE(!outcome.accepted);
		REQUIRE(outcome.rejected_empty_field);
	}

	// Blank details pasted into ReaCast fail in a way that looks like ReaCast's fault,
	// so a malformed envelope does not get to overwrite a token the producer is
	// publishing with.
	REQUIRE(presenter.ingest_display()->stream_key_plaintext == std::string{publish_token});
	REQUIRE(presenter.ingest_display()->rtmps_ingest_url == std::string{rtmps_ingest_url});
}

TEST_CASE("the stream key cannot be logged", "[ui][stream][ivs][security]")
{
	SECTION("it has no stream insertion operator")
	{
		// The positive control, so the detection itself is known to work.
		STATIC_REQUIRE(is_stream_insertable<std::ostringstream, std::string>::value);
		STATIC_REQUIRE(is_stream_insertable<std::ostringstream, int>::value);

		// Requirement 16.4, as a property of the type rather than a rule about the
		// next log line somebody writes.
		STATIC_REQUIRE(!is_stream_insertable<std::ostringstream, StreamKey>::value);
		STATIC_REQUIRE(!is_stream_insertable<std::ostringstream, StreamIngestDisplay>::value);
	}

	SECTION("it does not become a string on its own")
	{
		STATIC_REQUIRE(!std::is_convertible_v<StreamKey, std::string>);
		STATIC_REQUIRE(!std::is_constructible_v<std::string, StreamKey>);

		// And it cannot be built from a string implicitly either, so a token cannot
		// arrive here from a std::string without someone writing the type's name.
		STATIC_REQUIRE(!std::is_convertible_v<std::string, StreamKey>);
		STATIC_REQUIRE(std::is_constructible_v<StreamKey, std::string>);
	}

	SECTION("the log-facing view has no field that could carry it")
	{
		StreamPresenter presenter;
		push_rtmps_url(presenter, publish_token, false);

		const std::optional<StreamIngestLogFields> fields = presenter.ingest_log_fields();

		REQUIRE(fields.has_value());

		const std::string log_line = fields->describe_for_log();

		REQUIRE(log_line.find(std::string{publish_token}) == std::string::npos);

		// Not a prefix of it either, which is what a "first eight characters" style
		// abbreviation would leave behind.
		REQUIRE(log_line.find(std::string{publish_token}.substr(0, 8)) == std::string::npos);

		// What a diagnostic actually needs is present: the participant IVS assigned,
		// the endpoint, the expiry, and the length that separates "no token" from "a
		// token ReaCast refuses".
		REQUIRE(log_line.find(std::string{participant_id}) != std::string::npos);
		REQUIRE(log_line.find(std::string{rtmps_ingest_url}) != std::string::npos);
		REQUIRE(log_line.find(std::string{expires_at}) != std::string::npos);
		REQUIRE(log_line.find("<redacted") != std::string::npos);
		REQUIRE(fields->stream_key_length == publish_token.size());
	}

	SECTION("comparing and measuring a token needs no access to it")
	{
		const StreamKey first{std::string{publish_token}};
		const StreamKey second{std::string{publish_token}};
		const StreamKey different{std::string{reissued_publish_token}};
		const StreamKey empty;

		REQUIRE(first == second);
		REQUIRE(first != different);
		REQUIRE(empty.empty());
		REQUIRE(!first.empty());
		REQUIRE(first.size() == publish_token.size());
	}
}

TEST_CASE("a stream error is recorded without disturbing what is displayed", "[ui][stream][ivs]")
{
	StreamPresenter presenter;
	push_rtmps_url(presenter, publish_token, false);
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "Still streaming.");

	const auto outcome = presenter.handle_stream_error(
		"participant_token_issuance_failed",
		"Could not issue a replacement publish token. Audio monitoring may drop.",
		"stream:request_audio",
		""
	);

	REQUIRE(outcome.recorded);
	REQUIRE(outcome.retained_existing_ingest_details);

	REQUIRE(presenter.last_stream_error().has_value());
	REQUIRE(presenter.last_stream_error()->code == "participant_token_issuance_failed");
	REQUIRE(presenter.last_stream_error()->envelope_type == "stream:request_audio");

	// A failure to issue a replacement does not invalidate the token the producer is
	// publishing with, and the assistant's answer has nothing to do with IVS.
	REQUIRE(presenter.ingest_display()->stream_key_plaintext == std::string{publish_token});
	REQUIRE(presenter.rendered_text() == "Still streaming.");
	REQUIRE(presenter.response_state() == ResponseStreamState::streaming);
}

TEST_CASE("the state revision moves only when something changed", "[ui][stream]")
{
	StreamPresenter presenter;

	const std::uint64_t at_rest = presenter.state_revision();

	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	const std::uint64_t after_start = presenter.state_revision();

	REQUIRE(after_start > at_rest);

	push_delta(presenter, 0, "a");
	const std::uint64_t after_delta = presenter.state_revision();

	REQUIRE(after_delta > after_start);

	// A duplicate changes nothing, so the UI Host has nothing to publish.
	push_delta(presenter, 0, "a");

	REQUIRE(presenter.state_revision() == after_delta);
}

TEST_CASE("reset clears the response and the credential", "[ui][stream][security]")
{
	StreamPresenter presenter;
	presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
	push_delta(presenter, 0, "a");
	push_rtmps_url(presenter, publish_token, false);
	presenter.handle_stream_error("stage_creation_failed", "No stage.");

	presenter.reset();

	REQUIRE(!presenter.current_response().active);
	REQUIRE(presenter.rendered_text().empty());
	REQUIRE(presenter.response_state() == ResponseStreamState::idle);
	REQUIRE(!presenter.has_ingest_details());
	REQUIRE(!presenter.ingest_display().has_value());
	REQUIRE(!presenter.last_stream_error().has_value());
}

TEST_CASE("the presenter handles envelopes routed to it", "[ui][stream]")
{
	StreamPresenter presenter;

	SECTION("an agent response start, delta, and stop")
	{
		StubInboundEnvelope start;
		start.type = std::string{agent_response_start_envelope_type};
		start.payload.members.emplace("conversationId", StubJsonValue::of_string(conversation_id));
		start.payload.members.emplace("turnId", StubJsonValue::of_string(turn_id));
		start.payload.members.emplace("messageId", StubJsonValue::of_string(message_id));

		const StreamEnvelopeOutcome start_outcome = presenter.handle(start);

		REQUIRE(start_outcome.recognised);
		REQUIRE(start_outcome.start.has_value());
		REQUIRE(start_outcome.start->opened_response);
		REQUIRE(presenter.current_response().turn_id == std::string{turn_id});

		StubInboundEnvelope delta;
		delta.type = std::string{agent_response_delta_envelope_type};
		delta.payload.members.emplace("conversationId", StubJsonValue::of_string(conversation_id));
		delta.payload.members.emplace("turnId", StubJsonValue::of_string(turn_id));
		delta.payload.members.emplace("messageId", StubJsonValue::of_string(message_id));
		delta.payload.members.emplace("sequence", StubJsonValue::of_integer(0));
		delta.payload.members.emplace("text", StubJsonValue::of_string("Queued the render."));

		const StreamEnvelopeOutcome delta_outcome = presenter.handle(delta);

		REQUIRE(delta_outcome.recognised);
		REQUIRE(delta_outcome.delta.has_value());
		REQUIRE(delta_outcome.delta->applied);
		REQUIRE(delta_outcome.delta->sequence == 0);
		REQUIRE(presenter.rendered_text() == "Queued the render.");

		StubInboundEnvelope stop;
		stop.type = std::string{agent_response_stop_envelope_type};
		stop.payload.members.emplace("conversationId", StubJsonValue::of_string(conversation_id));
		stop.payload.members.emplace("turnId", StubJsonValue::of_string(turn_id));
		stop.payload.members.emplace("messageId", StubJsonValue::of_string(message_id));
		stop.payload.members.emplace("text", StubJsonValue::of_string("Queued the render."));
		stop.payload.members.emplace("stopReason", StubJsonValue::of_string("end_turn"));
		stop.payload.members.emplace("deltaCount", StubJsonValue::of_integer(1));

		const StreamEnvelopeOutcome stop_outcome = presenter.handle(stop);

		REQUIRE(stop_outcome.recognised);
		REQUIRE(stop_outcome.stop.has_value());
		REQUIRE(stop_outcome.stop->accepted);
		REQUIRE(stop_outcome.stop->delta_count_matched);
		REQUIRE(presenter.current_response().state == ResponseStreamState::ended);
		REQUIRE(*presenter.current_response().stop_reason == "end_turn");
	}

	SECTION("a delta whose sequence field is absent")
	{
		StubInboundEnvelope delta;
		delta.type = std::string{agent_response_delta_envelope_type};
		delta.payload.members.emplace("conversationId", StubJsonValue::of_string(conversation_id));
		delta.payload.members.emplace("turnId", StubJsonValue::of_string(turn_id));
		delta.payload.members.emplace("messageId", StubJsonValue::of_string(message_id));
		delta.payload.members.emplace("text", StubJsonValue::of_string("Unplaceable."));

		const StreamEnvelopeOutcome outcome = presenter.handle(delta);

		REQUIRE(outcome.recognised);
		REQUIRE(outcome.delta.has_value());
		REQUIRE(!outcome.delta->applied);
		REQUIRE(outcome.delta->disposition == DeltaDisposition::ignored_invalid_sequence);
	}

	SECTION("a stop whose stopReason is null")
	{
		StubInboundEnvelope stop;
		stop.type = std::string{agent_response_stop_envelope_type};
		stop.payload.members.emplace("conversationId", StubJsonValue::of_string(conversation_id));
		stop.payload.members.emplace("turnId", StubJsonValue::of_string(turn_id));
		stop.payload.members.emplace("messageId", StubJsonValue::of_string(message_id));
		stop.payload.members.emplace("text", StubJsonValue::of_string("Ended without a reason."));
		stop.payload.members.emplace("deltaCount", StubJsonValue::of_integer(0));

		const StreamEnvelopeOutcome outcome = presenter.handle(stop);

		REQUIRE(outcome.stop.has_value());
		REQUIRE(outcome.stop->accepted);
		REQUIRE(!presenter.current_response().stop_reason.has_value());
		REQUIRE(presenter.rendered_text() == "Ended without a reason.");
	}

	SECTION("an rtmps_url")
	{
		StubInboundEnvelope ingest;
		ingest.type = std::string{stream_rtmps_url_envelope_type};
		ingest.payload.members.emplace("stageArn", StubJsonValue::of_string(stage_arn));
		ingest.payload.members.emplace("rtmpsIngestUrl", StubJsonValue::of_string(rtmps_ingest_url));
		ingest.payload.members.emplace("streamKey", StubJsonValue::of_string(publish_token));
		ingest.payload.members.emplace("participantId", StubJsonValue::of_string(participant_id));
		ingest.payload.members.emplace("expiresAt", StubJsonValue::of_string(expires_at));
		ingest.payload.members.emplace("reissued", StubJsonValue::of_boolean(true));

		const StreamEnvelopeOutcome outcome = presenter.handle(ingest);

		REQUIRE(outcome.recognised);
		REQUIRE(outcome.ingest.has_value());
		REQUIRE(outcome.ingest->accepted);
		REQUIRE(outcome.ingest->reissued);
		REQUIRE(presenter.ingest_display()->stream_key_plaintext == std::string{publish_token});
		REQUIRE(presenter.ingest_display()->expires_at == std::string{expires_at});
	}

	SECTION("a stream error")
	{
		StubInboundEnvelope error;
		error.type = std::string{stream_error_envelope_type};
		error.payload.members.emplace("code", StubJsonValue::of_string("stage_creation_failed"));
		error.payload.members.emplace("message", StubJsonValue::of_string("IVS refused the stage."));

		const StreamEnvelopeOutcome outcome = presenter.handle(error);

		REQUIRE(outcome.recognised);
		REQUIRE(outcome.error.has_value());
		REQUIRE(outcome.error->recorded);
		REQUIRE(presenter.last_stream_error()->message == "IVS refused the stage.");
	}

	SECTION("an envelope type it does not handle changes nothing")
	{
		presenter.handle_agent_response_start(conversation_id, turn_id, message_id);
		push_delta(presenter, 0, "Untouched.");

		const std::uint64_t before = presenter.state_revision();

		StubInboundEnvelope unrelated;
		unrelated.type = "stream:subscribe_token";

		const StreamEnvelopeOutcome outcome = presenter.handle(unrelated);

		REQUIRE(!outcome.recognised);
		REQUIRE(outcome.envelope_type == "stream:subscribe_token");
		REQUIRE(!outcome.start.has_value());
		REQUIRE(!outcome.delta.has_value());
		REQUIRE(!outcome.stop.has_value());
		REQUIRE(!outcome.ingest.has_value());
		REQUIRE(!outcome.error.has_value());
		REQUIRE(presenter.state_revision() == before);
		REQUIRE(presenter.rendered_text() == "Untouched.");
	}
}
