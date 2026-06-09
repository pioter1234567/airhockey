#include "table_lights.h"

#include <Arduino.h>
#include <FastLED.h>
#include <SD.h>

// =====================================================
// HARDWARE
// =====================================================

// GPIO12 jest zajęte przez główną matrycę.
// GPIO13 dajemy na pasek ambiente pod stołem.
// GPIO11 zostawiamy pod bramki.
#define AMBIENT_DATA_PIN 13
#define AMBIENT_LED_COUNT 192

#define GOAL_DATA_PIN 11
#define GOAL_LED_COUNT 0   // na razie 0, jak podłączysz bramki, ustawimy realną liczbę

// =====================================================
// BIN PARAMS
// =====================================================

#define TABLE_BIN_FPS 20
#define TABLE_BIN_FRAME_MS (1000 / TABLE_BIN_FPS)

#define AMBIENT_FRAME_SIZE (AMBIENT_LED_COUNT * 3)

static CRGB ambientLeds[AMBIENT_LED_COUNT];

#if GOAL_LED_COUNT > 0
static CRGB goalLeds[GOAL_LED_COUNT];
#endif

// =====================================================
// MUSIC BIN STATE
// =====================================================

static File ambientFile;
static bool ambientPlaying = false;
static const char *ambientPath = nullptr;

static uint32_t ambientFrameCount = 0;
static uint32_t ambientLastFrame = UINT32_MAX;
static uint32_t ambientStartMs = 0;

static uint8_t ambientBuf[AMBIENT_FRAME_SIZE];



// =====================================================
// ROUND AMBIENT STATE
// =====================================================

static bool ambientRoundEnabled = false;
static bool ambientBreathing = false;
static CRGB ambientRoundColor = CRGB::Black;
static uint32_t ambientLastSolidFrame = 0;


// =====================================================
// HELPERS
// =====================================================

static bool readExact(File &file, uint8_t *dst, size_t n)
{
  size_t got = 0;

  while (got < n)
  {
    int r = file.read(dst + got, n - got);

    if (r <= 0)
      return false;

    got += (size_t)r;
  }

  return true;
}

static void ambientClear()
{
  fill_solid(ambientLeds, AMBIENT_LED_COUNT, CRGB::Black);
}


static void renderAmbientRound(uint32_t nowMs)
{
  // limit ~40 FPS, tak jak matryca
  if (nowMs - ambientLastSolidFrame < 25)
    return;

  ambientLastSolidFrame = nowMs;

  float scale = 1.0f;

  if (ambientBreathing)
  {
    float t = (nowMs % 2600) / 2600.0f;
    scale = 0.35f + 0.65f * (0.5f - 0.5f * cosf(t * 2.0f * PI));
  }

  CRGB c = ambientRoundColor;
  c.nscale8_video((uint8_t)(scale * 255.0f));

  // Tak samo jak przy BIN-ach ambiente: swap R/G
  fill_solid(ambientLeds, AMBIENT_LED_COUNT, CRGB(c.g, c.r, c.b));
}

#if GOAL_LED_COUNT > 0
static void goalsClear()
{
  fill_solid(goalLeds, GOAL_LED_COUNT, CRGB::Black);
}
#endif

// =====================================================
// PUBLIC API
// =====================================================

void tableLightsInit(uint8_t brightness)
{
  // Uwaga: FastLED brightness jest globalne.
  // Nie ustawiamy tu FastLED.setBrightness(), żeby nie mieszać jasności matrycy.
  FastLED.addLeds<WS2812B, AMBIENT_DATA_PIN, GRB>(ambientLeds, AMBIENT_LED_COUNT);

#if GOAL_LED_COUNT > 0
  FastLED.addLeds<WS2812B, GOAL_DATA_PIN, GRB>(goalLeds, GOAL_LED_COUNT);
#endif

  ambientClear();

#if GOAL_LED_COUNT > 0
  goalsClear();
#endif

  FastLED.show();

  Serial.printf("[TABLE] init ambient pin=%u leds=%u\n",
                (unsigned)AMBIENT_DATA_PIN,
                (unsigned)AMBIENT_LED_COUNT);

#if GOAL_LED_COUNT > 0
  Serial.printf("[TABLE] init goals pin=%u leds=%u\n",
                (unsigned)GOAL_DATA_PIN,
                (unsigned)GOAL_LED_COUNT);
#endif
}

void tableLightsAllOff()
{
  tableLightsMusicStop();

  ambientClear();

#if GOAL_LED_COUNT > 0
  goalsClear();
#endif
}

void tableLightsMusicStart(const char *path, uint32_t syncStartMs)
{
  tableLightsMusicStop();

  if (!path)
  {
    ambientClear();
    Serial.println("[TABLE] ambient music: no file for this theme");
    return;
  }

  ambientPath = path;
  ambientFile = SD.open(ambientPath, FILE_READ);

  if (!ambientFile)
  {
    Serial.printf("[TABLE] ambient open FAIL: %s\n", ambientPath);
    ambientClear();
    return;
  }

  uint32_t size = (uint32_t)ambientFile.size();
  ambientFrameCount = size / AMBIENT_FRAME_SIZE;

  if (ambientFrameCount == 0)
  {
    Serial.printf("[TABLE] ambient empty/too small: %s size=%lu\n",
                  ambientPath,
                  (unsigned long)size);

    ambientFile.close();
    ambientClear();
    return;
  }

  ambientStartMs = syncStartMs;
  ambientLastFrame = UINT32_MAX;
  ambientPlaying = true;

  Serial.printf("[TABLE] ambient music START: %s frames=%lu size=%lu startMs=%lu\n",
                ambientPath,
                (unsigned long)ambientFrameCount,
                (unsigned long)size,
                (unsigned long)ambientStartMs);
}

void tableLightsMusicStop()
{
  if (ambientFile)
    ambientFile.close();

  ambientPlaying = false;
  ambientPath = nullptr;
  ambientFrameCount = 0;
  ambientLastFrame = UINT32_MAX;
}


void tableLightsSetRoundColor(uint8_t r, uint8_t g, uint8_t b)
{
  ambientRoundColor = CRGB(r, g, b);
}

void tableLightsSetBreathing(bool enable)
{
  ambientBreathing = enable;
}

void tableLightsSetRoundEnabled(bool enable)
{
  ambientRoundEnabled = enable;

  if (!enable)
    ambientClear();
}

void tableLightsTick(uint32_t nowMs)
{
  if (!ambientPlaying)
  {
    if (ambientRoundEnabled)
      renderAmbientRound(nowMs);

    return;
  }

  if (!ambientFile || ambientFrameCount == 0)
  {
    if (ambientRoundEnabled)
      renderAmbientRound(nowMs);

    return;
  }

  uint32_t elapsed = nowMs - ambientStartMs;
  uint32_t frameIndex = (elapsed / TABLE_BIN_FRAME_MS) % ambientFrameCount;

  if (frameIndex == ambientLastFrame)
    return;

  uint32_t pos = frameIndex * AMBIENT_FRAME_SIZE;

  if (!ambientFile.seek(pos))
  {
    Serial.printf("[TABLE] ambient seek FAIL frame=%lu pos=%lu\n",
                  (unsigned long)frameIndex,
                  (unsigned long)pos);
    return;
  }

  if (!readExact(ambientFile, ambientBuf, AMBIENT_FRAME_SIZE))
  {
    Serial.printf("[TABLE] ambient read FAIL frame=%lu pos=%lu\n",
                  (unsigned long)frameIndex,
                  (unsigned long)pos);
    return;
  }

  for (uint16_t i = 0; i < AMBIENT_LED_COUNT; i++)
  {
    uint8_t r = ambientBuf[i * 3 + 0];
    uint8_t g = ambientBuf[i * 3 + 1];
    uint8_t b = ambientBuf[i * 3 + 2];

    // Tak samo jak w matrixSetIndex() masz swap R/G.
    // Jeśli kolory będą odwrócone, zmienimy na CRGB(r, g, b).
    ambientLeds[i] = CRGB(g, r, b);
  }

  ambientLastFrame = frameIndex;
}

// =====================================================
// GOALS — na później
// =====================================================

void tableLightsGoalFx(uint8_t scoringSide)
{
  Serial.printf("[TABLE] goal FX side=%u\n", (unsigned)scoringSide);
}

void tableLightsSetGoalsEnabled(bool enabled)
{
  Serial.printf("[TABLE] goals %s\n", enabled ? "ON" : "OFF");
}