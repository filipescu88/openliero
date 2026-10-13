// Sterowanie dotykowe — implementacja. Patrz touchControls.hpp po opis układu
// i zasadę „dotyk wciska te same klawisze DOS, co klawiatura”.

#include "touch/touchControls.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "controller/controller.hpp"
#include "game.hpp"
#include "gfx.hpp"
#include "settings.hpp"
#include "worm.hpp"

namespace {

// Kolory nakładki (ARGB, niezależne od palety gry) — spójne z makietą F3.
constexpr uint32_t kZoneFill = 0x18FFFFFF;
constexpr uint32_t kZoneRing = 0x70FFFFFF;
constexpr uint32_t kZoneRingHot = 0xC0FFD166;
constexpr uint32_t kKnob = 0xCCFFC64D;
constexpr uint32_t kBtnFire = 0x77E5484D;
constexpr uint32_t kBtnJump = 0x7756D364;
constexpr uint32_t kBtnRope = 0x77FFB300;
constexpr uint32_t kBtnChange = 0x775B8DEF;

// Martwa strefa drążka jest okrągła i z histerezą: kierunek „łapie” dopiero po
// wyjściu poza kStickEngage, a puszcza dopiero po powrocie poniżej kStickRelease.
// Pojedynczy próg sprawiał, że palec trzymany przy granicy strefy na zmianę
// wciskał i puszczał klawisz.
constexpr float kStickEngage = 0.20F;
constexpr float kStickRelease = 0.12F;
// Sektory kierunków jak na krzyżaku (8 kierunków): kierunek jest wciśnięty, gdy
// wychylenie leży w stożku ±67,5° wokół jego osi — czyli „czysta” góra to ±22,5°,
// a skosy łączą dwa kierunki. Trzymany kierunek puszcza dopiero poza ±75°
// (histereza na granicy sektorów). Wartości to cosinusy tych kątów.
constexpr float kConeEnter = 0.3827F;     // cos(67,5°)
constexpr float kConeHold = 0.2588F;      // cos(75°)
constexpr float kAutoFire = 0.55F;        // wychylenie prawego drążka = auto-ogień
constexpr float kTurnThresh = 0.60F;      // wychylenie X prawego drążka = obrót
constexpr float kStickZoneScale = 1.25F;  // strefa chwytu drążka (× promień)
// Autopowtarzanie trzymanego kierunku w menu — jak klawiatura (SDL key repeat):
// pierwszy krok od razu, kolejne po opóźnieniu, potem w stałym rytmie.
constexpr uint64_t kRepeatDelayMs = 350;
constexpr uint64_t kRepeatIntervalMs = 90;

// Indeksy kierunków w tablicach drążka.
enum StickDir { kDirLeft = 0, kDirRight, kDirUp, kDirDown, kDirCount };

// Wychylenie drążka (osie -1..1) → wciśnięte kierunki, z histerezą względem `prev`.
std::array<bool, kDirCount> StickDirs(float ax, float ay, std::array<bool, kDirCount> const& prev) {
  std::array<bool, kDirCount> out{};
  float const kR = std::sqrt((ax * ax) + (ay * ay));
  bool const kWasHeld = std::ranges::any_of(prev, [](bool b) { return b; });
  if (kR < (kWasHeld ? kStickRelease : kStickEngage)) {
    return out;
  }
  // Rzut kierunku wychylenia na oś danego kierunku (cos kąta między nimi).
  std::array<float, kDirCount> const kCos = {-ax / kR, ax / kR, -ay / kR, ay / kR};
  for (std::size_t d = 0; d < kDirCount; ++d) {
    out.at(d) = kCos.at(d) > (prev.at(d) ? kConeHold : kConeEnter);
  }
  return out;
}

inline void BlendPixel(SDL_Surface& s, int x, int y, uint32_t argb, float a) {
  if (x < 0 || y < 0 || x >= s.w || y >= s.h || a <= 0.0F) {
    return;
  }
  uint32_t* const kRow =
      static_cast<uint32_t*>(s.pixels) + (static_cast<std::size_t>(y) * (s.pitch / 4U));
  uint32_t const kSrc = argb;
  uint32_t const kDst = kRow[x];
  auto const kMix = [&](int shift) {
    float const kS = static_cast<float>((kSrc >> shift) & 0xFFU) / 255.0F;
    float const kD = static_cast<float>((kDst >> shift) & 0xFFU) / 255.0F;
    return static_cast<uint32_t>(std::lround((kS * a + kD * (1.0F - a)) * 255.0F)) & 0xFFU;
  };
  kRow[x] = 0xFF000000U | (kMix(16) << 16) | (kMix(8) << 8) | kMix(0);
}

void DrawDisc(SDL_Surface& s, float cx, float cy, float r, uint32_t fill, float fill_a,
              uint32_t ring, float ring_a) {
  int const kX0 = static_cast<int>(std::floor(cx - r));
  int const kX1 = static_cast<int>(std::ceil(cx + r));
  int const kY0 = static_cast<int>(std::floor(cy - r));
  int const kY1 = static_cast<int>(std::ceil(cy + r));
  float const kRingW = std::max(1.5F, r * 0.09F);
  // Krycie bierzemy z kanału alfa koloru; parametr `*_a` to dodatkowy mnożnik
  // (np. 0.6 gdy przycisk jest wciśnięty). Bez tego półprzezroczyste kolory
  // nakładki malowałyby się na biało.
  float const kFillA = fill_a * (static_cast<float>((fill >> 24) & 0xFFU) / 255.0F);
  float const kRingA = ring_a * (static_cast<float>((ring >> 24) & 0xFFU) / 255.0F);
  for (int y = kY0; y <= kY1; ++y) {
    for (int x = kX0; x <= kX1; ++x) {
      float const kDx = (static_cast<float>(x) + 0.5F) - cx;
      float const kDy = (static_cast<float>(y) + 0.5F) - cy;
      float const kDist = std::sqrt(kDx * kDx + kDy * kDy);
      if (kDist > r) {
        continue;
      }
      if (kDist > r - kRingW) {
        BlendPixel(s, x, y, ring, kRingA);
      } else {
        BlendPixel(s, x, y, fill, kFillA);
      }
    }
  }
}

inline bool InDisc(float px, float py, float cx, float cy, float r) {
  float const kDx = px - cx;
  float const kDy = py - cy;
  return (kDx * kDx + kDy * kDy) <= r * r;
}

inline float ClampUnit(float v) { return std::clamp(v, -1.0F, 1.0F); }

}  // namespace

void TouchControls::RecomputeLayout(Gfx& gfx) {
  int w = gfx.window_w;
  int h = gfx.window_h;
  if ((gfx.sdl_draw_surface != nullptr) && gfx.sdl_draw_surface->w > 0 &&
      (gfx.sdl_draw_surface->h > 0)) {
    w = gfx.sdl_draw_surface->w;
    h = gfx.sdl_draw_surface->h;
  }
  if (w <= 0 || h <= 0) {
    return;
  }
  if (layout_valid_ && layout_w_ == w && layout_h_ == h) {
    return;
  }
  layout_w_ = w;
  layout_h_ = h;
  layout_valid_ = true;

  float const kMin = static_cast<float>(std::min(w, h));
  float const kMargin = std::max(6.0F, kMin * 0.03F);
  float const kStickR = std::clamp(kMin * 0.20F, 34.0F, 110.0F);
  float const kBtnR = std::clamp(kMin * 0.10F, 20.0F, 46.0F);
  float const kStepY = kBtnR * 1.85F;          // odstęp między krążkami w kolumnie
  float const kGap = kBtnR * 0.55F;            // odstęp drążka od kolumny przycisków

  // Lewy drążek: ruch + celowanie (odpowiednik strzałek).
  sticks_[0].base_x = kMargin + kStickR;
  sticks_[0].base_y = static_cast<float>(h) - kMargin - kStickR;
  // Prawy drążek: celowanie + auto-ogień — na lewo od kolumny przycisków.
  sticks_[1].base_x = static_cast<float>(w) - kMargin - (kBtnR * 2.0F) - kGap - kStickR;
  sticks_[1].base_y = static_cast<float>(h) - kMargin - kStickR;

  // Kolumna przycisków przy prawej krawędzi: OGIEN (dół) → SKOK → LINA → BRON → MENU.
  float const kColX = static_cast<float>(w) - kMargin - kBtnR;
  float y = static_cast<float>(h) - kMargin - kBtnR;
  buttons_[kBtnFire] = {kColX, y, kBtnR, false, 0};
  y -= kStepY;
  buttons_[kBtnJump] = {kColX, y, kBtnR * 0.85F, false, 0};
  y -= kStepY;
  buttons_[kBtnRope] = {kColX, y, kBtnR * 0.80F, false, 0};
  y -= kStepY;
  buttons_[kBtnChange] = {kColX, y, kBtnR * 0.80F, false, 0};
  y -= kStepY;
  buttons_[kBtnMenu] = {kColX, y, kBtnR * 0.70F, false, 0};

  // Cała kolumna (od góry BRONIA do dołu ekranu) to strefa przycisków. Dotyk
  // tutaj — nawet w przerwę albo w etykietę — trafia w najbliższy przycisk.
  // Wcześniej niecelny dotyk po prawej stronie kończył się „tapnięciem”, czyli
  // strzałem, więc wszystkie przyciski wyglądały jak OGIEN.
  zone_x0_ = kColX - (kBtnR * 1.55F);
  zone_x1_ = static_cast<float>(w);
  zone_y0_ = buttons_[kBtnMenu].y - (kBtnR * 1.10F);
  zone_y1_ = static_cast<float>(h);
}

void TouchControls::SimulateFinger(SDL_FingerID id, bool down, float nx, float ny, Gfx& gfx) {
  SDL_Event ev{};
  ev.type = down ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP;
  ev.tfinger.fingerID = id;
  ev.tfinger.x = nx;
  ev.tfinger.y = ny;
  HandleEvent(ev, gfx);
}

void TouchControls::HandleEvent(SDL_Event const& ev, Gfx& gfx) {
  switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
      break;
    default:
      return;
  }
  had_finger_ = true;
  if (!enabled_) {
    return;
  }
  RecomputeLayout(gfx);
  if (layout_w_ <= 0 || layout_h_ <= 0) {
    return;
  }

  // SDL podaje pozycję dotyku znormalizowaną (0..1) względem okna.
  float const kX = ev.tfinger.x * static_cast<float>(layout_w_);
  float const kY = ev.tfinger.y * static_cast<float>(layout_h_);
  SDL_FingerID const kId = ev.tfinger.fingerID;

  if (ev.type == SDL_EVENT_FINGER_DOWN) {
    // 1) przyciski — tolerancja 1.25× promienia (palec jest większy niż kropka)
    int best = -1;
    float best_r = 1e9F;
    for (int i = 0; i < kBtnCount; ++i) {
      Button const& b = buttons_[i];
      if (InDisc(kX, kY, b.x, b.y, b.r * 1.25F) && b.r < best_r) {
        best = i;
        best_r = b.r;
      }
    }
    if (best >= 0) {
      buttons_[best].down = true;
      buttons_[best].finger = kId;
      if (best == kBtnMenu) {
        // MENU = ESC, tak jak START na padzie (Gfx::DispatchGamepadInput):
        // kontroler (LocalController::OnKey) wygasza i wraca do menu.
        PulseKey(gfx, kDkEscape, 2);
      }
      return;
    }

    // 2) drążki — „chwyt” w promieniu strefy. Środek drążka jest STAŁY (środek
    //    strefy z layoutu) — dotknięcie nie przesuwa drążka pod palec.
    float const kStickR =
        std::clamp(static_cast<float>(std::min(layout_w_, layout_h_)) * 0.20F, 34.0F, 110.0F);
    for (int i = 0; i < 2; ++i) {
      Stick& s = sticks_[i];
      if (!s.active && InDisc(kX, kY, s.base_x, s.base_y, kStickR * kStickZoneScale)) {
        s.active = true;
        s.finger = kId;
        s.x = kX;
        s.y = kY;
        return;
      }
    }

    // 3) gdziekolwiek w kolumnie przycisków → NAJBLIŻSZY przycisk (bez tapnięcia:
    //    tapnięcie w grze to strzał, a tu chodziło właśnie o skok/linę/broń).
    if ((kX >= zone_x0_) && (kX <= zone_x1_) && (kY >= zone_y0_) && (kY <= zone_y1_)) {
      int nearest = 0;
      float nearest_d = 1e9F;
      for (int i = 0; i < kBtnCount; ++i) {
        float const kDx = kX - buttons_[i].x;
        float const kDy = kY - buttons_[i].y;
        float const kD = std::sqrt((kDx * kDx) + (kDy * kDy));
        if (kD < nearest_d) {
          nearest_d = kD;
          nearest = i;
        }
      }
      buttons_[nearest].down = true;
      buttons_[nearest].finger = kId;
      if (nearest == kBtnMenu) {
        PulseKey(gfx, kDkEscape, 2);
      }
      return;
    }

    // 4) poza strefami → potencjalne tapnięcie (potwierdzenie)
    tap_pending_ = true;
    tap_finger_ = kId;
    tap_x_ = kX;
    tap_y_ = kY;
    tap_frames_ = 0;
    return;
  }

  bool const kReleased = (ev.type != SDL_EVENT_FINGER_MOTION);
  for (Stick& s : sticks_) {
    if (s.active && s.finger == kId) {
      s.x = kX;
      s.y = kY;
      if (kReleased) {
        s.active = false;
        s.finger = 0;
      }
      return;
    }
  }
  for (Button& b : buttons_) {
    if (b.down && b.finger == kId) {
      if (kReleased) {
        b.down = false;
        b.finger = 0;
      }
      return;
    }
  }
  if (tap_pending_ && (tap_finger_ == kId)) {
    if (ev.type == SDL_EVENT_FINGER_MOTION) {
      ++tap_frames_;
      float const kDx = kX - tap_x_;
      float const kDy = kY - tap_y_;
      float const kLimit =
          static_cast<float>(std::min(layout_w_, layout_h_)) * 0.07F;  // ruch = gest, nie tap
      if (std::sqrt(kDx * kDx + kDy * kDy) > kLimit) {
        tap_pending_ = false;
      }
    } else {
      // Krótki dotyk bez ruchu poza strefami = potwierdzenie. Menu i ekran broni
      // reagują na ENTER **oraz** na klawisz ognia gracza (TestControlOnce(kFire)) —
      // dlatego pulsujemy oba, żeby dało się potwierdzać palcem w każdym ekranie.
      if (ev.type == SDL_EVENT_FINGER_UP && tap_frames_ <= 30) {
        PulseKey(gfx, SDLToDOSKey(SDL_SCANCODE_RETURN), 2);
        PulseKey(gfx, ActionKeyCode(gfx, WormSettings::kFire), 2);
      }
      tap_pending_ = false;
    }
  }
}

void TouchControls::PulseKey(Gfx& gfx, uint32_t code, int frames) {
  if (code == 0 || code >= kMaxDosKey) {
    return;
  }
  std::fprintf(stderr, "[touch] pulse key=%u frames=%d\n", code, frames);
  pulse_frames_[code] = std::max(pulse_frames_[code], frames);
  // Impuls działa jak jedno naciśnięcie klawisza: ustawiamy klawisz TERAZ (ta klatka
  // jest widoczna dla TestKeyOnce/TestControlOnce w Update), a nie odtwarzamy go przez
  // kolejne klatki — inaczej jedno wychylenie drążka dawałoby kilka kroków w menu.
  gfx.dos_keys[code] = true;
  if (!owned_dos_[code]) {
    // Klawiatura w ProcessEvent woła też controller->OnKey — ekran wyboru broni i
    // inne stany sterowane kontrolerem (nie samym gfx.dos_keys) potrzebują tego
    // samego powiadomienia, inaczej dotyk nie ruszy ich wcale.
    if (gfx.controller) {
      gfx.controller->OnKey(code, /*state=*/true);
    }
  }
  owned_dos_[code] = true;
}

uint32_t TouchControls::ActionKeyCode(Gfx& gfx, int control) const {
  if (gfx.settings == nullptr) {
    return 0;
  }
  auto const& kWs = gfx.settings->worm_settings[0];
  if (kWs == nullptr) {
    return 0;
  }
  return Settings::kExtensions ? kWs->controls_ex[control] : kWs->controls[control];
}

void TouchControls::SetActionKey(Gfx& gfx, int control, bool down) {
  uint32_t const kCode = ActionKeyCode(gfx, control);
  if (kCode == 0) {
    return;
  }
  if (kCode < kMaxDosKey) {
    if (down) {
      // Stan trzymamy po SWOJEJ stronie: gra zeruje `gfx.dos_keys` co klatkę
      // (ClearKeys), więc „czy już wciśnięty” sprawdzamy w owned_dos_, a nie w gfx.
      // Powiadamiamy kontroler tylko o ZMIANIE — inaczej każde odtworzenie klawisza
      // po ClearKeys dawałoby nową krawędź i menu przewijałoby się co klatkę.
      //
      // UWAGA: klawisza NIE odtwarzamy w gfx.dos_keys co klatkę. Menu czytają go
      // przez TestKeyOnce/TestControlOnce (destrukcyjnie — „zużywają” naciśnięcie),
      // więc odtwarzanie = nowe naciśnięcie w każdej klatce = menu leci jak szalone.
      // Ruch/akcje w grze nie cierpią: kontroler dostaje zdarzenia OnKey i trzyma
      // stan robaka (LocalController::OnKey → worm->SetControlState).
      if (!owned_dos_[kCode]) {
        std::fprintf(stderr, "[touch] control=%d key=%u DOWN\n", control, kCode);
        owned_dos_[kCode] = true;
        gfx.dos_keys[kCode] = true;
        if (gfx.controller) {
          gfx.controller->OnKey(kCode, /*state=*/true);
        }
      }
      wanted_dos_[kCode] = true;
    } else if (owned_dos_[kCode] && (pulse_frames_[kCode] == 0)) {
      std::fprintf(stderr, "[touch] control=%d key=%u up\n", control, kCode);
      gfx.dos_keys[kCode] = false;
      owned_dos_[kCode] = false;
      if (gfx.controller) {
        gfx.controller->OnKey(kCode, /*state=*/false);
      }
    }
  } else if (down) {
    if (owned_ex_.count(kCode) == 0U) {
      std::fprintf(stderr, "[touch] control=%d exkey=%u DOWN\n", control, kCode);
      if (gfx.controller) {
        gfx.controller->OnKey(kCode, /*state=*/true);
      }
    }
    gfx.ex_keys[kCode] = true;
    owned_ex_.insert(kCode);
    wanted_ex_.insert(kCode);
  } else if (owned_ex_.erase(kCode) > 0U) {
    gfx.ex_keys[kCode] = false;
    if (gfx.controller) {
      gfx.controller->OnKey(kCode, /*state=*/false);
    }
  }
}

bool TouchControls::KeyHeldByTouch(uint32_t code) const {
  if (code >= kMaxDosKey) {
    return owned_ex_.count(code) > 0U;
  }
  return owned_dos_[code];
}

void TouchControls::Apply(Gfx& gfx) { Apply(gfx, SDL_GetTicks()); }

void TouchControls::Apply(Gfx& gfx, uint64_t now_ms) {
  if (!enabled_) {
    return;
  }
  RecomputeLayout(gfx);
  if (layout_w_ <= 0 || layout_h_ <= 0) {
    return;
  }

  // 1) impulsy (tap = ENTER+kFire, impulsy kierunkowe dla menu): klawisz został
  //    ustawiony w PulseKey i NIE jest odtwarzany — tu tylko odliczamy czas trwania
  //    i zwalniamy (żeby nie zostawić klawisza wciśniętego ani w kontrolerze).
  for (uint32_t k = 0; k < kMaxDosKey; ++k) {
    if (pulse_frames_[k] > 0) {
      wanted_dos_[k] = true;
      if (--pulse_frames_[k] == 0) {
        if (gfx.controller) {
          gfx.controller->OnKey(k, /*state=*/false);
        }
        owned_dos_[k] = false;
        gfx.dos_keys[k] = false;
        wanted_dos_[k] = false;
      }
    }
  }

  // 2) drążki → kierunki (ruch + celowanie) i auto-ogień
  float const kStickR =
      std::clamp(static_cast<float>(std::min(layout_w_, layout_h_)) * 0.20F, 34.0F, 110.0F);
  auto const kAxis = [&](Stick const& s) {
    return std::pair<float, float>{ClampUnit((s.x - s.base_x) / kStickR),
                                   ClampUnit((s.y - s.base_y) / kStickR)};
  };
  Stick const& kMove = sticks_[0];
  Stick const& kAim = sticks_[1];
  auto const kMoveAx = kAxis(kMove);
  auto const kAimAx = kAxis(kAim);

  // Lewy drążek: 8 kierunków (ruch + celowanie). Prawy: góra/dół z tych samych
  // sektorów, obrót dopiero przy mocnym wychyleniu w bok.
  std::array<bool, kDirCount> move_dirs{};
  if (kMove.active) {
    move_dirs = StickDirs(kMoveAx.first, kMoveAx.second, stick_dirs_[0]);
  }
  std::array<bool, kDirCount> aim_dirs{};
  if (kAim.active) {
    aim_dirs = StickDirs(kAimAx.first, kAimAx.second, stick_dirs_[1]);
    aim_dirs[kDirLeft] = kAimAx.first < -kTurnThresh;
    aim_dirs[kDirRight] = kAimAx.first > kTurnThresh;
  }
  stick_dirs_[0] = move_dirs;
  stick_dirs_[1] = aim_dirs;

  std::array<bool, kDirCount> dirs{};
  for (std::size_t d = 0; d < kDirCount; ++d) {
    dirs.at(d) = move_dirs.at(d) || aim_dirs.at(d);
  }
  bool const kAutoFire = kAim.active && ((std::fabs(kAimAx.first) > kAutoFire) ||
                                         (std::fabs(kAimAx.second) > kAutoFire));

  constexpr std::array<int, kDirCount> kDirControls = {WormSettings::kLeft, WormSettings::kRight,
                                                       WormSettings::kUp, WormSettings::kDown};
  for (std::size_t d = 0; d < kDirCount; ++d) {
    SetActionKey(gfx, kDirControls.at(d), dirs.at(d));
  }
  SetActionKey(gfx, WormSettings::kFire, buttons_[kBtnFire].down || kAutoFire);
  SetActionKey(gfx, WormSettings::kJump, buttons_[kBtnJump].down);
  SetActionKey(gfx, WormSettings::kDig, buttons_[kBtnRope].down);
  SetActionKey(gfx, WormSettings::kChange, buttons_[kBtnChange].down);

  // 3) autopowtarzanie kierunków w menu. Menu czytają strzałki jednorazowo
  //    (TestControlOnce), więc trzymany drążek dawał tylko JEDEN krok — trzeba było
  //    wracać do środka po każdy następny. Klawiatura ma tu systemowe powtarzanie
  //    (zdarzenia `repeat` ponownie ustawiają gfx.dos_keys, bez OnKey) — robimy to
  //    samo. Kontrolera nie powiadamiamy: robak trzyma stan z pierwszego OnKey,
  //    a ekran wyboru broni ma własne powtarzanie (LocalController::Process).
  for (std::size_t d = 0; d < kDirCount; ++d) {
    uint64_t& repeat_at = repeat_at_ms_.at(d);
    if (!dirs.at(d)) {
      repeat_at = 0;
      continue;
    }
    if (repeat_at == 0) {
      repeat_at = now_ms + kRepeatDelayMs;  // pierwszy krok dało samo wciśnięcie
      continue;
    }
    if (now_ms < repeat_at) {
      continue;
    }
    repeat_at = now_ms + kRepeatIntervalMs;
    uint32_t const kCode = ActionKeyCode(gfx, kDirControls.at(d));
    if (kCode == 0 || !KeyHeldByTouch(kCode)) {
      continue;
    }
    if (kCode < kMaxDosKey) {
      gfx.dos_keys[kCode] = true;
    } else {
      gfx.ex_keys[kCode] = true;
    }
  }

  // 4) zwolnij to, co nakładka trzymała, a czego już nie chce (pulsy i „chciane” zostają)
  for (uint32_t k = 0; k < kMaxDosKey; ++k) {
    if (owned_dos_[k] && !wanted_dos_[k] && (pulse_frames_[k] == 0)) {
      gfx.dos_keys[k] = false;
      owned_dos_[k] = false;
    }
    wanted_dos_[k] = false;  // na następną klatkę liczymy od zera
  }
  for (auto it = owned_ex_.begin(); it != owned_ex_.end();) {
    if (wanted_ex_.count(*it) == 0U) {
      gfx.ex_keys[*it] = false;
      it = owned_ex_.erase(it);
    } else {
      ++it;
    }
  }
  wanted_ex_.clear();
}

void TouchControls::DrawLabel(SDL_Surface& surface, Gfx& gfx, std::string const& text, float cx,
                              float cy) {
  if (gfx.common == nullptr) {
    return;
  }
  int height = 0;
  int const kW = gfx.common->font.GetDims(text, &height);
  if (kW <= 0 || height <= 0) {
    return;
  }
  Bitmap scratch;
  scratch.Alloc(kW, height);
  scratch.pal32 = gfx.play_renderer.pal32;  // te same kolory co w grze
  std::fill_n(scratch.pixels, static_cast<std::size_t>(scratch.pitch) * scratch.h, 0U);
  // 7 = biały w palecie gry; tekst bez cienia, żeby zmieścił się w przycisku
  gfx.common->font.DrawString(scratch, text, 0, 0, 7);

  int const kX0 = static_cast<int>(std::lround(cx - (static_cast<float>(kW) / 2.0F)));
  int const kY0 = static_cast<int>(std::lround(cy - (static_cast<float>(height) / 2.0F)));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < kW; ++x) {
      uint32_t const kPixel = scratch.GetPixel(x, y);
      if ((kPixel & 0x00FFFFFFU) == 0U) {
        continue;
      }
      // Rysujemy na biało, a nie paletą gry: ekrany (np. wybór broni) mają własne,
      // rotowane palety, przez co napisy bywały niewidoczne.
      BlendPixel(surface, kX0 + x, kY0 + y, 0xFFFFFFFFU, 0.85F);
    }
  }
}

void TouchControls::DrawOverlay(SDL_Surface& surface, Gfx& gfx) {
  if (!Enabled() || !draw_overlay_) {
    return;
  }
  RecomputeLayout(gfx);
  if (layout_w_ <= 0 || layout_h_ <= 0) {
    return;
  }
  float const kStickR =
      std::clamp(static_cast<float>(std::min(layout_w_, layout_h_)) * 0.20F, 34.0F, 110.0F);

  // Drążki: podstawa + gałka (gałka idzie za palcem, przycięta do promienia).
  for (int i = 0; i < 2; ++i) {
    Stick const& s = sticks_[i];
    float const kCx = s.active ? s.base_x : s.base_x;
    float const kCy = s.active ? s.base_y : s.base_y;
    DrawDisc(surface, kCx, kCy, kStickR, kZoneFill, 1.0F, s.active ? kZoneRingHot : kZoneRing,
             s.active ? 0.85F : 0.45F);
    if (s.active) {
      float kDx = s.x - s.base_x;
      float kDy = s.y - s.base_y;
      float const kLen = std::sqrt(kDx * kDx + kDy * kDy);
      if (kLen > kStickR) {
        kDx = kDx / kLen * kStickR;
        kDy = kDy / kLen * kStickR;
      }
      DrawDisc(surface, kCx + kDx, kCy + kDy, kStickR * 0.42F, kKnob, 1.0F, 0x33FFFFFF, 0.5F);
    }
  }

  // Przyciski akcji.
  struct BtnStyle {
    uint32_t color;
    char const* label;
  };
  BtnStyle const kStyles[kBtnCount] = {{.color = kBtnFire, .label = "OGIEN"},
                                       {.color = kBtnJump, .label = "SKOK"},
                                       {.color = kBtnRope, .label = "LINA"},
                                       {.color = kBtnChange, .label = "BRON"},
                                       {.color = 0xFF444444, .label = "MENU"}};
  for (int i = 0; i < kBtnCount; ++i) {
    Button const& b = buttons_[i];
    DrawDisc(surface, b.x, b.y, b.r, kStyles[i].color, b.down ? 1.0F : 0.75F, 0xAAFFFFFF,
             b.down ? 1.0F : 0.55F);
    DrawLabel(surface, gfx, kStyles[i].label, b.x, b.y);
  }

}
