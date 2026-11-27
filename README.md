# esphome-rpi-7inch-display

## Purpose
The Raspberry Pi 7" Touch Display v1 (800x480, TC358762 DSI bridge, ATTINY88 power / backlight, FT5x06 touch) on
the ESP32-P4. The display model `RPI-7INCH-V1` lives in `mipi_dsi` of
[esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display), the owner of `mipi_dsi`, `ft5x06` and `i2c`;
this repository shows how to use it.

## How it works
`mipi_dsi` drives the panel through the TC358762 bridge with generic DSI commands; the model's init sequence also
writes the ATTINY88 (I2C 0x45) over I2C and so powers the display up: no `on_boot` lambda, the picture comes up after
a power cycle, a reset and an OTA restart alike. `ft5x06` reads the touch (I2C 0x38). The backlight is ATTINY88
register 0x86 (0-255). The DSI PHY is supplied by `esp_ldo` channel 3.

## Quick start
Chips: ESP32-P4

Needs: [esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display),
[esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers) (`esp_ldo`)

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-mipi-display@main
    components: [display, mipi, mipi_dsi, i2c, ft5x06, touchscreen]
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [esp_ldo]

esp_ldo:
  - channel: 3
    voltage: 2.5V

i2c:
  sda: GPIO7
  scl: GPIO8

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
    show_test_card: true

touchscreen:
  - platform: ft5x06
    address: 0x38
```

Use your board's pins (the I2C bus of the display connector). Full example:
[example/rpi-7inch-v1.yaml](example/rpi-7inch-v1.yaml) (tested with the Guition JC-ESP32P4-M3-Dev).

## Main options
| Option | What it does | Default |
|---|---|---|
| `model: RPI-7INCH-V1` (`mipi_dsi`) | the display model: init sequence, bridge, ATTINY88 power-up | - |
| `i2c_id` (display) | the I2C bus of the display connector when there is more than one | the only bus |
| [`transform`](#orientation) (display and touch) | `mirror_x` / `mirror_y` in hardware; both = 180° | off |
| `rotation` (display) | 90 / 270 degrees, in software | 0 |
| `video_mode` (`mipi_dsi`) | `burst`, `non_burst_sync_pulses`, `non_burst_sync_events` | model's |
| `address` (`ft5x06`) | the touch controller's I2C address | `0x38` |

## Catalog
- Components: none of its own. The model `RPI-7INCH-V1` is `models/raspberrypi.py` in the
  [mipi_dsi folder](https://github.com/p1ngb4ck/esphome-rpi-7inch-display/tree/main/esphome/components/mipi_dsi)
  (this repo's main; owner of `mipi_dsi`, `ft5x06`, `i2c`, `display`, `touchscreen`:
  [esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display)). No CONFIGURATION.md.
- Branches:
  - [`main`](https://github.com/p1ngb4ck/esphome-rpi-7inch-display/tree/main): the whole topic
  - [`pr/mipi-dsi-video-mode`](https://github.com/p1ngb4ck/esphome-rpi-7inch-display/tree/pr/mipi-dsi-video-mode)
  - 8 `dependabot/*` branches: not topic branches
- Needed repos: [esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display) (display, mipi, mipi_dsi,
  i2c, ft5x06, touchscreen), [esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers) (esp_ldo)
- All topics together: `github://p1ngb4ck/esphome@dev`

---

# Detailed version

## Raspberry Pi 7" Touch Display v1 on ESP32-P4: DSI display (800x480, TC358762 bridge) and touch

Base: ESPHome `dev` at `a5a75738ad`. The changes this display needs are branches of the owner repo
[esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display); this repository's `main` carries the same
state of `mipi`, `mipi_dsi` and `ft5x06`. The display powers up through its ATTINY88 (I2C 0x45) from the
model's init sequence, once the DSI lanes are up: no `on_boot` lambda, the picture comes up after a power cycle, a
reset and an OTA restart alike. Each change as its own pull request branch (of esphome-mipi-display):

- [pr/ft5x06-vendor-ids][ft5x06-vendor-ids]: ft5x06 accepts vendor ID 0x79 and treats an invalid touch count as no
  touch.
- [pr/mipi-dsi-link-options][link-options]: mipi_dsi options `frame_ack`, `continuous_clock`, `eotp`,
  `lp_horizontal_blanking`, `lp_vertical_active`.
- [pr/mipi-dsi-generic-commands][generic-commands]: mipi_dsi `command_mode: generic` for bridge chips (generic
  writes, no acknowledge, no DCS soft reset or added DCS commands).
- [pr/mipi-i2c-init-writes][i2c-init-writes]: I2C register writes (`i2c_write`) as init sequence entries, display
  option `i2c_id`.
- [pr/mipi-dsi-rpi-7inch-v1][rpi-7inch-v1]: the model `RPI-7INCH-V1` (needs the three mipi PRs above).
- `main` also: with `command_mode: generic` the init sequence goes out before `esp_lcd_panel_init()` starts the
  video, the bridge's register writes in command mode.
- [pr/mipi-dsi-video-mode][video-mode] (this repository, also in `main`): mipi_dsi `video_mode` (`burst`,
  `non_burst_sync_pulses`, `non_burst_sync_events`), `RPI-7INCH-V1` with `non_burst_sync_pulses`.

[ft5x06-vendor-ids]: https://github.com/p1ngb4ck/esphome-mipi-display/tree/pr/ft5x06-vendor-ids
[link-options]: https://github.com/p1ngb4ck/esphome-mipi-display/tree/pr/mipi-dsi-link-options
[generic-commands]: https://github.com/p1ngb4ck/esphome-mipi-display/tree/pr/mipi-dsi-generic-commands
[i2c-init-writes]: https://github.com/p1ngb4ck/esphome-mipi-display/tree/pr/mipi-i2c-init-writes
[rpi-7inch-v1]: https://github.com/p1ngb4ck/esphome-mipi-display/tree/pr/mipi-dsi-rpi-7inch-v1
[video-mode]: https://github.com/p1ngb4ck/esphome-rpi-7inch-display/tree/pr/mipi-dsi-video-mode

## Status

- Init: complete, after a cold start, a restart and a flash (OTA) alike
- Touch: complete
- Backlight: complete (ATTINY88 register 0x86, 0-255)
- Checks: upstream's CI jobs run locally (not on GitHub) and an own review; no open findings

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-mipi-display@main
    components: [display, mipi, mipi_dsi, i2c, ft5x06, touchscreen]
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [esp_ldo]

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1

touchscreen:
  - platform: ft5x06
    address: 0x38
```

`i2c_id` on the display selects the I2C bus of the display connector when there is more than one. The backlight is
ATTINY88 register 0x86 (0-255), see the example.

## Orientation

The display mirrors its picture in hardware through the scan direction of its panel (set by the ATTINY88 at
start-up), with ESPHome's `transform` option:

| `mirror_x` | `mirror_y` | Picture |
|---|---|---|
| `false` | `false` | normal (default) |
| `true` | `false` | mirrored left / right |
| `false` | `true` | mirrored top / bottom |
| `true` | `true` | rotated 180 degrees |

Rotated 180 degrees:

```yaml
display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
    transform:
      mirror_x: true
      mirror_y: true

touchscreen:
  - platform: ft5x06
    address: 0x38
    transform:
      mirror_x: true
      mirror_y: true
```

The touch controller is not affected by the panel's scan direction: give the touchscreen the same `transform` as the
display. 90 and 270 degrees are not possible in hardware; `rotation` on the display (or in LVGL) rotates in
software.

## Example

[example/rpi-7inch-v1.yaml](example/rpi-7inch-v1.yaml), tested with the Guition JC-ESP32P4-M3-Dev; should work on
any ESP32-P4 board with a DSI connector. Set for your board before use:

- `esp32:` `board:`, and `engineering_sample: true` for ESP32-P4 chip revisions below v3 (boot log:
  `chip revision: vX.Y`)
- `i2c:` your board's pins on the display connector
- `esp32_hosted:` your board's pins (`reset_pin`, `cmd_pin`, `clk_pin`, `d0_pin`-`d3_pin`, `active_high`)

```yaml
# Raspberry Pi 7" Touch Display v1 on an ESP32-P4: minimal config, ESPHome defaults, the display parts from
# esphome-mipi-display. Shows the display's test card. Tested with the Guition JC-ESP32P4-M3-Dev; should work on
# any ESP32-P4 board with a DSI connector (set the board-specific options, see README).

esphome:
  name: esp32-p4-rpi7-generic
  friendly_name: ESP32-P4-RPi7-Generic

esp32:
  variant: ESP32P4
  flash_size: 16MB
  framework:
    type: esp-idf

external_components:
  - source: github://p1ngb4ck/esphome-mipi-display@main
    components:
      - display
      - mipi      # changed upstream component: i2c_write in init sequences
      - mipi_dsi  # changed upstream component: link options, generic commands, I2C init writes, model RPI-7INCH-V1
      - i2c
      - ft5x06    # changed upstream component: vendor ID 0x79, 0xFF touch count
      - touchscreen
    refresh: always
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [esp_ldo]

psram:
  mode: hex
  speed: 200MHz

# MIPI DSI PHY supply
esp_ldo:
  - channel: 3
    voltage: 2.5V

# Wi-Fi through the ESP32-C6: add your board's pins (see README)
esp32_hosted:
  variant: ESP32C6

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret esphome_encryption_key

ota:
  - platform: esphome

logger:

i2c:  # your board's I2C pins on the display connector
  sda: GPIO7
  scl: GPIO8

i2c_device:
  - id: attiny
    address: 0x45

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
    transform:
      mirror_x: true
      mirror_y: true
    show_test_card: true

touchscreen:
  - platform: ft5x06
    address: 0x38
    transform:
      mirror_x: true
      mirror_y: true

output:
  - platform: template
    id: backlight_output
    type: float
    write_action:
      - lambda: id(attiny).write_byte(0x86, static_cast<uint8_t>(state * 255.0f));  # PWM

light:
  - platform: monochromatic
    name: Backlight
    output: backlight_output
    restore_mode: ALWAYS_ON
```

## License

ESPHome's files: [LICENSE](LICENSE). The additions of this branch (the changes to `mipi`, `mipi_dsi` and `ft5x06`,
the model file `esphome/components/mipi_dsi/models/raspberrypi.py`, the example and this README):
[LICENSE-MIT](LICENSE-MIT).

## Branch structure

Each `pr/` branch holds one part of this topic on top of upstream and says in its `BRANCH.md` what it is based on.
`main` holds all of them.

| Branch | Based on |
|---|---|
| `pr/mipi-dsi-video-mode` | `pr/ft5x06-vendor-ids` of p1ngb4ck/esphome-mipi-display |
