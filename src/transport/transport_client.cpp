// The socket.io-client-cpp half of the Transport Client.
//
// This is the only translation unit in the extension that includes the Socket.IO
// client, websocketpp, or asio. Everything worth testing is in transport_client.h,
// behind `SocketConnection` — see that header for why, and for the four properties it
// makes structural. What is left here is the library call sequence and the translation
// of the library's callbacks into the four events `TransportClient` understands.
//
// ---------------------------------------------------------------------------
// It compiles only once the dependency is populated
//
// The Socket.IO client is declared in CMakeLists.txt as a FetchContent pinned to
// commit 3b7be7e4173b5bdeed393966e3274f65d513a280, and deliberately *not* populated:
// `FetchContent_MakeAvailable` has not been called, so nothing is downloaded and
// `sio_client.h` is not on any include path. A contributor with no vcpkg can still
// configure, build, and run the whole Catch2 suite, which is the property the top-level
// CMakeLists.txt goes to some trouble to preserve.
//
// So the body of this file is behind `__has_include(<sio_client.h>)`. Without the
// dependency it is an empty translation unit rather than a build failure, and the
// moment the dependency is populated it becomes the real thing with no edit here. The
// alternative — populating the dependency as part of this component — would put a
// source clone and a TLS-stack build in front of every configure, including the ones
// that only want to run the suite.
//
// ---------------------------------------------------------------------------
// The pin is an untagged `master` commit, and the handshake `auth` is why
//
// Requirement 3.1 puts the access token, `clientType`, and `protocolVersion` in the
// Socket.IO handshake `auth`, and the server reads exactly that — `socket.handshake.auth`
// in `readHandshakeAuth`, with no fallback to the query string or the headers.
//
// No tagged release of this library can do that. At tag 3.1.0 — released 2021-10-12 and
// still the newest tag upstream has cut — `sio::client::connect` has three overloads and
// none of them takes an auth message; `query` arrives as `socket.handshake.query` and
// `http_extra_headers` as `socket.handshake.headers`, neither of which the server reads.
// The overloads taking `const message::ptr& auth` landed on `master` six weeks after that
// tag, in upstream #335, and have never been released.
//
// So the pin is an untagged `master` commit, 3b7be7e (tip of master, 2025-08-28), rather
// than a release. `src/sio_client.h` at that commit declares the overload this file calls:
//
//     void connect(const std::string& uri, const std::map<std::string,std::string>& query,
//                  const std::map<std::string,std::string>& http_extra_headers,
//                  const message::ptr& auth);
//
// which is the signature `SocketIoConnection::open` calls below, in that argument order
// and with that const-ness — checked against the header at the pinned commit rather than
// taken from a changelog. Empty maps for `query` and `http_extra_headers`: the three
// fields requirement 3.1 names belong in `auth` and nowhere else, and a token in a query
// string is a token in the server's access log. DEPENDENCIES.md records what pinning past
// a release costs, and the forking alternative that was rejected.
//
// ---------------------------------------------------------------------------
// TLS is a compile-time property of this library, so it is checked at compile time
//
// Requirement 26.3 is TLS on every outbound connection. `SecureEndpoint` makes a
// plaintext URL unconstructable, and on its own that would not be enough: this library
// selects the scheme from the `SIO_TLS` build define and *rewrites* whatever URL it is
// given, so a non-TLS build silently connects `ws://` to the `https://` URL it was
// handed. A runtime assertion would not see it, because from this side the URL still
// says `https`.
//
// So the check is `#error`. A build without `SIO_TLS` does not produce a plaintext
// extension; it produces no extension.
//
// ---------------------------------------------------------------------------
// The library's own reconnection is switched off
//
// `set_reconnect_attempts(0)`. The library reconnects by replaying the URL and query
// string it was first given, so an internally-reconnected socket presents the token
// from the original handshake — the stale handshake requirement 3.2 exists to prevent.
// Every attempt originates in `TransportClient`, which builds a fresh auth each time.
//
// The transport is WebSocket only, which is this library's single documented limitation
// and is fine here: the extension connects from the producer's own machine rather than
// through a corporate proxy, and the URL it builds carries `transport=websocket` with no
// long-polling fallback.
//
// ---------------------------------------------------------------------------
// Why the rejection code arrives on a different listener from the failure
//
// A Socket.IO middleware refusal is a CONNECT_ERROR packet carrying
// `{ message, data: { code } }`, which this library surfaces through
// `sio::socket::on_error` as a message object. The connection failure itself arrives
// separately, on `set_fail_listener`, which takes no argument at all — so the code and
// the "it failed" notification cannot be read from one place.
//
// The packet arrives first, so the error listener records the code and the fail listener
// spends it. A failure with no recorded code is a connection that never got an answer
// from the server, which is exactly how `classify_handshake_rejection` reads an empty
// code.

#include <transport/transport_client.h>

#if __has_include(<sio_client.h>)

#ifndef SIO_TLS
#error "The Sesh AI extension requires socket.io-client-cpp built with SIO_TLS. Without it the client rewrites every URL to ws:// and requirement 26.3 (TLS on every outbound connection) cannot hold. See DEPENDENCIES.md."
#endif

#include <sio_client.h>
#include <sio_message.h>
#include <sio_socket.h>

#include <map>
#include <string>
#include <utility>

namespace sesh_ai::transport {

	namespace {

		// The server attaches its stable rejection code as `data.code` on the
		// CONNECT_ERROR payload. Read defensively: this is untrusted input arriving
		// before the connection is established, and every shape that is not the one
		// expected reads as "no code", which classifies as a network failure and backs
		// off.
		std::string read_server_rejection_code(const sio::message::ptr& error_message)
		{
			if (!error_message || error_message->get_flag() != sio::message::flag_object) {
				return std::string{};
			}

			const std::map<std::string, sio::message::ptr>& fields = error_message->get_map();

			const auto data_field = fields.find("data");

			if (data_field == fields.end()
				|| !data_field->second
				|| data_field->second->get_flag() != sio::message::flag_object) {
				return std::string{};
			}

			const std::map<std::string, sio::message::ptr>& data = data_field->second->get_map();

			const auto code_field = data.find("code");

			if (code_field == data.end()
				|| !code_field->second
				|| code_field->second->get_flag() != sio::message::flag_string) {
				return std::string{};
			}

			return code_field->second->get_string();
		}

		// The handshake `auth` object.
		//
		// Built and handed straight to `connect`, so the token exists as a plain string
		// inside this object for the duration of the call and nowhere else on this side.
		// `reveal()` is the one route to a `secret_string`'s characters and is named to
		// be conspicuous; this is the legitimate use of it.
		sio::message::ptr build_handshake_auth_message(const HandshakeAuth& handshake_auth)
		{
			const sio::message::ptr auth_message = sio::object_message::create();

			std::map<std::string, sio::message::ptr>& fields = auth_message->get_map();

			fields[std::string{handshake_auth_token_field}] =
				sio::string_message::create(handshake_auth.access_token().reveal());
			fields[std::string{handshake_auth_client_type_field}] =
				sio::string_message::create(std::string{HandshakeAuth::client_type()});
			fields[std::string{handshake_auth_protocol_version_field}] =
				sio::string_message::create(std::string{HandshakeAuth::protocol_version()});

			return auth_message;
		}

	}

	SocketIoConnection::SocketIoConnection()
		: client_{new sio::client{}}
	{
		// See the header comment. Reconnection belongs to TransportClient because only
		// TransportClient builds a fresh token for it.
		client_->set_reconnect_attempts(0);

		// The library's default logging writes the connection URL, and the URL is
		// assembled with a query string. Quiet rather than default so that nothing this
		// library decides to log can carry anything from the handshake (requirement
		// 26.4).
		client_->set_logs_quiet();
	}

	SocketIoConnection::~SocketIoConnection()
	{
		if (client_ == nullptr) {
			return;
		}

		client_->clear_con_listeners();
		client_->clear_socket_listeners();
		client_->sync_close();

		delete client_;
		client_ = nullptr;
	}

	void SocketIoConnection::report_to(TransportClient& client)
	{
		transport_client_ = &client;

		// The namespace socket is created on first request, before any connection, which
		// is what allows the error listener to be installed ahead of the first
		// handshake. Installing it afterwards would miss the refusal of that handshake —
		// the one most likely to be refused, since it is the one that has not yet proved
		// the token works.
		client_->socket()->on_error([this](const sio::message::ptr& error_message) {
			last_server_rejection_code_ = read_server_rejection_code(error_message);
		});

		// Fires when the namespace is connected, which is after the server's handshake
		// middleware accepted the token and the client type. `set_open_listener` fires
		// earlier, on the engine.io transport opening, and a socket that is open at the
		// transport level but refused at the Socket.IO level is not a connection this
		// extension has.
		client_->set_socket_open_listener([this](const std::string&) {
			last_server_rejection_code_.clear();

			if (transport_client_ != nullptr) {
				transport_client_->on_connected();
			}
		});

		// The handshake failed. The code, if the server sent one, was recorded by the
		// error listener above.
		client_->set_fail_listener([this] {
			const std::string rejection_code = std::move(last_server_rejection_code_);

			last_server_rejection_code_.clear();

			if (transport_client_ != nullptr) {
				transport_client_->on_handshake_rejected(rejection_code);
			}
		});

		// An established connection closed. `close_reason_normal` is this side having
		// asked, which TransportClient already knows about — reporting it would turn its
		// own `close()` into an event it has to filter. A drop is what it needs to hear.
		//
		// No code is passed, and that is correct rather than a shortcut. This library's
		// `close_reason` has two values and carries no string, so a drop observed here
		// genuinely has no reason attached. Requirement 17.6's case — the server dropping
		// a socket whose token passed `exp` — arrives with a reason, but through a
		// different route: the server pushes an `error:authentication` envelope carrying
		// `expired_token` before it disconnects, and the Message Dispatcher passes that
		// code to `on_disconnected`. So the eager refresh is driven by the envelope and
		// this listener reports the bare drop it can actually see.
		client_->set_close_listener([this](const sio::client::close_reason reason) {
			if (reason == sio::client::close_reason_normal) {
				return;
			}

			if (transport_client_ != nullptr) {
				transport_client_->on_disconnected();
			}
		});
	}

	bool SocketIoConnection::open(
		const SecureEndpoint& endpoint,
		const HandshakeAuth& handshake_auth
	)
	{
		if (client_ == nullptr || transport_client_ == nullptr) {
			return false;
		}

		// No query string and no extra headers. The three fields requirement 3.1 names
		// go in the `auth` object and nowhere else: a token in the query string is a
		// token in the server's access log.
		client_->connect(
			endpoint.url(),
			std::map<std::string, std::string>{},
			std::map<std::string, std::string>{},
			build_handshake_auth_message(handshake_auth)
		);

		return true;
	}

	void SocketIoConnection::close()
	{
		if (client_ == nullptr) {
			return;
		}

		// Idempotent, as SocketConnection's contract requires: closing an already
		// closed client is a no-op in this library, and the close listener filters
		// `close_reason_normal` so this cannot present itself back as a drop.
		client_->close();
	}

}

#endif
