#include "led_matrix.h"
#include <FastLED.h>

static const uint16_t W   = 64;
static const uint16_t H   = 16;
static const uint16_t NUM = W * H;
constexpr uint8_t MATRIX_DATA_PIN = 12;

static CRGB leds[NUM];


static inline uint16_t XY(uint8_t x, uint8_t y){
  if (x >= W || y >= H) return 0;
  // serpentine wierszami
  if (y & 1) return (uint16_t)y * W + (W - 1 - x);
  else       return (uint16_t)y * W + x;
}


static inline uint16_t XY_linear(uint8_t x, uint8_t y){
  if (x >= W || y >= H) return 0;
  return (uint16_t)y * W + x; // bez serpentine
}

void matrixSetPixelLinear(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b){
  leds[XY_linear(x,y)] = CRGB(g, r, b); // zostawiamy Twój swap jak wszędzie
}



void matrixInit(uint8_t brightness){
  
  

  // Najstabilniejsze na ESP32 przy WS281x: NEOPIXEL backend
  // (WS2815 używa tego samego 800kHz 1-wire co WS2812/WS2813)
 FastLED.addLeds<WS2812B, MATRIX_DATA_PIN, GRB>(leds, NUM);

  FastLED.setBrightness(brightness);

  // Redukuje “dziwne migotki” na przejściach
  FastLED.setDither(false);
  FastLED.setCorrection(UncorrectedColor);
  FastLED.setTemperature(UncorrectedTemperature);

  fill_solid(leds, NUM, CRGB::Black);
  FastLED.show();
}

void matrixClear(){
  fill_solid(leds, NUM, CRGB::Black);
}

void matrixShow(){
  FastLED.show();
}

void matrixSetPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b){
  leds[XY(x,y)] = CRGB(g, r, b); 
}

void matrixSetIndex(uint16_t idx, uint8_t r, uint8_t g, uint8_t b){
  // idx = fizyczny indeks LED (0..1023)
  if (idx >= NUM) return;
  leds[idx] = CRGB(g, r, b); // zostawiam Twój swap R<->G jak masz wszędzie
}

void matrixFill(uint8_t r, uint8_t g, uint8_t b){
  fill_solid(leds, NUM, CRGB(g, r, b));
}

static inline CRGB RGBx(uint8_t r, uint8_t g, uint8_t b){
  return CRGB(g, r, b); // swap R<->G
}


// === GOAL: 512/512 po INDEKSIE LED ===
// side: 0 = GOAL A -> 0..511 GREEN, 512..1023 RED
//       1 = GOAL B -> 0..511 RED,   512..1023 GREEN
void matrixGoalSplit(uint8_t side){
  const uint16_t HALF = NUM / 2; // 512 przy 64x16

  for (uint16_t i = 0; i < NUM; i++){
    const bool firstHalf = (i < HALF);

    if (side == 0){
            leds[i] = firstHalf ? RGBx(0,255,0) : RGBx(255,0,0);

    } else {
            leds[i] = firstHalf ? RGBx(255,0,0) : RGBx(0,255,0);

    }
  }
  // NIE rób FastLED.show() tutaj – show tylko przez matrixShow()
}

uint16_t matrixWidth(){  return W; }
uint16_t matrixHeight(){ return H; }
