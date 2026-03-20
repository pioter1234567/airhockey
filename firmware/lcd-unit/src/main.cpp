#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FS.h>
#include <LittleFS.h>
#include <LovyanGFX.hpp>
#include "driver/twai.h"
#include <string.h>
#include "anims.h"
#include "Rayman_Origins72pt7b.h"
#include "display.h"

// --- OLEDy (Adafruit) ---

#define SDA1 8
#define SCL1 7

#define SDA2 16
#define SCL2 15

#define OLED_ADDR 0x3C

//SD

#define SD_CS   34   // SCS
#define SD_MOSI 35   // SDI
#define SD_MISO 37   // SDO
#define SD_SCK  36   // CLK

TwoWire I2C2 = TwoWire(1); // Wire1
Adafruit_SH1106G oled1(128, 64, &Wire, -1);
Adafruit_SH1106G oled2(128, 64, &I2C2, -1);

// --- XBM 64x128 ---
#include "d0.xbm"
#include "d1.xbm"
#include "d2.xbm"
#include "d3.xbm"
#include "d4.xbm"
#include "d5.xbm"
#include "d6.xbm"
#include "d7.xbm"
#include "d8.xbm"
#include "d9.xbm"

struct DigitBmp
{
  const unsigned char *bits;
  uint16_t w, h;
};
const DigitBmp DIGITS[10] = {
    {d0_bits, d0_width, d0_height}, {d1_bits, d1_width, d1_height}, {d2_bits, d2_width, d2_height}, {d3_bits, d3_width, d3_height}, {d4_bits, d4_width, d4_height}, {d5_bits, d5_width, d5_height}, {d6_bits, d6_width, d6_height}, {d7_bits, d7_width, d7_height}, {d8_bits, d8_width, d8_height}, {d9_bits, d9_width, d9_height}};

static void showDigit(Adafruit_SH1106G &disp, uint8_t d)
{
  const DigitBmp &B = DIGITS[d % 10];
  disp.clearDisplay();
  disp.drawXBitmap(0, 0, B.bits, B.w, B.h, SH110X_WHITE);
  disp.display();
}

// ======================= TFT (ILI9488 + LovyanGFX) =======================

// Piny TFT
#define TFT_CS 10
#define TFT_DC 14
#define TFT_RST 21
#define TFT_MOSI 11
#define TFT_MISO -1 // jeśli Twój moduł nie ma SDO/MISO → 3-wire
#define TFT_SCK 12
#define TFT_BL 4


#define TFT_BL_PWM_CH   0
#define TFT_BL_PWM_FREQ 5000
#define TFT_BL_PWM_RES  8

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF

#define CAN_TX_PIN GPIO_NUM_17
#define CAN_RX_PIN GPIO_NUM_18

#define CAN_ID_START_GAME 0x301
#define CAN_CMD_START_GAME 0x01

#define CAN_ID_SCORE_UPDATE 0x321 // [scoreA, scoreB]
#define CAN_ID_GOAL_ANIM 0x322    // [side(0=A,1=B), type]
#define CAN_ID_GAME_EVENT 0x323   // [evt, a, b]
#define CAN_ID_ROUND_TIME 0x324   // [sec_hi, sec_lo]


LGFX_ILI9488 tft;


SPIClass sdSpi(FSPI);




static constexpr int CLOCK_SLOTS = 4;

//sprity slotow zegara

lgfx::LGFX_Sprite g_clockSlotBase[CLOCK_SLOTS] = {
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft)
};

lgfx::LGFX_Sprite g_clockSlotWork[CLOCK_SLOTS] = {
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft),
  lgfx::LGFX_Sprite(&tft)
};

// clock

static bool g_clockActive = false;
static uint32_t g_clockEndMs = 0;
static uint16_t g_clockLastShownSec = 0xFFFF;
static bool g_fontLoaded = false;
static bool g_clockHideScheduled = false;
static uint32_t g_clockHideAtMs = 0;

static String g_clockLastText = "";
static int g_clockX = 240;
static int g_clockY = 160;
static int g_clockW = 36;   // dobierzemy
static int g_clockH = 80;

static int g_clockCharGap = 0;      // dodatkowy odstęp między znakami
static int g_clockSlotW   = 55;     // szerokość slotu na jeden znak
static int g_clockSlotH   = 80;     // wysokość slotu
static int g_clockPadX    = 10;      // zapas tła po bokach
static int g_clockPadY    = 4;      // zapas tła góra/dół

static int g_clockStartX = 0;
static int g_clockStartY = 0;

static int g_clockAreaX = 110;
static int g_clockAreaY = 95;
static int g_clockAreaW = 260;
static int g_clockAreaH = 120;




const char *g_currentBgPath = "/test.bin";



// ======================= CAN / SCOREBOARD =======================

static twai_general_config_t g_can =
    TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
static twai_timing_config_t t_can = TWAI_TIMING_CONFIG_250KBITS();
static twai_filter_config_t f_can = TWAI_FILTER_CONFIG_ACCEPT_ALL();

static uint8_t g_scoreA = 0;
static uint8_t g_scoreB = 0;
static uint8_t g_lastGoalSide = 255; // 0=A, 1=B, 255=none
static uint8_t g_lastGoalType = 0;   // 1=normal, 2=music
static uint8_t g_lastEvent = 255;
static uint8_t g_eventA = 0;
static uint8_t g_eventB = 0;

static uint8_t g_gameType = 0;  // z 0x301
static uint8_t g_gameIndex = 0; // z 0x301




//

static bool g_gameStarted = false;
static bool g_returnToIdleAfterClockHide = false;
static uint8_t g_roundNo = 0;

// evt: 0=GameStart, 1=RoundStart, 2=RoundEnd, 3=GameOver, 4=CountdownTick
// goal type: 1=normal round, 2=music round

static bool canInit()
{

  // konfiguracja ogólna
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      CAN_TX_PIN,
      CAN_RX_PIN,
      TWAI_MODE_NORMAL);

  // timing – taki sam jak w main-unit
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();

  // filtr – przyjmujemy wszystko
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
  {
    Serial.println("[CAN] driver install FAILED");
    return false;
  }

  if (twai_start() != ESP_OK)
  {
    Serial.println("[CAN] start FAILED");
    return false;
  }

  Serial.println("[CAN] started @250kbps");

  return true;
}

bool drawRgb565File(const char* path, int w, int h)
{
  File f = SD.open(path, FILE_READ);
  if (!f)
  {
    Serial.printf("[RGB565] open FAIL: %s\n", path);
    return false;
  }

  const size_t bytesNeeded = (size_t)w * h * 2;
  uint8_t* buf = (uint8_t*)ps_malloc(bytesNeeded);
  if (!buf)
  {
    Serial.println("[RGB565] ps_malloc FAIL");
    f.close();
    return false;
  }

  size_t rd = f.read(buf, bytesNeeded);
  f.close();

  if (rd != bytesNeeded)
  {
    Serial.printf("[RGB565] read FAIL: got %u need %u\n",
                  (unsigned)rd, (unsigned)bytesNeeded);
    free(buf);
    return false;
  }

  tft.pushImage(0, 0, w, h, (uint16_t*)buf);
  free(buf);
  return true;
}

static bool loadRgb565RegionToSprite(lgfx::LGFX_Sprite& spr,
                                     const char* path,
                                     int imgW, int imgH,
                                     int srcX, int srcY,
                                     int regionW, int regionH)
{
  if (srcX < 0 || srcY < 0) return false;
  if (srcX + regionW > imgW) return false;
  if (srcY + regionH > imgH) return false;

  File f = SD.open(path, FILE_READ);
  if (!f)
  {
    Serial.printf("[RGB565] open FAIL: %s\n", path);
    return false;
  }

  uint16_t* lineBuf = (uint16_t*)heap_caps_malloc(regionW * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!lineBuf)
  {
    Serial.println("[RGB565] lineBuf alloc FAIL");
    f.close();
    return false;
  }

  for (int y = 0; y < regionH; y++)
  {
    size_t offset = ((size_t)(srcY + y) * imgW + srcX) * 2;
    if (!f.seek(offset))
    {
      Serial.printf("[RGB565] seek FAIL at y=%d\n", y);
      free(lineBuf);
      f.close();
      return false;
    }

    size_t need = regionW * 2;
    size_t rd = f.read((uint8_t*)lineBuf, need);
    if (rd != need)
    {
      Serial.printf("[RGB565] row read FAIL at y=%d got=%u need=%u\n",
                    y, (unsigned)rd, (unsigned)need);
      free(lineBuf);
      f.close();
      return false;
    }

    spr.pushImage(0, y, regionW, 1, lineBuf);
  }

  free(lineBuf);
  f.close();
  return true;
}

static bool restoreRgb565RegionToScreen(const char* path,
                                        int imgW, int imgH,
                                        int srcX, int srcY,
                                        int dstX, int dstY,
                                        int regionW, int regionH)
{
  File f = SD.open(path, FILE_READ);
  if (!f)
  {
    Serial.printf("[RGB565] open FAIL: %s\n", path);
    return false;
  }

  uint16_t* lineBuf = (uint16_t*)heap_caps_malloc(regionW * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!lineBuf)
  {
    Serial.println("[RGB565] lineBuf alloc FAIL");
    f.close();
    return false;
  }

  for (int y = 0; y < regionH; y++)
  {
    size_t offset = ((size_t)(srcY + y) * imgW + srcX) * 2;
    if (!f.seek(offset))
    {
      Serial.printf("[RGB565] seek FAIL at y=%d\n", y);
      free(lineBuf);
      f.close();
      return false;
    }

    size_t need = regionW * 2;
    size_t rd = f.read((uint8_t*)lineBuf, need);
    if (rd != need)
    {
      Serial.printf("[RGB565] row read FAIL at y=%d got=%u need=%u\n",
                    y, (unsigned)rd, (unsigned)need);
      free(lineBuf);
      f.close();
      return false;
    }

    tft.pushImage(dstX, dstY + y, regionW, 1, lineBuf);
  }

  free(lineBuf);
  f.close();
  return true;
}




static void updateScoreDisplay()
{

  if (!g_gameStarted)
  {
    oled1.clearDisplay();
    oled1.display();

    oled2.clearDisplay();
    oled2.display();
    return;
  }

  // ===== A =====
  oled1.clearDisplay();

if (g_scoreA <= 9) {
  showDigit(oled1, g_scoreA);
} else {
  oled1.clearDisplay();
  oled1.setTextWrap(false);
  oled1.setTextSize(6);
  oled1.setTextColor(SH110X_WHITE);

  String s = String(g_scoreA);

  int16_t x1, y1;
  uint16_t w, h;
  oled1.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);

  int x = (oled1.width()  - w) / 2 - x1 + 2;
  int y = (oled1.height() - h) / 2 - y1;

  oled1.setCursor(x, y);
  oled1.print(s);
  oled1.display();
}

// ===== B =====
oled2.clearDisplay();

if (g_scoreB <= 9) {
  showDigit(oled2, g_scoreB);
} else {
  oled2.setTextWrap(false);
  oled2.setTextSize(6);
  oled2.setTextColor(SH110X_WHITE);

  String s = String(g_scoreB);

  int16_t x1, y1;
  uint16_t w, h;
  oled2.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);

  int x = (oled1.width()  - w) / 2 - x1 + 2;
  int y = (oled2.height() - h) / 2 - y1;

  oled2.setCursor(x, y);
  oled2.print(s);
  oled2.display();
}

}

static const char *bgPathForGame(uint8_t gameType, uint8_t gameIndex)
{
  // na razie przykładowe mapowanie
  // dopasujesz sobie nazwy plików jak wrzucisz JPG do LittleFS

  if (gameType == 1) // GT_FULL
  {
    switch (gameIndex)
    {
case 0: return "/teensies.bin";
case 1: return "/toad.bin";
case 2: return "/fiesta.bin";
case 3: return "/20000.bin";
case 4: return "/olympus.bin";
case 5: return "/grannies.bin";
default: return "/test.bin";
    }
  }

  if (gameType == 2) // GT_MUSIC
{
  switch (gameIndex)
  {
case 0: return "/teensies.bin";
case 1: return "/toad.bin";
case 2: return "/fiesta.bin";
case 3: return "/20000.bin";
case 4: return "/olympus.bin";
case 5: return "/grannies.bin";
default: return "/test.bin";
  }
}

  return "/test.bin";
}


static int clockSlotX(int idx);
static int clockSlotY();
static void setBacklight(uint8_t value);
static void fadeBacklight(uint8_t from, uint8_t to, uint16_t durationMs);
static bool rebuildClockSlotCaches()
{
  for (int i = 0; i < CLOCK_SLOTS; i++)
  {
    if (g_clockSlotBase[i].getBuffer() != nullptr)
      g_clockSlotBase[i].deleteSprite();
    if (g_clockSlotWork[i].getBuffer() != nullptr)
      g_clockSlotWork[i].deleteSprite();
  }

  int totalW = CLOCK_SLOTS * g_clockSlotW + (CLOCK_SLOTS - 1) * g_clockCharGap;
  int totalH = g_clockSlotH;

  g_clockStartX = (tft.width()  - totalW) / 2;
  g_clockStartY = (tft.height() - totalH) / 2;

  for (int i = 0; i < CLOCK_SLOTS; i++)
  {
    g_clockSlotBase[i].setColorDepth(16);
    g_clockSlotBase[i].setPsram(true);

    if (!g_clockSlotBase[i].createSprite(g_clockSlotW, g_clockSlotH))
    {
      Serial.printf("[CLOCK] base slot %d create FAIL\n", i);
      return false;
    }

    g_clockSlotBase[i].fillScreen(C_BLACK);

    int x = clockSlotX(i);
    int y = clockSlotY();

    bool ok = false;

    if (SD.cardType() != CARD_NONE && SD.exists(g_currentBgPath))
    {
      ok = loadRgb565RegionToSprite(
        g_clockSlotBase[i],
        g_currentBgPath,
        480, 320,
        x, y,
        g_clockSlotW, g_clockSlotH
      );
    }

    if (!ok)
    {
      Serial.printf("[CLOCK] base slot %d bin FAIL -> fallback BLACK\n", i);
      g_clockSlotBase[i].fillScreen(C_BLACK);
    }

    g_clockSlotWork[i].setColorDepth(16);
    g_clockSlotWork[i].setPsram(true);

    if (!g_clockSlotWork[i].createSprite(g_clockSlotW, g_clockSlotH))
    {
      Serial.printf("[CLOCK] work slot %d create FAIL\n", i);
      return false;
    }

    if (LittleFS.exists("/Rayman72.vlw"))
    {
      g_clockSlotWork[i].loadFont(LittleFS, "/Rayman72.vlw");
    }
  }

  g_clockLastText = "";
  return true;
}






static int clockSlotX(int idx)
{
  return g_clockStartX + idx * (g_clockSlotW + g_clockCharGap);
}

static int clockSlotY()
{
  return g_clockStartY;
}





static void applyGameBackground()
{
  const char *newPath = bgPathForGame(g_gameType, g_gameIndex);
  g_currentBgPath = newPath;

  bool ok = false;

  if (SD.cardType() != CARD_NONE && SD.exists(g_currentBgPath))
  {
    fadeBacklight(255, 0, 120);
ok = drawRgb565File(g_currentBgPath, 480, 320);
fadeBacklight(0, 255, 140);
  }

  if (!ok)
  {
    Serial.printf("[BG] draw %s FAIL -> fallback BLACK\n", g_currentBgPath);
    tft.fillScreen(C_BLACK);
  }
  else
  {
    Serial.printf("[BG] draw %s -> OK\n", g_currentBgPath);
  }

  if (!rebuildClockSlotCaches())
  {
    Serial.println("[CLOCK] cache rebuild FAIL, but continue");
  }

  g_clockLastText = "";
}

static void drawClockCharToSlot(int idx, char ch)
{
  if (idx < 0 || idx >= CLOCK_SLOTS)
    return;

  if (g_clockSlotBase[idx].getBuffer() == nullptr || g_clockSlotWork[idx].getBuffer() == nullptr)
    return;

  // skopiuj tło slotu do roboczego sprite'a
  g_clockSlotWork[idx].pushImage(
    0, 0,
    g_clockSlotW, g_clockSlotH,
    (uint16_t*)g_clockSlotBase[idx].getBuffer()
  );

  char buf[2] = { ch, 0 };

  int charW = g_clockSlotWork[idx].textWidth(buf);
  int charH = g_clockSlotWork[idx].fontHeight();

  int drawX = (g_clockSlotW - charW) / 2;
  int drawY = (g_clockSlotH - charH) / 2;

  g_clockSlotWork[idx].setTextDatum(top_left);

  g_clockSlotWork[idx].setTextColor(0x4208);
  g_clockSlotWork[idx].drawString(buf, drawX + 3, drawY + 3);

  g_clockSlotWork[idx].setTextColor(0x8410);
  g_clockSlotWork[idx].drawString(buf, drawX + 1, drawY + 1);

  g_clockSlotWork[idx].setTextColor(C_WHITE);
  g_clockSlotWork[idx].drawString(buf, drawX, drawY);

  g_clockSlotWork[idx].pushSprite(clockSlotX(idx), clockSlotY());
}










static void drawClockText(const String &s)
{
  if (s.length() != CLOCK_SLOTS)
    return;

  if (g_clockLastText.length() != CLOCK_SLOTS)
  {
    for (int i = 0; i < CLOCK_SLOTS; i++)
      drawClockCharToSlot(i, s[i]);

    g_clockLastText = s;
    return;
  }

  for (int i = 0; i < CLOCK_SLOTS; i++)
  {
    if (s[i] != g_clockLastText[i])
      drawClockCharToSlot(i, s[i]);
  }

  g_clockLastText = s;
}


static void drawClockSeconds(uint16_t totalSec)
{
  uint16_t mm = totalSec / 60;
  uint16_t ss = totalSec % 60;

  char buf[5];
  snprintf(buf, sizeof(buf), "%1u:%02u", mm, ss);

  drawClockText(String(buf));
}

static void startClock(uint16_t secs)
{
  g_clockActive = true;
    g_clockHideScheduled = false;
  g_clockHideAtMs = 0;
  g_clockEndMs = millis() + (uint32_t)secs * 1000UL;
  g_clockLastShownSec = 0xFFFF;
  g_clockLastText = "";
  drawClockSeconds(secs);
  Serial.printf("[CLOCK] start %u s\n", secs);
}

static void stopClock()
{
  g_clockActive = false;
  Serial.println("[CLOCK] stop");
}

static void clockTick()
{
  if (!g_clockActive)
    return;

  uint32_t now = millis();
  int32_t remMs = (int32_t)(g_clockEndMs - now);

  uint16_t leftSec;
  if (remMs > 0)
    leftSec = (uint16_t)((remMs + 999) / 1000UL);  // klasyczne zaokrąglenie w górę
  else
    leftSec = 0;

  if (leftSec != g_clockLastShownSec)
  {
    g_clockLastShownSec = leftSec;
    drawClockSeconds(leftSec);
  }

  if (remMs <= 0)
  {
    // dopiero po faktycznym upływie czasu zatrzymaj i zaplanuj schowanie
    g_clockLastText = "";
    g_clockLastShownSec = 0;
    drawClockText("0:00");

    g_clockActive = false;
    g_clockHideScheduled = true;
    g_clockHideAtMs = now + 5000;
  }
}

static void clearClockArea()
{
  int totalW = CLOCK_SLOTS * g_clockSlotW + (CLOCK_SLOTS - 1) * g_clockCharGap;
  int totalH = g_clockSlotH;

  int x = (tft.width()  - totalW) / 2;
  int y = (tft.height() - totalH) / 2;

  bool ok = false;

  if (SD.cardType() != CARD_NONE && SD.exists(g_currentBgPath))
  {
    ok = restoreRgb565RegionToScreen(
      g_currentBgPath,
      480, 320,
      x, y,
      x, y,
      totalW, totalH
    );
  }

  if (!ok)
  {
    tft.fillRect(x, y, totalW, totalH, C_BLACK);
  }
}


static void onScoreChanged()
{
  Serial.printf("[SCORE] %u:%u\n", g_scoreA, g_scoreB);
  updateScoreDisplay();
}

static void onGoalAnim(uint8_t side, uint8_t type)
{
  g_lastGoalSide = side;
  g_lastGoalType = type;

  Serial.printf("[GOAL_ANIM] side=%s type=%u\n", side == 0 ? "A" : "B", type);

  // TODO:
  // tutaj możesz potem zrobić flash / grafikę / overlay na TFT
}

static void onGameEvent(uint8_t evt, uint8_t a, uint8_t b)
{
  g_lastEvent = evt;
  g_eventA = a;
  g_eventB = b;

  Serial.printf("[GAME_EVENT] evt=%u a=%u b=%u\n", evt, a, b);

  switch (evt)
  {
  case 0: // GameStart
    Serial.println("[GAME] start");
    break;

case 1: // RoundStart
  g_gameStarted = true;
  g_roundNo = a;
  Serial.printf("[ROUND] start #%u\n", a);
  updateScoreDisplay();
  break;

case 2: // RoundEnd
{
  Serial.printf("[ROUND] end score=%u:%u\n", a, b);

  uint32_t now = millis();
  int32_t remMs = (int32_t)(g_clockEndMs - now);

  // jeśli jesteśmy już w ostatniej sekundzie albo już było 0,
  // to traktujemy to jako koniec przez czas
  bool endedByTime = (g_clockLastShownSec == 0) || (remMs <= 999);

if (endedByTime)
{
  g_clockLastText = "";
  g_clockLastShownSec = 0;
  drawClockText("0:00");

  g_clockActive = false;
  g_clockHideScheduled = true;
  g_clockHideAtMs = now + 5000;

  bool isStandaloneMusic = (g_gameType == 2);
  bool isFinalRoundOfFullGame = (g_gameType == 1 && g_roundNo >= 3);

  g_returnToIdleAfterClockHide = (isStandaloneMusic || isFinalRoundOfFullGame);
}
  else
  {
    // koniec przez punkty -> zostaw to co jest na ekranie
    g_clockActive = false;
    g_clockHideScheduled = false;
    g_clockHideAtMs = 0;
  }

  break;
}

case 3: // GameOver
  Serial.println("[GAME] OVER");

  g_gameStarted = false;
  stopClock();

  // jeśli powrót do idle już jest zaplanowany po schowaniu zegara,
  // to tutaj nie rób nic więcej
  if (g_returnToIdleAfterClockHide)
  {
    updateScoreDisplay();
    break;
  }

  // jeśli już jesteśmy na test.bin, też nic nie rób
  if (strcmp(g_currentBgPath, "/test.bin") == 0)
  {
    updateScoreDisplay();
    break;
  }

  g_clockLastText = "";
  g_returnToIdleAfterClockHide = false;
  g_currentBgPath = "/test.bin";

  if (SD.cardType() != CARD_NONE && SD.exists(g_currentBgPath))
  {
    fadeBacklight(255, 0, 120);
    bool ok = drawRgb565File(g_currentBgPath, 480, 320);
    fadeBacklight(0, 255, 140);

    Serial.printf("[BG] game over -> %s: %s\n",
                  g_currentBgPath,
                  ok ? "OK" : "FAIL");

    if (ok)
      rebuildClockSlotCaches();
    else
      tft.fillScreen(C_BLACK);
  }
  else
  {
    Serial.println("[BG] game over -> /test.bin missing on SD");
    tft.fillScreen(C_BLACK);
  }

  updateScoreDisplay();
  break;

  case 4:                                 // CountdownTick
    Serial.printf("[COUNTDOWN] %u\n", a); // a = 3,2,1
    break;
  }
}

static void handleCanFrame(const twai_message_t &msg)
{
  if (msg.extd || msg.rtr)
    return;

  switch (msg.identifier)
  {

case CAN_ID_START_GAME:
  if (msg.data_length_code == 3 && msg.data[0] == CAN_CMD_START_GAME)
  {
    g_gameType = msg.data[1];
    g_gameIndex = msg.data[2];
    g_gameStarted = true;

    Serial.printf("[START] type=%u index=%u\n", g_gameType, g_gameIndex);

applyGameBackground();
g_clockLastText = "";

g_clockHideScheduled = false;
g_clockHideAtMs = 0;

updateScoreDisplay();
  }
  else
  {
    Serial.printf("[WARN] bad START frame dlc=%d\n", msg.data_length_code);
  }
  break;

  case CAN_ID_SCORE_UPDATE:
    if (msg.data_length_code == 2)
    {
      g_scoreA = msg.data[0];
      g_scoreB = msg.data[1];

      g_gameStarted = true;

      Serial.printf("[SCORE] %u:%u\n", g_scoreA, g_scoreB);
      updateScoreDisplay();
    }
    else
    {
      Serial.printf("[WARN] bad SCORE frame dlc=%d\n", msg.data_length_code);
    }
    break;

  case CAN_ID_GOAL_ANIM:
    if (msg.data_length_code == 2)
    {
      onGoalAnim(msg.data[0], msg.data[1]);
    }
    else
    {
      Serial.printf("[WARN] bad GOAL_ANIM frame dlc=%d\n", msg.data_length_code);
    }
    break;

  case CAN_ID_GAME_EVENT:
    if (msg.data_length_code == 3)
    {
      onGameEvent(msg.data[0], msg.data[1], msg.data[2]);
    }
    else
    {
      Serial.printf("[WARN] bad GAME_EVENT frame dlc=%d\n", msg.data_length_code);
    }
    break;

      case CAN_ID_ROUND_TIME:
    if (msg.data_length_code == 2)
    {
      uint16_t secs = ((uint16_t)msg.data[0] << 8) | msg.data[1];
      Serial.printf("[ROUND_TIME] %u s\n", secs);
      startClock(secs);
    }
    else
    {
      Serial.printf("[WARN] bad ROUND_TIME frame dlc=%d\n", msg.data_length_code);
    }
    break;

  default:
    Serial.printf("[CAN] unhandled id=0x%03X\n", msg.identifier);
    break;
  }
}

static void applyScore(uint8_t a, uint8_t b)
{
  g_scoreA = a;
  g_scoreB = b;
  onScoreChanged();
}

static void handleSerialCommand(char *line)
{
  while (*line == ' ')
    line++;

  if (*line == 0)
    return;

  if (strcmp(line, "start") == 0)
  {
    g_gameStarted = true;
    Serial.println("[SERIAL] game started");
    updateScoreDisplay();
    return;
  }

  if (strcmp(line, "stop") == 0)
  {
    g_gameStarted = false;
    Serial.println("[SERIAL] game stopped");
    updateScoreDisplay();
    return;
  }

  if (strcmp(line, "reset") == 0)
  {
    g_scoreA = 0;
    g_scoreB = 0;
    Serial.println("[SERIAL] reset 0:0");
    updateScoreDisplay();
    return;
  }

if (strcmp(line, "anim") == 0)
{
  animsStartTest();
  return;
}

if (strcmp(line, "anim stop") == 0)
{
  animsStop();
  return;
}

  if (strcmp(line, "goal a") == 0)
  {
    if (g_scoreA < 99)
      g_scoreA++;
    Serial.printf("[SERIAL] goal A -> %u:%u\n", g_scoreA, g_scoreB);
    updateScoreDisplay();
    return;
  }

  if (strcmp(line, "goal b") == 0)
  {
    if (g_scoreB < 99)
      g_scoreB++;
    Serial.printf("[SERIAL] goal B -> %u:%u\n", g_scoreA, g_scoreB);
    updateScoreDisplay();
    return;
  }

  int a, b;
  if (sscanf(line, "s %d %d", &a, &b) == 2)
  {
    if (a < 0)
      a = 0;
    if (b < 0)
      b = 0;
    if (a > 99)
      a = 99;
    if (b > 99)
      b = 99;

    g_gameStarted = true;
    applyScore((uint8_t)a, (uint8_t)b);
    Serial.printf("[SERIAL] score set -> %u:%u\n", g_scoreA, g_scoreB);
    return;
  }

  Serial.printf("[SERIAL] unknown command: %s\n", line);
  Serial.println("[SERIAL] commands: start | stop | reset | anim | anim stop | goal a | goal b | s <A> <B>");
}

static void handleSerialInjection()
{
  static char buf[64];
  static size_t pos = 0;

  while (Serial.available())
  {
    char c = (char)Serial.read();

    if (c == '\r')
      continue;

    if (c == '\n')
    {
      buf[pos] = 0;
      handleSerialCommand(buf);
      pos = 0;
      continue;
    }

    if (pos < sizeof(buf) - 1)
    {
      buf[pos++] = c;
    }
  }
}

void listFiles() {
  File root = LittleFS.open("/");
  File file = root.openNextFile();

  while (file) {
    Serial.print("FILE: ");
    Serial.print(file.name());
    Serial.print("  SIZE: ");
    Serial.println(file.size());
    file = root.openNextFile();
  }
  
}


void listSdFiles()
{
  File root = SD.open("/");
  if (!root || !root.isDirectory())
  {
    Serial.println("[SD] open root FAIL");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    Serial.print("[SD] FILE: ");
    Serial.print(file.name());
    Serial.print("  SIZE: ");
    Serial.println(file.size());
    file = root.openNextFile();
  }
}


static bool sdInit()
{
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sdSpi))
  {
    Serial.println("[SD] mount FAIL");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE)
  {
    Serial.println("[SD] no card");
    return false;
  }

  Serial.print("[SD] card type: ");
  switch (cardType)
  {
    case CARD_MMC:  Serial.println("MMC"); break;
    case CARD_SD:   Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC/SDXC"); break;
    default:        Serial.println("UNKNOWN"); break;
  }

  uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] size: %llu MB\n", cardSizeMB);

  listSdFiles();
  return true;
}



static void setBacklight(uint8_t value)
{
  ledcWrite(TFT_BL_PWM_CH, value);
}

static void fadeBacklight(uint8_t from, uint8_t to, uint16_t durationMs)
{
  const int steps = 24;
  int delta = (int)to - (int)from;

  for (int i = 0; i <= steps; i++)
  {
    uint8_t v = from + (delta * i) / steps;
    ledcWrite(TFT_BL_PWM_CH, v);
    delay(durationMs / steps);
  }
}

void setup()
{
  Serial.begin(115200);

  // --- OLEDy ---
  Wire.begin(SDA1, SCL1);
  I2C2.begin(SDA2, SCL2);
  oled1.begin(OLED_ADDR, true);
  oled2.begin(OLED_ADDR, true);
  oled1.setRotation(1);
  oled2.setRotation(1);

  oled1.clearDisplay();
  oled1.display();
  oled2.clearDisplay();
  oled2.display();

  // --- TFT ---
ledcSetup(TFT_BL_PWM_CH, TFT_BL_PWM_FREQ, TFT_BL_PWM_RES);
ledcAttachPin(TFT_BL, TFT_BL_PWM_CH);
ledcWrite(TFT_BL_PWM_CH, 0);   // start z wygaszonym podświetleniem

  if (!tft.begin())
  {
    Serial.println("LGFX: init FAILED");
    while (true)
      delay(1000);
  }
  tft.setRotation(1);
  tft.setBrightness(255);
  tft.fillScreen(C_BLACK);

  // --- littlefs + JPG ---
if (!LittleFS.begin()) {
  Serial.println("LittleFS mount FAIL");
} else {
  Serial.println("LittleFS mount OK");
  listFiles();
}


//SD init

if (!sdInit())
{
  Serial.println("[SD] init failed");
}
else
{
  Serial.println("[SD] init OK");
}


bool haveTestBg = false;

if (SD.cardType() == CARD_NONE)
{
  Serial.println("SD nie dziala - start na czarnym tle");
}
else if (!SD.exists("/test.bin"))
{
  Serial.println("Brak /test.bin na SD - start na czarnym tle");
}
else
{
  haveTestBg = true;
}



  Serial.printf("PSRAM size: %u\n", ESP.getPsramSize());
  Serial.printf("Free PSRAM: %u\n", ESP.getFreePsram());

bool ok = false;

if (haveTestBg)
{
  g_currentBgPath = "/test.bin";
  fadeBacklight(255, 0, 120);
ok = drawRgb565File(g_currentBgPath, 480, 320);
fadeBacklight(0, 255, 140);
  Serial.printf("drawRgb565File: %s\n", ok ? "OK" : "FAIL");
}
else
{
  g_currentBgPath = "";
  tft.fillScreen(C_BLACK);
  Serial.println("[BG] fallback BLACK");
}

rebuildClockSlotCaches();
g_clockLastText = "";

ledcWrite(TFT_BL_PWM_CH, 255); // pełna jasność

  if (!canInit())
  {
    while (true)
      delay(1000);
  }

  if (!LittleFS.exists("/Rayman72.vlw")) {
    Serial.println("FONT FILE NOT FOUND");
  } else {
    g_fontLoaded = tft.loadFont(LittleFS, "/Rayman72.vlw");
    Serial.printf("loadFont: %s\n", g_fontLoaded ? "OK" : "FAIL");

  }
 animsInit();
  updateScoreDisplay();
}

void loop()
{
  handleSerialInjection();
  animsTick();
  twai_message_t msg;

  while (twai_receive(&msg, 0) == ESP_OK)
  {
    Serial.printf("[CAN] RX id=0x%03X dlc=%d data:",
                  msg.identifier,
                  msg.data_length_code);

    for (int i = 0; i < msg.data_length_code; i++)
    {
      Serial.printf(" %02X", msg.data[i]);
    }

    Serial.println();

    handleCanFrame(msg);
  }

  clockTick();
 
if (g_clockHideScheduled)
{
  if ((int32_t)(millis() - g_clockHideAtMs) >= 0)
  {
    g_clockHideScheduled = false;

    if (g_returnToIdleAfterClockHide)
    {
      g_returnToIdleAfterClockHide = false;
      g_clockLastText = "";
      g_currentBgPath = "/test.bin";

      fadeBacklight(255, 0, 120);

      clearClockArea();

      if (SD.cardType() != CARD_NONE && SD.exists(g_currentBgPath))
      {
        bool ok = drawRgb565File(g_currentBgPath, 480, 320);
        Serial.printf("[BG] after clock hide -> %s: %s\n",
                      g_currentBgPath,
                      ok ? "OK" : "FAIL");

        if (ok)
          rebuildClockSlotCaches();
        else
          tft.fillScreen(C_BLACK);
      }
      else
      {
        Serial.println("[BG] after clock hide -> /test.bin missing on SD");
        tft.fillScreen(C_BLACK);
      }

      fadeBacklight(0, 255, 140);
    }
    else
    {
      clearClockArea();
    }
  }
}
}