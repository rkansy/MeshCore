// Diagnostic: XL9555 expander state + try enabling the keyboard gate.
#include <Arduino.h>
#include <Wire.h>

#define BUILD_ID "XL9555-V8"
#define EXP_ADDR 0x20
#define KB_ADDR 0x32

static bool x_read(uint8_t reg, uint8_t* v) {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(EXP_ADDR, (uint8_t)1) != 1) return false;
  *v = Wire.read();
  return true;
}

static bool x_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool kb_alive() {
  Wire.beginTransmission(KB_ADDR);
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom(KB_ADDR, (uint8_t)1);
  return Wire.available() >= 1;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  log_e("%s start", BUILD_ID);
  Wire.begin(3, 2, 400000UL);
  delay(100);

  uint8_t a = 0, b = 0, oa = 0, ob = 0, ca = 0, cb = 0;
  bool ok = x_read(0x00, &a) && x_read(0x01, &b) && x_read(0x02, &oa) && x_read(0x03, &ob) && x_read(0x10, &ca) && x_read(0x11, &cb);
  log_e("%s expander: inA=0x%02X inB=0x%02X outA=0x%02X outB=0x%02X cfgA=0x%02X cfgB=0x%02X",
        BUILD_ID, a, b, oa, ob, ca, cb);
  log_e("%s kb before: %s", BUILD_ID, kb_alive() ? "ALIVE" : "DEAD");

  // meshtastic earlyInitVariant gates:
  //   port A: DRV_EN(0)=1, LORA_EN(3)=1, GPS_EN(4)=1   => 0x19 (AMP_EN(1) stays 0)
  //   port B: KB_EN(8)=1 bit0, GPIO_EN(9)=1 bit1, SD_EN(12)=1 bit4 => 0x13
  x_write(0x10, 0x00);  // port A all outputs
  x_write(0x11, 0x00);  // port B all outputs
  x_write(0x02, 0x19);
  x_write(0x03, 0x13);
  delay(200);
  x_read(0x02, &oa);
  x_read(0x03, &ob);
  log_e("%s expander after write: outA=0x%02X outB=0x%02X", BUILD_ID, oa, ob);
  log_e("%s kb after gates-on: %s", BUILD_ID, kb_alive() ? "ALIVE" : "DEAD");

  // try KB_EN alone: port B = 0x01
  x_write(0x03, 0x01);
  delay(200);
  log_e("%s kb KB_EN only: %s", BUILD_ID, kb_alive() ? "ALIVE" : "DEAD");
  log_e("%s done", BUILD_ID);
}

void loop() {
  delay(10000);
  log_e("%s kb now: %s", BUILD_ID, kb_alive() ? "ALIVE" : "DEAD");
}
