#pragma once

#define TETRIS_CYAN   0x07FF
#define TETRIS_YELLOW 0xFFE0
#define TETRIS_PURPLE 0x780F
#define TETRIS_GREEN  0x07E0
#define TETRIS_RED    0xF800
#define TETRIS_BLUE   0x001F
#define TETRIS_ORANGE 0xFD20



void runSnake();
void runTetris();

extern bool snakeActive;
extern bool tetrisActive;

extern bool snakeComboReleased;
extern bool tetrisComboReleased;