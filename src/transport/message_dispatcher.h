// The Message Dispatcher: one inbound envelope in, one destination out
// (requirement 5, design "Message Dispatcher").
//
// Everything the extension does on behalf of the agent starts here. An envelope
// arrives already parsed and already schema-validated — the codec did that on the
// network thread and refused anything it could not vouch for (requirement 4.3) — and
// this decides which of seven components owns it. Nothing else happens here. No
// payload is read, no REAPER call is made, no state is kept.
//
// ---------------------------------------------------------------------------
// Three unknowns, three different answers
//
// The protocol has three ways of not recognising something, they are three different
// requirements, and they must not be collapsed into one:
//
//   | What is unknown        | Requirement | Answer                                 |
//   | the payload's shape    | 4.4, 23.4   | refused by the codec, error response   |
//   | the envelope type      | 5.8, 23.5   | logged and ignored, no response        |
//   | the tool name          | 23.6        | an error response naming the tool      |
//
// The middle row is this component's, and it is the one that reads oddly: a type this
// build has no route for is *not* an error. The server's protocol may be ahead of this
// build, and a producer who updates the server first should not get an error for every
// envelope the new version emits — they should get the features this build does
// support and nothing said about the rest. So `unknown` is a route like the others; it
// just has nowhere to go.
//
// The bottom row is why this file does not use
// `transport::tool_name_from_request_envelope_type` to decide whether an envelope is a
// tool call. That function answers empty for a name that is not one of the 42, which is
// correct for its own purpose and wrong here: routing on it would send
// `request:some_new_tool` down the unknown-envelope-type path, and requirement 23.6
// wants an error that names the tool so a missing implementation is visible rather than
// silent. So the route is decided by the `request:` prefix alone and the Tool Executor
// decides whether the tool exists — it is the thing that holds the registry.
//
// ---------------------------------------------------------------------------
// Exact routes are matched before prefix routes
//
// `request:` is a prefix route whose tail is the tool name, and two `request:` types
// are not tool calls at all. Match the prefix first and `request:project_context`
// dispatches as a tool named `project_context`, which the executor then reports as an
// unknown tool — so the producer's "what am I working with?" turn fails with a message
// about a missing implementation, and the actual Context Builder is never reached.
// `route_for_envelope_type` therefore answers the exact bindings first and the three
// prefixes after, and it is a free function over a string so that the ordering is
// checkable on its own without a single seam wired up.
//
// ---------------------------------------------------------------------------
// The requestId echo is a property of the type, not a rule to remember
//
// Requirement 5.9 is that every response carries the identifier the request arrived
// with, and that there is no pending-request table for tool calls — there is no need
// for one, because a tool call is answered inside the same tick that received it, so
// the identifier never has to be stored anywhere. `ToolResultResponse` is that stated
// as a type: it has no default constructor, no setter for the identifier, and its only
// constructor takes the inbound envelope being answered. A response that forgot to echo
// is not something this file can express.
//
// Confirmations are the one genuinely deferred case — the producer answers them, which
// takes as long as it takes — and `confirmation_coordinator.h` already tracks those. No
// second table is kept here.
//
// ---------------------------------------------------------------------------
// The executor's schema answer is carried, never re-derived
//
// A tool result is the one response the dispatcher builds itself, and the schema it
// validates against cannot be worked out from the payload. `daw::result_schema_path_for`
// is the only thing that can answer: a framework `tool_partial_outcome` validates
// against `messages/tool-result-partial.schema.json` while a `tool_success` validates
// against the named tool's own output schema, and from a payload plus a tool name those
// two are indistinguishable — `set_item_properties`' own output schema also carries
// `actions`. Routing a framework partial to that tool's own schema refuses it on
// `items`, which the framework cannot supply, and a producer sees the tool silently not
// working.
//
// So `ToolCallResponse::result_schema_path` crosses the seam as data and
// `ToolResultResponse` carries it to the sink. `tool_executor.h` states the same rule
// in its own words: anything encoding a result should carry this answer rather than
// re-deriving one from the JSON.
//
// Downstream of that there used to be a gap, and the shape of the sink below is easier
// to read knowing what closed it. `EnvelopeCodec::encode_outbound` takes an envelope and
// nothing else and asks `outbound_schema_path_for(type, payload)` for the schema itself;
// `OutboundEnvelope` had no field to carry an answer in, so an implementation of
// `ToolResultSink` that copied this response into an envelope and pushed it onto the
// outbound queue threw the carried answer away, and the codec re-derived the wrong one
// for every framework partial.
//
// `OutboundEnvelope::payload_schema_path_override` is now that field, and the codec
// treats it as an override when non-empty and derives as before when empty — task 22.2.
// An override parameter on `encode_outbound` was the other candidate and only relocates
// the gap: requirement 22.2 puts the production of an outbound envelope on the main
// thread and its validation on the network thread, so the answer has to travel *with*
// the envelope across the `QueuePair` rather than beside it. An adapter's job is now one
// assignment — copy `result_schema_path()` onto the override — and the answer survives
// the handoff.
//
// The sink still takes a `ToolResultResponse` rather than an envelope, and the reason is
// no longer that gap. It is that `ToolResultResponse` is requirement 5.9 expressed as a
// type: it cannot be built without the call it answers, so the echo is not a step an
// adapter can skip. An `OutboundEnvelope` is a plain struct with an assignable
// `request_id`, so moving the seam to it would trade a guarantee for nothing — the
// adapter performs the same copy either way, just on the other side of the boundary —
// and would add an outbound envelope type to a class template that currently needs only
// the inbound one and the payload.
//
// ---------------------------------------------------------------------------
// Seven destinations, seven narrow seams
//
// None of the seven components is named here. Each is reached through the smallest
// interface this component needs from it — `transport_client.h`'s `SocketConnection` and
// `ui_host.h`'s `BrowserHost` are the convention — and the plugin entry wires the real
// ones. That is what lets the whole of the routing be driven against recorders with no
// REAPER, no CEF, and no JSON library, which is the same arrangement every other
// component here uses.
//
// What *is* taken from those components is their envelope type constants. Three of them
// say in their own comments that they spell the strings once so this file routes on the
// same constant the handler answers to, and a dispatcher routing on a second copy that
// drifted is a handler that is simply never reached. Protocol spellings are data; the
// behaviour stays behind the seams.
//
// ---------------------------------------------------------------------------
// Logging is the returned outcome
//
// Requirement 5.8 asks for an unknown type to be logged. This codebase has no logger —
// every component returns what it did and the caller, which knows how this build
// reports things, decides — so `RoutingOutcome` names the type it could not place and
// the plugin entry writes the line. `TickReport` and `TransportRequestOutcome` are the
// same arrangement.

#ifndef SESH_AI_TRANSPORT_MESSAGE_DISPATCHER_H
#define SESH_AI_TRANSPORT_MESSAGE_DISPATCHER_H

#include <string>
#include <string_view>
#include <utility>

#include <context/project_context_builder.h>
#include <transport/confirmation_coordinator.h>
#include <transport/envelope_codec.h>
#include <transport/schema_validator.h>
#include <transport/transport_handler.h>
#include <ui/stream_presenter.h>
#include <ui/ui_host.h>

namespace sesh_ai::transport {

	// ---------------------------------------------------------------------------
	// Route classification
	// ---------------------------------------------------------------------------

	// The two prefix routes that are not tool calls. `stream:` covers the five types
	// the Stream Presenter assembles a response from; `error:` covers the seven the
	// server emits, which requirement 5.5 sends to the same place.
	inline constexpr std::string_view stream_envelope_type_prefix{"stream:"};

	// The Stream Presenter owns this spelling now, for the same reason it owns the five
	// `stream:*` ones: it is the component that answers to them, and a dispatcher
	// routing on a second copy that drifted is an error envelope silently dropped. Kept
	// under this name so every reader here still reads as before.
	inline constexpr std::string_view error_envelope_type_prefix{
		sesh_ai::ui::error_envelope_type_namespace
	};

	// The ReaScript download (requirement 5.7). The UI Host owns the spelling now —
	// it is one of the bridge message names `ui/ui_host.h` declares, because this is
	// the one route whose payload crosses to JavaScript untouched, so the dispatcher
	// and the bridge have to agree on the string twice over. Kept under this name so
	// every reader here still reads as before, and
	// `inbound_envelope_schema_bindings` remains the second copy it is checked
	// against.
	inline constexpr std::string_view script_download_envelope_type{
		sesh_ai::ui::script_download_bridge_message_name
	};

	// Where an envelope goes. One alternative per destination, plus the two
	// confirmation entry points kept apart — they are different operations on the
	// Confirmation Coordinator, and collapsing them would make the outcome unable to
	// say which one ran.
	enum class EnvelopeRoute {
		// `request:<tool_name>` → Tool Executor (requirement 5.1).
		tool_executor,

		// `confirm:request` → Confirmation Coordinator (requirement 5.2).
		confirmation_request,

		// `confirm:resolved` → Confirmation Coordinator (requirement 5.6).
		confirmation_resolution,

		// `request:project_context` → Context Builder (requirement 5.3).
		context_builder,

		// `request:transport` → Transport Handler (requirement 5.4).
		transport_handler,

		// `stream:*` and `error:*` → Stream Presenter (requirement 5.5).
		stream_presenter,

		// `script:download` → UI Host (requirement 5.7).
		script_download,

		// No route in this build. Logged and ignored, not an error — requirement 5.8
		// and 23.5. Also where every outbound-only type lands if one is ever echoed
		// back: `state:*`, `response:*`, `confirm:approve`, `confirm:reject`.
		unknown
	};

	// True when `envelope_type` begins with `prefix` and has at least one character
	// after it. A bare prefix is not a route: `request:` names no tool, and
	// `envelope.schema.json`'s type pattern requires a character after the colon
	// anyway, so this cannot arrive through the codec.
	constexpr bool envelope_type_has_prefix(std::string_view envelope_type, std::string_view prefix)
	{
		return envelope_type.size() > prefix.size()
			&& envelope_type.substr(0, prefix.size()) == prefix;
	}

	// Which destination owns an envelope type.
	//
	// Exact bindings first, prefixes after — see the file comment for what mis-ordering
	// costs. Total over every string, because `unknown` is a real answer rather than a
	// failure to have one.
	constexpr EnvelopeRoute route_for_envelope_type(std::string_view envelope_type)
	{
		// The two `request:*` types that are not tool calls. Before the `request:`
		// prefix test, which is the whole point of the ordering.
		if (envelope_type == sesh_ai::context::project_context_request_envelope_type) {
			return EnvelopeRoute::context_builder;
		}

		if (envelope_type == transport_request_envelope_type) {
			return EnvelopeRoute::transport_handler;
		}

		if (envelope_type == confirmation_request_envelope_type) {
			return EnvelopeRoute::confirmation_request;
		}

		if (envelope_type == confirmation_resolved_envelope_type) {
			return EnvelopeRoute::confirmation_resolution;
		}

		if (envelope_type == script_download_envelope_type) {
			return EnvelopeRoute::script_download;
		}

		// The three prefix routes. `stream:` and `error:` both belong to the Stream
		// Presenter; which of its five types it recognises is its own business, and a
		// `stream:` type it does not know is a known route with an unrecognised member
		// rather than requirement 5.8's unknown envelope type.
		if (envelope_type_has_prefix(envelope_type, stream_envelope_type_prefix)
			|| envelope_type_has_prefix(envelope_type, error_envelope_type_prefix)) {
			return EnvelopeRoute::stream_presenter;
		}

		if (envelope_type_has_prefix(envelope_type, tool_request_envelope_type_prefix)) {
			return EnvelopeRoute::tool_executor;
		}

		return EnvelopeRoute::unknown;
	}

	// The tool name a `request:<tool_name>` envelope dispatches on, or empty for every
	// other type.
	//
	// The tail as it arrived, including when it is not one of the 42 — that is
	// requirement 23.6's case and the name is the whole of what the error has to carry.
	// Deliberately not `tool_name_from_request_envelope_type`, which filters to the 42;
	// see the file comment.
	constexpr std::string_view tool_name_from_routed_envelope_type(std::string_view envelope_type)
	{
		if (route_for_envelope_type(envelope_type) != EnvelopeRoute::tool_executor) {
			return std::string_view{};
		}

		return envelope_type.substr(tool_request_envelope_type_prefix.size());
	}

	// For a log line, and for a test that wants the route named rather than numbered.
	constexpr std::string_view describe(EnvelopeRoute route)
	{
		switch (route) {
			case EnvelopeRoute::tool_executor:
				return "Tool Executor";
			case EnvelopeRoute::confirmation_request:
				return "Confirmation Coordinator (request)";
			case EnvelopeRoute::confirmation_resolution:
				return "Confirmation Coordinator (resolution)";
			case EnvelopeRoute::context_builder:
				return "Context Builder";
			case EnvelopeRoute::transport_handler:
				return "Transport Handler";
			case EnvelopeRoute::stream_presenter:
				return "Stream Presenter";
			case EnvelopeRoute::script_download:
				return "UI Host (script download)";
			case EnvelopeRoute::unknown:
				return "no route in this build";
		}

		return "";
	}

	// ---------------------------------------------------------------------------
	// The tool result response
	// ---------------------------------------------------------------------------

	// What the Tool Executor answered with: the payload, and the schema that describes
	// it.
	//
	// `result_schema_path` is `daw::result_schema_path_for`'s answer, crossing the seam
	// as data because it is the only place that can produce it. Empty is a legitimate
	// answer and means there is nothing to validate against — an unknown tool, or a
	// success naming something outside the 42 — which requirement 23.6 makes correct
	// rather than a failure.
	template <typename PayloadType>
	struct ToolCallResponse {
		PayloadType payload{};

		std::string result_schema_path;

		// False for requirement 23.6. The response still goes out, and the executor has
		// already named the tool inside the payload — `unknown_tool_error` carries the
		// name, the `unknown_tool` code, and the sentence a producer reads.
		bool tool_is_known = true;
	};

	// A tool result on its way out, addressed to the call it answers.
	//
	// Requirement 5.9 as a type. There is no default constructor, no way to assign the
	// identifier afterwards, and the only constructor takes the envelope being answered
	// — so the echo is not a step that can be skipped. See the file comment.
	template <typename PayloadType>
	class ToolResultResponse {
	public:
		ToolResultResponse() = delete;

		// A constructor template so the inbound envelope type is not named here, for the
		// same dependency reason every other entry point in this codebase is one.
		// `answered_call` is taken by const reference because nothing is kept but the
		// one string copied out of it.
		template <typename InboundEnvelopeType>
		ToolResultResponse(
			const InboundEnvelopeType& answered_call,
			std::string_view tool_name,
			PayloadType payload,
			std::string_view result_schema_path
		)
			: type_{build_envelope_type(tool_name)},
			request_id_{answered_call.request_id},
			payload_{std::move(payload)},
			result_schema_path_{result_schema_path}
		{
		}

		// `response:<tool_name>`, naming the tool whether or not this build implements
		// it. The name arrived inside an envelope type that already satisfied
		// `envelope.schema.json`'s `^(state|request|...):[a-z][a-z0-9_]*$`, so the
		// response type satisfies it too and an unknown tool's error is sendable.
		const std::string& type() const { return type_; }

		// The identifier the call arrived with (requirement 5.9). Empty only when the
		// call carried none, which `encode_outbound` omits rather than sends empty.
		const std::string& request_id() const { return request_id_; }

		const PayloadType& payload() const { return payload_; }

		// The Tool Executor's answer, carried rather than re-derived. Empty when it had
		// none.
		const std::string& result_schema_path() const { return result_schema_path_; }

	private:
		static std::string build_envelope_type(std::string_view tool_name)
		{
			std::string envelope_type{tool_response_envelope_type_prefix};
			envelope_type.append(tool_name);

			return envelope_type;
		}

		std::string type_;
		std::string request_id_;
		PayloadType payload_;
		std::string result_schema_path_;
	};

	// ---------------------------------------------------------------------------
	// The seven seams
	// ---------------------------------------------------------------------------

	// What the dispatcher needs from the Tool Executor: run the tool the envelope type
	// names and answer with the payload plus the schema that describes it.
	//
	// `tool_name` is passed separately rather than left for the implementation to split
	// back out of the type. Splitting it is this component's job, and doing it in two
	// places is how the two answers come to disagree.
	template <typename InboundEnvelopeType, typename PayloadType>
	class ToolCallRouter {
	public:
		virtual ~ToolCallRouter() = default;

		virtual ToolCallResponse<PayloadType> execute_tool_call(
			std::string_view tool_name,
			const InboundEnvelopeType& inbound_envelope
		) = 0;
	};

	// Where a tool result goes.
	//
	// Takes the response rather than an outbound envelope, because the response is the
	// one of the two that cannot be built without the call it answers (requirement 5.9).
	// An implementation copies `result_schema_path()` onto the envelope's
	// `payload_schema_path_override` so the Tool Executor's answer reaches the codec
	// rather than being re-derived; see the file comment for why that field exists.
	template <typename PayloadType>
	class ToolResultSink {
	public:
		virtual ~ToolResultSink() = default;

		// False when the result could not be queued — which the codec reports for a
		// payload that failed its own schema (requirement 4.1).
		virtual bool send_tool_result(const ToolResultResponse<PayloadType>& response) = 0;
	};

	// What the dispatcher needs from the Confirmation Coordinator. Two operations,
	// because `confirm:request` and `confirm:resolved` are two different things that
	// happen to the same prompt.
	//
	// Both are named after the coordinator's own envelope-level entry points, which
	// `confirmation_coordinator.h` labels as this component's in so many words. The
	// names matter more here than elsewhere because that component also has
	// `present_confirmation_request` and `resolve_confirmation`, which take the payload
	// already taken apart — four decoded strings between them, not an envelope. Naming
	// the seams after those would point an adapter author at the wrong half of a
	// component that has both.
	template <typename InboundEnvelopeType>
	class ConfirmationRouter {
	public:
		virtual ~ConfirmationRouter() = default;

		virtual void handle_confirmation_request(const InboundEnvelopeType& inbound_envelope) = 0;

		// Multi-client prompt dismissal (requirement 5.6). The coordinator owns the
		// pending table this reads, which is the one table in the extension there is a
		// reason for.
		virtual void handle_confirmation_resolved(const InboundEnvelopeType& inbound_envelope) = 0;
	};

	// What the dispatcher needs from the Context Builder: answer a request with the
	// snapshot, under the identifier it was asked with (requirements 5.3, 6.9).
	//
	// The identifier and nothing else, because `request:project_context` has an empty
	// payload — there is no bundled schema for it and nothing in it to read. A seam that
	// takes only the echo is also the narrowest way to make requirement 5.9 hold on this
	// path: there is no overload that could forget it.
	class ProjectContextRouter {
	public:
		virtual ~ProjectContextRouter() = default;

		virtual void answer_project_context_request(std::string_view request_id) = 0;
	};

	// What the dispatcher needs from the Transport Handler. The handler reads the
	// `command` field and acknowledges under the envelope's own identifier, so the whole
	// envelope goes across.
	template <typename InboundEnvelopeType>
	class TransportCommandRouter {
	public:
		virtual ~TransportCommandRouter() = default;

		virtual void execute_transport_command(const InboundEnvelopeType& inbound_envelope) = 0;
	};

	// What the dispatcher needs from the Stream Presenter: everything under `stream:`
	// and `error:`, which it recognises or does not on its own.
	template <typename InboundEnvelopeType>
	class StreamRouter {
	public:
		virtual ~StreamRouter() = default;

		virtual void present_stream_envelope(const InboundEnvelopeType& inbound_envelope) = 0;
	};

	// What the dispatcher needs from the UI Host: show the producer a generated
	// ReaScript they can download.
	//
	// A download and never an execution (ADR 0011, requirement 26.2). Nothing in the
	// payload instructs REAPER to run anything, and there is deliberately no operation
	// here that would.
	template <typename InboundEnvelopeType>
	class ScriptDownloadRouter {
	public:
		virtual ~ScriptDownloadRouter() = default;

		virtual void present_script_download(const InboundEnvelopeType& inbound_envelope) = 0;
	};

	// ---------------------------------------------------------------------------
	// Outcome
	// ---------------------------------------------------------------------------

	// What routing one envelope did. Returned rather than logged, for the reason
	// `TickReport` is: the caller knows how this build reports things.
	struct RoutingOutcome {
		// As it arrived, including when nothing could be done with it — which is the
		// value worth having in the log line requirement 5.8 asks for.
		std::string envelope_type;

		EnvelopeRoute route = EnvelopeRoute::unknown;

		// The identifier the envelope carried, echoed onto the response when there was
		// one. Empty when it carried none.
		std::string request_id;

		// The tail of `request:<tool_name>`. Empty for every other route.
		std::string tool_name;

		// True when a destination was reached. False only for `unknown`, which is not a
		// failure — requirement 5.8.
		bool routed = false;

		// True when a tool result was handed to the sink. Only the tool call route
		// produces a response here; the other destinations queue their own.
		bool response_sent = false;

		// Requirement 23.6: the name was not one this build implements. The response
		// still went out and still names it.
		bool tool_was_unknown = false;

		// The schema path the response carried — the Tool Executor's answer, not one
		// derived here. Empty when it had none, or when no response was produced.
		std::string response_schema_path;

		// Requirement 5.8's case, named so a caller does not have to compare against
		// the enumerator to recognise it.
		bool ignored() const { return route == EnvelopeRoute::unknown; }
	};

	// ---------------------------------------------------------------------------
	// The dispatcher
	// ---------------------------------------------------------------------------

	// Routes an inbound envelope to the component that owns it.
	//
	// Stateless between calls, deliberately: requirement 5.9's "no pending-request
	// table" is visible as there being nothing to hold one in. Runs on the main thread
	// inside the tick that drained the envelope, which is what makes a synchronous
	// answer possible and the table unnecessary.
	//
	// Templated on the inbound envelope type and the outbound payload type, so the whole
	// of the routing is exercisable without nlohmann/json. The seven destinations are
	// taken as references and are all of distinct types, so wiring two of them the wrong
	// way round does not compile.
	template <typename InboundEnvelopeType, typename PayloadType>
	class MessageDispatcher {
	public:
		MessageDispatcher(
			ToolCallRouter<InboundEnvelopeType, PayloadType>& tool_calls,
			ToolResultSink<PayloadType>& tool_results,
			ConfirmationRouter<InboundEnvelopeType>& confirmations,
			ProjectContextRouter& project_context,
			TransportCommandRouter<InboundEnvelopeType>& transport_commands,
			StreamRouter<InboundEnvelopeType>& streams,
			ScriptDownloadRouter<InboundEnvelopeType>& script_downloads
		)
			: tool_calls_{tool_calls},
			tool_results_{tool_results},
			confirmations_{confirmations},
			project_context_{project_context},
			transport_commands_{transport_commands},
			streams_{streams},
			script_downloads_{script_downloads}
		{
		}

		MessageDispatcher(const MessageDispatcher&) = delete;
		MessageDispatcher& operator=(const MessageDispatcher&) = delete;
		MessageDispatcher(MessageDispatcher&&) = delete;
		MessageDispatcher& operator=(MessageDispatcher&&) = delete;

		// The Main-Thread Dispatcher's entry point (requirement 2.1).
		//
		// The envelope is taken by const reference: nothing here keeps any part of it,
		// and the one string a response needs is copied by `ToolResultResponse`.
		RoutingOutcome route(const InboundEnvelopeType& inbound_envelope)
		{
			RoutingOutcome outcome;

			outcome.envelope_type = inbound_envelope.type;
			outcome.request_id = inbound_envelope.request_id;
			outcome.route = route_for_envelope_type(outcome.envelope_type);

			switch (outcome.route) {
				case EnvelopeRoute::tool_executor:
					outcome.tool_name.assign(tool_name_from_routed_envelope_type(outcome.envelope_type));
					route_tool_call(inbound_envelope, outcome);
					break;

				case EnvelopeRoute::confirmation_request:
					confirmations_.handle_confirmation_request(inbound_envelope);
					break;

				case EnvelopeRoute::confirmation_resolution:
					confirmations_.handle_confirmation_resolved(inbound_envelope);
					break;

				case EnvelopeRoute::context_builder:
					// The echo, passed across as the only thing this request carries.
					project_context_.answer_project_context_request(outcome.request_id);
					break;

				case EnvelopeRoute::transport_handler:
					transport_commands_.execute_transport_command(inbound_envelope);
					break;

				case EnvelopeRoute::stream_presenter:
					streams_.present_stream_envelope(inbound_envelope);
					break;

				case EnvelopeRoute::script_download:
					script_downloads_.present_script_download(inbound_envelope);
					break;

				case EnvelopeRoute::unknown:
					// Requirement 5.8 and 23.5. Nothing is called and nothing is
					// answered; the outcome names the type and the caller logs it. The
					// server's protocol being ahead of this build is the expected cause,
					// so an error response would be the extension complaining about a
					// version it is not entitled to an opinion on.
					return outcome;
			}

			outcome.routed = true;

			return outcome;
		}

	private:
		// Requirement 5.1, and requirements 5.9 and 23.6 as they land on this one path.
		//
		// The response is built from the inbound envelope rather than from fields copied
		// out of it, which is what makes the identifier echo structural, and it carries
		// the executor's schema answer rather than a second derivation of one.
		void route_tool_call(const InboundEnvelopeType& inbound_envelope, RoutingOutcome& outcome)
		{
			ToolCallResponse<PayloadType> response =
				tool_calls_.execute_tool_call(outcome.tool_name, inbound_envelope);

			outcome.tool_was_unknown = !response.tool_is_known;
			outcome.response_schema_path = response.result_schema_path;

			const ToolResultResponse<PayloadType> tool_result{
				inbound_envelope,
				outcome.tool_name,
				std::move(response.payload),
				response.result_schema_path
			};

			outcome.response_sent = tool_results_.send_tool_result(tool_result);
		}

		ToolCallRouter<InboundEnvelopeType, PayloadType>& tool_calls_;
		ToolResultSink<PayloadType>& tool_results_;
		ConfirmationRouter<InboundEnvelopeType>& confirmations_;
		ProjectContextRouter& project_context_;
		TransportCommandRouter<InboundEnvelopeType>& transport_commands_;
		StreamRouter<InboundEnvelopeType>& streams_;
		ScriptDownloadRouter<InboundEnvelopeType>& script_downloads_;
	};

	// ---------------------------------------------------------------------------
	// The spellings are the handlers' own
	// ---------------------------------------------------------------------------

	// Each exact route is checked against the constant the component that answers it
	// declares, or against the codec's own inbound binding table where the component has
	// no constant yet. A dispatcher routing on a spelling that drifted from the handler's
	// is a handler that is never reached, which is silence rather than a failure — so it
	// is caught here, at compile time, rather than by a producer whose transport buttons
	// stopped working.
	static_assert(
		route_for_envelope_type(transport_request_envelope_type) == EnvelopeRoute::transport_handler,
		"request:transport must reach the Transport Handler (requirement 5.4)"
	);

	static_assert(
		route_for_envelope_type(confirmation_request_envelope_type) == EnvelopeRoute::confirmation_request,
		"confirm:request must reach the Confirmation Coordinator (requirement 5.2)"
	);

	static_assert(
		route_for_envelope_type(confirmation_resolved_envelope_type) == EnvelopeRoute::confirmation_resolution,
		"confirm:resolved must reach the Confirmation Coordinator (requirement 5.6)"
	);

	static_assert(
		route_for_envelope_type(sesh_ai::context::project_context_request_envelope_type)
			== EnvelopeRoute::context_builder,
		"request:project_context must reach the Context Builder, not the Tool Executor (requirement 5.3)"
	);

	static_assert(
		route_for_envelope_type(sesh_ai::ui::stream_error_envelope_type) == EnvelopeRoute::stream_presenter,
		"stream:error must reach the Stream Presenter (requirement 5.5)"
	);

	static_assert(
		route_for_envelope_type(validation_error_envelope_type) == EnvelopeRoute::stream_presenter,
		"an error:* envelope must reach the Stream Presenter (requirement 5.5)"
	);

	static_assert(
		route_for_envelope_type(script_download_envelope_type) == EnvelopeRoute::script_download,
		"script:download must reach the UI Host (requirement 5.7)"
	);

	// The two outbound types the extension produces for a confirmation. Neither is ever
	// received, and an echo of one must not be mistaken for a route.
	static_assert(
		route_for_envelope_type(confirmation_approve_envelope_type) == EnvelopeRoute::unknown,
		"confirm:approve is outbound only"
	);

	static_assert(
		route_for_envelope_type(confirmation_reject_envelope_type) == EnvelopeRoute::unknown,
		"confirm:reject is outbound only"
	);

	// The acknowledgement the Transport Handler produces. It travels the other way, and
	// a `response:*` type arriving here is not a tool call however much it looks like
	// one — the prefix route is `request:`, not `response:`.
	static_assert(
		route_for_envelope_type(transport_response_envelope_type) == EnvelopeRoute::unknown,
		"response:transport is outbound only"
	);

}

#endif
