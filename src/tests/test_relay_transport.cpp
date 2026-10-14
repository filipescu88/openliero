// Online play through the WebSocket relay (web build): RelayClient pairs two
// players in a room, NetTransport carries the game over the paired socket,
// and two NetSessions get from the room code to a running, in-sync match.
//
// The relay is an in-memory stand-in for server/ws.go speaking the same
// protocol, so this runs without a browser or network (the browser socket
// itself is exercised end-to-end in the web build).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game.hpp"
#include "gfx/renderer.hpp"
#include "math.hpp"
#include "net/relayLink.hpp"
#include "net/session.hpp"
#include "net/transport.hpp"
#include "viewport.hpp"
#include "webPage.hpp"

namespace {

struct FakeRelay;

// One "WebSocket" to the fake relay.
struct MemSocket : RelaySocket {
  MemSocket(FakeRelay& relay, int id, State state) : relay(relay), id(id), state(state) {}
  ~MemSocket() override;
  MemSocket(MemSocket const&) = delete;
  MemSocket& operator=(MemSocket const&) = delete;
  MemSocket(MemSocket&&) = delete;
  MemSocket& operator=(MemSocket&&) = delete;

  State GetState() const override { return state; }
  void Send(std::vector<uint8_t> const& msg) override;
  bool Receive(std::vector<uint8_t>& msg) override {
    if (inbox.empty()) {
      return false;
    }
    msg = std::move(inbox.front());
    inbox.pop_front();
    return true;
  }

  FakeRelay& relay;
  int id;
  State state;
  std::deque<std::vector<uint8_t>> inbox;
};

// Rooms, pairing and forwarding as in server/ws.go.
struct FakeRelay {
  std::unique_ptr<MemSocket> Connect(RelaySocket::State state = RelaySocket::State::kOpen) {
    auto s = std::make_unique<MemSocket>(*this, next_id_++, state);
    sockets_[s->id] = s.get();
    return s;
  }

  // The WebSocket handshake completes.
  static void Open(MemSocket& s) { s.state = RelaySocket::State::kOpen; }

  // Server side drops a connection (e.g. the relay restarts).
  void Drop(MemSocket& s) {
    s.state = RelaySocket::State::kClosed;
    Closed(s.id);
  }

  void OnMessage(int from, std::vector<uint8_t> const& msg) {
    if (msg.empty()) {
      return;
    }
    switch (msg[0]) {
      case relay::kCreateRoom: {
        std::string const kCode = "ROOM" + std::to_string(10 + rooms_.size());
        rooms_[kCode] = {from, 0};
        room_of_[from] = kCode;
        std::vector<uint8_t> reply{relay::kRoomCreated};
        reply.insert(reply.end(), kCode.begin(), kCode.end());
        Deliver(from, reply);
        break;
      }
      case relay::kJoinRoom: {
        if (msg.size() < 1 + relay::kRoomCodeLen) {
          break;
        }
        // A resume token may follow the code; this relay ignores it.
        std::string const kCode(msg.begin() + 1, msg.begin() + 1 + relay::kRoomCodeLen);
        auto it = rooms_.find(kCode);
        if (it == rooms_.end()) {
          Deliver(from, {relay::kError, 0x01});
        } else if (it->second.second != 0) {
          Deliver(from, {relay::kError, 0x02});
        } else {
          it->second.second = from;
          room_of_[from] = kCode;
          std::vector<uint8_t> reply{relay::kJoined};
          reply.insert(reply.end(), kCode.begin(), kCode.end());
          Deliver(from, reply);
          Deliver(it->second.first, {relay::kPeerJoined});
        }
        break;
      }
      case relay::kData:
        ++data_messages;
        if (int const kOther = Other(from); kOther != 0) {
          Deliver(kOther, msg);
        }
        break;
      default:
        break;
    }
  }

  void Closed(int id) {
    sockets_.erase(id);
    auto it = room_of_.find(id);
    if (it == room_of_.end()) {
      return;
    }
    int const kOther = Other(id);
    rooms_.erase(it->second);
    room_of_.erase(it);
    if (kOther != 0) {
      room_of_.erase(kOther);
      Deliver(kOther, {relay::kPeerLeft});
    }
  }

  int data_messages = 0;

 private:
  int Other(int id) const {
    auto it = room_of_.find(id);
    if (it == room_of_.end()) {
      return 0;
    }
    auto const& room = rooms_.at(it->second);
    return room.first == id ? room.second : room.first;
  }

  void Deliver(int to, std::vector<uint8_t> const& msg) {
    if (auto it = sockets_.find(to); it != sockets_.end()) {
      it->second->inbox.push_back(msg);
    }
  }

  int next_id_ = 1;
  std::map<int, MemSocket*> sockets_;
  std::map<std::string, std::pair<int, int>> rooms_;  // code → host id, client id
  std::map<int, std::string> room_of_;
};

MemSocket::~MemSocket() {
  if (state != State::kClosed) {
    relay.Closed(id);
  }
}

void MemSocket::Send(std::vector<uint8_t> const& msg) {
  if (state == State::kOpen) {
    relay.OnMessage(id, msg);
  }
}

// Pairs two RelayClients through `relay` and returns linked transports.
std::pair<NetTransport, NetTransport> PairedTransports(FakeRelay& relay) {
  RelayClient host;
  host.Start(relay.Connect(), "");
  host.Poll();
  REQUIRE(host.GetState() == RelayClient::State::kWaitingForPeer);

  RelayClient client;
  client.Start(relay.Connect(), host.RoomCode());
  client.Poll();
  host.Poll();
  REQUIRE(client.GetState() == RelayClient::State::kPaired);
  REQUIRE(host.GetState() == RelayClient::State::kPaired);

  std::pair<NetTransport, NetTransport> out;
  REQUIRE(out.first.UseRelaySocket(host.ReleaseSocket()));
  REQUIRE(out.second.UseRelaySocket(client.ReleaseSocket()));
  return out;
}

struct Env {
  std::shared_ptr<Common> common;
  std::shared_ptr<Settings> settings;
  FsNode tc_root;

  Env() {
    PrecomputeTables();
    common = std::make_shared<Common>();
    tc_root = FsNode("data") / "TC" / "openliero";
    common->load(tc_root);
    settings = std::make_shared<Settings>();
    settings->lives = 10;
    settings->loading_time = 0;
    settings->load_change = true;
    settings->random_level = true;
    settings->game_mode = Settings::kGmKillEmAll;
  }
};

}  // namespace

TEST_CASE("RelayClient pairs a host and a client by room code", "[relay]") {
  FakeRelay relay;
  RelayClient host;
  auto host_sock = relay.Connect(RelaySocket::State::kConnecting);
  MemSocket& host_raw = *host_sock;
  host.Start(std::move(host_sock), "");
  host.Poll();
  CHECK(host.GetState() == RelayClient::State::kConnecting);  // socket not open yet

  // Once the socket opens, the create request goes out on the next poll.
  FakeRelay::Open(host_raw);
  host.Poll();
  REQUIRE(host.GetState() == RelayClient::State::kWaitingForPeer);
  REQUIRE(host.RoomCode().size() == relay::kRoomCodeLen);

  RelayClient client;
  client.Start(relay.Connect(), host.RoomCode());
  client.Poll();
  CHECK(client.GetState() == RelayClient::State::kPaired);
  CHECK(client.RoomCode() == host.RoomCode());
  host.Poll();
  CHECK(host.GetState() == RelayClient::State::kPaired);
  CHECK(host.ReleaseSocket() != nullptr);
  CHECK(host.ReleaseSocket() == nullptr);  // only once
}

TEST_CASE("RelayClient reports relay errors", "[relay]") {
  FakeRelay relay;

  RelayClient unknown;
  unknown.Start(relay.Connect(), "NOROOM");
  unknown.Poll();
  CHECK(unknown.GetState() == RelayClient::State::kFailed);
  CHECK(unknown.Error() == "ROOM NOT FOUND");

  RelayClient bad_code;
  bad_code.Start(relay.Connect(), "ABC");
  CHECK(bad_code.GetState() == RelayClient::State::kFailed);

  RelayClient no_socket;
  no_socket.Start(nullptr, "");
  CHECK(no_socket.GetState() == RelayClient::State::kFailed);
  CHECK(no_socket.Error() == "CANNOT REACH SERVER");

  RelayClient dropped;
  auto sock = relay.Connect();
  MemSocket& raw = *sock;
  dropped.Start(std::move(sock), "");
  dropped.Poll();
  relay.Drop(raw);
  dropped.Poll();
  CHECK(dropped.GetState() == RelayClient::State::kFailed);
}

TEST_CASE("Native builds have no browser relay socket", "[relay]") {
  CHECK(OpenBrowserRelaySocket("ws://localhost/ws") == nullptr);
  CHECK(DefaultRelayUrl().empty());
}

TEST_CASE("Invite links accept only relay room codes", "[relay]") {
  CHECK(web_page::IsRoomCode("AB12CD"));
  CHECK(web_page::IsRoomCode("Z9Z9Z9"));
  CHECK_FALSE(web_page::IsRoomCode("ab12cd"));   // normalized to upper case before this
  CHECK_FALSE(web_page::IsRoomCode("AB12C"));    // too short
  CHECK_FALSE(web_page::IsRoomCode("AB12CDE"));  // too long
  CHECK_FALSE(web_page::IsRoomCode("AB'+1D"));   // pasted into page script: no quotes

  CHECK(web_page::ExtractRoomCode("ab12cd") == "AB12CD");
  CHECK(web_page::ExtractRoomCode("  ab1-2cd \n") == "AB12CD");
  CHECK(web_page::ExtractRoomCode("https://liero.pl/?join=ab12cd#top") == "AB12CD");
  CHECK(web_page::ExtractRoomCode("https://liero.pl/?relay=wss://x&join=zx98qp&x=1") == "ZX98QP");
  CHECK(web_page::ExtractRoomCode("Liero (kod pokoju: k7m9p2): https://liero.pl/?join=k7m9p2") ==
        "K7M9P2");
  CHECK(web_page::ExtractRoomCode("Kod pokoju: qw34er") == "QW34ER");
  CHECK(web_page::ExtractRoomCode("ROOM CODE: qw34er") == "QW34ER");
  CHECK(web_page::ExtractRoomCode("https://liero.pl/index.html").empty());
  CHECK(web_page::ExtractRoomCode("abc").empty());

  // Native builds have no page to read from or share to.
  CHECK(web_page::TakeJoinCodeFromUrl().empty());
  CHECK(web_page::InviteLink("AB12CD").empty());
  CHECK_FALSE(web_page::ShareInvite("AB12CD"));
}

TEST_CASE("NetTransport carries game packets over the relay", "[relay]") {
  FakeRelay relay;
  auto [host, client] = PairedTransports(relay);

  int host_connected = 0;
  int client_connected = 0;
  host.on_connected = [&] { ++host_connected; };
  client.on_connected = [&] { ++client_connected; };

  uint32_t got_seed = 0;
  client.on_handshake = [&](uint32_t seed, uint32_t /*hash*/) { got_seed = seed; };
  std::vector<uint8_t> got_inputs;
  uint32_t got_remote_frame = 0;
  host.on_remote_input_batch = [&](uint8_t /*gen*/, uint32_t /*base*/, uint8_t count,
                                   uint8_t const* inputs, uint32_t remote_frame) {
    got_inputs.assign(inputs, inputs + count);
    got_remote_frame = remote_frame;
  };
  size_t got_map_bytes = 0;
  bool map_intact = false;
  client.on_map_data = [&](void const* data, size_t len) {
    got_map_bytes = len;
    auto const* p = static_cast<uint8_t const*>(data);
    map_intact = len > 0 && p[0] == 0x5A && p[len - 1] == 0x5A;
  };

  // Nothing can be sent before the link counts as connected.
  host.SendHandshake(1, 2);
  CHECK(relay.data_messages == 0);

  host.Poll();
  client.Poll();
  CHECK(host_connected == 1);
  CHECK(client_connected == 1);
  CHECK(host.CurrentState() == NetTransport::kConnected);

  host.SendHandshake(0xC0FFEE, 7);
  uint8_t const kInputs[3] = {1, 2, 3};
  client.SendInputBatch(/*generation=*/0, /*base_frame=*/100, 3, /*local_delta=*/2, kInputs);
  std::vector<uint8_t> const kMap(300'000, 0x5A);
  host.SendMapData(kMap.data(), kMap.size());

  host.Poll();
  client.Poll();
  CHECK(got_seed == 0xC0FFEE);
  CHECK(got_inputs == std::vector<uint8_t>{1, 2, 3});
  CHECK(got_remote_frame == 102);
  CHECK(got_map_bytes == kMap.size());
  CHECK(map_intact);
  CHECK(host_connected == 1);  // on_connected fires once
}

TEST_CASE("NetTransport over the relay notices the peer leaving", "[relay]") {
  FakeRelay relay;
  auto [host, client] = PairedTransports(relay);
  bool host_lost = false;
  host.on_disconnected = [&] { host_lost = true; };
  host.Poll();
  client.Poll();

  client.Disconnect();  // closes the client's socket → relay tells the host
  CHECK_FALSE(host.Poll());
  CHECK(host_lost);
  CHECK(host.CurrentState() == NetTransport::kDisconnected);
}

TEST_CASE("Two NetSessions play a match over the relay", "[relay][session]") {
  Env const kE;
  FakeRelay relay;  // outlives the sessions' sockets
  auto [host_transport, client_transport] = PairedTransports(relay);

  NetSession host(kE.common, kE.settings, kE.tc_root);
  NetSession client(kE.common, kE.settings, kE.tc_root);
  bool const kHostStarted = host.HostWithTransport(std::move(host_transport));
  bool const kClientStarted = client.JoinWithTransport(std::move(client_transport));
  REQUIRE(kHostStarted);
  REQUIRE(kClientStarted);

  auto const kBothPlaying = [&] {
    return host.State() == NetSession::kPlaying && client.State() == NetSession::kPlaying;
  };
  for (int i = 0; i < 200 && !kBothPlaying(); ++i) {
    host.Update();
    client.Update();
  }
  REQUIRE(host.State() == NetSession::kPlaying);
  REQUIRE(client.State() == NetSession::kPlaying);
  REQUIRE(host.Rollback()->game.rand == client.Rollback()->game.rand);

  host.Rollback()->Focus();
  client.Rollback()->Focus();
  for (int i = 0; i < 200; ++i) {
    host.Rollback()->Process();
    client.Rollback()->Process();
    host.Update();
    client.Update();
  }
  CHECK(host.Rollback()->CurrentFrame() > 0);
  int32_t const kGap = static_cast<int32_t>(host.Rollback()->CurrentFrame()) -
                       static_cast<int32_t>(client.Rollback()->CurrentFrame());
  CHECK(std::abs(kGap) <= RollbackController::kFrameAdvantage);
  CHECK_FALSE(host.DesyncDetected());
  CHECK_FALSE(client.DesyncDetected());

  // The other player closes the page: the session drops out cleanly.
  client.Disconnect();
  client.Transport().Disconnect();
  for (int i = 0; i < 5; ++i) {
    host.Update();
  }
  CHECK(host.State() == NetSession::kDisconnected);
}

TEST_CASE("A single-view peer stays in sync and draws its own worm wide", "[relay][session]") {
  Env const kE;
  FakeRelay relay;
  auto [host_transport, client_transport] = PairedTransports(relay);

  NetSession host(kE.common, kE.settings, kE.tc_root);
  NetSession client(kE.common, kE.settings, kE.tc_root);
  bool const kHostStarted = host.HostWithTransport(std::move(host_transport));
  bool const kClientStarted = client.JoinWithTransport(std::move(client_transport));
  REQUIRE(kHostStarted);
  REQUIRE(kClientStarted);
  for (int i = 0;
       i < 200 && (host.State() != NetSession::kPlaying || client.State() != NetSession::kPlaying);
       ++i) {
    host.Update();
    client.Update();
  }
  REQUIRE(host.State() == NetSession::kPlaying);
  REQUIRE(client.State() == NetSession::kPlaying);

  // Only the client switches (as the web build does); the host keeps split
  // screen. Display layout must not leak into the simulation.
  RollbackController& rc = *client.Rollback();
  rc.UseSingleView();
  REQUIRE(rc.game.viewports.size() == 2);
  for (Viewport const* vp : rc.game.viewports) {
    if (vp->rect.Width() > 0) {
      CHECK(vp->rect.Width() == 320);
      CHECK(vp->rect.Height() == 158);
    } else {
      CHECK(vp->rect.Height() == 0);  // the opponent: status bar only
    }
  }

  Renderer renderer;
  renderer.Init(320, 200);
  renderer.LoadPalette(*kE.common);

  host.Rollback()->Focus();
  rc.Focus();
  for (int i = 0; i < 300; ++i) {
    host.Rollback()->Process();
    rc.Process();
    host.Update();
    client.Update();
    if (i % 50 == 0) {
      rc.Draw(renderer, /*use_spectator_viewports=*/false);
    }
  }
  CHECK(rc.CurrentFrame() > 0);
  CHECK_FALSE(host.DesyncDetected());
  CHECK_FALSE(client.DesyncDetected());
}

namespace {

// A socket whose far end is the test: it reads what was sent and scripts
// what the relay answers.
struct ScriptSocket : RelaySocket {
  State GetState() const override { return state; }
  void Send(std::vector<uint8_t> const& msg) override {
    if (state == State::kOpen) {
      sent.push_back(msg);
    }
  }
  bool Receive(std::vector<uint8_t>& msg) override {
    if (inbox.empty()) {
      return false;
    }
    msg = std::move(inbox.front());
    inbox.pop_front();
    return true;
  }

  State state = State::kOpen;
  std::vector<std::vector<uint8_t>> sent;
  std::deque<std::vector<uint8_t>> inbox;
};

// Owns the ResumableRelaySocket under test, its clock, and every socket it
// opened (they outlive the wrapper's handles so the test can inspect them).
struct ResumeRig {
  ResumeRig() {
    sock = std::make_unique<ResumableRelaySocket>(
        [this]() -> std::unique_ptr<RelaySocket> {
          if (!can_connect) {
            return nullptr;
          }
          return std::make_unique<Proxy>(*this);
        },
        [this] { return now; });
  }

  // Forwards to the newest ScriptSocket (kept in `opened`).
  struct Proxy : RelaySocket {
    explicit Proxy(ResumeRig& rig)
        : script(rig.opened.emplace_back(std::make_shared<ScriptSocket>())) {
      script->state = rig.open_state;
    }
    ~Proxy() override { script->state = State::kClosed; }
    Proxy(Proxy const&) = delete;
    Proxy& operator=(Proxy const&) = delete;
    Proxy(Proxy&&) = delete;
    Proxy& operator=(Proxy&&) = delete;
    State GetState() const override { return script->GetState(); }
    void Send(std::vector<uint8_t> const& msg) override { script->Send(msg); }
    bool Receive(std::vector<uint8_t>& msg) override { return script->Receive(msg); }
    std::shared_ptr<ScriptSocket> script;
  };

  ScriptSocket& Last() { return *opened.back(); }

  // Creates a room as the host would and gets it "joined".
  void IntoRoom() {
    sock->Send({relay::kCreateRoom});
    Last().inbox.push_back({relay::kRoomCreated, 'A', 'B', 'C', '2', '3', '4'});
    Tick(16);
    Last().inbox.push_back({relay::kPeerJoined});
    Tick(16);
    std::vector<uint8_t> msg;
    while (sock->Receive(msg)) {
    }
  }

  void Tick(uint64_t ms) {
    now += ms;
    sock->Poll();
  }

  uint64_t now = 1000;
  bool can_connect = true;
  RelaySocket::State open_state = RelaySocket::State::kOpen;
  std::vector<std::shared_ptr<ScriptSocket>> opened;
  std::unique_ptr<ResumableRelaySocket> sock;
};

std::vector<uint8_t> WithCount(uint8_t type, uint32_t n) {
  return {type, static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
          static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
}

bool SentType(ScriptSocket const& s, uint8_t type) {
  return std::ranges::any_of(s.sent, [type](auto const& m) { return !m.empty() && m[0] == type; });
}

}  // namespace

TEST_CASE("ResumableRelaySocket sends a resume token with the room request", "[relay][resume]") {
  ResumeRig rig;
  rig.sock->Send({relay::kJoinRoom, 'A', 'B', 'C', '2', '3', '4'});
  REQUIRE(rig.Last().sent.size() == 1);
  CHECK(rig.Last().sent[0].size() == 1 + relay::kRoomCodeLen + relay::kTokenLen);

  ResumeRig host;
  host.sock->Send({relay::kCreateRoom});
  REQUIRE(host.Last().sent.size() == 1);
  CHECK(host.Last().sent[0].size() == 1 + relay::kTokenLen);
}

TEST_CASE("ResumableRelaySocket takes its seat back after the socket drops", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  auto const kToken =
      std::vector<uint8_t>(rig.Last().sent[0].begin() + 1, rig.Last().sent[0].end());

  // Two messages each way before the drop.
  rig.sock->Send({relay::kData, 1});
  rig.sock->Send({relay::kData, 2});
  rig.Last().inbox.push_back({relay::kData, 10});
  rig.Last().inbox.push_back({relay::kData, 11});
  rig.Tick(16);
  std::vector<uint8_t> msg;
  REQUIRE(rig.sock->Receive(msg));
  REQUIRE(rig.sock->Receive(msg));

  // The browser drops the socket (page in the background).
  rig.Last().state = RelaySocket::State::kClosed;
  rig.Tick(16);
  CHECK(rig.sock->Reconnecting());
  CHECK(rig.sock->GetState() == RelaySocket::State::kOpen);  // the game sees no disconnect
  rig.sock->Send({relay::kData, 3});                         // held until resumed

  rig.Tick(16);
  REQUIRE(rig.opened.size() == 2);
  ScriptSocket& again = rig.Last();
  REQUIRE(again.sent.size() == 1);
  std::vector<uint8_t> want{relay::kResume, 'A', 'B', 'C', '2', '3', '4'};
  want.insert(want.end(), kToken.begin(), kToken.end());
  auto const kCount = WithCount(0, 2);  // we have the peer's two messages
  want.insert(want.end(), kCount.begin() + 1, kCount.end());
  CHECK(again.sent[0] == want);

  // The relay only got our first message: the second and third go again.
  again.inbox.push_back(WithCount(relay::kResumed, 1));
  again.inbox.push_back({relay::kPeerJoined});
  again.inbox.push_back({relay::kData, 12});
  rig.Tick(16);
  CHECK_FALSE(rig.sock->Reconnecting());
  REQUIRE(again.sent.size() >= 3);
  CHECK(again.sent[1] == std::vector<uint8_t>{relay::kData, 2});
  CHECK(again.sent[2] == std::vector<uint8_t>{relay::kData, 3});

  REQUIRE(rig.sock->Receive(msg));
  CHECK(msg == std::vector<uint8_t>{relay::kPeerJoined});
  REQUIRE(rig.sock->Receive(msg));
  CHECK(msg == std::vector<uint8_t>{relay::kData, 12});
  CHECK_FALSE(rig.sock->Receive(msg));
}

TEST_CASE("ResumableRelaySocket acks what it got and forgets what the relay has",
          "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  rig.sock->Send({relay::kData, 1});
  rig.Last().inbox.push_back({relay::kData, 10});
  rig.Tick(ResumableRelaySocket::kAckIntervalMs);
  CHECK(rig.Last().sent.back() == WithCount(relay::kAck, 1));

  rig.Last().inbox.push_back(WithCount(relay::kAckReply, 1));
  rig.Tick(16);
  rig.Last().state = RelaySocket::State::kClosed;
  rig.Tick(16);
  rig.Tick(16);
  rig.Last().inbox.push_back(WithCount(relay::kResumed, 1));
  rig.Tick(16);
  CHECK_FALSE(rig.sock->Reconnecting());
  CHECK_FALSE(SentType(rig.Last(), relay::kData));  // nothing to send again
}

TEST_CASE("ResumableRelaySocket replaces a socket that went silent", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  for (uint64_t t = 0; t <= ResumableRelaySocket::kSilenceMs; t += 500) {
    rig.Tick(500);  // acks go out, no reply ever comes
  }
  CHECK(rig.sock->Reconnecting());
  rig.Tick(16);
  CHECK(rig.opened.size() == 2);
  CHECK(SentType(rig.Last(), relay::kResume));
}

TEST_CASE("ResumableRelaySocket does not blame the socket for a frozen page", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  rig.Tick(60'000);  // the page was in the background for a minute
  rig.Tick(16);
  CHECK_FALSE(rig.sock->Reconnecting());
  CHECK(rig.opened.size() == 1);
}

TEST_CASE("ResumableRelaySocket closes when the room is gone", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  rig.Last().state = RelaySocket::State::kClosed;
  rig.Tick(16);
  rig.Tick(16);
  rig.Last().inbox.push_back({relay::kError, 0x01});
  rig.Tick(16);
  CHECK(rig.sock->GetState() == RelaySocket::State::kClosed);
  std::vector<uint8_t> msg;
  REQUIRE(rig.sock->Receive(msg));
  CHECK(msg[0] == relay::kError);
}

TEST_CASE("ResumableRelaySocket keeps retrying while the network is down", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  rig.can_connect = false;
  rig.Last().state = RelaySocket::State::kClosed;
  for (int i = 0; i < 20; ++i) {
    rig.Tick(1000);
  }
  CHECK(rig.sock->Reconnecting());
  CHECK(rig.sock->GetState() == RelaySocket::State::kOpen);

  rig.can_connect = true;
  for (int i = 0; i < 10 && rig.opened.size() == 1; ++i) {
    rig.Tick(1000);
  }
  REQUIRE(rig.opened.size() == 2);
  CHECK(SentType(rig.Last(), relay::kResume));

  // Long after the relay gave up on us, so do we.
  ResumeRig late;
  late.IntoRoom();
  late.can_connect = false;
  late.Last().state = RelaySocket::State::kClosed;
  for (uint64_t t = 0; t <= ResumableRelaySocket::kGiveUpMs + 10'000; t += 1000) {
    late.Tick(1000);
  }
  CHECK(late.sock->GetState() == RelaySocket::State::kClosed);
}

TEST_CASE("ResumableRelaySocket: losing the first socket is final", "[relay][resume]") {
  ResumeRig rig;
  rig.sock->Send({relay::kCreateRoom});
  rig.Last().state = RelaySocket::State::kClosed;
  rig.Tick(16);
  CHECK(rig.sock->GetState() == RelaySocket::State::kClosed);
  CHECK(rig.opened.size() == 1);
}

TEST_CASE("ResumableRelaySocket says when it leaves and tracks the peer", "[relay][resume]") {
  ResumeRig rig;
  rig.IntoRoom();
  rig.Last().inbox.push_back({relay::kPeerAway});
  rig.Tick(16);
  CHECK(rig.sock->PeerAway());
  rig.Last().inbox.push_back({relay::kPeerBack});
  rig.Tick(16);
  CHECK_FALSE(rig.sock->PeerAway());

  std::shared_ptr<ScriptSocket> const kLast = rig.opened.back();
  rig.sock.reset();
  REQUIRE_FALSE(kLast->sent.empty());
  // Sent just before the socket was closed.
  CHECK(SentType(*kLast, relay::kLeave));
}
