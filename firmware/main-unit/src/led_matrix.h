#pragma once
#include <stdint.h>

void matrixInit(uint8_t brightness);
void matrixClear();
void matrixShow();

void matrixSetPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void matrixFill(uint8_t r, uint8_t g, uint8_t b);
void matrixSetIndex(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
// wersja bez serpentine (czysto liniowo wierszami y*64+x)
void matrixSetPixelLinear(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void matrixGoalSplit(uint8_t side); // 0=A, 1=B

uint16_t matrixWidth();
uint16_t matrixHeight();
