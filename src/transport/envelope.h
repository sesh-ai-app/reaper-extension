// The parsed envelope structures the queue pair carries.
//
// Every message on the wire is `{ type, payload, requestId? }`. These are what it
// looks like once parsed — the form the main thread works with, never the text.
//
// The codec that produces and consumes them is task 4.2 and will extend these; what
// is fixed here is only what the queues need in order to be typed rather than
// generic. The split into two types is deliberate: an inbound envelope has already
// been parsed and schema-validated on the network thread and is trusted by the time
// the main thread sees it (requirement 22.2), while an outbound envelope is still
// to be validated and serialized. They are not interchangeable, so the compiler
// should not let them be.

#ifndef SESH_AI_TRANSPORT_ENVELOPE_H
#define SESH_AI_TRANSPORT_ENVELOPE_H

#include <string>

#include <nlohmann/json.hpp>

namespace sesh_ai::transport {

	// Parsed and already validated. The Message Dispatcher routes on `type`.
	struct InboundEnvelope {
		std::string type;

		// Echoed onto the response. Empty when the envelope carried none — the
		// extension keeps no pending-request table for tool calls, it just hands the
		// identifier back.
		std::string request_id;

		nlohmann::json payload;
	};

	// Produced on the main thread, validated and serialized on the network thread.
	struct OutboundEnvelope {
		std::string type;
		std::string request_id;
		nlohmann::json payload;

		// The schema the payload is to be validated against, when the producer of this
		// envelope already knows and the codec could not work it out.
		//
		// A fourth field in a struct the file comment above calls deliberately minimal,
		// and that is a real cost — so it is the only one added and it is here because
		// the thread split leaves nowhere else to put it. `EnvelopeCodec::encode_outbound`
		// runs on the network thread and derives the schema from the type and the payload;
		// for a tool result that derivation cannot be right. A framework
		// `tool_partial_outcome` and the named tool's own success are indistinguishable
		// from a payload plus a tool name, so the partial gets routed to the tool's output
		// schema and is refused on `items`, which the framework cannot supply. Only
		// `daw::result_schema_path_for` can answer, it answers on the main thread, and
		// requirement 22.2 puts validation on the other side of the `QueuePair` — so the
		// answer has to travel with the envelope rather than beside it.
		//
		// Empty is the ordinary case and means "derive it": every envelope whose schema
		// follows from its type — a snapshot, a prompt, an acknowledgement, a confirmation
		// decision — leaves this alone and the codec answers as it always has. Non-empty
		// is an override and is not re-derived or cross-checked, because the point is that
		// the codec's derivation is the thing that cannot be trusted here.
		std::string payload_schema_path_override;
	};

}

#endif
