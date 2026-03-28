#pragma once
#include <stdint.h>
#include <stddef.h>

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

enum UvLevel : uint8_t
{
  UV_LEVEL_OFF = 0,
  UV_LEVEL_BASE,
  UV_LEVEL_BOOST
};

// ===== API =====
void animsInit();
void animsSetMode(AnimMode m);
void animsSetRoundColor(uint8_t r, uint8_t g, uint8_t b);
void animsSetBreathing(bool enable);
void animsOnGoal(uint8_t side, uint16_t durationMs = 500);
void animsTick(uint32_t nowMs);
AnimMode animsGetMode();

UvLevel animsGetUvLevel(bool uvEnabled, bool animEnabled, uint8_t theme, uint32_t ms);






