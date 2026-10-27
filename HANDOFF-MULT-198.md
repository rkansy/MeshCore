# MULT-198 handoff — T-LoRa Pager firmware (keyboard debugging session)

## Repository state

- Both pager PRs remain merged to `main`; repo at merge commit `93357c65`.
- **This session's fixes are uncommitted in the working tree** (`git status`):
  - `src/helpers/AutoDiscoverRTCClock.cpp` — RX8130CE probe behind `DISABLE_RX8130CE_PROBE`
  - `src/helpers/ESP32Board.h` — optional `PIN_BOARD_I2C_FREQ` for `Wire.begin`
  - `src/helpers/ui/TCA8418Keyboard.{h,cpp}` — presence probe in `begin()`, graceful degrade
  - `variants/lilygo_tlora_pager/TLoraPagerBoard.cpp` — XL9555 gate init + gauge NAK check
  - `variants/lilygo_tlora_pager/platformio.ini` — 400 kHz I2C, `DISABLE_RX8130CE_PROBE`, new `LilyGo_TLoRaPager_pager_i2cscan` diagnostic env
  - `examples/i2c_scan/` — new on-device I2C diagnostic app (untracked)

## What was found

### 1. I2C reads only work at 400 kHz on this board (fixed)
On-screen A/B test (diagnostic app + webcam capture): at 100 kHz the TCA8418 and
PCF85063 both ACK writes but NAK every read; at 400 kHz everything works.
`board.begin()` used the 100 kHz default, so every I2C read in the pager
firmware failed. Fix: `PIN_BOARD_I2C_FREQ=400000` on the pager env.

### 2. RTC auto-discovery clobbered the keyboard (fixed)
`AutoDiscoverRTCClock` probes 0x32 as RX8130CE — that address is the TCA8418
keyboard. It misdetected the keyboard as an RTC, and the RX8130CE init sequence
writes `KP_GPIO_2`/`KP_GPIO_3` = 0x00 (key matrix dies) plus disables GPI
interrupts. Proven on screen: registers correct after `keyboard.begin()`,
dead after the fake RX8130 init. Since `rtc_clock.begin()` runs in
`radio_init()` (after `keyboard.begin()`), the matrix stayed dead.
Fix: `DISABLE_RX8130CE_PROBE` on the pager env. The real RTC on this board is
the PCF85063 at 0x51 (detected fine by the existing PCF8563 probe).

### 3. TCA8418 keyboard chip is DEAD on this unit (hardware, not fixable here)
Timeline (device under test, USB serial 50:78:7D:2D:31:D8):
- 17:36: fully alive — ID reg reads 0x18, all registers readable, 400 kHz.
- 17:42: degraded — writes ACK, reads NAK @100k / OK @400k.
- ~17:47 onward: NAKs everything (writes and reads), at every clock speed.
- Persists through full power cycles (power-button shutdown cuts USB for ~32 s;
  the pager has an internal battery that also kept it alive when the USB cable
  was unplugged). XL9555 gates confirmed on (KB_EN already high by default) —
  gating is not the cause.
Conclusion: the TCA8418 (or its local rail / pull-ups) is permanently damaged.
Key input cannot be verified on this unit; a working unit or a reflowed
TCA8418 is needed to finish that verification.

### 4. I2C bus map (in power-button-boot power state)
ACKers: 0x18 (ES8311 codec), 0x20 (XL9555 expander), 0x28 (?), 0x34 (?),
0x51 (PCF85063 RTC), 0x55 (BQ27220 gauge — present! was unpowered in USB-only
state), 0x5A (?), 0x6B (BQ25896 charger). 0x32 (keyboard) dead.
Note: in USB-only power state only 0x20/0x51(+gates-off peripherals) appear —
most peripherals sit on rails that come up with the battery/power-button boot
sequence. The XL9555 gates are now initialized in `TLoraPagerBoard::begin()`
mirroring meshtastic's `earlyInitVariant`.

## Working diagnostic setup

- Diagnostic app: `examples/i2c_scan` + env `LilyGo_TLoRaPager_pager_i2cscan`
  (build+flash: `pio run -e LilyGo_TLoRaPager_pager_i2cscan -t upload --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_50:78:7D:2D:31:D8-if00`).
  Results via `log_e` (see console quirk below) or drawn on the ST7796.
- Webcam: LifeCam VX-800 at `/dev/video0`;
  `ffmpeg -f v4l2 -video_size 640x480 -i /dev/video0 -frames:v 1 /tmp/pager_screen.png`.
- Console quirk (unresolved): app `Serial` output was NOT visible over
  /dev/ttyACM0 in CDC-on-boot builds, while core `log_e` output WAS. Use
  `log_e` for diagnostics (filter at ERROR level: env uses `CORE_DEBUG_LEVEL=1`).
- Host USB autosuspend disabled during session:
  `echo -1 | sudo tee /sys/module/usbcore/parameters/autosuspend`
  (re-apply after host reboots; the pager otherwise drops out of the host).
- Reset the pager from the host: DTR/RTS toggle on the by-id port works.
- `/dev/ttyUSB0` (CP2102) is a different device — do not use.

## What to test next (resume plan)

When the device battery has drained / a working unit is available:

1. **Power-state check first**: hold the power button until off, boot again,
   capture serial. Confirm the boot log is clean (no `i2cRead returned Error`),
   and check which I2C devices ACK in this power state (run the i2cscan app or
   read the `0x32`/`0x51`/`0x55` lines). On the dead unit, keyboard was alive
   in USB-only state but dead after battery boot — on a healthy unit it should
   ACK at 0x32 in every state, and `keyboard.begin()` should NOT print
   "no keyboard at 0x32".
2. **Key input end-to-end** (needs live TCA8418): boot the debug build, then
   - Inbox → user button back → Channels → Add channel → type into the name
     field with the keyboard; verify characters on screen via webcam.
   - Test multi-tap (q → q q → 'q','Q','1'), TAB symbol layer, ENTER, SPACE,
     back-to-BSP/ESC, backlight toggle (triple space).
   - Confirm the keyboard backlight LEDC (GPIO 46) cycles 0→40→127→0.
3. **Battery gauge**: `board.getBattPercent()` / `getBattMilliVolts()` —
   gauge is at 0x55 (BQ27220). Check the battery icon/percent in the UI and
   that no gauge I2C errors appear when the rail is unpowered.
4. **LEDC warning** (`E (897) ledc: LEDC is not initialized`): display
   backlight via LovyanGFX `Light_PWM` (pin 42, channel 7) writes before LEDC
   is ready, and/or keyboard backlight `ledcSetup(3)`. Check the display
   backlight path in `TLoraPagerDisplay::begin()` ordering.
5. **Two-node round-trip**: flash a second pager or the T-Deck companion
   (`LilyGo_TDeck_companion_radio_ble`), same PSK/channel, send a direct
   message pager→pager and verify inbox delivery, ack, and wake-flash behavior.
   Also exercises the shared SPI (SX1262 + ST7796) under radio traffic.
6. **Screen timeout / sleep**: default (non-debug) build — verify display off
   after timeout, wake on message, and that `rst` reasons stay healthy.

## Environment / machine notes

- Repo: /home/rkansy/MeshCore (remote origin = github.com/rkansy/MeshCore).
- Meshtastic reference source (working pager variant): /home/rkansy/firmware
  — see `variants/esp32s3/tlora-pager/` (variant.h has the full pin/gate map),
  `src/input/TLoraPagerKeyboard.cpp`, `src/Pca9555.h` (XL9555 driver),
  `src/platform/extra_variants/t_lora_pager/`.
- Flash with esptool via the by-id port (no OpenOCD needed):
  `pio run -e <env> -t upload --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_50:78:7D:2D:31:D8-if00`
- Verify what is actually flashed by reading back
  `esptool.py read_flash 0x10000 ...` and comparing md5 (the S3 persists
  across USB unplug when the battery is charged, so "Saved PC" values in
  boot logs are not a reliable freshness signal).

## Verification status

- `pio test -e native -e native_kiss_modem`: 54 passed.
- `pio run` pager / pager_debug / companion: all pass.
- Pager debug build booted clean: no I2C errors, keyboard driver prints
  "no keyboard at 0x32, continuing without it" and backs off.
- Still unverified: real key input (dead chip on this unit), radio/display SPI
  sharing, two-node message round-trip, battery-gauge readings (gauge now
  known present at 0x55 — easy next test), LEDC warnings (display backlight
  path, `E (897) ledc: LEDC is not initialized` remains).
