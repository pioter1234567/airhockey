// ===== AirHockey Panel — File Map =====
// 1) Ścieżki SD + stan gry (ustawienia/monety)           [sekcja globali]
// 2) Piny, SPI/TFT
// 3) Kolory + geometria
// 4) Wejście: definicje przycisków + struct Button
// 5) Prototypy (wczesne: showERROR, sdSaveSettings)
// 6) SD: init + RW + counters              (dalej w pliku: settings + coin log)
// 7) (dalej) Czas: systemUnix + RTC status + rtcSyncToSystem
// 8) (dalej) Rysowanie: BG z LittleFS, listy, ekrany
// 9) (dalej) Wejście: debounce, pressedNow, nawigacja
// 10) (dalej) Setup/Loop

// ====================== 1) Includy ======================
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include <Wire.h>
#include "RTClib.h"
#include "FS.h"
#include "LittleFS.h"
#include <SD_MMC.h>
#include "driver/twai.h"
#include <driver/adc.h>
#include "esp_system.h"
#include "games.h"
#include "main.h"


// === I2C PINS (ESP32-S3) ===
static const int I2C_SDA = 21; //rev1.3
static const int I2C_SCL = 7;  //rev1.3

// ====================== 1) Ścieżki SD + stan gry ======================
static const char* SETTINGS_PATH = "/settings.cfg";
static const char* COINLOG_PATH  = "/coinlog.csv";
static const char* COIN2_PATH    = "/coins2.dat";
static const char* COIN5_PATH    = "/coins5.dat";

Preferences gamePrefs;

int  gameTimeIdx = 1;            // 0=3 min, 1=5 min, 2=7 min, 3=9 min, 4=12 min, 5=15 min
int  goalsIdx    = 1;            // 0=3, 1=5, 2=7, 3=9
bool strobesOn   = true;
bool uvOn        = true;
bool animOn      = true;
bool standbyOn   = true;
bool goalIllumOn = true;
uint32_t coins2  = 0;            // licznik 2 PLN
uint32_t coins5  = 0;            // licznik 5 PLN
bool g_hasSD     = false;        // czy karta SD jest dostępna po starcie

// ====== Marquee (przewijanie wiersza w SCR_COINLOG) ======
int        coinlogMarqueeX      = 0;        // przesunięcie w px w lewo (0..wrap)
uint32_t   coinlogMarqueeLastMs = 0;        // ostatnia aktualizacja
const int  COINLOG_MARQ_SPEED   = 160;       // px/s (~1 px co 33 ms)
const int  COINLOG_MARQ_PAUSE   = 800;      // pauza na starcie (ms), zanim ruszy

// Totale przeliczone z pliku coinlog.csv (do wyświetlania)
uint32_t coinlogTotal2 = 0;
uint32_t coinlogTotal5 = 0;

//monety
#define COIN_2_PIN 36
#define COIN_5_PIN 37


// debounce impulsu wrzutnika monet
static const uint32_t COIN_DEBOUNCE_MS = 25;

// „gra trwa” (soft) – blokujemy kolejne starty na czas gry
static uint32_t gameBusyUntilMs = 0;

// kolejka monet (FIFO)
static const uint8_t COINQ_MAX = 10;
static uint8_t coinQ[COINQ_MAX];
static uint8_t coinQHead = 0, coinQTail = 0, coinQCount = 0;

// stan do debounce/edge
static bool last2Read = HIGH;
static bool last5Read = HIGH;
static uint32_t last2ChangeMs = 0;
static uint32_t last5ChangeMs = 0;


//inne

bool comboReady = true;

bool g_goalCalUiDirty = false;


//kalibracja 
// ===== ADC DEFAULTS =====
static const uint16_t ADC_IDLE_DEFAULT = 4095;
static const uint16_t ADC_BOTH_DEFAULT = 525;

static const uint16_t ADC_A_MIN_DEFAULT = 2200;
static const uint16_t ADC_A_MAX_DEFAULT = 3000;
static const uint16_t ADC_A_CENTER_DEFAULT = 2600;

static const uint16_t ADC_B_MIN_DEFAULT = 800;
static const uint16_t ADC_B_MAX_DEFAULT = 1600;
static const uint16_t ADC_B_CENTER_DEFAULT = 1100;


// ===== CURRENT SETTINGS =====
uint16_t adcIdle    = ADC_IDLE_DEFAULT;
uint16_t adcBoth    = ADC_BOTH_DEFAULT;

uint16_t adcAmin    = ADC_A_MIN_DEFAULT;
uint16_t adcAmax    = ADC_A_MAX_DEFAULT;
uint16_t adcAcenter = ADC_A_CENTER_DEFAULT;

uint16_t adcBmin    = ADC_B_MIN_DEFAULT;
uint16_t adcBmax    = ADC_B_MAX_DEFAULT;
uint16_t adcBcenter = ADC_B_CENTER_DEFAULT;



//LED button
#define LED_PIN 15

// --- TWAI (CAN) piny + config ---
#define CAN_TX_PIN  GPIO_NUM_14  //rev1.3
#define CAN_RX_PIN  GPIO_NUM_34 //rev1.3


//config po can
#define CAN_ID_SETTINGS      0x300
#define CAN_CMD_SETTINGS     0x02
#define CAN_ID_SETTINGS_ACK  0x312


// ====== CAN dla lokalnej muzyki mini-gier ======
#define CAN_ID_MINIGAME_AUDIO   0x340
#define CAN_CMD_MINIGAME_AUDIO  0x50
#define CAN_AUDIO_PLAY_TETRIS   0x01
#define CAN_AUDIO_STOP          0x00


// CAN dla goal calib
#define CAN_ID_GOALCAL_CMD   0x350
#define CAN_ID_GOALCAL_DATA  0x351

#define CAN_CMD_GOALCAL_START_A 0x60
#define CAN_CMD_GOALCAL_START_B 0x61
#define CAN_CMD_GOALCAL_CANCEL  0x62
#define CAN_CMD_GOALCAL_IDLE    0x63
#define CAN_CMD_GOALCAL_SAMPLE  0x64

// nie wiem do czego to??? pod 9 jest btn down w rev1.3 
//const int PIN = 9; // GPIO35 = ADC1_CH7




static twai_general_config_t twai_g_config =
    TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
static twai_timing_config_t  twai_t_config = TWAI_TIMING_CONFIG_250KBITS();
static twai_filter_config_t  twai_f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

bool canInit() {
  // ustaw długości kolejek **PRZED** instalacją drivera
  twai_g_config.tx_queue_len = 1;   // nie kolejkować wielu ramek na start
  twai_g_config.rx_queue_len = 10;  // jak chcesz

  if (twai_driver_install(&twai_g_config, &twai_t_config, &twai_f_config) != ESP_OK) {
    Serial.println("[CAN] driver_install ERROR");
    return false;
  }
  if (twai_start() != ESP_OK) {
    Serial.println("[CAN] start ERROR");
    return false;
  }

  // (opcjonalnie) alerty
  twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED, nullptr);

  Serial.printf("[CAN] TWAI started @250kbps (TX=%d RX=%d)\n", (int)CAN_TX_PIN, (int)CAN_RX_PIN);
  return true;
}


// ====== flaga ustawień ======
bool puckLockEnabled = true;   // domyślnie ON

// ====== CAN dla puck lock ======
#define CAN_ID_PUCKLOCK_CMD   0x330   // komendy natychmiastowe
#define CAN_CMD_PUCKLOCK_NOW  0x40    // typ komendy
#define CAN_PUCK_LOCK         0x01
#define CAN_PUCK_UNLOCK       0x02


// --- ACK/Backoff config ---
#define CAN_ACK_TIMEOUT_MS   120     // ile czekamy na ACK 0x311
#define CAN_MIN_BACKOFF_MS   800     // pierwszy „cooldown”
#define CAN_MAX_BACKOFF_MS   8000    // max „cooldown”

static uint32_t canCooldownUntil = 0; // do kiedy blokujemy kolejne START-y
static uint16_t canBackoffMs     = 0; // aktualny czas cooldownu

enum StartSendStatus : uint8_t {
  START_CAN_ERROR   = 0,   // nie wysłano (twai_transmit fail)
  START_SENT_NO_ACK = 1,   // wysłano, brak ACK w czasie
  START_SENT_ACK    = 2,   // wysłano i przyszło ACK
  START_COOLDOWN    = 3    // zablokowane: trwający cooldown po wcześniejszym braku ACK
};

// Typy gier (takie same jak po stronie koordynatora)
enum : uint8_t { GT_FULL = 1, GT_MUSIC = 2, GT_QUICK = 3 };

// gameTimeIdx -> minuty (masz już gameTimeIdx w kodzie)
static inline uint8_t gameTimeMinutes() {
  static const uint8_t mins[] = {3,5,7,9,12,15};
  return mins[gameTimeIdx];
}



static bool canWaitTxIdle(uint32_t timeout_ms) {
  uint32_t t0 = millis();
  twai_status_info_t st;
  do {
    if (twai_get_status_info(&st) == ESP_OK) {
      if (st.msgs_to_tx == 0) return true;  // nic nie czeka w kolejce
    }
    delay(2);
  } while ((int32_t)(millis() - t0) < (int32_t)timeout_ms);
  return false;
}

static void canFlushTxQueue() {
  // opróżnij TX, żeby nic nie zostało “na później”
  twai_clear_transmit_queue();
  Serial.println("[CAN] TX queue cleared");
}


static void canDumpBytesHex(const uint8_t* data, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    if (data[i] < 16) Serial.print('0');
    Serial.print(data[i], HEX);
    if (i + 1 < len) Serial.print(' ');
  }
}

static void canSendHeartbeat() {
  twai_message_t m = {};
  m.identifier = 0x555; m.extd = 0; m.rtr = 0;
  m.data_length_code = 2; m.data[0] = 0xAA; m.data[1] = 0x55;
  twai_transmit(&m, pdMS_TO_TICKS(10)); // w NO_ACK nie będzie czekał na ACK
}


static bool waitAck(uint32_t timeout_ms) {
  uint32_t t0 = millis();
  twai_message_t m;
  while ((int32_t)(millis() - t0) < (int32_t)timeout_ms) {
    if (twai_receive(&m, 0) == ESP_OK) {
      if (!m.extd && !m.rtr && m.identifier == 0x311 &&
          m.data_length_code >= 1 && m.data[0] == 0xAA) {
        return true;
      }
    }
    delay(2);
  }
  return false;
}


static bool canRecoverBlocking(uint32_t timeout_ms = 600) {
  twai_status_info_t st;
  twai_get_status_info(&st);
  if (st.state != TWAI_STATE_BUS_OFF) return true;

  twai_initiate_recovery();
  uint32_t deadline = millis() + timeout_ms;
  uint32_t alerts = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    if (twai_read_alerts(&alerts, pdMS_TO_TICKS(25)) == ESP_OK &&
        (alerts & TWAI_ALERT_BUS_RECOVERED)) {
      twai_start();
      return true;
    }
  }
  return false;
}




// Niski poziom: wysyłka ramki STD
static bool canSendStd(uint32_t id, const uint8_t* data, uint8_t len) {
  

  twai_message_t m = {};
  m.identifier = id; m.extd = 0; m.rtr = 0; m.data_length_code = len;
  if (len) memcpy(m.data, data, len);

  // 1) Pierwsza próba
  esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(50));
  if (err == ESP_OK) {
    Serial.print("[CAN] TX OK  ID=0x"); Serial.print(id, HEX);
    Serial.print(" DLC="); Serial.print(len); Serial.print(" DATA:");
    for (uint8_t i=0;i<len;i++){ Serial.print(' '); if (m.data[i]<16) Serial.print('0'); Serial.print(m.data[i],HEX); }
    Serial.println();
    return true;
  }

  // 2) Diagnostyka
  twai_status_info_t st;
  twai_get_status_info(&st);
  Serial.print("[CAN] TX FAIL err="); Serial.print((int)err);
  Serial.print(" state=");  Serial.print(st.state); // 0=Stopped,1=Running,2=BusOff,3=Recovering
  Serial.print(" txerr=");  Serial.print(st.tx_error_counter);
  Serial.print(" rxerr=");  Serial.println(st.rx_error_counter);

  // 3) Jeśli BUS_OFF/INVALID_STATE → recovery blokujące i retry
  if (err == ESP_ERR_INVALID_STATE || st.state == TWAI_STATE_BUS_OFF) {
    if (canRecoverBlocking(600)) {
      Serial.println("[CAN] recovered, retry TX");
      err = twai_transmit(&m, pdMS_TO_TICKS(50));
      if (err == ESP_OK) {
        Serial.println("[CAN] TX OK after recovery");
        return true;
      }
      Serial.print("[CAN] retry failed err="); Serial.println((int)err);
    }
  }

  return false;
}

static void goalCalSendCancelToMain() {
  uint8_t c[1] = { CAN_CMD_GOALCAL_CANCEL };
  canSendStd(CAN_ID_GOALCAL_CMD, c, 1);
  Serial.println("[GCAL] sent CANCEL to main");
}


static inline uint32_t msLeft(uint32_t t_future){
  int32_t d = (int32_t)(t_future - millis());
  return d > 0 ? (uint32_t)d : 0;
}



//Wysylka ustwaien
static inline uint8_t settingsFlags() {
  uint8_t f = 0;
  if (strobesOn)   f |= (1 << 0);
  if (animOn)      f |= (1 << 1);
  if (standbyOn)   f |= (1 << 2);
  if (uvOn)        f |= (1 << 3);
  if (goalIllumOn) f |= (1 << 4);
  if (puckLockEnabled) f |= (1 << 5);   // <—— NOWE
  return f;
}

void sendAdcThresholdsToMain() {

  uint8_t d[8];

  d[0] = adcAmin >> 8;
  d[1] = adcAmin & 0xFF;

  d[2] = adcAmax >> 8;
  d[3] = adcAmax & 0xFF;

  d[4] = adcBmin >> 8;
  d[5] = adcBmin & 0xFF;

  d[6] = adcBmax >> 8;
  d[7] = adcBmax & 0xFF;

  canSendStd(0x360, d, 8);

  Serial.printf("[ADC] sent thresholds to main A:%u-%u B:%u-%u\n",
                adcAmin, adcAmax,
                adcBmin, adcBmax);
}

// WYŚLIJ ustawienia (raz) + krótko poczekaj na ACK (opcjonalnie)
static bool canSendSettingsOnce(uint32_t ackWaitMs = 120) {
  const uint8_t p[4] = {
    CAN_CMD_SETTINGS,
    (uint8_t)gameTimeIdx,
    (uint8_t)goalsIdx,
    settingsFlags()
  };
  bool txok = canSendStd(CAN_ID_SETTINGS, p, sizeof(p));
  if (!txok) {
    Serial.println("[CAN] SettingsUpdate -> TX ERROR");
    return false;
  }

  // tu wyślij aktualne progi ADC do main
  sendAdcThresholdsToMain();

  // opcjonalne: szybki ACK (ID 0x312, DATA[0]=0xA5)
  uint32_t t0 = millis();
  twai_message_t m;
  while ((int32_t)(millis() - t0) < (int32_t)ackWaitMs) {
    if (twai_receive(&m, 0) == ESP_OK) {
      if (!m.extd && !m.rtr && m.identifier == CAN_ID_SETTINGS_ACK &&
          m.data_length_code >= 1 && m.data[0] == 0xA5) {
        Serial.println("[CAN] SettingsUpdate -> ACK");
        return true;
      }
    }
    delay(2);
  }
  Serial.println("[CAN] SettingsUpdate -> SENT (no ACK)");
  return true; // wysłane, nawet jeśli bez ACK
}

// Payload START: [0x01, gtype, gindex]
static StartSendStatus canSendStartGame(uint8_t gtype, uint8_t gindex) {
  const uint32_t SEND_WINDOW_MS = 2000;  // łączny czas na próby
  const uint32_t ACK_PER_TRY_MS = 600;   // ile czekamy na ACK po pojedynczej wysyłce

  const uint8_t data[3] = { 0x01, gtype, gindex };
  uint32_t deadline = millis() + SEND_WINDOW_MS;
  bool anyPosted = false;

  // kosmetyka logu
  Serial.print(F("[CAN] StartGame "));
  switch (gtype) {
    case GT_FULL:  Serial.print(F("FULL"));  break;
    case GT_MUSIC: Serial.print(F("MUSIC")); break;
    case GT_QUICK: Serial.print(F("QUICK")); break;
    default:       Serial.print(F("TYPE=")); Serial.print(gtype); break;
  }
  Serial.print(F(" index=")); Serial.print(gindex);

  // 1) ZACZNIJ “na czysto”
  canFlushTxQueue();

  // 2) PIERWSZA PRÓBA
  twai_message_t m = {};
  m.identifier = 0x301; m.extd = 0; m.rtr = 0;
  m.data_length_code = sizeof(data);
  memcpy(m.data, data, sizeof(data));

  esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(50));
  if (err == ESP_OK) {
    anyPosted = true;
    if (waitAck(ACK_PER_TRY_MS)) { Serial.println(F(" -> SENT + ACK")); return START_SENT_ACK; }
  } else {
    // jeśli BUS_OFF itp., spróbuj wyjść i dać JESZCZE jedną szansę (w oknie czasu)
    twai_status_info_t st; twai_get_status_info(&st);
    if (err == ESP_ERR_INVALID_STATE || st.state == TWAI_STATE_BUS_OFF) {
      // szybka próba recovery (nie zostawiamy kolejki z syfem)
      twai_initiate_recovery();
      uint32_t al=0; if (twai_read_alerts(&al, pdMS_TO_TICKS(200))==ESP_OK && (al & TWAI_ALERT_BUS_RECOVERED)) twai_start();
    }
  }

  // 3) DRUGA (ostatnia) PRÓBA – tylko gdy mamy jeszcze czas i kolejka jest PUSTA
  if ((int32_t)(millis() - (int32_t)deadline) < 0) {
    if (canWaitTxIdle(100)) {
      err = twai_transmit(&m, pdMS_TO_TICKS(50));
      if (err == ESP_OK) {
        anyPosted = true;
        uint32_t left = (uint32_t)max(0, (int32_t)deadline - (int32_t)millis());
        if (waitAck(left > ACK_PER_TRY_MS ? ACK_PER_TRY_MS : left)) {
          Serial.println(F(" -> SENT + ACK"));
          return START_SENT_ACK;
        }
      }
    }
  }

  // 4) BRAK ACK -> czyścimy, żeby NIC nie poszło “po czasie”
  if (anyPosted) {
    Serial.println(F(" -> SENT (timeout: no ACK)"));
    canFlushTxQueue();
    return START_SENT_NO_ACK;
  } else {
    Serial.println(F(" -> ERROR (TX)"));
    return START_CAN_ERROR;
  }
}








// Rejestry / kolory ST77xx
#define ST77XX_MADCTL     0x36
#define ST77XX_MADCTL_BGR 0x08


Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

// ====================== 3) Kolory + geometria ======================
uint16_t purple   = tft.color565(210, 210, 210);
uint16_t purpleD  = tft.color565(110, 110, 110);
uint16_t violet   = tft.color565(150, 120, 180);
uint16_t violetD  = tft.color565(80,  50,  110);

const uint16_t COLOR_BG     = ST77XX_BLACK;
const uint16_t COLOR_BG2    = violetD;
const uint16_t COLOR_FG     = ST77XX_BLACK;
const uint16_t COLOR_SEL_BG = violet;
const uint16_t COLOR_SEL_FG = ST77XX_WHITE;

const int16_t HEADER_H       = 16;
const int16_t TOP_MARGIN     = HEADER_H + 6;
const int16_t LEFT_MARGIN    = 10;
const int16_t LINE_H         = 18;
const int16_t SELECT_OFFSET_Y= 3;
const int16_t NL             = 9;

const int16_t FOOTER_H       = 16;

/*
// ====================== 4) Wejście: przyciski + struct ======================
#define BTN_UP     10 //rev1.3
#define BTN_DOWN   5 //rev1.3
#define BTN_SELECT 33 //rev1.3
#define BTN_BACK   9 //rev1.3 */



// ====================== 5) Prototypy (wczesne, wymagane przez SD) ======================
void showERROR(const char* msg);
bool sdSaveSettings();





bool sdWriteU32(const char* path, uint32_t v) {
  if (!g_hasSD) return false;
  SD_MMC.remove(path); // najprościej i najpewniej
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;
  size_t n = f.write((uint8_t*)&v, sizeof(v));
  f.flush();
  f.close();
  return n == sizeof(v);
}


// ====================== 6) SD: init + RW + counters ======================
bool sdInitOnce() {
  // Ustaw piny SDMMC pod Twoje GPIO:
  // CLK=41, CMD=40, D0=42, reszta nieużywana
  SD_MMC.setPins(/*clk*/41, /*cmd*/40, /*d0*/42, /*d1*/-1, /*d2*/-1, /*d3*/-1);

  // true = 1-bit mode (bo masz tylko DAT0)
  g_hasSD = SD_MMC.begin("/sdcard", true);

  if (!g_hasSD) {
    Serial.println("[SD_MMC] init failed");
    return false;
  }

  Serial.println("[SD_MMC] ready");




   

  
  // --- utwórz brakujące pliki jeśli karta jest "pusta"
  if (!SD_MMC.exists(COIN2_PATH)) {
    Serial.println("[SD] creating coins2.dat");
    sdWriteU32(COIN2_PATH, 0);
  }

  if (!SD_MMC.exists(COIN5_PATH)) {
    Serial.println("[SD] creating coins5.dat");
    sdWriteU32(COIN5_PATH, 0);
  }

  if (!SD_MMC.exists(SETTINGS_PATH)) {
    Serial.println("[SD] creating settings.dat");
    sdSaveSettings();
  }

  return true;
}

uint32_t sdReadU32(const char* path) {
  if (!g_hasSD) return 0;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  uint32_t v = 0;
  size_t n = f.read((uint8_t*)&v, sizeof(v));
  f.close();
  return (n == sizeof(v)) ? v : 0;
}







void sdGetCoinCounters(uint32_t& c2, uint32_t& c5) {
  if (!g_hasSD) { c2 = 0; c5 = 0; return; }
  c2 = sdReadU32(COIN2_PATH);
  c5 = sdReadU32(COIN5_PATH);
}

// ====== SETTINGS: zapis/odczyt na SD jako key=value ======
static void kv_trim(String& s) {
  s.trim();
  if (s.startsWith("#")) s = ""; // komentarze
}

// ====================== 6) SD: Settings (key=value) ======================
void sdLoadSettings() {
  // domyślne na wypadek braku pliku/karty
  gameTimeIdx = 1;
  goalsIdx    = 1;
  strobesOn   = true;
  animOn      = true;
  standbyOn   = true;
  uvOn        = true;
  goalIllumOn = true;
  puckLockEnabled = true; 


  if (!g_hasSD) return; // brak karty – zostają domyślne

  File f = SD_MMC.open(SETTINGS_PATH, FILE_READ);
  if (!f) {
    // brak pliku – zapisz domyślne i wyjdź
    sdSaveSettings();
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    kv_trim(line);
    if (line.length() == 0) continue;

    int eq = line.indexOf('=');
    if (eq < 0) continue;

    String k = line.substring(0, eq); kv_trim(k);
    String v = line.substring(eq + 1); kv_trim(v);

    if      (k == "gameTime")  gameTimeIdx  = v.toInt();
    else if (k == "goals")     goalsIdx     = v.toInt();
    else if (k == "strobes")   strobesOn    = (v.toInt() != 0);
    else if (k == "anim")      animOn       = (v.toInt() != 0);
    else if (k == "standby")   standbyOn    = (v.toInt() != 0);
    else if (k == "uv")        uvOn         = (v.toInt() != 0);
    else if (k == "goalIllum") goalIllumOn  = (v.toInt() != 0);
    else if (k == "puckLock") puckLockEnabled = (v.toInt() != 0);
  }
  f.close();
}

bool sdSaveSettings() {
  if (!g_hasSD) return false;

  SD_MMC.remove(SETTINGS_PATH);
  File f = SD_MMC.open(SETTINGS_PATH, FILE_WRITE);
  if (!f) return false;

  f.printf("# AirHockey settings\n");
  f.printf("gameTime=%d\n",  gameTimeIdx);
  f.printf("goals=%d\n",     goalsIdx);
  f.printf("strobes=%d\n",   strobesOn   ? 1 : 0);
  f.printf("anim=%d\n",      animOn      ? 1 : 0);
  f.printf("standby=%d\n",   standbyOn   ? 1 : 0);
  f.printf("uv=%d\n",        uvOn        ? 1 : 0);
  f.printf("goalIllum=%d\n", goalIllumOn ? 1 : 0);
  f.printf("puckLock=%d\n", puckLockEnabled ? 1 : 0);
  f.flush();
  f.close();

  // —— SYNC po CAN ——
  canSendSettingsOnce(120);  // wyślij i krótko poczekaj na ACK (nie blokuje dłużej niż ~120 ms)
  return true;
}



void adcSettingsLoad() {
  Preferences p;
  p.begin("adc", true);   // read-only

  adcIdle    = p.getUShort("idle",    ADC_IDLE_DEFAULT);
  adcBoth    = p.getUShort("both",    ADC_BOTH_DEFAULT);

  adcAmin    = p.getUShort("amin",    ADC_A_MIN_DEFAULT);
  adcAmax    = p.getUShort("amax",    ADC_A_MAX_DEFAULT);
  adcAcenter = p.getUShort("acen",    ADC_A_CENTER_DEFAULT);

  adcBmin    = p.getUShort("bmin",    ADC_B_MIN_DEFAULT);
  adcBmax    = p.getUShort("bmax",    ADC_B_MAX_DEFAULT);
  adcBcenter = p.getUShort("bcen",    ADC_B_CENTER_DEFAULT);

  p.end();

  Serial.printf("[ADC] load idle=%u both=%u A:%u-%u c=%u B:%u-%u c=%u\n",
                adcIdle, adcBoth,
                adcAmin, adcAmax, adcAcenter,
                adcBmin, adcBmax, adcBcenter);
}

void adcSettingsSave() {
  Preferences p;
  p.begin("adc", false);  // read-write

  p.putUShort("idle", adcIdle);
  p.putUShort("both", adcBoth);

  p.putUShort("amin", adcAmin);
  p.putUShort("amax", adcAmax);
  p.putUShort("acen", adcAcenter);

  p.putUShort("bmin", adcBmin);
  p.putUShort("bmax", adcBmax);
  p.putUShort("bcen", adcBcenter);

  p.end();

  Serial.printf("[ADC] save idle=%u both=%u A:%u-%u c=%u B:%u-%u c=%u\n",
                adcIdle, adcBoth,
                adcAmin, adcAmax, adcAcenter,
                adcBmin, adcBmax, adcBcenter);
}

// ====================== 7) Czas: systemUnix + RTC status + sync ======================
uint32_t sysEpochAtSync = 0;   // unixtime w momencie synchronizacji
uint32_t sysMsAtSync    = 0;   // millis() w momencie synchronizacji

static inline uint32_t systemUnix() {
  // epoch_z_RTC + (ile sekund minęło od synchronizacji)
  return sysEpochAtSync + (uint32_t)((millis() - sysMsAtSync) / 1000);
}

// RTC status
bool        rtcReady        = false;
bool        rtcRunning      = false;
RTC_DS3231  rtc;
bool        rtcWarnChangeBat= false;
bool        rtcWarnNotFound = false;

// Synchronizacja „zegara systemowego” z RTC (albo z czasem kompilacji)
void rtcSyncToSystem() {
  if (rtcReady && rtcRunning) {
    sysEpochAtSync = rtc.now().unixtime();
  } else {
    DateTime fallback(F(__DATE__), F(__TIME__));
    sysEpochAtSync = fallback.unixtime();
  }
  sysMsAtSync = millis();
}

// ====================== 8) SD: Coin log (CSV) + API liczników ======================
bool sdLogCoinInsert(uint8_t denom, const char* reason) {
  if (!g_hasSD) return false;

  // utwórz nagłówek jeśli brak pliku
  if (!SD_MMC.exists(COINLOG_PATH)) {
    File h = SD_MMC.open(COINLOG_PATH, FILE_WRITE);
    if (!h) return false;
    h.println("unix_ts;denom;reason");
    h.close();
  }

  File f = SD_MMC.open(COINLOG_PATH, FILE_APPEND);
  if (!f) return false;

  const uint32_t ts = systemUnix();
  f.printf("%lu;%u;%s\n", (unsigned long)ts, denom, reason ? reason : "");
  f.flush();
  f.close();
  return true;
}

bool incCoin2(const char* reason) {
  if (!g_hasSD) { coins2++; return false; }  // RAM only, bez loga
  coins2 = sdReadU32(COIN2_PATH) + 1;
  const bool ok1 = sdWriteU32(COIN2_PATH, coins2);
  const bool ok2 = sdLogCoinInsert(2, reason);
  return ok1 && ok2;
}

bool incCoin5(const char* reason) {
  if (!g_hasSD) { coins5++; return false; }  // RAM only, bez loga
  coins5 = sdReadU32(COIN5_PATH) + 1;
  const bool ok1 = sdWriteU32(COIN5_PATH, coins5);
  const bool ok2 = sdLogCoinInsert(5, reason);
  return ok1 && ok2;
}


// ====== CoinLog: delikatne odświeżanie tylko zaznaczonego wiersza ======
int lastSelCoinLog = -1;   // ostatnio narysowany zaznaczony globalIdx
int lastCoinLogTop = -1;   // ostatnio narysowany top (do wykrycia przewinięcia)


// przerwanie marquee
inline void coinlogMarqueeReset() {
  coinlogMarqueeX = 0;
  coinlogMarqueeLastMs = millis();
}




// ====================== 9) Wejście: debounce setup (instancje przycisków) ======================
const uint16_t DEBOUNCE_MS = 40;
const uint16_t REPEAT_DELAY_MS  = 500; // po tylu ms od wciśnięcia rusza auto-repeat
const uint16_t REPEAT_RATE_MS   = 120; // co tyle ms kolejny krok podczas trzymaniaz
Button bUp     { BTN_UP,     HIGH, HIGH, 0, true, 0 };
Button bDown   { BTN_DOWN,   HIGH, HIGH, 0, true, 0 };
Button bSelect { BTN_SELECT, HIGH, HIGH, 0, true, 0 };
Button bBack   { BTN_BACK,   HIGH, HIGH, 0, true, 0 };

// ====================== 10) UI: Play Confirm (gradient + rysowanie) ======================
char confirmText[32];
int  selConfirm = 0;   // 0=YES, 1=NO

void drawVerticalGradient(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t c1, uint16_t c2) {
  for (int16_t j = 0; j < h; j++) {
    const float t = (float)j / (h - 1);
    const uint8_t r = (uint8_t)(((1 - t) * ((c1 >> 11) & 0x1F) + t * ((c2 >> 11) & 0x1F)) * 255.0 / 31.0);
    const uint8_t g = (uint8_t)(((1 - t) * ((c1 >> 5)  & 0x3F) + t * ((c2 >> 5)  & 0x3F)) * 255.0 / 63.0);
    const uint8_t b = (uint8_t)(((1 - t) * ( c1        & 0x1F) + t * ( c2        & 0x1F)) * 255.0 / 31.0);
    const uint16_t color = tft.color565(r, g, b);
    tft.drawFastHLine(x, y + j, w, color);
  }
}



static inline bool repeatNow(Button& b) {
  if (b.stableState == LOW && b.repeatAtMs != 0 &&
      (int32_t)(millis() - b.repeatAtMs) >= 0) {
    b.repeatAtMs += REPEAT_RATE_MS;   // zaplanuj kolejny krok
    return true;                      // właśnie „kliknęło” z auto-repeat
  }
  return false;
}







void drawPlayConfirm() {
  // obrazek z Full game zostaje w tle

  // Pasek na dole pod tekst
  const int HBAR = 60;
  const int y0 = tft.height() - HBAR;
  drawVerticalGradient(0, y0, tft.width(), HBAR, purpleD, purple);

  // Tekst pytania (zawijamy w dwie linie)
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(6, y0 + 6);   tft.print("Do you want to play");
  tft.setCursor(6, y0 + 18);  tft.print(confirmText);

  // Przyciski NO / YES
  const int yBtn = y0 + 36;
  const int btnW = 40;
  const int btnH = 16;

  auto drawButton = [&](int x, const char* label, uint16_t colorBg, uint16_t colorFg) {
    tft.fillRect(x, yBtn, btnW, btnH, colorBg);
    const int textW = (int)strlen(label) * 6; // szerokość w px dla fontu 5x7(+1)
    const int textX = x + (btnW - textW) / 2;
    const int textY = yBtn + (btnH - 8) / 2;
    tft.setTextColor(colorFg);
    tft.setCursor(textX, textY);
    tft.print(label);
  };

  drawButton(20, "NO",  ST77XX_RED,   ST77XX_WHITE);
  drawButton(80, "YES", ST77XX_GREEN, ST77XX_BLACK);
}

// ====================== 11) Stan gry / kolejka monet ======================
bool   gameRunning = false;
uint8_t queued2PLN = 0;
uint8_t queued5PLN = 0;







// ====================== 12) Ekrany / nawigacja ======================
enum Screen {
  // Główne menu i potwierdzenie
  SCR_MAIN,
  SCR_PLAYCONFIRM,

  // Podmenu
  SCR_PLAYROOT,        // Play >
  SCR_SETTINGS,        // Settings >

  // Listy gier
  SCR_PLAY,            // Full game (lista światów)
  SCR_MUSICROUND,      // Music round (lista utworów)

  // Ustawienia
  SCR_GAMETIME,        // 3/5/7/9/12/15
  SCR_GOALS,           // 1/3/5/7/9
  SCR_LIGHTS,          // Strobes/Animation/Stand-by/UV/Goal illum
  SCR_STROBES,         // ON/OFF
  SCR_ANIMATION,       // ON/OFF
  SCR_STANDBY,         // ON/OFF
  SCR_UVLIGHT,         // ON/OFF
  SCR_GOALILLUM,       // ON/OFF
  SCR_PUCKLOCK,  

  SCR_SETDT,           // Set date & time

  SCR_GOALCAL,         // Goal calibration >
  SCR_GOALCAL_RUN,     // ekran trwającej kalibracji
  SCR_GOALCAL_THRESH,  // ADC threshold

  // Pozostałe
  SCR_OTHER,           // About
  SCR_COINS,            // Coin counter

  SCR_COINROOT,        // Coin counter >
  SCR_COINLOG         // Latest entries
};

Screen current     = SCR_MAIN;
Screen previous    = SCR_MAIN;
Screen backStack[5];
int    backStackSize   = 0;
unsigned long lastInputTime = 0;
bool   screenDimmed    = false;

// ====================== 13) Selektory ======================
int selMain     = 0;
int selPlay     = 0;
int selMusic    = 0;
int selTime     = gameTimeIdx;
int selGoals    = goalsIdx;
int selLights   = 0;
int selBin      = 0;
int selPlayRoot = 0;   // Play >
int selSettings = 0;   // Settings >
int selCoinRoot = 0;   // w podmenu Coin counter
int selCoinLog  = 0;   // indeks globalny wybranej pozycji (0 = najnowsza)
int coinLogTop  = 0;   // pierwszy indeks na ekranie (do przewijania)
int selPuckLock = 0;  

int selGoalCal = 0;

enum GoalCalMode : uint8_t {
  GCAL_NONE = 0,
  GCAL_A    = 1,
  GCAL_B    = 2
};

enum GoalCalPhase : uint8_t {
  GCAL_WAIT_IDLE = 0,
  GCAL_RUNNING   = 1,
  GCAL_DONE      = 2
};

struct GoalCalState {
  bool active = false;
  GoalCalMode mode = GCAL_NONE;
  GoalCalPhase phase = GCAL_WAIT_IDLE;

  uint8_t scored = 0;
  uint8_t target = 10;

  uint32_t phaseStartMs = 0;
  bool lockedAt9 = false;

  uint16_t samples[10];   // raw ADC dla każdej bramki
} g_goalCal;


// ======  13)a ;) sCoinLog: bufor RAM + loader (najnowsze będą liczone z końca) ======
struct CoinLogEntry {
  uint32_t ts;     // unix_ts
  uint8_t  denom;  // 2 lub 5
};

static const int LOG_BUF_CAP  = 256; // ile maksymalnie buforujemy w RAM
static const int LOG_MAX_SHOW = 99;  // UI pokazuje najwyżej 99 najnowszych
CoinLogEntry coinLogBuf[LOG_BUF_CAP];
int coinLogCount = 0; // ile realnie wczytano (<= LOG_BUF_CAP)

// Wczytuje CAŁY plik do bufora cyklicznego (ostatnie LOG_BUF_CAP wpisów).
// Format linii: unix_ts;denom;reason
void coinlogLoadBuffer() {
  coinLogCount = 0;
  if (!g_hasSD) return;
  File f = SD_MMC.open(COINLOG_PATH, FILE_READ);
  if (!f) return;

  // prosty ring-buffer: nadpisujemy najstarsze, aby mieć ostatnie N
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line.startsWith("unix_ts")) continue; // pomiń nagłówek/puste

    int s1 = line.indexOf(';');
    if (s1 < 0) continue;
    int s2 = line.indexOf(';', s1 + 1);
    if (s2 < 0) s2 = line.length();

    uint32_t ts = (uint32_t) line.substring(0, s1).toInt();
    int denom   = line.substring(s1 + 1, s2).toInt();
    if (denom != 2 && denom != 5) continue;

    // wpis do ringa (oldest->newest w kolejnych pozycjach bufora)
    if (coinLogCount < LOG_BUF_CAP) {
      coinLogBuf[coinLogCount].ts    = ts;
      coinLogBuf[coinLogCount].denom = (uint8_t)denom;
      coinLogCount++;
    } else {
      // przesuw: drop najstarszy, dopisz na końcu
      memmove(coinLogBuf, coinLogBuf + 1, sizeof(CoinLogEntry) * (LOG_BUF_CAP - 1));
      coinLogBuf[LOG_BUF_CAP - 1].ts    = ts;
      coinLogBuf[LOG_BUF_CAP - 1].denom = (uint8_t)denom;
    }
  }
  f.close();

  // po wczytaniu zresetuj pozycję listy
  selCoinLog = 0;
  coinLogTop = 0;
  coinlogMarqueeReset();
}

// mapowanie: indeks 0 = najnowszy, 1 = poprzedni, ...
bool coinlogGetNewest(int newestIndex, CoinLogEntry& out) {
  if (coinLogCount <= 0) return false;
  if (newestIndex < 0) return false;
  // globalny indeks w buforze rosnącym oldest..newest
  int idx = coinLogCount - 1 - newestIndex;
  if (idx < 0 || idx >= coinLogCount) return false;
  out = coinLogBuf[idx];
  return true;
}


// ====================== 14) Definicje list menu ======================

// Główne menu
const char* mainItems[] = {
  "Play >",
  "Settings >",
  "About",
  "Coin counter >",
  //"Wrzuc 5 zl (test)",
  //"Wrzuc 2 zl (test)"
};
const uint8_t mainCount = sizeof(mainItems)/sizeof(mainItems[0]);

// Podmenu Play
const char* playRootItems[] = {
  "Full game >",
  "Music round >"
};
const uint8_t playRootCount = sizeof(playRootItems)/sizeof(playRootItems[0]);

// Podmenu Settings
const char* settingsItems[] = {
  "Game time >",
  "Goals to win >",
  "Light effects >",
  "Puck lock >",
  "Set date & time >",
  "Goal calibration >"
};
const uint8_t settingsCount = sizeof(settingsItems)/sizeof(settingsItems[0]);

//Podmenu coin counter
const char* coinRootItems[] = {
  "Latest entries >"
};
const uint8_t coinRootCount = sizeof(coinRootItems)/sizeof(coinRootItems[0]); // = 1

// Listy gier
const char* fullGameItems[] = {
  "Teensies in Trouble",
  "Toad Story",
  "Fiesta de Muertos",
  "20000 Lums",
  "Olympus Maximus",
  "Living Dead Party"
};
const uint8_t fullGameCount = sizeof(fullGameItems)/sizeof(fullGameItems[0]);

const char* musicRoundItems[] = {
  "Castle Rock",
  "Orchestral Chaos",
  "Mariachi Madness",
  "Gloo Gloo",
  "Dragon Slayer",
  "Grannies' World Tour"
};
const uint8_t musicRoundCount = sizeof(musicRoundItems)/sizeof(musicRoundItems[0]);

// Ustawienia
const char* timeItems[]  = { "3 min", "5 min", "7 min", "9 min", "12 min", "15 min" };
const uint8_t timeCount  = sizeof(timeItems)/sizeof(timeItems[0]);

const char* goalsItems[] = { "1", "3", "5", "7", "9" };
const uint8_t goalsCount = sizeof(goalsItems)/sizeof(goalsItems[0]);

const char* lightsItems[] = {
  "Strobes >",
  "Animation >",
  "Stand-by lights >",
  "UV lights >",
  "Goal illumination >"
};
const uint8_t lightsCount = sizeof(lightsItems)/sizeof(lightsItems[0]);

const char* onOffItems[] = { "ON", "OFF" };
const uint8_t onOffCount = 2;


// Puck lock set

static const char* puckLockItems[] = {
  "Enabled",
  "Disabled",
  "Lock now",
  "Unlock now"
};
static const uint8_t puckLockCount = sizeof(puckLockItems)/sizeof(puckLockItems[0]);

static const char* goalCalItems[] = {
  "Auto calibration >",
  "ADC thresholds >",
  "Reset to defaults"
};
static const uint8_t goalCalCount = sizeof(goalCalItems) / sizeof(goalCalItems[0]);










// ====================== 15) RTC: ustawianie daty/czasu ======================
struct DtState {
  int year, month, day, hour, minute, second;
  uint8_t cursor; // 0:Y,1:M,2:D,3:h,4:m,5:s
  bool loaded;
} dt;

static inline bool isLeap(int y) {
  return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}
static inline int daysInMonth(int y, int m) {
  static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m == 2) return d[1] + (isLeap(y) ? 1 : 0);
  return d[m - 1];
}

void setdtLoadFromRTC() {
  DateTime now = (rtcReady && rtcRunning)
                 ? rtc.now()
                 : DateTime(F(__DATE__), F(__TIME__));

  dt.year   = now.year();
  dt.month  = now.month();
  dt.day    = now.day();
  dt.hour   = now.hour();
  dt.minute = now.minute();
  dt.second = now.second();
  dt.cursor = 0;
  dt.loaded = true;
}

void setdtApplyDelta(int dir) {
  switch (dt.cursor) {
    case 0: // year
      dt.year += dir;
      if (dt.year < 2000) dt.year = 2000;
      if (dt.year > 2099) dt.year = 2099;
      if (dt.day > daysInMonth(dt.year, dt.month)) dt.day = daysInMonth(dt.year, dt.month);
      break;
    case 1: // month
      dt.month += dir;
      if (dt.month < 1) dt.month = 12;
      if (dt.month > 12) dt.month = 1;
      if (dt.day > daysInMonth(dt.year, dt.month)) dt.day = daysInMonth(dt.year, dt.month);
      break;
    case 2: // day
      dt.day += dir;
      if (dt.day < 1) dt.day = daysInMonth(dt.year, dt.month);
      if (dt.day > daysInMonth(dt.year, dt.month)) dt.day = 1;
      break;
    case 3: // hour
      dt.hour += dir;
      if (dt.hour < 0) dt.hour = 23;
      if (dt.hour > 23) dt.hour = 0;
      break;
    case 4: // minute
      dt.minute += dir;
      if (dt.minute < 0) dt.minute = 59;
      if (dt.minute > 59) dt.minute = 0;
      break;
    case 5: // second
      dt.second += dir;
      if (dt.second < 0) dt.second = 59;
      if (dt.second > 59) dt.second = 0;
      break;
  }
}

// ====================== 16) Rysowanie: header + gradienty ======================
void drawHorizontalGradient(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t c1, uint16_t c2) {
  for (int16_t i = 0; i < w; i++) {
    float t = (float)i / (w - 1);
    uint8_t r = (uint8_t)(((1 - t) * ((c1 >> 11) & 0x1F) + t * ((c2 >> 11) & 0x1F)) * 255.0 / 31.0);
    uint8_t g = (uint8_t)(((1 - t) * ((c1 >> 5)  & 0x3F) + t * ((c2 >> 5)  & 0x3F)) * 255.0 / 63.0);
    uint8_t b = (uint8_t)(((1 - t) * ( c1       & 0x1F) + t * ( c2       & 0x1F)) * 255.0 / 31.0);
    uint16_t color = tft.color565(r, g, b);
    tft.drawFastVLine(x + i, y, h, color);
  }
}

void drawHeader(const char* title) {
  drawVerticalGradient(0, 0, tft.width(), HEADER_H, purpleD, purple); // spójnie: góra dark -> dół jasny
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(7, 5);
  tft.print(title);
}




void drawPuckLockMenu() {
  drawHeader("Puck lock");
  tft.setTextWrap(false);
  tft.setTextSize(1);

  for (uint8_t i = 0; i < puckLockCount; ++i) {
    int16_t y = TOP_MARGIN + i * LINE_H;

    // rysowanie separatora przed akcjami (czyli przed "Lock now")
    if (i == 2) {
      tft.drawFastHLine(0, y - 4, tft.width(), ST77XX_BLACK);
    }

    if (i == selPuckLock) {
      drawVerticalGradient(0, y - 2, tft.width(), LINE_H, COLOR_SEL_BG, COLOR_BG2);
      tft.setTextColor(COLOR_SEL_FG);
    } else {
      tft.setTextColor(COLOR_FG);
    }

    tft.setCursor(LEFT_MARGIN, y + SELECT_OFFSET_Y);

    if (i == 0 || i == 1) {
      bool isChecked = (i == 0) ? puckLockEnabled : !puckLockEnabled;
      tft.print(isChecked ? "[x] " : "[ ] ");
    }

    tft.print(puckLockItems[i]);
  }
}




void drawSetDateTime() {
  drawHeader("Set date & time");

  tft.setTextColor(COLOR_FG);
  tft.setTextSize(1);

  // Linia 1: Data YYYY-MM-DD
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.year, dt.month, dt.day);
  int16_t y1 = TOP_MARGIN + 4;
  tft.setCursor(LEFT_MARGIN, y1);
  tft.print("Date: ");
  tft.print(buf);

  // Linia 2: Czas hh:mm:ss
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
  int16_t y2 = y1 + LINE_H + 6;
  tft.setCursor(LEFT_MARGIN, y2);
  tft.print("Time: ");
  tft.print(buf);

  // Podpowiedzi
  int16_t y3 = y2 + LINE_H + 8;
  tft.setCursor(LEFT_MARGIN, y3);              tft.print("UP/DOWN change");
  tft.setCursor(LEFT_MARGIN, y3 + LINE_H-2);   tft.print("SELECT next/save");
  tft.setCursor(LEFT_MARGIN, y3 + LINE_H*2-4); tft.print("BACK prev/exit");

  // Obrys aktywnego pola
  int xBase   = LEFT_MARGIN + 6*6; // po "Date: "
  int xBaseT  = LEFT_MARGIN + 6*6; // po "Time: "
  int segX[6] = { xBase+0*6, xBase+5*6, xBase+8*6, xBaseT+0*6, xBaseT+3*6, xBaseT+6*6 };
  int segW[6] = { 4*6, 2*6, 2*6, 2*6, 2*6, 2*6 };
  int segY[6] = { y1, y1, y1, y2, y2, y2 };

  tft.drawRect(segX[dt.cursor]-2, segY[dt.cursor]-3, segW[dt.cursor]+4, LINE_H-2, ST77XX_RED);
}


void drawGoalCalMenu() {
  drawHeader("Goal calibration");
  tft.setTextWrap(false);
  tft.setTextSize(1);

  for (uint8_t i = 0; i < goalCalCount; ++i) {
    int16_t y = TOP_MARGIN + i * LINE_H;

    if (i == selGoalCal) {
      drawVerticalGradient(0, y - 2, tft.width(), LINE_H, COLOR_SEL_BG, COLOR_BG2);
      tft.setTextColor(COLOR_SEL_FG);
    } else {
      tft.setTextColor(COLOR_FG);
    }

    tft.setCursor(LEFT_MARGIN, y + SELECT_OFFSET_Y);
    tft.print(goalCalItems[i]);
  }
}

void drawGoalCalRun() {
  drawHeader(g_goalCal.mode == GCAL_A ? "Auto-cal A" : "Auto-cal B");

  tft.setTextWrap(true);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_FG);

  int y = TOP_MARGIN;
  const int bigspace = LINE_H - 0;   
  const int smallspace = 10;         

  if (g_goalCal.phase == GCAL_WAIT_IDLE) {
    tft.setCursor(LEFT_MARGIN, y);
    tft.print("Please wait 3 sec.");
    y += LINE_H + 4;

    tft.setCursor(LEFT_MARGIN, y);
    tft.print("Reading idle ");
    y += smallspace;
    tft.setCursor(LEFT_MARGIN, y);
    tft.print("sensor level...");
  }
  else if (g_goalCal.phase == GCAL_RUNNING) {
    if (g_goalCal.mode == GCAL_A) {
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("Cover Goal B entry");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("slot with a towel.");

      y += bigspace;

      tft.setCursor(LEFT_MARGIN, y);
      tft.print("Score 10 goals into");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("Goal A.");
      y += bigspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("(the one near");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("this unit)");
      y += bigspace;
   
    } else {
tft.setCursor(LEFT_MARGIN, y);
      tft.print("Cover Goal A entry");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("slot with a towel.");

      y += bigspace;

      tft.setCursor(LEFT_MARGIN, y);
      tft.print("Score 10 goals into");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("Goal B.");
      y += bigspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("(the one further");
      y += smallspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("from this unit)");
      y += bigspace;
    }

    y += smallspace;

    tft.setTextWrap(false);

    tft.setCursor(LEFT_MARGIN, y);
    tft.setTextSize(1);
    tft.print("Scored goals: ");

    // większa czcionka dla licznika
    y += smallspace;
    tft.setCursor(LEFT_MARGIN, y);
    tft.setTextSize(2);
    tft.print(g_goalCal.scored);
    tft.print("/");
    tft.print(g_goalCal.target);

    // wróć do normalnej czcionki
    tft.setTextSize(1);
  }
  else if (g_goalCal.phase == GCAL_DONE)
  {
    tft.setCursor(LEFT_MARGIN, y);
    if (g_goalCal.mode == GCAL_A) {
      tft.print("Calibration A");
      y += bigspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("completed.");
    } else {
      tft.print("Calibration B");
      y += bigspace;
      tft.setCursor(LEFT_MARGIN, y);
      tft.print("completed.");
    }
  }
}

void resetAdcDefaults() {

  adcIdle    = ADC_IDLE_DEFAULT;
  adcBoth    = ADC_BOTH_DEFAULT;

  adcAmin    = ADC_A_MIN_DEFAULT;
  adcAmax    = ADC_A_MAX_DEFAULT;
  adcAcenter = ADC_A_CENTER_DEFAULT;

  adcBmin    = ADC_B_MIN_DEFAULT;
  adcBmax    = ADC_B_MAX_DEFAULT;
  adcBcenter = ADC_B_CENTER_DEFAULT;

  Serial.println("[ADC] Reset to defaults");
    adcSettingsSave();
  sendAdcThresholdsToMain();
  // odśwież UI
  g_goalCalUiDirty = true;
}

// ====================== 17) Ścieżki teł (LittleFS) ======================
const char* MENU_BG = "/bg_menu.raw"; 
const char* fullGameBGPaths[] = {
  "/bg_teensies.raw",
  "/bg_toad.raw",
  "/bg_fiesta.raw",
  "/bg_20000.raw",
  "/bg_olympus.raw",
  "/bg_ldp.raw"
};
const char* ABOUT_BG = "/bg_about.raw";  // 128x160, RGB565

const char* musicRoundBGPaths[] = {
  "/bg_castle.raw",     // Castle Rock
  "/bg_orchestral.raw", // Orchestral Chaos
  "/bg_mariachi.raw",   // Dia de los Muertos
  "/bg_gloo.raw",       // Gloo Gloo
  "/bg_dragon.raw",     // Dragon Slayer
  "/bg_grannies.raw"    // Grannies' World Tour
};


// ====================== 18) Rysowanie: tła (LittleFS) ======================
bool drawBGFromFS(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) { tft.fillScreen(COLOR_BG); return false; }

  const int W = 128;   // szerokość ekranu przy rotation(0)
  const int H = 160;   // wysokość ekranu przy rotation(0)
  const size_t need = (size_t)W * H * 2;

  size_t sz = f.size();
  // plik 40964 => 4 bajty nagłówka/paddingu
  if (sz == need + 4) f.seek(4, SeekSet);
  else if (sz != need) { f.close(); tft.fillScreen(COLOR_BG); return false; }

  static uint16_t line[W];
  for (int y = 0; y < H; y++) {
    size_t n = f.read((uint8_t*)line, W * 2);
    if (n != W * 2) break;
    // Jeśli kolory będą zamienione, odkomentuj:
    // for (int i=0;i<W;++i) line[i] = (line[i] >> 8) | (line[i] << 8);
    tft.drawRGBBitmap(0, y, line, W, 1);
  }

  f.close();
  return true;
}

// Wspólny wybór tła po indeksie
void drawBGForSelection(const char** paths, uint8_t count, int selectedIdx) {
  if (selectedIdx >= 0 && selectedIdx < (int)count) {
    if (!drawBGFromFS(paths[selectedIdx])) tft.fillScreen(COLOR_BG);
  } else {
    tft.fillScreen(COLOR_BG);
  }
}

void drawMenuBackground() {
  if (!drawBGFromFS(MENU_BG)) {
    tft.fillScreen(COLOR_BG);
  }
}

void drawAboutScreen() {
  if (!drawBGFromFS(ABOUT_BG)) {
    tft.fillScreen(COLOR_BG);
  }
}

void drawFullGameMenu(const char*, const char**, uint8_t, int selectedIdx) {
  drawBGForSelection(fullGameBGPaths,
    (uint8_t)(sizeof(fullGameBGPaths)/sizeof(fullGameBGPaths[0])), selectedIdx);
}

void drawMusicRoundMenu(const char*, const char**, uint8_t, int selectedIdx) {
  drawBGForSelection(musicRoundBGPaths,
    (uint8_t)(sizeof(musicRoundBGPaths)/sizeof(musicRoundBGPaths[0])), selectedIdx);
}

// ====================== 19) Rysowanie: listy i ekrany ======================



// ====== BG helper: wklej pasek tapety MENU_BG (RAW 128x160 RGB565) ======
static void blitMenuBGStrip(int y, int h) {
  if (y >= tft.height() || h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > tft.height()) h = tft.height() - y;

  File f = LittleFS.open(MENU_BG, "r");
  if (!f) { 
    // fallback: czyszczenie, gdyby nie było pliku
    tft.fillRect(0, y, tft.width(), h, COLOR_BG);
    return;
  }

  const int W = 128;              // szerokość przy rotation(0)
  const int H = 160;              // wysokość przy rotation(0)
  const size_t need = (size_t)W * H * 2;

  size_t sz = f.size();
  size_t header = 0;
  if (sz == need + 4) header = 4;           // 4-bajtowy nagłówek
  else if (sz != need) { f.close(); tft.fillRect(0, y, tft.width(), h, COLOR_BG); return; }

  // przeskocz do pierwszej linii paska
  size_t offset = header + (size_t)y * W * 2;
  f.seek(offset, SeekSet);

  static uint16_t line[W];
  for (int j = 0; j < h; ++j) {
    size_t n = f.read((uint8_t*)line, W * 2);
    if (n != W * 2) break;
    tft.drawRGBBitmap(0, y + j, line, W, 1);
  }
  f.close();
}


void drawMenuList(const char* title, const char** items, uint8_t count,
                  int selectedIdx, int checkedIdx = -1, bool drawCheckboxes = false) {
  tft.setTextWrap(false);

  // nagłówek
  drawHeader(title);

  // lista
  tft.setTextSize(1);
  for (uint8_t i = 0; i < count; i++) {
    int16_t y = TOP_MARGIN + i * LINE_H;

    if (i == selectedIdx) {
      drawVerticalGradient(0, y - 2, tft.width(), LINE_H, COLOR_SEL_BG, COLOR_BG2);
      tft.setTextColor(COLOR_SEL_FG);
    } else {
      tft.setTextColor(COLOR_FG);
    }
    tft.setCursor(LEFT_MARGIN, y + SELECT_OFFSET_Y);

    if (drawCheckboxes) {
      if (i == checkedIdx) tft.print("[x] ");
      else                 tft.print("[ ] ");
    }
    tft.print(items[i]);
  }
}

// ---- CoinLog layout offsets (lista + marquee w tym samym układzie) ----
static const int COINLOG_SHIFT_UP   = 6;  // px w górę
static const int COINLOG_SHIFT_LEFT = 6;  // px w lewo

static inline int coinlogTopYBase()  { return max(0, TOP_MARGIN  - COINLOG_SHIFT_UP); }
static inline int coinlogLeftXBase() { return max(0, LEFT_MARGIN - COINLOG_SHIFT_LEFT); }




void drawAboutFallback() {
  // fallback tekstowy „About”
  tft.setTextWrap(true);
  drawHeader("About");
  tft.setTextColor(COLOR_FG);
  tft.setCursor(6, TOP_MARGIN + NL);     tft.print("Author: Pio i Niko");
  tft.setCursor(6, TOP_MARGIN + NL*3);   tft.print("Makeover: 2025");
  tft.setCursor(6, TOP_MARGIN + NL*4);   tft.print("Make year: 2000");
  tft.setCursor(6, TOP_MARGIN + NL*6);   tft.print("More information,");
  tft.setCursor(6, TOP_MARGIN + NL*7);   tft.print("documentation and");
  tft.setCursor(6, TOP_MARGIN + NL*8);   tft.print("software can be");
  tft.setCursor(6, TOP_MARGIN + NL*9);   tft.print("found at:");
  tft.setCursor(6, TOP_MARGIN + NL*11);  tft.print("placeholder.pl");
}

void drawCoinsPage() {
  tft.setTextWrap(false);
  drawHeader("Coin counter");
  tft.setTextColor(COLOR_FG);
  tft.setCursor(LEFT_MARGIN, TOP_MARGIN);
  tft.print("2 PLN: ");  tft.print(coins2);
  tft.setCursor(LEFT_MARGIN, TOP_MARGIN + LINE_H);
  tft.print("5 PLN: ");  tft.print(coins5);
}


// policz ile wierszy mieści się na ekranie (header + footer odjęte)
static inline int coinlogLinesPerPage() {
  int freeH = tft.height() - TOP_MARGIN - FOOTER_H;
  int rows  = freeH / LINE_H;
  if (rows < 1) rows = 1;
  return rows;
}

// buduje pełny tekst wpisu tak jak na ekranie loga
static inline void coinlogBuildLine(int globalIdx, char* out, size_t outsz) {
  CoinLogEntry e;
  if (!coinlogGetNewest(globalIdx, e)) { out[0] = 0; return; }
  DateTime dt(e.ts);
  snprintf(out, outsz, "%02d: %dzl %02d.%02d.%04d %02d:%02d:%02d",
           globalIdx + 1, (int)e.denom,
           dt.day(), dt.month(), dt.year(),
           dt.hour(), dt.minute(), dt.second());
}

// rysuje tylko jeden wiersz (globalIdx widoczny na ekranie); używa marquee dla zaznaczonego
// rysuje tylko jeden wiersz (globalIdx widoczny na ekranie); marquee dla zaznaczonego
void coinlogDrawRow(int globalIdx) {
  const int rows = coinlogLinesPerPage();
  int i = globalIdx - coinLogTop;
  if (i < 0 || i >= rows) return;

  const int16_t topY  = coinlogTopYBase();
  const int16_t leftX = coinlogLeftXBase();
  const int16_t y     = topY + i * LINE_H;
  const bool isSelected = (globalIdx == selCoinLog);

  char line[64];
  coinlogBuildLine(globalIdx, line, sizeof(line));

  if (isSelected) {
    drawVerticalGradient(0, y - 2, tft.width(), LINE_H, COLOR_SEL_BG, COLOR_BG2);
    tft.setTextColor(COLOR_SEL_FG);

    int availW = tft.width() - leftX - 2;
    int textW  = 6 * (int)strlen(line);
    if (textW <= availW) {
      tft.setCursor(leftX, y + SELECT_OFFSET_Y);
      tft.print(line);
    } else {
      const int spacer = 24;
      int x0 = leftX - coinlogMarqueeX;
      tft.setCursor(x0, y + SELECT_OFFSET_Y);                  tft.print(line);
      tft.setCursor(x0 + textW + spacer, y + SELECT_OFFSET_Y); tft.print(line);
    }
  } else {
    blitMenuBGStrip(y - 2, LINE_H);
    tft.setTextColor(COLOR_FG);
    tft.setCursor(leftX, y + SELECT_OFFSET_Y);
    tft.print(line);
  }
}





void drawCoinLogScreen() {
  // najpierw tapeta, potem header (by gradient nie został przykryty)
  drawMenuBackground();
  //drawHeader("Latest entries");

  const int rows = coinlogLinesPerPage();
  const int totalDisplay = min(coinLogCount, LOG_MAX_SHOW);

  // pełne pierwsze malowanie okna
  for (int i = 0; i < rows; ++i) {
    int globalIdx = coinLogTop + i;
    if (globalIdx >= totalDisplay) break;
    coinlogDrawRow(globalIdx);
  }

  // zapamiętaj stan okna/selektora, by kolejne delikatne odświeżenia nie migały
  lastCoinLogTop = coinLogTop;
  lastSelCoinLog = selCoinLog;
}



// Szybkie odświeżenie samego okna listy (bez headera i bez globalnego BG)
static void coinlogRedrawWindowFast() {
  const int rows = 9; // coinlogLinesPerPage();
  const int totalDisplay = min(coinLogCount, LOG_MAX_SHOW);

  // --- Przesunięcia tylko dla tego widoku (zmniejszenie marginesów) ---
  const int COINLOG_SHIFT_UP   = 6;  // px w górę
  const int COINLOG_SHIFT_LEFT = 6;  // px w lewo

  const int topY  = max(0, TOP_MARGIN  - COINLOG_SHIFT_UP);
  const int leftX = max(0, LEFT_MARGIN - COINLOG_SHIFT_LEFT);

  // 1) Podklej tło pod całą strefą listy + zapas na gradient
  const int stripY = max(0, topY - 2);
  const int stripH = min(tft.height() - stripY, rows * LINE_H + 4);
  blitMenuBGStrip(stripY, stripH);

  // 2) Narysuj widoczne wiersze (gradient tylko dla zaznaczonego)
  tft.setTextWrap(false);
  tft.setTextSize(1);

  for (int i = 0; i < rows; ++i) {
    int globalIdx = coinLogTop + i;
    if (globalIdx >= totalDisplay) break;

    const int16_t y = topY + i * LINE_H;
    const bool isSelected = (globalIdx == selCoinLog);

    char line[64];
    coinlogBuildLine(globalIdx, line, sizeof(line));

    if (isSelected) {
      // Pas pod selekcję z bezpiecznym obcięciem, gdyby y-2 < 0
      int16_t gy = y - 2;
      int16_t gh = LINE_H;
      if (gy < 0) { gh += gy; gy = 0; }

      drawVerticalGradient(0, gy, tft.width(), gh, COLOR_SEL_BG, COLOR_BG2);
      tft.setTextColor(COLOR_SEL_FG);

      const int availW = tft.width() - leftX - 2;
      const int textW  = 6 * (int)strlen(line);
      if (textW <= availW) {
        tft.setCursor(leftX, y + SELECT_OFFSET_Y);
        tft.print(line);
      } else {
        const int spacer = 24;
        const int x0 = leftX - coinlogMarqueeX;
        tft.setCursor(x0, y + SELECT_OFFSET_Y);                  tft.print(line);
        tft.setCursor(x0 + textW + spacer, y + SELECT_OFFSET_Y); tft.print(line);
      }
    } else {
      // Zwykły wiersz
      tft.setTextColor(COLOR_FG);
      tft.setCursor(leftX, y + SELECT_OFFSET_Y);
      tft.print(line);
    }
  }

  // Stan na przyszłość
  lastCoinLogTop = coinLogTop;
  lastSelCoinLog = selCoinLog;
}


// rysowanie aktualne stany adc

void drawGoalCalThresholds() {
  drawHeader("ADC thresholds");

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_FG);

  int y = TOP_MARGIN;
  const int bigspace = LINE_H - 2;
  const int smallspace = 10;

  // ===== idle =====
  tft.setCursor(LEFT_MARGIN, y);
  tft.print("ADC idle: ");
  tft.print(adcIdle);
  y += smallspace;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("ADC both: ");
  tft.print(adcBoth);

  y += bigspace;

  // ===== A =====
  tft.setCursor(LEFT_MARGIN, y);
  tft.print("A min: ");
  tft.print(adcAmin);
  y += smallspace;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("A max: ");
  tft.print(adcAmax);
  y += smallspace;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("A center: ");
  tft.print(adcAcenter);

  y += bigspace;

  // ===== B =====
  tft.setCursor(LEFT_MARGIN, y);
  tft.print("B min: ");
  tft.print(adcBmin);
  y += smallspace;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("B max: ");
  tft.print(adcBmax);
  y += smallspace;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("B center: ");
  tft.print(adcBcenter);
}




void coinTotalsRebuildFromLog() {
  if (!g_hasSD) return;
  uint32_t c2 = 0, c5 = 0;

  File f = SD_MMC.open(COINLOG_PATH, FILE_READ);
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line.startsWith("unix_ts")) continue;

    int s1 = line.indexOf(';');
    if (s1 < 0) continue;
    int s2 = line.indexOf(';', s1 + 1);
    if (s2 < 0) s2 = line.length();

    int denom = line.substring(s1 + 1, s2).toInt();
    if      (denom == 2) c2++;
    else if (denom == 5) c5++;
  }
  f.close();

  coins2 = c2;
  coins5 = c5;
  // zapisz do plików liczników (tak jak standardowo)
  sdWriteU32(COIN2_PATH, coins2);
  sdWriteU32(COIN5_PATH, coins5);
}


// ====== Coin counter: totals + link do "Latest entries" ======
void drawCoinRoot() {
  drawHeader("Coin counter");
  tft.setTextWrap(false);
  tft.setTextSize(1);

  // totals z liczników (coins2/coins5 są ładowane w setup: sdGetCoinCounters)
  tft.setTextColor(COLOR_FG);
  int y = TOP_MARGIN;
  tft.setCursor(LEFT_MARGIN, y);
  tft.print("2 PLN: ");  tft.print(coins2);  y += LINE_H;

  tft.setCursor(LEFT_MARGIN, y);
  tft.print("5 PLN: ");  tft.print(coins5);  y += LINE_H + 6;

  // separator
  tft.drawFastHLine(0, y - 4, tft.width(), ST77XX_BLACK);

  // Jedyna pozycja: "Latest entries >"
  const char* label = "Latest entries >";
  if (selCoinRoot == 0) {
    drawVerticalGradient(0, y - 2, tft.width(), LINE_H, COLOR_SEL_BG, COLOR_BG2);
    tft.setTextColor(COLOR_SEL_FG);
  } else {
    tft.setTextColor(COLOR_FG);
  }
  tft.setCursor(LEFT_MARGIN, y + SELECT_OFFSET_Y);
  tft.print(label);
}





// ====================== 20) Rysowanie: statusy OK/ERR ======================
void showOK(const char* msg) {
  const int16_t h = 18;
  tft.fillRect(0, tft.height() - h, tft.width(), h, ST77XX_GREEN);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, tft.height() - h + 5);
  tft.print("OK: "); tft.print(msg);
}

void showERROR(const char* msg) {
  const int16_t h = 18;
  tft.fillRect(0, tft.height() - h, tft.width(), h, ST77XX_ORANGE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, tft.height() - h + 5);
  tft.print("ERR: "); tft.print(msg);
}

// ====================== 21) Komunikacja (placeholder) ======================
void sendToHost(const char* payload) {
  // Krótki ASCII payload (<=8 znaków) na ID 0x321 – czysto diagnostycznie
  if (!payload) return;
  size_t n = strnlen(payload, 8);
  canSendStd(0x321, (const uint8_t*)payload, (uint8_t)n);
}

// can: granie muzyki tetris
void canPlayTetrisMusic() {
  const uint8_t d[2] = { CAN_CMD_MINIGAME_AUDIO, CAN_AUDIO_PLAY_TETRIS };
  bool ok = canSendStd(CAN_ID_MINIGAME_AUDIO, d, 2);

  Serial.print("[CAN] Tetris music PLAY -> ");
  Serial.println(ok ? "OK" : "FAIL");
}

//can: zatrzymanie muzyki tetris
void canStopMinigameMusic() {
  const uint8_t d[2] = { CAN_CMD_MINIGAME_AUDIO, CAN_AUDIO_STOP };
  bool ok = canSendStd(CAN_ID_MINIGAME_AUDIO, d, 2);

  Serial.print("[CAN] Tetris music STOP -> ");
  Serial.println(ok ? "OK" : "FAIL");
}

// ====================== 22) Wejście: debounce ======================
void updateButton(Button& b) {
  bool r = digitalRead(b.pin);
  if (r != b.lastRead) {
    b.lastRead = r;
    b.lastChangeMs = millis();
  }
  if ((millis() - b.lastChangeMs) > DEBOUNCE_MS && r != b.stableState) {
    b.stableState = r;
    if (b.stableState == HIGH) { 
      b.armed = true; 
      b.repeatAtMs = 0;                 // puszczony -> stop auto-repeat
    } else { // LOW (wciśnięty)
      b.repeatAtMs = millis() + REPEAT_DELAY_MS;  // ustaw start auto-repeat
    }
  }
}

bool pressedNow(Button& b) {
  if (b.stableState == LOW && b.armed) {
    b.armed = false;
    return true;
  }
  return false;
}
// ====================== 23) Helpers: wrapPrev/Next ======================
static inline int wrapPrev(int idx, int count) { return (idx > 0) ? idx - 1 : count - 1; }
static inline int wrapNext(int idx, int count) { return (idx < count - 1) ? idx + 1 : 0; }







// ====================== 24) TFT: rotacja z wymuszonym RGB ======================
void setRotationKeepRGB(uint8_t r) {
  tft.setRotation(r);
  uint8_t mad = 0;
  switch (r & 3) {
    case 0: mad = ST77XX_MADCTL_MX | ST77XX_MADCTL_MY; break;
    case 1: mad = ST77XX_MADCTL_MY | ST77XX_MADCTL_MV; break;
    case 2: mad = 0;                                   break;
    case 3: mad = ST77XX_MADCTL_MX | ST77XX_MADCTL_MV; break;
  }
  mad |= 0x00; // ST77XX_MADCTL_RGB
  tft.sendCommand(ST77XX_MADCTL, &mad, 1);
}

// ====================== 25) Footer: zegar (data+czas) ======================
void drawFooterClock(bool force) {
  static uint16_t lastMinute = 0xFFFF;
  uint16_t y = tft.height() - FOOTER_H;

  uint32_t nowUnix = systemUnix();
  DateTime now(nowUnix);

  uint16_t currentMinute = now.hour() * 60 + now.minute();
  if (!force && currentMinute == lastMinute) return;
  lastMinute = currentMinute;

  // tło jak header
  drawVerticalGradient(0, y, tft.width(), FOOTER_H, purpleD, purple);

  char dateBuf[16];
  char timeBuf[8];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", now.year(), now.month(), now.day());
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", now.hour(), now.minute());

  // data z lewej
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(7, y + 5);
  tft.print(dateBuf);

  // godzina z prawej
  int16_t textW = 6 * strlen(timeBuf);
  int16_t x = tft.width() - textW - 7;
  if (x < 64) x = 64; // nie wjeżdżaj na datę

  tft.setCursor(x, y + 5);
  tft.print(timeBuf);
}

// ====================== 26) Redraw: wg aktualnego ekranu ======================
void redraw() {
  // Tła dla ekranów: wszystkie menu, oprócz pełnoekranowych (PLAY, MUSICROUND, CONFIRM, ABOUT)
  bool needsMenuBG =
    !(current == SCR_PLAY ||
      current == SCR_MUSICROUND ||
      current == SCR_PLAYCONFIRM ||
      current == SCR_OTHER);

  if (needsMenuBG) drawMenuBackground();

  switch (current) {
    // --- menu główne i podmenu ---
    case SCR_MAIN:
      drawMenuList("Main Menu", mainItems, mainCount, selMain); break;
    case SCR_PLAYROOT:
      drawMenuList("Play", playRootItems, playRootCount, selPlayRoot); break;
    case SCR_SETTINGS:
      drawMenuList("Settings", settingsItems, settingsCount, selSettings); break;

    // --- pełnoekranowe gry ---
    case SCR_PLAY:
      drawFullGameMenu("Play a game", fullGameItems, fullGameCount, selPlay); break;
    case SCR_MUSICROUND:
      drawMusicRoundMenu("Music round", musicRoundItems, musicRoundCount, selMusic); break;

    // --- potwierdzenia ---
    case SCR_PLAYCONFIRM:
      drawPlayConfirm(); break;

    // --- ustawienia ---
    case SCR_GAMETIME:
      drawMenuList("Game time", timeItems, timeCount, selTime, gameTimeIdx, true);
      break;
    case SCR_GOALS:
      drawMenuList("Goals to win", goalsItems, goalsCount, selGoals, goalsIdx, true);
      break;
    case SCR_LIGHTS:
      drawMenuList("Light effects", lightsItems, lightsCount, selLights);
      break;
    case SCR_STROBES:
      drawMenuList("Strobes", onOffItems, onOffCount, selBin, strobesOn ? 0 : 1, true);
      break;
    case SCR_ANIMATION:
      drawMenuList("Animation", onOffItems, onOffCount, selBin, animOn ? 0 : 1, true);
      break;
    case SCR_STANDBY:
      drawMenuList("Stand-by lights", onOffItems, onOffCount, selBin, standbyOn ? 0 : 1, true);
      break;
    case SCR_UVLIGHT:
      drawMenuList("UV lights", onOffItems, onOffCount, selBin, uvOn ? 0 : 1, true);
      break;
    case SCR_GOALILLUM:
      drawMenuList("Goal illumination", onOffItems, onOffCount, selBin, goalIllumOn ? 0 : 1, true);
      break;
    case SCR_SETDT:
      if (!dt.loaded)
        setdtLoadFromRTC();
      drawSetDateTime();
      break;

      // kalibracja

    case SCR_GOALCAL:
      drawGoalCalMenu();
      break;

    case SCR_GOALCAL_RUN:
      drawGoalCalRun();
      break;

    case SCR_GOALCAL_THRESH:
      drawGoalCalThresholds();
      break;

      // puck lock
    case SCR_PUCKLOCK:
      drawPuckLockMenu();
      break;

    // --- pozostałe ---
    case SCR_OTHER:
      drawAboutScreen(); break;
    case SCR_COINS:
      drawCoinsPage(); break;
    case SCR_COINROOT:
      drawCoinRoot(); break;
    case SCR_COINLOG:
      coinlogRedrawWindowFast();break;
  }

  // Footer z zegarem: tylko na ekranach menu/ustawień (nie na fullscreen)
if (needsMenuBG && current != SCR_COINLOG) {
  drawFooterClock(true);
}
}
// ====================== 27) Nawigacja ekranów ======================
void enterScreen(Screen next) {
  if (current == next) return;
  if (backStackSize < 5) backStack[backStackSize++] = current;
  previous = current;         // <— zapamiętaj skąd weszliśmy
  current  = next;
}

void goBack() {
  if (backStackSize > 0) {
    previous = current;       // <— też aktualizuj przy „wstecz”
    current  = backStack[--backStackSize];
  }
}

void dropTopScreenIf(Screen s) {
  while (backStackSize > 0 && backStack[backStackSize - 1] == s) {
    backStackSize--;
  }
}



void switchScreenNoBack(Screen next) {
  previous = current;
  current = next;
}

// ====================== 28) Obsługa ustawień / aktywności ======================
void saveSettings() {
  if (!sdSaveSettings()) {
    showERROR("SD error");
  }
}

void registerInputActivity() {
  lastInputTime = millis();
  if (screenDimmed) {
    digitalWrite(LCD_PWR, HIGH); // ON
    delay(10);
    screenDimmed = false;
    delay(150);
  
redraw();

  }
}


// === Random bez powtórzeń: "shuffle-bag" dla Full i Music ===
static int8_t  bagFull[fullGameCount];
static uint8_t bagFullLeft = 0;
static int8_t  bagMusic[musicRoundCount];
static uint8_t bagMusicLeft = 0;

static int8_t lastFull = -1;
static int8_t lastMusic = -1;

static void refillBagFull() {
  for (uint8_t i = 0; i < fullGameCount; i++) bagFull[i] = i;
  for (int i = fullGameCount - 1; i > 0; i--) { int j = random(i + 1); int8_t t = bagFull[i]; bagFull[i] = bagFull[j]; bagFull[j] = t; }
  bagFullLeft = fullGameCount;
}
static void refillBagMusic() {
  for (uint8_t i = 0; i < musicRoundCount; i++) bagMusic[i] = i;
  for (int i = musicRoundCount - 1; i > 0; i--) { int j = random(i + 1); int8_t t = bagMusic[i]; bagMusic[i] = bagMusic[j]; bagMusic[j] = t; }
  bagMusicLeft = musicRoundCount;
}

// wybór z unikiem "tej samej co przed chwilą"
static uint8_t nextFullIdx() {
  if (bagFullLeft == 0) refillBagFull();
  uint8_t cand = bagFull[--bagFullLeft];
  if (fullGameCount > 1 && cand == (uint8_t)lastFull) {
    if (bagFullLeft > 0) { uint8_t swap = bagFull[0]; bagFull[0] = cand; cand = swap; }
    else { refillBagFull(); cand = bagFull[--bagFullLeft]; }
  }
  lastFull = cand;
  return cand;
}
static uint8_t nextMusicIdx() {
  if (bagMusicLeft == 0) refillBagMusic();
  uint8_t cand = bagMusic[--bagMusicLeft];
  if (musicRoundCount > 1 && cand == (uint8_t)lastMusic) {
    if (bagMusicLeft > 0) { uint8_t swap = bagMusic[0]; bagMusic[0] = cand; cand = swap; }
    else { refillBagMusic(); cand = bagMusic[--bagMusicLeft]; }
  }
  lastMusic = cand;
  return cand;
}

// obudówki: wybudź LCD, krótki toast, start gry po CAN
static void wakeDisplay() { registerInputActivity(); } // już masz tę funkcję – wznawia LCD i robi redraw :contentReference[oaicite:1]{index=1}

static void startRandomFullFromCoin() {
  wakeDisplay();
  uint8_t idx = nextFullIdx();                                  // nazwy/ilości: fullGameItems/fullGameCount :contentReference[oaicite:2]{index=2}
  char toast[40]; snprintf(toast, sizeof(toast), "FULL: %s", fullGameItems[idx]);
  showOK(toast);
  StartSendStatus st = canSendStartGame(GT_FULL, idx);          // używasz już w PLAYCONFIRM :contentReference[oaicite:3]{index=3}
  if      (st == START_SENT_ACK)    { delay(400); showOK("Sent, ACK OK"); }
  else if (st == START_SENT_NO_ACK) { delay(400); showERROR("No ACK"); }
  else if (st == START_COOLDOWN)    showERROR("Wait.");
  else                              showERROR("CAN error");
  delay(600);
  enterScreen(SCR_MAIN);
}

static void startRandomMusicFromCoin() {
  wakeDisplay();
  uint8_t idx = nextMusicIdx();                                 // nazwy/ilości: musicRoundItems/musicRoundCount :contentReference[oaicite:4]{index=4}
  char toast[40]; snprintf(toast, sizeof(toast), "MUSIC: %s", musicRoundItems[idx]);
  showOK(toast);
  StartSendStatus st = canSendStartGame(GT_MUSIC, idx);         // jak wyżej (ACK/timeout/cooldown) :contentReference[oaicite:5]{index=5}
  if      (st == START_SENT_ACK)    { delay(400); showOK("Sent, ACK OK"); }
  else if (st == START_SENT_NO_ACK) { delay(400); showERROR("No ACK"); }
  else if (st == START_COOLDOWN)    showERROR("Wait.");
  else                              showERROR("CAN error");
  delay(600);
  enterScreen(SCR_MAIN);
}





// ====================== 29) Datownik: przejścia i zapis do RTC ======================
void setdtNextFieldOrSave() {
  if (dt.cursor < 5) {
    dt.cursor++;
    return;
  }

  DateTime val(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  if (rtcReady) {
    rtc.adjust(val);
    rtcRunning = true;   // po adjust zegar „biegnie”
    rtcSyncToSystem();   // po zapisie aktualizujemy zegar systemowy
    showOK("RTC updated");
    delay(800);
  } else {
    showERROR("RTC not found");
    delay(800);
  }
}

void setdtPrevFieldOrExit() {
  if (dt.cursor > 0) {
    dt.cursor--;
  } else {
    goBack();
  }
}

// ====================== 30) Narzędzia diagnostyczne ======================
void checkPSRAM() {
  //Serial.begin(115200);
  delay(200);

  Serial.println("=== PSRAM check ===");
#if defined(BOARD_HAS_PSRAM)
  Serial.println("BOARD_HAS_PSRAM: defined (wlaczona w definicji plytki)");
#else
  Serial.println("BOARD_HAS_PSRAM: NOT defined");
#endif

  bool ok = psramFound();
  size_t total = ESP.getPsramSize();
  size_t freeb = ESP.getFreePsram();

  Serial.printf("psramFound(): %s\n", ok ? "YES" : "NO");
  Serial.printf("ESP.getPsramSize(): %u bytes\n", (unsigned)total);
  Serial.printf("ESP.getFreePsram(): %u bytes\n", (unsigned)freeb);

  // próba alokacji 1 MB w PSRAM:
  const size_t testSize = 1 * 1024 * 1024;
  uint8_t* p = (uint8_t*)ps_malloc(testSize);
  Serial.printf("ps_malloc(1 MB): %s\n", p ? "OK" : "FAILED");
  if (p) {
    memset(p, 0xAA, testSize);
    free(p);
  }
  Serial.println("====================");
}

void i2cScan() {
  
  delay(50);
  Serial.println("\nI2C scan...");
  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  found: 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("  (nothing found)");
  Serial.println("----");
}

/*
// --- ADC35 (drabinka monet) : klasyfikacja napięcia na 0/2/5 zł ---
int classifyCoinADC(int raw) {
  // ESP32 ADC bywa nieliniowy – progi szerokie i łatwe do korekty
  // 12 bit: 0..4095
  if (raw > 3200) return 0;                 // ~3.3V: brak monety (pull-up)
  if (raw > 1500 && raw < 2700) return 2;   // ~1.6V: 2 zł (100k:100k)
  if (raw > 700  && raw < 1400) return 5;   // ~1.05V: 5 zł (100k:47k)
  return -1;                                 // niepewne / szum
}*/

//kalibracja goli
void goalCalStart(GoalCalMode mode) {
  g_goalCalUiDirty = true;
  g_goalCal.lockedAt9 = false;
  g_goalCal.active = true;
  g_goalCal.mode = mode;
  g_goalCal.scored = 0;
  g_goalCal.target = 10;
  g_goalCal.phaseStartMs = millis();
  memset(g_goalCal.samples, 0, sizeof(g_goalCal.samples));

  // tylko przy A czekamy 3 s i mierzymy idle
  if (mode == GCAL_A) {
    g_goalCal.phase = GCAL_WAIT_IDLE;
  } else {
    // przy B od razu przechodzimy do zbierania goli
    g_goalCal.phase = GCAL_RUNNING;
  }

  Serial.printf("[GCAL] START mode=%s\n", mode == GCAL_A ? "A" : "B");
  if (mode == GCAL_A) {
    Serial.println("[GCAL] Wait 3 sec for idle measurement");
  } else {
    Serial.println("[GCAL] Skip idle measurement for B");
  }

  uint8_t calCmd = (mode == GCAL_A) ? CAN_CMD_GOALCAL_START_A : CAN_CMD_GOALCAL_START_B;
  uint8_t c[1] = { calCmd };
  canSendStd(CAN_ID_GOALCAL_CMD, c, 1);
  Serial.printf("[GCAL] sent START_%s to main\n", mode == GCAL_A ? "A" : "B");

  // od razu włącz dmuchawę i odblokuj puck locki
  Serial.println("[GCAL] blower ON (todo CAN)");

  uint8_t d[3] = { CAN_CMD_PUCKLOCK_NOW, CAN_PUCK_UNLOCK, 0x03 };
  canSendStd(CAN_ID_PUCKLOCK_CMD, d, 3);
  Serial.println("[GCAL] puck locks -> UNLOCK ALL");

  enterScreen(SCR_GOALCAL_RUN);
}

bool adcOrderOk =
  (adcBoth < adcBcenter) &&
  (adcBcenter < adcAcenter) &&
  (adcAcenter < adcIdle);

void goalCalFinish() {

  Serial.printf("[GCAL] COMPLETE mode=%s\n", g_goalCal.mode == GCAL_A ? "A" : "B");

  // ===== pokaż wszystkie próbki =====
  Serial.println("[GCAL] samples:");
  for (int i = 0; i < g_goalCal.target; i++) {
    Serial.printf("[GCAL] sample[%d] = %u\n", i, g_goalCal.samples[i]);
  }

  // ===== policz center =====
  uint32_t sum = 0;
  for (int i = 0; i < g_goalCal.target; i++) {
    sum += g_goalCal.samples[i];
  }

  uint16_t center = sum / g_goalCal.target;
  Serial.printf("[GCAL] center=%u\n", center);

  // ===== jeśli skończyliśmy A, zapisz A i od razu przejdź do B =====
  if (g_goalCal.mode == GCAL_A) {
    adcAcenter = center;
    Serial.printf("[ADC] stored A center = %u\n", adcAcenter);

    showOK("Calibrate B now");
    delay(1200);
    redraw();
    

    goalCalStart(GCAL_B);
    redraw();
    return;
  }

  // ===== jeśli skończyliśmy B, zapisz B =====
  if (g_goalCal.mode == GCAL_B) {
    adcBcenter = center;
    Serial.printf("[ADC] stored B center = %u\n", adcBcenter);
  }

  // ===== sprawdź poprawność kolejności ADC =====
  bool adcOrderOk =
    (adcBoth < adcBcenter) &&
    (adcBcenter < adcAcenter) &&
    (adcAcenter < adcIdle);

  if (!adcOrderOk) {
  Serial.printf(
    "[GCAL] ERROR wrong calibration order both=%u B=%u A=%u idle=%u\n",
    adcBoth, adcBcenter, adcAcenter, adcIdle
  );

  resetAdcDefaults();

  showERROR("Wrong goal sides");
  delay(1200);
  redraw();
goalCalSendCancelToMain();
g_goalCal = GoalCalState{};
dropTopScreenIf(SCR_GOALCAL_RUN);
dropTopScreenIf(SCR_GOALCAL);
switchScreenNoBack(SCR_GOALCAL);
redraw();
  return;
}

  // ===== przelicz progi =====
  uint16_t midAB = (adcAcenter + adcBcenter) / 2;
  uint16_t midAI = (adcAcenter + adcIdle) / 2;

  adcBmin = 700;      // ustalona bezpieczna granica
  adcBmax = midAB;

  adcAmin = midAB;
  adcAmax = midAI;

  Serial.println("[ADC] recalculated thresholds:");
  Serial.printf("B: %u - %u (center %u)\n", adcBmin, adcBmax, adcBcenter);
  Serial.printf("A: %u - %u (center %u)\n", adcAmin, adcAmax, adcAcenter);

  adcSettingsSave();

// ===== wyślij nowe progi do main =====
sendAdcThresholdsToMain();
goalCalSendCancelToMain();

Serial.println("[GCAL] calibration OK");

showOK("Goal calibrated");
redraw();
delay(1000);

// wyczyść stan kalibracji
g_goalCal = GoalCalState{};
dropTopScreenIf(SCR_GOALCAL_RUN);
dropTopScreenIf(SCR_GOALCAL);
switchScreenNoBack(SCR_GOALCAL);
redraw();
}
void goalCalTick() {
  if (!g_goalCal.active) return;

  if (g_goalCal.phase == GCAL_WAIT_IDLE) {
    if (millis() - g_goalCal.phaseStartMs >= 3000) {
      g_goalCal.phase = GCAL_RUNNING;
      g_goalCal.phaseStartMs = millis();
      g_goalCalUiDirty = true;

      Serial.printf("[GCAL] idle measured, begin scoring mode=%s\n",
                    g_goalCal.mode == GCAL_A ? "A" : "B");
    }
  }

  if (g_goalCal.phase == GCAL_RUNNING) {
    if (g_goalCal.scored == 9 && !g_goalCal.lockedAt9) {
      uint8_t d[3] = { CAN_CMD_PUCKLOCK_NOW, CAN_PUCK_LOCK, 0x03 };
      canSendStd(CAN_ID_PUCKLOCK_CMD, d, 3);
      Serial.println("[GCAL] scored=9 -> puck locks LOCK ALL");
      g_goalCal.lockedAt9 = true;
    }

    if (g_goalCal.scored >= g_goalCal.target) {
      goalCalFinish();
      g_goalCalUiDirty = true;
    }
  }

  if (g_goalCal.phase == GCAL_DONE) {
    if (millis() - g_goalCal.phaseStartMs >= 1500) {
      Serial.println("[GCAL] return to Goal calibration menu");
      g_goalCal = GoalCalState{};
      g_goalCalUiDirty = true;
      enterScreen(SCR_GOALCAL);
    }
  }
}


void goalCalOnAdcSampleFromMain(uint16_t rawAdc) {
  if (!g_goalCal.active) return;
  if (g_goalCal.phase != GCAL_RUNNING) return;
  if (g_goalCal.scored >= g_goalCal.target) return;

  g_goalCal.samples[g_goalCal.scored] = rawAdc;
  g_goalCal.scored++;
  g_goalCalUiDirty = true;
  lastInputTime = millis();

  Serial.printf("[GCAL] sample %u/%u mode=%s rawAdc=%u\n",
                g_goalCal.scored,
                g_goalCal.target,
                g_goalCal.mode == GCAL_A ? "A" : "B",
                rawAdc);
}





void goalCalCanPoll() {
  twai_message_t msg;

  while (twai_receive(&msg, 0) == ESP_OK) {

    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_GOALCAL_DATA &&
        msg.data_length_code >= 3) {

      uint8_t cmd = msg.data[0];
      uint16_t raw = ((uint16_t)msg.data[1] << 8) | msg.data[2];

      if (cmd == CAN_CMD_GOALCAL_IDLE) {
        adcIdle = raw;
        g_goalCalUiDirty = true;
        Serial.printf("[GCAL] idle from main = %u\n", raw);
      }
      else if (cmd == CAN_CMD_GOALCAL_SAMPLE) {
        goalCalOnAdcSampleFromMain(raw);
      }
    }

    // tu później można dopisać inne RX-y, jeśli będą potrzebne
  }
}






static inline bool gameIsBusy() {
  return (int32_t)(millis() - gameBusyUntilMs) < 0;
}

static bool coinQPush(uint8_t denom) {
  if (coinQCount >= COINQ_MAX) return false;
  coinQ[coinQTail] = denom;
  coinQTail = (uint8_t)((coinQTail + 1) % COINQ_MAX);
  coinQCount++;
  return true;
}

static bool coinQPop(uint8_t &denomOut) {
  if (!coinQCount) return false;
  denomOut = coinQ[coinQHead];
  coinQHead = (uint8_t)((coinQHead + 1) % COINQ_MAX);
  coinQCount--;
  return true;
}

// ile blokować po starcie (żeby „kolejne monety” nie odpalały od razu)
static uint32_t busyMsForDenom(uint8_t denom) {
  if (denom == 5) {
    // Full Game: czas z ustawień (gameTimeIdx) + mały bufor
    return (uint32_t)gameTimeMinutes() * 60UL * 1000UL + 10UL * 1000UL;
  } else {
    // Music Round: przyjmijmy 90 s (zmień jak chcesz)
    return 90UL * 1000UL;
  }
}

static void handleCoinDetected(uint8_t denom) {
  registerInputActivity();

  // 1) Zlicz + log na SD (masz już incCoin2/incCoin5)
  bool ok = false;
  if (denom == 2) ok = incCoin2("hw_gpio");
  else            ok = incCoin5("hw_gpio");

  if (ok) {
    if (denom == 2) showOK("Dodano: 2 zl");
    else            showOK("Dodano: 5 zl");
  } else {
    if (denom == 2) showERROR("2 zl (SD/RAM)");
    else            showERROR("5 zl (SD/RAM)");
  }

  // 2) Wrzuć do kolejki
  if (!coinQPush(denom)) {
    showERROR("Coin queue FULL");
    return;
  }
}

// próba odpalenia kolejnej gry z kolejki
static void processCoinQueue() {
  if (!coinQCount) return;

  // jeśli „gra trwa” – nie startujemy kolejnej
  if (gameIsBusy()) return;

  // jeśli CAN ma cooldown po braku ACK – też czekamy (to Twoja logika CAN)
  if ((int32_t)(millis() - canCooldownUntil) < 0) return;

  uint8_t denom = 0;
  if (!coinQPop(denom)) return;

  // odpal
  if (denom == 2) {
    startRandomMusicFromCoin(); // masz już gotowe
  } else {
    startRandomFullFromCoin();  // masz już gotowe
  }

  // po starcie ustawiamy „busy” (soft)
  gameBusyUntilMs = millis() + busyMsForDenom(denom);
}



void deleteSnakeRecord() {
  gamePrefs.begin("snake", false);
  gamePrefs.remove("snaketopScore");
  gamePrefs.end();

  Serial.println("Snake record deleted. Reboot system.");
}
void deleteTetrisRecord() {
  gamePrefs.begin("tetris", false);
  gamePrefs.remove("tetristopScore");
  gamePrefs.end();

  Serial.println("Tetris record deleted. Reboot system.");
}

void showNopeAndReturn() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds("NOPE", 0, 0, &x1, &y1, &w, &h);

  int x = (tft.width() - w) / 2;
  int y = (tft.height() - h) / 2;

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(x, y);
  tft.print("NOPE");

  delay(150);
  redraw();
}

void checkSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "deletesnakerecord") {
    deleteSnakeRecord();
  }
  if (cmd == "deletetetrisrecord") {
  deleteTetrisRecord();
}
}






// ====================== 31) Setup ======================
void setup() {
  Serial.begin(115200);
  
  
  
    

  


  // --- LittleFS dla teł/RAW (lokalne tła)
  if (!LittleFS.begin()) {
    Serial.println("Błąd: Nie mogę zamontować LittleFS!");
    while (true); // hard-stop
  }
  Serial.println("LittleFS zamontowany OK!");
  Serial.println(LittleFS.exists("/bg_teensies.raw") ? "jest" : "brak");


   // ADC WYŁĄCZONE (GPIO35 na S3 nie jest ADC)
  //  analogReadResolution(12);
 // analogSetPinAttenuation(PIN, ADC_11db);  

pinMode(COIN_2_PIN, INPUT_PULLUP);
pinMode(COIN_5_PIN, INPUT_PULLUP);




  // --- I2C + RTC ---
  Wire.begin(I2C_SDA, I2C_SCL);   // SDA, SCL
  Wire.setClock(100000);
  delay(10);
  i2cScan();

  rtcReady   = rtc.begin(&Wire);
  delay(5);
  rtcRunning = rtcReady && !rtc.lostPower();
  DateTime build(F(__DATE__), F(__TIME__));
  DateTime now = (rtcReady ? rtc.now() : build);

  // zapisz ostatni odczyt (Preferences)
  
  Preferences pr;
  pr.begin("rtc", false);
  uint32_t last = pr.getUInt("lastRtc", 0);
  pr.putUInt("lastRtc", (uint32_t)now.unixtime());
  pr.end();

  // ostrzeżenia RTC
  rtcWarnNotFound   = !rtcReady;
  bool looksReset   = (now.unixtime() + 60 < build.unixtime());
  bool wentBack     = last && (now.unixtime() + 60 < last);
  rtcWarnChangeBat  = (rtcReady && (!rtcRunning || looksReset || wentBack));

  rtcSyncToSystem();

  canInit();
  sendAdcThresholdsToMain();
  canStopMinigameMusic();
   goalCalSendCancelToMain();

  randomSeed(esp_random() ^ micros() ^ (systemUnix() << 8));
  adcSettingsLoad();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // LED zgaszona na starcie

  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  // pinMode(TFT_RST, OUTPUT);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // SCK, MISO(nie używasz), MOSI, SS
  digitalWrite(TFT_CS, HIGH);                // odznacz TFT

  tft.initR(INITR_BLACKTAB);

  setRotationKeepRGB(0);

  tft.fillScreen(ST77XX_BLACK);

  redraw();

  bool sdOk = sdInitOnce();

  if (sdOk)
    sdGetCoinCounters(coins2, coins5);

  if (sdOk)
    sdLoadSettings();


  // --- GPIO + TFT ---
  pinMode(LCD_PWR, OUTPUT);
  digitalWrite(LCD_PWR, HIGH);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  lastInputTime = millis();
//tft.initR(INITR_BLACKTAB);
  setRotationKeepRGB(0);
  digitalWrite(TFT_CS, HIGH);

  // --- Najpierw narysuj ekran, potem wyświetl błędy (sticky na dole)
redraw();

  // --- SD (ustawienia + monety)
  if (!sdInitOnce()) {
    showERROR("SD card error");  // nie blokujemy działania
  }
  sdGetCoinCounters(coins2, coins5);
  sdLoadSettings();

  // --- Błędy RTC (po redraw, żeby „zostały” na pasku)
  if (rtcWarnNotFound) {
    showERROR("RTC not found");
  } else if (rtcWarnChangeBat) {
    showERROR("Change RTC bat.");
  }
twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED, nullptr);
    // UWAGA: tylko raz, synchronizacja pliku csv wrzutnika monet. Potem wyłączamy
  //coinTotalsRebuildFromLog();


  Serial.println();
Serial.println("To delete Games Records type:");
Serial.println("'deletesnakerecord' for Snake");
Serial.println("'deletetetrisrecord' for Tetris");
Serial.println();

}







// ====================== 32) Loop ======================
void loop() {

checkSerialCommands();


// ===== COIN GPIO scan (falling edge + debounce) =====
{
  const uint32_t now = millis();

  bool r2 = digitalRead(COIN_2_PIN);
  bool r5 = digitalRead(COIN_5_PIN);

  // wykrywanie zmian do debounce
  if (r2 != last2Read) { last2Read = r2; last2ChangeMs = now; }
  if (r5 != last5Read) { last5Read = r5; last5ChangeMs = now; }

  // jeśli stan niski utrzymał się COIN_DEBOUNCE_MS, traktuj jako „impuls”
  static bool latched2 = false;
  static bool latched5 = false;

  // 2 PLN
  if (!latched2) {
    if (last2Read == LOW && (now - last2ChangeMs) >= COIN_DEBOUNCE_MS) {
      latched2 = true;                 // złap impuls
      handleCoinDetected(2);
    }
  } else {
    // odblokuj dopiero po powrocie na HIGH
    if (last2Read == HIGH && (now - last2ChangeMs) >= COIN_DEBOUNCE_MS) {
      latched2 = false;
    }
  }

  // 5 PLN
  if (!latched5) {
    if (last5Read == LOW && (now - last5ChangeMs) >= COIN_DEBOUNCE_MS) {
      latched5 = true;
      handleCoinDetected(5);
    }
  } else {
    if (last5Read == HIGH && (now - last5ChangeMs) >= COIN_DEBOUNCE_MS) {
      latched5 = false;
    }
  }
}

// spróbuj odpalić to, co w kolejce
processCoinQueue();


// auto-wygaszanie po 30 s
if (current == SCR_GOALCAL_RUN && g_goalCal.active) {
  lastInputTime = millis();
}

if (!screenDimmed && (millis() - lastInputTime > 180000UL)) {
  digitalWrite(LCD_PWR, LOW);  // OFF
  screenDimmed = true;
  current = SCR_MAIN;
  selMain = 0;
  tft.fillScreen(ST77XX_BLACK);
}

bool buttonPressed =
  digitalRead(BTN_UP) == LOW ||
  digitalRead(BTN_DOWN) == LOW ||
  digitalRead(BTN_SELECT) == LOW ||
  digitalRead(BTN_BACK) == LOW;

digitalWrite(LED_PIN, buttonPressed ? HIGH : LOW);


  // aktualizacja przycisków
  updateButton(bUp);
  updateButton(bDown);
  updateButton(bSelect);
  updateButton(bBack);


bool upNow     = (bUp.stableState == LOW);
bool downNow   = (bDown.stableState == LOW);
bool selectNow = (bSelect.stableState == LOW);
bool backNow   = (bBack.stableState == LOW);

bool anyHeld = upNow || downNow || selectNow || backNow;

if (!anyHeld) {
  comboReady = true;
}




// ===== SECRET COMBOS =====




int heldCount = 0;
if (upNow)     heldCount++;
if (downNow)   heldCount++;
if (selectNow) heldCount++;
if (backNow)   heldCount++;

if (!snakeActive && !tetrisActive && comboReady && heldCount == 2) {

  comboReady = false;   // zablokuj do czasu puszczenia wszystkich przycisków

  // poprawne combo: Snake
  if (selectNow && backNow) {
    snakeActive = true;
    snakeComboReleased = false;

    tft.fillScreen(ST77XX_BLACK);

    bSelect.armed = false;
    bBack.armed = false;
    return;
  }

  // poprawne combo: Tetris
  if (upNow && downNow) {
    tetrisActive = true;
    tetrisComboReleased = false;

    tft.fillScreen(ST77XX_BLACK);

    bUp.armed = false;
    bDown.armed = false;
    return;
  }

  // każda inna para = NOPE
  showNopeAndReturn();

  bUp.armed = false;
  bDown.armed = false;
  bSelect.armed = false;
  bBack.armed = false;
  return;
}

if (snakeActive) {
  runSnake();
  return;
}

if (tetrisActive) {
  runTetris();
  return;
}

  bool need = false;

// --- Nawigacja GÓRA ---
if (pressedNow(bUp) || repeatNow(bUp)) {
  registerInputActivity();
  switch (current) {
    // Menu główne / katalogi
    case SCR_MAIN:       selMain      = wrapPrev(selMain,      mainCount);       break;
    case SCR_PLAYROOT:   selPlayRoot  = wrapPrev(selPlayRoot,  playRootCount);   break;
    case SCR_SETTINGS:   selSettings  = wrapPrev(selSettings,  settingsCount);   break;

    // Listy gier
    case SCR_PLAY:       selPlay      = wrapPrev(selPlay,      fullGameCount);   break;
    case SCR_MUSICROUND: selMusic     = wrapPrev(selMusic,     musicRoundCount); break;

    // Ustawienia czasu i goli
    case SCR_GAMETIME:   selTime      = wrapPrev(selTime,      timeCount);       break;
    case SCR_GOALS:      selGoals     = wrapPrev(selGoals,     goalsCount);      break;

    // Goal Calibration
    case SCR_GOALCAL:
      selGoalCal = wrapPrev(selGoalCal, goalCalCount);
      need = true;
      break;

    // Lista efektów świetlnych
    case SCR_LIGHTS:     selLights    = wrapPrev(selLights,    lightsCount);     break;

    // ON/OFF ekrany
    case SCR_STROBES:
    case SCR_ANIMATION:
    case SCR_STANDBY:
    case SCR_UVLIGHT:
    case SCR_GOALILLUM:  selBin       = wrapPrev(selBin,       onOffCount);      break;

    // Potwierdzenie YES/NO
    case SCR_PLAYCONFIRM: selConfirm = (selConfirm + 1) % 2;                     break;

  // puck lock
    case SCR_PUCKLOCK:
    selPuckLock = wrapPrev(selPuckLock, puckLockCount);                          break;

    // Ustawianie daty/czasu
    case SCR_SETDT:      setdtApplyDelta(+1);                                    break;

    // Statyczne
    case SCR_OTHER:
    case SCR_COINS:                                                              break;

    // Coin Counter
    case SCR_COINROOT:                                                           break;

    case SCR_COINLOG: {
      const int totalDisplay = min(coinLogCount, LOG_MAX_SHOW);
      if (totalDisplay > 0) {
        const int rows   = coinlogLinesPerPage();
        const int oldSel = selCoinLog;

        if (selCoinLog == 0) {
          selCoinLog = totalDisplay - 1;
          const int newTop = max(0, totalDisplay - rows);
          if (newTop != coinLogTop) {
            coinLogTop = newTop;
            coinlogRedrawWindowFast();   // przewinięto okno – narysuj okno raz
          }
        } else {
          selCoinLog--;
          if (selCoinLog < coinLogTop) {
            coinLogTop = selCoinLog;
            coinlogRedrawWindowFast();   // przewinięto okno – narysuj okno raz
          } else {
            // bez przewijania – odśwież tylko dwie linie
            coinlogDrawRow(oldSel);
            coinlogDrawRow(selCoinLog);
          }
        }
        coinlogMarqueeReset();
      }
      break;
    }

    default: break;
  }
  need = true;
}

// --- Nawigacja DÓŁ ---
if (pressedNow(bDown) || repeatNow(bDown)) {
  registerInputActivity();
  switch (current) {
    // Menu główne / katalogi
    case SCR_MAIN:       selMain      = wrapNext(selMain,      mainCount);       break;
    case SCR_PLAYROOT:   selPlayRoot  = wrapNext(selPlayRoot,  playRootCount);   break;
    case SCR_SETTINGS:   selSettings  = wrapNext(selSettings,  settingsCount);   break;

    // Listy gier
    case SCR_PLAY:       selPlay      = wrapNext(selPlay,      fullGameCount);   break;
    case SCR_MUSICROUND: selMusic     = wrapNext(selMusic,     musicRoundCount); break;

    // Ustawienia czasu i goli
    case SCR_GAMETIME:   selTime      = wrapNext(selTime,      timeCount);       break;
    case SCR_GOALS:      selGoals     = wrapNext(selGoals,     goalsCount);      break;

    // Goal Calibration
    case SCR_GOALCAL:
      selGoalCal = wrapNext(selGoalCal, goalCalCount);
      need = true;
      break;

    // Lista efektów świetlnych
    case SCR_LIGHTS:     selLights    = wrapNext(selLights,    lightsCount);     break;

    // ON/OFF ekrany
    case SCR_STROBES:
    case SCR_ANIMATION:
    case SCR_STANDBY:
    case SCR_UVLIGHT:
    case SCR_GOALILLUM:  selBin       = wrapNext(selBin,       onOffCount);      break;

    // Potwierdzenie YES/NO
    case SCR_PLAYCONFIRM: selConfirm = (selConfirm + 1) % 2;                     break;

    // Ustawianie daty/czasu
    case SCR_SETDT:      setdtApplyDelta(-1);                                    break;

    //puck lock
    case SCR_PUCKLOCK:
    selPuckLock = wrapNext(selPuckLock, puckLockCount);                          break;

    // Statyczne
    case SCR_OTHER:
    case SCR_COINS:                                                              break;

    // Coin counter
    case SCR_COINROOT:                                                           break;

    case SCR_COINLOG: {
      const int totalDisplay = min(coinLogCount, LOG_MAX_SHOW);
      if (totalDisplay > 0) {
        const int rows   = coinlogLinesPerPage();
        const int oldSel = selCoinLog;

        if (selCoinLog >= totalDisplay - 1) {
          selCoinLog = 0;
          if (coinLogTop != 0) {
            coinLogTop = 0;
            coinlogRedrawWindowFast();  // przewinięto okno – narysuj okno raz
          }
        } else {
          selCoinLog++;
          if (selCoinLog >= coinLogTop + rows) {
            coinLogTop = selCoinLog - (rows - 1);
            coinlogRedrawWindowFast();  // przewinięto okno – narysuj okno raz
          } else {
            // bez przewijania – odśwież tylko dwie linie
            coinlogDrawRow(oldSel);
            coinlogDrawRow(selCoinLog);
          }
        }
        coinlogMarqueeReset();
      }
      break;
    }

    default: break;
  }
  need = true;
}

// --- Zatwierdzanie (SELECT) ---
if (pressedNow(bSelect)) {
  registerInputActivity();

  switch (current) {

    // ───────── Main menu ─────────
    case SCR_MAIN:
      if      (selMain == 0) { enterScreen(SCR_PLAYROOT);  selPlayRoot  = 0; need = true; }
      else if (selMain == 1) { enterScreen(SCR_SETTINGS);  selSettings  = 0; need = true; }
      else if (selMain == 2) { enterScreen(SCR_OTHER);                     need = true; }
      else if (selMain == 3) { enterScreen(SCR_COINROOT); selCoinRoot   = 0; need = true; }
      else if (selMain == 4) {
        bool ok = incCoin5("test");
        if (ok) showOK("Dodano: 5 zl"); else showERROR("5 zl (brak SD, tylko RAM)");
        delay(400); need = true;
      }
      else if (selMain == 5) {
        bool ok = incCoin2("test");
        if (ok) showOK("Dodano: 2 zl"); else showERROR("2 zl (brak SD, tylko RAM)");
        delay(400); need = true;
      }
      break;

    // ───────── Play root → listy gier ─────────
    case SCR_PLAYROOT:
      if      (selPlayRoot == 0) { enterScreen(SCR_PLAY);       selPlay  = 0; }
      else if (selPlayRoot == 1) { enterScreen(SCR_MUSICROUND); selMusic = 0; }
      need = true; break;

    // ───────── Settings → podmenu ─────────
    case SCR_SETTINGS:
      if      (selSettings == 0) { enterScreen(SCR_GAMETIME);  selTime = gameTimeIdx; }
      else if (selSettings == 1) { enterScreen(SCR_GOALS);     selGoals = goalsIdx; }
      else if (selSettings == 2) { enterScreen(SCR_LIGHTS); }
      else if (selSettings == 3) { enterScreen(SCR_PUCKLOCK);  selPuckLock = puckLockEnabled ? 0 : 1; }
      else if (selSettings == 4) { enterScreen(SCR_SETDT); }
      else if (selSettings == 5) { enterScreen(SCR_GOALCAL);   selGoalCal = 0; }
      need = true; break;
    
      // Goal calibration menu

case SCR_GOALCAL:
  if (selGoalCal == 0) {
    goalCalStart(GCAL_A);   // zaczynamy zawsze od A
  }
  else if (selGoalCal == 1) {
    enterScreen(SCR_GOALCAL_THRESH);
  }
  else if (selGoalCal == 2) {
    resetAdcDefaults();
    showOK("Defaults restored");
    delay(800);
  }
  need = true;
  break;

case SCR_GOALCAL_RUN:
  // podczas kalibracji SELECT nic nie robi
  break;

    // ───────── Puck lock (Enabled/Disabled + komendy) ─────────
    case SCR_PUCKLOCK:
      if (selPuckLock == 0) {                      // Enabled
        puckLockEnabled = true;
        showOK("Puck lock: ENABLED");
        saveSettings();                            // zapis + wysyłka settings po CAN (bit flagi)
        delay(600); need = true;
      } else if (selPuckLock == 1) {               // Disabled
        puckLockEnabled = false;
        showOK("Puck lock: DISABLED");
        saveSettings();
        delay(600); need = true;
      } else if (selPuckLock == 2) {               // Lock now
        uint8_t d[2] = { CAN_CMD_PUCKLOCK_NOW, CAN_PUCK_LOCK };
        canSendStd(CAN_ID_PUCKLOCK_CMD, d, 2);
        showOK("LOCK sent");
        delay(400); need = true;
      } else if (selPuckLock == 3) {               // Unlock now
        uint8_t d[2] = { CAN_CMD_PUCKLOCK_NOW, CAN_PUCK_UNLOCK };
        canSendStd(CAN_ID_PUCKLOCK_CMD, d, 2);
        showOK("UNLOCK sent");
        delay(400); need = true;
      }
      break;

    // ───────── Potwierdzenie startu gry ─────────
    case SCR_PLAY:
      snprintf(confirmText, sizeof(confirmText), "%s", fullGameItems[selPlay]);
      enterScreen(SCR_PLAYCONFIRM); selConfirm = 0; need = true; break;

    case SCR_MUSICROUND:
      snprintf(confirmText, sizeof(confirmText), "%s", musicRoundItems[selMusic]);
      enterScreen(SCR_PLAYCONFIRM);                // brak kursora – Select=YES, Back=NO
      need = true; break;

    case SCR_PLAYCONFIRM: {
      // YES – wyślij START do koordynatora po CAN (czas/gole idą osobnym „plikiem ustawień”)
      Screen src = previous; // ustawiony w enterScreen()
      uint8_t gtype, gindex;
      if      (src == SCR_PLAY)        { gtype = GT_FULL;  gindex = (uint8_t)selPlay; }          // 0..5
      else if (src == SCR_MUSICROUND)  { gtype = GT_MUSIC; gindex = (uint8_t)(selMusic + 1); }   // 1..6
      else                              { gtype = GT_QUICK; gindex = 0; }

      StartSendStatus st = canSendStartGame(gtype, gindex);
      if      (st == START_SENT_ACK)    { showOK("Sending..."); delay(800); showOK("Sent, ACK OK"); }
      else if (st == START_SENT_NO_ACK) { showERROR("No ACK");  delay(800); showERROR("Check CAN BUS"); }
      else if (st == START_COOLDOWN)    { showERROR("Wait..."); }
      else                              { showERROR("CAN error"); }

      delay(1000);
      enterScreen(SCR_MAIN);
      need = true;
      break;
    }

    // ───────── Ustawienia wartości ─────────
    case SCR_GAMETIME:
      gameTimeIdx = selTime;
      { char buf[16]; snprintf(buf, sizeof(buf), "time_%s",  timeItems[gameTimeIdx]); sendToHost(buf); }
      showOK(timeItems[gameTimeIdx]); saveSettings(); delay(800);
      need = true; break;

    case SCR_GOALS:
      goalsIdx = selGoals;
      { char buf[16]; snprintf(buf, sizeof(buf), "goals_%s", goalsItems[goalsIdx]); sendToHost(buf); }
      showOK(goalsItems[goalsIdx]); saveSettings(); delay(800);
      need = true; break;

    case SCR_LIGHTS:
      if      (selLights == 0) { enterScreen(SCR_STROBES);   selBin = strobesOn   ? 0 : 1; }
      else if (selLights == 1) { enterScreen(SCR_ANIMATION); selBin = animOn      ? 0 : 1; }
      else if (selLights == 2) { enterScreen(SCR_STANDBY);   selBin = standbyOn   ? 0 : 1; }
      else if (selLights == 3) { enterScreen(SCR_UVLIGHT);   selBin = uvOn        ? 0 : 1; }
      else if (selLights == 4) { enterScreen(SCR_GOALILLUM); selBin = goalIllumOn ? 0 : 1; }
      need = true; break;

    case SCR_STROBES:
      strobesOn = (selBin == 0);
      showOK(strobesOn ? "Strobes ON" : "Strobes OFF");
      sendToHost(strobesOn ? "strobes_on" : "strobes_off");
      saveSettings(); delay(800);
      need = true; break;

    case SCR_ANIMATION:
      animOn = (selBin == 0);
      showOK(animOn ? "Animation ON" : "Animation OFF");
      sendToHost(animOn ? "anim_on" : "anim_off");
      saveSettings(); delay(800);
      need = true; break;

    case SCR_STANDBY:
      standbyOn = (selBin == 0);
      showOK(standbyOn ? "Stand-by ON" : "Stand-by OFF");
      sendToHost(standbyOn ? "standby_on" : "standby_off");
      saveSettings(); delay(800);
      need = true; break;

    case SCR_UVLIGHT:
      uvOn = (selBin == 0);
      showOK(uvOn ? "UV ON" : "UV OFF");
      sendToHost(uvOn ? "uv_on" : "uv_off");
      saveSettings(); delay(800);
      need = true; break;

    case SCR_GOALILLUM:
      goalIllumOn = (selBin == 0);
      showOK(goalIllumOn ? "Goal illum. ON" : "Goal illum. OFF");
      sendToHost(goalIllumOn ? "goalillum_on" : "goalillum_off");
      saveSettings(); delay(800);
      need = true; break;

    case SCR_SETDT:
      setdtNextFieldOrSave();
      need = true; break;

    case SCR_OTHER:
    case SCR_COINS:
      need = true; break;

    case SCR_COINROOT:
      coinlogLoadBuffer();
      enterScreen(SCR_COINLOG);
      lastSelCoinLog = -1;
      lastCoinLogTop = -1;
      coinlogMarqueeReset();
      need = true; break;

    case SCR_COINLOG:
      // brak akcji na SELECT – tylko podgląd
      need = true; break;
  }
}


  // --- Cofanie (BACK) ---
if (pressedNow(bBack)) {
  registerInputActivity();

  if (current == SCR_SETDT) {
    setdtPrevFieldOrExit();
    need = true;
  }
  else if (current == SCR_GOALCAL_RUN)
  {
    Serial.println("[GCAL] CANCEL by user");
    goalCalSendCancelToMain();
    // tu później dodasz CAN cancel + blower OFF
    Serial.println("[GCAL] blower OFF (todo CAN)");

    uint8_t d[3] = {CAN_CMD_PUCKLOCK_NOW, CAN_PUCK_LOCK, 0x03};
    canSendStd(CAN_ID_PUCKLOCK_CMD, d, 3);
    Serial.println("[GCAL] puck locks -> LOCK ALL");

g_goalCal = GoalCalState{};
dropTopScreenIf(SCR_GOALCAL_RUN);
dropTopScreenIf(SCR_GOALCAL);
switchScreenNoBack(SCR_GOALCAL);
need = true;
  }
  else if (current != SCR_MAIN)
  {
    // obejmuje m.in. SCR_COINLOG i SCR_COINROOT
    goBack();
    need = true;
  }
}

// --- Delikatny odśwież zegara (co minutę) na ekranach z menu/ustawieniami ---
bool wantsFooter =
    !(current == SCR_PLAY ||
      current == SCR_MUSICROUND ||
      current == SCR_PLAYCONFIRM ||
      current == SCR_OTHER ||
      current == SCR_COINLOG);
if (wantsFooter)
{
  drawFooterClock(false);
}

// Marquee tick tylko na ekranie loga
// --- Marquee tick tylko na ekranie loga ---
auto coinlogMarqueeTick = [&]()
{
  if (current != SCR_COINLOG)
    return;

  CoinLogEntry e;
  if (!coinlogGetNewest(selCoinLog, e))
    return;
  DateTime dt(e.ts);

  char line[64];
  snprintf(line, sizeof(line), "%02d: %dzl %02d.%02d.%04d %02d:%02d:%02d",
           selCoinLog + 1, (int)e.denom,
           dt.day(), dt.month(), dt.year(),
           dt.hour(), dt.minute(), dt.second());

  const int leftX = coinlogLeftXBase(); // <<< NOWE
  int availW = tft.width() - leftX - 2; // <<< BYŁO: LEFT_MARGIN
  int textW = 6 * (int)strlen(line);
  if (textW <= availW)
    return;

  uint32_t now = millis();

  if (coinlogMarqueeX == 0 && (now - coinlogMarqueeLastMs) < (uint32_t)COINLOG_MARQ_PAUSE)
    return;

  if (now - coinlogMarqueeLastMs >= 120)
  {
    coinlogMarqueeLastMs = now;
    coinlogMarqueeX += 2;

    const int spacer = 24;
    const int wrapAt = textW + spacer;
    if (coinlogMarqueeX >= wrapAt)
    {
      coinlogMarqueeX = 0;
      coinlogMarqueeLastMs = now;
    }
    coinlogDrawRow(selCoinLog); // już rysuje w przesunięciu
  }
};
coinlogMarqueeTick();
goalCalTick();
goalCalCanPoll();

if (current == SCR_GOALCAL_RUN && g_goalCalUiDirty)
{
  redraw();
  g_goalCalUiDirty = false;
}

if (need)
  redraw();

/*// ===== RTC drift diagnostic =====
static uint32_t lastRtcPrint = 0;

if (millis() - lastRtcPrint > 120000) {   // co 2 minutę
  lastRtcPrint = millis();

  DateTime r = rtc.now();
  uint32_t sys = systemUnix();
  DateTime s(sys);

  Serial.print("RTC: ");
  Serial.print(r.year()); Serial.print("-");
  Serial.print(r.month()); Serial.print("-");
  Serial.print(r.day()); Serial.print(" ");
  Serial.print(r.hour()); Serial.print(":");
  Serial.print(r.minute()); Serial.print(":");
  Serial.print(r.second());

  Serial.print(" | SYS: ");
  Serial.print(s.year()); Serial.print("-");
  Serial.print(s.month()); Serial.print("-");
  Serial.print(s.day()); Serial.print(" ");
  Serial.print(s.hour()); Serial.print(":");
  Serial.print(s.minute()); Serial.print(":");
  Serial.print(s.second());
  Serial.println();
}*/
}
