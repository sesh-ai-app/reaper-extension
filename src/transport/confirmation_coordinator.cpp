// The Confirmation Coordinator's production instantiation.
//
// Nothing else is here, and the absence is the point. The coordinator has no REAPER
// dependency at all — a confirmation is a protocol and UI concern from the moment the
// request arrives to the moment the prompt comes down, and the operation it
// authorises runs later, in the Tool Executor — so there is no SDK seam to define in
// a translation unit of its own, the way transport_handler.cpp and
// reaper_timer_registrar.cpp both need.
//
// What is left is the reason a template needs a .cpp regardless: a template nobody
// instantiates is a template nobody compiles. The suite drives the coordinator with a
// stub envelope whose payload is a std::map, because nlohmann/json is an optional
// dependency of the test target. These instantiations are the compile-time proof that
// the coordinator works with the envelope it will actually carry — that
// nlohmann::json accepts the two payload writes, and that an already-validated
// nlohmann::json payload satisfies the defensive field reads.

#include <transport/confirmation_coordinator.h>

#include <transport/envelope.h>

namespace sesh_ai::transport {

	template class ConfirmationCoordinator<OutboundEnvelope>;

	// Explicit class instantiation does not reach a member template, and the two
	// dispatcher entry points are member templates — they have to be, so that the
	// suite can exercise everything around them without nlohmann/json installed. So
	// they are named separately, which is what compiles the payload reads against the
	// real nlohmann::json rather than against the stub the suite substitutes.
	template ConfirmationRequestOutcome
	ConfirmationCoordinator<OutboundEnvelope>::handle_confirmation_request<InboundEnvelope>(
		const InboundEnvelope&
	);

	template ConfirmationResolvedOutcome
	ConfirmationCoordinator<OutboundEnvelope>::handle_confirmation_resolved<InboundEnvelope>(
		const InboundEnvelope&
	);

}
