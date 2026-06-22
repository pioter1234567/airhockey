#include "anims.h"
#include "led_matrix.h"
#include <Arduino.h> // millis()
#include <FastLED.h>

static AnimMode g_mode = ANIM_OFF;
static CRGB g_roundColor = CRGB::Black;
static bool g_breathing = false;
static uint32_t g_goalUntilMs = 0;

static bool     g_lampManualActive = false;
static uint8_t  g_lampModeManual = LMODE_OFF;
static uint8_t  g_lampValueManual = 0;
static uint32_t g_lampUntilMs = 0;
static uint32_t g_lampFxStartMs = 0;

// GOAL state
static uint8_t  g_goalSide = 0;     // 0 = A, 1 = B
static uint32_t g_eventMs  = 0;
static uint16_t g_goalDur  = 500;   // ms



// UV tablice

struct TimeWindow
{
  uint32_t fromMs;
  uint32_t toMs;
};



struct UvPattern
{
  const TimeWindow* offWindows;
  size_t offCount;
  const TimeWindow* boostWindows;
  size_t boostCount;
};

static bool isInWindow(uint32_t t, const TimeWindow* w, size_t n)
{
  for (size_t i = 0; i < n; ++i)
  {
    if (t >= w[i].fromMs && t < w[i].toMs)
      return true;
  }
  return false;
}




// ===== TEENSIES =====
static const TimeWindow UV_Teensies_OFF[] = {
    // TODO
};

static const TimeWindow UV_Teensies_BOOST[] = {
    // TODO
};


// ===== TOAD =====
static const TimeWindow UV_ToadStory_OFF[] = {
    {  5150,   5200 },
    {  5800,   6100 },
    { 14050,  14700 },
    { 15400,  16000 },
    { 16450,  17350 },
    { 29250,  29350 },
    { 29550,  29600 },
    { 29950,  30000 },
    { 30200,  30300 },
    { 30500,  30550 },
    { 30850,  30950 },
    { 31100,  31300 },
    { 49550,  49600 },
    { 55350,  55450 },
    { 64950,  65050 },
    { 65250,  65350 },
    { 65550,  65700 },
    { 65900,  66000 },
    { 66200,  66350 },
    { 66550,  66650 },
    { 66900,  67000 },
    { 67200,  67350 },
    { 81100,  81750 },
};

static const TimeWindow UV_ToadStory_BOOST[] = {
    {  6700,   6800 },
    {  7050,   7150 },
    { 12350,  12550 },
    { 18700,  19200 },
    { 28650,  28950 },
    { 29050,  29250 },
    { 29350,  29550 },
    { 29600,  29950 },
    { 30000,  30200 },
    { 30300,  30500 },
    { 30550,  30850 },
    { 30950,  31100 },
    { 33050,  33400 },
    { 33650,  34050 },
    { 35700,  36050 },
    { 36300,  36700 },
    { 38350,  38700 },
    { 39000,  39400 },
    { 39650,  40200 },
    { 44700,  45100 },
    { 47100,  47500 },
    { 49600,  50100 },
    { 52700,  53200 },
    { 53350,  53900 },
    { 57250,  57900 },
    { 81900,  82400 },
};


// ===== FIESTA =====
static const TimeWindow UV_Fiesta_OFF[] = {
    // TODO
};

static const TimeWindow UV_Fiesta_BOOST[] = {
    // TODO
};


// ===== 20000 LUMS =====
static const TimeWindow UV_Lums_OFF[] = {
    // TODO
};

static const TimeWindow UV_Lums_BOOST[] = {
    // TODO
};


// ===== OLYMPUS =====
static const TimeWindow UV_Olympus_OFF[] = {
    // TODO
};

static const TimeWindow UV_Olympus_BOOST[] = {
    // TODO
};


// ===== GRANNIES =====
static const TimeWindow UV_Grannies_OFF[] = {
    { 35800, 36050 },
    { 36350, 36700 },
    { 37000, 37400 },
    { 46700, 49200 },
    { 58400, 59950 },
    { 89550, 91600 },
};

static const TimeWindow UV_Grannies_BOOST[] = {
    { 33400, 33600 },
    { 34050, 34250 },
    { 36050, 36350 },
    { 36700, 37000 },
    { 38650, 38850 },
    { 39300, 39500 },
    { 46400, 46700 },
};

static const UvPattern UV_PATTERNS[] = {
    {
      UV_Teensies_OFF,
      sizeof(UV_Teensies_OFF) / sizeof(TimeWindow),
      UV_Teensies_BOOST,
      sizeof(UV_Teensies_BOOST) / sizeof(TimeWindow)
    }, // 0 = teensies

    {
      UV_ToadStory_OFF,
      sizeof(UV_ToadStory_OFF) / sizeof(TimeWindow),
      UV_ToadStory_BOOST,
      sizeof(UV_ToadStory_BOOST) / sizeof(TimeWindow)
    }, // 1 = toad

    {
      UV_Fiesta_OFF,
      sizeof(UV_Fiesta_OFF) / sizeof(TimeWindow),
      UV_Fiesta_BOOST,
      sizeof(UV_Fiesta_BOOST) / sizeof(TimeWindow)
    }, // 2 = fiesta

    {
      UV_Lums_OFF,
      sizeof(UV_Lums_OFF) / sizeof(TimeWindow),
      UV_Lums_BOOST,
      sizeof(UV_Lums_BOOST) / sizeof(TimeWindow)
    }, // 3 = 20000lums

    {
      UV_Olympus_OFF,
      sizeof(UV_Olympus_OFF) / sizeof(TimeWindow),
      UV_Olympus_BOOST,
      sizeof(UV_Olympus_BOOST) / sizeof(TimeWindow)
    }, // 4 = olympus

    {
      UV_Grannies_OFF,
      sizeof(UV_Grannies_OFF) / sizeof(TimeWindow),
      UV_Grannies_BOOST,
      sizeof(UV_Grannies_BOOST) / sizeof(TimeWindow)
    }, // 5 = grannies

};







UvLevel animsGetUvLevel(bool uvEnabled, bool animEnabled, uint8_t theme, uint32_t ms)
{
  if (!uvEnabled) return UV_LEVEL_OFF;

  // animacje OFF -> stałe UV bazowe
  if (!animEnabled) return UV_LEVEL_BASE;

  if (theme >= (sizeof(UV_PATTERNS) / sizeof(UV_PATTERNS[0])))
    return UV_LEVEL_BASE;

  const UvPattern& p = UV_PATTERNS[theme];

  if (p.offWindows && p.offCount && isInWindow(ms, p.offWindows, p.offCount))
    return UV_LEVEL_OFF;

  if (p.boostWindows && p.boostCount && isInWindow(ms, p.boostWindows, p.boostCount))
    return UV_LEVEL_BOOST;

  return UV_LEVEL_BASE;
}

void animsInit(){
  g_mode    = ANIM_OFF;
  g_eventMs = 0;
}

void animsSetMode(AnimMode m){
  if (g_mode != m){
    g_mode    = m;
    g_eventMs = 0; // restart phase timer
  }
}




static uint32_t lampDurationToMs(uint8_t d)
{
  switch (d)
  {
    case LDUR_15MIN: return 15UL * 60UL * 1000UL;
    case LDUR_60MIN: return 60UL * 60UL * 1000UL;
    case LDUR_3H:    return 3UL  * 60UL * 60UL * 1000UL;
    case LDUR_6H:    return 6UL  * 60UL * 60UL * 1000UL;
    default:         return 0;
  }
}

static void renderLampOff()
{
  matrixFill(0, 0, 0);
}

static void renderLampScaled(uint8_t r, uint8_t g, uint8_t b, float scale)
{
  if (scale < 0.0f) scale = 0.0f;
  if (scale > 1.0f) scale = 1.0f;

  uint8_t rr = (uint8_t)(r * scale + 0.5f);
  uint8_t gg = (uint8_t)(g * scale + 0.5f);
  uint8_t bb = (uint8_t)(b * scale + 0.5f);

  matrixFill(rr, gg, bb);
}

static void renderLampManual(uint32_t nowMs)
{
  if (!g_lampManualActive) return;

  if (g_lampUntilMs && nowMs >= g_lampUntilMs)
  {
    g_lampManualActive = false;
    g_lampModeManual = LMODE_OFF;
    renderLampOff();
    return;
  }

  if (g_lampModeManual == LMODE_WHITE)
  {
    float scale = (g_lampValueManual >= 100) ? 1.0f : 0.5f;
    renderLampScaled(255, 255, 255, scale);
    return;
  }

  if (g_lampModeManual == LMODE_COLOR)
  {
    if (g_lampValueManual == LCOLOR_RAINBOW)
    {
      const uint16_t W = matrixWidth();
      const uint16_t H = matrixHeight();
      uint32_t t = nowMs - g_lampFxStartMs;

      for (uint16_t y = 0; y < H; ++y)
      {
        for (uint16_t x = 0; x < W; ++x)
        {
         uint8_t hue = (uint8_t)(((x + y) * 2 + t / 100) & 0xFF);
          CHSV hsv(hue, 255, 128);
          CRGB rgb;
          hsv2rgb_rainbow(hsv, rgb);
          matrixSetPixel((uint8_t)x, (uint8_t)y, rgb.g, rgb.r, rgb.b);
        }
      }
      return;
    }

    float phase = ((nowMs - g_lampFxStartMs) % 2600) / 2600.0f;
    float breath = 0.30f + 0.20f * (0.5f - 0.5f * cosf(phase * 2.0f * PI));

    uint8_t r = 0, g = 0, b = 0;
    switch (g_lampValueManual)
    {
      case LCOLOR_RED:     r = 255; g = 0;   b = 0;   break;
      case LCOLOR_GREEN:   r = 0;   g = 255; b = 0;   break;
      case LCOLOR_BLUE:    r = 0;   g = 0;   b = 255; break;
      case LCOLOR_YELLOW:  r = 255; g = 255; b = 0;   break;
      case LCOLOR_CYAN:    r = 0;   g = 255; b = 255; break;
      case LCOLOR_MAGENTA: r = 255; g = 0;   b = 255; break;
      default:
        renderLampOff();
        return;
    }

    renderLampScaled(r, g, b, breath);
    return;
  }

  renderLampOff();
}


void animsLampSet(uint8_t mode, uint8_t value, uint8_t duration)
{
  g_lampModeManual = mode;
  g_lampValueManual = value;
  g_lampFxStartMs = millis();

  if (mode == LMODE_OFF)
  {
    g_lampManualActive = false;
    g_lampUntilMs = 0;
    return;
  }

  g_lampManualActive = true;
  g_lampUntilMs = millis() + lampDurationToMs(duration);
}

void animsLampOff()
{
  g_lampManualActive = false;
  g_lampModeManual = LMODE_OFF;
  g_lampValueManual = 0;
  g_lampUntilMs = 0;
}

bool animsLampActive()
{
  return g_lampManualActive;
}



void animsOnGoal(uint8_t side, uint16_t durationMs) {
  g_goalSide = (side ? 1 : 0);
  g_goalDur  = durationMs;
  g_eventMs  = 0;                   // reset czasu efektu przy każdym golu
  animsSetMode(ANIM_GOAL);
  Serial.printf("[GOALFX] start side=%u dur=%u\n", (unsigned)g_goalSide, (unsigned)g_goalDur);
}

// ================= RENDERY =================

static void renderOff(){
  matrixFill(0,0,0);
}

static void renderRound(uint32_t nowMs) {
  float scale = 1.0f;

  if (g_breathing) {
    float t = (nowMs % 2600) / 2600.0f;   // ~2.6s
    scale = 0.35f + 0.65f * (0.5f - 0.5f * cosf(t * 2 * PI));
  }

  CRGB c = g_roundColor;
  c.nscale8_video((uint8_t)(scale * 255));

  matrixFill(c.r, c.g, c.b);
}


static void drawBar(int x0, int w, uint8_t r, uint8_t g, uint8_t b){
  const uint16_t W = matrixWidth();
  const uint16_t H = matrixHeight();
  for (uint16_t y=0; y<H; ++y){
    for (int dx=0; dx<w; ++dx){
      int x = x0 + dx;
      if (x < 0 || x >= (int)W) continue;
      matrixSetPixel((uint8_t)x, (uint8_t)y, r,g,b);
    }
  }
}




static void renderGoal(uint32_t now){
  if (g_eventMs == 0) g_eventMs = now;
  uint32_t dt = now - g_eventMs;

  // TYLKO czerwony/zielony split zależnie od strony gola.
  // Bez niebieskich/żółtych pasków i bez białego flasha.
  matrixGoalSplit(g_goalSide);

  uint32_t dur = (g_goalDur < 100) ? 100 : g_goalDur;
  if (dt >= dur){
    
    animsSetMode(ANIM_ROUND);
  }
}

static void renderGameOver(uint32_t now){
  if (g_eventMs == 0) g_eventMs = now;
  uint32_t dt = now - g_eventMs;

  // fade red down over 2s, then stay dark red
  uint8_t k = (dt < 2000) ? (uint8_t)(255 - (dt * 255) / 2000) : 0;
  matrixFill(k / 4, 0, 0);
}

void animsSetRoundColor(uint8_t r, uint8_t g, uint8_t b) {
  g_roundColor = CRGB(r, g, b);
}

void animsSetBreathing(bool enable) {
  g_breathing = enable;
}


void animsTick(uint32_t nowMs){
  static uint32_t last = 0;
  if (nowMs - last < 25) return; // ~40 fps max
  last = nowMs;

if (g_lampManualActive)
{
  renderLampManual(nowMs);
  matrixShow();
  return;
}

switch (g_mode){
  case ANIM_OFF:      renderOff();           break;
  case ANIM_ROUND:    renderRound(nowMs);    break;
  case ANIM_GOAL:     renderGoal(nowMs);     break;
  case ANIM_GAMEOVER: renderGameOver(nowMs); break;
  default:            renderOff();           break;
}

matrixShow();
}

AnimMode animsGetMode() {
    return g_mode;
}
