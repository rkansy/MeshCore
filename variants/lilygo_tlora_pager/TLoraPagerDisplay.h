#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <lgfx/v1/panel/Panel_ST7796.hpp>

//
// ST7796 display for the LilyGO T-LoRa Pager.
//
// The ST7796 shares the SPI host / bus with the SX1262 LoRa radio (SCK 35,
// MOSI 34, MISO 33) but is selected with its own CS (38). Configuration mirrors
// the LGFX_TLORA_PAGER panel in meshtastic/src/graphics/TFTDisplay.cpp.
//
// The active area is 222 x 480 in the panel's native (portrait) orientation;
// with offset_rotation 3 the logical screen becomes 480 x 222 landscape, with a
// 49px X offset to centre the active area.
//
#ifndef PIN_ST7796_SCK
  #define PIN_ST7796_SCK    35
#endif
#ifndef PIN_ST7796_MOSI
  #define PIN_ST7796_MOSI   34
#endif
#ifndef PIN_ST7796_MISO
  #define PIN_ST7796_MISO   33
#endif
#ifndef PIN_ST7796_CS
  #define PIN_ST7796_CS     38
#endif
#ifndef PIN_ST7796_DC
  #define PIN_ST7796_DC     37
#endif
#ifndef PIN_ST7796_RST
  #define PIN_ST7796_RST    -1
#endif
#ifndef PIN_ST7796_BL
  #define PIN_ST7796_BL     42
#endif
#ifndef ST7796_SPI_HOST
  #define ST7796_SPI_HOST   SPI2_HOST
#endif
#ifndef ST7796_SPI_FREQ
  #define ST7796_SPI_FREQ   75000000
#endif

class LGFX_TLORA_PAGER : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX_TLORA_PAGER(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = ST7796_SPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = ST7796_SPI_FREQ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_ST7796_SCK;
      cfg.pin_mosi = PIN_ST7796_MOSI;
      cfg.pin_miso = PIN_ST7796_MISO;
      cfg.pin_dc = PIN_ST7796_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = PIN_ST7796_CS;
      cfg.pin_rst = PIN_ST7796_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 222;        // native portrait width
      cfg.panel_height = 480;       // native portrait height
      cfg.offset_x = 49;            // centre the active area
      cfg.offset_y = 0;
      cfg.offset_rotation = 3;      // landscape
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = true;        // SPI bus shared with the LoRa radio
      cfg.readable = true;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = PIN_ST7796_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

//
// TLoraPagerDisplay - wraps the ST7796 panel in MeshCore's LGFXDisplay driver
// interface. Overrides begin() so the logical rotation matches meshtastic's
// landscape orientation (offset_rotation 3, base rotation 0) and uses 16-bit
// colour for the colour panel.
//
class TLoraPagerDisplay : public LGFXDisplay {
  LGFX_TLORA_PAGER _disp;

public:
  TLoraPagerDisplay() : LGFXDisplay(480, 222, _disp) {}

  bool begin() {
    turnOn();
    display->init();
    display->setRotation(0);        // base 0 + offset_rotation 3 => landscape
    display->setBrightness(64);
    display->setColorDepth(16);
    display->setTextColor(TFT_WHITE);

    buffer.setColorDepth(16);
    buffer.setPsram(true);
    buffer.createSprite(display->width(), display->height());

    _isOn = true;
    return true;
  }
};
