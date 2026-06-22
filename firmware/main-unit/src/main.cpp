#include <Arduino.h>
#include "driver/twai.h"
#include <Preferences.h>
#include "led_matrix.h"
#include "anims.h"
#include "table_lights.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>




// ---------- LAMP / GOAL FX (split halves, no blinking) ----------
// static uint32_t g_goalFxUntilMs = 0;
// static uint8_t  g_goalFxSide = 0;   // 0=A, 1=B

// 0=OFF, 1=STEADY ON, 2=GOAL FX
// static uint8_t  g_lampMode = 255;

static bool g_strobeActive = false;
static uint32_t g_strobeIntervalMs = 0;
static uint32_t g_strobeLastToggleMs = 0;
static bool g_strobeLampOn = false;


static inline void lampOff()
{
  matrixFill(0, 0, 0);
  matrixShow();
}

static inline void lampFillScaled(uint8_t r, uint8_t g, uint8_t b, float scale)
{
  if (scale < 0.0f) scale = 0.0f;
  if (scale > 1.0f) scale = 1.0f;

  uint8_t rr = (uint8_t)(r * scale + 0.5f);
  uint8_t gg = (uint8_t)(g * scale + 0.5f);
  uint8_t bb = (uint8_t)(b * scale + 0.5f);

  matrixFill(rr, gg, bb);
  matrixShow();
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



/* static inline void goalFxStart(uint8_t side, uint32_t nowMs) {
  g_goalFxSide = (side ? 1 : 0);
  g_goalFxUntilMs = nowMs + 800;

  Serial.printf("[GOALFX] start side=%u\n", (unsigned)g_goalFxSide);
}

static inline bool goalFxActive(uint32_t nowMs) {
  return (nowMs < g_goalFxUntilMs);
}

static inline void goalFxTick(uint32_t nowMs) {
  matrixGoalSplit(g_goalFxSide);  // 0: 0..511 zielony, 512..1023 czerwony
                                 // 1: 0..511 czerwony, 512..1023 zielony
  matrixShow();
}
*/
// ---------------------------------------------------------

// === DEBUG ===
// Wymuś czas rundy (ms). Ustaw 0, aby wyłączyć.
#define DEBUG_FORCE_ROUND_TIME_MS 0

// NASLUCH ADC
static bool g_adcWatch = false;
static uint32_t g_adcWatchPeriodMs = 100;
static uint32_t g_adcWatchUntilMs = 0;
static uint32_t g_adcWatchLastMs = 0;

// --- SERIAL TEST: one-shot injected goal (0=A, 1=B)
static volatile int8_t g_forcedGoal = -1;

// BLOWER TIMEOUT AFTER GAMEOVER
static uint32_t g_gameOverT0 = 0;
static const uint32_t GAMEOVER_BLOWER_TIMEOUT_MS = 180000; // 3 min


// uv ambiente
static bool g_postGameActive = false;
static uint32_t g_postGameStartMs = 0;
static const uint32_t POST_GAME_DURATION_MS = 600000 ; // 10 min

//UV LEVELS
#define UV_LEVEL_IDLE_DUTY     0.00f
#define UV_LEVEL_AMBIENT_DUTY  0.1f
#define UV_LEVEL_BASE_DUTY     0.4f
#define UV_LEVEL_BOOST_DUTY    0.8f


// ==================== PUCKLOCK (TB6612/TB505A1) ====================

#define PIN_PWMA 39
#define PIN_PWMB 40
#define PIN_AIN1 7
#define PIN_AIN2 8
#define PIN_BIN1 9
#define PIN_BIN2 10 //test


// UV (LDD-1000H DIM on GPIO14)
#define PIN_UV_DIM 14
#define UV_PWM_CH   0
#define UV_PWM_HZ   1000
#define UV_PWM_RES  8

static bool g_uvCur = false;

static inline uint8_t pwmDutyUv(float duty)
{
  if (duty < 0.0f) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;
  return (uint8_t)(duty * 255.0f + 0.5f);
}

// FAN PWM on GPIO2
#define PIN_FAN_PWM 2
#define FAN_PWM_CH   1
#define FAN_PWM_HZ 15000
#define FAN_PWM_RES  8

static uint8_t g_fanPercent = 0;

static inline uint32_t fanDutyFromPercent(uint8_t pct)
{
  if (pct > 100) pct = 100;

  const uint32_t maxDuty = (1u << FAN_PWM_RES) - 1;
  return (uint32_t)(maxDuty * pct / 100u);
}
static void fanSetPercent(uint8_t pct)
{
  if (pct > 100) pct = 100;

  if (pct > 0 && pct < 30)
    pct = 30;

  g_fanPercent = pct;

  if (pct == 0)
  {
    ledcWrite(FAN_PWM_CH, 0);
    ledcDetachPin(PIN_FAN_PWM);
    pinMode(PIN_FAN_PWM, OUTPUT);
    digitalWrite(PIN_FAN_PWM, LOW);   // twarde odcięcie MOSFET-a
  }
  else
  {
    ledcAttachPin(PIN_FAN_PWM, FAN_PWM_CH);
    ledcWrite(FAN_PWM_CH, fanDutyFromPercent(pct));
  }


}

static uint8_t g_fanGamePercent = 60;     // domyślnie dla gry i ręcznej lampy
static uint8_t g_fanAmbientPercent = 30;  // po grze w ambient

static bool g_postGameAmbientFanActive = false;

static bool g_manualLampActive = false;
static uint32_t g_manualLampUntilMs = 0;



static bool isManualLampActive()
{
  if (!g_manualLampActive)
    return false;

  if (g_manualLampUntilMs != 0 && millis() >= g_manualLampUntilMs)
  {
    g_manualLampActive = false;
    g_manualLampUntilMs = 0;
    Serial.println("[FAN] manual lamp expired");
    return false;
  }

  return true;
}




// BUSY DF
#define DF_BUSY_PIN 38

unsigned long lastDfCheck = 0; //do wywalenia potem

// Jeśli STBY masz podpięte na stałe do 3V3 -> ustaw -1
#ifndef PIN_STBY
#define PIN_STBY -1
#endif

// Animacje LED lampa góra
static const char *TEENSIES_PATH = "/anims/teensies.bin";
static const char *TOAD_PATH     = "/anims/toad.bin";
static const char *FIESTA_PATH   = "/anims/fiesta.bin";
static const char *GLOOGLOO_PATH = "/anims/gloogloo.bin"; // "20.000 lums under the sea" 🌊
static const char *OLYMPUS_PATH  = "/anims/olympus.bin";
static const char *GRANNIES_PATH = "/anims/grannies.bin";

// Animacje ambiente pod stołem
static const char *TEENSIES_AMBIENT_PATH = "/anims/teensies_ambiente.bin";
static const char *TOAD_AMBIENT_PATH     = "/anims/toad_ambiente.bin";
static const char *FIESTA_AMBIENT_PATH   = "/anims/fiesta_ambiente.bin";
static const char *GLOOGLOO_AMBIENT_PATH = "/anims/gloogloo_ambiente.bin";
static const char *OLYMPUS_AMBIENT_PATH  = "/anims/olympus_ambiente.bin";
static const char *GRANNIES_AMBIENT_PATH = "/anims/grannies_ambiente.bin";

// Mapowanie: theme -> animacja BIN dla rundy MUSIC
// Uwaga: jeśli pliku nie ma na SD, binStart() wypisze błąd i nic nie zagra (bez crasha).
static const char *MUSIC_ANIM_PATHS[] = {
    TEENSIES_PATH, // TH_TEENSIES
    TOAD_PATH,     // TH_TOAD
    FIESTA_PATH,   // TH_FIESTA
    GLOOGLOO_PATH, // TH_20000
    OLYMPUS_PATH,  // TH_OLYMPUS
    GRANNIES_PATH, // TH_GRANNIES
};

static const char *MUSIC_AMBIENT_PATHS[] = {
    TEENSIES_AMBIENT_PATH, // TH_TEENSIES
    TOAD_AMBIENT_PATH,     // TH_TOAD
    FIESTA_AMBIENT_PATH,   // TH_FIESTA
    GLOOGLOO_AMBIENT_PATH, // TH_20000
    OLYMPUS_AMBIENT_PATH,  // TH_OLYMPUS
    GRANNIES_AMBIENT_PATH, // TH_GRANNIES
};

/*bool binStart(const char* path)
{
  binPath = path;

  if (binFile) binFile.close();
  binFile = SD.open(binPath, FILE_READ);

  if (!binFile) {
    Serial.printf("[BIN] open FAIL: %s\n", binPath);
    return false;
  }

  binPlaying = true;
  binLastFrame = millis();
  return true;
}*/

// CAN protokół (musi się zgadzać z main.cpp)
#ifndef CAN_ID_PUCKLOCK_CMD
#define CAN_ID_PUCKLOCK_CMD 0x330
#endif
#ifndef CAN_CMD_PUCKLOCK_NOW
#define CAN_CMD_PUCKLOCK_NOW 0x40
#endif
#ifndef CAN_PUCK_LOCK
#define CAN_PUCK_LOCK 0x01
#endif
#ifndef CAN_PUCK_UNLOCK
#define CAN_PUCK_UNLOCK 0x02
#endif



// parametry ruchu (możesz potem dostroić)
#ifndef MS_LOCK_MOVE
#define MS_LOCK_MOVE 70
#endif
#ifndef MS_UNLOCK_MOVE
#define MS_UNLOCK_MOVE 70
#endif
#ifndef LOCK_A_IS_FORWARD
#define LOCK_A_IS_FORWARD true
#endif
#ifndef LOCK_B_IS_FORWARD
#define LOCK_B_IS_FORWARD false
#endif

// SD piny
static constexpr int PIN_SD_SCK = 47;  // CLK
static constexpr int PIN_SD_MISO = 21; // DAT0
static constexpr int PIN_SD_MOSI = 48; // CMD
static constexpr int PIN_SD_CS = 5;    // CD/DAT3

// ESP32-S3: FSPI/HSPI zależy od core, ale FSPI zwykle jest OK do własnych pinów.
SPIClass SD_SPI(FSPI);

static bool sd_ok = false;

bool sd_init()
{
  // CS jako wyjście + stan wysoki (ważne przy init)
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  // Start SPI na podanych pinach
  SD_SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  // Spróbuj z umiarkowaną prędkością na start (jak ruszy, możesz podnieść)
  // SD.begin(csPin, spi, frequency)
  if (!SD.begin(PIN_SD_CS, SD_SPI, 10000000))
  {
    Serial.println("[SD] init FAIL");
    sd_ok = false;
    return false;
  }

  uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] OK, size: %llu MB\n", cardSize);

  sd_ok = true;
  return true;
}

bool sd_recover()
{
  SD.end(); // ważne
  delay(20);
  digitalWrite(PIN_SD_CS, HIGH);
  delay(2);

  // niższy zegar do ponownego podniesienia
  if (!SD.begin(PIN_SD_CS, SD_SPI, 4000000))
  {
    sd_ok = false;
    return false;
  }
  sd_ok = true;
  return true;
}

void sd_list_root()
{
  if (!sd_ok)
    return;

  File root = SD.open("/");
  if (!root)
  {
    Serial.println("[SD] open / FAIL");
    return;
  }

  Serial.println("[SD] / content:");
  while (true)
  {
    File f = root.openNextFile();
    if (!f)
      break;

    if (f.isDirectory())
    {
      Serial.printf("  <DIR> %s\n", f.name());
    }
    else
    {
      Serial.printf("  %s (%u bytes)\n", f.name(), (unsigned)f.size());
    }
    f.close();
  }
  root.close();
}

void sd_check_all_anims()
{
  if (!sd_ok)
  {
    Serial.println("[SD] not ready");
    return;
  }

  const uint8_t count = sizeof(MUSIC_ANIM_PATHS) / sizeof(MUSIC_ANIM_PATHS[0]);

  Serial.println("[SD] Checking animation files...");

  for (uint8_t i = 0; i < count; i++)
  {

    const char *path = MUSIC_ANIM_PATHS[i];

    File f = SD.open(path, FILE_READ);

    if (!f)
    {
      Serial.printf("[BIN] MISSING: %s\n", path);

      // próba recover
      Serial.println("  -> trying SD recover...");
      if (sd_recover())
      {
        f = SD.open(path, FILE_READ);
        if (f)
        {
          Serial.printf("[BIN] OK after recover: %s (%u bytes)\n",
                        path, (unsigned)f.size());
          f.close();
        }
        else
        {
          Serial.printf("[BIN] STILL MISSING: %s\n", path);
        }
      }
    }
    else
    {
      Serial.printf("[BIN] OK: %s (%u bytes)\n",
                    path, (unsigned)f.size());
      f.close();
    }
  }

  Serial.println("[SD] Animation check done.");
}

// PWMunlock ab

static constexpr int PUCK_PWM_FREQ = 20000;
static constexpr int PUCK_PWM_RES = 9; // 0..511
static constexpr int PUCK_PWM_MAX = (1 << PUCK_PWM_RES) - 1;
// UWAGA: nie używaj kanałów zajętych w main.cpp — daję 6 i 7, zwykle wolne:
static constexpr int PUCK_CH_A = 6;
static constexpr int PUCK_CH_B = 7;

static constexpr int PUCK_SPEED_PERCENT = 80;
static constexpr int PUCK_RAMP_UP_MS = 80;
static constexpr int PUCK_RAMP_DOWN_MS = 80;

enum class PuckDir
{
  Fwd,
  Rev,
  Coast
};
enum class PuckPhase
{
  Idle,
  RampUp,
  Hold,
  RampDown,
  Done
};

struct PuckMove
{
  bool active = false;
  PuckDir dir = PuckDir::Coast;
  PuckPhase phase = PuckPhase::Idle;
  uint32_t phase_t0 = 0;
  uint32_t hold_ms = 0;
  uint16_t runDuty = 0;
};

static PuckMove puckA, puckB;

static inline uint16_t puckPercentToDuty(int pct)
{
  if (pct < 5)
    pct = 5;
  if (pct > 100)
    pct = 100;
  return (uint16_t)((uint32_t)PUCK_PWM_MAX * (uint32_t)pct / 100U);
}

static inline void puckSetDir(bool axisA, PuckDir d)
{
  if (axisA)
  {
    if (d == PuckDir::Fwd)
    {
      digitalWrite(PIN_AIN1, HIGH);
      digitalWrite(PIN_AIN2, LOW);
    }
    if (d == PuckDir::Rev)
    {
      digitalWrite(PIN_AIN1, LOW);
      digitalWrite(PIN_AIN2, HIGH);
    }
    if (d == PuckDir::Coast)
    {
      digitalWrite(PIN_AIN1, LOW);
      digitalWrite(PIN_AIN2, LOW);
    }
  }
  else
  {
    if (d == PuckDir::Fwd)
    {
      digitalWrite(PIN_BIN1, HIGH);
      digitalWrite(PIN_BIN2, LOW);
    }
    if (d == PuckDir::Rev)
    {
      digitalWrite(PIN_BIN1, LOW);
      digitalWrite(PIN_BIN2, HIGH);
    }
    if (d == PuckDir::Coast)
    {
      digitalWrite(PIN_BIN1, LOW);
      digitalWrite(PIN_BIN2, LOW);
    }
  }
}

static inline void puckSetDuty(bool axisA, uint16_t duty)
{
  if (duty > PUCK_PWM_MAX)
    duty = PUCK_PWM_MAX;
  ledcWrite(axisA ? PUCK_CH_A : PUCK_CH_B, duty);
}

static inline void puckInit()
{
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_BIN2, OUTPUT);

  if (PIN_STBY >= 0)
  {
    pinMode(PIN_STBY, OUTPUT);
    digitalWrite(PIN_STBY, HIGH);
  }

  // bezpieczny start: COAST
  puckSetDir(true, PuckDir::Coast);
  puckSetDir(false, PuckDir::Coast);

  ledcSetup(PUCK_CH_A, PUCK_PWM_FREQ, PUCK_PWM_RES);
  ledcSetup(PUCK_CH_B, PUCK_PWM_FREQ, PUCK_PWM_RES);
  ledcAttachPin(PIN_PWMA, PUCK_CH_A);
  ledcAttachPin(PIN_PWMB, PUCK_CH_B);
  puckSetDuty(true, 0);
  puckSetDuty(false, 0);
}

static inline void puckStartAxis(PuckMove &m, bool axisA, bool toLock)
{
  if (m.active)
    return;

  const bool forward = axisA
                           ? (LOCK_A_IS_FORWARD ? toLock : !toLock)
                           : (LOCK_B_IS_FORWARD ? toLock : !toLock);

  m.dir = forward ? PuckDir::Fwd : PuckDir::Rev;
  m.runDuty = puckPercentToDuty(PUCK_SPEED_PERCENT);
  m.hold_ms = toLock ? MS_LOCK_MOVE : MS_UNLOCK_MOVE;

  puckSetDir(axisA, m.dir);
  puckSetDuty(axisA, 0);

  const uint32_t now = millis();
  m.active = true;
  m.phase = (PUCK_RAMP_UP_MS > 0) ? PuckPhase::RampUp : PuckPhase::Hold;
  m.phase_t0 = now;
}

static inline void puckTickAxis(PuckMove &m, bool axisA)
{
  if (!m.active)
    return;
  const uint32_t now = millis();

  switch (m.phase)
  {
  case PuckPhase::RampUp:
  {
    uint32_t dt = now - m.phase_t0;
    if (dt >= (uint32_t)PUCK_RAMP_UP_MS)
    {
      puckSetDuty(axisA, m.runDuty);
      m.phase = PuckPhase::Hold;
      m.phase_t0 = now;
    }
    else
    {
      float x = (float)dt / (float)PUCK_RAMP_UP_MS;
      float y = x * x * (3.f - 2.f * x);
      puckSetDuty(axisA, (uint16_t)(m.runDuty * y));
    }
  }
  break;

  case PuckPhase::Hold:
    puckSetDuty(axisA, m.runDuty);
    if (now - m.phase_t0 >= m.hold_ms)
    {
      m.phase = (PUCK_RAMP_DOWN_MS > 0) ? PuckPhase::RampDown : PuckPhase::Done;
      m.phase_t0 = now;
    }
    break;

  case PuckPhase::RampDown:
  {
    uint32_t dt = now - m.phase_t0;
    if (dt >= (uint32_t)PUCK_RAMP_DOWN_MS)
    {
      puckSetDuty(axisA, 0);
      m.phase = PuckPhase::Done;
      m.phase_t0 = now;
    }
    else
    {
      float x = 1.f - (float)dt / (float)PUCK_RAMP_DOWN_MS;
      float y = x * x * (3.f - 2.f * x);
      puckSetDuty(axisA, (uint16_t)(m.runDuty * y));
    }
  }
  break;

  case PuckPhase::Done:
    // WAŻNE: kończymy w COAST (luz), żeby nie “blokowało”
    puckSetDir(axisA, PuckDir::Coast);
    puckSetDuty(axisA, 0);
    m.active = false;
    m.phase = PuckPhase::Idle;
    break;

  default:
    break;
  }
}

// ========================sfx5= Puck-lock helper =========================
static uint8_t s_puckLockMaskActive = 0; // bit0=A, bit1=B (what is currently locked)
static inline void puckLockCmd(bool lock, uint8_t mask)
{
  // lokalnie: bit0 = A, bit1 = B
  if (mask & 0x01)
    puckStartAxis(puckA, true, lock);
  if (mask & 0x02)
    puckStartAxis(puckB, false, !lock);
}

// wołasz z main loop po odebraniu CAN
static inline void puckOnCan(const twai_message_t &m)
{
  if (m.identifier != CAN_ID_PUCKLOCK_CMD)
    return;
  if (m.data_length_code < 2)
    return;
  if (m.data[0] != CAN_CMD_PUCKLOCK_NOW)
    return;

  const bool toLock = (m.data[1] == CAN_PUCK_LOCK);
  uint8_t mask = (m.data_length_code >= 3) ? m.data[2] : 0x03;

  puckLockCmd(toLock, mask);
}

// wołasz co loop
static inline void puckTick()
{
  puckTickAxis(puckA, true);
  puckTickAxis(puckB, false);
}
// ================== end PUCKLOCK ==================

// ========================= CAN protocol (existing + new) =========================
#define CAN_ID_SETTINGS 0x300
#define CAN_CMD_SETTINGS 0x02
#define CAN_ID_SETTINGS_ACK 0x312
#define CAN_ID_PUCKLOCK_CMD 0x330
#define CAN_CMD_PUCKLOCK_NOW 0x40
#define CAN_PUCK_LOCK 0x01
#define CAN_PUCK_UNLOCK 0x02
#define CAN_ID_GOALCAL_CMD   0x350   // settings -> main
#define CAN_ID_GOALCAL_DATA  0x351   // main -> settings

#define CAN_CMD_GOALCAL_START_A 0x60
#define CAN_CMD_GOALCAL_START_B 0x61
#define CAN_CMD_GOALCAL_CANCEL  0x62

#define CAN_CMD_GOALCAL_IDLE    0x63 // main -> settings, payload: [cmd, hi, lo]
#define CAN_CMD_GOALCAL_SAMPLE  0x64 // main -> settings, payload: [cmd, hi, lo]

// New: scoreboard/events towards Display ESP
#define CAN_ID_SCORE_UPDATE 0x321 // data: [scoreA, scoreB]
#define CAN_ID_GOAL_ANIM 0x322    // data: [side(0=A,1=B), type]
#define CAN_ID_GAME_EVENT 0x323   // data: [evt, a, b]

#define CAN_ID_ROUND_TIME 0x324   // data: [sec_hi, sec_lo]
// evt: 0=GameStart, 1=RoundStart, 2=RoundEnd, 3=GameOver, 4=CountdownTick(3..1)

// New: Start-Game we already listen (0x301) → [0x01, gtype, gindex]
#define CAN_ID_START_GAME 0x301
#define CAN_CMD_START_GAME 0x01
#define CAN_ID_MINIGAME_AUDIO 0x340
#define CAN_CMD_MINIGAME_AUDIO 0x50
#define CAN_AUDIO_STOP 0x00
#define CAN_AUDIO_PLAY_TETRIS 0x01


//Lampa
#define CAN_ID_LAMP_CMD     0x341
#define CAN_CMD_LAMP_SCENE  0x70




static uint32_t g_roundGuardUntilMs = 0; // przez ten czas nie kończymy rundy

// ========================= Flags bits =========================
enum : uint8_t
{
  F_STROBES = 0,
  F_ANIM = 1,
  F_STANDBY = 2,
  F_UV = 3,
  F_GOALILLUM = 4,
  F_PUCKLOCK = 5 // enable/disable puck-lock mechanics
};
static inline bool flagOn(uint8_t f, uint8_t bit) { return (f >> bit) & 1; }




// kalibracja flagi
enum GoalCalMainMode : uint8_t {
  GCALM_NONE = 0,
  GCALM_A    = 1,
  GCALM_B    = 2
};

struct GoalCalMainState {
  bool active = false;
  GoalCalMainMode mode = GCALM_NONE;
  uint32_t startMs = 0;
  bool idleSent = false;
};

static GoalCalMainState g_goalCalMain;
static bool g_goalCalPulseActive = false;


// ========================= Settings structure (NVS) =========================
struct AHSettings
{
  uint8_t gameTimeIdx; // 0..5
  uint8_t goalsIdx;    // 0..4
  uint8_t flags;       // bits as above
  uint8_t _pad;
};

Preferences prefs;
AHSettings g_set = {1, 1, 0x3F, 0}; // default: 5 min, 5 goals, all features ON (incl. puck-lock)

// ========================= Hardware mapping (PLACEHOLDERS) =========================
// ⚠️ Ustal piny pod swój ESP32‑S3. Zostawiłem sensowne domyślne numery – łatwo zmienić.

// CAN on safe pins for S3 (avoid 19/20 USB D±)
#define CAN_TX_PIN GPIO_NUM_41
#define CAN_RX_PIN GPIO_NUM_42

// Blower control (230V/80W via relay).
#define PIN_BLOWER 4
#define BLOWER_ACTIVE HIGH

// Goal sensor on a single ADC pin (A/B rozróżniamy poziomem napięcia przez dzielniki)
#define PIN_GOAL_ADC 6 // TODO: podaj swój pin ADC
#define GOAL_DEBOUNCE_MS 250
#define GOAL_COOLDOWN_MS 500

// ADC THRESH
static uint16_t adcThreshAmin = 2200;
static uint16_t adcThreshAmax = 3000;

static uint16_t adcThreshBmin = 800;
static uint16_t adcThreshBmax = 1600;

static uint16_t adcIdle = 4095;

// MP3 players (YX5200/DFPlayer-compatible) – two UARTs
// Music player (background tracks)
#define MP3_MUSIC_UART 1
#define MP3_MUSIC_TX 18 // ESP32 TX → MP3 RX
#define MP3_MUSIC_RX 17 // ESP32 RX ← MP3 TX
// SFX player (goal sounds, countdown)
#define MP3_SFX_UART 2
#define MP3_SFX_TX 16
#define MP3_SFX_RX 15

// Light control – placeholders (replace with GPIO/CAN to your light module)
static uint8_t g_lightModeCur = 255;
static inline void lightsSetMode(uint8_t mode)
{
  // 0: no strobes, no anim | 1: no strobes, anim | 2: strobes + anim
  if (g_lightModeCur == mode)
    return; // ← nie spamuj, jeśli bez zmian
  g_lightModeCur = mode;
  Serial.printf("[LIGHTS] mode=%u\n", mode);
  // TODO: tu wyślij faktyczne komendy do sterownika świateł
}



static inline void goalsIllumSet(bool on) { Serial.printf("[GOALS ILLUM] %s\n", on ? "ON" : "OFF"); }

// ========================= Games catalog (names) =========================
const char *FULL_GAMES[] = {
    "Teensies in Trouble",
    "Toad Story",
    "Fiesta de los Muertos",
    "20,000 Lums Under the Sea",
    "Olympus Maximus",
    "Living Dead Party"};
const size_t FULL_N = sizeof(FULL_GAMES) / sizeof(FULL_GAMES[0]);

const char *MUSIC_ROUNDS[] = {
    "Castle Rock",
    "Orchestral Chaos",
    "Mariachi Madness",
    "Gloo Gloo",
    "Dragon Slayer",
    "Grannies"};
const size_t MUSIC_N = sizeof(MUSIC_ROUNDS) / sizeof(MUSIC_ROUNDS[0]);

static uint32_t g_gameOverGuardUntilMs = 0;

enum : uint8_t
{
  GT_FULL = 1,
  GT_MUSIC = 2,
  GT_QUICK = 3
};

// ========================= Helpers (CAN logging) =========================
static void logFrame(const twai_message_t &m)
{
  Serial.printf("[CAN] RX %s%s ID=0x%03X DLC=%d DATA:",
                m.extd ? "EXT " : "STD ",
                m.rtr ? "RTR " : "",
                m.identifier, m.data_length_code);
  for (int i = 0; i < m.data_length_code; ++i)
    Serial.printf(" %02X", m.data[i]);
  Serial.println();
}

static int readGoalRawMedian()
{
  int v[5];
  for (int i = 0; i < 5; i++)
    v[i] = analogRead(PIN_GOAL_ADC);
  // insertion sort
  for (int i = 1; i < 5; i++)
  {
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k)
    {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = k;
  }
  return v[2];
}






// ========================= Settings (NVS) =========================
static void settingsSaveToNVS()
{
  prefs.begin("ah", false);
  prefs.putBytes("settings", &g_set, sizeof(g_set));
  prefs.end();
}
static void settingsLoadFromNVS()
{
  prefs.begin("ah", true);
  size_t n = prefs.getBytes("settings", &g_set, sizeof(g_set));
  prefs.end();
  if (n != sizeof(g_set))
    settingsSaveToNVS();
}

static inline bool isUvEnabled()
{
  return flagOn(g_set.flags, F_UV);
}


static void sendSettingsAck()
{
  twai_message_t m = {};
  m.identifier = CAN_ID_SETTINGS_ACK;
  m.data_length_code = 1;
  m.data[0] = 0xA5;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}
static void printSettings(const AHSettings &s)
{
  Serial.printf(
      "Settings: timeIdx=%u goalsIdx=%u | strobes=%d anim=%d standby=%d uv=%d goalIllum=%d puckLock=%d", s.gameTimeIdx, s.goalsIdx,
      flagOn(s.flags, F_STROBES), flagOn(s.flags, F_ANIM), flagOn(s.flags, F_STANDBY),
      flagOn(s.flags, F_UV), flagOn(s.flags, F_GOALILLUM), flagOn(s.flags, F_PUCKLOCK));
}
// Short window to ingest late Settings frames just after START
static void settingsSyncWindow(uint32_t ms = 200)
{
  uint32_t t0 = millis();
  twai_message_t m;
  while ((int32_t)(millis() - t0) < (int32_t)ms)
  {
    if (twai_receive(&m, 0) == ESP_OK)
    {
      puckOnCan(m);
      if (!m.extd && !m.rtr && m.identifier == CAN_ID_SETTINGS && m.data_length_code >= 4 && m.data[0] == CAN_CMD_SETTINGS)
      {
        g_set.gameTimeIdx = m.data[1];
        g_set.goalsIdx = m.data[2];
        g_set.flags = m.data[3];
        settingsSaveToNVS();
        sendSettingsAck();
        
        Serial.print("[CAN] SettingsUpdate during START → ");
        printSettings(g_set);
      }
      else
      {
        // other frames can be ignored here (they'll be handled next loop)
      }
    }
    puckTick();
    delay(1);
  }
}

// ========================= Time/goals options =========================
// timeIdx (0..5) → seconds | goalsIdx (0..4) → goals
static const uint16_t TIME_OPTIONS_S[6] = {180, 300, 420, 600, 720, 900}; // 3,5,7,10,12,15 min
static const uint8_t GOAL_OPTIONS[5] = {1, 3, 5, 7, 9};                   // panel: 1,3,5,7,9 (IDX 0..4)

// ========================= MP3 minimal driver (YX5200/DFPlayer-like) =========================
// Protocol frame: 0x7E 0xFF 0x06 CMD 0x00 PAR1 PAR2 CHK1 CHK2 0xEF
// We use: 0x03 play index, 0x0F play folder/file, 0x16 stop, 0x06 set volume

static HardwareSerial MusicSerial(MP3_MUSIC_UART);
static HardwareSerial SfxSerial(MP3_SFX_UART);

static void dfpSend(HardwareSerial &ser, uint8_t cmd, uint16_t param)
{
  uint8_t buf[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, (uint8_t)(param >> 8), (uint8_t)param, 0, 0, 0xEF};
  uint16_t sum = 0;
  for (int i = 1; i < 7; i++)
    sum += buf[i];
  sum = 0xFFFF - sum + 1; // two's complement
  buf[7] = (uint8_t)(sum >> 8);
  buf[8] = (uint8_t)sum;
  ser.write(buf, sizeof(buf));
}
static void mp3SetVol(HardwareSerial &ser, uint8_t vol /*0..30*/) { dfpSend(ser, 0x06, vol); }
static void mp3Stop(HardwareSerial &ser) { dfpSend(ser, 0x16, 0); }
static void mp3PlayIdx(HardwareSerial &ser, uint16_t idx) { dfpSend(ser, 0x03, idx); }
static void mp3PlayFolderFile(HardwareSerial &ser, uint8_t folder /*01..99*/, uint8_t file /*001..255*/)
{
  dfpSend(ser, 0x0F, ((uint16_t)folder << 8) | file);
}

// ========================= MUSIC (themes) =========================

// Folder mapping (DFPlayer: 01..99). Music-round in each folder is file 010.mp3.
// 01=teensies
// 02=toad
// 03=fiesta
// 04=20000lums
// 05=olympus
// 06=grannies
// File 010.mp3 = music round
// Files 001..N = normal background tracks
enum ThemeId : uint8_t
{
  TH_TEENSIES = 0,
  TH_TOAD = 1,
  TH_FIESTA = 2,
  TH_20000 = 3,
  TH_OLYMPUS = 4,
  TH_GRANNIES = 5,
  TH_COUNT = 6
};

struct ThemeInfo
{
  const char *name;
  uint8_t folder;     // 01..06
  uint8_t trackCount; // normal tracks: 001..N (file 010 reserved for music round)
};

static const ThemeInfo THEMES[TH_COUNT] = {
    {"teensies", 1, 8},
    {"toad", 2, 5},
    {"fiesta", 3, 4},
    {"20000lums", 4, 7},
    {"olympus", 5, 6},
    {"grannies", 6, 6},
};

// Music-round duration per theme (theme 1..6 -> g_theme 0..5)
static const uint32_t MUSIC_ROUND_TIME_MS[TH_COUNT] = {
    114000UL, // teensies     1:54 //114000
    86000UL,  // toad       1:26
    85000UL,  // fiesta     1:25
    100000UL, // 20000lums  1:40
    101000UL, // olympus    1:41
    96000UL   // grannies   1:36
};



// --- SD BIN anim forward declarations ---
void binStop();
bool binStart(const char *path, uint32_t syncStartMs);
void binUpdate();

static ThemeId g_theme = TH_TEENSIES;

static void getThemeColor(uint8_t &r, uint8_t &g, uint8_t &b)
{
  switch (g_theme)
  {
    case TH_TEENSIES:
      r = 0;   g = 0;   b = 255; break; // niebieski

    case TH_TOAD:
      r = 0;   g = 255; b = 0;   break; // zielony

    case TH_FIESTA:
      r = 255; g = 90;  b = 0;   break; // pomarańczowy

    case TH_20000:
      r = 0;   g = 50;  b = 70;  break; // morski

    case TH_OLYMPUS:
      r = 150; g = 0;   b = 255; break; // fiolet

    case TH_GRANNIES:
      r = 255; g = 0;   b = 140; break; // różowy

    default:
      r = 0;   g = 0;   b = 0;   break;
  }
}

static void applyGoalLightsBySettings()
{
  uint8_t r = 0, g = 0, b = 0;
  getThemeColor(r, g, b);

  tableLightsSetGoalsColor(r, g, b);
  tableLightsSetGoalsEnabled(flagOn(g_set.flags, F_GOALILLUM));
}




static void applyRoundTheme(bool breathing)
{
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  getThemeColor(r, g, b);

  animsSetRoundColor(r, g, b);
  animsSetBreathing(breathing);

  tableLightsSetRoundColor(r, g, b);
  tableLightsSetBreathing(breathing);
  tableLightsSetRoundEnabled(true);

  applyGoalLightsBySettings();
}


static void applyWhiteRoundLights()
{
  binStop();
  tableLightsMusicStop();

  animsSetRoundColor(255, 255, 255);
  animsSetBreathing(false);
  animsSetMode(ANIM_ROUND);

  tableLightsSetRoundColor(255, 255, 255);
  tableLightsSetBreathing(false);
  tableLightsSetRoundEnabled(true);
}

static uint32_t g_musicStartMs = 0;

static void applyRoundLightsBySettings(bool musicRound)
{
  const bool animOn   = flagOn(g_set.flags, F_ANIM);
  const bool stroboOn = flagOn(g_set.flags, F_STROBES);

  // Bramki świecą kolorem świata niezależnie od trybu animacji,
  // ale tylko jeśli Goal illumination jest ON.
  applyGoalLightsBySettings();

  // ANIM OFF = biała lampa, niezależnie od strobo
  if (!animOn)
  {
    applyWhiteRoundLights();
    return;
  }

    // ANIM ON + STROBO ON + RUNDA MUZYCZNA = BIN-y
  if (musicRound && stroboOn)
  {
    const uint8_t ti = (uint8_t)g_theme;

    const char *path = (ti < (uint8_t)(sizeof(MUSIC_ANIM_PATHS) / sizeof(MUSIC_ANIM_PATHS[0])))
                           ? MUSIC_ANIM_PATHS[ti]
                           : nullptr;

    const char *ambientPath = (ti < (uint8_t)(sizeof(MUSIC_AMBIENT_PATHS) / sizeof(MUSIC_AMBIENT_PATHS[0])))
                                  ? MUSIC_AMBIENT_PATHS[ti]
                                  : nullptr;

    // Podczas rundy muzycznej osią czasu jest start rundy / start piosenki.
    // Dzięki temu jak włączysz BIN w połowie utworu, wskoczy od razu w połowę animacji.

    uint32_t syncStartMs = g_musicStartMs ? g_musicStartMs : millis();

    binStop();
    tableLightsMusicStop();

    // binUpdate() renderuje tylko w ANIM_ROUND
    animsSetBreathing(false);
    animsSetMode(ANIM_ROUND);

    // Ambiente ma grać BIN-em, nie stałym kolorem
    tableLightsSetRoundEnabled(false);

    if (path && binStart(path, syncStartMs))
    {
      // Ambiente dostaje dokładnie ten sam start czasu co główna matryca.
      tableLightsMusicStart(ambientPath, syncStartMs);
    }
    else
    {
      binStop();
      tableLightsMusicStop();

      // fallback gdyby SD/plik padł
      applyRoundTheme(true);
      animsSetMode(ANIM_ROUND);
    }

    return;
  }

  // ANIM ON + STROBO OFF = kolory + oddychanie we wszystkich rundach
  // ANIM ON + STROBO ON + pełna/normalna runda = kolory + oddychanie
  binStop();
  tableLightsMusicStop();

  applyRoundTheme(true);
  animsSetMode(ANIM_ROUND);
}



static uint16_t g_usedMask = 0;      // "no repeat" mask for 001..N (N<=8 here)
static bool g_bgPlaying = false;     // background started for current game?
static bool g_musicOnlyMode = false; // GT_MUSIC: only music round, no normal rounds


static void sayStart(uint8_t gtype, uint8_t idx)
{
  switch (gtype)
  {
  case GT_FULL:
    if (idx < FULL_N)
      Serial.printf("\nStartuję %s\n", FULL_GAMES[idx]);
    else
      Serial.printf("\nStartuję FULL index=%u\n", idx);
    break;
  case GT_MUSIC:
  if (idx < TH_COUNT)
    Serial.printf("\nStartuję MUSIC: %s\n", THEMES[idx].name);
  else
    Serial.printf("\nStartuję MUSIC index=%u\n", idx);
  
    break;
  case GT_QUICK:
    Serial.printf("\nStartuję QUICK index=%u\n", idx);
    break;
  default:
    Serial.printf("\nStartuję TYPE=%u index=%u\n", gtype, idx);
    break;
  }
}



static void poolResetTheme() { g_usedMask = 0; }

static uint8_t poolPickRandomTrack()
{
  const uint8_t n = THEMES[g_theme].trackCount; // 1..n
  if (!n)
    return 1;
  const uint16_t allMask = (1u << n) - 1u;
  if ((g_usedMask & allMask) == allMask)
    g_usedMask = 0;

  for (uint8_t tries = 0; tries < 50; ++tries)
  {
    uint8_t pick = 1 + (uint8_t)random(0, n); // 1..n
    uint16_t bit = 1u << (pick - 1);
    if (!(g_usedMask & bit))
    {
      g_usedMask |= bit;
      return pick;
    }
  }
  return 1 + (uint8_t)random(0, n);
}


static void musicPlayFolderFile(uint8_t folder, uint8_t file);
static void musicPlayTrack(uint8_t fileInThemeFolder)
{
  Serial.printf(
      "[MUSIC] theme=%s folder=%02u file=%03u.mp3\n",
      THEMES[g_theme].name,
      THEMES[g_theme].folder,
      fileInThemeFolder);

  musicPlayFolderFile(THEMES[g_theme].folder, fileInThemeFolder);

  g_bgPlaying = true;
}

static void musicPlayMusicRound()
{
  // music round always 010.mp3 in current theme folder
  Serial.printf("[MUSIC] theme=%s MUSIC-ROUND file=010.mp3\n", THEMES[g_theme].name);
  mp3PlayFolderFile(MusicSerial, THEMES[g_theme].folder, 10);
}

// Background: start only if not started yet (no BUSY → we don't auto-advance here).
static void musicEnsureBackground()
{
  if (!g_bgPlaying)
  {
    uint8_t file = poolPickRandomTrack();
    Serial.printf("[MUSIC] pick=%u usedMask=0x%04X n=%u\n", file, g_usedMask, THEMES[g_theme].trackCount);
    musicPlayTrack(file);
  }
}

static void musicStop() { mp3Stop(MusicSerial); }
static void sfxPlayIndex(uint16_t idx)
{
  Serial.printf("[SFX] play %u.mp3\n", idx);
  mp3PlayIdx(SfxSerial, idx);
}
static void musicPlayFolderFile(uint8_t folder, uint8_t file)
{
  Serial.printf("[MUSIC] play folder=%02u file=%03u.mp3\n", folder, file);

  // DFPlayer: STOP -> krótka pauza -> PLAY (często inaczej ignoruje)
  mp3Stop(MusicSerial);
  delay(150);

  mp3PlayFolderFile(MusicSerial, folder, file);
  delay(40);
  mp3PlayFolderFile(MusicSerial, folder, file); // retry (DFPlayer bywa kapryśny)
}

static void sfxPlayFolderFile(uint8_t folder, uint8_t file)
{
  Serial.printf("[SFX] play folder=%u file=%u\n", folder, file);
  mp3PlayFolderFile(SfxSerial, folder, file);
}

static inline bool musicIsPlaying()
{
  return digitalRead(DF_BUSY_PIN) == LOW; // BUSY LOW = gra
}



static void musicPlayTetris()
{
  Serial.println("[MUSIC] TETRIS start");
  g_bgPlaying = false;       // żeby nie udawało, że tło dalej gra
  musicPlayFolderFile(7, 1); // folder 07, plik 001.mp3
}

// ==========PUCK LOCK DELAY AFTER SECOND TO LAST GOAL

// Zwłoka przed zablokowaniem po „przedostatnim” golu
static const uint32_t PRELAST_LOCK_DELAY_MS = 700;

// Latch / „uzbrojenie” blokady po wykryciu stanu przedostatniego
static bool s_armA = false, s_armB = false;
static uint32_t s_armT0A = 0, s_armT0B = 0;

static inline void puckLockDelayReset()
{
  s_armA = s_armB = false;
  s_armT0A = s_armT0B = 0;
}

static inline void puckLockUnlockAll()
{
  puckLockCmd(false, 0x03);
}

// ========================= Scoreboard (CAN) =========================
static void sendScoreUpdate(uint8_t a, uint8_t b)
{
  twai_message_t m = {};
  m.identifier = CAN_ID_SCORE_UPDATE;
  m.data_length_code = 2;
  m.data[0] = a;
  m.data[1] = b;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}
static void sendGoalAnim(uint8_t side /*0=A,1=B*/, uint8_t type)
{
  twai_message_t m = {};
  m.identifier = CAN_ID_GOAL_ANIM;
  m.data_length_code = 2;
  m.data[0] = side;
  m.data[1] = type;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}
static void sendGameEvent(uint8_t evt, uint8_t a = 0, uint8_t b = 0)
{
  twai_message_t m = {};
  m.identifier = CAN_ID_GAME_EVENT;
  m.data_length_code = 3;
  m.data[0] = evt;
  m.data[1] = a;
  m.data[2] = b;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

static void sendRoundTimeSeconds(uint16_t secs)
{
  twai_message_t m = {};
  m.identifier = CAN_ID_ROUND_TIME;
  m.data_length_code = 2;
  m.data[0] = (uint8_t)(secs >> 8);
  m.data[1] = (uint8_t)(secs & 0xFF);
  twai_transmit(&m, pdMS_TO_TICKS(20));
}



// ========================= Game state machine =========================

enum class GState : uint8_t
{
  IDLE,
  PREPARE,
  COUNTDOWN,
  ROUND_NORMAL,
  ROUND_BREAK,
  ROUND_MUSIC,
  GAME_OVER,
  WAIT_PUCK_CLEAR
};

struct RoundCtx
{
  uint8_t scoreA = 0, scoreB = 0;
  uint32_t tStart = 0;   // millis of round start
  uint32_t tLimitMs = 0; // 0 = no time limit
  uint8_t goalLimit = 0; // 0 = no goal limit
  bool timeLimited = false;
};

static GState g_state = GState::IDLE;
static uint8_t g_fullIndex = 0; // which full game (0..5)
static uint8_t g_roundNo = 0;   // 1..3 (R1,R2,Music)
static RoundCtx g_round;
static uint8_t g_roundsWonA = 0, g_roundsWonB = 0;
static bool g_musicRound = false;

// --- Remaining time logger (co 10 s) ---
static uint32_t remainLogLastMs = 0;
static const uint32_t REMAIN_LOG_PERIOD_MS = 10000;

static inline void remainLogReset() { remainLogLastMs = millis(); }

static inline void remainLogTick()
{
  // loguj tylko w trakcie aktywnej rundy z limitem czasu
  if (!(g_state == GState::ROUND_NORMAL || g_state == GState::ROUND_MUSIC))
    return;
  if (!g_round.timeLimited)
    return;

  uint32_t now = millis();
  if ((int32_t)(now - remainLogLastMs) >= (int32_t)REMAIN_LOG_PERIOD_MS)
  {
    // trzymaj równy krok 10 s nawet przy okazjonalnych lagach
    remainLogLastMs += REMAIN_LOG_PERIOD_MS;

    uint32_t elapsed = now - g_round.tStart;
    uint32_t leftMs = (g_round.tLimitMs > elapsed) ? (g_round.tLimitMs - elapsed) : 0;
    unsigned long leftSec = (leftMs + 500) / 1000; // zaokrąglij ładnie
    Serial.printf("[TIME] remaining: %lu s\n", leftSec);
  }
}

static inline bool isGameActive()
{
  return
      (g_state == GState::PREPARE) ||
      (g_state == GState::COUNTDOWN) ||
      (g_state == GState::ROUND_NORMAL) ||
      (g_state == GState::ROUND_MUSIC);
}

static void fanAutoUpdate()
{
  uint8_t target = 0;
  bool uv = isUvEnabled();

  if (isGameActive() || isManualLampActive())
  {
    target = uv ? 70 : 40;
  }
  else if (g_postGameAmbientFanActive)
  {
    target = uv ? 30 : 0;
  }
  else
  {
    target = 0;
  }

  if (target != g_fanPercent || target == 0)
    fanSetPercent(target);

    
}

static void musicAutoAdvanceTick()
{
  static bool wasPlaying = false;

  const bool nowPlaying = musicIsPlaying();

  // wykrycie zakończenia utworu: było granie i nagle przestało
  if (wasPlaying && !nowPlaying)
  {

    // jeśli nadal jesteśmy w trakcie gry (nie IDLE / nie GAME_OVER)
    const bool gameActive =
        (g_state != GState::IDLE) &&
        (g_state != GState::GAME_OVER) &&
        (g_state != GState::WAIT_PUCK_CLEAR);

    if (gameActive)
    {
      // Jeżeli trwa runda muzyczna -> odpal ponownie 010.mp3 (żeby nie było ciszy)
      if (g_state == GState::ROUND_MUSIC)
      {
        musicPlayMusicRound(); // u Ciebie to gra 010.mp3 w folderze theme
      }
      else
      {
        // normalne tło: następny losowy 001..N z tego samego folderu
        g_bgPlaying = false;     // ważne: pozwala musicEnsureBackground() ruszyć
        musicEnsureBackground(); // wybiera kolejny z poolPickRandomTrack()
      }
    }
  }

  wasPlaying = nowPlaying;
}

static uint16_t mapTimeIdxToSeconds(uint8_t idx)
{
  if (idx > 5)
    idx = 5;
  return TIME_OPTIONS_S[idx];
}
static uint8_t mapGoalsIdxToCount(uint8_t idx)
{
  if (idx > 4)
    idx = 4;
  return GOAL_OPTIONS[idx];
}

// ========================= Blower =========================
static bool blowerOn = false;

static void blowerSet(bool on)
{
  bool changed = (blowerOn != on);
  blowerOn = on;
  digitalWrite(PIN_BLOWER, on ? BLOWER_ACTIVE : !BLOWER_ACTIVE); // ZAWSZE jedź na pin
  if (changed)
    Serial.printf("[BLOWER] %s\n", on ? "ON" : "OFF"); // log tylko gdy zmiana
}
// ========================= Round lifecycle =========================
static void lightsApplyFromFlags()
{
  const bool gameActive =
      (g_state == GState::PREPARE) ||
      (g_state == GState::COUNTDOWN) ||
      (g_state == GState::ROUND_NORMAL) ||
      (g_state == GState::ROUND_MUSIC);

  if (!gameActive)
  {
    lightsSetMode(0);
    goalsIllumSet(false);
    return;
  }

  const bool strobes = flagOn(g_set.flags, F_STROBES);
  const bool anim = flagOn(g_set.flags, F_ANIM);
  uint8_t mode = (strobes ? 2 : (anim ? 1 : 0));

  lightsSetMode(mode);
  goalsIllumSet(flagOn(g_set.flags, F_GOALILLUM));
}

static void applyLiveSettingsNow()
{
  const bool roundActive =
      (g_state == GState::ROUND_NORMAL) ||
      (g_state == GState::ROUND_MUSIC);

  lightsApplyFromFlags();

  if (!roundActive)
    return;

  // Nie przerywamy efektu gola w połowie.
  // Po golu animsOnGoal() sam wraca do ANIM_ROUND.
  if (animsGetMode() == ANIM_GOAL)
    return;

  if (g_state == GState::ROUND_MUSIC)
  {
    applyRoundLightsBySettings(true);
  }
  else
  {
    applyRoundLightsBySettings(false);
  }
}

static void roundResetScore()
{
  g_round.scoreA = g_round.scoreB = 0;
  sendScoreUpdate(0, 0);
}
static void puckLockAutoReset();

static void startNormalRound()
{
  blowerSet(true);
  g_postGameAmbientFanActive = false;
  g_musicRound = false;
  g_roundNo++;
  if (g_roundNo > 3)
    g_roundNo = 3;

  // Wyniki 0:0 + wysyłka do wyświetlaczy
  roundResetScore();

  // Limity czasu/goli
  uint16_t secs = mapTimeIdxToSeconds(g_set.gameTimeIdx);
  g_round.tLimitMs = secs ? (uint32_t)secs * 1000UL : 0;
  sendRoundTimeSeconds(secs);

#if DEBUG_FORCE_ROUND_TIME_MS
  g_round.tLimitMs = DEBUG_FORCE_ROUND_TIME_MS;
  sendRoundTimeSeconds((uint16_t)(DEBUG_FORCE_ROUND_TIME_MS / 1000UL));
#endif
  g_round.timeLimited = (g_round.tLimitMs > 0);
  g_round.goalLimit = mapGoalsIdxToCount(g_set.goalsIdx);

  // Start rundy + „guard” 0.5 s (nie kończymy od razu)
  g_round.tStart = millis();
  g_roundGuardUntilMs = g_round.tStart + 500;
  remainLogReset();

  // Czysty start: odblokuj obie (jeśli puck-lock włączony)
  puckLockAutoReset();
  puckLockUnlockAll();

  // Światła wg flag
  lightsApplyFromFlags();

  // Muzyka tła: start tylko jeśli jeszcze nie wystartowała (ma lecieć między rundami)
  musicEnsureBackground();
  // Event + log
  sendGameEvent(1, g_roundNo, 0); // RoundStart
  Serial.printf("[ROUND] NORMAL #%u started: limit %u goals / %lu s (idx g=%u,t=%u)\n",
                g_roundNo, g_round.goalLimit,
                (unsigned long)(g_round.tLimitMs / 1000),
                g_set.goalsIdx, g_set.gameTimeIdx);
applyRoundLightsBySettings(false);
}



static void startMusicRound()
{
  blowerSet(true);

  binStop();
  tableLightsMusicStop();
  g_strobeActive = false;
  g_strobeLampOn = false;
  g_postGameActive = false;

  g_postGameAmbientFanActive = false;
  g_musicRound = true;
  g_roundNo = 3;
  roundResetScore();
  puckLockAutoReset();
  puckLockUnlockAll();

  // per-theme duration for MUSIC round (theme 1..6 => g_theme 0..5)
  uint8_t ti = (uint8_t)g_theme; // 0..5
  g_round.tLimitMs = MUSIC_ROUND_TIME_MS[ti];
  sendRoundTimeSeconds((uint16_t)(g_round.tLimitMs / 1000UL));

#if DEBUG_FORCE_ROUND_TIME_MS
  g_round.tLimitMs = DEBUG_FORCE_ROUND_TIME_MS;
  sendRoundTimeSeconds((uint16_t)(DEBUG_FORCE_ROUND_TIME_MS / 1000UL));
#endif

  g_round.timeLimited = true;
  g_round.tStart = millis();
  g_musicStartMs = g_round.tStart;
  g_roundGuardUntilMs = g_round.tStart + 500; // 0.5 s bufor
  

  remainLogReset();
  lightsApplyFromFlags();

  musicPlayMusicRound();
  sendGameEvent(1, g_roundNo, 0); // RoundStart



  Serial.printf("[ROUND] MUSIC started: %lus (theme=%s)\n",
                (unsigned long)(g_round.tLimitMs / 1000UL),
                THEMES[g_theme].name);
applyRoundLightsBySettings(true);
}




static void endRound(bool finalGameOver = false)
{
  binStop();
  tableLightsMusicStop();
  tableLightsSetGoalsEnabled(false);

  uint8_t winner = 2;
  if (g_round.scoreA > g_round.scoreB) {
    winner = 0;
    g_roundsWonA++;
  } else if (g_round.scoreB > g_round.scoreA) {
    winner = 1;
    g_roundsWonB++;
  }

  sendGameEvent(2, g_round.scoreA, g_round.scoreB);

  if (finalGameOver) {
    sendGameEvent(3, g_round.scoreA, g_round.scoreB);
  }

  Serial.printf("[ROUND] end. score %u:%u | roundsWon A=%u B=%u\n",
                g_round.scoreA, g_round.scoreB,
                g_roundsWonA, g_roundsWonB);
}

static void startCountdown()
{
  sendGameEvent(0); // GameStart (once per game)
  g_state = GState::COUNTDOWN;
}

static uint8_t countdownStep = 3;
static uint32_t countdownT0 = 0;
static void countdownBegin()
{
  countdownStep = 3;
  countdownT0 = millis();
  sfxPlayIndex(3);
  sendGameEvent(4, 3, 0);
  Serial.println("[COUNTDOWN] 3");
}
static bool countdownTick()
{
  uint32_t now = millis();
  if (countdownStep == 3 && now - countdownT0 >= 1000)
  {
    countdownStep = 2;
    sfxPlayIndex(2);
    sendGameEvent(4, 2, 0);
    Serial.println("[COUNTDOWN] 2");
  }
  if (countdownStep == 2 && now - countdownT0 >= 2000)
  {
    countdownStep = 1;
    sfxPlayIndex(1);
    sendGameEvent(4, 1, 0);
    Serial.println("[COUNTDOWN] 1");
  }
  if (countdownStep == 1 && now - countdownT0 >= 3000)
  {
    return true;
  }
  return false;
}

static uint32_t g_breakT0 = 0;
static const uint32_t BREAK_MS = 3000;
static bool g_pendingBetweenRoundSfx = false;
static uint32_t g_betweenRoundSfxAtMs = 0;
static uint32_t g_breakPostGameLookAtMs = 0;

// ========================= Goal input (ADC classifier) =========================
// Returns: -1 none, 0 = A, 1 = B
static int8_t classifyGoalADC(int raw)
{
  if (raw >= adcThreshAmin && raw <= adcThreshAmax)
    return 1; // A
  if (raw >= adcThreshBmin && raw <= adcThreshBmax)
    return 0; // B
  return -1;
}
static uint32_t lastGoalTs = 0;
static int8_t lastGateLatched = -1; // edge-type lock to avoid double count while signal persists
static uint32_t lastNoGoalTs = 0;
static const uint32_t GOAL_RELEASE_MS = 120;
static int readGoalRaw() { return analogRead(PIN_GOAL_ADC); }

static int8_t pollGoal(int *rawOut = nullptr)
{
  if (rawOut) *rawOut = -1;

  // SERIAL injected goal has priority (one-shot)
  if (g_forcedGoal >= 0)
  {
    int8_t g = g_forcedGoal;
    g_forcedGoal = -1;
    Serial.printf("[GOAL] injected side=%c\n", g == 0 ? 'A' : 'B');
    return g;
  }

  uint32_t now = millis();

  // Cooldown
  if (now - lastGoalTs < GOAL_COOLDOWN_MS)
    return -1;

  int raw = readGoalRawMedian();
  if (rawOut) *rawOut = raw;

  int8_t g = classifyGoalADC(raw);

  if (g >= 0)
  {
    // wróciliśmy do aktywnej bramki, więc kasujemy timer "brak gola"
    lastNoGoalTs = 0;

    // jeśli nadal ten sam przelot, ignoruj
    if (lastGateLatched == g)
      return -1;

    lastGateLatched = g;
    lastGoalTs = now;

    Serial.printf("[GOAL] side=%c (ADC=%d)\n", g == 0 ? 'A' : 'B', raw);
    return g;
  }

  // g < 0 -> nie zwalniaj latcha od razu, tylko po chwili stabilnego spokoju
  if (lastGateLatched != -1)
  {
    if (lastNoGoalTs == 0)
      lastNoGoalTs = now;
    else if (now - lastNoGoalTs >= GOAL_RELEASE_MS)
      lastGateLatched = -1;
  }

  return -1;
}
static int8_t pollGoalForGameOver()
{
  // SERIAL injected goal has priority (one-shot)
  if (g_forcedGoal >= 0)
  {
    int8_t g = g_forcedGoal;
    g_forcedGoal = -1;
    Serial.printf("[GOAL] injected (GO) side=%c\n", g == 0 ? 'A' : 'B');
    return g;
  }

  int raw0 = readGoalRawMedian();
  int8_t g = classifyGoalADC(raw0);
  if (g < 0)
    return -1;

  // potwierdzenie ~30 ms: ≥70% trafień w ten sam koszyk
  uint32_t t0 = millis();
  int hits = 0, tries = 0;
  while (millis() - t0 < 30)
  {
    int r = readGoalRawMedian();
    if (classifyGoalADC(r) == g)
      hits++;
    tries++;
    delay(1);
  }

  if (hits < (tries * 7) / 10)
    return -1;

  Serial.printf("[GOAL] (confirm) side=%c (ADC~%d)\n", g == 0 ? 'A' : 'B', raw0);
  return g;
}





// ========================= Puck-lock logic during round =========================

static void updatePuckLockDuringRound()
{
  if (!flagOn(g_set.flags, F_PUCKLOCK))
    return;
  if (g_musicRound)
    return;

  const uint8_t GL = g_round.goalLimit ? g_round.goalLimit : 255;
  const bool a_at = (g_round.scoreA + 1 >= GL);
  const bool b_at = (g_round.scoreB + 1 >= GL);
  const uint32_t now = millis();

  // Jednorazowe „uzbrojenie” po wejściu w stan przedostatni (bez cofania)
  if (a_at && !s_armA)
  {
    s_armA = true;
    s_armT0A = now;
  }
  if (b_at && !s_armB)
  {
    s_armB = true;
    s_armT0B = now;
  }

  // Po 1000 ms od uzbrojenia – zablokuj daną stronę
  uint8_t wantLock = 0;
  if (s_armA && (now - s_armT0A >= PRELAST_LOCK_DELAY_MS))
    wantLock |= 0x01;
  if (s_armB && (now - s_armT0B >= PRELAST_LOCK_DELAY_MS))
    wantLock |= 0x02;

  const uint8_t doLock = wantLock & ~s_puckLockMaskActive;
  if (doLock)
    puckLockCmd(true, doLock);

  // Uwaga: tu nie robimy auto-unlock. Odblokowanie następuje globalnie
  // (np. na starcie nowej rundy albo przy przejściu stanów).
}

static void puckLockAutoReset()
{
  s_armA = false;
  s_armB = false;
  s_armT0A = 0;
  s_armT0B = 0;
  // opcjonalnie też to, jeśli chcesz zresetować aktywną maskę:
  // s_puckLockMaskActive = 0;
}

// ========================= CAN core =========================
static twai_general_config_t gconf = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
static twai_timing_config_t tconf = TWAI_TIMING_CONFIG_250KBITS();
static twai_filter_config_t fconf = TWAI_FILTER_CONFIG_ACCEPT_ALL();

static void sendAck()
{
  twai_message_t m = {};
  m.identifier = 0x311;
  m.data_length_code = 1;
  m.data[0] = 0xAA;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

// Forward decls
static void gameStartFull(uint8_t index);
static void gameStartMusicOnlyRandom();
static void startMusicRound();
static void musicStop();
static void blowerSet(bool on);
static void puckLockCmd(bool lock, uint8_t mask);
static void puckLockAutoReset();
static void puckLockUnlockAll();
static void lightsApplyFromFlags();
static void settingsSaveToNVS();
static void printSettings(const AHSettings &s);
static void sayStart(uint8_t gtype, uint8_t idx);
static void sfxPlayIndex(uint16_t idx);
static void applyLiveSettingsNow();

// ========================= SERIAL CONSOLE (TEST) =========================
static bool g_serialEcho = true;

static const char *stateName(GState s)
{
  switch (s)
  {
  case GState::IDLE:
    return "IDLE";
  case GState::PREPARE:
    return "PREPARE";
  case GState::COUNTDOWN:
    return "COUNTDOWN";
  case GState::ROUND_NORMAL:
    return "ROUND_NORMAL";
  case GState::ROUND_BREAK:
    return "ROUND_BREAK";
  case GState::ROUND_MUSIC:
    return "ROUND_MUSIC";
  case GState::GAME_OVER:
    return "GAME_OVER";
  case GState::WAIT_PUCK_CLEAR:
    return "WAIT_PUCK_CLEAR";
  default:
    return "?";
  }
}

static void printState()
{
  Serial.printf("[STATE] %s | roundNo=%u | score %u:%u | roundsWon A=%u B=%u | blower=%d | puckActive=0x%02X\n",
                stateName(g_state), g_roundNo, g_round.scoreA, g_round.scoreB, g_roundsWonA, g_roundsWonB,
                blowerOn ? 1 : 0, s_puckLockMaskActive);
  Serial.print("        ");
  printSettings(g_set);
  Serial.println();
}

static uint8_t parseHexByte(const char *s, bool *ok)
{
  *ok = false;
  if (!s || !*s)
    return 0;
  char *end = nullptr;
  long v = strtol(s, &end, 16);
  if (end == s)
    return 0;
  if (v < 0)
    v = 0;
  if (v > 255)
    v = 255;
  *ok = true;
  return (uint8_t)v;
}

static void hardResetToIdle()
{

  g_postGameAmbientFanActive = false;
  g_manualLampActive = false;
  g_manualLampUntilMs = 0;
  fanSetPercent(0);
  musicStop();
  blowerSet(false);
  puckLockAutoReset();
  puckLockUnlockAll();
  lightsSetMode(0);
  g_state = GState::IDLE;
  g_roundNo = 0;
  g_roundsWonA = g_roundsWonB = 0;
  g_round.scoreA = g_round.scoreB = 0;
  g_musicRound = false;
  Serial.println("[RESET] -> IDLE");
}

static void serialHelp()
{
  Serial.println("\n=== SERIAL COMMANDS ===");
  Serial.println("help");
  Serial.println("echo on|off");
  Serial.println("state");
  Serial.println("reset");
  Serial.println("settings");
  Serial.println("set <timeIdx> <goalsIdx> <flagsHex>    (np: set 1 1 3F)");
  Serial.println("flags <flagsHex>                       (np: flags 1F)");
  Serial.println("start full <idx>                       (np: start full 0)");
  Serial.println("start music                            (debug: od razu muzyczna)");
  Serial.println("goal a|b                               (wstrzyknij gola bez czujnika)");
  Serial.println("blower on|off");
  Serial.println("fan <0..100>|on|off");
  Serial.println("lock a|b|ab");
  Serial.println("unlock a|b|ab");
  Serial.println("mp3 stop");
  Serial.println("mp3 teensies <fileNo>                  (folder 01)");
  Serial.println("sfx <idx>");
  Serial.println("strobe <hz>|a|b                        (strobe10, strobe b=strobeb.bin, strobe a=strobea.bin)");
  Serial.println("=======================\n");
}

static void adcWatchPoll()
{
  if (!g_adcWatch)
    return;

  uint32_t now = millis();
  if (g_adcWatchUntilMs && now > g_adcWatchUntilMs)
  {
    g_adcWatch = false;
    Serial.println("[ADC] watch stop");
    return;
  }
  if (now - g_adcWatchLastMs < g_adcWatchPeriodMs)
    return;
  g_adcWatchLastMs = now;

  int raw = readGoalRawMedian();   // już masz w pliku
  int8_t g = classifyGoalADC(raw); // już masz w pliku

  const float v = (raw / 4095.0f) * 3.3f;
  Serial.printf("[ADC] raw=%d  V=%.3f  class=%s\n",
                raw, v, (g == 0 ? "A" : (g == 1 ? "B" : "-")));
}

static void serialHandleLine(char *line)
{
  const int MAXTOK = 6;
  char *tok[MAXTOK] = {0};
  int n = 0;
  for (char *p = strtok(line, " \t\r\n"); p && n < MAXTOK; p = strtok(nullptr, " \t\r\n"))
    tok[n++] = p;
  if (!n)
    return;

  if (!strcasecmp(tok[0], "help") || !strcasecmp(tok[0], "?"))
  {
    serialHelp();
    return;
  }
  if (!strcasecmp(tok[0], "echo") && n >= 2)
  {
    if (!strcasecmp(tok[1], "on"))
      g_serialEcho = true;
    else if (!strcasecmp(tok[1], "off"))
      g_serialEcho = false;
    Serial.printf("[SER] echo=%s\n", g_serialEcho ? "on" : "off");
    return;
  }
  if (!strcasecmp(tok[0], "state"))
  {
    printState();
    return;
  }
  if (!strcasecmp(tok[0], "reset"))
  {
    hardResetToIdle();
    return;
  }

    if (!strncasecmp(tok[0], "strobe", 6))
{
  // obsługa:
  // strobe10
  // strobe 10
  // strobe a
  // strobe b
  // strobe 0

  if (n >= 2 && (!strcasecmp(tok[1], "a") || !strcasecmp(tok[1], "b")))
{
  g_strobeActive = false;
  g_strobeLampOn = false;

  animsSetMode(ANIM_ROUND);   // <-- to jest klucz

if (!strcasecmp(tok[1], "a"))
{
  binStart("/anims/strobea.bin", millis());
  Serial.println("[STROBE] BIN A");
}
else
{
  binStart("/anims/strobeb.bin", millis());
  Serial.println("[STROBE] BIN B");
}
  return;
}
  int hz = 0;

  if (strlen(tok[0]) > 6)
    hz = atoi(tok[0] + 6);
  else if (n >= 2)
    hz = atoi(tok[1]);
if (hz <= 0)
{
  g_strobeActive = false;
  g_strobeLampOn = false;
  binStop();
  animsSetMode(ANIM_OFF);
  matrixFill(0, 0, 0);
  matrixShow();
  Serial.println("[STROBE] OFF");
  return;
}

  binStop(); // wyłącz BIN-y, jeśli leciały
  g_strobeIntervalMs = 1000UL / (uint32_t)(hz * 2);
  if (g_strobeIntervalMs == 0)
    g_strobeIntervalMs = 1;

  g_strobeLastToggleMs = millis();
  g_strobeLampOn = false;
  g_strobeActive = true;

  Serial.printf("[STROBE] ON %d Hz (toggle %lu ms)\n",
                hz, (unsigned long)g_strobeIntervalMs);
  return;
}




  

  // ---- ADC debug ----
  if (!strcasecmp(tok[0], "adc"))
  {
    if (n == 1)
    {
      int raw = readGoalRawMedian();
      int8_t g = classifyGoalADC(raw);
      float v = (raw / 4095.0f) * 3.3f;
      Serial.printf("[ADC] raw=%d  V=%.3f  class=%s\n",
                    raw, v, (g == 0 ? "A" : (g == 1 ? "B" : "-")));
      return;
    }
    if (n >= 2 && !strcasecmp(tok[1], "watch"))
    {
      // adc watch [period_ms] [seconds]
      if (n >= 3)
        g_adcWatchPeriodMs = (uint32_t)atoi(tok[2]);
      if (n >= 4)
        g_adcWatchUntilMs = millis() + (uint32_t)atoi(tok[3]) * 1000UL;
      else
        g_adcWatchUntilMs = 0; // bez limitu
      g_adcWatch = true;
      g_adcWatchLastMs = 0;
      Serial.printf("[ADC] watch start period=%ums\n", (unsigned)g_adcWatchPeriodMs);
      return;
    }
    if (n >= 2 && (!strcasecmp(tok[1], "stop") || !strcasecmp(tok[1], "off")))
    {
      g_adcWatch = false;
      Serial.println("[ADC] watch stop");
      return;
    }
    Serial.println("[SER] adc | adc watch [period_ms] [seconds] | adc stop");
    return;
  }

  // ---- Settings ----
  if (!strcasecmp(tok[0], "settings"))
  {
    Serial.print("[SET] ");
    printSettings(g_set);
    Serial.println();
    return;
  }
  if (!strcasecmp(tok[0], "set") && n >= 4)
  {
    g_set.gameTimeIdx = (uint8_t)atoi(tok[1]);
    g_set.goalsIdx = (uint8_t)atoi(tok[2]);
    bool ok = false;
    g_set.flags = parseHexByte(tok[3], &ok);
    if (!ok)
      Serial.println("[SET] flagsHex parse error -> 0");
    settingsSaveToNVS();
    Serial.print("[SET] updated -> ");
    printSettings(g_set);
    Serial.println();
    lightsApplyFromFlags();
    return;
  }
  if (!strcasecmp(tok[0], "flags") && n >= 2)
  {
    bool ok = false;
    g_set.flags = parseHexByte(tok[1], &ok);
    if (!ok)
      Serial.println("[SET] flagsHex parse error");
    settingsSaveToNVS();
    Serial.print("[SET] flags -> ");
    printSettings(g_set);
    Serial.println();
    lightsApplyFromFlags();
    return;
  }

  // ---- Blower ----
  if (!strcasecmp(tok[0], "blower") && n >= 2)
  {
    if (!strcasecmp(tok[1], "on"))
      blowerSet(true);
    else if (!strcasecmp(tok[1], "off"))
      blowerSet(false);
    else
      Serial.println("[SER] blower on|off");
    return;
  }

// ---- fan ------
if (!strcasecmp(tok[0], "fan") && n >= 2)
{
  if (!strcasecmp(tok[1], "on"))
  {
    fanSetPercent(100);
    return;
  }

  if (!strcasecmp(tok[1], "off"))
  {
    fanSetPercent(0);
    return;
  }

  int pct = atoi(tok[1]);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  fanSetPercent((uint8_t)pct);
  return;
}


  // ---- Puck lock ----
  if ((!strcasecmp(tok[0], "lock") || !strcasecmp(tok[0], "unlock")) && n >= 2)
  {
    bool lock = !strcasecmp(tok[0], "lock");
    uint8_t mask = 0;
    if (!strcasecmp(tok[1], "a"))
      mask = 0x01;
    else if (!strcasecmp(tok[1], "b"))
      mask = 0x02;
    else if (!strcasecmp(tok[1], "ab") || !strcasecmp(tok[1], "all"))
      mask = 0x03;
    else
    {
      Serial.println("[SER] lock/unlock a|b|ab");
      return;
    }
    puckLockCmd(lock, mask);
    return;
  }

  // ---- Injected goals ----
  if (!strcasecmp(tok[0], "goal") && n >= 2)
  {
    if (!strcasecmp(tok[1], "a"))
      g_forcedGoal = 0;
    else if (!strcasecmp(tok[1], "b"))
      g_forcedGoal = 1;
    else
    {
      Serial.println("[SER] goal a|b");
      return;
    }
    Serial.printf("[SER] injected goal=%c\n", g_forcedGoal == 0 ? 'A' : 'B');
    return;
  }

  // ---- Start ----
  if (!strcasecmp(tok[0], "start") && n >= 2)
  {
    if (!strcasecmp(tok[1], "full") && n >= 3)
    {
      uint8_t idx = (uint8_t)atoi(tok[2]);
      sayStart(GT_FULL, idx);
      gameStartFull(idx);
      return;
    }
    if (!strcasecmp(tok[1], "music"))
    {
      // start music [1..6] (bez parametru = losowo)
      if (n >= 3)
      {
        int idx = atoi(tok[2]);

        if (idx < 1 || idx > 6)
        {
          Serial.println("[ERR] start music: dozwolone tylko 1..6 (bez parametru = losowo)");
          return;
        }

        // inicjalizacja identyczna jak w gameStartMusicOnlyRandom(), tylko temat z idx
        g_musicOnlyMode = true;
        g_theme = (ThemeId)(idx - 1); // 1..6 -> 0..5

        g_fullIndex = 0;
        g_roundNo = 0;
        g_roundsWonA = g_roundsWonB = 0;
        g_round.scoreA = g_round.scoreB = 0;

        poolResetTheme();
        g_bgPlaying = false;

        Serial.printf("[GAME] MUSIC-ONLY: theme=%s (folder %02u)", THEMES[g_theme].name, THEMES[g_theme].folder);
        g_state = GState::PREPARE;
        return;
      }

      // bez parametru: losowo
      gameStartMusicOnlyRandom();
      return;
    }

    Serial.println("[SER] start full <idx> | start music [1..6]");
    return;
  }

  // ---- MP3 ----
  if (!strcasecmp(tok[0], "mp3") && n >= 2)
  {
    if (!strcasecmp(tok[1], "stop"))
    {
      musicStop();
      return;
    }
    if (!strcasecmp(tok[1], "teensies") && n >= 3)
    {
      uint8_t file = (uint8_t)atoi(tok[2]);
      musicPlayTrack(file);
      return;
    }
    Serial.println("[SER] mp3 stop | mp3 teensies <fileNo>");
    return;
  }

  // ---- SFX ----
  if (!strcasecmp(tok[0], "sfx") && n >= 2)
  {
    uint16_t idx = (uint16_t)atoi(tok[1]);
    sfxPlayIndex(idx);
    return;
  }

  Serial.println("[SER] unknown. type: help");
}

static void serialPoll()
{
  static char buf[96];
  static uint8_t len = 0;

  while (Serial.available())
  {
    int c = Serial.read();
    if (c < 0)
      break;

    if (c == '\n' || c == '\r')
    {
      if (!len)
        continue;
      buf[len] = 0;
      if (g_serialEcho)
        Serial.printf("\n> %s\n", buf);
      serialHandleLine(buf);
      len = 0;
      continue;
    }

    if (c == 8 || c == 127)
    {
      if (len)
        len--;
      continue;
    } // backspace

    if (len < sizeof(buf) - 1)
      buf[len++] = (char)c;
  }
}



static void sendGoalCalValue(uint8_t cmd, uint16_t raw)
{
  twai_message_t m = {};
  m.identifier = CAN_ID_GOALCAL_DATA;
  m.data_length_code = 3;
  m.data[0] = cmd;
  m.data[1] = (uint8_t)(raw >> 8);
  m.data[2] = (uint8_t)(raw & 0xFF);

  esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(20));
  if (err != ESP_OK) {
    Serial.printf("[GCAL-MAIN] TX err=%d cmd=0x%02X raw=%u\n", (int)err, cmd, raw);
  }
}

static void goalCalMainStart(GoalCalMainMode mode)
{
g_goalCalMain.active = true;
g_goalCalMain.mode = mode;
g_goalCalMain.startMs = millis();

g_goalCalPulseActive = false;

if (mode == GCALM_A) {
  g_goalCalMain.idleSent = false;
  Serial.println("[GCAL-MAIN] idle measurement enabled");
} else {
  g_goalCalMain.idleSent = true;
  Serial.println("[GCAL-MAIN] idle measurement skipped for B");
}

  lastGateLatched = -1;
  lastGoalTs = 0;
  lastNoGoalTs = 0;

  blowerSet(true);
  puckLockUnlockAll();

  Serial.printf("[GCAL-MAIN] START mode=%s\n", mode == GCALM_A ? "A" : "B");
}

static void goalCalMainCancel()
{
  if (!g_goalCalMain.active) return;

  Serial.println("[GCAL-MAIN] CANCEL");
  g_goalCalMain = GoalCalMainState{};

  blowerSet(false);
  puckLockCmd(true, 0x03);
}

static void goalCalMainTick()
{
  if (!g_goalCalMain.active) return;

  uint32_t now = millis();

  if (!g_goalCalMain.idleSent) {
    if (now - g_goalCalMain.startMs >= 3000) {
uint16_t idle = (uint16_t)readGoalRawMedian();
adcIdle = idle;
sendGoalCalValue(CAN_CMD_GOALCAL_IDLE, idle);
g_goalCalMain.idleSent = true;

      Serial.printf("[GCAL-MAIN] idle=%u mode=%s\n",
                    idle,
                    g_goalCalMain.mode == GCALM_A ? "A" : "B");
    }
    return;
  }

int raw = readGoalRawMedian();

// podczas kalibracji:
// - NIE klasyfikujemy A/B
// - NIE używamy progów A/B
// - bierzemy każde przejście pucka jako próbkę
// - ale tylko raz na jedno przejście

bool active = (raw < (int)adcIdle - 200);

// wejście w impuls -> wyślij jedną próbkę
if (active && !g_goalCalPulseActive) {
  g_goalCalPulseActive = true;

  sendGoalCalValue(CAN_CMD_GOALCAL_SAMPLE, (uint16_t)raw);

  Serial.printf("[GCAL-MAIN] sample mode=%s raw=%d idle=%u\n",
                g_goalCalMain.mode == GCALM_A ? "A" : "B",
                raw,
                adcIdle);
}

// wyjście z impulsu -> uzbrój na następne przejście pucka
if (!active) {
  g_goalCalPulseActive = false;
}
}


// ANIMACJE led lampa gora z pliku
static File binFile;
static bool binPlaying = false;


#define BIN_PIXELS 1024
#define BIN_FRAME_SIZE (BIN_PIXELS * 3)
#define BIN_FPS 20
#define BIN_FRAME_MS (1000 / BIN_FPS)

static const char *binPath = nullptr;

static uint32_t binFrameCount = 0;        // ile pełnych klatek w pliku
static uint32_t binLastRenderedFrame = UINT32_MAX; // żeby nie renderować 2x tej samej

void binStop()
{
  if (binFile)
    binFile.close();

  binPlaying = false;
  binFrameCount = 0;
  binLastRenderedFrame = UINT32_MAX;

  tableLightsMusicStop();
}

bool binStart(const char *path, uint32_t syncStartMs)
{
  binStop();

  binPath = path;
  binFile = SD.open(binPath, FILE_READ);

  if (!binFile)
  {
    Serial.printf("[BIN] open failed: %s\n", binPath);
    return false;
  }

  uint32_t size = (uint32_t)binFile.size();
  binFrameCount = size / BIN_FRAME_SIZE;

  if (binFrameCount == 0)
  {
    Serial.printf("[BIN] empty/too small: %s size=%lu\n",
                  binPath,
                  (unsigned long)size);

    binFile.close();
    return false;
  }

  // TO JEST KLUCZ:
  // nie ustawiamy startu animacji na millis(),
  // tylko na start rundy muzycznej / start piosenki.
  g_musicStartMs = syncStartMs;

  binPlaying = true;
  binLastRenderedFrame = UINT32_MAX;

  uint32_t now = millis();
  uint32_t elapsedMs = now - g_musicStartMs;
  uint32_t frameIndex = ((elapsedMs * BIN_FPS) / 1000UL) % binFrameCount;

  Serial.printf("[BIN] start sync path=%s frames=%lu size=%lu syncStart=%lu elapsed=%lu frame=%lu\n",
                binPath,
                (unsigned long)binFrameCount,
                (unsigned long)size,
                (unsigned long)g_musicStartMs,
                (unsigned long)elapsedMs,
                (unsigned long)frameIndex);

  return true;
}




static void adcThreshSaveToNVS()
{
  prefs.begin("ah", false);
  prefs.putUShort("adcAmin", adcThreshAmin);
  prefs.putUShort("adcAmax", adcThreshAmax);
  prefs.putUShort("adcBmin", adcThreshBmin);
  prefs.putUShort("adcBmax", adcThreshBmax);
  prefs.end();

  Serial.printf("[ADC] saved to NVS A:%u-%u B:%u-%u\n",
                adcThreshAmin, adcThreshAmax,
                adcThreshBmin, adcThreshBmax);
}

static void adcThreshLoadFromNVS()
{
  prefs.begin("ah", true);

  adcThreshAmin = prefs.getUShort("adcAmin", 2200);
  adcThreshAmax = prefs.getUShort("adcAmax", 3000);
  adcThreshBmin = prefs.getUShort("adcBmin", 800);
  adcThreshBmax = prefs.getUShort("adcBmax", 1600);

  prefs.end();

  Serial.printf("[ADC] loaded from NVS A:%u-%u B:%u-%u\n",
                adcThreshAmin, adcThreshAmax,
                adcThreshBmin, adcThreshBmax);
}

static uint16_t xyToIndex(uint8_t x, uint8_t y)
{
  // matryca 16x64, serpentyna po wierszach
  if (y % 2 == 0)
    return y * 16 + x;
  else
    return y * 16 + (15 - x);
}

static void showPurpleWithCorners(uint8_t bgLevel)
{
  const uint8_t cornerR = 150;
  const uint8_t cornerG = 0;
  const uint8_t cornerB = 255;

  // tło całej matrycy
  matrixFill(bgLevel, 0, bgLevel);

  // 3x3 lewy górny
  for (uint8_t y = 0; y < 3; y++) {
    for (uint8_t x = 0; x < 3; x++) {
      matrixSetIndex(xyToIndex(x, y), cornerR, cornerG, cornerB);
    }
  }

  // 3x3 prawy górny
  for (uint8_t y = 0; y < 3; y++) {
    for (uint8_t x = 13; x < 16; x++) {
      matrixSetIndex(xyToIndex(x, y), cornerR, cornerG, cornerB);
    }
  }

  // 3x3 lewy dolny
  for (uint8_t y = 61; y < 64; y++) {
    for (uint8_t x = 0; x < 3; x++) {
      matrixSetIndex(xyToIndex(x, y), cornerR, cornerG, cornerB);
    }
  }

  // 3x3 prawy dolny
  for (uint8_t y = 61; y < 64; y++) {
    for (uint8_t x = 13; x < 16; x++) {
      matrixSetIndex(xyToIndex(x, y), cornerR, cornerG, cornerB);
    }
  }

  matrixShow();
}



static float g_uvCurrent = -1.0f; // żeby nie spamować PWM bez potrzeby
static void uvSetLevel(float duty) // 0.0 - 1.0
{
  if (duty < 0.0f) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;

  // zabezpieczenie: jeśli UV wyłączone w ustawieniach → zawsze 0
  if (!flagOn(g_set.flags, F_UV))
    duty = 0.0f;

  // jeśli nic się nie zmieniło → nie pisz ponownie PWM
  if (fabsf(duty - g_uvCurrent) < 0.0005f)
    return;

  g_uvCurrent = duty;

  uint8_t pwm = pwmDutyUv(duty);
  ledcWrite(UV_PWM_CH, pwm);

  Serial.printf("[UV] level=%.3f (PWM=%u/255)\n", duty, (unsigned)pwm);
}


static void applyPostGameLights()
{
  bool uvEnabled = flagOn(g_set.flags, F_UV);

  if (uvEnabled)
  {
    uvSetLevel(UV_LEVEL_AMBIENT_DUTY);   
    matrixFill(0, 0, 0);
    matrixShow();
  }
  else
  {
    uvSetLevel(0.0f);                    // <-- dla porządku też jawnie
    showPurpleWithCorners(0);
  }
}


static void enterGameOver()
{
  lightsSetMode(0);
  puckLockCmd(true, 0x03);

  animsSetBreathing(false);
  animsSetMode(ANIM_OFF);

  g_postGameActive = true;
  g_postGameStartMs = millis();

  applyPostGameLights();
  g_postGameAmbientFanActive = true;

  g_state = GState::GAME_OVER;
  g_gameOverT0 = millis();
  g_gameOverGuardUntilMs = millis() + 500;

  // Koniec całego meczu
  sfxPlayFolderFile(1, 2);

 
}

static void debugScanPixels()
{
  static uint16_t idx = 0;
  static uint32_t last = 0;

  if (millis() - last < 250)
    return;

  last = millis();

  matrixFill(0, 0, 0);

  // jeden pixel na fioletowo
  matrixSetIndex(idx, 150, 0, 255);

  matrixShow();

  Serial.printf("[PIXEL] idx=%u\n", idx);

  idx++;
  if (idx >= 1024)
    idx = 0;
}


static void showFullPurple(uint8_t level)
{
  matrixFill(level, 0, level);
  matrixShow();
}

static void applyIdleAmbientLights()
{
  if (flagOn(g_set.flags, F_UV))
  {
    
    matrixFill(0, 0, 0);
    matrixShow();
  }
  else
  {
 
    showPurpleWithCorners(0);
  }
}




static void runBootLightSequence()
{
  const bool uvEnabled = flagOn(g_set.flags, F_UV);

  const uint16_t steps = 80;
  const uint16_t stepDelayMs = 12;

  // start od ciemności
  matrixFill(0, 0, 0);
  matrixShow();


  // =========================
  // FADE IN
  // =========================
  for (uint16_t i = 0; i <= steps; i++)
  {
    float t = (float)i / (float)steps;
    float k = t * t * (3.0f - 2.0f * t);   // smoothstep

    uint8_t level = (uint8_t)(k * 255.0f + 0.5f);

    matrixFill(level, 0, level);
    matrixShow();

   
  }

  delay(250);

  // =========================
  // FADE OUT
  // =========================
  if (uvEnabled)
  {
    // matryca do 0, UV do 1%
    for (int i = steps; i >= 0; i--)
    {
      float t = (float)i / (float)steps;
      float k = t * t * (3.0f - 2.0f * t);

      uint8_t level = (uint8_t)(k * 255.0f + 0.5f);

      matrixFill(level, 0, level);
      matrixShow();

      // 40% -> 1%
      float uv = 0.01f + (0.40f - 0.01f) * k;


      delay(stepDelayMs);
    }

    matrixFill(0, 0, 0);
    matrixShow();

  }
  else
  {
// środek wygasa, rogi zostają
for (int i = steps; i >= 0; i--)
{
  float t = (float)i / (float)steps;
  float k = t * t * (3.0f - 2.0f * t);

  uint8_t level = (uint8_t)(k * 255.0f + 0.5f);

  showPurpleWithCorners(level);

  delay(stepDelayMs);
}


showPurpleWithCorners(0);
  }

  // 10 minut ambientu po starcie
  g_postGameActive = true;
  
  g_postGameStartMs = millis();
}

static void strobeTick()
{
  if (!g_strobeActive)
    return;

  uint32_t now = millis();

  // zmiana stanu co interval
  if (now - g_strobeLastToggleMs >= g_strobeIntervalMs)
  {
    g_strobeLastToggleMs = now;
    g_strobeLampOn = !g_strobeLampOn;
  }

  // 👉 TO JEST KLUCZ:
  // rysuj ZA KAŻDYM razem, nie tylko przy zmianie

  if (g_strobeLampOn)
    matrixFill(255, 255, 255);
  else
    matrixFill(0, 0, 0);

  matrixShow();
}



static float uvDutyFromAnimLevel(UvLevel level)
{
  switch (level)
  {
    case UV_LEVEL_OFF:   return UV_LEVEL_IDLE_DUTY;
    case UV_LEVEL_BASE:  return UV_LEVEL_BASE_DUTY;
    case UV_LEVEL_BOOST: return UV_LEVEL_BOOST_DUTY;
    default:             return UV_LEVEL_IDLE_DUTY;
  }
}







static float uvTargetFromState()
{
  if (g_postGameActive)
  {
    return flagOn(g_set.flags, F_UV) ? UV_LEVEL_AMBIENT_DUTY : 0.0f;
  }

  switch (g_state)
  {
    case GState::PREPARE:
    case GState::COUNTDOWN:
    case GState::ROUND_NORMAL:
    case GState::ROUND_MUSIC:
      return flagOn(g_set.flags, F_UV) ? UV_LEVEL_BASE_DUTY : 0.0f;

    default:
      return 0.0f;
  }
}




static void uvApply()
{
  uvSetLevel(uvTargetFromState());
}

// ========================= Arduino setup/loop =========================
void setup()
{
  Serial.begin(115200);

  sd_init();
  sd_list_root();
  sd_check_all_anims();

  ledcSetup(UV_PWM_CH, UV_PWM_HZ, UV_PWM_RES);
ledcAttachPin(PIN_UV_DIM, UV_PWM_CH);

uvSetLevel(UV_LEVEL_IDLE_DUTY);


//ledcWrite(UV_PWM_CH, 0);   // startowo zgaszone
g_uvCur = false;

ledcSetup(FAN_PWM_CH, FAN_PWM_HZ, FAN_PWM_RES);
fanSetPercent(0);


  File f = SD.open(TOAD_PATH, FILE_READ);
  if (!f)
  {
    Serial.printf("[BIN] brak pliku: %s\n", TOAD_PATH);
  }
  else
  {
    Serial.printf("[BIN] %s size=%u bytes\n", TOAD_PATH, (unsigned)f.size());
    f.close();
  }
#ifdef ESP32
  WiFi.mode(WIFI_OFF);
  btStop();
#endif
matrixInit(200); // jasnosc startowa
tableLightsInit();

animsInit();
animsSetMode(ANIM_OFF);

analogReadResolution(12);
analogSetPinAttenuation(PIN_GOAL_ADC, ADC_11db);

// Jeden seed, porządny (HW RNG + drobna entropia)
randomSeed((uint32_t)esp_random() ^ (uint32_t)micros() ^ (uint32_t)analogRead(PIN_GOAL_ADC));

puckInit();

pinMode(DF_BUSY_PIN, INPUT);

pinMode(PIN_BLOWER, OUTPUT);
digitalWrite(PIN_BLOWER, !BLOWER_ACTIVE); // wymuś OFF zanim reszta ruszy

// MP3 UARTs
MusicSerial.begin(9600, SERIAL_8N1, MP3_MUSIC_RX, MP3_MUSIC_TX);
SfxSerial.begin(9600, SERIAL_8N1, MP3_SFX_RX, MP3_SFX_TX);
mp3SetVol(MusicSerial, 28);
mp3SetVol(SfxSerial, 30);

// CAN
if (twai_driver_install(&gconf, &tconf, &fconf) != ESP_OK)
{
  Serial.println("[CAN] driver_install ERR");
  while (true)
    delay(1000);
}
if (twai_start() != ESP_OK)
{
  Serial.println("[CAN] start ERR");
  while (true)
    delay(1000);
}
twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED, nullptr);

// NAJPIERW wczytaj zapisane ustawienia
settingsLoadFromNVS();
adcThreshLoadFromNVS();
printSettings(g_set);

// DOPIERO TERAZ odpal sekwencję startową, bo używa F_UV
//runBootLightSequence();

  Serial.printf("\n== CAN RX @250kbps  TX=%d RX=%d ==\n", (int)CAN_TX_PIN, (int)CAN_RX_PIN);
  Serial.println("[READY] Waiting for START (0x301: 01 gtype gindex)");
}

// Forward decls
static void gameStartFull(uint8_t index);
static void gameStartMusicOnlyRandom();

static inline bool uvShouldBeOn()
{
  // UV tylko podczas aktywnej gry i tylko jeśli flaga UV jest ON w ustawieniach
  if (!flagOn(g_set.flags, F_UV)) return false;

  switch (g_state)
  {
    case GState::ROUND_NORMAL:
    case GState::ROUND_MUSIC:
    case GState::PREPARE:
    case GState::COUNTDOWN:
      return true;

    default:
      return false;
  }
}

void loop()
{


  // =========================================================
  // 1) POST-GAME TIMEOUT
  // =========================================================
  // Po 10 minutach od końca gry gasimy wszystko całkowicie.
  if (g_postGameActive)
  {
    uint32_t now = millis();

    if (now - g_postGameStartMs >= POST_GAME_DURATION_MS)
    {
      
matrixFill(0, 0, 0);
tableLightsAllOff();
matrixShow();

g_postGameActive = false;
g_postGameAmbientFanActive = false;
      Serial.println("[POST GAME] timeout -> all OFF");
    }
  }

  // =========================================================
  // 2) TICKI OGÓLNE / DEBUG / AUDIO
  // =========================================================
  serialPoll();
  adcWatchPoll();
  musicAutoAdvanceTick();
  goalCalMainTick();

  // =========================================================
  // 3) ODBIÓR CAN
  // =========================================================
  twai_message_t msg;
  if (twai_receive(&msg, pdMS_TO_TICKS(10)) == ESP_OK)
  {
    bool handled = false;

    // ---------------------------------------------------------
    // 3.1) Puck-lock low level handler
    // ---------------------------------------------------------
    // To ma działać zawsze, niezależnie od dalszego parsowania.
    puckOnCan(msg);

    // ---------------------------------------------------------
    // 3.2) ADC thresholds update z settings-unit
    // CAN ID 0x360, payload 8 bajtów:
    // [AminH AminL AmaxH AmaxL BminH BminL BmaxH BmaxL]
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == 0x360 &&
        msg.data_length_code == 8)
    {
      adcThreshAmin = ((uint16_t)msg.data[0] << 8) | msg.data[1];
      adcThreshAmax = ((uint16_t)msg.data[2] << 8) | msg.data[3];
      adcThreshBmin = ((uint16_t)msg.data[4] << 8) | msg.data[5];
      adcThreshBmax = ((uint16_t)msg.data[6] << 8) | msg.data[7];

      Serial.printf("[ADC] thresholds updated A:%u-%u B:%u-%u\n",
                    adcThreshAmin, adcThreshAmax,
                    adcThreshBmin, adcThreshBmax);

      adcThreshSaveToNVS();
      handled = true;
    }

    // ---------------------------------------------------------
    // 3.3) START GAME
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_START_GAME &&
        msg.data_length_code >= 3 &&
        msg.data[0] == CAN_CMD_START_GAME)
    {
      handled = true;

      uint8_t gtype  = msg.data[1];
      uint8_t gindex = msg.data[2];

      sayStart(gtype, gindex);
      sendAck();

      if (gtype == GT_FULL)
      {
        gameStartFull(gindex);
      }
      else if (gtype == GT_MUSIC)
      {
        if (gindex < TH_COUNT)
        {
          g_musicOnlyMode = true;
          g_theme = (ThemeId)gindex;

          g_fullIndex = 0;
          g_roundNo = 0;
          g_roundsWonA = g_roundsWonB = 0;
          g_round.scoreA = g_round.scoreB = 0;

          poolResetTheme();
          g_bgPlaying = false;

          Serial.printf("[GAME] MUSIC-ONLY: theme=%s (folder %02u)\n",
                        THEMES[g_theme].name,
                        THEMES[g_theme].folder);

          g_state = GState::PREPARE;
        }
        else
        {
          gameStartMusicOnlyRandom();
        }
      }
      else
      {
        Serial.printf("[START] Unknown game type %u\n", gtype);
      }
    }

    // ---------------------------------------------------------
    // 3.4) Goal calibration control
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_GOALCAL_CMD &&
        msg.data_length_code >= 1)
    {
      handled = true;

      switch (msg.data[0])
      {
        case CAN_CMD_GOALCAL_START_A:
          Serial.println("[CAN] GoalCal START A");
          goalCalMainStart(GCALM_A);
          break;

        case CAN_CMD_GOALCAL_START_B:
          Serial.println("[CAN] GoalCal START B");
          goalCalMainStart(GCALM_B);
          break;

        case CAN_CMD_GOALCAL_CANCEL:
          Serial.println("[CAN] GoalCal CANCEL");
          goalCalMainCancel();
          break;

        default:
          Serial.printf("[CAN] GoalCal unknown cmd=0x%02X\n", msg.data[0]);
          break;
      }
    }

    // ---------------------------------------------------------
    // 3.5) Settings update
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_SETTINGS &&
        msg.data_length_code >= 4 &&
        msg.data[0] == CAN_CMD_SETTINGS)
{
  handled = true;

  const uint8_t oldFlags = g_set.flags;

  g_set.gameTimeIdx = msg.data[1];
  g_set.goalsIdx    = msg.data[2];
  g_set.flags       = msg.data[3];

  settingsSaveToNVS();
  sendSettingsAck();

  Serial.print("[CAN] SettingsUpdate -> ");
  printSettings(g_set);
  Serial.println();

  if (oldFlags != g_set.flags)
  {
    applyLiveSettingsNow();
  }
}

    // ---------------------------------------------------------
    // 3.6) PuckLock NOW – log
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_PUCKLOCK_CMD &&
        msg.data_length_code >= 2 &&
        msg.data[0] == CAN_CMD_PUCKLOCK_NOW)
    {
      handled = true;
      logFrame(msg);

      uint8_t act = msg.data[1];

      if (act == CAN_PUCK_LOCK)
      {
        if (msg.data_length_code >= 3)
        {
          uint8_t mask = msg.data[2];
          Serial.printf("           -> PUCKLOCK: Lock now   (A=%d, B=%d)\n",
                        (mask & 1) != 0, (mask & 2) != 0);
        }
        else
        {
          Serial.println("           -> PUCKLOCK: Lock now   (A+B)");
        }
      }
      else if (act == CAN_PUCK_UNLOCK)
      {
        if (msg.data_length_code >= 3)
        {
          uint8_t mask = msg.data[2];
          Serial.printf("           -> PUCKLOCK: Unlock now (A=%d, B=%d)\n",
                        (mask & 1) != 0, (mask & 2) != 0);
        }
        else
        {
          Serial.println("           -> PUCKLOCK: Unlock now (A+B)");
        }
      }
      else
      {
        Serial.printf("           -> PUCKLOCK: UNKNOWN action=0x%02X\n", act);
      }
    }

    // ---------------------------------------------------------
    // 3.7) Minigame audio: Tetris
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_MINIGAME_AUDIO &&
        msg.data_length_code >= 2 &&
        msg.data[0] == CAN_CMD_MINIGAME_AUDIO)
    {
      handled = true;
      logFrame(msg);

      if (msg.data[1] == CAN_AUDIO_PLAY_TETRIS)
      {
        musicPlayTetris();
      }
      else if (msg.data[1] == CAN_AUDIO_STOP)
      {
        Serial.println("[MUSIC] TETRIS stop");
        musicStop();
      }
    }

    // ---------------------------------------------------------
    // 3.8) Lamp command
    // Ręczna lampa ma działać przez anims.cpp, nie przez lokalne
    // zmienne w main.cpp.
    // ---------------------------------------------------------
    if (!msg.extd && !msg.rtr &&
        msg.identifier == CAN_ID_LAMP_CMD &&
        msg.data_length_code >= 4 &&
        msg.data[0] == CAN_CMD_LAMP_SCENE)
    {
      handled = true;
      logFrame(msg);

      uint8_t mode     = msg.data[1];
      uint8_t value    = msg.data[2];
      uint8_t duration = msg.data[3];

      // porządkowo: jeśli akurat był aktywny debug strobe,
      // to go zdejmujemy
      g_strobeActive = false;
      g_strobeLampOn = false;

      if (mode == LMODE_OFF)
      {
        animsLampOff();
        g_manualLampActive = false;
        g_manualLampUntilMs = 0;
        Serial.println("[LAMP] OFF");
      }
      else
      {
        animsLampSet(mode, value, duration);

        g_manualLampActive = true;

        uint32_t durMs = lampDurationToMs(duration);
        if (durMs > 0)
          g_manualLampUntilMs = millis() + durMs;
        else
          g_manualLampUntilMs = 0; // 0 = bez auto-timeoutu po stronie main

        Serial.printf("[LAMP] mode=%u value=%u duration=%u\n",
                      mode, value, duration);
      }
    }

    // ---------------------------------------------------------
    // 3.9) Inne ramki – tylko log
    // ---------------------------------------------------------
    if (!handled)
    {
      logFrame(msg);
    }
  }

  // =========================================================
  // 4) STATE MACHINE
  // =========================================================
  switch (g_state)
  {
    case GState::IDLE:
    {
      // Nic nie robimy. Czekamy na START.
    }
    break;

    case GState::PREPARE:
    {
      // Daj settings-unit krótkie okno na dosłanie najnowszych ustawień
      // tuż po START.
      settingsSyncWindow(250);

      blowerSet(true);
      lightsApplyFromFlags();
      puckLockAutoReset();
      puckLockUnlockAll();

      Serial.printf("[ROUND] SETTINGS USED -> goals=%u (idx=%u), time=%lus (idx=%u)\n",
                    mapGoalsIdxToCount(g_set.goalsIdx), g_set.goalsIdx,
                    (unsigned long)mapTimeIdxToSeconds(g_set.gameTimeIdx), g_set.gameTimeIdx);

      startCountdown();
      countdownBegin();

      g_state = GState::COUNTDOWN;
    }
    break;

    case GState::COUNTDOWN:
    {
      if (countdownTick())
      {
        blowerSet(true);

        if (g_musicOnlyMode)
        {
          g_state = GState::ROUND_MUSIC;
          startMusicRound();
        }
        else
        {
          g_roundsWonA = g_roundsWonB = 0;
          poolResetTheme();

          g_state = GState::ROUND_NORMAL;
          startNormalRound();
        }
      }
    }
    break;

    case GState::ROUND_NORMAL:
    {
      // krótki guard po starcie rundy
      if ((int32_t)(millis() - g_roundGuardUntilMs) < 0)
        break;

      int8_t goal = pollGoal();

      if (goal == 0)
      {
        g_round.scoreA++;
        sendScoreUpdate(g_round.scoreA, g_round.scoreB);
        sendGoalAnim(0, 1);
        sfxPlayIndex(random(1, 31));
        Serial.printf("[GOAL] pollGoal()=%d  (ms=%lu)\n", (int)goal, (unsigned long)millis());
if (flagOn(g_set.flags, F_ANIM))
{
  animsOnGoal((uint8_t)goal, 800);
  tableLightsGoalFx((uint8_t)goal);
}
      }
      else if (goal == 1)
      {
        g_round.scoreB++;
        sendScoreUpdate(g_round.scoreA, g_round.scoreB);
        sendGoalAnim(1, 1);
        sfxPlayIndex(random(1, 31));
        Serial.printf("[GOAL] pollGoal()=%d  (ms=%lu)\n", (int)goal, (unsigned long)millis());
if (flagOn(g_set.flags, F_ANIM))
{
  animsOnGoal((uint8_t)goal, 800);
  tableLightsGoalFx((uint8_t)goal);
}
      }

      updatePuckLockDuringRound();

      bool byGoals = (g_round.goalLimit &&
                      (g_round.scoreA >= g_round.goalLimit ||
                       g_round.scoreB >= g_round.goalLimit));

      bool byTime = (g_round.timeLimited &&
                     (millis() - g_round.tStart >= g_round.tLimitMs));

if (byGoals || byTime)
{
  uint32_t now = millis();

  endRound();
  puckLockCmd(true, 0x03);

  // Jeśli runda 1 skończyła się golem, dajemy wybrzmieć SFX gola
  // i dopiero po 2 sekundach odpalamy dźwięk międzyrundowy.
if (g_roundNo == 1 || g_roundNo == 2)
{
  g_pendingBetweenRoundSfx = true;

  // Po golu dajemy wybrzmieć SFX gola.
  // Po czasie nie ma SFX gola, więc dźwięk międzyrundowy może wejść od razu.
  g_betweenRoundSfxAtMs = byGoals ? (now + 2000) : now;
}

  // Wygląd przerwy jak po grze:
  // po golu czekamy 2 sekundy, żeby efekt gola zdążył polecieć,
  // po końcu z czasu można wejść w ten stan od razu.
  if (byGoals)
  {
    g_breakPostGameLookAtMs = now + 800;
  }
  else
  {
    g_breakPostGameLookAtMs = now;
  }

  // Jeśli koniec był z czasu, nie ma efektu gola, więc można zgasić od razu.
  // Jeśli koniec był golem, nie gasimy ANIM_GOAL ręcznie — niech doleci normalnie.
  if (byTime)
  {
    animsSetMode(ANIM_OFF);
  }

  g_state = GState::ROUND_BREAK;
  g_breakT0 = now;
}
    }
    break;

case GState::ROUND_BREAK:
{
  if (g_pendingBetweenRoundSfx &&
      (int32_t)(millis() - g_betweenRoundSfxAtMs) >= 0)
  {
    g_pendingBetweenRoundSfx = false;
    sfxPlayFolderFile(1, 1);   // /01/001.mp3
  }

  if (millis() - g_breakT0 >= BREAK_MS)
  {
    if (g_roundNo < 2)
    {
      g_state = GState::ROUND_NORMAL;
      startNormalRound();
    }
    else if (g_roundNo == 2)
    {
      g_state = GState::ROUND_MUSIC;
      startMusicRound();
    }
    else
    {
      enterGameOver();
    }
  }
}
break;

    case GState::ROUND_MUSIC:
    {
      if ((int32_t)(millis() - g_roundGuardUntilMs) < 0)
        break;

      int8_t goal = pollGoal();

      if (goal == 0)
      {
        g_round.scoreA++;
        sendScoreUpdate(g_round.scoreA, g_round.scoreB);
        sendGoalAnim(0, 2);
        sfxPlayIndex(random(31, 51));
if (flagOn(g_set.flags, F_ANIM))
{
  animsOnGoal(0, 800);
  tableLightsGoalFx(0);
}
      }
      else if (goal == 1)
      {
        g_round.scoreB++;
        sendScoreUpdate(g_round.scoreA, g_round.scoreB);
        sendGoalAnim(1, 2);
        sfxPlayIndex(random(31, 51));
if (flagOn(g_set.flags, F_ANIM))
{
  animsOnGoal(1, 800);
  tableLightsGoalFx(1);
}
      }

      if (millis() - g_round.tStart >= g_round.tLimitMs)
      {
        endRound();
        sendGameEvent(3, g_roundsWonA, g_roundsWonB);
        enterGameOver();
      }
    }
    break;

    case GState::GAME_OVER:
    {
      if ((int32_t)(millis() - g_gameOverGuardUntilMs) < 0)
        break;

      if (blowerOn && (millis() - g_gameOverT0) >= GAMEOVER_BLOWER_TIMEOUT_MS)
      {
        blowerSet(false);
        Serial.println("[BLOWER] GameOver timeout 3 min -> OFF");
        g_state = GState::WAIT_PUCK_CLEAR;
        break;
      }

      int8_t goal = pollGoalForGameOver();

      if (goal >= 0)
      {
        blowerSet(false);

        applyPostGameLights();
        g_state = GState::WAIT_PUCK_CLEAR;
      }
    }
    break;

    case GState::WAIT_PUCK_CLEAR:
    {
      // Stan bezpieczny po wyłączeniu dmuchawy.
      // Czekamy na kolejny START.
    }
    break;
  }

  // =========================================================
  // 5) TICKI KOŃCOWE
  // =========================================================
  puckTick();
  remainLogTick();
  fanAutoUpdate();







  // =========================================================
  // 6) CAN AUTO-RECOVERY
  // =========================================================
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) == ESP_OK && alerts)
  {
    if (alerts & TWAI_ALERT_BUS_OFF)
    {
      Serial.println("[CAN] BUS_OFF -> recovery");
      twai_initiate_recovery();
    }

    if (alerts & TWAI_ALERT_BUS_RECOVERED)
    {
      Serial.println("[CAN] BUS_RECOVERED -> start()");
      twai_start();
    }
  }

  // =========================================================
  // 7) RENDER / ŚWIATŁA / UV
  // PRIORYTETY:
  //   1. ręczna lampa
  //   2. debug strobe
  //   3. BIN + UV pattern
  //   4. game over ambient
  //   5. idle / wait state
  //   6. normalne animsTick()
  // =========================================================
  uint32_t now = millis();

  // ---------------------------------------------------------
  // 7.1) RĘCZNA LAMPA – absolutny priorytet nad wszystkim
  // ---------------------------------------------------------
  // Nie zatrzymujemy gry ani timingu BIN.
  // Po prostu zasłaniamy wszystko innym renderem.
  if (animsLampActive())
  {
    animsTick(now);

    // przy ręcznej lampie wyłączamy UV / ambiente UV
    ledcWrite(UV_PWM_CH, 0);
    return;
  }

  // ---------------------------------------------------------
  // 7.2) DEBUG STROBE
  // ---------------------------------------------------------
  if (g_strobeActive)
  {
    if (now - g_strobeLastToggleMs >= g_strobeIntervalMs)
    {
      g_strobeLastToggleMs = now;
      g_strobeLampOn = !g_strobeLampOn;
    }

    if (g_strobeLampOn)
      matrixFill(255, 255, 255);
    else
      matrixFill(0, 0, 0);

    matrixShow();
    return;
  }

  // ---------------------------------------------------------
  // 7.3) BIN playback + UV pattern
  // ---------------------------------------------------------
  // BIN ma timing po czasie absolutnym, więc jeśli ręczna lampa
  // przez chwilę go zasłaniała, to po powrocie wskoczy w poprawne
  // miejsce utworu/animacji.
if (binPlaying)
{
  // Ambiente może dalej lecieć z BIN-a.
  tableLightsTick(now);

  // Jeśli trwa efekt gola, to MUSIMY tykać animsTick(),
  // bo inaczej ANIM_GOAL nigdy nie wróci do ANIM_ROUND.
  if (animsGetMode() == ANIM_GOAL)
  {
    animsTick(now);
    uvApply();
    return;
  }

  // Normalnie, gdy jesteśmy w ANIM_ROUND, rysujemy BIN głównej matrycy.
  binUpdate();

  UvLevel uvLevel = animsGetUvLevel(
      flagOn(g_set.flags, F_UV),
      true,
      (uint8_t)g_theme,
      now - g_round.tStart);

  uvSetLevel(uvDutyFromAnimLevel(uvLevel));

  return;
}

  // ---------------------------------------------------------
  // 7.4) Post-game ambient
  // ---------------------------------------------------------
if (g_postGameActive &&
    (g_state == GState::GAME_OVER || g_state == GState::WAIT_PUCK_CLEAR))
{
  applyPostGameLights();
  return;
}

// ---------------------------------------------------------
// 7.5) Round-break look (jak po grze)
// ---------------------------------------------------------
if (g_state == GState::ROUND_BREAK)
{
  if ((int32_t)(now - g_breakPostGameLookAtMs) >= 0)
  {
    // Nie pokazujemy już kolorów rundy / BIN-ów podczas przerwy
    tableLightsSetRoundEnabled(false);
    tableLightsMusicStop();

    applyPostGameLights();
    return;
  }
}

if (g_state == GState::IDLE)
{
  tableLightsSetRoundEnabled(false);
  tableLightsMusicStop();

  matrixFill(0, 0, 0);
  matrixShow();
  ledcWrite(UV_PWM_CH, 0);
  return;
}

// ---------------------------------------------------------
// 7.6) Normalne animacje matrycy + ambiente
// ---------------------------------------------------------
tableLightsTick(now);
animsTick(now);

uvApply();
}

static bool binReadExact(uint8_t *dst, size_t n)
{
  size_t got = 0;
  while (got < n)
  {
    int r = binFile.read(dst + got, n - got);
    if (r <= 0)
      return false; // EOF / błąd
    got += (size_t)r;
  }
  return true;
}

void binUpdate()
{
  if (!binPlaying)
    return;
  if (animsGetMode() != ANIM_ROUND)
    return;
  if (!binFile || !binPath || binFrameCount == 0)
    return;

  uint32_t now = millis();
  uint32_t elapsedMs = now - g_musicStartMs;

  // numer klatki wynikający z czasu absolutnego od startu
  uint32_t wantFrame = (elapsedMs * BIN_FPS) / 1000UL;

  // zapętlenie po liczbie klatek w pliku
  uint32_t frameIndex = wantFrame % binFrameCount;

  // nie renderuj tej samej klatki 2x, jeśli loop wpadnie szybciej niż 50 ms
  if (frameIndex == binLastRenderedFrame)
    return;

  uint32_t framePos = frameIndex * BIN_FRAME_SIZE;

  if (!binFile.seek(framePos))
  {
    Serial.printf("[SD] seek FAIL frame=%lu pos=%lu file=%s -> recover\n",
                  (unsigned long)frameIndex,
                  (unsigned long)framePos,
                  binPath);

    binFile.close();

    if (!sd_recover())
    {
      Serial.println("[SD] recover FAIL");
      sd_ok = false;
      return;
    }

    binFile = SD.open(binPath, FILE_READ);
    if (!binFile)
    {
      Serial.printf("[SD] reopen FAIL after recover: %s\n", binPath);
      sd_ok = false;
      return;
    }

    if (!binFile.seek(framePos))
    {
      Serial.printf("[SD] seek FAIL again frame=%lu pos=%lu\n",
                    (unsigned long)frameIndex,
                    (unsigned long)framePos);
      return;
    }
  }

  static uint8_t buf[BIN_FRAME_SIZE];

  if (!binReadExact(buf, BIN_FRAME_SIZE))
  {
    Serial.printf("[SD] read FAIL frame=%lu pos=%lu file=%s\n",
                  (unsigned long)frameIndex,
                  (unsigned long)framePos,
                  binPath);
    return;
  }

  for (uint16_t i = 0; i < BIN_PIXELS; i++)
  {
    uint8_t r = buf[i * 3 + 0];
    uint8_t g = buf[i * 3 + 1];
    uint8_t b = buf[i * 3 + 2];
    matrixSetIndex(i, r, g, b);
  }

  matrixShow();
  binLastRenderedFrame = frameIndex;
}

// ========================= Game start entry points =========================
static void gameStartFull(uint8_t index)
{
  // FULL z CAN/UI przychodzi jako normalny indeks 0..5
  uint8_t th;
  if (index < TH_COUNT)
    th = index;
  else
    th = (uint8_t)random(0, TH_COUNT);

  g_theme = (ThemeId)th;
  g_musicOnlyMode = false;

  g_fullIndex = index;
  g_roundNo = 0;
  g_roundsWonA = g_roundsWonB = 0;
  g_round.scoreA = g_round.scoreB = 0;

  poolResetTheme();
  g_bgPlaying = false;

  Serial.printf("[GAME] FULL: theme=%s (folder %02u)\n",
                THEMES[g_theme].name,
                THEMES[g_theme].folder);

  g_state = GState::PREPARE;
}



static void gameStartMusicOnlyRandom()
{
  g_musicOnlyMode = true;

  g_theme = (ThemeId)random(0, TH_COUNT);
  g_fullIndex = 0;
  g_roundNo = 0;
  g_roundsWonA = g_roundsWonB = 0;
  g_round.scoreA = g_round.scoreB = 0;

  poolResetTheme(); // = g_usedMask = 0
  g_bgPlaying = false;

  Serial.printf("[GAME] MUSIC-ONLY: theme=%s (folder %02u)", THEMES[g_theme].name, THEMES[g_theme].folder);
  g_state = GState::PREPARE;
}
