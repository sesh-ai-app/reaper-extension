// The Transport Handler.
//
// Seven transport commands arrive from the producer's phone — play, pause, stop,
// return to zero, previous marker, next marker, loop toggle — each is executed as
// one REAPER action, and each is acknowledged (requirement 14.1). This is the only
// inbound path in the extension that does not originate with the agent: nobody
// reasoned about it, nothing needs confirming, and the producer is standing at the
// other end of it expecting the transport to move.
//
// Three things about the shape of this file.
//
// **The mapping is data, not behaviour.** Each command names one native REAPER
// action ID, and the whole of the execution is handing that integer to
// Main_OnCommand. Keeping the mapping as a total constexpr function over the enum
// means the suite can assert all seven are distinct and none is zero without
// REAPER present, which is the check worth having — a wrong ID is not a crash or a
// failed call, it is a transport button that does something unexpected in a
// producer's session.
//
// **REAPER is reached through one call, behind an interface.** TransportActionInvoker
// is the narrowest seam this component could have: invoke one action by ID. The
// REAPER-backed implementation is declared here without the SDK and defined in
// transport_handler.cpp, which is the same arrangement as
// entry/reaper_timer_registrar.h — and for the same structural reason, that the test
// target has no SDK include path, so a header reaching the SDK cannot be tested.
//
// **The handler is templated on the outbound envelope type.** transport/envelope.h
// pulls nlohmann/json, and the test target links it only when it is installed, so
// the logic worth testing — parse, map, invoke, acknowledge — is written against the
// two operations it actually performs on an envelope: assigning `type` and
// `request_id`, and writing one string through `payload["requestId"]`. Both
// nlohmann::json and a std::map<std::string, std::string> support that line, and the
// production instantiation in transport_handler.cpp is the compile-time proof that
// the real envelope does.

#ifndef SESH_AI_TRANSPORT_TRANSPORT_HANDLER_H
#define SESH_AI_TRANSPORT_TRANSPORT_HANDLER_H

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <transport/queue_pair.h>

// Defined by the SDK as `typedef struct reaper_plugin_info_t { ... }
// reaper_plugin_info_t;`, so naming the struct here is a forward declaration of the
// same type rather than a second one. It is only ever held as a pointer, which is
// what keeps this header free of reaper_plugin.h.
struct reaper_plugin_info_t;

namespace sesh_ai::transport {

	// The envelope types this component consumes and produces. Spelled once so the
	// Message Dispatcher (task 14.1) can route on the same constant the handler
	// answers with, rather than on a second copy of the string.
	inline constexpr std::string_view transport_request_envelope_type{"request:transport"};
	inline constexpr std::string_view transport_response_envelope_type{"response:transport"};

	// The seven commands, spelled as transport-command.schema.json spells them. The
	// schema's `command` enum is closed and `additionalProperties` is false, so this
	// list is the whole inbound vocabulary and will not grow without the schema
	// growing first.
	enum class TransportCommand {
		play,
		pause,
		stop,
		return_to_zero,
		previous_marker,
		next_marker,
		loop_toggle
	};

	// Every command, so the suite can iterate the enum exhaustively rather than
	// repeating it, and so a value added here without a mapping is a test failure
	// instead of a silent gap.
	inline constexpr std::array<TransportCommand, 7> all_transport_commands{
		TransportCommand::play,
		TransportCommand::pause,
		TransportCommand::stop,
		TransportCommand::return_to_zero,
		TransportCommand::previous_marker,
		TransportCommand::next_marker,
		TransportCommand::loop_toggle
	};

	// REAPER's own command IDs for the seven actions, taken from the Main section of
	// REAPER's action list as REAPER itself dumps it.
	//
	// Native action IDs are the only ones Cockos guarantees are stable across
	// versions and installations — SWS, ReaPack, and custom action IDs are assigned
	// per launch and are looked up by name instead. All seven here are native, so
	// they are safe as literals, and none of them needs NamedCommandLookup.
	//
	// The names are REAPER's names, not ours, because that is what a producer sees in
	// the action list and in the undo history when one of these fires. Note the two
	// that read oddly next to the schema's vocabulary: "return to zero" is REAPER's
	// "go to start of project", and "loop toggle" is REAPER's "toggle repeat" —
	// repeat is what REAPER calls looping the time selection.
	inline constexpr int reaper_action_transport_play = 1007;
	inline constexpr int reaper_action_transport_pause = 1008;
	inline constexpr int reaper_action_transport_stop = 1016;
	inline constexpr int reaper_action_transport_toggle_repeat = 1068;
	inline constexpr int reaper_action_transport_go_to_start_of_project = 40042;
	inline constexpr int reaper_action_markers_go_to_previous_marker = 40172;
	inline constexpr int reaper_action_markers_go_to_next_marker = 40173;

	// The schema spelling of a command. Total over the enum, so there is no fallback
	// string that could be compared against a payload and match something nobody
	// wrote.
	constexpr std::string_view to_schema_string(TransportCommand command)
	{
		switch (command) {
			case TransportCommand::play:
				return "play";
			case TransportCommand::pause:
				return "pause";
			case TransportCommand::stop:
				return "stop";
			case TransportCommand::return_to_zero:
				return "return_to_zero";
			case TransportCommand::previous_marker:
				return "previous_marker";
			case TransportCommand::next_marker:
				return "next_marker";
			case TransportCommand::loop_toggle:
				return "loop_toggle";
		}

		// Unreachable for any enumerator. Present because a switch over an enum class
		// with no default is not a guarantee to the compiler that the value is one of
		// them.
		return "";
	}

	// The inverse. Empty for anything that is not one of the seven schema strings —
	// there is no nearest match and no default command, because every candidate
	// default is an action that moves a producer's transport without being asked.
	constexpr std::optional<TransportCommand> transport_command_from_schema_string(std::string_view command_name)
	{
		for (const TransportCommand candidate : all_transport_commands) {
			if (to_schema_string(candidate) == command_name) {
				return candidate;
			}
		}

		return std::nullopt;
	}

	// The native REAPER action ID for a command. Total over the enum, and the
	// unreachable tail returns an ID the invoker refuses rather than an action, so a
	// command added without a mapping cannot fire an arbitrary one.
	constexpr int reaper_action_command_id(TransportCommand command)
	{
		switch (command) {
			case TransportCommand::play:
				return reaper_action_transport_play;
			case TransportCommand::pause:
				return reaper_action_transport_pause;
			case TransportCommand::stop:
				return reaper_action_transport_stop;
			case TransportCommand::return_to_zero:
				return reaper_action_transport_go_to_start_of_project;
			case TransportCommand::previous_marker:
				return reaper_action_markers_go_to_previous_marker;
			case TransportCommand::next_marker:
				return reaper_action_markers_go_to_next_marker;
			case TransportCommand::loop_toggle:
				return reaper_action_transport_toggle_repeat;
		}

		return 0;
	}

	// What the extension needs from REAPER in order to execute a transport command:
	// run one action in the main section, by ID.
	//
	// One call, because that is the component's entire dependency on REAPER. A
	// per-component seam rather than a shared ReaperApi interface is the convention
	// entry/timer_registration.h established, and this is the second instance of it.
	class TransportActionInvoker {
	public:
		virtual ~TransportActionInvoker() = default;

		// False when the action could not be run at all — no registration table, no
		// Main_OnCommand, or an ID this invoker refuses. Not a report of what the
		// action did: REAPER's transport actions do not report that, which is the
		// reason response-transport.schema.json carries nothing but the correlation
		// identifier.
		virtual bool invoke_main_action(int reaper_action_command_id) = 0;
	};

	// What handling one request:transport did. Returned rather than logged, for the
	// same reason TickReport is: the caller knows how this build reports things.
	//
	// The three booleans are deliberately separate rather than one status enum,
	// because they fail independently and a producer-visible symptom differs for
	// each. An unrecognised command means nothing happened and nothing should have.
	// A recognised command that was not invoked means REAPER was unreachable. An
	// invoked command that was not acknowledged means the transport moved but the
	// phone will not hear back about it.
	struct TransportRequestOutcome {
		// The `command` field as it arrived, including when it was not one of the
		// seven — which is the value worth having in the log line.
		std::string command_name;

		// Empty when command_name was not one of the seven.
		std::optional<TransportCommand> command;

		// True when command_name parsed to one of the seven.
		bool command_recognised = false;

		// The action ID that was handed to REAPER. Zero when nothing was.
		int reaper_action_command_id = 0;

		// True when the invoker reported that it ran the action.
		bool action_invoked = false;

		// True when a response:transport envelope was queued.
		bool acknowledgement_queued = false;
	};

	// Executes transport commands and acknowledges them.
	//
	// Templated on the outbound envelope type — see the file comment for why. The
	// production instantiation lives in transport_handler.cpp.
	template <typename OutboundEnvelopeType>
	class TransportHandler {
	public:
		using OutboundQueue = ConcurrentQueue<OutboundEnvelopeType>;

		TransportHandler(TransportActionInvoker& action_invoker, OutboundQueue& outbound_queue)
			: action_invoker_{action_invoker},
			outbound_queue_{outbound_queue}
		{
		}

		TransportHandler(const TransportHandler&) = delete;
		TransportHandler& operator=(const TransportHandler&) = delete;
		TransportHandler(TransportHandler&&) = delete;
		TransportHandler& operator=(TransportHandler&&) = delete;

		// The entry point for the Message Dispatcher (task 14.1): hand it the
		// request:transport envelope it routed here and it does the rest.
		//
		// A member template, so the JSON-touching half is instantiated only where
		// the real envelope is used and the suite can exercise the rest without
		// nlohmann/json installed. The envelope is taken by const reference because
		// nothing here keeps any part of it — the acknowledgement copies the one
		// string it needs.
		//
		// Runs inside the main-thread tick and nowhere else, since it calls the
		// REAPER C API (requirement 22.3).
		template <typename InboundEnvelopeType>
		TransportRequestOutcome handle(const InboundEnvelopeType& inbound_envelope)
		{
			const std::string command_name = read_transport_command_name(inbound_envelope.payload);

			return handle_transport_command(command_name, inbound_envelope.request_id);
		}

		// The same work with the envelope already taken apart, which is the form the
		// suite drives and the form the sequence is actually about.
		//
		// The order is the contract. Nothing is invoked until the command is one of
		// the seven, and nothing is acknowledged until REAPER reported that the
		// action ran — an acknowledgement is the only thing the phone gets, so it
		// must not arrive for a command that did nothing.
		TransportRequestOutcome handle_transport_command(std::string_view command_name, std::string_view request_id)
		{
			TransportRequestOutcome outcome;
			outcome.command_name.assign(command_name);

			const std::optional<TransportCommand> command =
				transport_command_from_schema_string(command_name);

			// Not one of the seven. The Envelope Codec validates request:transport
			// against the vendored transport-command.schema.json and refuses an
			// invalid payload before it reaches a handler (requirement 4.3), so
			// arriving here means either the codec was bypassed or the server's
			// protocol is ahead of this build. Both are handled the way
			// requirement 5.8 handles an unknown envelope type: report it, execute
			// nothing. Guessing a nearest action would move a producer's transport
			// on the strength of a typo.
			if (!command.has_value()) {
				return outcome;
			}

			outcome.command_recognised = true;
			outcome.command = command;
			outcome.reaper_action_command_id = reaper_action_command_id(*command);

			if (!action_invoker_.invoke_main_action(outcome.reaper_action_command_id)) {
				return outcome;
			}

			outcome.action_invoked = true;

			// No correlation identifier, nothing to acknowledge. The phone generates
			// the requestId on its request:transport and the acknowledgement is
			// nothing but that identifier echoed, so an empty one produces a payload
			// response-transport.schema.json rejects (`minLength` 1) — which
			// requirement 4.1 has failing here rather than at the server.
			//
			// The action still ran. A producer pressing play wants the transport to
			// move whether or not the reply is addressable, and a missing identifier
			// is the server's problem rather than theirs.
			if (request_id.empty()) {
				return outcome;
			}

			queue_acknowledgement(request_id);
			outcome.acknowledgement_queued = true;

			return outcome;
		}

	private:
		// Builds the response:transport envelope and hands it to the network thread
		// through the outbound queue.
		//
		// The payload carries requestId and nothing else, which is the whole of
		// response-transport.schema.json. Notably no play state: REAPER's transport
		// actions do not fail in a way this component can observe, and the resulting
		// state reaches both clients through the project context snapshot rather than
		// through this reply. A playState here would be read at the moment the action
		// was queued rather than after it took effect, so it would be a field that is
		// sometimes a lie.
		//
		// The identifier goes on the envelope as well as into the payload. The
		// envelope's copy is what the Message Router forwards; the payload's copy is
		// what lets the payload identify its own subject.
		void queue_acknowledgement(std::string_view request_id)
		{
			OutboundEnvelopeType acknowledgement;

			acknowledgement.type = std::string{transport_response_envelope_type};
			acknowledgement.request_id = std::string{request_id};
			acknowledgement.payload["requestId"] = std::string{request_id};

			outbound_queue_.push(std::move(acknowledgement));
		}

		// Reads the `command` field out of an already-validated request:transport
		// payload, defensively rather than by assuming the codec ran.
		//
		// An empty string for anything missing or not a string, which
		// handle_transport_command then treats as unrecognised — one path for "no
		// usable command" rather than an exception from the JSON library crossing
		// into the tick.
		template <typename PayloadType>
		static std::string read_transport_command_name(const PayloadType& payload)
		{
			if (!payload.is_object() || !payload.contains("command")) {
				return std::string{};
			}

			const auto& command_field = payload.at("command");

			if (!command_field.is_string()) {
				return std::string{};
			}

			return command_field.template get<std::string>();
		}

		TransportActionInvoker& action_invoker_;
		OutboundQueue& outbound_queue_;
	};

	// The REAPER side of the seam. Defined in transport_handler.cpp, which is the one
	// translation unit here that includes the SDK.
	//
	// Main_OnCommand is resolved through the registration table REAPER hands the
	// extension at load rather than through reaper_plugin_functions.h's import
	// table, which is the same choice ReaperTimerRegistrar makes and for the same
	// reason: it does not depend on REAPERAPI_LoadAPI having run, and it keeps the
	// storage for the API function pointers a concern of the plugin entry alone.
	class ReaperTransportActionInvoker final : public TransportActionInvoker {
	public:
		// The pointer REAPER passed to ReaperPluginEntry. It stays valid for the
		// lifetime of the loaded extension, which outlives this object.
		explicit ReaperTransportActionInvoker(reaper_plugin_info_t* plugin_info);

		bool invoke_main_action(int reaper_action_command_id) override;

	private:
		// REAPER's own signature for the function: an action ID and a flag.
		using MainOnCommandFunction = void (*)(int command, int flag);

		reaper_plugin_info_t* plugin_info_;

		// Resolved on first use and kept. GetFunc is a string lookup, and this runs
		// on REAPER's main thread every time the producer touches the transport on
		// their phone.
		MainOnCommandFunction main_on_command_ = nullptr;
	};

}

#endif
