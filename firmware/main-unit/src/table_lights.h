#pragma once

#include <Arduino.h>

void tableLightsInit(uint8_t brightness = 255);

void tableLightsAllOff();
void tableLightsTick(uint32_t nowMs);

// BIN-y muzyczne ambiente
void tableLightsMusicStart(const char *ambientPath, uint32_t syncStartMs);
void tableLightsMusicStop();

// Kolor / oddychanie ambiente jak matryca rundy
void tableLightsSetRoundColor(uint8_t r, uint8_t g, uint8_t b);
void tableLightsSetBreathing(bool enable);
void tableLightsSetRoundEnabled(bool enable);

// Na później — bramki
void tableLightsGoalFx(uint8_t scoringSide);
void tableLightsSetGoalsEnabled(bool enabled);
