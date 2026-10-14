#pragma once

#include "net/relayLink.hpp"
#include "net/session.hpp"
#include "state.hpp"

#include <string>

// "Online" connection flow of the web build: browsers cannot use UDP, so
// instead of signaling + ICE both players connect to the WebSocket relay
// (server/ws.go) on the site the game is served from.
//
// Flow:
// 1. Open a WebSocket to the relay (DefaultRelayUrl)
// 2. Host creates a room and shows its code; client joins by code
// 3. Once paired, hand the socket to a NetTransport and go to NetConnectState
struct RelayConnectState : AppState {
  RelayConnectState(NetSession::Role role, std::string room_code = "");
  ~RelayConnectState() override;

  void Enter() override;
  void HandleEvent(SDL_Event& ev) override;
  bool Update() override;
  void Draw() override;

 private:
  void TransitionToGame();

  NetSession::Role role_;
  std::string roomCode_;
  RelayClient relay_;

  std::string inviteLink_;  // empty on native builds — nothing to share
  bool shared_ = false;
  bool copied_ = false;

  std::string statusLine1_;
  std::string statusLine2_;
  bool cancel_ = false;
};
