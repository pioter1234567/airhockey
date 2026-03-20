#include "anims.h"
#include "display.h"
#include <Arduino.h>
#include <SD.h>
#include <LovyanGFX.hpp>



// funkcja już istnieje w main.cpp
extern bool drawRgb565File(const char* path, int w, int h);

// aktualne tło też trzymasz w main.cpp
extern const char *g_currentBgPath;

// ======================= TEST ANIM =======================

static constexpr int ANIM_X = 192;
static constexpr int ANIM_Y = 148;
static constexpr int ANIM_W = 188;
static constexpr int ANIM_H = 172;

static constexpr const char* ANIM_BASE_PATH   = "/base.bin";
static constexpr const char* ANIM_MIDDLE_PATH = "/middle.bin";
static constexpr const char* ANIM_RIGHT_PATH  = "/right.bin";
static constexpr const char* ANIM_LEFT_PATH   = "/left.bin";

static lgfx::LGFX_Sprite g_animSprMiddle(&tft);
static lgfx::LGFX_Sprite g_animSprRight(&tft);
static lgfx::LGFX_Sprite g_animSprLeft(&tft);

static bool g_animSpritesReady = false;
static bool g_animPlaying = false;
static uint8_t g_animStep = 0;
static uint32_t g_animNextAtMs = 0;

// middle, right, middle, left
static const uint16_t g_animDurMs[4] = { 30, 500, 50, 500 };

static bool createAnimSpriteFromFile(lgfx::LGFX_Sprite& spr, const char* path, int w, int h)
{
  if (spr.getBuffer() != nullptr)
    spr.deleteSprite();

  spr.setColorDepth(16);
  spr.setPsram(true);

  if (!spr.createSprite(w, h))
  {
    Serial.printf("[ANIM] createSprite FAIL: %s\n", path);
    return false;
  }

  File f = SD.open(path, FILE_READ);
  if (!f)
  {
    Serial.printf("[ANIM] open FAIL: %s\n", path);
    spr.deleteSprite();
    return false;
  }

  const size_t need = (size_t)w * h * 2;
  size_t rd = f.read((uint8_t*)spr.getBuffer(), need);
  f.close();

  if (rd != need)
  {
    Serial.printf("[ANIM] read FAIL: %s got=%u need=%u\n",
                  path, (unsigned)rd, (unsigned)need);
    spr.deleteSprite();
    return false;
  }

  Serial.printf("[ANIM] sprite loaded: %s\n", path);
  return true;
}

static bool loadTestAnimSprites()
{
  if (SD.cardType() == CARD_NONE)
  {
    Serial.println("[ANIM] no SD");
    return false;
  }

  if (!SD.exists(ANIM_MIDDLE_PATH) || !SD.exists(ANIM_RIGHT_PATH) || !SD.exists(ANIM_LEFT_PATH))
  {
    Serial.println("[ANIM] missing one of: /middle.bin /right.bin /left.bin");
    return false;
  }

  bool ok = true;
  ok &= createAnimSpriteFromFile(g_animSprMiddle, ANIM_MIDDLE_PATH, ANIM_W, ANIM_H);
  ok &= createAnimSpriteFromFile(g_animSprRight,  ANIM_RIGHT_PATH,  ANIM_W, ANIM_H);
  ok &= createAnimSpriteFromFile(g_animSprLeft,   ANIM_LEFT_PATH,   ANIM_W, ANIM_H);

  g_animSpritesReady = ok;
  Serial.printf("[ANIM] sprites ready: %s\n", ok ? "YES" : "NO");
  return ok;
}

static void drawAnimStep(uint8_t step)
{
  switch (step)
  {
    case 0:
    case 2:
      g_animSprMiddle.pushSprite(ANIM_X, ANIM_Y);
      break;

    case 1:
      g_animSprRight.pushSprite(ANIM_X, ANIM_Y);
      break;

    case 3:
      g_animSprLeft.pushSprite(ANIM_X, ANIM_Y);
      break;
  }
}

void animsInit()
{
  g_animSpritesReady = false;
  g_animPlaying = false;
  g_animStep = 0;
  g_animNextAtMs = 0;
}

void animsStartTest()
{
  if (SD.cardType() == CARD_NONE)
  {
    Serial.println("[ANIM] start FAIL: no SD");
    return;
  }

  if (!SD.exists(ANIM_BASE_PATH))
  {
    Serial.println("[ANIM] start FAIL: missing /base.bin");
    return;
  }

  bool ok = drawRgb565File(ANIM_BASE_PATH, 480, 320);
  Serial.printf("[ANIM] draw base: %s\n", ok ? "OK" : "FAIL");
  if (!ok)
    return;

  g_currentBgPath = ANIM_BASE_PATH;

  if (!g_animSpritesReady)
  {
    if (!loadTestAnimSprites())
      return;
  }

  g_animPlaying = true;
  g_animStep = 0;

  drawAnimStep(g_animStep);
  g_animNextAtMs = millis() + g_animDurMs[g_animStep];

  Serial.println("[ANIM] START");
}

void animsStop()
{
  g_animPlaying = false;
  Serial.println("[ANIM] STOP");
}

bool animsIsPlaying()
{
  return g_animPlaying;
}

void animsTick()
{
  if (!g_animPlaying)
    return;

  if ((int32_t)(millis() - g_animNextAtMs) < 0)
    return;

  g_animStep = (g_animStep + 1) & 0x03;
  drawAnimStep(g_animStep);
  g_animNextAtMs = millis() + g_animDurMs[g_animStep];
}