#pragma once

#define USER_SETUP_ID 999

#define ILI9488_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC   14
#define TFT_RST  21
#define TFT_MISO -1

#define TFT_BL   4

#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY  16000000

#define LOAD_GLCD
#define LOAD_GFXFF
#define SMOOTH_FONT