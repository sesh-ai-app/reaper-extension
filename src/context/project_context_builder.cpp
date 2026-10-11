// The Project Context Builder's production instantiation.
//
// Nothing behavioural lives here. project_context_builder.h is templated on the JSON
// document type and the outbound envelope type so the Catch2 suite can drive the
// component with types it controls, and a template nobody instantiates is a template
// nobody compiles — so this translation unit names each piece against nlohmann::json
// and the real envelope, and the build is then the proof that the calls the
// serialiser makes on a document are calls nlohmann::json actually has.
//
// The same arrangement, and the same reason, as envelope_codec.cpp and
// transport_handler.cpp.

#include <context/project_context_builder.h>

#include <transport/envelope.h>
#include <transport/queue_pair.h>

#include <nlohmann/json.hpp>

namespace sesh_ai::context
{
	// Proves the serialiser's whole vocabulary against the real document type:
	// `object()`, `array()`, `operator[]`, `push_back`, and assignment from
	// std::string, double, int, bool, and a nested document.
	template nlohmann::json serialize_project_context<nlohmann::json>(const ProjectContextSnapshot&);

	// Proves the component against the real document type and the real outbound
	// envelope — that `payload` accepts a serialised snapshot, and that the queue the
	// dispatcher drains accepts the envelope.
	template class ProjectContextBuilder<nlohmann::json, sesh_ai::transport::OutboundEnvelope>;
}
