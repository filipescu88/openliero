// Touch overlay → key mapping: a held stick direction must behave like a held
// keyboard key (one press, then auto-repeat for menus), and fingers resting near
// a dead-zone or sector boundary must not flicker the key.

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

#include "common.hpp"
#include "controller/controller.hpp"
#include "controller/localController.hpp"
#include "filesystem.hpp"
#include "gfx.hpp"
#include "level.hpp"
#include "math.hpp"
#include "settings.hpp"
#include "touch/touchControls.hpp"
#include "worm.hpp"

namespace {

// Layout for a 640×400 surface (see TouchControls::RecomputeLayout): the move
// stick sits at (92, 308) with an 80 px radius.
constexpr float kW = 640.0F;
constexpr float kH = 400.0F;
constexpr float kBaseX = 92.0F;
constexpr float kBaseY = 308.0F;
constexpr float kR = 80.0F;
constexpr SDL_FingerID kFinger = 7;

struct TouchFixture {
  TouchFixture() {
    gfx.settings = std::make_shared<Settings>();
    gfx.window_w = static_cast<int>(kW);
    gfx.window_h = static_cast<int>(kH);
    gfx.sdl_draw_surface = nullptr;
    gfx.controller.reset();
    gfx.ClearKeys();
    touch.SetEnabled(/*on=*/true);
    touch.SetDrawOverlay(/*on=*/false);
  }
  ~TouchFixture() { gfx.ClearKeys(); }
  TouchFixture(TouchFixture const&) = delete;
  TouchFixture& operator=(TouchFixture const&) = delete;
  TouchFixture(TouchFixture&&) = delete;
  TouchFixture& operator=(TouchFixture&&) = delete;

  // Puts the move-stick finger at (ax, ay) stick units from the stick centre.
  void Stick(float ax, float ay) {
    float const kNx = (kBaseX + (ax * kR)) / kW;
    float const kNy = (kBaseY + (ay * kR)) / kH;
    if (!down) {
      touch.SimulateFinger(kFinger, /*down=*/true, kNx, kNy, gfx);
      down = true;
      return;
    }
    SDL_Event ev{};
    ev.type = SDL_EVENT_FINGER_MOTION;
    ev.tfinger.fingerID = kFinger;
    ev.tfinger.x = kNx;
    ev.tfinger.y = kNy;
    touch.HandleEvent(ev, gfx);
  }

  void Release() {
    touch.SimulateFinger(kFinger, /*down=*/false, kBaseX / kW, kBaseY / kH, gfx);
    down = false;
  }

  // One frame: overlay → keys, then a menu consuming the control like
  // MainMenuState does. Returns whether the menu saw a press this frame.
  bool Frame(int control) {
    touch.Apply(gfx, now_ms);
    now_ms += 16;
    return gfx.TestControlOnce(control);
  }

  int CountPresses(int control, int frames) {
    int n = 0;
    for (int i = 0; i < frames; ++i) {
      n += Frame(control) ? 1 : 0;
    }
    return n;
  }

  TouchControls touch;
  uint64_t now_ms = 1000;
  bool down = false;
};

}  // namespace

TEST_CASE("Held stick direction repeats like a held key") {
  TouchFixture f;
  f.Stick(0.0F, -0.8F);

  // The push itself is one press.
  CHECK(f.Frame(WormSettings::kUp));
  // Nothing more until the repeat delay (~350 ms) passes...
  CHECK(f.CountPresses(WormSettings::kUp, 15) == 0);
  // ...then steady repeats (~90 ms apart → about 10 in one second).
  int const kRepeats = f.CountPresses(WormSettings::kUp, 60);
  CHECK(kRepeats >= 8);
  CHECK(kRepeats <= 12);

  // Releasing stops the repeats and a new push is a press again.
  f.Release();
  CHECK(f.CountPresses(WormSettings::kUp, 60) == 0);
  f.Stick(0.0F, -0.8F);
  CHECK(f.Frame(WormSettings::kUp));
}

TEST_CASE("Finger hovering at the dead-zone edge does not flicker the key") {
  TouchFixture f;
  f.Stick(0.0F, -0.25F);
  CHECK(f.Frame(WormSettings::kUp));

  // Jitter around the old 15% threshold: still held, no new presses.
  for (int i = 0; i < 20; ++i) {
    f.Stick(0.0F, (i % 2 == 0) ? -0.13F : -0.18F);
    CHECK_FALSE(f.Frame(WormSettings::kUp));
  }

  // Back near the centre: released; a new push presses again.
  f.Stick(0.0F, -0.05F);
  CHECK_FALSE(f.Frame(WormSettings::kUp));
  f.Stick(0.0F, -0.25F);
  CHECK(f.Frame(WormSettings::kUp));
}

TEST_CASE("Mostly-vertical push is up only, diagonal is up + side") {
  TouchFixture f;
  // ~17° off vertical: pure up (before, any |x| > 15% also pressed right).
  f.Stick(0.25F, -0.8F);
  f.touch.Apply(gfx, f.now_ms);
  CHECK(gfx.TestControl(WormSettings::kUp));
  CHECK_FALSE(gfx.TestControl(WormSettings::kRight));

  // 45°: both.
  f.Stick(0.6F, -0.6F);
  f.touch.Apply(gfx, f.now_ms);
  CHECK(gfx.TestControl(WormSettings::kUp));
  CHECK(gfx.TestControl(WormSettings::kRight));
}

namespace {

// Button centres for the 640×400 layout (see TouchControls::RecomputeLayout):
// fire r=50 at (578, 338); the arc is 118 px away from it.
constexpr float kJumpX = 460.0F;
constexpr float kJumpY = 338.0F;
constexpr float kRopeX = 494.6F;
constexpr float kRopeY = 254.6F;
constexpr float kDigX = 578.0F;
constexpr float kDigY = 146.0F;
constexpr SDL_FingerID kThumb = 9;

void Touch(TouchControls& t, bool down, float x, float y) {
  t.SimulateFinger(kThumb, down, x / kW, y / kH, gfx);
}

}  // namespace

TEST_CASE("A tap shorter than a frame still reaches the game") {
  TouchFixture f;
  // Finger down and up arrive in the same event poll, before the frame.
  Touch(f.touch, /*down=*/true, kJumpX, kJumpY);
  Touch(f.touch, /*down=*/false, kJumpX, kJumpY);
  f.touch.Apply(gfx, f.now_ms);
  CHECK(gfx.TestControl(WormSettings::kJump));
  f.touch.Apply(gfx, f.now_ms + 16);
  CHECK_FALSE(gfx.TestControl(WormSettings::kJump));
}

TEST_CASE("LINA presses change and jump together, briefly") {
  TouchFixture f;
  Touch(f.touch, /*down=*/true, kRopeX, kRopeY);
  f.touch.Apply(gfx, f.now_ms);
  CHECK(gfx.TestControl(WormSettings::kChange));
  CHECK(gfx.TestControl(WormSettings::kJump));
  // Released after a few frames even while the finger stays on the button, so
  // a held change key does not block walking.
  for (int i = 0; i < 4; ++i) {
    f.touch.Apply(gfx, f.now_ms);
  }
  CHECK_FALSE(gfx.TestControl(WormSettings::kChange));
  CHECK_FALSE(gfx.TestControl(WormSettings::kJump));
  Touch(f.touch, /*down=*/false, kRopeX, kRopeY);
}

TEST_CASE("The right side has no stick: no movement and no auto-fire") {
  TouchFixture f;
  // Where the old aim stick was, dragged hard to the side and up.
  Touch(f.touch, /*down=*/true, 446.0F, 308.0F);
  SDL_Event ev{};
  ev.type = SDL_EVENT_FINGER_MOTION;
  ev.tfinger.fingerID = kThumb;
  ev.tfinger.x = 380.0F / kW;
  ev.tfinger.y = 240.0F / kH;
  f.touch.HandleEvent(ev, gfx);
  f.touch.Apply(gfx, f.now_ms);
  CHECK_FALSE(gfx.TestControl(WormSettings::kLeft));
  CHECK_FALSE(gfx.TestControl(WormSettings::kRight));
  CHECK_FALSE(gfx.TestControl(WormSettings::kUp));
  CHECK_FALSE(gfx.TestControl(WormSettings::kFire));
}

TEST_CASE("KOP digs with left+right when no dig key is bound") {
  TouchFixture f;
  REQUIRE(gfx.settings->worm_settings[0]->controls_ex[WormSettings::kDig] == 0);
  Touch(f.touch, /*down=*/true, kDigX, kDigY);
  f.touch.Apply(gfx, f.now_ms);
  CHECK(gfx.TestControl(WormSettings::kLeft));
  CHECK(gfx.TestControl(WormSettings::kRight));
  Touch(f.touch, /*down=*/false, kDigX, kDigY);
  f.touch.Apply(gfx, f.now_ms);
  CHECK_FALSE(gfx.TestControl(WormSettings::kLeft));
  CHECK_FALSE(gfx.TestControl(WormSettings::kRight));
}

TEST_CASE("Buttons drive a real match - LINA throws the rope and SKOK releases it") {
  TouchFixture f;
  PrecomputeTables();
  auto common = std::make_shared<Common>();
  common->load(FsNode("data") / "TC" / "openliero");
  gfx.settings->record_replays = false;
  gfx.settings->random_level = true;
  gfx.settings->loading_time = 0;

  auto lc = std::make_unique<LocalController>(common, gfx.settings);
  auto* ctrl = lc.get();
  Level level(*common);
  level.GenerateFromSettings(*common, *gfx.settings, ctrl->game.rand);
  ctrl->SwapLevel(level);
  ctrl->ChangeState(kStateWeaponSelection);
  ctrl->ChangeState(kStateGame);
  gfx.controller = std::move(lc);

  Worm const& worm = *ctrl->game.worms[0];
  for (int i = 0; i < 3000 && !worm.visible; ++i) {
    ctrl->Process();
  }
  REQUIRE(worm.visible);
  REQUIRE_FALSE(worm.ninjarope.out);

  // One frame of the overlay, then one tick of the game — like Gfx::RunOneFrame.
  auto const kFrame = [&] {
    f.touch.Apply(gfx, f.now_ms);
    f.now_ms += 16;
    ctrl->Process();
  };

  Touch(f.touch, /*down=*/true, kRopeX, kRopeY);
  kFrame();
  Touch(f.touch, /*down=*/false, kRopeX, kRopeY);
  for (int i = 0; i < 5; ++i) {
    kFrame();
  }
  CHECK(worm.ninjarope.out);
  // The change key is not left held (it would block walking).
  CHECK_FALSE(worm.Pressed(Worm::kChange));

  // A quick tap on SKOK (down+up before the frame) removes the rope.
  Touch(f.touch, /*down=*/true, kJumpX, kJumpY);
  Touch(f.touch, /*down=*/false, kJumpX, kJumpY);
  kFrame();
  kFrame();
  CHECK_FALSE(worm.ninjarope.out);

  gfx.controller.reset();
}
