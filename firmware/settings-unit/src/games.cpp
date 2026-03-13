#include <Arduino.h>
#include "games.h"
#include "main.h"


bool snakeActive = false;
bool snakeComboReleased = true;

bool tetrisActive = false;
bool tetrisComboReleased = true;


void runSnake() {
  // ===== konfiguracja planszy =====
  const int GRID_W  = 20;
  const int GRID_H  = 20;
  const int CELL    = 6;
  const int BOARD_X = (tft.width() - GRID_W * CELL) / 2;
  const int BOARD_Y = 22;
  const int BOARD_W = GRID_W * CELL;
  const int BOARD_H = GRID_H * CELL;

  const uint32_t EXIT_HOLD_THRESHOLD = 500; // ms

  // ===== stan gry =====
  static bool inited          = false;
  static bool gameOver        = false;
  static bool needFullRedraw  = false;
  static bool prefsInited     = false;

  static int snake[GRID_W * GRID_H];
  static int snakeLen = 3;
  static int appleIndex = 0;
  static int direction = 1;
  static int queuedDir1 = 1;
  static int queuedDir2 = 0;
  static int score = 0;
  static int topScore = 0;
  static bool newRecordShown = false;
  static bool recordBrokenThisRun = false;
  static bool pendingTopCommit = false;
  static int pendingTopScore = 0;

  static uint32_t intervalTime     = 0; // bierze z startgame()
  static uint32_t lastMoveMs       = 0;
  static uint32_t deathGraceStart  = 0;
  static const uint32_t DEATH_GRACE_MS = 100;

  static bool deathBlinkActive = false;
  static bool deathBlinkVisible = true;
  static uint32_t deathBlinkLastMs = 0;
  static int deathBlinkTogglesLeft = 0;

  static uint32_t appleSpawnMs = 0;
  static const int APPLE_START_POINTS = 100;
  static const int APPLE_MIN_POINTS   = 1;
  static const uint32_t APPLE_DECAY_MS = 100;

  static bool bigAppleActive = false;
  static int bigAppleIndex = -1;
  static uint32_t bigAppleSpawnMs = 0;
  static uint32_t nextBigAppleMs = 0;

  static const uint32_t BIG_APPLE_LIFETIME_MS = 5000;
  static const int BIG_APPLE_BONUS_POINTS = 300;
  static const uint32_t BIG_APPLE_SPAWN_MIN_MS = 15000;
  static const uint32_t BIG_APPLE_SPAWN_MAX_MS = 20000;



  // ===== wyjście =====
  static uint32_t exitHoldMs = 0;
  static int lastBarFillW    = 0;

  // ===== helpers =====
  auto isSnakeCell = [&](int idx) -> bool {
    for (int i = 0; i < snakeLen; i++) {
      if (snake[i] == idx) return true;
    }
    return false;
  };

  auto drawCell = [&](int idx, uint16_t color) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;
    tft.fillRect(px, py, CELL, CELL, color);
  };

  auto getApplePoints = [&]() -> int {
    uint32_t elapsed = millis() - appleSpawnMs;
    int decay = elapsed / APPLE_DECAY_MS;
    int pts = APPLE_START_POINTS - decay;
    if (pts < APPLE_MIN_POINTS) pts = APPLE_MIN_POINTS;
    return pts;
  };

  auto scheduleNextBigApple = [&]() {
    nextBigAppleMs = millis() + random(BIG_APPLE_SPAWN_MIN_MS, BIG_APPLE_SPAWN_MAX_MS + 1);
  };

  auto drawSnakeSegment = [&](int idx, int dir, uint16_t color) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;

    if (dir == 1 || dir == -1) {
      tft.fillRect(px, py, 6, 5, color);
    } else {
      tft.fillRect(px + 1, py, 5, 6, color);
    }
  };

  auto cleanupTurnArtifact = [&](int idx, int oldDir, int newDir) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;

    if ((oldDir == -GRID_W && newDir == -1) ||
        (oldDir == 1       && newDir == GRID_W) ||
        (oldDir == -GRID_W && newDir == 1) ||
        (oldDir == 1       && newDir == -GRID_W)) {
      return;
    }

    tft.fillRect(px, py,     1, 6, ST77XX_BLACK);
    tft.fillRect(px, py + 5, 6, 1, ST77XX_BLACK);
  };

  auto cleanupNewSegmentAfterTurn = [&](int idx, int oldDir, int newDir) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;

    if (oldDir == -1 && newDir == -GRID_W) {
      tft.fillRect(px + 1, py + 5, 5, 1, ST77XX_BLACK);
    }

    if (oldDir == GRID_W && newDir == 1) {
      tft.fillRect(px, py, 1, 5, ST77XX_BLACK);
    }

    if (oldDir == -GRID_W && newDir == 1) {
      tft.fillRect(px, py, 1, 5, ST77XX_BLACK);
    }

    if (oldDir == 1 && newDir == -GRID_W) {
      tft.fillRect(px + 1, py + 5, 5, 1, ST77XX_BLACK);
    }
  };

  auto drawApple = [&](int idx) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;

    tft.fillRect(px, py, CELL, CELL, ST77XX_BLACK);

    // wzór:
    // xxxxxx
    // xxooxx
    // xoxxox
    // xoxxox
    // xxooxx
    // xxxxxx

    tft.drawPixel(px + 2, py + 1, ST77XX_RED);
    tft.drawPixel(px + 3, py + 1, ST77XX_RED);

    tft.drawPixel(px + 1, py + 2, ST77XX_RED);
    tft.drawPixel(px + 4, py + 2, ST77XX_RED);

    tft.drawPixel(px + 1, py + 3, ST77XX_RED);
    tft.drawPixel(px + 4, py + 3, ST77XX_RED);

    tft.drawPixel(px + 2, py + 4, ST77XX_RED);
    tft.drawPixel(px + 3, py + 4, ST77XX_RED);
  };

  auto drawBigApple = [&](int idx) {
    int gx = idx % GRID_W;
    int gy = idx / GRID_W;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + gy * CELL;

    tft.fillRect(px, py, CELL, CELL, ST77XX_BLACK);

    // wzór:
    // xxxoxx
    // xxooox
    // xooooo
    // xooooo
    // xxooox
    // xxxxxx

    tft.drawPixel(px + 3, py + 0, SNAKE_ORANGE);

    tft.drawPixel(px + 2, py + 1, SNAKE_ORANGE);
    tft.drawPixel(px + 3, py + 1, SNAKE_ORANGE);
    tft.drawPixel(px + 4, py + 1, SNAKE_ORANGE);

    tft.drawPixel(px + 1, py + 2, SNAKE_ORANGE);
    tft.drawPixel(px + 2, py + 2, SNAKE_ORANGE);
    tft.drawPixel(px + 3, py + 2, SNAKE_ORANGE);
    tft.drawPixel(px + 4, py + 2, SNAKE_ORANGE);
    tft.drawPixel(px + 5, py + 2, SNAKE_ORANGE);

    tft.drawPixel(px + 1, py + 3, SNAKE_ORANGE);
    tft.drawPixel(px + 2, py + 3, SNAKE_ORANGE);
    tft.drawPixel(px + 3, py + 3, SNAKE_ORANGE);
    tft.drawPixel(px + 4, py + 3, SNAKE_ORANGE);
    tft.drawPixel(px + 5, py + 3, SNAKE_ORANGE);

    tft.drawPixel(px + 2, py + 4, SNAKE_ORANGE);
    tft.drawPixel(px + 3, py + 4, SNAKE_ORANGE);
    tft.drawPixel(px + 4, py + 4, SNAKE_ORANGE);
  };

  auto drawHeader = [&]() {
  tft.fillRect(0, 8, tft.width(), 12, ST77XX_BLACK);
  tft.setTextWrap(false);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(2, 10);
  tft.print("");
  tft.print(score);

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(65, 10);
  tft.print("Top:");
  tft.print(topScore);
};

  auto drawNewRecordOverlay = [&]() {
    const int boxW = 104;
    const int boxH = 42;
    const int boxX = (tft.width() - boxW) / 2;
    const int boxY = BOARD_Y + (BOARD_H - boxH) / 2;

tft.fillRect(boxX, boxY, boxW, boxH, ST77XX_BLACK);
tft.drawRect(boxX, boxY, boxW, boxH, ST77XX_YELLOW);

tft.setTextWrap(false);

int16_t x1, y1;
uint16_t w, h;

// ===== CONGRATS =====
tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
tft.getTextBounds("CONGRATS!", 0, 0, &x1, &y1, &w, &h);
tft.setCursor(boxX + (boxW - w) / 2, boxY + 10);
tft.print("CONGRATS!");

// ===== NEW RECORD =====
tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
tft.getTextBounds("NEW RECORD", 0, 0, &x1, &y1, &w, &h);
tft.setCursor(boxX + (boxW - w) / 2, boxY + 24);
tft.print("NEW RECORD");
  };

  auto drawGameOverOverlay = [&]() {
  const int boxW = 104;
  const int boxH = 42;
  const int boxX = (tft.width() - boxW) / 2;
  const int boxY = BOARD_Y + (BOARD_H - boxH) / 2;

tft.fillRect(boxX, boxY, boxW, boxH, ST77XX_BLACK);
tft.drawRect(boxX, boxY, boxW, boxH, ST77XX_RED);

tft.setTextWrap(false);

int16_t x1, y1;
uint16_t w, h;

// ===== GAME OVER =====
tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
tft.getTextBounds("GAME OVER", 0, 0, &x1, &y1, &w, &h);
tft.setCursor(boxX + (boxW - w) / 2, boxY + 10);
tft.print("GAME OVER");

// ===== PRESS ANY KEY =====
tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
tft.getTextBounds("PRESS ANY KEY", 0, 0, &x1, &y1, &w, &h);
tft.setCursor(boxX + (boxW - w) / 2, boxY + 24);
tft.print("PRESS ANY KEY");
};

  auto drawBoard = [&]() {
    tft.fillScreen(ST77XX_BLACK);

    if (lastBarFillW > 0) {
      tft.fillRect(0, 0, lastBarFillW, 6, ST77XX_RED);
    }

    drawHeader();
    tft.drawRect(BOARD_X - 1, BOARD_Y - 2, BOARD_W + 3, BOARD_H + 3, ST77XX_WHITE);

    drawApple(appleIndex);

    if (bigAppleActive) {
      drawBigApple(bigAppleIndex);
    }

    for (int i = 0; i < snakeLen; i++) {
      int segDir = (i == 0) ? direction : (snake[i - 1] - snake[i]);
      drawSnakeSegment(snake[i], segDir, ST77XX_GREEN);
    }
  };

  auto drawWholeSnake = [&](uint16_t color) {
    for (int i = 0; i < snakeLen; i++) {
      int segDir = (i == 0) ? direction : (snake[i - 1] - snake[i]);
      drawSnakeSegment(snake[i], segDir, color);
    }
  };

  auto drawCurrentSnakeExact = [&](uint16_t color) {
    for (int i = 0; i < snakeLen; i++) {
      int gx = snake[i] % GRID_W;
      int gy = snake[i] / GRID_W;
      int px = BOARD_X + gx * CELL;
      int py = BOARD_Y + gy * CELL;

      int dirToPrev = (i == 0) ? direction : (snake[i - 1] - snake[i]);
      int dirToNext = 0;

      if (i < snakeLen - 1) {
        dirToNext = snake[i] - snake[i + 1];
      }

      if (dirToPrev == 1 || dirToPrev == -1) {
        tft.fillRect(px, py, 6, 5, color);
      } else {
        tft.fillRect(px + 1, py, 5, 6, color);
      }

      if (i > 0 && i < snakeLen - 1 && dirToPrev != dirToNext) {
        if (dirToPrev == -GRID_W) {
          tft.fillRect(px + 1, py + 5, 5, 1, ST77XX_BLACK);
        }

        if (dirToPrev == 1) {
          tft.fillRect(px, py, 1, 5, ST77XX_BLACK);
        }
      }
    }
  };

  auto clearCurrentSnakeExact = [&]() {
    for (int i = 0; i < snakeLen; i++) {
      int gx = snake[i] % GRID_W;
      int gy = snake[i] / GRID_W;
      int px = BOARD_X + gx * CELL;
      int py = BOARD_Y + gy * CELL;

      int dirToPrev = (i == 0) ? direction : (snake[i - 1] - snake[i]);
      int dirToNext = 0;

      if (i < snakeLen - 1) {
        dirToNext = snake[i] - snake[i + 1];
      }

      if (dirToPrev == 1 || dirToPrev == -1) {
        tft.fillRect(px, py, 6, 5, ST77XX_BLACK);
      } else {
        tft.fillRect(px + 1, py, 5, 6, ST77XX_BLACK);
      }

      if (i > 0 && i < snakeLen - 1 && dirToPrev != dirToNext) {
        if (dirToPrev == -GRID_W) {
          tft.fillRect(px + 1, py + 5, 5, 1, ST77XX_BLACK);
        }

        if (dirToPrev == 1) {
          tft.fillRect(px, py, 1, 5, ST77XX_BLACK);
        }
      }
    }
  };

  auto placeApple = [&]() {
    do {
      appleIndex = random(GRID_W * GRID_H);
    } while (isSnakeCell(appleIndex) || (bigAppleActive && appleIndex == bigAppleIndex));
    appleSpawnMs = millis();
  };

  auto placeBigApple = [&]() {
    if (bigAppleActive) return;

    int idx;
    do {
      idx = random(GRID_W * GRID_H);
    } while (isSnakeCell(idx) || idx == appleIndex);

    bigAppleIndex = idx;
    bigAppleActive = true;
    bigAppleSpawnMs = millis();
  };

  auto startGame = [&]() {
    snakeLen  = 3;
    snake[0]  = 2;
    snake[1]  = 1;
    snake[2]  = 0;

    deathBlinkActive = false;
    deathBlinkVisible = true;
    deathBlinkLastMs = 0;
    deathBlinkTogglesLeft = 0;

    bigAppleActive = false;
    bigAppleIndex = -1;
    bigAppleSpawnMs = 0;
    scheduleNextBigApple();

    direction = 1;
    queuedDir1 = 1;
    queuedDir2 = 0;
    score = 0;
    newRecordShown = false;
    recordBrokenThisRun = false;
    intervalTime = 150;
    lastMoveMs = millis();
    deathGraceStart = 0;
    gameOver = false;

    exitHoldMs = 0;
    lastBarFillW = 0;
    tft.fillRect(0, 0, tft.width(), 8, ST77XX_BLACK);

    placeApple();
    drawBoard();
  };

  // ===== aktywność / wybudzanie =====
  

  bool anyBtnRaw =
    digitalRead(BTN_UP) == LOW ||
    digitalRead(BTN_DOWN) == LOW ||
    digitalRead(BTN_SELECT) == LOW ||
    digitalRead(BTN_BACK) == LOW;

  if (anyBtnRaw) {
    lastInputTime = millis();

    if (screenDimmed) {
      digitalWrite(LCD_PWR, HIGH);
      delay(10);
      screenDimmed = false;
      delay(120);
      needFullRedraw = true;
    }
  }

  // ===== init =====
  if (!inited) {
    inited = true;
    needFullRedraw = false;

    if (!prefsInited) {
      gamePrefs.begin("snake", false);
      topScore = gamePrefs.getInt("snaketopScore", 0);
      prefsInited = true;
    }

    startGame();
  }

  // ===== wyjście z gry: SELECT + BACK =====
  bool selRaw  = (digitalRead(BTN_SELECT) == LOW);
  bool backRaw = (digitalRead(BTN_BACK)   == LOW);

  if (selRaw && backRaw) {
    if (exitHoldMs == 0) {
      exitHoldMs = millis();
      lastBarFillW = 0;
      tft.fillRect(0, 0, tft.width(), 6, ST77XX_BLACK);
    }

    uint32_t held = millis() - exitHoldMs;
    if (held > EXIT_HOLD_THRESHOLD) held = EXIT_HOLD_THRESHOLD;

    int fillW = (int)((held * (uint32_t)tft.width()) / EXIT_HOLD_THRESHOLD);

    if (fillW > lastBarFillW) {
      tft.fillRect(lastBarFillW, 0, fillW - lastBarFillW, 6, ST77XX_RED);
      lastBarFillW = fillW;
    }

    if (held >= EXIT_HOLD_THRESHOLD) {
      snakeActive = false;
      snakeComboReleased = false;
      inited = false;
      gameOver = false;
      comboReady = false;
      exitHoldMs = 0;
      lastBarFillW = 0;

      bSelect.armed = false;
      bBack.armed = false;

      redraw();
      return;
    }

    return;
  } else {
    if (exitHoldMs != 0) {
      exitHoldMs = 0;
      lastBarFillW = 0;
      tft.fillRect(0, 0, tft.width(), 6, ST77XX_BLACK);
      drawHeader();
    }
  }

  // ===== po wybudzeniu odtwórz ekran gry =====
if (needFullRedraw) {
  needFullRedraw = false;
  drawBoard();

  if (gameOver) {
    if (recordBrokenThisRun && newRecordShown) {
      drawNewRecordOverlay();
    } else {
      drawGameOverOverlay();
    }
  }
}

  // ===== duze jablko bonusowe =====
  if (!gameOver && !deathBlinkActive) {
    if (!bigAppleActive) {
      if (millis() >= nextBigAppleMs) {
        placeBigApple();
        drawBigApple(bigAppleIndex);
      }
    } else {
      if (millis() - bigAppleSpawnMs >= BIG_APPLE_LIFETIME_MS) {
        drawCell(bigAppleIndex, ST77XX_BLACK);
        bigAppleActive = false;
        bigAppleIndex = -1;
        bigAppleSpawnMs = 0;
        scheduleNextBigApple();
      }
    }
  }

  if (deathBlinkActive) {
    const uint32_t BLINK_MS = 100;

    if (millis() - deathBlinkLastMs >= BLINK_MS) {
      deathBlinkLastMs = millis();
      deathBlinkVisible = !deathBlinkVisible;
      deathBlinkTogglesLeft--;

      if (deathBlinkVisible) {
        drawCurrentSnakeExact(ST77XX_GREEN);
      } else {
        clearCurrentSnakeExact();
      }

      if (deathBlinkTogglesLeft <= 0) {
        deathBlinkActive = false;
        deathBlinkVisible = true;
        drawCurrentSnakeExact(ST77XX_GREEN);

        if (bigAppleActive) {
          drawCell(bigAppleIndex, ST77XX_BLACK);
          bigAppleActive = false;
          bigAppleIndex = -1;
          bigAppleSpawnMs = 0;
        }

         gameOver = true;

        if (recordBrokenThisRun && score > topScore) {
          topScore = score;
          gamePrefs.putInt("snaketopScore", topScore);
        }

        if (recordBrokenThisRun && !newRecordShown) {
          newRecordShown = true;
          drawNewRecordOverlay();
        } else {
          drawGameOverOverlay();
        }
      }
    }

    return;
  }

  // ===== restart po GAME OVER =====
  if (gameOver) {
    if (pressedNow(bUp) || pressedNow(bDown) || pressedNow(bSelect) || pressedNow(bBack)) {
      startGame();
    }
    return;
  }

  // ===== sterowanie =====
  auto isOpposite = [&](int a, int b) -> bool {
    return (a == 1       && b == -1) ||
           (a == -1      && b == 1) ||
           (a == GRID_W  && b == -GRID_W) ||
           (a == -GRID_W && b == GRID_W);
  };

  auto enqueueDirection = [&](int newDir) {
    if (newDir == 0) return;

    if (queuedDir1 == 0) {
      if (!isOpposite(direction, newDir) && direction != newDir) {
        queuedDir1 = newDir;
      }
      return;
    }

    if (queuedDir2 == 0) {
      if (!isOpposite(queuedDir1, newDir) && queuedDir1 != newDir) {
        queuedDir2 = newDir;
      }
      return;
    }
  };

  if (pressedNow(bSelect)) enqueueDirection(1);
  if (pressedNow(bUp))     enqueueDirection(-GRID_W);
  if (pressedNow(bBack))   enqueueDirection(-1);
  if (pressedNow(bDown))   enqueueDirection(GRID_W);

  // ===== ruch co intervalTime ms =====
  uint32_t now = millis();
  if (now - lastMoveMs < intervalTime) return;
  lastMoveMs = now;

  int oldHead = snake[0];
  int oldDirection = direction;
  int head = snake[0];

  if (queuedDir1 != 0) {
    direction = queuedDir1;
    queuedDir1 = queuedDir2;
    queuedDir2 = 0;
  }

  // ===== kolizje =====
  bool hitBottom = (head + GRID_W >= GRID_W * GRID_H && direction == GRID_W);
  bool hitRight  = (head % GRID_W == GRID_W - 1 && direction == 1);
  bool hitLeft   = (head % GRID_W == 0 && direction == -1);
  bool hitTop    = (head - GRID_W < 0 && direction == -GRID_W);

  int newHead = head + direction;

  bool hitSelf = false;
  if (!hitBottom && !hitRight && !hitLeft && !hitTop) {
    for (int i = 0; i < snakeLen; i++) {
      if (snake[i] == newHead) {
        hitSelf = true;
        break;
      }
    }
  }

  bool willHit = (hitBottom || hitRight || hitLeft || hitTop || hitSelf);

  if (willHit) {
    if (deathGraceStart == 0) {
      deathGraceStart = millis();
      return;
    }

    if (millis() - deathGraceStart >= DEATH_GRACE_MS) {
      deathGraceStart = 0;

      deathBlinkActive = true;
      deathBlinkVisible = false;
      deathBlinkLastMs = millis();
      deathBlinkTogglesLeft = 6;

      drawWholeSnake(ST77XX_BLACK);
      return;
    }

    return;
  }

  deathGraceStart = 0;

  // ===== ruch snake =====
  int tail = snake[snakeLen - 1];

  for (int i = snakeLen - 1; i > 0; i--) {
    snake[i] = snake[i - 1];
  }
  snake[0] = newHead;

  bool ateApple = (snake[0] == appleIndex);
  bool ateBigApple = (bigAppleActive && snake[0] == bigAppleIndex);

  if (ateApple) {
    snake[snakeLen] = tail;
    snakeLen++;

    score += getApplePoints();
    if (score > topScore) {
      recordBrokenThisRun = true;
    }

    placeApple();

 
  } else {
    drawCell(tail, ST77XX_BLACK);
  }

if (ateBigApple) {

  // wydłuż węża tak samo jak przy zwykłym jabłku
  snake[snakeLen] = tail;
  snakeLen++;

  score += BIG_APPLE_BONUS_POINTS;
  if (score > topScore) {
    recordBrokenThisRun = true;
  }

  bigAppleActive = false;
  bigAppleIndex = -1;
  bigAppleSpawnMs = 0;
  scheduleNextBigApple();
}

  // ===== poprawka artefaktu po skręcie =====
  if (oldDirection != direction) {
    cleanupTurnArtifact(oldHead, oldDirection, direction);
  }

  // ===== rysowanie po ruchu =====
  drawSnakeSegment(snake[0], direction, ST77XX_GREEN);

  if (snakeLen > 1) {
    int segDir = snake[0] - snake[1];
    drawSnakeSegment(snake[1], segDir, ST77XX_GREEN);

    if (oldDirection != direction) {
      cleanupNewSegmentAfterTurn(snake[1], oldDirection, direction);
    }
  }

  if (ateApple) {
    drawApple(appleIndex);
  }

  if (ateApple || ateBigApple) {
    drawHeader();
  }
}



void runTetris() {
  
  const int GRID_W = 14;
  const int GRID_H = 20;
  const int CELL   = 7;

  const int BOARD_X = 4;
  const int BOARD_Y = 12;
  const int BOARD_W = GRID_W * CELL;
  const int BOARD_H = GRID_H * CELL;

  const uint32_t EXIT_HOLD_THRESHOLD = 500;
  const uint32_t FALL_INTERVAL_MS = 500;
  const uint32_t SOFT_DROP_MS = 60;

  struct PieceDef {
    uint8_t blocks[4][4][4]; // 4 rotacje, 4 bloki, x/y
    uint16_t color;
  };

  static bool inited = false;
  static bool gameOver = false;
  static bool needFullRedraw = false;
  static bool musicStarted = false;

  static uint8_t grid[GRID_H][GRID_W];
  static uint16_t gridColor[GRID_H][GRID_W];

  static int curPiece = 0;
  static int curRot = 0;
  static int curX = 3;
  static int curY = 14;

  static uint32_t lastFallMs = 0;
  static uint32_t exitHoldMs = 0;
  static int lastBarFillW = 0;
  static int score = 0;
  static int linesCleared = 0;

  static uint32_t lrHoldStartMs = 0;
  static uint32_t lrRepeatMs = 0;
  static int lrHeldDir = 0; // -1 = lewo, +1 = prawo, 0 = nic

  static int nextPiece = 0;

  static bool prefsInited = false;
  static int topScore = 0;
  static bool recordBrokenThisRun = false;

  static const PieceDef pieces[7] = {
    // I
{
  {
    {{2,0},{2,1},{2,2},{2,3}},
    {{0,1},{1,1},{2,1},{3,1}},
    {{2,0},{2,1},{2,2},{2,3}},
    {{0,1},{1,1},{2,1},{3,1}}
  },
      TETRIS_CYAN
    },
    // O
    {
      {
        {{1,1},{2,1},{1,2},{2,2}},
        {{1,1},{2,1},{1,2},{2,2}},
        {{1,1},{2,1},{1,2},{2,2}},
        {{1,1},{2,1},{1,2},{2,2}}
      },
      TETRIS_YELLOW
    },
    // T
    {
      {
        {{1,1},{0,2},{1,2},{2,2}},
        {{1,1},{1,2},{2,2},{1,3}},
        {{0,2},{1,2},{2,2},{1,3}},
        {{1,1},{0,2},{1,2},{1,3}}
      },
      TETRIS_PURPLE
    },
    // S
    {
      {
        {{1,1},{2,1},{0,2},{1,2}},
        {{1,1},{1,2},{2,2},{2,3}},
        {{1,2},{2,2},{0,3},{1,3}},
        {{0,1},{0,2},{1,2},{1,3}}
      },
      TETRIS_GREEN
    },
    // Z
    {
      {
        {{0,1},{1,1},{1,2},{2,2}},
        {{2,1},{1,2},{2,2},{1,3}},
        {{0,2},{1,2},{1,3},{2,3}},
        {{1,1},{0,2},{1,2},{0,3}}
      },
      TETRIS_RED
    },
    // J
    {
      {
        {{0,1},{0,2},{1,2},{2,2}},
        {{1,1},{2,1},{1,2},{1,3}},
        {{0,2},{1,2},{2,2},{2,3}},
        {{1,1},{1,2},{0,3},{1,3}}
      },
      TETRIS_BLUE
    },
    // L
    {
      {
        {{2,1},{0,2},{1,2},{2,2}},
        {{1,1},{1,2},{1,3},{2,3}},
        {{0,2},{1,2},{2,2},{0,3}},
        {{0,1},{1,1},{1,2},{1,3}}
      },
      TETRIS_ORANGE
    }
  };

  auto clearGrid = [&]() {
    for (int y = 0; y < GRID_H; y++) {
      for (int x = 0; x < GRID_W; x++) {
        grid[y][x] = 0;
        gridColor[y][x] = ST77XX_BLACK;
      }
    }
  };

  auto drawCell = [&](int gx, int gy, uint16_t color) {
    if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H) return;
    int px = BOARD_X + gx * CELL;
    int py = BOARD_Y + (GRID_H - 1 - gy) * CELL;
    tft.fillRect(px, py, CELL - 1, CELL - 1, color);
  };

  auto drawHeader = [&]() {
    tft.fillRect(0, 0, tft.width(), 10, ST77XX_BLACK);
    tft.setTextWrap(false);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(2, 1);
    tft.print("S:");
    tft.print(score);
    tft.setCursor(80, 1);
    tft.print("L:");
    tft.print(linesCleared);
  };

  auto clearPiece = [&](int piece, int rot, int px, int py) {
  for (int i = 0; i < 4; i++) {
    int x = pieces[piece].blocks[rot][i][0] + px;
    int y = pieces[piece].blocks[rot][i][1] + py;

    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) continue;

    if (grid[y][x]) {
      drawCell(x, y, gridColor[y][x]);   // jeśli pod spodem coś już jest w siatce
    } else {
      drawCell(x, y, ST77XX_BLACK);      // normalnie wyczyść
    }
  }
};


  auto drawPiece = [&](int piece, int rot, int px, int py, uint16_t color) {
    for (int i = 0; i < 4; i++) {
      int x = pieces[piece].blocks[rot][i][0] + px;
      int y = pieces[piece].blocks[rot][i][1] + py;
      if (y >= 0 && y < GRID_H) {
        drawCell(x, y, color);
      }
    }
  };

auto redrawBoardArea = [&]() {
  tft.fillRect(BOARD_X, BOARD_Y, BOARD_W, BOARD_H, ST77XX_BLACK);

  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      if (grid[y][x]) {
        drawCell(x, y, gridColor[y][x]);
      }
    }
  }

  if (!gameOver) {
    drawPiece(curPiece, curRot, curX, curY, pieces[curPiece].color);
  }
};

  auto drawOverlayCentered = [&](uint16_t borderColor, uint16_t titleColor, const char* line1, const char* line2) {
    const int boxW = 108;
    const int boxH = 42;
    const int boxX = (tft.width() - boxW) / 2;
    const int boxY = BOARD_Y + (BOARD_H - boxH) / 2;

    int16_t x1, y1;
    uint16_t w, h;

    tft.fillRect(boxX, boxY, boxW, boxH, ST77XX_BLACK);
    tft.drawRect(boxX, boxY, boxW, boxH, borderColor);
    tft.setTextWrap(false);

    tft.setTextColor(titleColor, ST77XX_BLACK);
    tft.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(boxX + (boxW - w) / 2, boxY + 10);
    tft.print(line1);

    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(boxX + (boxW - w) / 2, boxY + 24);
    tft.print(line2);
  };

  auto isValid = [&](int piece, int rot, int px, int py) -> bool {
    for (int i = 0; i < 4; i++) {
      int x = pieces[piece].blocks[rot][i][0] + px;
      int y = pieces[piece].blocks[rot][i][1] + py;

      if (x < 0 || x >= GRID_W) return false;
      if (y < 0) return false;
      if (y >= GRID_H) continue; // może wystawać u góry przy spawnie

      if (grid[y][x]) return false;
    }
    return true;
  };



  auto drawNextPiece = [&]() {

tft.fillRect(BOARD_X + BOARD_W + 2, BOARD_Y, tft.width() - (BOARD_X + BOARD_W + 2), 80, ST77XX_BLACK);

  int baseX = BOARD_X + BOARD_W + 4;
  int baseY = BOARD_Y + 20;

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(baseX - 2, BOARD_Y);
  

  for (int i = 0; i < 4; i++) {

    int x = pieces[nextPiece].blocks[0][i][0];
    int y = pieces[nextPiece].blocks[0][i][1];

    int px = baseX + x * CELL;
    int py = baseY + (3 - y) * CELL;

    tft.fillRect(px, py, CELL - 1, CELL - 1, pieces[nextPiece].color);
  }

  int topY = baseY + 4 * CELL + 12;

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(baseX - 2, topY);
  tft.print("TOP:");

  tft.fillRect(baseX - 2, topY + 10, 40, 12, ST77XX_BLACK);

tft.setCursor(baseX - 2, topY + 10);

tft.fillRect(baseX - 2, topY + 10, 40, 24, ST77XX_BLACK);

tft.setCursor(baseX - 2, topY + 10);

if (topScore < 10000) {

  tft.print(topScore);

} 
else if (topScore < 100000) {

  int k = topScore / 1000;
  int decimal = (topScore % 1000) / 100;

  tft.print(k);
  tft.print(",");
  tft.print(decimal);

  tft.setCursor(baseX - 2, topY + 20);
  tft.print("K");

} 
else {

  int k = topScore / 1000;

  tft.print(k);

  tft.setCursor(baseX - 2, topY + 20);
  tft.print("K");

}



};


  auto drawBoard = [&]() {
    tft.fillScreen(ST77XX_BLACK);

    if (lastBarFillW > 0) {
      tft.fillRect(0, 0, lastBarFillW, 6, ST77XX_RED);
    }

    drawHeader();
    tft.drawRect(BOARD_X - 1, BOARD_Y - 1, BOARD_W + 2, BOARD_H + 2, ST77XX_WHITE);

    for (int y = 0; y < GRID_H; y++) {
      for (int x = 0; x < GRID_W; x++) {
        if (grid[y][x]) {
          drawCell(x, y, gridColor[y][x]);
        }
      }
    }

    if (!gameOver) {
      drawPiece(curPiece, curRot, curX, curY, pieces[curPiece].color);
      drawNextPiece();
    }
  };


  
auto spawnPiece = [&]() {

  curPiece = nextPiece;
  nextPiece = random(0, 7);

  curRot = 0;
  curX = GRID_W / 2 - 2;
  curY = GRID_H - 3;

    if (!isValid(curPiece, curRot, curX, curY)) {
      gameOver = true;

      if (score > topScore) {
        topScore = score;
        gamePrefs.begin("tetris", false);
        gamePrefs.putInt("tetristopScore", topScore);
        gamePrefs.end();
      }

      drawBoard();
      drawOverlayCentered(ST77XX_RED, ST77XX_RED, "GAME OVER", "PRESS ANY KEY");
    }

  if (score > topScore) {
  topScore = score;
  gamePrefs.begin("tetris", false);
  gamePrefs.putInt("tetristopScore", topScore);
  gamePrefs.end();
}
};

  auto lockPiece = [&]() {
    for (int i = 0; i < 4; i++) {
      int x = pieces[curPiece].blocks[curRot][i][0] + curX;
      int y = pieces[curPiece].blocks[curRot][i][1] + curY;

      if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H) {
        grid[y][x] = 1;
        gridColor[y][x] = pieces[curPiece].color;
      }
    }
  };

auto clearLines = [&]() -> bool {
  int cleared = 0;
  int fullRows[4];
  int fullCount = 0;

  // 1) znajdź wszystkie pełne linie
  for (int y = 0; y < GRID_H; y++) {
    bool full = true;

    for (int x = 0; x < GRID_W; x++) {
      if (!grid[y][x]) {
        full = false;
        break;
      }
    }

    if (full) {
      if (fullCount < 4) {
        fullRows[fullCount++] = y;
      }
    }
  }

  // 2) flash wszystkich naraz
  if (fullCount > 0) {
    for (int i = 0; i < fullCount; i++) {
      int y = fullRows[i];
      for (int x = 0; x < GRID_W; x++) {
        drawCell(x, y, ST77XX_WHITE);
      }
    }

    delay(100);

    // 3) usuń linie od dołu do góry
    for (int i = 0; i < fullCount; i++) {
      int y = fullRows[i] - i; // po każdym usunięciu wszystko nad nią spada o 1
      cleared++;

      for (int yy = y; yy < GRID_H - 1; yy++) {
        for (int x = 0; x < GRID_W; x++) {
          grid[yy][x] = grid[yy + 1][x];
          gridColor[yy][x] = gridColor[yy + 1][x];
        }
      }

      for (int x = 0; x < GRID_W; x++) {
        grid[GRID_H - 1][x] = 0;
        gridColor[GRID_H - 1][x] = ST77XX_BLACK;
      }
    }

    linesCleared += cleared;

    if (cleared == 1) score += 100;
    else if (cleared == 2) score += 300;
    else if (cleared == 3) score += 500;
    else if (cleared == 4) score += 800;
  }

  if (score > topScore) {
    recordBrokenThisRun = true;
  }

  return (cleared > 0);
};

  auto startGame = [&]() {
    clearGrid();
    score = 0;
    linesCleared = 0;
    gameOver = false;
    nextPiece = random(0,7);
    exitHoldMs = 0;
    lastBarFillW = 0;
    lrHoldStartMs = 0;
    lrRepeatMs = 0;
    lrHeldDir = 0;
    lastFallMs = millis();
    spawnPiece();
    drawBoard();
    recordBrokenThisRun = false;

  };

  // ===== init =====
if (!inited) {
  inited = true;
  needFullRedraw = false;
  musicStarted = false;

  if (!prefsInited) {
    gamePrefs.begin("tetris", false);
    topScore = gamePrefs.getInt("tetristopScore", 0);
    gamePrefs.end();
    prefsInited = true;
  }

  startGame();

  if (!musicStarted) {
    canPlayTetrisMusic();
    musicStarted = true;
  }
}

  // ===== wybudzanie =====
  bool anyBtnRaw =
    digitalRead(BTN_UP) == LOW ||
    digitalRead(BTN_DOWN) == LOW ||
    digitalRead(BTN_SELECT) == LOW ||
    digitalRead(BTN_BACK) == LOW;

  if (anyBtnRaw) {
    lastInputTime = millis();

    if (screenDimmed) {
      digitalWrite(LCD_PWR, HIGH);
      delay(10);
      screenDimmed = false;
      delay(120);
      needFullRedraw = true;
    }
  }

  // ===== wyjście z gry: SELECT + BACK =====
bool upRaw   = (digitalRead(BTN_UP)   == LOW);
bool downRaw = (digitalRead(BTN_DOWN) == LOW);

if (upRaw && downRaw) {
    if (exitHoldMs == 0) {
      exitHoldMs = millis();
      lastBarFillW = 0;
      tft.fillRect(0, 0, tft.width(), 6, ST77XX_BLACK);
    }

    uint32_t held = millis() - exitHoldMs;
    if (held > EXIT_HOLD_THRESHOLD) held = EXIT_HOLD_THRESHOLD;

    int fillW = (int)((held * (uint32_t)tft.width()) / EXIT_HOLD_THRESHOLD);

    if (fillW > lastBarFillW) {
      tft.fillRect(lastBarFillW, 0, fillW - lastBarFillW, 6, ST77XX_RED);
      lastBarFillW = fillW;
    }

    if (held >= EXIT_HOLD_THRESHOLD) {
  canStopMinigameMusic();

  tetrisActive = false;
  tetrisComboReleased = false;
  inited = false;
  gameOver = false;
  musicStarted = false;
  comboReady = false;
  exitHoldMs = 0;
  lastBarFillW = 0;

  bSelect.armed = false;
  bBack.armed = false;

  redraw();
  return;
}

    return;
  } else if (exitHoldMs != 0) {
    exitHoldMs = 0;
    lastBarFillW = 0;
    drawBoard();

    if (gameOver) {
      drawOverlayCentered(ST77XX_RED, ST77XX_RED, "GAME OVER", "PRESS ANY KEY");
    }
  }

  // ===== redraw =====
  if (needFullRedraw) {
    needFullRedraw = false;
    drawBoard();

    if (gameOver) {
      drawOverlayCentered(ST77XX_RED, ST77XX_RED, "GAME OVER", "PRESS ANY KEY");
    }
  }

  // ===== game over =====
  if (gameOver) {
    if (pressedNow(bUp) ||  pressedNow(bSelect) || pressedNow(bBack)) {
      startGame();
    }
    return;
  }

  // ===== sterowanie =====
    bool changed = false;
int oldX = curX;
int oldY = curY;
int oldRot = curRot;

  const uint32_t LR_INITIAL_DELAY_MS = 180; // chwila pauzy po 1. ruchu
  const uint32_t LR_REPEAT_MS = 70;         // potem szybkie przesuwanie

  bool leftHeld  = (digitalRead(BTN_BACK) == LOW);
  bool rightHeld = (digitalRead(BTN_SELECT) == LOW);

  int wantedDir = 0;
  if (leftHeld && !rightHeld) wantedDir = -1;
  if (rightHeld && !leftHeld) wantedDir = 1;

  if (wantedDir == 0) {
    lrHeldDir = 0;
    lrHoldStartMs = 0;
    lrRepeatMs = 0;
  } else {
    uint32_t nowMs = millis();

    // nowy hold albo zmiana kierunku -> natychmiast 1 ruch
    if (lrHeldDir != wantedDir) {
      lrHeldDir = wantedDir;
      lrHoldStartMs = nowMs;
      lrRepeatMs = nowMs;

      if (isValid(curPiece, curRot, curX + wantedDir, curY)) {
        curX += wantedDir;
        changed = true;
      }
    } else {
      // po chwili zacznij auto-repeat
      if ((nowMs - lrHoldStartMs >= LR_INITIAL_DELAY_MS) &&
          (nowMs - lrRepeatMs >= LR_REPEAT_MS)) {
        lrRepeatMs = nowMs;

        if (isValid(curPiece, curRot, curX + wantedDir, curY)) {
          curX += wantedDir;
          changed = true;
        }
      }
    }
  }

  if (pressedNow(bUp)) {
    int newRot = (curRot + 1) & 3;
    if (isValid(curPiece, newRot, curX, curY)) {
      curRot = newRot;
      changed = true;
    } else if (isValid(curPiece, newRot, curX - 1, curY)) {
      curX--;
      curRot = newRot;
      changed = true;
    } else if (isValid(curPiece, newRot, curX + 1, curY)) {
      curX++;
      curRot = newRot;
      changed = true;
    }
  }

  uint32_t now = millis();
  uint32_t interval = (digitalRead(BTN_DOWN) == LOW) ? SOFT_DROP_MS : FALL_INTERVAL_MS;

if (changed) {
  clearPiece(curPiece, oldRot, oldX, oldY);
  drawPiece(curPiece, curRot, curX, curY, pieces[curPiece].color);
}

  if (now - lastFallMs >= interval) {
    lastFallMs = now;
if (isValid(curPiece, curRot, curX, curY - 1)) {
  clearPiece(curPiece, curRot, curX, curY);
  curY--;
  drawPiece(curPiece, curRot, curX, curY, pieces[curPiece].color);
}else {
lockPiece();
bool linesWereCleared = clearLines();
spawnPiece();

if (linesWereCleared) {
  redrawBoardArea();
} else {
  drawPiece(curPiece, curRot, curX, curY, pieces[curPiece].color);
}

drawHeader();
drawNextPiece();

      if (gameOver) {
        drawOverlayCentered(ST77XX_RED, ST77XX_RED, "GAME OVER", "PRESS ANY KEY");
      }
    }
  }
}
