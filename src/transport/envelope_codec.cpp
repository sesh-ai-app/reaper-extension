// The Envelope Codec's production instantiation.
//
// Nothing behavioural lives here. envelope_codec.h is templated on the JSON document
// type so the Catch2 suite can drive it with a type it controls, and a template nobody
// instantiates is a template nobody compiles — so this translation unit names each
// piece against nlohmann::json and the real envelope structures, and the build is then
// the proof that the calls the codec makes on a document are calls nlohmann::json
// actually has.
//
// Explicit class instantiation does not reach a member template, and most of the codec
// is member templates: the inbound and outbound entry points are parameterised on the
// envelope types so the suite can substitute stubs for the nlohmann::json-carrying
// ones in transport/envelope.h. Each is therefore named separately below. The same
// arrangement, and the same reason, as transport_handler.cpp.

#include <transport/envelope_codec.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <transport/envelope.h>
#include <transport/queue_pair.h>
#include <transport/schema_validator.h>

#include <nlohmann/json.hpp>

namespace sesh_ai::transport {

	template class EnvelopeCodec<nlohmann::json>;

	// The adapter that puts the real validator behind the seam the codec depends on.
	// Instantiated here rather than in schema_validator.cpp because this is where the
	// production wiring is, and because instantiating it is what compiles
	// BundledSchemaValidator::validate_against<nlohmann::json> into a virtual call the
	// codec can reach.
	template class BundledPayloadSchemaValidator<nlohmann::json>;

	template InboundDecodeOutcome<InboundEnvelope>
		EnvelopeCodec<nlohmann::json>::decode_inbound<InboundEnvelope>(std::string_view);

	template InboundDecodeOutcome<InboundEnvelope>
		EnvelopeCodec<nlohmann::json>::accept_inbound_message<InboundEnvelope, OutboundEnvelope>(
			std::string_view,
			ConcurrentQueue<InboundEnvelope>&,
			ConcurrentQueue<OutboundEnvelope>&
		);

	template OutboundEncodeOutcome
		EnvelopeCodec<nlohmann::json>::encode_outbound<OutboundEnvelope>(const OutboundEnvelope&);

	// Reached through encode_outbound, so naming it here adds no instantiation that the
	// line above did not already force. It is named anyway because this file's job is to
	// list what the production types are expected to satisfy, and this is the member that
	// reads `OutboundEnvelope::payload_schema_path_override` — the one field the real
	// envelope has that a substituted stub could have been written without.
	template std::optional<std::string_view>
		EnvelopeCodec<nlohmann::json>::schema_path_for_outbound_envelope<OutboundEnvelope>(
			const OutboundEnvelope&
		);

	template std::optional<std::string_view>
		EnvelopeCodec<nlohmann::json>::outbound_schema_path_for<nlohmann::json>(
			std::string_view,
			const nlohmann::json&
		);

	template bool
		EnvelopeCodec<nlohmann::json>::payload_is_tool_refusal<nlohmann::json>(const nlohmann::json&);

	template OutboundEnvelope build_validation_error_envelope<OutboundEnvelope>(
		InboundRefusalReason,
		std::string_view,
		std::string_view,
		const std::vector<std::string>&
	);

}
