# AD35-S3 (AIHA) Board — Minimal Documentation

- Board: `ad35_s3`
- MCU: ESP32-S3
- LCD: ST7796, 480x320, RGB565, I80 parallel
- I2C: SDA=6, SCL=5
- AW9523: 0x59 (LCD RST=P14, EN/LED=P8-P11, ES8311_EN=P13)
- LCD I80: DC=45, WR=10, D0-D7=9,4,3,8,18,17,16,15
- Firmware: `merged-0.1.2.bin` (from `.github/workflows/build-ad35-s3.yml`, FIRMWARE_VERSION=0.1.2)
- Skip WiFi/NTP: `board_post_init_diagnostics()` in `setup_device.c` logs LCD info; full LCD init uses `display_lcd` custom device
