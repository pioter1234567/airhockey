#include "anims.h"
#include "led_matrix.h"
#include <Arduino.h> // millis()
#include <FastLED.h>

static AnimMode g_mode = ANIM_OFF;
static CRGB g_roundColor = CRGB::Black;
static bool g_breathing = false;
static uint32_t g_goalUntilMs = 0;

// GOAL state
static uint8_t  g_goalSide = 0;     // 0 = A, 1 = B
static uint32_t g_eventMs  = 0;
static uint16_t g_goalDur  = 500;   // ms

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

void animsOnGoal(uint8_t side, uint16_t durationMs) {
  g_goalSide = (side ? 1 : 0);
  g_goalDur  = durationMs;          // <-- to dodaj
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

  // Tło: pół na pół (zależnie od strony gola)
  matrixGoalSplit(g_goalSide);

  const uint16_t W = matrixWidth();
  // Animacja różna dla A i B:
  // - A: niebieski "wipe" lewo→prawo
  // - B: żółty "wipe" prawo→lewo
  const int barW = 6;

  // progress 0..(W+barW)
  uint32_t dur = (g_goalDur < 100) ? 100 : g_goalDur;
  int prog = (int)((dt * (uint32_t)(W + barW)) / dur);

  if (g_goalSide == 0){
    // A: start poza lewą krawędzią, jedzie w prawo
    int x0 = prog - barW;
    drawBar(x0, barW, 0, 0, 255);
  } else {
    // B: start poza prawą krawędzią, jedzie w lewo
    int x0 = (int)(W - 1) - prog;
    drawBar(x0, barW, 255, 255, 0);
  }

  // Krótki "flash" na koniec efektu (żeby był czytelny)
  if (dt + 80 >= dur){
    // 2 ostatnie klatki: rozjaśnij tylko połowę po stronie gola
    // (czytelny hint kierunku)
    const uint16_t H = matrixHeight();
    const uint16_t mid = W/2;
    for (uint16_t y=0; y<H; ++y){
      for (uint16_t x=0; x<W; ++x){
        bool left = (x < mid);
        bool goalOnLeft = (g_goalSide == 0); // możesz odwrócić jeśli A to prawa bramka
        if (goalOnLeft == left){
          matrixSetPixel((uint8_t)x,(uint8_t)y,255,255,255);
        }
      }
    }
  }

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
