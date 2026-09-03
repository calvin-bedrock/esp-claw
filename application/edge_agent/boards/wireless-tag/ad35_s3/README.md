# AD35-S3 Board Support

This directory contains the board support files for the AIHA AD35-S3 ESP32-S3 development board in ESP-Claw.

## Board Information

- **Board**: AD35-S3
- **MCU**: ESP32-S3
- **Manufacturer**: AIHA (note: the existing wireless-tag directory is used for historical reasons)
- **Description**: AD35-S3 ESP32-S3 with ST7796 I80 LCD

## LCD Specification

- **Controller**: ST7796
- **Interface**: 8-bit I80 / 8080 parallel
- **Resolution**: 480 x 320
- **Pixel Format**: RGB565

## Pinout

### I2C
- SDA = GPIO6
- SCL = GPIO5

### AW9523 GPIO Expander
- I2C Address = 0x59

### LCD I80 Signals
- DC = GPIO45
- WR = GPIO10
- D0 = GPIO9
- D1 = GPIO4
- D2 = GPIO3
- D3 = GPIO8
- D4 = GPIO18
- D5 = GPIO17
- D6 = GPIO16
- D7 = GPIO15

### AW9523 LCD Control Pins (Verified)
- LCD Backlight/Enable: P8, P9, P10, P11 (active-low)
- LCD Reset: P14 (active-low)
- ES8311 Audio Codec Enable: P13 (active-high)

## SDK Configuration

See `sdkconfig.defaults.board` for board-specific default configuration values.

## Build Instructions

The AD35-S3 board is built using the standard ESP-Claw build process:

```bash
cd application/edge_agent
idf.py bmgr -c ./boards/wireless-tag -b ad35_s3
idf.py build
```

The build produces firmware artifacts that can be flashed using the existing ESP-Claw Web Flasher.

## Web Flasher Integration

AD35-S3 is automatically discoverable through the existing ESP-Claw Web Flasher workflow:
1. Open ESP-Claw Web Flasher
2. Select AD35-S3 from the board list
3. Connect USB
4. Install/Flash
5. ESP-Claw starts with LCD initialized

## Hardware Validation

This implementation follows the layered validation strategy:
1. ESP32-S3 boots normally
2. I2C bus works
3. AW9523 detected at 0x59
4. AW9523 LCD control pins configured correctly
5. LCD backlight/power works
6. LCD reset sequence works
7. ST7796 initialization succeeds
8. I80 bus works
9. LCD displays basic colors (BLACK, WHITE, RED, GREEN, BLUE)
10. LCD can display text
11. ESP-Claw UI renders on the LCD

## Notes

- This implementation reuses existing ESP-Claw display infrastructure
- No unnecessary generic code changes were made
- The board uses the existing Board Manager/device architecture
- Setup device code is required for AW9523 initialization which cannot be fully expressed through YAML alone