//
// T-LoRa Pager bring-up / boot test (MULT-197, Stage 2).
//
// This is NOT the pager chat UI (that is Stage 3 / MULT-198). It exists to
// prove the board bring-up: the ST7796 display renders, the TCA8418 keyboard
// produces characters, the rotary encoder updates a counter and the user
// button toggles a state. It also reports the radio init result and the BQ27220
// battery level.
//
#include <Arduino.h>
#include <target.h>

#define INPUT_BUF_SIZE 48

static char input_buf[INPUT_BUF_SIZE];
static int input_len = 0;
static int rotary_count = 0;
static bool button_on = false;
static bool radio_ok = false;
static bool display_ok = false;
static uint32_t last_battery_read = 0;
static int battery_pct = -1;

static void drawScreen() {
  char line[64];

  display.startFrame(UIColor::window_bkg);

  // Title bar
  display.setColor(UIColor::title_bkg);
  display.fillRect(0, 0, display.width(), 30);
  display.setColor(UIColor::title_txt);
  display.setTextSize(2);
  display.setCursor(6, 8);
  display.print("MeshCore  -  T-LoRa Pager (bring-up)");

  display.setColor(UIColor::primary_txt);
  display.setTextSize(2);

  if (battery_pct >= 0)
    snprintf(line, sizeof(line), "Battery:  %d%%", battery_pct);
  else
    snprintf(line, sizeof(line), "Battery:  --");
  display.setCursor(6, 46);
  display.print(line);

  snprintf(line, sizeof(line), "Radio:    %s", radio_ok ? "SX1262 OK" : "FAILED");
  display.setCursor(6, 74);
  display.print(line);

  snprintf(line, sizeof(line), "Rotary:   %d   (spin me)", rotary_count);
  display.setCursor(6, 102);
  display.print(line);

  snprintf(line, sizeof(line), "Button:   %s   (pin %d)", button_on ? "ON" : "OFF", (int)user_btn.getPin());
  display.setCursor(6, 130);
  display.print(line);

  display.setCursor(6, 158);
  display.print("Type on the keyboard:");

  display.setCursor(6, 186);
  if (input_len == 0)
    display.print("<waiting for keypress...>");
  else
    display.print(input_buf);

  display.endFrame();
}

static void handleKey(char k) {
  if (k >= 32 && k < 127) {  // printable ASCII
    if (input_len < (int)sizeof(input_buf) - 1)
      input_buf[input_len++] = k;
  } else if (k == TCA8418Keyboard::BSP) {
    if (input_len > 0)
      input_buf[--input_len] = 0;
  } else if (k == TCA8418Keyboard::SELECT || k == TCA8418Keyboard::ESC) {
    // Enter / Esc clear the demo line
    input_len = 0;
    input_buf[0] = 0;
  } else {
    return;  // TAB / BL_TOGGLE etc.
  }
  drawScreen();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  MESH_DEBUG_PRINTLN("MeshCore T-LoRa Pager bring-up boot");

  board.begin();

  display_ok = display.begin();
  if (display_ok) {
    radio_ok = radio_init();
    MESH_DEBUG_PRINTLN("radio_init -> %s", radio_ok ? "ok" : "FAILED");

    keyboard.begin();
    rotary.begin();
    user_btn.begin();
    keyboard.setBacklightBrightness(127);
    drawScreen();
  }
}

void loop() {
  if (!display_ok)
    return;

  // Keyboard (interrupt-driven; drain the event queue)
  keyboard.update();
  while (keyboard.hasKey())
    handleKey(keyboard.readKey());

  // Rotary encoder
  RotaryInputEvent ev = rotary.poll();
  if (ev == RotaryInputEvent::Next) {
    rotary_count++;
    drawScreen();
  } else if (ev == RotaryInputEvent::Prev) {
    rotary_count--;
    drawScreen();
  }

  // User button
  if (user_btn.check() == BUTTON_EVENT_CLICK) {
    button_on = !button_on;
    drawScreen();
  }

  // Battery (BQ27220 I2C read) ~2x/second
  if (millis() - last_battery_read > 500) {
    last_battery_read = millis();
    int p = board.getBattPercent();
    if (p != battery_pct) {
      battery_pct = p;
      drawScreen();
    }
  }

  delay(5);
}
