#pragma once

#include <Wire.h>
#include <Arduino.h>
#include "helpers/ESP32Board.h"

// BQ27220 fuel-gauge I2C address (see mverch67/BQ27220 / BQ27220_I2C_ADDRESS).
#ifndef BQ27220_I2C_ADDR
  #define BQ27220_I2C_ADDR 0x55
#endif

class TLoraPagerBoard : public ESP32Board {
public:
  void begin();

  // Battery state of charge (%) from the BQ27220 fuel gauge. Returns -1 if the
  // gauge is not present / not responding (bring-up: no on-device verification
  // has been performed yet, see the variant README).
  int getBattPercent();

  // Raw pack voltage in mV from the BQ27220 (PACK_V), 0 if unavailable.
  uint16_t getBattMilliVolts() override;

  const char* getManufacturerName() const override {
    return "LilyGo T-LoRa Pager";
  }
};
