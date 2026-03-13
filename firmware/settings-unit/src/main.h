#pragma once

#include <Arduino.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>

#define BTN_UP     10
#define BTN_DOWN   5
#define BTN_SELECT 33
#define BTN_BACK   9

struct Button {
  uint8_t pin;
  bool    stableState;
  bool    lastRead;
  uint32_t lastChangeMs;
  bool    armed;
  uint32_t repeatAtMs;   // 0 = brak auto-repeat; >0 = timestamp następnego kroku
};

// Piny TFT (ESP32)
#define TFT_CS   13 //rev1.3
#define TFT_DC   16 //rev1.3
#define TFT_RST  -1  //rev1.3 (pullup do 3v3 przez 10k)
#define TFT_SCLK 18 //rev1.3
#define TFT_MOSI 8  //rev 1.3
#define LCD_PWR  11  //rev1.3



extern Adafruit_ST7735 tft;
extern Preferences gamePrefs;

extern bool screenDimmed;
extern unsigned long lastInputTime;

extern Button bUp;
extern Button bDown;
extern Button bSelect;
extern Button bBack;

extern bool comboReady;

void canPlayTetrisMusic();
void canStopMinigameMusic();

bool pressedNow(Button &b);
void redraw();



#define SNAKE_ORANGE 0xFD20