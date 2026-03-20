#pragma once
#include <LovyanGFX.hpp>

// Piny TFT
#define TFT_CS 10
#define TFT_DC 14
#define TFT_RST 21
#define TFT_MOSI 11
#define TFT_MISO -1
#define TFT_SCK 12

class LGFX_ILI9488 : public lgfx::LGFX_Device
{
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9488 _panel;

public:
  LGFX_ILI9488()
  {
    auto bcfg = _bus.config();
    bcfg.spi_host = SPI2_HOST;
    bcfg.spi_mode = 0;
    bcfg.freq_write = 27000000;
    bcfg.freq_read = 16000000;
    bcfg.spi_3wire = (TFT_MISO == -1);
    bcfg.use_lock = true;
    bcfg.dma_channel = 1;
    bcfg.pin_sclk = TFT_SCK;
    bcfg.pin_mosi = TFT_MOSI;
    bcfg.pin_miso = TFT_MISO;
    bcfg.pin_dc = TFT_DC;
    _bus.config(bcfg);
    _panel.setBus(&_bus);

    auto pcfg = _panel.config();
    pcfg.pin_cs = TFT_CS;
    pcfg.pin_rst = TFT_RST;
    pcfg.pin_busy = -1;
    pcfg.panel_width = 320;
    pcfg.panel_height = 480;
    pcfg.offset_x = 0;
    pcfg.offset_y = 0;
    pcfg.offset_rotation = 0;
    pcfg.dummy_read_pixel = 8;
    pcfg.dummy_read_bits = 1;
    pcfg.readable = (TFT_MISO != -1);
    pcfg.invert = false;
    pcfg.rgb_order = false;
    pcfg.dlen_16bit = false;
    pcfg.bus_shared = false;
    _panel.config(pcfg);

    setPanel(&_panel);
  }
};

extern LGFX_ILI9488 tft;