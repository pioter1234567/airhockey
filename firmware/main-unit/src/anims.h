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


enum LampMode : uint8_t {
  LMODE_OFF   = 0,
  LMODE_WHITE = 1,
  LMODE_COLOR = 2
};

enum LampColor : uint8_t {
  LCOLOR_NONE    = 0,
  LCOLOR_RED     = 1,
  LCOLOR_GREEN   = 2,
  LCOLOR_BLUE    = 3,
  LCOLOR_YELLOW  = 4,
  LCOLOR_CYAN    = 5,
  LCOLOR_MAGENTA = 6,
  LCOLOR_RAINBOW = 7
};

enum LampDuration : uint8_t {
  LDUR_15MIN = 1,
  LDUR_60MIN = 2,
  LDUR_3H    = 3,
  LDUR_6H    = 4
};

void animsLampSet(uint8_t mode, uint8_t value, uint8_t duration);
void animsLampOff();
bool animsLampActive();




