#pragma once

#include "state.hpp"

struct Gfx;

// While an online match is on hold because a player's link to the relay is
// down, says so in a box centered at `y` on the play screen.
void DrawNetLinkNotice(Gfx& gfx, int y);

// Wraps the game loop (controller->process() per frame) as an AppState.
// Pushed onto the state stack when the player starts or resumes a game.
// Pops itself when the controller signals the game is over (process() returns false).
struct GamePlayState : AppState {
  void Enter() override;
  void HandleEvent(SDL_Event& ev) override;
  bool Update() override;
  void Draw() override;
  bool WantsMenuFlip() const override { return false; }
};
