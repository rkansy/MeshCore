#include <Arduino.h>
#include "target.h"

TLoraPagerBoard board;

// The SX1262 and ST7796 share the same wires and ESP32-S3 SPI2 controller.
// Select Arduino FSPI (SPI2_HOST) explicitly; SPIClass' default HSPI selects
// SPI3_HOST on S3 and makes two controllers drive the same physical wires.
static SPIClass lora_spi(FSPI);
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
