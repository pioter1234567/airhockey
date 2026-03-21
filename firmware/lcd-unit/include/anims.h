#pragma once
#include <stdint.h>

void animsInit();
void animsTick();

void animsStartTest();
void animsStop();
bool animsIsPlaying();

// nowy test logiki:
void animsStartStateTest(uint8_t scoreA, uint8_t scoreB, char whichChar);
// whichChar: 'a' albo 'b'
void animsStartResult(uint8_t scoreA, uint8_t scoreB);