# AIHA AD35-S3

ESP32-S3 board profile for the AD35-S3 with a 480x320 ST7796 LCD on an
8-bit Intel 8080 bus.

## Build

```bash
cd application/edge_agent
idf.py gen-bmgr-config -c ./boards/aiha -b ad35_s3
idf.py build
```

The CI workflow packages `merged-0.1.2.bin`, generated from the build's own
`flash_args`, for flashing at address `0x0`.

## Current scope

- I2C master: SDA GPIO6, SCL GPIO5
- AW9523: 7-bit address `0x59`
- ST7796 I80 LCD: 480x320
- LCD data pins D0-D7: GPIO9, 4, 3, 8, 18, 17, 16, 15
- LCD WR: GPIO10; DC: GPIO45
- LCD reset and backlight control: AW9523

Physical display behavior must be verified on the target board after the CI
build succeeds.
