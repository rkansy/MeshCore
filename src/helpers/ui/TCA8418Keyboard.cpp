#include "TCA8418Keyboard.h"

// T-LoRa Pager multi-tap keymap (31 keys, 3 characters each).
// Ported from meshtastic src/input/TLoraPagerKeyboard.cpp (TLoraPagerTapMap).
// Layout (physical key rows):
//   0: q w e r t y u i o p
//   1: a s d f g h j k l .
//   2: ENTER (SYM-mod) z x c v b n m  (SHIFT-mod)
//   3: (empty) BSP(ESC) SPACE(BL_TOGGLE)
static const char kTapMap[TCA8418Keyboard::NUM_KEYS][TCA8418Keyboard::TAP_MODS] = {
  {'q', 'Q', '1'},
  {'w', 'W', '2'},
  {'e', 'E', '3'},
  {'r', 'R', '4'},
  {'t', 'T', '5'},
  {'y', 'Y', '6'},
  {'u', 'U', '7'},
  {'i', 'I', '8'},
  {'o', 'O', '9'},
  {'p', 'P', '0'},
  {'a', 'A', '*'},
  {'s', 'S', '/'},
  {'d', 'D', '+'},
  {'f', 'F', '-'},
  {'g', 'G', '='},
  {'h', 'H', ':'},
  {'j', 'J', '\''},
  {'k', 'K', '"'},
  {'l', 'L', '@'},
  {TCA8418Keyboard::SELECT, 0x00, TCA8418Keyboard::TAB},
  {0x00, 0x00, 0x00},
  {'z', 'Z', '_'},
  {'x', 'X', '$'},
  {'c', 'C', ';'},
  {'v', 'V', '?'},
  {'b', 'B', '!'},
  {'n', 'N', ','},
  {'m', 'M', '.'},
  {0x00, 0x00, 0x00},
  {TCA8418Keyboard::BSP, 0x00, TCA8418Keyboard::ESC},
  {' ', 0x00, TCA8418Keyboard::BL_TOGGLE},
};

namespace {
  constexpr uint8_t LEDC_BACKLIGHT_CHANNEL = 3;
  constexpr uint32_t LEDC_BACKLIGHT_FREQ = 1000;
  constexpr uint8_t LEDC_BACKLIGHT_BITS = 8;
}  // namespace

TCA8418Keyboard* TCA8418Keyboard::_instance = nullptr;

void TCA8418Keyboard::_irqHandler() {
  if (_instance)
    _instance->_int_pending = true;
}

TCA8418Keyboard::TCA8418Keyboard(uint8_t addr, int8_t int_pin, int8_t bl_pin)
  : _addr(addr), _int_pin(int_pin), _bl_pin(bl_pin), _wire(nullptr) {}

bool TCA8418Keyboard::begin(TwoWire &wire) {
  _wire = &wire;

  // Keyboard backlight via LED PWM. Continue keyboard bring-up if PWM cannot
  // be allocated; callers may still use the keys without a backlight.
  _backlight_initialized = ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BITS) != 0;
  if (_backlight_initialized) {
    ledcAttachPin(_bl_pin, LEDC_BACKLIGHT_CHANNEL);
  } else {
    Serial.println("TCA8418: keyboard backlight PWM unavailable");
  }

  // Probe the controller: presence is "ACKs a register read". Do NOT require
  // a chip-ID match: this unit's TCA8418 answers with chip_id=0x00 (not the
  // datasheet 0x18), which previously made a live keyboard look absent and
  // left the INT line unattached (all keys dead). If truly absent, degrade
  // gracefully so key polling stays out of the I2C bus.
  if (!probePresent()) {
    log_e("TCA8418: no keyboard at 0x%02X, continuing without it", _addr);
    pinMode(_int_pin, INPUT_PULLUP);
    return true;
  }
  _present = true;

  reset();

  // Enable key-event + GPI interrupts on the controller
  uint8_t cfg = readReg(REG_CFG);
  cfg |= 0x03;
  writeReg(REG_CFG, cfg);

  pinMode(_int_pin, INPUT_PULLUP);
  _instance = this;
  attachInterrupt(_int_pin, _irqHandler, FALLING);

  setBacklightBrightness(0);
  return true;
}

bool TCA8418Keyboard::probePresent() {
  uint8_t r0 = 0x00, chip_id = 0xFF;

  _wire->beginTransmission(_addr);
  _wire->write(0x00);
  if (_wire->endTransmission() != 0)
    return false;
  _wire->requestFrom(_addr, (uint8_t)1);
  if (_wire->available() >= 1)
    r0 = _wire->read();

  _wire->beginTransmission(_addr);
  _wire->write(0xE7); // CHIP_ID
  if (_wire->endTransmission() != 0)
    return false;
  _wire->requestFrom(_addr, (uint8_t)1);
  if (_wire->available() >= 1)
    chip_id = _wire->read();

  log_e("TCA8418 probe @0x%02X: reg0=0x%02X chip_id=0x%02X (chip_id not checked: unit reports 0x00)", _addr, r0, chip_id);
  return true;  // bus presence is enough (see note in begin())
}

void TCA8418Keyboard::reset() {
  // All GPIO as inputs, falling-edge interrupt, all in event mode
  writeReg(REG_GPIO_DIR_1, 0x00);
  writeReg(REG_GPIO_DIR_2, 0x00);
  writeReg(REG_GPIO_DIR_3, 0x00);
  writeReg(REG_GPI_EM_1, 0xFF);
  writeReg(REG_GPI_EM_2, 0xFF);
  writeReg(REG_GPI_EM_3, 0xFF);
  writeReg(REG_GPIO_INT_LVL_1, 0x00);
  writeReg(REG_GPIO_INT_LVL_2, 0x00);
  writeReg(REG_GPIO_INT_LVL_3, 0x00);
  writeReg(REG_GPIO_INT_EN_1, 0xFF);
  writeReg(REG_GPIO_INT_EN_2, 0xFF);
  writeReg(REG_GPIO_INT_EN_3, 0xFF);

  configureMatrix();
  enableDebounce();
  flush();
}

void TCA8418Keyboard::configureMatrix() {
  uint8_t mask = 0;
  for (int r = 0; r < ROWS; r++)
    mask = (mask << 1) | 1;
  writeReg(REG_KP_GPIO_1, mask);

  mask = 0;
  for (int c = 0; c < COLS && c < 8; c++)
    mask = (mask << 1) | 1;
  writeReg(REG_KP_GPIO_2, mask);

  if (COLS > 8)
    writeReg(REG_KP_GPIO_3, COLS == 9 ? 0x01 : 0x03);
}

void TCA8418Keyboard::enableDebounce() {
  writeReg(REG_DEBOUNCE_DIS_1, 0x00);
  writeReg(REG_DEBOUNCE_DIS_2, 0x00);
  writeReg(REG_DEBOUNCE_DIS_3, 0x00);
}

void TCA8418Keyboard::flush() {
  while (readReg(REG_KEY_EVENT_A) != 0) {
  }
  readReg(REG_GPIO_INT_STAT_1);
  readReg(REG_GPIO_INT_STAT_2);
  readReg(REG_GPIO_INT_STAT_3);
  writeReg(REG_INT_STAT, 3);
}

uint8_t TCA8418Keyboard::readReg(uint8_t reg) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->endTransmission();
  _wire->requestFrom(_addr, (uint8_t)1);
  if (_wire->available() < 1)
    return 0;
  return _wire->read();
}

void TCA8418Keyboard::writeReg(uint8_t reg, uint8_t value) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->write(value);
  _wire->endTransmission();
}

uint8_t TCA8418Keyboard::keyCount() {
  return readReg(REG_KEY_LCK_EC) & 0x0F;
}

void TCA8418Keyboard::update() {
  if (!_present)
    return;
  if (!_int_pending) {
    // Polling fallback: the TCA8418 INT line is not guaranteed on every
    // board wiring; Meshtastic polls for the same reason. Reading the FIFO
    // is cheap and key events are latched in-chip until read.
    uint32_t now = millis();
    if (now - _last_poll >= 50) {
      _last_poll = now;
      if (keyCount() == 0)
        return;
    }
  }
  _int_pending = false;
  processEvents();
}

void TCA8418Keyboard::processEvents() {
  uint8_t count = keyCount();
  for (uint8_t i = 0; i < count; i++) {
    uint8_t k = readReg(REG_KEY_EVENT_A + i);
    uint8_t key = k & 0x7F;
    if (k & 0x80)
      pressed(key);
    else
      released();
  }
}

void TCA8418Keyboard::pressed(uint8_t key) {
  if (_modifierFlag && (millis() - _last_modifier_time > MULTI_TAP_THRESHOLD))
    _modifierFlag = 0;

  int row = (key - 1) / COLS;
  int col = (key - 1) % COLS;
  if (row >= ROWS || col >= COLS)
    return;  // invalid key

  _next_key = row * COLS + col;

  uint32_t now = millis();
  updateModifierFlag(_next_key);
  if (isModifierKey(_next_key))
    _last_modifier_time = now;

  _last_key = _next_key;
  _last_tap = now;
}

void TCA8418Keyboard::released() {
  if (_last_key >= NUM_KEYS) {
    _last_key = 255;
    return;
  }

  uint8_t layer = _modifierFlag % TAP_MODS;
  char c = kTapMap[_last_key][layer];

  if (c == BL_TOGGLE) {
    toggleBacklight();
    log_e("TCA8418 key: backlight toggle");
  } else if (c != NONE) {
    queueEvent(c);
    log_e("TCA8418 key: layer=%u char=0x%02X key=%u", layer, (uint8_t)c, _last_key);
  }

  if (!isModifierKey(_last_key))
    _modifierFlag = 0;
}

void TCA8418Keyboard::updateModifierFlag(uint8_t key) {
  if (key == MODIFIER_SHIFT_KEY)
    _modifierFlag ^= 0x01;
  else if (key == MODIFIER_SYM_KEY)
    _modifierFlag ^= 0x02;
}

bool TCA8418Keyboard::isModifierKey(uint8_t key) const {
  return key == MODIFIER_SHIFT_KEY || key == MODIFIER_SYM_KEY;
}

void TCA8418Keyboard::queueEvent(char c) {
  if (c == NONE)
    return;
  uint8_t next = (_head + 1) % sizeof(_queue);
  if (next == _tail)
    return;  // queue full, drop
  _queue[_head] = c;
  _head = next;
}

char TCA8418Keyboard::readKey() {
  if (_head == _tail)
    return NONE;
  char c = _queue[_tail];
  _tail = (_tail + 1) % sizeof(_queue);
  return c;
}

void TCA8418Keyboard::setBacklight(bool on) {
  if (_backlight_initialized)
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, on ? _brightness : 0);
}

void TCA8418Keyboard::setBacklightBrightness(uint8_t value) {
  _brightness = value;
  if (_backlight_initialized)
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, value);
}

void TCA8418Keyboard::toggleBacklight() {
  if (_brightness == 0)
    _brightness = 40;
  else if (_brightness == 40)
    _brightness = 127;
  else
    _brightness = 0;
  setBacklightBrightness(_brightness);
}
