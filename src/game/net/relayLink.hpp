#pragma once

// Client side of the WebSocket relay (server/ws.go) — how the web build
// reaches the other player. Browsers cannot send UDP, so instead of ENet/ICE
// both players open a WebSocket to the relay, meet in a room by its code,
// and the relay forwards their game messages (NetTransport::UseRelaySocket).

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace relay {
// Wire protocol of server/ws.go — keep in sync. Every message is binary with
// a 1-byte type prefix.
inline constexpr uint8_t kCreateRoom = 0x01;  // + optional resume token
inline constexpr uint8_t kJoinRoom = 0x02;    // + room code + optional resume token
inline constexpr uint8_t kResume = 0x03;      // + room code + token + u32 data received
inline constexpr uint8_t kLeave = 0x04;
inline constexpr uint8_t kAck = 0x05;   // + u32 data received
inline constexpr uint8_t kData = 0x10;  // + payload, forwarded to the peer
inline constexpr uint8_t kPing = 0x20;
inline constexpr uint8_t kPong = 0x21;
inline constexpr uint8_t kRoomCreated = 0x81;  // + room code
inline constexpr uint8_t kJoined = 0x82;       // + room code
inline constexpr uint8_t kPeerJoined = 0x83;
inline constexpr uint8_t kPeerLeft = 0x84;
inline constexpr uint8_t kResumed = 0x85;  // + u32 data the server got from us
inline constexpr uint8_t kPeerAway = 0x86;
inline constexpr uint8_t kPeerBack = 0x87;
inline constexpr uint8_t kAckReply = 0x88;  // + u32 data the server got from us
inline constexpr uint8_t kError = 0x8F;     // + error code + message
inline constexpr size_t kRoomCodeLen = 6;
inline constexpr size_t kTokenLen = 16;
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
  // Housekeeping; called once per frame before Receive.
  virtual void Poll() {}
  // Our own link to the relay is down and being restored (still kOpen).
  virtual bool Reconnecting() const { return false; }
  // The other player's link is down; the relay waits for it to come back.
  virtual bool PeerAway() const { return false; }
};

// Browser WebSocket (Emscripten builds). nullptr on native builds.
std::unique_ptr<RelaySocket> OpenBrowserRelaySocket(std::string const& url);

// A link to the relay that outlives its WebSocket. Phones drop a page's
// sockets as soon as the browser goes to the background (say, to paste the
// room code into a messenger); the relay keeps our seat for a few minutes
// and this reconnects and takes it back (server/ws.go: WSResume), so the
// room and the match survive. Data messages are counted both ways and kept
// until the relay acknowledges them, so nothing is lost or repeated across a
// reconnect. Stays kOpen while reconnecting; kClosed once the room is gone.
// Destroying it leaves the room for good (WSLeave).
class ResumableRelaySocket : public RelaySocket {
 public:
  using Opener = std::function<std::unique_ptr<RelaySocket>()>;
  using Clock = std::function<uint64_t()>;  // milliseconds, monotonic

  static constexpr uint64_t kAckIntervalMs = 1000;
  // Nothing heard for this long (acks are answered) means a dead socket.
  static constexpr uint64_t kSilenceMs = 8000;
  // A longer gap between polls means the page was frozen in the background,
  // not that the socket went quiet.
  static constexpr uint64_t kFrozenGapMs = 2000;
  static constexpr uint64_t kConnectTimeoutMs = 10000;
  static constexpr uint64_t kMinRetryMs = 500;
  static constexpr uint64_t kMaxRetryMs = 5000;
  // Longer than the relay waits for us (wsResumeGraceAlone); it decides.
  static constexpr uint64_t kGiveUpMs = 16ULL * 60 * 1000;
  static constexpr size_t kMaxUnackedBytes = 32 << 20;

  ResumableRelaySocket(Opener open, Clock now);
  ~ResumableRelaySocket() override;
  ResumableRelaySocket(ResumableRelaySocket const&) = delete;
  ResumableRelaySocket& operator=(ResumableRelaySocket const&) = delete;
  ResumableRelaySocket(ResumableRelaySocket&&) = delete;
  ResumableRelaySocket& operator=(ResumableRelaySocket&&) = delete;

  State GetState() const override;
  void Send(std::vector<uint8_t> const& msg) override;
  bool Receive(std::vector<uint8_t>& msg) override;
  void Poll() override;
  bool Reconnecting() const override { return phase_ == Phase::kReconnecting; }
  bool PeerAway() const override { return peerAway_; }

 private:
  // kFirst: the first socket, before we have a seat in a room (losing it
  // then is final). kLive: in a room. kReconnecting: in a room, socket lost.
  enum class Phase { kFirst, kLive, kReconnecting, kDead };

  void Handle(std::vector<uint8_t> msg, uint64_t now);
  void LoseSocket(uint64_t now);
  void Forget(uint32_t acked);
  bool SocketOpen() const;

  Opener open_;
  Clock now_;
  std::unique_ptr<RelaySocket> sock_;
  Phase phase_ = Phase::kFirst;
  std::array<uint8_t, relay::kTokenLen> token_{};
  std::string code_;
  bool ended_ = false;  // the peer left or the room closed: nothing to resume
  bool peerAway_ = false;
  bool resumeSent_ = false;
  std::deque<std::vector<uint8_t>> inbox_;

  // Data messages we sent, numbered from 0; unacked_ holds those from
  // unackedStart_ on that the relay may not have yet.
  uint32_t sent_ = 0;
  uint32_t unackedStart_ = 0;
  size_t unackedBytes_ = 0;
  std::deque<std::vector<uint8_t>> unacked_;
  uint32_t received_ = 0;  // data messages received

  uint64_t lastPoll_ = 0;
  uint64_t lastHeard_ = 0;
  uint64_t lastAck_ = 0;
  uint64_t attemptStarted_ = 0;
  uint64_t nextAttempt_ = 0;
  uint64_t retryMs_ = kMinRetryMs;
  uint64_t lostSince_ = 0;
};

// ResumableRelaySocket over browser WebSockets to `url`. nullptr on native
// builds.
std::unique_ptr<RelaySocket> OpenResumableRelaySocket(std::string const& url);

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
  // The link to the relay dropped and is being restored; the room is kept.
  bool Reconnecting() const { return socket_ && socket_->Reconnecting(); }

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
