// The REAPER-facing half of the Transport Handler, and the handler's production
// instantiation.
//
// Two things live here, and both are here because they cannot live in the header.
//
// ReaperTransportActionInvoker is the only place in this component that includes the
// SDK. The test target has no SDK include path, so keeping the one call REAPER is
// needed for in its own translation unit is what lets the suite substitute a
// TransportActionInvoker and never link this — the convention
// entry/reaper_timer_registrar.cpp established.
//
// And the handler is a template, so a template nobody instantiates is a template
// nobody compiles. The explicit instantiation at the bottom is the compile-time
// proof that TransportHandler works with the outbound envelope it will actually
// carry, rather than only with the stub the suite drives it with.

#include <transport/transport_handler.h>

#include <transport/envelope.h>

#include <reaper_plugin.h>

namespace sesh_ai::transport {

	ReaperTransportActionInvoker::ReaperTransportActionInvoker(reaper_plugin_info_t* plugin_info)
		: plugin_info_{plugin_info}
	{
	}

	bool ReaperTransportActionInvoker::invoke_main_action(int reaper_action_command_id)
	{
		// Zero is REAPER's "no command", and a negative ID is not an action at all.
		// Refusing both here is what makes the unreachable tail of
		// reaper_action_command_id() safe: a command added to the enum without a
		// mapping runs nothing rather than something arbitrary.
		if (reaper_action_command_id <= 0) {
			return false;
		}

		if (main_on_command_ == nullptr) {
			if (plugin_info_ == nullptr || plugin_info_->GetFunc == nullptr) {
				return false;
			}

			// GetFunc returns void*, so the function pointer has to be cast back.
			// Casting an object pointer to a function pointer is conditionally
			// supported by the standard; it is well defined on all three platforms
			// the extension targets, and it is how REAPER's own SDK resolves its API.
			main_on_command_ = reinterpret_cast<MainOnCommandFunction>(
				plugin_info_->GetFunc("Main_OnCommand")
			);
		}

		// GetFunc returns null for a function this REAPER build does not have. It
		// will not happen for Main_OnCommand, which has been in the API since before
		// the minimum version the extension supports — but reporting it is the
		// difference between a transport button that explains itself and one that
		// silently does nothing.
		if (main_on_command_ == nullptr) {
			return false;
		}

		// The second argument is the flag REAPER uses to distinguish an action
		// triggered by a relative MIDI or OSC control from an ordinary one. These
		// seven are all plain triggers, so it is zero.
		main_on_command_(reaper_action_command_id, 0);

		return true;
	}

	template class TransportHandler<OutboundEnvelope>;

	// Explicit class instantiation does not reach a member template, and handle() is
	// one — it has to be, so that the suite can drive everything around it without
	// nlohmann/json installed. So it is named separately, which is what compiles the
	// payload read against the real nlohmann::json rather than against the stub the
	// suite substitutes.
	template TransportRequestOutcome TransportHandler<OutboundEnvelope>::handle<InboundEnvelope>(
		const InboundEnvelope&
	);

}
