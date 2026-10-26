#pragma once

#include <Arduino.h>
#include <Wire.h>

//
// TCA8418 I2C keyboard driver for the LilyGO T-LoRa Pager.
//
// Interrupt-driven: a key press/release pulls the INT pin low, an interrupt
// marks the controller dirty, and update() (called from loop()) drains the
// controller's key-event FIFO and translates the physical key into a character
// or special key using the T-LoRa Pager multi-tap keymap.
//
// The register access, state machine and keymap are ported from meshtastic's
// src/input/TCA8418KeyboardBase.* and src/input/TLoraPagerKeyboard.* (stripped
// of the meshtastic config / haptic dependencies).
//
class TCA8418Keyboard {
public:
  // Special key values (non-printable). Printable keys are plain ASCII.
  enum Key : char {
    NONE = 0x00,
    BSP = 0x08,
    TAB = 0x09,
    SELECT = 0x0D,   // enter
    ESC = 0x1B,
    BL_TOGGLE = 0xAB // backlight toggle
  };

  // addr     : I2C address of the TCA8418 (default 0x32)
  // int_pin  : keyboard INT line (active low)
  // bl_pin   : keyboard backlight PWM pin
  TCA8418Keyboard(uint8_t addr = 0x32, int8_t int_pin = 6, int8_t bl_pin = 46);

  // T-LoRa Pager multi-tap keymap parameters (exposed so the keymap table can
  // be sized from them in the .cpp).
  static constexpr uint8_t TAP_MODS = 3;    // 3 characters per key (multi-tap)
  static constexpr uint8_t ROWS = 4;
  static constexpr uint8_t COLS = 10;
  static constexpr uint8_t NUM_KEYS = 31;
  static constexpr uint32_t MULTI_TAP_THRESHOLD = 1500;

  // keymap index 28 is the SHIFT modifier, 20 the symbol (number) modifier
  static constexpr uint8_t MODIFIER_SHIFT_KEY = 28;
  static constexpr uint8_t MODIFIER_SYM_KEY = 20;

  bool begin(TwoWire &wire = Wire);
  void reset();

  // Call from loop(). Reads the INT line, drains the key FIFO and fills the
  // internal event queue.
  void update();

  bool hasKey() const { return _head != _tail; }
  char readKey();        // returns the next key (NONE if empty)

  // Backlight (LED PWM on bl_pin).
  void setBacklight(bool on);
  void setBacklightBrightness(uint8_t value);
  void toggleBacklight();
  uint8_t getBacklightBrightness() const { return _brightness; }

private:
  // TCA8418 registers used by this driver
  enum : uint8_t {
    REG_CFG = 0x01,
    REG_INT_STAT = 0x02,
    REG_KEY_LCK_EC = 0x03,
    REG_KEY_EVENT_A = 0x04,
    REG_GPIO_INT_STAT_1 = 0x11,
    REG_GPIO_INT_STAT_2 = 0x12,
    REG_GPIO_INT_STAT_3 = 0x13,
    REG_GPIO_INT_EN_1 = 0x1A,
    REG_GPIO_INT_EN_2 = 0x1B,
    REG_GPIO_INT_EN_3 = 0x1C,
    REG_KP_GPIO_1 = 0x1D,
    REG_KP_GPIO_2 = 0x1E,
    REG_KP_GPIO_3 = 0x1F,
    REG_GPI_EM_1 = 0x20,
    REG_GPI_EM_2 = 0x21,
    REG_GPI_EM_3 = 0x22,
    REG_GPIO_DIR_1 = 0x23,
    REG_GPIO_DIR_2 = 0x24,
    REG_GPIO_DIR_3 = 0x25,
    REG_GPIO_INT_LVL_1 = 0x26,
    REG_GPIO_INT_LVL_2 = 0x27,
    REG_GPIO_INT_LVL_3 = 0x28,
    REG_DEBOUNCE_DIS_1 = 0x29,
    REG_DEBOUNCE_DIS_2 = 0x2A,
    REG_DEBOUNCE_DIS_3 = 0x2B
  };

  // Single-instance IRQ trampoline (this Arduino-ESP32 core's attachInterrupt
  // only takes a plain function pointer, not a lambda).
  static TCA8418Keyboard* _instance;
  static void _irqHandler();

  uint8_t _addr;
  int8_t _int_pin;
  int8_t _bl_pin;
  TwoWire* _wire;

  char _queue[16];
  uint8_t _head = 0;
  uint8_t _tail = 0;

  uint8_t _modifierFlag = 0;
  uint32_t _last_modifier_time = 0;
  uint8_t _last_key = 255;
  uint8_t _next_key = 255;
  uint32_t _last_tap = 0;

  uint8_t _brightness = 0;
  bool _backlight_initialized = false;
  volatile bool _int_pending = false;

  uint8_t readReg(uint8_t reg);
  void writeReg(uint8_t reg, uint8_t value);
  uint8_t keyCount();

  void configureMatrix();
  void enableDebounce();
  void flush();
  void processEvents();
  void pressed(uint8_t key);
  void released();
  void queueEvent(char c);
  void updateModifierFlag(uint8_t key);
  bool isModifierKey(uint8_t key) const;
};
