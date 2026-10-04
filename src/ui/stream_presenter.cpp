// The Stream Presenter's production instantiation.
//
// Everything the presenter does is in the header, because none of it needs REAPER and
// all of it needs testing. What cannot be there is this: `handle` is a member template
// over the envelope type, and a template nobody instantiates is a template nobody
// compiles. Naming it here against transport/envelope.h's InboundEnvelope is the
// compile-time proof that the payload reads work against a real nlohmann::json and not
// only against the stub the suite substitutes — the same arrangement, for the same
// reason, as transport/transport_handler.cpp.
//
// This is also the one translation unit that pulls nlohmann/json on the presenter's
// behalf. The test target does not link it, which is what keeps the suite buildable on
// a machine with no vcpkg.

#include <ui/stream_presenter.h>

#include <transport/envelope.h>

namespace sesh_ai::ui {

	template StreamEnvelopeOutcome StreamPresenter::handle<transport::InboundEnvelope>(
		const transport::InboundEnvelope&
	);

}
