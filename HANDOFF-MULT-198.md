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

### 3. TCA8418 keyboard is at 0x34, NOT 0x32 — the chip was never dead (CORRECTED Oct 27)
The "dead chip" conclusion below was wrong: every test was probing the wrong
address (0x32, which does not exist on this bus). Historical timeline from the
first session, kept for reference — it all re-interprets as 0x32 NAKs:

**How it was found:** Meshtastic 2.8.2 (env `tlora-pager` from
/home/rkansy/firmware) was flashed as a cross-check. Its boot scan found 8
devices and identified **"TCA8418 found at address 0x34"** (it reads reg 0x90
== 0x0); the user confirmed live key input under Meshtastic. The 0x34 "(?)"
in the bus map (#4) WAS the keyboard all along.

Facts established Oct 27 (i2cscan app register dump of 0x34):
- Chip responds to reads at 100 kHz and 400 kHz; key events latch in its FIFO
  (0x04–0x09 showed 0x80 press bits from user keypresses) — matrix, chip and
  rail all healthy.
- Chip ID register 0xE7 reads **0x00** on this unit, not the datasheet 0x18 —
  a presence probe must NOT require the ID match.
- The "400 kHz only" finding (#1) is now doubtful: Meshtastic runs the whole
  bus at 100 kHz and reads the keyboard, RTC, gauge and IMU fine. The 100-kHz
  NAKs were likely the non-existent 0x32 all along. Re-test at 100 kHz later;
  `PIN_BOARD_I2C_FREQ=400000` is kept for now (harmless).

MeshCore fixes (this session, uncommitted):
- `KB_I2C_ADDR=0x34` in `variants/lilygo_tlora_pager/platformio.ini`.
- `TCA8418Keyboard::probePresent()`: logs reg0 + chip_id via `log_e`, accepts
  on bus presence (no ID requirement).
- `TCA8418Keyboard::update()`: 50 ms key-FIFO poll fallback when no INT edge
  is pending (Meshtastic polls for the same reason; INT wiring is unverified).
- Per-key `log_e` (`TCA8418 key: layer=.. char=.. key=..`) in `released()`.
- Verified end-to-end: probe passes at boot, user keypresses changed the pager
  UI (screen advanced to "NO CHANNEL / Press 4").

Also (host reset quirk, valid): DTR pulse alone = clean chip reset; DTR+RTS
high together pulls the S3 into download mode.

Meshtastic flash recipe that worked on walien: `pio run -e tlora-pager` in
/home/rkansy/firmware, then esptool `write_flash 0x0 bootloader.bin
0x8000 partitions.bin 0x10000 firmware-*.bin 0xc90000 littlefs-*.bin`
(esptool v5.6.0 installed into the pio uv venv; the pioarduino platform install
needed a local patch to `platformio/platform/base.py::ensure_engine_compatible`
— the core's "6.2.0-post.1" self-version sorts *below* 6.2.0 under semver
prerelease rules and failed the platform's `>=6.2.0` engine check).

### 4. I2C bus map (in power-button-boot power state)
ACKers: 0x18 (ES8311 codec), 0x20 (XL9555 expander), 0x28 (BHI260AP IMU),
0x34 (TCA8418 keyboard — see #3), 0x51 (PCF85063 RTC), 0x55 (BQ27220 gauge —
present! was unpowered in USB-only state), 0x5A (DRV2605 haptics),
0x6B (BQ25896 charger). 0x32 does not exist on this bus (nothing ACKs it).
Identity cross-check: Meshtastic's scan named every device except 0x34,
which it ID-matched as TCA8418.
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

Keyboard input is verified working (see #3). Remaining, in order:

1. **Power-state re-check after charging**: battery-boot state I2C map
   (gauge 0x55 should appear), boot log clean, probe at 0x34 passes.
2. **Key input end-to-end** (first keypress done — screen changed): typing
   into the Add-channel name field (verify on screen via webcam), multi-tap
   (q → q q → 'q','Q','1'), TAB symbol layer, ENTER, SPACE, BSP/ESC,
   backlight toggle (triple space), LEDC backlight cycle (GPIO 46).
   Per-key `log_e` lines make each event verifiable in the serial capture.
3. **I2C at 100 kHz re-test**: Meshtastic ran the whole bus at 100 kHz fine;
   if our firmware is also clean at 100 kHz, drop `PIN_BOARD_I2C_FREQ=400000`.
4. **Battery gauge**: `board.getBattPercent()` / `getBattMilliVolts()` —
   gauge is at 0x55 (BQ27220). Check the battery icon/percent in the UI and
   that no gauge I2C errors appear when the rail is unpowered. (Meshtastic
   needed one init retry, then fell back to BQ25896.)
5. **LEDC warning** (`E (897) ledc: LEDC is not initialized`): display
   backlight via LovyanGFX `Light_PWM` (pin 42, channel 7) writes before LEDC
   is ready, and/or keyboard backlight `ledcSetup(3)`. Check the display
   backlight path in `TLoraPagerDisplay::begin()` ordering.
6. **Two-node round-trip**: flash a second pager or the T-Deck companion
   (`LilyGo_TDeck_companion_radio_ble`), same PSK/channel, send a direct
   message pager→pager and verify inbox delivery, ack, and wake-flash behavior.
   Also exercises the shared SPI (SX1262 + ST7796) under radio traffic.
7. **Screen timeout / sleep**: default (non-debug) build — verify display off
   after timeout, wake on message, and that `rst` reasons stay healthy.

## Product / compliance notes (from user, Oct 27)

- **RF compliance / region setup**: the user must be able to configure the
  right radio channel/frequency for their location before the device is
  usable, and ship-time defaults must be legal everywhere (no illegal-band
  default). Needs: region/band selection (e.g. 869 EU / 915 US / 433 /
  regional LM), a first-run "set your region" gate, and a sanity warning when
  the configured frequency is outside the selected region's plan.
- **UI: menus over numbers**: numeric entry needs multi-tap (third tap of a
  letter key) and is awkward; prefer navigable menus/selection lists and keep
  on-screen "Press N" number hints to a minimum.

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
- Keyboard: PROBE + KEY INPUT VERIFIED at 0x34 (user keypress advanced the
  pager UI; shift-modifier key also tested). Remaining key-matrix details in
  resume plan #2.
- Still unverified: radio/display SPI sharing, two-node message round-trip,
  battery-gauge readings, LEDC warnings (display backlight path, `E (897)
  ledc: LEDC is not initialized` remains), I2C at 100 kHz re-test.
