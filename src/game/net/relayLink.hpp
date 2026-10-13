#pragma once

// Client side of the WebSocket relay (server/ws.go) — how the web build
// reaches the other player. Browsers cannot send UDP, so instead of ENet/ICE
// both players open a WebSocket to the relay, meet in a room by its code,
// and the relay forwards their game messages (NetTransport::UseRelaySocket).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace relay {
// Wire protocol of server/ws.go — keep in sync. Every message is binary with
// a 1-byte type prefix.
inline constexpr uint8_t kCreateRoom = 0x01;
inline constexpr uint8_t kJoinRoom = 0x02;  // + room code
inline constexpr uint8_t kData = 0x10;      // + payload, forwarded to the peer
inline constexpr uint8_t kPing = 0x20;
inline constexpr uint8_t kPong = 0x21;
inline constexpr uint8_t kRoomCreated = 0x81;  // + room code
inline constexpr uint8_t kJoined = 0x82;       // + room code
inline constexpr uint8_t kPeerJoined = 0x83;
inline constexpr uint8_t kPeerLeft = 0x84;
inline constexpr uint8_t kError = 0x8F;  // + error code + message
inline constexpr size_t kRoomCodeLen = 6;
}  // namespace relay

// One WebSocket to the relay, moving whole binary messages. Abstract so the
// browser socket can be swapped for an in-memory one in tests.
struct RelaySocket {
  enum class State { kConnecting, kOpen, kClosed };

  RelaySocket() = default;
  virtual ~RelaySocket() = default;
  RelaySocket(RelaySocket const&) = delete;
  RelaySocket& operator=(RelaySocket const&) = delete;
  RelaySocket(RelaySocket&&) = delete;
  RelaySocket& operator=(RelaySocket&&) = delete;

  virtual State GetState() const = 0;
  // Sends one message (relay type byte first). Dropped unless kOpen.
  virtual void Send(std::vector<uint8_t> const& msg) = 0;
  // Pops the next received message; false when none is waiting.
  virtual bool Receive(std::vector<uint8_t>& msg) = 0;
};

// Browser WebSocket (Emscripten builds). nullptr on native builds.
std::unique_ptr<RelaySocket> OpenBrowserRelaySocket(std::string const& url);

// Relay URL for the page the game runs on: the `?relay=` query parameter if
// given, else ws(s)://<page host>/ws. Empty on native builds.
std::string DefaultRelayUrl();

// Room handshake over a RelaySocket: create a room (host) or join one by
// code. Once paired, ReleaseSocket() hands the socket to NetTransport; game
// messages the peer sends right after pairing stay queued in the socket.
struct RelayClient {
  enum class State { kIdle, kConnecting, kWaitingForPeer, kPaired, kFailed };

  // Empty `join_code` creates a room; otherwise joins that room.
  void Start(std::unique_ptr<RelaySocket> socket, std::string join_code);
  void Poll();

  State GetState() const { return state_; }
  // The room code (known once the room exists or was joined).
  std::string const& RoomCode() const { return code_; }
  // Human-readable reason when kFailed.
  std::string const& Error() const { return error_; }

  // Takes the socket once kPaired; nullptr otherwise.
  std::unique_ptr<RelaySocket> ReleaseSocket();

 private:
  void Fail(std::string error);

  std::unique_ptr<RelaySocket> socket_;
  State state_ = State::kIdle;
  std::string code_;
  std::string error_;
  bool joining_ = false;
  bool request_sent_ = false;
};
