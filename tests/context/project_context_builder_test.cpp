// The Project Context Builder: when a snapshot goes out, and which envelope carries
// it.
//
// Three things are worth testing here.
//
// **The debounce does not flood** (requirement 6.4). REAPER raises no change event,
// so this is polling, and the case it exists for is a producer holding a fader: the
// change counter moves on every frame for as long as the drag lasts. The test drives
// a hundred consecutive changing ticks and asserts *nothing* was sent, then lets the
// counter settle and asserts exactly one snapshot followed. The cost claim is checked
// too — a tick with nothing due does not walk the project — because that is half of
// why the debounce is worth having: the walk happens on REAPER's main thread, and a
// quiet session should not pay for it thirty times a second.
//
// **The three reasons produce the right envelope** (requirements 6.3, 6.9). Connect
// and a debounced change send `state:project_context` unprompted; a request is
// answered with `response:project_context` carrying the same snapshot and the echoed
// requestId. The envelope types are also checked against the binding table in
// schema_validator.h, which is what makes requirement 6.8 hold — the codec validates
// what that table selects, and a snapshot under a type the table does not know would
// go out unvalidated.
//
// **A read that failed sends nothing.** A source that could not reach REAPER returns
// an unreadable reading, and the tempting thing — serialise it anyway, it is only an
// empty project — is the one thing not to do: an agent told the session has no tracks
// will act on that. The debounce is left armed so the next tick tries again.
//
// No REAPER and no JSON. The source is a project the case wrote by hand and the
// document type is substituted; what is under test is sequencing, which needs
// neither.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <context/project_context_builder.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

#include "project_context_test_doubles.h"

using sesh_ai::context::ProjectChangeDebouncer;
using sesh_ai::context::ProjectContextBuilder;
using sesh_ai::context::SnapshotDebounceSettings;
using sesh_ai::context::SnapshotPublication;
using sesh_ai::context::SnapshotReason;
using sesh_ai::context::describe_snapshot_reason;
using sesh_ai::context::envelope_type_for_snapshot_reason;
using sesh_ai::context::project_context_request_envelope_type;
using sesh_ai::context::project_context_response_envelope_type;
using sesh_ai::context::project_context_state_envelope_type;

using sesh_ai_tests::StubDocument;
using sesh_ai_tests::StubOutboundEnvelope;
using sesh_ai_tests::StubProjectContextSource;
using sesh_ai_tests::plain_reading;
using sesh_ai_tests::test_guid;

namespace
{
	using Builder = ProjectContextBuilder<StubDocument, StubOutboundEnvelope>;
	using OutboundQueue = sesh_ai::transport::ConcurrentQueue<StubOutboundEnvelope>;

	// A source that moves the change counter while the project is being walked — the
	// producer who nudged something during the snapshot.
	class ChangingWhileReadSource final : public sesh_ai::context::ProjectContextSource
	{
	public:
		sesh_ai::context::ProjectContextReading read_project_context() override
		{
			++change_count;

			return reading;
		}

		int read_project_state_change_count() override
		{
			return change_count;
		}

		sesh_ai::context::ProjectContextReading reading = plain_reading();
		int change_count = 100;
	};
}

TEST_CASE("a snapshot goes out on connect", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading = plain_reading();

	OutboundQueue outbound_queue;
	Builder builder{source};

	const SnapshotPublication publication = builder.publish_on_connect(outbound_queue);

	REQUIRE(publication.published);
	REQUIRE(publication.reason == SnapshotReason::connected);
	REQUIRE(publication.envelope_type == std::string{project_context_state_envelope_type});
	REQUIRE(publication.request_id.empty());
	REQUIRE(publication.track_count == 1);
	REQUIRE(publication.master_track_present);
	REQUIRE(source.project_reads == 1);

	const auto envelopes = outbound_queue.drain_up_to(4);

	REQUIRE(envelopes.size() == 1);
	REQUIRE(envelopes.front().type == std::string{project_context_state_envelope_type});

	// Unprompted, so no requestId. envelope.schema.json puts minLength 1 on the field,
	// so an empty string would be the codec failing its own wrapper check on every
	// snapshot — the codec omits the property when this is empty, which is why it must
	// be empty rather than a placeholder.
	REQUIRE(envelopes.front().request_id.empty());
	REQUIRE(envelopes.front().payload.at("tracks").elements().size() == 1);
}

TEST_CASE("a request is answered with the same snapshot and the echoed requestId", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading = plain_reading();

	OutboundQueue outbound_queue;
	Builder builder{source};

	const SnapshotPublication state_publication = builder.publish_on_connect(outbound_queue);
	const SnapshotPublication response_publication =
		builder.publish_on_request(outbound_queue, "req-7f3a");

	REQUIRE(state_publication.published);
	REQUIRE(response_publication.published);
	REQUIRE(response_publication.reason == SnapshotReason::requested);
	REQUIRE(response_publication.request_id == "req-7f3a");

	const auto envelopes = outbound_queue.drain_up_to(4);

	REQUIRE(envelopes.size() == 2);

	const StubOutboundEnvelope& state_envelope = envelopes.front();
	const StubOutboundEnvelope& response_envelope = envelopes.back();

	REQUIRE(state_envelope.type == std::string{project_context_state_envelope_type});
	REQUIRE(response_envelope.type == std::string{project_context_response_envelope_type});
	REQUIRE(response_envelope.request_id == "req-7f3a");

	SECTION("requirement 6.9: the payload is the same snapshot, only wrapped differently")
	{
		REQUIRE(response_envelope.payload.property_names() == state_envelope.payload.property_names());
		REQUIRE(
			response_envelope.payload.at("tracks").elements().front().at("guid").string_value()
				== state_envelope.payload.at("tracks").elements().front().at("guid").string_value()
		);
		REQUIRE(
			response_envelope.payload.at("projectName").string_value()
				== state_envelope.payload.at("projectName").string_value()
		);
	}
}

TEST_CASE("the debounce does not let a continuous edit flood the server", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading = plain_reading();
	source.change_count = 1000;

	OutboundQueue outbound_queue;

	SnapshotDebounceSettings settings;
	settings.quiet_ticks = 8;

	Builder builder{source, settings};

	SECTION("the first tick adopts the counter and sends nothing")
	{
		const SnapshotPublication publication = builder.publish_on_timer_tick(outbound_queue);

		REQUIRE_FALSE(publication.published);
		REQUIRE(publication.debounced);
		REQUIRE(source.project_reads == 0);
		REQUIRE(outbound_queue.empty());
	}

	SECTION("a hundred ticks of continuous change send nothing at all")
	{
		// The fader drag. The project is not walked once, which is the cost half of the
		// claim: the walk is a main-thread cost and a drag must not pay it per frame.
		std::size_t publications = 0;

		for (int tick = 0; tick < 100; ++tick)
		{
			++source.change_count;

			if (builder.publish_on_timer_tick(outbound_queue).published)
			{
				++publications;
			}
		}

		REQUIRE(publications == 0);
		REQUIRE(source.project_reads == 0);
		REQUIRE(outbound_queue.empty());
	}

	SECTION("and exactly one snapshot once the drag stops")
	{
		for (int tick = 0; tick < 100; ++tick)
		{
			++source.change_count;
			builder.publish_on_timer_tick(outbound_queue);
		}

		std::size_t publications = 0;

		for (std::size_t tick = 0; tick < settings.quiet_ticks * 4; ++tick)
		{
			if (builder.publish_on_timer_tick(outbound_queue).published)
			{
				++publications;
			}
		}

		REQUIRE(publications == 1);
		REQUIRE(source.project_reads == 1);

		const auto envelopes = outbound_queue.drain_up_to(8);

		REQUIRE(envelopes.size() == 1);
		REQUIRE(envelopes.front().type == std::string{project_context_state_envelope_type});
	}

	SECTION("the snapshot lands on the tick the quiet period completes, not before")
	{
		builder.publish_on_timer_tick(outbound_queue);

		++source.change_count;
		REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);

		for (std::size_t quiet_tick = 1; quiet_tick < settings.quiet_ticks; ++quiet_tick)
		{
			INFO("quiet tick: " << quiet_tick);
			REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
		}

		REQUIRE(builder.publish_on_timer_tick(outbound_queue).published);
	}

	SECTION("a quiet session after a snapshot stays quiet")
	{
		builder.publish_on_timer_tick(outbound_queue);
		++source.change_count;

		for (std::size_t tick = 0; tick < settings.quiet_ticks + 1; ++tick)
		{
			builder.publish_on_timer_tick(outbound_queue);
		}

		const std::size_t reads_after_the_change = source.project_reads;

		for (int tick = 0; tick < 200; ++tick)
		{
			REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
		}

		REQUIRE(source.project_reads == reads_after_the_change);
	}
}

TEST_CASE("the debouncer treats any different counter as a change", "[context][builder]")
{
	ProjectChangeDebouncer debouncer{SnapshotDebounceSettings{2}};

	SECTION("including one that went backwards, which is a different project")
	{
		REQUIRE_FALSE(debouncer.note_tick(5000));
		REQUIRE_FALSE(debouncer.note_tick(3));
		REQUIRE(debouncer.change_pending());

		REQUIRE_FALSE(debouncer.note_tick(3));
		REQUIRE(debouncer.note_tick(3));
	}

	SECTION("a publication by another route satisfies a pending change")
	{
		// Otherwise a request answered mid-drag is followed by a near-identical snapshot
		// a few ticks later — the flood arriving by the other door.
		REQUIRE_FALSE(debouncer.note_tick(10));
		REQUIRE_FALSE(debouncer.note_tick(11));
		REQUIRE(debouncer.change_pending());

		debouncer.note_snapshot_published(11);

		REQUIRE_FALSE(debouncer.change_pending());
		REQUIRE_FALSE(debouncer.note_tick(11));
		REQUIRE_FALSE(debouncer.note_tick(11));
	}

	SECTION("the quiet counter restarts on each further change")
	{
		REQUIRE_FALSE(debouncer.note_tick(1));
		REQUIRE_FALSE(debouncer.note_tick(2));
		REQUIRE_FALSE(debouncer.note_tick(2));
		REQUIRE(debouncer.quiet_ticks() == 1);

		REQUIRE_FALSE(debouncer.note_tick(3));
		REQUIRE(debouncer.quiet_ticks() == 0);
	}

	SECTION("a zero quiet period fires on the first still tick")
	{
		ProjectChangeDebouncer eager{SnapshotDebounceSettings{0}};

		REQUIRE_FALSE(eager.note_tick(1));
		REQUIRE_FALSE(eager.note_tick(2));
		REQUIRE(eager.note_tick(2));
	}
}

TEST_CASE("a snapshot sent on request does not produce a duplicate moments later", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading = plain_reading();
	source.change_count = 40;

	OutboundQueue outbound_queue;

	SnapshotDebounceSettings settings;
	settings.quiet_ticks = 3;

	Builder builder{source, settings};

	builder.publish_on_timer_tick(outbound_queue);

	// A change arms the debounce, and the agent asks before it fires.
	++source.change_count;
	builder.publish_on_timer_tick(outbound_queue);

	REQUIRE(builder.debouncer().change_pending());
	REQUIRE(builder.publish_on_request(outbound_queue, "req-1").published);
	REQUIRE_FALSE(builder.debouncer().change_pending());

	for (std::size_t tick = 0; tick < settings.quiet_ticks * 3; ++tick)
	{
		REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
	}

	const auto envelopes = outbound_queue.drain_up_to(8);

	REQUIRE(envelopes.size() == 1);
	REQUIRE(envelopes.front().type == std::string{project_context_response_envelope_type});
}

TEST_CASE("a change that lands during the project walk is not swallowed", "[context][builder]")
{
	// Walking a large project is not instantaneous. A change made during the walk is
	// one the snapshot may already be behind on, so the counter adopted afterwards is
	// the one from before the walk — which leaves the change looking unseen and a
	// corrected snapshot following.
	ChangingWhileReadSource source;

	OutboundQueue outbound_queue;

	SnapshotDebounceSettings settings;
	settings.quiet_ticks = 2;

	Builder builder{source, settings};

	REQUIRE(builder.publish_on_connect(outbound_queue).published);

	// The read bumped the counter, so the next tick sees a change rather than nothing.
	REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
	REQUIRE(builder.debouncer().change_pending());

	REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
	REQUIRE(builder.publish_on_timer_tick(outbound_queue).published);
}

TEST_CASE("a project the source could not read is not sent as an empty project", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading.readable = false;

	OutboundQueue outbound_queue;

	SnapshotDebounceSettings settings;
	settings.quiet_ticks = 2;

	Builder builder{source, settings};

	SECTION("nothing goes out on connect")
	{
		const SnapshotPublication publication = builder.publish_on_connect(outbound_queue);

		REQUIRE_FALSE(publication.published);
		REQUIRE_FALSE(publication.project_readable);
		REQUIRE(outbound_queue.empty());
		REQUIRE_FALSE(builder.has_built_a_snapshot());
	}

	SECTION("nothing goes out in answer to a request")
	{
		const SnapshotPublication publication = builder.publish_on_request(outbound_queue, "req-2");

		REQUIRE_FALSE(publication.published);
		REQUIRE_FALSE(publication.project_readable);
		REQUIRE(outbound_queue.empty());
	}

	SECTION("a failed read is not retried every quiet period")
	{
		// The only thing that makes a reading unreadable is a REAPER function the source
		// could not resolve, which does not clear on its own. Retrying would produce the
		// same failure four times a second for as long as REAPER is open.
		builder.publish_on_timer_tick(outbound_queue);

		++source.change_count;
		builder.publish_on_timer_tick(outbound_queue);
		builder.publish_on_timer_tick(outbound_queue);

		// The quiet period completed, the read was attempted, nothing was sent.
		const SnapshotPublication attempt = builder.publish_on_timer_tick(outbound_queue);

		REQUIRE(source.project_reads == 1);
		REQUIRE_FALSE(attempt.published);
		REQUIRE_FALSE(attempt.project_readable);
		REQUIRE(outbound_queue.empty());

		for (int tick = 0; tick < 50; ++tick)
		{
			builder.publish_on_timer_tick(outbound_queue);
		}

		REQUIRE(source.project_reads == 1);
	}

	SECTION("the next actual change brings a snapshot once the source recovers")
	{
		builder.publish_on_timer_tick(outbound_queue);

		source.reading = plain_reading();

		++source.change_count;
		REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
		REQUIRE_FALSE(builder.publish_on_timer_tick(outbound_queue).published);
		REQUIRE(builder.publish_on_timer_tick(outbound_queue).published);

		REQUIRE_FALSE(outbound_queue.empty());
	}
}

TEST_CASE("the builder keeps the last snapshot it built", "[context][builder]")
{
	StubProjectContextSource source;
	source.reading = plain_reading();

	OutboundQueue outbound_queue;
	Builder builder{source};

	REQUIRE_FALSE(builder.has_built_a_snapshot());

	builder.publish_on_connect(outbound_queue);

	REQUIRE(builder.has_built_a_snapshot());
	REQUIRE(builder.last_snapshot().tracks.size() == 1);
	REQUIRE(builder.last_snapshot().tracks.front().guid == test_guid(1));
	REQUIRE(builder.last_snapshot().project_name == "Sesh Test Session");
}

TEST_CASE("the envelope types are the ones the codec validates against the snapshot schema", "[context][builder]")
{
	// Requirement 6.8 holds because the codec validates an outbound payload against the
	// schema its envelope type is bound to. The static assertions in
	// project_context_builder.h check this at compile time; this case is here so a
	// failure names the requirement rather than only failing to build.
	SECTION("both outbound types resolve to project-context.schema.json")
	{
		REQUIRE(
			sesh_ai::transport::outbound_payload_schema_path(project_context_state_envelope_type)
				== sesh_ai::transport::project_context_schema_path
		);
		REQUIRE(
			sesh_ai::transport::outbound_payload_schema_path(project_context_response_envelope_type)
				== sesh_ai::transport::project_context_schema_path
		);
	}

	SECTION("the request type carries no bundled schema, and is not a tool name")
	{
		// Its payload is empty, so there is nothing to validate — and it must not be
		// mistaken for one of the 42 tools, which would put it on a path that looks up a
		// tool output schema.
		REQUIRE_FALSE(
			sesh_ai::transport::inbound_payload_schema_path(project_context_request_envelope_type)
				.has_value()
		);
		REQUIRE_FALSE(
			sesh_ai::transport::tool_name_from_request_envelope_type(
				project_context_request_envelope_type
			).has_value()
		);
	}

	SECTION("every reason maps to one of the two outbound types, and describes itself")
	{
		for (const SnapshotReason reason : {
			SnapshotReason::connected,
			SnapshotReason::project_changed,
			SnapshotReason::requested
		})
		{
			const std::string_view envelope_type = envelope_type_for_snapshot_reason(reason);

			INFO("reason: " << describe_snapshot_reason(reason));
			REQUIRE_FALSE(describe_snapshot_reason(reason).empty());
			REQUIRE(
				(envelope_type == project_context_state_envelope_type
					|| envelope_type == project_context_response_envelope_type)
			);
			REQUIRE(
				sesh_ai::transport::outbound_payload_schema_path(envelope_type)
					== sesh_ai::transport::project_context_schema_path
			);
		}
	}
}
