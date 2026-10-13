#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "keys.hpp"

struct Gfx;

// Warstwa sterowania dotykowego (wariant C — „hybryda”, makieta: docs/f3-*).
//
// Zasada: symulacja nie wie o dotyku. Nakładka wciska dokładnie te klawisze DOS,
// które gracz ma przypisane do akcji (`controls_ex`/`controls` — te same, których
// używa Gfx::TestControl), a w menu dodatkowo ENTER i strzałki (impuls, „one-shot”)
// — czyli to samo, co robi klawiatura w Gfx::ProcessEvent. Dzięki temu fizyka,
// snapshoty i determinizm (baseline F1) nie mają prawa się zmienić.
//
// Układ (współrzędne okna; nakładka skaluje się razem z oknem):
//   lewy drążek  = ruch (lewo/prawo) + celowanie (góra/dół)
//   prawy drążek = celowanie precyzyjne (góra/dół + obrót) i auto-ogień po wychyleniu
//   przyciski    = OGIEN, SKOK, LINA, BRON w kolumnie przy prawej krawędzi
//   tap poza strefami = potwierdzenie (ENTER) — pozwala przejść menu i wybór broni
struct TouchControls {
  enum ButtonAction {
    kBtnFire = 0,
    kBtnJump,
    kBtnRope,
    kBtnChange,
    kBtnCount,
  };

  struct Stick {
    bool active = false;
    SDL_FingerID finger = 0;
    float base_x = 0.0F;  // środek drążka: miejsce dotknięcia (przeciągasz względem niego)
    float base_y = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
  };

  struct Button {
    float x = 0.0F;
    float y = 0.0F;
    float r = 0.0F;
    bool down = false;
    SDL_FingerID finger = 0;
  };

  bool Enabled() const { return enabled_; }
  void SetEnabled(bool on) { enabled_ = on; }
  // Czy widać nakładkę (na desktopie/testach można wyłączyć rysowanie).
  bool Visible() const { return enabled_ && draw_overlay_; }
  void SetDrawOverlay(bool on) { draw_overlay_ = on; }
  // Czy dotknięto ekranu choć raz (używane do auto-włączania na desktopie).
  bool HadFinger() const { return had_finger_; }

  void HandleEvent(SDL_Event const& ev, Gfx& gfx);
  // Raz na klatkę, przed aktualizacją stanu gry: przelicza dotyk na klawisze.
  void Apply(Gfx& gfx);
  // Nakładka rysowana na powierzchni okna (ARGB8888) — na wierzchu klatki gry.
  void DrawOverlay(SDL_Surface& surface, Gfx& gfx);

  // Do testów: programowe dotknięcie (współrzędne znormalizowane 0..1 jak w SDL).
  void SimulateFinger(SDL_FingerID id, bool down, float nx, float ny, Gfx& gfx);

 private:
  void RecomputeLayout(Gfx& gfx);
  void PulseKey(Gfx& gfx, uint32_t code, int frames);
  uint32_t ActionKeyCode(Gfx& gfx, int control) const;
  void SetActionKey(Gfx& gfx, int control, bool down);
  void DrawLabel(SDL_Surface& surface, Gfx& gfx, std::string const& text, float cx, float cy);
  bool KeyHeldByTouch(uint32_t code) const;

  bool enabled_ = false;
  bool draw_overlay_ = true;
  bool had_finger_ = false;

  bool layout_valid_ = false;
  int layout_w_ = 0;
  int layout_h_ = 0;

  std::array<Stick, 2> sticks_{};  // 0 = ruch, 1 = celowanie
  std::array<Button, kBtnCount> buttons_{};

  // Które klawisze DOS wcisnęła nakładka (żeby nie zwalniać klawiszy klawiatury).
  std::array<bool, kMaxDosKey> owned_dos_{};
  std::unordered_set<uint32_t> owned_ex_{};
  // Klawisze „chciane” w tej klatce (ustawiane przez SetActionKey) — potrzebne, żeby
  // sprzątanie na końcu Apply nie zwalniało klawiszy właśnie wciśniętych.
  std::array<bool, kMaxDosKey> wanted_dos_{};
  std::unordered_set<uint32_t> wanted_ex_{};
  // Ile klatek trzymać klawisz-impuls (menu/tap); indeks = kod DOS.
  std::array<int, kMaxDosKey> pulse_frames_{};
  // Stan „impulsu kierunkowego” drążka: [drążek][oś] = -1/0/1 (0 = wrócił do martwej strefy).
  std::array<std::array<int, 2>, 2> nudge_{};

  // Potencjalne tapnięcie poza strefami (potwierdzenie w menu).
  bool tap_pending_ = false;
  SDL_FingerID tap_finger_ = 0;
  float tap_x_ = 0.0F;
  float tap_y_ = 0.0F;
  int tap_frames_ = 0;
};
