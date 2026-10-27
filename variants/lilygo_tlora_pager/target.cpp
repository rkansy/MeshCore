#include <Arduino.h>
#include "target.h"

TLoraPagerBoard board;

// The SX1262 and ST7796 share the same physical SPI bus (SCK 35, MOSI 34,
// MISO 33) but use separate chip-select lines, so the radio and display are
// each initialised on the shared host with their own pins.
static SPIClass lora_spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, lora_spi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);
#else
EnvironmentSensorManager sensors;
#endif

TLoraPagerDisplay display;
TCA8418Keyboard keyboard(KB_I2C_ADDR, KB_INT_PIN, KB_BL_PIN);
RotaryInputGPIO rotary(PIN_ROTARY_A, PIN_ROTARY_B, true, true);
MomentaryButton user_btn(PIN_USER_BTN, 1000, true, true);

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#ifdef P_LORA_SCLK
  return radio.std_init(&lora_spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
