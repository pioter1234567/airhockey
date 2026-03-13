#pragma once
#include <stdint.h>

// ===== TRYBY ANIMACJI =====
enum AnimMode : uint8_t {
  ANIM_OFF = 0,
  ANIM_ROUND,
  ANIM_GOAL,
  ANIM_GAMEOVER
};


struct RoundColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// ===== API =====
void animsInit();
void animsSetMode(AnimMode m);
void animsSetRoundColor(uint8_t r, uint8_t g, uint8_t b);
void animsSetBreathing(bool enable);

// GOAL trigger
// side: 0 = A, 1 = B
// durationMs: ile ma wisieć efekt (np. 400–600 ms)
void animsOnGoal(uint8_t side, uint16_t durationMs = 500);

// tick z loop()
void animsTick(uint32_t nowMs);

AnimMode animsGetMode();
