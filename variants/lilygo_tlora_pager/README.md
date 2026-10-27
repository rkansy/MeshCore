# LilyGO T-LoRa Pager (bring-up variant)

Bring-up variant for the MeshCore T-LoRa Pager work (MULT-194 / MULT-197, Stage 2).
It wires up the SX1262 radio, ST7796 display, TCA8418 keyboard, rotary encoder,
user button and BQ27220 battery gauge, and ships a minimal boot test firmware
(`examples/tlora_pager_boot`) that proves each input works.

This is **not** the pager chat UI — that is Stage 3 (MULT-198).

## Environments

- `LilyGo_TLoRaPager` — base variant (board, radio pins, display, keyboard,
  battery). Not a standalone build on its own; the firmware envs extend it.
- `LilyGo_TLoRaPager_pager` — the bring-up / boot-test firmware.

## Build

```sh
pio run -e LilyGo_TLoRaPager_pager
```

## Flash (esptool over the CP210x UART)

The T-LoRa Pager exposes a CP210x USB-UART bridge (a `/dev/ttyUSB*` port on
Linux/macOS) wired to the ESP32-S3's UART0. Flash over that port with esptool:

```sh
# hold the BOOT button down, plug in USB (or press BOOT while connected) to
# enter the ROM bootloader, then:
pio run -e LilyGo_TLoRaPager_pager -t upload --upload-port /dev/ttyUSB0
```

- Upload speed: 921600 (board default). If the link is unstable, retry with
  `--upload-speed 115200`.
- To enter bootloader mode: hold **BOOT** (GPIO0) low while pressing **EN**
  (reset), then release. The CP210x port will appear / the device resets into
  the ROM bootloader.
- The console (Serial) is on UART0 via the CP210x; this variant builds with
  `ARDUINO_USB_CDC_ON_BOOT=0` so `Serial` maps to the CP210x UART rather than
  the S3's native USB. Open the same `/dev/ttyUSB*` port at 115200 for logs.

## Pin map (from the Stage-1 hardware audit, MULT-195)

| Function | Pin |
|----------|-----|
| SX1262 CS / DIO1(IRQ) / RST / BUSY | 36 / 14 / 47 / 48 |
| SPI SCK / MOSI / MISO (shared with display) | 35 / 34 / 33 |
| ST7796 CS / DC / backlight | 38 / 37 / 42 |
| I2C SDA / SCL | 3 / 2 |
| TCA8418 keyboard INT / backlight | 6 / 46 |
| Rotary A / B / press | 40 / 41 / 7 |
| User button | 0 |
| GPS RX / TX / PPS (compile-time optional) | 4 / 12 / 13 |

## Notes / open items

- **Battery:** read from the BQ27220 fuel gauge (I2C 0x55) via the
  `mverch67/BQ27220` library. The gauge read has **not** been verified on a
  real device yet (on-device testing is the parked Stage-5 task). If the gauge
  is absent or uncalibrated, the boot screen shows `Battery: --`.
- **Shared SPI:** the SX1262 and ST7796 share the SPI host/bus (SCK 35, MOSI
  34, MISO 33) with separate CS (36 / 38), matching meshtastic's reference
  design. This is expected to be fine but has not been exercised on hardware.
- **PSRAM:** the board definition uses quad (qspi) PSRAM + 16MB flash so that
  GPIO33 (LoRa MISO) stays free, matching meshtastic's `t-deck-pro` board.
- **GPS:** defined (pins 4/12/13) but disabled for the bring-up stub
  (`ENV_INCLUDE_GPS=0`); enable later if needed.
