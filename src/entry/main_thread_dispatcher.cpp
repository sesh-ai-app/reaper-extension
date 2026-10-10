// The production instantiation of the Main-Thread Dispatcher.
//
// The dispatcher's logic is a template in the header, which is what lets the suite
// exercise the draining and bounding with a stub envelope. The cost of that choice
// is that a template nobody instantiates is a template nobody compiles, so an
// error only the real envelope types would provoke would sit undetected until the
// plugin entry wired them up.
//
// This file is the instantiation, and therefore the compile-time proof that the
// dispatcher works with the envelopes it will actually carry. It is the whole
// content of the translation unit on purpose.

#include <entry/main_thread_dispatcher.h>

#include <transport/envelope.h>

namespace sesh_ai::entry {

	template class MainThreadDispatcher<transport::InboundEnvelope, transport::OutboundEnvelope>;

}
