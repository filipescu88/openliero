#include "relayConnectState.hpp"

#include "common.hpp"
#include "gfx.hpp"
#include "keys.hpp"
#include "netConnectState.hpp"
#include "text.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

RelayConnectState::RelayConnectState(NetSession::Role role, std::string room_code)
    : role_(role), roomCode_(std::move(room_code)) {}

void RelayConnectState::Enter() {
  std::string const kUrl = DefaultRelayUrl();
  std::fprintf(stderr, "[relay] enter: role=%s room='%s' url=%s\n",
               role_ == NetSession::kHost ? "Host" : "Client", roomCode_.c_str(), kUrl.c_str());

  statusLine1_ = (role_ == NetSession::kHost) ? "CREATING ONLINE ROOM..."
                                              : "JOINING ROOM " + roomCode_ + "...";
  statusLine2_ = "CONNECTING TO SERVER...";

  relay_.Start(kUrl.empty() ? nullptr : OpenBrowserRelaySocket(kUrl),
               role_ == NetSession::kHost ? std::string() : roomCode_);
  if (relay_.GetState() == RelayClient::State::kFailed) {
    statusLine2_ = relay_.Error();
    cancel_ = true;
  }
}

void RelayConnectState::HandleEvent(SDL_Event& ev) { gfx->ProcessEvent(ev); }

bool RelayConnectState::Update() {
  if (cancel_) {
    return !(gfx->TestSdlKeyOnce(SDL_SCANCODE_ESCAPE) || gfx->TestSdlKeyOnce(SDL_SCANCODE_RETURN));
  }

  if (gfx->TestSdlKeyOnce(SDL_SCANCODE_ESCAPE)) {
    gfx->ClearKeys();
    return false;  // the socket closes with relay_, which tells the server
  }

  relay_.Poll();

  switch (relay_.GetState()) {
    case RelayClient::State::kWaitingForPeer:
      if (roomCode_ != relay_.RoomCode()) {
        std::fprintf(stderr, "[relay] room %s created, waiting for peer\n",
                     relay_.RoomCode().c_str());
      }
      roomCode_ = relay_.RoomCode();
      statusLine1_ = "ROOM CODE: " + roomCode_;
      statusLine2_ = "WAITING FOR PEER...";
      break;
    case RelayClient::State::kPaired:
      std::fprintf(stderr, "[relay] paired in room %s\n", relay_.RoomCode().c_str());
      statusLine2_ = "CONNECTED! STARTING GAME...";
      TransitionToGame();
      break;
    case RelayClient::State::kFailed:
      std::fprintf(stderr, "[relay] failed: %s\n", relay_.Error().c_str());
      statusLine2_ = relay_.Error();
      cancel_ = true;
      break;
    default:
      break;
  }
  return true;
}

void RelayConnectState::TransitionToGame() {
  NetTransport transport;
  if (!transport.UseRelaySocket(relay_.ReleaseSocket())) {
    statusLine2_ = "RELAY SETUP FAILED";
    cancel_ = true;
    return;
  }
  // Empty peer address: the transport is already linked to the other player.
  gfx->state_stack.ScheduleReplaceTop(
      std::make_unique<NetConnectState>(role_, std::move(transport)));
}

void RelayConnectState::Draw() {
  Common& common = *gfx->common;
  Font& font = common.font;

  gfx->play_renderer.pal =
      gfx->play_renderer.mode == ColorMode::kModern ? common.modernpal : common.exepal;
  Fill(gfx->play_renderer.bmp, 0);

  int const kCx = 160;
  int const kCy = 60;

  int const kW1 = font.GetDims(statusLine1_);
  font.DrawString(gfx->play_renderer.bmp, statusLine1_, kCx - kW1 / 2, kCy, 50);

  int const kW2 = font.GetDims(statusLine2_);
  font.DrawString(gfx->play_renderer.bmp, statusLine2_, kCx - kW2 / 2, kCy + 14, 7);

  std::string const kEsc = cancel_ ? "PRESS ESC TO RETURN" : "PRESS ESC TO CANCEL";
  int const kWe = font.GetDims(kEsc);
  font.DrawString(gfx->play_renderer.bmp, kEsc, kCx - kWe / 2, 170, 7);
}
