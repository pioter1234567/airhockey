#include "anims.h"
#include "display.h"
#include "unit_config.h"
#include <Arduino.h>
#include <SD.h>
#include <LovyanGFX.hpp>
#include <stdio.h>
#include <string.h>

extern bool drawRgb565File(const char* path, int w, int h);
extern const char *g_currentBgPath;
static uint32_t g_animEndAtMs = 0;

// ======================= LAYOUT =======================

static constexpr int ANIM_X = 192;
static constexpr int ANIM_Y = 148;
static constexpr int ANIM_W = 188;
static constexpr int ANIM_H = 172;

// middle, right, middle, left
static const uint16_t g_animDurMs[4] = { 30, 500, 50, 500 };

static lgfx::LGFX_Sprite g_animSprMiddle(&tft);
static lgfx::LGFX_Sprite g_animSprRight(&tft);
static lgfx::LGFX_Sprite g_animSprLeft(&tft);

static bool g_animSpritesReady = false;
static bool g_animPlaying = false;
static uint8_t g_animStep = 0;
static uint32_t g_animNextAtMs = 0;

// aktualnie załadowany zestaw
static char g_basePath[96]   = "";
static char g_middlePath[96] = "";
static char g_rightPath[96]  = "";
static char g_leftPath[96]   = "";

// ======================= LOGIKA =======================

enum class CharacterId : uint8_t
{
  A = 0,
  B = 1
};

enum class Mood : uint8_t
{
  Sad = 0,
  Happy = 1
};

struct AnimSet
{
  char base[96];
  char middle[96];
  char right[96];
  char left[96];
};

static const char* charFolder(CharacterId ch)
{

// A -> /rayman
// B -> /globox
return (ch == CharacterId::A) ? "/rayman" : "/globox";
}

static Mood computeMood(uint8_t selfScore, uint8_t otherScore)
{
  if (selfScore == otherScore)
    return (selfScore == 0) ? Mood::Sad : Mood::Happy;

  return (selfScore > otherScore) ? Mood::Happy : Mood::Sad;
}

static bool isStateValid(uint8_t score, Mood mood)
{
  if (score > 3) return false;
  if (score == 0 && mood == Mood::Happy) return false;
  if (score == 3 && mood == Mood::Sad)   return false;
  return true;
}

static bool buildAnimSet(AnimSet& out, CharacterId ch, uint8_t score, Mood mood)
{
  if (!isStateValid(score, mood))
    return false;

  const char* folder = charFolder(ch);
  const char* moodStr = (mood == Mood::Happy) ? "happy" : "sad";

  snprintf(out.base,   sizeof(out.base),   "%s/base_%u_%s.bin",   folder, score, moodStr);
  snprintf(out.middle, sizeof(out.middle), "%s/middle_%u_%s.bin", folder, score, moodStr);
  snprintf(out.right,  sizeof(out.right),  "%s/right_%u_%s.bin",  folder, score, moodStr);
  snprintf(out.left,   sizeof(out.left),   "%s/left_%u_%s.bin",   folder, score, moodStr);

  return true;
}

// ======================= SPRITES =======================

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

static bool loadCurrentAnimSprites()
{
  if (SD.cardType() == CARD_NONE)
  {
    Serial.println("[ANIM] no SD");
    return false;
  }

  if (!SD.exists(g_middlePath) || !SD.exists(g_rightPath) || !SD.exists(g_leftPath))
  {
    Serial.println("[ANIM] missing one of current sprite files");
    Serial.println(g_middlePath);
    Serial.println(g_rightPath);
    Serial.println(g_leftPath);
    return false;
  }

  bool ok = true;
  ok &= createAnimSpriteFromFile(g_animSprMiddle, g_middlePath, ANIM_W, ANIM_H);
  ok &= createAnimSpriteFromFile(g_animSprRight,  g_rightPath,  ANIM_W, ANIM_H);
  ok &= createAnimSpriteFromFile(g_animSprLeft,   g_leftPath,   ANIM_W, ANIM_H);

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

static bool startAnimFromSet(const AnimSet& set)
{
  if (SD.cardType() == CARD_NONE)
  {
    Serial.println("[ANIM] start FAIL: no SD");
    return false;
  }

  if (!SD.exists(set.base))
  {
    Serial.printf("[ANIM] missing base: %s\n", set.base);
    return false;
  }

  strncpy(g_basePath,   set.base,   sizeof(g_basePath)   - 1);
  strncpy(g_middlePath, set.middle, sizeof(g_middlePath) - 1);
  strncpy(g_rightPath,  set.right,  sizeof(g_rightPath)  - 1);
  strncpy(g_leftPath,   set.left,   sizeof(g_leftPath)   - 1);

  g_basePath[sizeof(g_basePath) - 1]     = 0;
  g_middlePath[sizeof(g_middlePath) - 1] = 0;
  g_rightPath[sizeof(g_rightPath) - 1]   = 0;
  g_leftPath[sizeof(g_leftPath) - 1]     = 0;

  bool ok = drawRgb565File(g_basePath, 480, 320);
  Serial.printf("[ANIM] draw base: %s -> %s\n", g_basePath, ok ? "OK" : "FAIL");
  if (!ok)
    return false;

  g_currentBgPath = g_basePath;

  if (!loadCurrentAnimSprites())
    return false;

  g_animPlaying = true;
  g_animStep = 0;
  drawAnimStep(g_animStep);
  g_animNextAtMs = millis() + g_animDurMs[g_animStep];

  Serial.println("[ANIM] START");
  return true;
}

// ======================= API =======================

void animsInit()
{
  g_animSpritesReady = false;
  g_animPlaying = false;
  g_animStep = 0;
  g_animNextAtMs = 0;
}

void animsStartTest()
{
  // stary prosty test zostawiamy jako fallback:
  AnimSet set{};
  strncpy(set.base,   "/base.bin",   sizeof(set.base) - 1);
  strncpy(set.middle, "/middle.bin", sizeof(set.middle) - 1);
  strncpy(set.right,  "/right.bin",  sizeof(set.right) - 1);
  strncpy(set.left,   "/left.bin",   sizeof(set.left) - 1);

  startAnimFromSet(set);
}

void animsStartStateTest(uint8_t scoreA, uint8_t scoreB, char whichChar)
{
  CharacterId ch;
  uint8_t selfScore, otherScore;

  if (whichChar == 'a' || whichChar == 'A')
  {
    ch = CharacterId::A;
    selfScore = scoreA;
    otherScore = scoreB;
  }
  else
  {
    ch = CharacterId::B;
    selfScore = scoreB;
    otherScore = scoreA;
  }

  Mood mood = computeMood(selfScore, otherScore);

  AnimSet set{};
  if (!buildAnimSet(set, ch, selfScore, mood))
  {
    Serial.printf("[ANIM] invalid state: ch=%c self=%u other=%u\n",
                  whichChar, selfScore, otherScore);
    return;
  }

  Serial.printf("[ANIM] state test: ch=%c self=%u other=%u mood=%s\n",
                whichChar,
                selfScore,
                otherScore,
                (mood == Mood::Happy) ? "happy" : "sad");

  startAnimFromSet(set);
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
    // KONIEC ANIMACJI
if (g_animEndAtMs != 0 && (int32_t)(millis() - g_animEndAtMs) >= 0)
{
  g_animEndAtMs = 0;

  Serial.println("[ANIM] RESULT END -> fade + test.bin");

    fadeBacklight(255, 0, 120);
  

  g_animPlaying = false;

  g_currentBgPath = "/test.bin";
  drawRgb565File(g_currentBgPath, 480, 320);

  fadeBacklight(0, 255, 140);
  return;
}
}

void animsStartResult(uint8_t scoreA, uint8_t scoreB)
{
  char who = (UNIT_SIDE == 'A') ? 'a' : 'b';
  animsStartStateTest(scoreA, scoreB, who);

  g_animEndAtMs = millis() + 13000; // ile sekund
}