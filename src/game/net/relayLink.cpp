#include "relayLink.hpp"

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
