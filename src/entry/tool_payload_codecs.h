// The 42 tools' codecs, assembled (task 24.3, requirements 4.1, 9.1, 9.2).
//
// Tasks 24.1 and 24.2 put the readers and the writers in `src/transport/`, beside the
// schemas they answer to. What was left was calling them — ten `bind_*` functions over
// seven per-family codec structs, plus the Tool Executor's own two-function seam — and
// handing the result to `compose_tool_handler_registry`.
//
// ---------------------------------------------------------------------------
// Why this is a header rather than three functions inside `extension_composition.cpp`
//
// Because of what the omission looks like. Every `register_*_tools` declines an
// incomplete codec rather than registering a handler that would throw
// `std::bad_function_call` from inside an undo block — so a family left out of the
// assembly below is not a compile error and not a crash. It is six tools quietly absent
// from the registry, and the symptom a producer gets is the agent being told a tool it
// can see in its own tool list does not exist.
//
// That is the failure the whole of task 24 exists to end, and the only thing that ends
// it is a test asserting the count. `tests/CMakeLists.txt` does not compile `src/`'s
// translation units, so an assembly written inside `extension_composition.cpp` could
// only be checked by a test that restated it — which would agree with itself and with
// nothing else, and would still pass with a family dropped from the real one.
//
// Here, `tests/entry/tool_payload_codecs_test.cpp` calls exactly the functions the
// extension calls, over the same `compose_tool_handler_registry`, and asserts the number
// that comes back. Drop a `bind_*` call and that test fails.
//
// Templated on the document and envelope types for the same reason everything else in
// this codebase is: not one line below needs a JSON library, so nothing below names one.
// `extension_composition.cpp` instantiates these with `nlohmann::json` and the two real
// envelopes; the suite instantiates them with `nlohmann::json` and a stand-in envelope.

#ifndef SESH_AI_ENTRY_TOOL_PAYLOAD_CODECS_H
#define SESH_AI_ENTRY_TOOL_PAYLOAD_CODECS_H

#include <string_view>

#include <entry/extension_composition.h>
#include <transport/tool_call_reader.h>
#include <transport/tool_outcome_writer.h>
#include <transport/tool_request_readers.h>
#include <transport/tool_result_writers.h>

namespace sesh_ai::entry {

	// The seven per-family codecs, readers and writers both.
	//
	// Thirteen calls over seven structs. Six families take a reader-binding and a
	// writer-binding; undo takes writers alone, because both of its tools' one input
	// field — `confirmedForeignUndo` — arrives on `daw::tool_call` rather than in the
	// payload, so there is no `bind_undo_request_readers` for a call to sit beside.
	//
	// By reference and in pairs rather than each binding returning a codec: the read side
	// and the write side populate different members of one struct, which is what lets
	// task 24.1's and task 24.2's work meet without either having to know the other's
	// shape.
	//
	// fx, item, and track state are absent, and that is not an omission. Those three take
	// a reader and a writer directly — `transport::ToolRequestReader` and
	// `transport::ToolResultWriter<JsonValue>` — because each of their readers is an
	// overload set across the family's request types, and an overload set does not fit in
	// a `std::function`. `compose_tool_handler_registry` takes them as its last two
	// parameters for that reason.
	template <typename JsonValue>
	ToolPayloadCodecs<JsonValue> build_tool_payload_codecs()
	{
		ToolPayloadCodecs<JsonValue> codecs;

		transport::bind_track_structure_request_readers<JsonValue>(codecs.track_structure);
		transport::bind_track_structure_result_writers<JsonValue>(codecs.track_structure);

		transport::bind_marker_region_request_readers<JsonValue>(codecs.marker_region);
		transport::bind_marker_region_result_writers<JsonValue>(codecs.marker_region);

		transport::bind_marker_region_read_request_readers<JsonValue>(codecs.marker_region_read);
		transport::bind_marker_region_read_result_writers<JsonValue>(codecs.marker_region_read);

		transport::bind_tempo_request_readers<JsonValue>(codecs.tempo);
		transport::bind_tempo_result_writers<JsonValue>(codecs.tempo);

		transport::bind_routing_request_readers<JsonValue>(codecs.routing);
		transport::bind_routing_result_writers<JsonValue>(codecs.routing);

		transport::bind_render_request_reader<JsonValue>(codecs.render);
		transport::bind_render_result_writer<JsonValue>(codecs.render);

		transport::bind_undo_result_writers<JsonValue>(codecs.undo);

		return codecs;
	}

	// The Tool Executor's own seam: one inbound envelope in, one outbound payload out.
	//
	// Both members forward to the function task 24.1 and task 24.2 wrote, and that is the
	// whole of it. In particular the schema the payload answers to is *not* derived here
	// — `ExecutorToolCallRouter` reads it off the outcome through
	// `daw::result_schema_path_for` and carries it across the queue on
	// `OutboundEnvelope::payload_schema_path_override`, which is the one answer nothing
	// downstream can reconstruct from a payload and a tool name.
	//
	// `is_complete()` on the returned codec is true, and that is the property that makes
	// the difference between this build and every build before task 24.3: an incomplete
	// codec makes `ExecutorToolCallRouter::execute_tool_call` report the tool as unknown
	// without dispatching, for all 42 of them.
	template <typename InboundEnvelopeType, typename JsonValue>
	ToolCallPayloadCodec<InboundEnvelopeType, JsonValue> build_tool_call_codec()
	{
		ToolCallPayloadCodec<InboundEnvelopeType, JsonValue> codec;

		codec.read_tool_call = [](
			std::string_view tool_name,
			const InboundEnvelopeType& inbound_envelope) {
			return transport::read_tool_call<InboundEnvelopeType, JsonValue>(
				tool_name,
				inbound_envelope
			);
		};

		codec.write_dispatch_outcome = [](const daw::dispatch_outcome<JsonValue>& outcome) {
			return transport::write_dispatch_outcome<JsonValue>(outcome);
		};

		return codec;
	}

}

#endif
