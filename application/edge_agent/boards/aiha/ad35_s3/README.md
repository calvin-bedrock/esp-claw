# AIHA AD35-S3

ESP32-S3 board profile for the AD35-S3 (启明云端 WP01N16R8WBDP3D5T1, ESP32-S3-WROOM-1,
16 MB flash / 8 MB PSRAM) with a 480x320 ST7796 LCD on an 8-bit Intel 8080 bus.

## Build

```bash
cd application/edge_agent
python -m pip install esp-bmgr-assist==0.8.3
IDF_TARGET=esp32s3 idf.py bmgr -c ./boards/aiha -b ad35_s3
idf.py build
```

CI packages `merged-<FIRMWARE_VERSION>.bin` from the build's own `flash_args`,
to be flashed at address `0x0`.

## Status

**Display bring-up verified on hardware.** The panel renders text and colour
correctly at 480x320. Board boots, initializes the AW9523, resets the ST7796,
and draws a boot splash showing the board name and firmware version.

## Verified pinout

**Source of truth: the board schematic `AD35-S3.kicad_sch`.** Every value below
was additionally confirmed on real hardware with a standalone Arduino_GFX build
before being adopted here.

| Signal | Pin |
|---|---|
| I2C SDA | GPIO5 |
| I2C SCL | GPIO4 |
| LCD RS / DC | GPIO45 |
| LCD WR / CLK | GPIO10 |
| LCD DB0..DB7 | GPIO 9, 3, 8, 18, 17, 16, 15, 7 |
| LCD CS | not connected |
| AW9523 address | `0x59` |
| LCD_LEDK (backlight) | AW9523 P8-P11, drive **LOW** to light |
| LCD_RST | AW9523 P14 |

Panel clock runs at 40 MHz, matching the vendor's own Arduino_GFX configuration.

## Do not use the vendor Arduino header

`PINS_AD35-S3.h` from the `Dev_Device_Pins` reference repository is **wrong for
this board** in two ways:

1. It declares I2C as `SDA=6 / SCL=5`. Those pins are dead here — an I2C scan on
   them finds zero devices.
2. Its `D1`..`D7` assignments are shifted by one position relative to the
   schematic.

The failure mode is deceptive and cost several flash cycles to isolate:

- The vendor's `DEV_DEVICE_INIT()` macro **never checks the return value of
  `aw.begin()`**. With the wrong I2C pins the AW9523 is never reached, but
  execution continues regardless.
- Consequently the backlight is never enabled and the `LCD_RST` pulse never
  happens, so the ST7796 is never reset and ignores all traffic.
- The I80 bus is write-only, so `esp_lcd_panel_init()` and every
  `esp_lcd_panel_draw_bitmap()` still return `ESP_OK`. A dead panel and a
  working one are indistinguishable from the driver's return codes alone.

Two rules follow, and they are enforced in `setup_device.c`:

- Always check the return value of any I2C device's `begin`/create call, and log
  a full bus scan, before trusting anything downstream of it.
- AW9523 pins power up as **inputs**. `esp_io_expander_set_level()` does nothing
  useful until `esp_io_expander_set_dir(..., IO_EXPANDER_OUTPUT)` has been
  called for those pins.

Note also that the product datasheet describes the LCD interface as "RGB"; that
is incorrect. It is an 8-bit 8080 parallel bus.
