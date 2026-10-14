#include "relayLink.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

#if OPENLIERO_EMSCRIPTEN
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

#include <deque>
#endif

#if OPENLIERO_EMSCRIPTEN

namespace {

// The browser delivers WebSocket events between frames on the main thread;
// they are queued here and drained by Receive() from the game loop.
struct BrowserRelaySocket : RelaySocket {
  explicit BrowserRelaySocket(std::string const& url) {
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url.c_str();
    attr.createOnMainThread = true;
    EMSCRIPTEN_WEBSOCKET_T const kWs = emscripten_websocket_new(&attr);
    if (kWs <= 0) {
      state_ = State::kClosed;
      return;
    }
    ws_ = kWs;
    emscripten_websocket_set_onopen_callback(ws_, this, &BrowserRelaySocket::OnOpen);
    emscripten_websocket_set_onmessage_callback(ws_, this, &BrowserRelaySocket::OnMessage);
    emscripten_websocket_set_onerror_callback(ws_, this, &BrowserRelaySocket::OnError);
    emscripten_websocket_set_onclose_callback(ws_, this, &BrowserRelaySocket::OnClose);
  }

  ~BrowserRelaySocket() override {
    if (ws_ > 0) {
      emscripten_websocket_close(ws_, 1000, "");
      // Also detaches the callbacks, so none can reach `this` afterwards.
      emscripten_websocket_delete(ws_);
    }
  }

  BrowserRelaySocket(BrowserRelaySocket const&) = delete;
  BrowserRelaySocket& operator=(BrowserRelaySocket const&) = delete;
  BrowserRelaySocket(BrowserRelaySocket&&) = delete;
  BrowserRelaySocket& operator=(BrowserRelaySocket&&) = delete;

  State GetState() const override { return state_; }

  void Send(std::vector<uint8_t> const& msg) override {
    if (state_ != State::kOpen || msg.empty()) {
      return;
    }
    // The API takes a non-const pointer but only copies the bytes out.
    emscripten_websocket_send_binary(ws_, const_cast<uint8_t*>(msg.data()),
                                     static_cast<uint32_t>(msg.size()));
  }

  bool Receive(std::vector<uint8_t>& msg) override {
    if (inbox_.empty()) {
      return false;
    }
    msg = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
  }

 private:
  static bool OnOpen(int /*type*/, EmscriptenWebSocketOpenEvent const* /*ev*/, void* user) {
    static_cast<BrowserRelaySocket*>(user)->state_ = State::kOpen;
    return true;
  }
  static bool OnMessage(int /*type*/, EmscriptenWebSocketMessageEvent const* ev, void* user) {
    if (!ev->isText && ev->numBytes > 0) {
      static_cast<BrowserRelaySocket*>(user)->inbox_.emplace_back(ev->data,
                                                                  ev->data + ev->numBytes);
    }
    return true;
  }
  static bool OnError(int /*type*/, EmscriptenWebSocketErrorEvent const* /*ev*/, void* user) {
    static_cast<BrowserRelaySocket*>(user)->state_ = State::kClosed;
    return true;
  }
  static bool OnClose(int /*type*/, EmscriptenWebSocketCloseEvent const* /*ev*/, void* user) {
    static_cast<BrowserRelaySocket*>(user)->state_ = State::kClosed;
    return true;
  }

  EMSCRIPTEN_WEBSOCKET_T ws_ = 0;
  State state_ = State::kConnecting;
  std::deque<std::vector<uint8_t>> inbox_;
};

}  // namespace

std::unique_ptr<RelaySocket> OpenBrowserRelaySocket(std::string const& url) {
  if (!emscripten_websocket_is_supported()) {
    return nullptr;
  }
  return std::make_unique<BrowserRelaySocket>(url);
}

std::string DefaultRelayUrl() {
  char const* const kUrl = emscripten_run_script_string(
      "(function () {"
      "  var q = new URLSearchParams(location.search).get('relay');"
      "  if (q) { return q; }"
      "  return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';"
      "})()");
  return kUrl ? std::string(kUrl) : std::string();
}

#else

std::unique_ptr<RelaySocket> OpenBrowserRelaySocket(std::string const& /*url*/) { return nullptr; }

std::string DefaultRelayUrl() { return {}; }

#endif

std::unique_ptr<RelaySocket> OpenResumableRelaySocket(std::string const& url) {
  // The first socket doubles as the check that WebSockets work at all.
  auto first = std::make_shared<std::unique_ptr<RelaySocket>>(OpenBrowserRelaySocket(url));
  if (!*first) {
    return nullptr;  // native build, or no WebSocket support
  }
  return std::make_unique<ResumableRelaySocket>(
      [url, first]() -> std::unique_ptr<RelaySocket> {
        if (*first) {
          return std::move(*first);
        }
        return OpenBrowserRelaySocket(url);
      },
      [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
      });
}

namespace {

void PutU32(std::vector<uint8_t>& msg, uint32_t v) {
  msg.push_back(static_cast<uint8_t>(v >> 24));
  msg.push_back(static_cast<uint8_t>(v >> 16));
  msg.push_back(static_cast<uint8_t>(v >> 8));
  msg.push_back(static_cast<uint8_t>(v));
}

uint32_t GetU32(uint8_t const* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

ResumableRelaySocket::ResumableRelaySocket(Opener open, Clock now)
    : open_(std::move(open)), now_(std::move(now)) {
  std::random_device rd;
  for (uint8_t& b : token_) {
    b = static_cast<uint8_t>(rd());
  }
  uint64_t const kNow = now_();
  lastPoll_ = kNow;
  lastHeard_ = kNow;
  attemptStarted_ = kNow;
  sock_ = open_();
  if (!sock_) {
    phase_ = Phase::kDead;
  }
}

ResumableRelaySocket::~ResumableRelaySocket() {
  // Leaving on purpose: without this the relay would hold our seat (and keep
  // the other player waiting) for minutes.
  if (SocketOpen()) {
    sock_->Send({relay::kLeave});
  }
}

bool ResumableRelaySocket::SocketOpen() const { return sock_ && sock_->GetState() == State::kOpen; }

RelaySocket::State ResumableRelaySocket::GetState() const {
  switch (phase_) {
    case Phase::kFirst:
      return sock_ ? sock_->GetState() : State::kClosed;
    case Phase::kLive:
    case Phase::kReconnecting:
      return State::kOpen;
    case Phase::kDead:
      break;
  }
  return State::kClosed;
}

void ResumableRelaySocket::Send(std::vector<uint8_t> const& msg) {
  if (phase_ == Phase::kDead || msg.empty()) {
    return;
  }
  if (msg[0] == relay::kCreateRoom || msg[0] == relay::kJoinRoom) {
    // Our token goes with the request, so the relay knows us if we return.
    std::vector<uint8_t> req(msg);
    if (msg[0] == relay::kJoinRoom && msg.size() >= 1 + relay::kRoomCodeLen) {
      code_.assign(msg.begin() + 1, msg.begin() + 1 + relay::kRoomCodeLen);
      req.resize(1 + relay::kRoomCodeLen);
    }
    req.insert(req.end(), token_.begin(), token_.end());
    if (SocketOpen()) {
      sock_->Send(req);
    }
    return;
  }
  if (msg[0] == relay::kData && phase_ != Phase::kFirst) {
    ++sent_;
    unacked_.push_back(msg);
    unackedBytes_ += msg.size();
    // Runaway guard only (acks keep this to about a second of traffic);
    // dropping makes a later resume fail rather than corrupt the stream.
    while (unackedBytes_ > kMaxUnackedBytes && unacked_.size() > 1) {
      unackedBytes_ -= unacked_.front().size();
      unacked_.pop_front();
      ++unackedStart_;
    }
    // While reconnecting it waits in unacked_ and goes out after the resume.
    if (phase_ == Phase::kLive && SocketOpen()) {
      sock_->Send(msg);
    }
    return;
  }
  if (SocketOpen() && phase_ != Phase::kReconnecting) {
    sock_->Send(msg);
  }
}

bool ResumableRelaySocket::Receive(std::vector<uint8_t>& msg) {
  if (inbox_.empty()) {
    return false;
  }
  msg = std::move(inbox_.front());
  inbox_.pop_front();
  return true;
}

void ResumableRelaySocket::Poll() {
  uint64_t const kNow = now_();
  if (kNow - lastPoll_ > kFrozenGapMs) {
    // The page was in the background and did not run: the socket had no
    // chance to be heard, so give it a fresh start instead of dropping it.
    lastHeard_ = kNow;
    attemptStarted_ = kNow;
  }
  lastPoll_ = kNow;

  if (phase_ == Phase::kDead) {
    return;
  }
  if (phase_ == Phase::kReconnecting && !sock_) {
    if (kNow - lostSince_ > kGiveUpMs) {
      phase_ = Phase::kDead;
      return;
    }
    if (kNow < nextAttempt_) {
      return;
    }
    sock_ = open_();
    resumeSent_ = false;
    attemptStarted_ = kNow;
    if (!sock_) {
      LoseSocket(kNow);
      return;
    }
  }
  if (!sock_) {
    return;
  }

  std::vector<uint8_t> msg;
  while (sock_ && phase_ != Phase::kDead && sock_->Receive(msg)) {
    lastHeard_ = kNow;
    Handle(std::move(msg), kNow);
  }
  if (phase_ == Phase::kDead || !sock_) {
    return;
  }

  switch (sock_->GetState()) {
    case State::kOpen:
      if (phase_ == Phase::kReconnecting && !resumeSent_) {
        std::vector<uint8_t> req{relay::kResume};
        req.insert(req.end(), code_.begin(), code_.end());
        req.insert(req.end(), token_.begin(), token_.end());
        PutU32(req, received_);
        sock_->Send(req);
        resumeSent_ = true;
        lastHeard_ = kNow;
      } else if (phase_ == Phase::kLive && kNow - lastAck_ >= kAckIntervalMs) {
        std::vector<uint8_t> ack{relay::kAck};
        PutU32(ack, received_);
        sock_->Send(ack);
        lastAck_ = kNow;
      }
      if (phase_ != Phase::kFirst && kNow - lastHeard_ > kSilenceMs) {
        LoseSocket(kNow);  // half-open: e.g. the phone switched networks
      }
      break;
    case State::kConnecting:
      if (phase_ == Phase::kReconnecting && kNow - attemptStarted_ > kConnectTimeoutMs) {
        LoseSocket(kNow);
      }
      break;
    case State::kClosed:
      LoseSocket(kNow);
      break;
  }
}

void ResumableRelaySocket::Handle(std::vector<uint8_t> msg, uint64_t now) {
  if (msg.empty()) {
    return;
  }
  switch (msg[0]) {
    case relay::kRoomCreated:
    case relay::kJoined:
      if (msg.size() == 1 + relay::kRoomCodeLen) {
        code_.assign(msg.begin() + 1, msg.end());
        if (phase_ == Phase::kFirst) {
          // We have a seat now; from here on a lost socket is resumed.
          phase_ = Phase::kLive;
          lastHeard_ = now;
          lastAck_ = now;
        }
      }
      break;
    case relay::kResumed:
      if (phase_ == Phase::kReconnecting && msg.size() == 5) {
        uint32_t const kGot = GetU32(msg.data() + 1);
        if (kGot < unackedStart_ || kGot > sent_) {
          ended_ = true;
          phase_ = Phase::kDead;
          return;
        }
        Forget(kGot);
        for (auto const& data : unacked_) {
          sock_->Send(data);
        }
        phase_ = Phase::kLive;
        peerAway_ = false;  // the relay says again if the other player is still away
        retryMs_ = kMinRetryMs;
        lastAck_ = now;
      }
      return;
    case relay::kAckReply:
      if (msg.size() == 5) {
        Forget(GetU32(msg.data() + 1));
      }
      return;
    case relay::kPong:
      return;
    case relay::kPeerAway:
      peerAway_ = true;
      return;
    case relay::kPeerBack:
      peerAway_ = false;
      return;
    case relay::kPeerLeft:
      ended_ = true;
      peerAway_ = false;
      break;
    case relay::kError:
      if (phase_ == Phase::kReconnecting) {
        // The room is gone (closed, or we were away too long).
        inbox_.push_back(std::move(msg));
        ended_ = true;
        phase_ = Phase::kDead;
        return;
      }
      break;
    case relay::kData:
      ++received_;
      break;
    default:
      break;
  }
  inbox_.push_back(std::move(msg));
}

void ResumableRelaySocket::LoseSocket(uint64_t now) {
  sock_.reset();  // no kLeave: the relay keeps our seat
  if (phase_ == Phase::kFirst || ended_ || code_.empty()) {
    phase_ = Phase::kDead;
    return;
  }
  if (phase_ == Phase::kLive) {
    phase_ = Phase::kReconnecting;
    lostSince_ = now;
    retryMs_ = kMinRetryMs;
    nextAttempt_ = now;  // straight away
    return;
  }
  nextAttempt_ = now + retryMs_;
  retryMs_ = std::min(retryMs_ * 2, kMaxRetryMs);
}

void ResumableRelaySocket::Forget(uint32_t acked) {
  while (unackedStart_ < acked && !unacked_.empty()) {
    unackedBytes_ -= unacked_.front().size();
    unacked_.pop_front();
    ++unackedStart_;
  }
}

namespace {

// Error codes as in server/protocol.go; the game font is upper-case only.
std::string ErrorText(uint8_t code) {
  switch (code) {
    case 0x01:
      return "ROOM NOT FOUND";
    case 0x02:
      return "ROOM FULL";
    default:
      return "SERVER ERROR";
  }
}

}  // namespace

void RelayClient::Start(std::unique_ptr<RelaySocket> socket, std::string join_code) {
  socket_ = std::move(socket);
  code_ = std::move(join_code);
  joining_ = !code_.empty();
  request_sent_ = false;
  error_.clear();
  if (!socket_ || socket_->GetState() == RelaySocket::State::kClosed) {
    Fail("CANNOT REACH SERVER");
    return;
  }
  if (joining_ && code_.size() != relay::kRoomCodeLen) {
    Fail("BAD ROOM CODE");
    return;
  }
  state_ = State::kConnecting;
}

void RelayClient::Poll() {
  if (!socket_ || state_ == State::kIdle || state_ == State::kPaired || state_ == State::kFailed) {
    return;
  }
  socket_->Poll();

  if (!request_sent_ && socket_->GetState() == RelaySocket::State::kOpen) {
    std::vector<uint8_t> req;
    if (joining_) {
      req.push_back(relay::kJoinRoom);
      req.insert(req.end(), code_.begin(), code_.end());
    } else {
      req.push_back(relay::kCreateRoom);
    }
    socket_->Send(req);
    request_sent_ = true;
  }

  // One message at a time: stop as soon as the room is paired, so the
  // peer's first game messages stay queued for NetTransport.
  std::vector<uint8_t> msg;
  while (state_ != State::kPaired && state_ != State::kFailed && socket_->Receive(msg)) {
    if (msg.empty()) {
      continue;
    }
    switch (msg[0]) {
      case relay::kRoomCreated:
        if (!joining_ && msg.size() == 1 + relay::kRoomCodeLen) {
          code_.assign(msg.begin() + 1, msg.end());
          state_ = State::kWaitingForPeer;
        }
        break;
      case relay::kPeerJoined:
        if (!joining_ && state_ == State::kWaitingForPeer) {
          state_ = State::kPaired;
        }
        break;
      case relay::kJoined:
        if (joining_) {
          state_ = State::kPaired;
        }
        break;
      case relay::kError:
        Fail(ErrorText(msg.size() > 1 ? msg[1] : 0));
        break;
      default:
        break;  // pongs, or data before pairing (not expected)
    }
  }

  if (state_ != State::kPaired && state_ != State::kFailed &&
      socket_->GetState() == RelaySocket::State::kClosed) {
    Fail("CANNOT REACH SERVER");
  }
}

std::unique_ptr<RelaySocket> RelayClient::ReleaseSocket() {
  if (state_ != State::kPaired) {
    return nullptr;
  }
  state_ = State::kIdle;
  return std::move(socket_);
}

void RelayClient::Fail(std::string error) {
  state_ = State::kFailed;
  error_ = std::move(error);
  socket_.reset();
}
