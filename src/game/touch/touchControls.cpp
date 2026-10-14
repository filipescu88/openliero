// Sterowanie dotykowe — implementacja. Patrz touchControls.hpp po opis układu
// i zasadę „dotyk wciska te same klawisze DOS, co klawiatura”.

#include "touch/touchControls.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include "controller/controller.hpp"
#include "game.hpp"
#include "gfx.hpp"
#include "settings.hpp"
#include "worm.hpp"

namespace {

// Kolory nakładki (ARGB, niezależne od palety gry) — spójne z makietą F3.
// Prefiks kCol, nie kBtn: w metodach TouchControls nazwa kBtnFire oznacza indeks
// przycisku z enuma klasy (0), więc kolory o tych nazwach wychodziły przezroczyste.
constexpr uint32_t kZoneFill = 0x18FFFFFF;
constexpr uint32_t kZoneRing = 0x70FFFFFF;
constexpr uint32_t kZoneRingHot = 0xC0FFD166;
constexpr uint32_t kKnob = 0xCCFFC64D;
constexpr uint32_t kColFire = 0x77E5484D;
constexpr uint32_t kColJump = 0x7756D364;
constexpr uint32_t kColRope = 0x77FFB300;
constexpr uint32_t kColChange = 0x775B8DEF;
constexpr uint32_t kColDig = 0x77A0703C;
constexpr uint32_t kColMenu = 0xFF444444;

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
constexpr float kStickZoneScale = 1.25F;  // strefa chwytu drążka (× promień)
// LINA = rzut liny, czyli w Liero BRON trzymana + SKOK wciśnięty (Worm::ProcessTasks).
// Jednym kciukiem tego nie zrobisz, więc przycisk wciska oba na kilka klatek.
// Krótko, bo trzymana BRON blokuje chodzenie (Worm::Process → ProcessWeaponChange).
constexpr int kRopePulseFrames = 3;
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

  auto const kW = static_cast<float>(w);
  auto const kH = static_cast<float>(h);

  // Ekran szerszy niż gra (telefon w poziomie, gfx.cpp poszerza wtedy klatkę):
  // gra 16:10 stoi pośrodku, a po bokach są pasy — sterowanie idzie tam, żeby
  // ani przyciski, ani palce nie zasłaniały robaków.
  float const kBar = (kW - (kH * 1.6F)) / 2.0F;
  if (kBar >= kH * 0.22F) {
    float const kPad = std::max(4.0F, kBar * 0.06F);

    // Lewy pas: drążek nisko, pod kciukiem.
    stick_r_ = std::min((kBar * 0.5F) - kPad, kH * 0.20F);
    stick_.base_x = kBar / 2.0F;
    stick_.base_y = kH - kPad - stick_r_ - (kH * 0.04F);

    // Prawy pas: OGIEN na dole pod kciukiem, nad nim dwie kolumny
    // (SKOK | LINA, BRON | KOP), MENU na samej górze.
    float const kCx = kW - (kBar / 2.0F);
    float const kFireR = std::min((kBar * 0.5F) - kPad, kH * 0.14F);
    float const kFireY = kH - kPad - kFireR;
    float const kSmallR = std::min((kBar - (3.0F * kPad)) / 4.0F, kH * 0.075F);
    float const kLeftX = kCx - kSmallR - (kPad / 2.0F);
    float const kRightX = kCx + kSmallR + (kPad / 2.0F);
    float const kRow1 = kFireY - kFireR - kPad - kSmallR;
    float const kRow2 = kRow1 - (2.0F * kSmallR) - (kPad * 1.5F);
    buttons_[kBtnFire] = {.x = kCx, .y = kFireY, .r = kFireR};
    buttons_[kBtnJump] = {.x = kLeftX, .y = kRow1, .r = kSmallR};
    buttons_[kBtnRope] = {.x = kRightX, .y = kRow1, .r = kSmallR};
    buttons_[kBtnChange] = {.x = kLeftX, .y = kRow2, .r = kSmallR};
    buttons_[kBtnDig] = {.x = kRightX, .y = kRow2, .r = kSmallR};
    buttons_[kBtnMenu] = {.x = kCx, .y = kPad + (kSmallR * 0.9F), .r = kSmallR * 0.9F};

    // Cały prawy pas od BRON w dół to strefa przycisków.
    zone_x0_ = kW - kBar;
    zone_x1_ = kW;
    zone_y0_ = kRow2 - (kSmallR * 1.4F);
    zone_y1_ = kH;
    return;
  }

  // Ekran o proporcjach gry (albo wyższy): sterowanie na obrazie gry.
  float const kMin = std::min(kW, kH);
  float const kMargin = std::max(6.0F, kMin * 0.03F);
  stick_r_ = std::clamp(kMin * 0.20F, 34.0F, 110.0F);
  float const kBtnR = std::clamp(kMin * 0.10F, 20.0F, 46.0F);

  // Drążek (lewy kciuk): ruch + celowanie (odpowiednik strzałek).
  stick_.base_x = kMargin + stick_r_;
  stick_.base_y = kH - kMargin - stick_r_;

  // Prawy kciuk: duży OGIEN w rogu, reszta w łuku wokół niego (od dołu-lewej do
  // góry): SKOK, LINA, BRON — każdy w tej samej odległości od OGIEN, więc kciuk
  // oparty na ogniu sięga wszystkich. KOP nad BRON, MENU osobno w rogu ekranu.
  float const kFireR = kBtnR * 1.25F;
  float const kFireX = kW - kMargin - kFireR;
  float const kFireY = kH - kMargin - kFireR;
  buttons_[kBtnFire] = {.x = kFireX, .y = kFireY, .r = kFireR};
  float const kArc = kFireR + (kBtnR * 1.70F);  // odległość środków od OGIEN
  auto const kOnArc = [&](float deg, float r) {
    float const kRad = deg * std::numbers::pi_v<float> / 180.0F;
    return Button{
        .x = kFireX + (std::cos(kRad) * kArc), .y = kFireY - (std::sin(kRad) * kArc), .r = r};
  };
  buttons_[kBtnJump] = kOnArc(180.0F, kBtnR * 0.95F);
  buttons_[kBtnRope] = kOnArc(135.0F, kBtnR * 0.90F);
  buttons_[kBtnChange] = kOnArc(90.0F, kBtnR * 0.90F);
  buttons_[kBtnDig] = {
      .x = kFireX, .y = buttons_[kBtnChange].y - (kBtnR * 1.85F), .r = kBtnR * 0.80F};
  buttons_[kBtnMenu] = {
      .x = kW - kMargin - (kBtnR * 0.70F), .y = kMargin + (kBtnR * 0.70F), .r = kBtnR * 0.70F};

  // Strefa przycisków: prostokąt obejmujący łuk i KOP (bez MENU). Dotyk tutaj —
  // nawet w przerwę albo w etykietę — trafia w najbliższy przycisk. Wcześniej
  // niecelny dotyk po prawej stronie kończył się „tapnięciem”, czyli strzałem.
  zone_x0_ = buttons_[kBtnJump].x - (buttons_[kBtnJump].r * 1.4F);
  zone_x1_ = kW;
  zone_y0_ = buttons_[kBtnDig].y - (buttons_[kBtnDig].r * 1.4F);
  zone_y1_ = kH;
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
      PressButton(gfx, best, kId);
      return;
    }

    // 2) drążek — „chwyt” w promieniu strefy. Środek drążka jest STAŁY (środek
    //    strefy z layoutu) — dotknięcie nie przesuwa drążka pod palec.
    float const kStickR = stick_r_;
    if (!stick_.active && InDisc(kX, kY, stick_.base_x, stick_.base_y, kStickR * kStickZoneScale)) {
      stick_.active = true;
      stick_.finger = kId;
      stick_.x = kX;
      stick_.y = kY;
      return;
    }

    // 3) gdziekolwiek w strefie przycisków → NAJBLIŻSZY przycisk (bez tapnięcia:
    //    tapnięcie w grze to strzał, a tu chodziło właśnie o skok/linę/broń).
    //    MENU pomijamy — wyjście z meczu tylko po trafieniu w sam przycisk.
    if ((kX >= zone_x0_) && (kX <= zone_x1_) && (kY >= zone_y0_) && (kY <= zone_y1_)) {
      int nearest = kBtnFire;
      float nearest_d = 1e9F;
      for (int i = 0; i < kBtnCount; ++i) {
        if (i == kBtnMenu) {
          continue;
        }
        // Odległość od KRAWĘDZI krążka — duży OGIEN nie „kradnie” sąsiadów.
        float const kDx = kX - buttons_[i].x;
        float const kDy = kY - buttons_[i].y;
        float const kD = std::sqrt((kDx * kDx) + (kDy * kDy)) - buttons_[i].r;
        if (kD < nearest_d) {
          nearest_d = kD;
          nearest = i;
        }
      }
      PressButton(gfx, nearest, kId);
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
  if (stick_.active && stick_.finger == kId) {
    stick_.x = kX;
    stick_.y = kY;
    if (kReleased) {
      stick_.active = false;
      stick_.finger = 0;
    }
    return;
  }
  for (Button& b : buttons_) {
    if (b.down && b.finger == kId) {
      if (kReleased) {
        b.finger = 0;
        if (b.seen) {
          b.down = false;
        } else {
          b.release_pending = true;  // gra jeszcze nie widziała wciśnięcia
        }
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

void TouchControls::PressButton(Gfx& gfx, int index, SDL_FingerID finger) {
  Button& b = buttons_.at(static_cast<std::size_t>(index));
  b.down = true;
  b.finger = finger;
  b.seen = false;
  b.release_pending = false;
  if (index == kBtnMenu) {
    // MENU = ESC, tak jak START na padzie (Gfx::DispatchGamepadInput):
    // kontroler (LocalController::OnKey) wygasza i wraca do menu.
    PulseKey(gfx, kDkEscape, 2);
  } else if (index == kBtnRope) {
    PulseKey(gfx, ActionKeyCode(gfx, WormSettings::kChange), kRopePulseFrames);
    PulseKey(gfx, ActionKeyCode(gfx, WormSettings::kJump), kRopePulseFrames);
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
  // In a network match the local worm is driven by the network player's
  // profile (RollbackController), not the left player's.
  int const kProfile = gfx.net_session ? Settings::kNetworkPlayerIdx : 0;
  auto const& ws = gfx.settings->worm_settings[kProfile];
  if (ws == nullptr) {
    return 0;
  }
  return Settings::kExtensions ? ws->controls_ex[control] : ws->controls[control];
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
  uint32_t const kChangeCode = ActionKeyCode(gfx, WormSettings::kChange);
  bool change_pulse_ended = false;
  for (uint32_t k = 0; k < kMaxDosKey; ++k) {
    if (pulse_frames_[k] > 0) {
      wanted_dos_[k] = true;
      if (--pulse_frames_[k] == 0) {
        change_pulse_ended = change_pulse_ended || (k == kChangeCode);
        if (gfx.controller) {
          gfx.controller->OnKey(k, /*state=*/false);
        }
        owned_dos_[k] = false;
        gfx.dos_keys[k] = false;
        wanted_dos_[k] = false;
      }
    }
  }

  // Krótkie tapnięcia: palec puścił, zanim gra zobaczyła przycisk — był wciśnięty
  // przez poprzednią klatkę, więc teraz można go puścić.
  for (Button& b : buttons_) {
    if (b.release_pending && b.seen) {
      b.down = false;
      b.release_pending = false;
    }
  }

  // 2) drążek → 8 kierunków (ruch + celowanie)
  float const kStickR = stick_r_;
  std::array<bool, kDirCount> dirs{};
  if (stick_.active) {
    dirs = StickDirs(ClampUnit((stick_.x - stick_.base_x) / kStickR),
                     ClampUnit((stick_.y - stick_.base_y) / kStickR), stick_dirs_);
  }
  stick_dirs_ = dirs;

  // KOP: klawisz kopania gracza; gdy go nie ma (stare profile mają 0), kopanie
  // to po prostu lewo+prawo naraz — tak samo kopie się na klawiaturze.
  bool const kDigKeyBound = ActionKeyCode(gfx, WormSettings::kDig) != 0;
  if (buttons_[kBtnDig].down && !kDigKeyBound) {
    dirs[kDirLeft] = true;
    dirs[kDirRight] = true;
  }

  constexpr std::array<int, kDirCount> kDirControls = {WormSettings::kLeft, WormSettings::kRight,
                                                       WormSettings::kUp, WormSettings::kDown};
  for (std::size_t d = 0; d < kDirCount; ++d) {
    SetActionKey(gfx, kDirControls.at(d), dirs.at(d));
  }
  SetActionKey(gfx, WormSettings::kFire, buttons_[kBtnFire].down);
  SetActionKey(gfx, WormSettings::kJump, buttons_[kBtnJump].down);
  SetActionKey(gfx, WormSettings::kDig, buttons_[kBtnDig].down && kDigKeyBound);
  SetActionKey(gfx, WormSettings::kChange, buttons_[kBtnChange].down);

  // Po rzucie liny: trzymana BRON zdjęła robakowi lewo/prawo (ProcessWeaponChange
  // robi Release), a palec dalej trzyma drążek. Przypominamy kontrolerowi o
  // trzymanych kierunkach, żeby po rzucie dało się od razu bujać/chodzić.
  if (change_pulse_ended && !buttons_[kBtnChange].down && gfx.controller) {
    for (std::size_t d = kDirLeft; d <= kDirRight; ++d) {
      uint32_t const kCode = ActionKeyCode(gfx, kDirControls.at(d));
      if (dirs.at(d) && kCode != 0 && KeyHeldByTouch(kCode)) {
        gfx.controller->OnKey(static_cast<int>(kCode), /*state=*/true);
      }
    }
  }

  // Gra zobaczyła w tej klatce wszystko, co jest wciśnięte.
  for (Button& b : buttons_) {
    b.seen = b.down;
  }

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
  float const kStickR = stick_r_;

  // Drążek: podstawa + gałka (gałka idzie za palcem, przycięta do promienia).
  Stick const& s = stick_;
  DrawDisc(surface, s.base_x, s.base_y, kStickR, kZoneFill, 1.0F,
           s.active ? kZoneRingHot : kZoneRing, s.active ? 0.85F : 0.45F);
  if (s.active) {
    float dx = s.x - s.base_x;
    float dy = s.y - s.base_y;
    float const kLen = std::sqrt(dx * dx + dy * dy);
    if (kLen > kStickR) {
      dx = dx / kLen * kStickR;
      dy = dy / kLen * kStickR;
    }
    DrawDisc(surface, s.base_x + dx, s.base_y + dy, kStickR * 0.42F, kKnob, 1.0F, 0x33FFFFFF, 0.5F);
  }

  // Przyciski akcji.
  struct BtnStyle {
    uint32_t color;
    char const* label;
  };
  BtnStyle const kStyles[kBtnCount] = {
      {.color = kColFire, .label = "OGIEN"}, {.color = kColJump, .label = "SKOK"},
      {.color = kColRope, .label = "LINA"},  {.color = kColChange, .label = "BRON"},
      {.color = kColDig, .label = "KOP"},    {.color = kColMenu, .label = "MENU"}};
  for (int i = 0; i < kBtnCount; ++i) {
    Button const& b = buttons_[i];
    DrawDisc(surface, b.x, b.y, b.r, kStyles[i].color, b.down ? 1.0F : 0.75F, 0xAAFFFFFF,
             b.down ? 1.0F : 0.55F);
    DrawLabel(surface, gfx, kStyles[i].label, b.x, b.y);
  }
}
