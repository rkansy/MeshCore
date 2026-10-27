#include <Arduino.h>
#include "TLoraPagerBoard.h"
#include <bq27220.h>

static BQ27220 bq27220;  // uses the global Wire (begun on pins 3/2 by begin())

static bool gaugePresent();

// XL9555 I/O expander at 0x20 (TCA9555 register map): gates peripheral
// power rails. Meshtastic's tlora-pager earlyInitVariant enables
// DRV_EN(0), LORA_EN(3), GPS_EN(4), KB_EN(8), GPIO_EN(9), SD_EN(12) and
// leaves AMP_EN(1) low. The chip's power-on defaults already enable most
// gates, but set them explicitly so every boot path (USB, battery,
// power-button) reaches a known state.
static void xl9555_init() {
  constexpr uint8_t ADDR = 0x20;
  Wire.beginTransmission(ADDR);
  if (Wire.endTransmission() != 0)
    return;  // expander absent / unpowered

  auto readReg = [&](uint8_t reg) -> uint8_t {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
      return 0xFF;
    if (Wire.requestFrom(ADDR, (uint8_t)1) != 1)
      return 0xFF;
    return Wire.read();
  };
  auto writeReg = [&](uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  };

  // Config: all outputs except SD_PULLEN (pin 11 = port B bit 3) input.
  writeReg(0x10, 0x00);
  writeReg(0x11, 0x08);

  // Port A: DRV_EN, LORA_EN, GPS_EN on; AMP_EN off; keep other bits.
  uint8_t outA = readReg(0x02);
  outA = (outA & ~0x02u) | 0x19u;
  writeReg(0x02, outA);

  // Port B: KB_EN, GPIO_EN, SD_EN on; keep other bits.
  uint8_t outB = readReg(0x03);
  outB |= 0x13u;
  writeReg(0x03, outB);
}

void TLoraPagerBoard::begin() {
  ESP32Board::begin();  // starts Wire on PIN_BOARD_SDA/PIN_BOARD_SCL

  xl9555_init();

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
// read. On this Arduino-ESP32 core endTransmission() returns the bus status,
// so a NAK (gauge absent, as on the T-LoRa Pager) skips the read and keeps
// the Wire error log clean.
static bool gaugePresent() {
  Wire.beginTransmission(BQ27220_I2C_ADDR);
  if (Wire.endTransmission() != 0)
    return false;
  Wire.requestFrom(BQ27220_I2C_ADDR, (uint8_t)1);
  bool present = (Wire.available() >= 1);
  if (present)
    (void)Wire.read();
  return present;
}
