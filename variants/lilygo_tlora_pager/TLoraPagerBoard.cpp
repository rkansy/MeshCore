#include <Arduino.h>
#include "TLoraPagerBoard.h"
#include <bq27220.h>

static BQ27220 bq27220;  // uses the global Wire (begun on pins 3/2 by begin())

static bool gaugePresent();

void TLoraPagerBoard::begin() {
  ESP32Board::begin();  // starts Wire on PIN_BOARD_SDA/PIN_BOARD_SCL

#if defined(PIN_USER_BTN)
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

#if defined(PIN_ROTARY_PRESS)
  pinMode(PIN_ROTARY_PRESS, INPUT_PULLUP);
#endif

#ifdef P_LORA_MISO
  pinMode(P_LORA_MISO, INPUT_PULLUP);
#endif
}

int TLoraPagerBoard::getBattPercent() {
  if (!gaugePresent())
    return -1;

  int soc = bq27220.getChargePercent();
  if (soc < 0 || soc > 100)
    return -1;  // not yet calibrated / invalid
  return soc;
}

uint16_t TLoraPagerBoard::getBattMilliVolts() {
  if (!gaugePresent())
    return 0;
  return bq27220.getVoltage();
}

// Probe the fuel gauge: an empty write (address only) followed by a 1-byte
// read. A NAK leaves nothing available. (beginTransmission() returns void on
// this Arduino-ESP32 core, so NAK can't be read from it directly.)
static bool gaugePresent() {
  Wire.beginTransmission(BQ27220_I2C_ADDR);
  Wire.endTransmission();
  Wire.requestFrom(BQ27220_I2C_ADDR, (uint8_t)1);
  bool present = (Wire.available() >= 1);
  if (present)
    (void)Wire.read();
  return present;
}
