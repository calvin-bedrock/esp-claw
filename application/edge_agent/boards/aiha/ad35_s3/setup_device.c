/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_aw9523b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gen_board_device_custom.h"

/* Love2D runtime start — declared as extern so the board component doesn't
 * need to depend on lua_module_love2d for the include path. */
extern esp_err_t lua_module_love2d_start(void *display_panel_handle, void *touch_i2c_bus_handle);

#include <math.h>
#include <stdio.h>

static const char *TAG = "ad35_s3";

/* AW9523 pin assignments — confirmed from AD35-S3.kicad_sch.
 *
 * P0_8..P0_11 = LCD backlight LEDK cathodes (sink to illuminate).
 * P0_14       = LCD_RST (active-low, pulsed in display_lcd_init).
 * P1_5        = PA_CTRL → HT6872 speaker amplifier enable (active-high).
 *
 * The vendor header PINS_AD35-S3.h erroneously places I2C on SDA=6/SCL=5
 * and shifts D1..D7 by one.  Do NOT use it for pin assignments. */
#define AW9523_PIN_LCD_LED_0   8
#define AW9523_PIN_LCD_LED_1   9
#define AW9523_PIN_ES_CTRL    10   /* P1_2 — ES8311+ES7210 reset (active-low, drive HIGH to release) */
#define AW9523_PIN_LCD_LED_3  11
#define AW9523_PIN_SENSOR_PWR 12   /* P1_4 — proximity sensor VCC enable */
#define AW9523_PIN_PA_CTRL    13   /* P1_5 — HT6872 amplifier enable */
#define AW9523_PIN_LCD_RESET  14

/* Pin assignments below are taken from the board schematic
 * (AD35-S3.kicad_sch, "8Bit 8080" LCD sheet) and confirmed on hardware with a
 * standalone Arduino_GFX build.
 *
 * Do NOT copy these from the vendor's PINS_AD35-S3.h: that header has D1..D7
 * shifted by one position and lists I2C as SDA=6/SCL=5, which prevents the
 * AW9523 from ever being addressed. With a wrong D-line map every command and
 * pixel byte reaches the ST7796 corrupted, while the write-only I80 bus keeps
 * returning ESP_OK. */
#define LCD_PIN_DC  45
#define LCD_PIN_WR  10
#define LCD_PIN_D0   9
#define LCD_PIN_D1   3
#define LCD_PIN_D2   8
#define LCD_PIN_D3  18
#define LCD_PIN_D4  17
#define LCD_PIN_D5  16
#define LCD_PIN_D6  15
#define LCD_PIN_D7   7

#define LCD_H_RES  480
#define LCD_V_RES  320
/* 40 MHz is the rate the vendor's own Arduino_GFX build runs this ST7796 at,
 * and it was confirmed working on hardware with the schematic pin map. */
#define LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
/* Full-frame transfer cap: 480*320*2 = 307200 bytes.  This lets a single
 * esp_lcd_panel_draw_bitmap(0,0,W,H,fb) call pass through without being
 * split, reducing GDMA descriptor fragmentation. */
#define LCD_MAX_TRANSFER_BYTES (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))

/* Built-in 8x8 ASCII font (public domain).
 * Each glyph is 8 bytes, LSB-first within each row. */
static const uint8_t s_font8x8_basic[128][8] = {
    [0x20] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    [0x2D] = {0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00}, /* - */
    [0x2E] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30}, /* . */
    [0x30] = {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    [0x31] = {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    [0x32] = {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    [0x33] = {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    [0x34] = {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    [0x35] = {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    [0x41] = {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    [0x43] = {0x1E, 0x33, 0x03, 0x03, 0x03, 0x33, 0x1E, 0x00}, /* C */
    [0x44] = {0x1F, 0x33, 0x33, 0x33, 0x33, 0x33, 0x1F, 0x00}, /* D */
    [0x46] = {0x3F, 0x33, 0x03, 0x1F, 0x03, 0x03, 0x03, 0x00}, /* F */
    [0x47] = {0x1E, 0x33, 0x03, 0x03, 0x3B, 0x33, 0x3E, 0x00}, /* G */
    [0x48] = {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    [0x4B] = {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    [0x4C] = {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x3F, 0x00}, /* L */
    [0x4E] = {0x33, 0x37, 0x3F, 0x3B, 0x33, 0x33, 0x33, 0x00}, /* N */
    [0x4F] = {0x1E, 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x00}, /* O */
    [0x50] = {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, /* P */
    [0x53] = {0x1E, 0x33, 0x03, 0x1E, 0x30, 0x33, 0x1E, 0x00}, /* S */
    [0x57] = {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    [0x2A] = {0x00, 0x36, 0x1C, 0x7F, 0x1C, 0x36, 0x00, 0x00}, /* * */
    [0x21] = {0x0C, 0x1E, 0x1E, 0x0C, 0x0C, 0x00, 0x0C, 0x00}, /* ! */
};

static esp_lcd_i80_bus_handle_t s_i80_bus;
static dev_display_lcd_handles_t s_lcd_handles;
static uint8_t s_i2c_diagnostics_handle;

/* LTR-553ALS proximity & light sensor on shared I2C bus (0x39).
 * Not to be confused with 0x38 (FT6236/FT6336 capacitive touch).
 *
 * LTR-553 I2C address is actually 0x23 per official libraries (chenhonglin,
 * lewisxhe SensorLib), but our board has a TMD2772-like chip at 0x39.
 * After hardware cold reset via AW9523 P1_4 it should respond properly. */
#define PROX_ADDR               0x39

/* TMD2772 / APDS-9930 command protocol — bit 7 must be set (CMD). */
#define TMD_CMD_BYTE            0x80
#define TMD_CMD_AUTO_INC        0xA0
#define TMD_CMD_CLEAR_ALL       0xE7
#define TMD_REG_ENABLE          0x00
#define TMD_REG_PTIME           0x02
#define TMD_REG_PPULSE          0x0E
#define TMD_REG_CONTROL         0x0F
#define TMD_REG_ID              0x12
#define TMD_REG_PDATAL          0x18

/* Global proximity state — updated by background task. */
static volatile uint8_t s_latest_proximity_val = 0;
static volatile bool    s_proximity_is_near    = false;
static i2c_master_dev_handle_t s_prox_dev = NULL;

/* Forward declaration for test tone — defined below globals. */
static void play_boot_test_tone(esp_io_expander_handle_t expander);

/* Deferred speaker test tone: ~500ms after display_lcd_init returns so
 * audio_dac is ready.  Uses a work task instead of a FreeRTOS timer:
 * timer callbacks run on Tmr Svc (small stack) and must not call
 * blocking operations or large mallocs — play_boot_test_tone does both. */
static esp_io_expander_handle_t s_tone_expander;

static void tone_work_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    play_boot_test_tone(s_tone_expander);
    s_tone_expander = NULL;
    vTaskDelete(NULL);
}

/* ===== I2C Diagnostics (custom device) ===== */

static int i2c_diagnostics_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "I2C diagnostics device_handle is NULL");

    void *periph_handle = NULL;
    esp_err_t ret = esp_board_periph_ref_handle("i2c_master", &periph_handle);
    ESP_RETURN_ON_FALSE(ret == ESP_OK && periph_handle != NULL,
                        ret == ESP_OK ? ESP_FAIL : ret, TAG,
                        "I2C diagnostics could not get i2c_master");

    ESP_LOGI(TAG, "I2C scan start: SDA=GPIO6 SCL=GPIO5");
    ESP_LOGI(TAG, "I2C idle levels: SDA=%d SCL=%d",
             gpio_get_level(GPIO_NUM_6), gpio_get_level(GPIO_NUM_5));

    unsigned int found = 0;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)periph_handle;
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "I2C ACK at 7-bit address 0x%02x", (unsigned int)addr);
            ++found;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %u device(s)", found);

    esp_board_periph_unref_handle("i2c_master");
    *device_handle = &s_i2c_diagnostics_handle;
    return ESP_OK;
}

static int i2c_diagnostics_deinit(void *device_handle)
{
    (void)device_handle;
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(i2c_diagnostics, i2c_diagnostics_init, i2c_diagnostics_deinit);

/* ===== AW9523 IO-Expander Factory ===== */

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_aw9523b(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AW9523 creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* P0 uses push-pull outputs; both ports must be GPIO rather than LED mode. */
    uint8_t data = 0x10;
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_GCR, &data, 1),
                        TAG, "AW9523 push-pull configuration failed");
    data = 0xFF;
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_LEDMODE0, &data, 1),
                        TAG, "AW9523 P0 GPIO-mode configuration failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_aw9523b_write_reg(
                            *handle_ret, AW9523B_REG_LEDMODE1, &data, 1),
                        TAG, "AW9523 P1 GPIO-mode configuration failed");

    /* Configure PA_CTRL (P1_5) and ES_CTRL (P1_2) as OUTPUT.
     * SENSOR_PWR (P1_4) is NOT configured here — it is powered on demand by
     * ad35_s3_init_proximity() with hardware cold reset (OFF→ON).
     * ES_CTRL must be driven HIGH immediately to release the ES8311/ES7210
     * codecs from hardware reset — without this the I2C init fails with
     * "Fail to write to dev 30/80" and the devices are not usable.
     * The analog output_io_mask/level_mask in board_devices.yaml duplicates
     * this, but being explicit here ensures correct timing. */
    const uint32_t pa_mask   = 1U << AW9523_PIN_PA_CTRL;
    const uint32_t es_mask   = 1U << AW9523_PIN_ES_CTRL;
    uint32_t ctrl_mask = pa_mask | es_mask;

    ret = esp_io_expander_set_dir(*handle_ret, ctrl_mask, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AW9523 control pins set_dir -> %s", esp_err_to_name(ret));
    } else {
        /* PA muted, codec held in reset initially.
         * set_level takes a pin NUMBER (0~15), not a mask. */
        esp_io_expander_set_level(*handle_ret, AW9523_PIN_PA_CTRL, 0);
        esp_io_expander_set_level(*handle_ret, AW9523_PIN_ES_CTRL, 0);
        ESP_LOGI(TAG, "AW9523 PA_CTRL+ES_CTRL set 0");

        /* Release codec reset: ES_CTRL HIGH, 10ms delay per ES8311 datasheet. */
        ret = esp_io_expander_set_level(*handle_ret, AW9523_PIN_ES_CTRL, 1);
        ESP_LOGI(TAG, "AW9523 ES_CTRL(P1_2) HIGH (codec reset released): %s",
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

/* ===== TMD2772 / APDS-9930 Proximity Sensor (0x39) ===== */

/* TMD2772 register helpers. */
static esp_err_t tmd_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {TMD_CMD_BYTE | reg, val};
    return i2c_master_transmit(dev, buf, 2, 50);
}

static esp_err_t tmd_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    uint8_t cmd = TMD_CMD_BYTE | reg;
    esp_err_t ret = i2c_master_transmit(dev, &cmd, 1, 50);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev, val, 1, 50);
    }
    return ret;
}

/* On-demand single proximity read — kept for Lua C-API.
 * Auto-Increment burst read (0xB8), then clear interrupt. */
esp_err_t ad35_s3_read_proximity_once(uint16_t *out_val)
{
    if (!s_prox_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Auto-Increment read: 0xA0 | 0x18 = 0xB8, 2 bytes via ReSTART. */
    uint8_t cmd = TMD_CMD_AUTO_INC | TMD_REG_PDATAL;  /* 0xB8 */
    uint8_t data[2];
    esp_err_t ret = i2c_master_transmit_receive(s_prox_dev, &cmd, 1, data, 2, 50);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_val = ((uint16_t)data[1] << 8) | data[0];

    /* Clear interrupt — prevents state machine lock. */
    uint8_t clr = TMD_CMD_CLEAR_ALL;
    i2c_master_transmit(s_prox_dev, &clr, 1, 50);

    return ESP_OK;
}

/* Background polling task. */
static void proximity_sensor_task(void *pvParameters)
{
    (void)pvParameters;
    int heartbeat_tick = 0;

    while (1) {
        uint16_t raw_prox = 0;
        esp_err_t ret = ad35_s3_read_proximity_once(&raw_prox);

        if (ret == ESP_OK) {
            s_latest_proximity_val = (raw_prox > 255) ? 255 : (uint8_t)raw_prox;

            if (++heartbeat_tick >= 5) {
                heartbeat_tick = 0;
                ESP_LOGI(TAG, "[TMD HEARTBEAT] prox=%u", raw_prox);
            }

            if (!s_proximity_is_near && raw_prox >= 60) {
                s_proximity_is_near = true;
                ESP_LOGI(TAG, ">>> Prox EVENT: NEAR (%u) <<<", raw_prox);
            } else if (s_proximity_is_near && raw_prox < 30) {
                s_proximity_is_near = false;
                ESP_LOGI(TAG, ">>> Prox EVENT: FAR (%u) <<<", raw_prox);
            }
        } else {
            ESP_LOGW(TAG, "TMD read failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* Initialize TMD2772 proximity sensor at 0x39.
 * Step 1: Hardware cold reset via AW9523 P1_4 (OFF 300ms → ON 50ms).
 * Step 2: Verify Chip ID via two-step TMD protocol (0x92).
 * Step 3: Init with minimal sequence, then start background polling. */
static void ad35_s3_init_proximity(esp_io_expander_handle_t expander, i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle || !expander) {
        ESP_LOGW(TAG, "Proximity — no bus or expander");
        return;
    }

    esp_err_t ret;
    uint8_t chip_id = 0;
    const uint32_t pwr_mask = 1U << AW9523_PIN_SENSOR_PWR;

    /* === HARDWARE COLD RESET: P1_4 OFF 300ms → ON 50ms === */
    ESP_LOGI(TAG, "Proximity — hardware cold reset via AW9523 P1_4...");
    ret = esp_io_expander_set_dir(expander, pwr_mask, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Proximity — P1_4 set_dir: %s", esp_err_to_name(ret));
        return;
    }
    esp_io_expander_set_level(expander, AW9523_PIN_SENSOR_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_io_expander_set_level(expander, AW9523_PIN_SENSOR_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Proximity — cold reset complete");

    /* === Attach device at 0x39 === */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PROX_ADDR,
        .scl_speed_hz    = 100000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_prox_dev);
    if (ret != ESP_OK || s_prox_dev == NULL) {
        ESP_LOGW(TAG, "Proximity sensor not found (no ACK at 0x39 after cold reset)");
        return;
    }

    /* === Read Chip ID via TMD two-step (0x80|0x12 = 0x92) ===
     * First read after cold power-on — no stale writes, pure chip response. */
    {
        uint8_t cmd_id = TMD_CMD_BYTE | TMD_REG_ID;  /* 0x92 */
        esp_err_t t_err = i2c_master_transmit(s_prox_dev, &cmd_id, 1, 50);
        esp_err_t r_err = ESP_FAIL;
        if (t_err == ESP_OK) {
            r_err = i2c_master_receive(s_prox_dev, &chip_id, 1, 50);
        }
        ESP_LOGI(TAG, "COLD BOOT READ 0x92: tx=%s, rx=%s, CHIP_ID=0x%02X",
                 esp_err_to_name(t_err), esp_err_to_name(r_err), chip_id);
    }

    /* Accept MK-PB2016PS (ID=0x03) and known TMD variants. */
    if (chip_id != 0x03 && chip_id != 0x0D && chip_id != 0x9C && chip_id != 0xAB && chip_id != 0x39 && chip_id != 0x30) {
        ESP_LOGW(TAG, "Proximity sensor not found (unexpected ID=0x%02X)", chip_id);
        i2c_master_bus_rm_device(s_prox_dev);
        s_prox_dev = NULL;
        return;
    }

    /* === MK-PB2016PS init sequence (external PNP SA1015 IR LED driver) === */
    /* 1. Clear interrupt lock. */
    {
        uint8_t clr = TMD_CMD_CLEAR_ALL;
        i2c_master_transmit(s_prox_dev, &clr, 1, 50);
    }

    /* 2. Configure timing & drive for external PNP transistor. */
    {
        uint8_t buf_ptime[2]  = {TMD_CMD_BYTE | TMD_REG_PTIME,  0xFF};
        uint8_t buf_wtime[2]  = {TMD_CMD_BYTE | 0x03,           0xFF};
        uint8_t buf_ppulse[2] = {TMD_CMD_BYTE | TMD_REG_PPULSE, 0x20};  /* 32 pulses — drive external PNP */
        uint8_t buf_ctrl[2]   = {TMD_CMD_BYTE | TMD_REG_CONTROL, 0x08}; /* PDRIVE=00=100mA LDR sink, PGAIN=4x */
        i2c_master_transmit(s_prox_dev, buf_ptime, 2, 50);
        i2c_master_transmit(s_prox_dev, buf_wtime, 2, 50);
        i2c_master_transmit(s_prox_dev, buf_ppulse, 2, 50);
        i2c_master_transmit(s_prox_dev, buf_ctrl, 2, 50);
    }

    /* 3. Step ENABLE: PON first, wait for oscillator. */
    {
        uint8_t buf_pon[2] = {TMD_CMD_BYTE | TMD_REG_ENABLE, 0x01};
        i2c_master_transmit(s_prox_dev, buf_pon, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* 4. Enable proximity + wait timer: PON+PEN+WEN = 0x0D */
    {
        uint8_t buf_enable[2] = {TMD_CMD_BYTE | TMD_REG_ENABLE, 0x0D};
        i2c_master_transmit(s_prox_dev, buf_enable, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    /* 5. Clear initial interrupt. */
    {
        uint8_t clr = TMD_CMD_CLEAR_ALL;
        i2c_master_transmit(s_prox_dev, &clr, 1, 50);
    }

    ESP_LOGI(TAG, "MK-PB2016PS proximity engine started (PPULSE=32, PDRIVE=100mA)");

    /* Start background polling. */
    xTaskCreate(proximity_sensor_task, "prox", 4096, NULL, tskIDLE_PRIORITY, NULL);
    ESP_LOGI(TAG, "Proximity sensor initialized on 0x39");
}

/* ===== FT6336U Capacitive Touch Controller (0x38) =====
 *
 * The FT6336U sits on the main I2C bus at 7-bit address 0x38.  This boot-time
 * probe verifies the chip ACKs and reads the vendor ID for the Boot HUD.
 * Continuous touch polling is performed by the Love2D runtime event loop. */

#define FT6336U_ADDR            0x38
#define FT6336U_REG_VEND_ID     0xA3

static esp_err_t ft6336u_probe(i2c_master_bus_handle_t bus, uint8_t *vendor_id)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT6336U_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t reg = FT6336U_REG_VEND_ID;
    ret = i2c_master_transmit(dev, &reg, 1, 20);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev, vendor_id, 1, 20);
    }
    i2c_master_bus_rm_device(dev);
    return ret;
}

/* ===== Camera (DVP) Presence Probe =====
 *
 * Scans the SCCB bus for camera sensor addresses (OV2640/OV5640 default to
 * 0x21, 0x30, or 0x3C).  The full DVP init is handled by Lua camera modules. */

#define CAMERA_SCCB_ADDRS_COUNT 3
static const uint16_t s_camera_sccb_addrs[CAMERA_SCCB_ADDRS_COUNT] = {0x21, 0x30, 0x3C};

static bool camera_probe(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return false;
    }
    for (int i = 0; i < CAMERA_SCCB_ADDRS_COUNT; ++i) {
        if (i2c_master_probe(bus, s_camera_sccb_addrs[i], 20) == ESP_OK) {
            ESP_LOGI(TAG, "Camera sensor SCCB ACK at 0x%02X",
                     (unsigned int)s_camera_sccb_addrs[i]);
            return true;
        }
    }
    return false;
}

/* ===== Display LCD (custom device) ===== */

static const dev_display_lcd_config_t s_lcd_config = {
    .name = "display_lcd",
    .chip = "st7796",
    .sub_type = "i80",
    .lcd_width = LCD_H_RES,
    .lcd_height = LCD_V_RES,
    .swap_xy = 1,
    .mirror_x = 0,
    .mirror_y = 0,
    .need_reset = 0,
    .invert_color = 0,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
};

static void cleanup_display_lcd(esp_lcd_panel_handle_t panel_handle,
                                esp_lcd_panel_io_handle_t io_handle)
{
    if (panel_handle != NULL) {
        esp_lcd_panel_del(panel_handle);
    }
    if (io_handle != NULL) {
        esp_lcd_panel_io_del(io_handle);
    }
    if (s_i80_bus != NULL) {
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
    }
}

/* Draw a single 8x8 glyph, scaled by `scale`, into an RGB565 framebuffer. */
static void draw_glyph_rgb565(uint16_t *fb, int fb_w, int fb_h,
                              int x, int y, char ch, int scale,
                              uint16_t color, uint16_t bg, bool with_bg)
{
    if ((uint8_t)ch >= 128) {
        return;
    }
    const uint8_t *glyph = s_font8x8_basic[(int)ch];
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            bool on = (bits >> col) & 0x1;
            if (!on && !with_bg) {
                continue;
            }
            uint16_t px = on ? color : bg;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    int px_x = x + col * scale + dx;
                    int px_y = y + row * scale + dy;
                    if (px_x < 0 || px_y < 0 || px_x >= fb_w || px_y >= fb_h) {
                        continue;
                    }
                    fb[px_y * fb_w + px_x] = px;
                }
            }
        }
    }
}

static void draw_string_rgb565(uint16_t *fb, int fb_w, int fb_h,
                               int x, int y, const char *str, int scale,
                               uint16_t color, uint16_t bg, bool with_bg)
{
    int cursor = x;
    for (const char *p = str; *p; ++p) {
        draw_glyph_rgb565(fb, fb_w, fb_h, cursor, y, *p, scale, color, bg, with_bg);
        cursor += 8 * scale + scale; /* 1-column spacing */
    }
}

/* ===== Boot HUD: Self-Test Status Panel =====
 *
 * Renders a modern minimal system status panel on the ST7796 after all
 * peripheral init is complete.  Accepts self-test booleans for each
 * subsystem so the caller can pass real probe results.
 */

typedef struct {
    bool expander_ok;       /* AW9523 at 0x59 */
    bool backlight_ok;      /* LCD backlight (AW9523 P8-P11) */
    bool touch_ok;          /* FT6336U at 0x38 */
    uint8_t touch_vendor_id;
    bool dac_ok;            /* ES8311 at 0x30 */
    bool adc_ok;            /* ES7210 at 0x40 */
    bool camera_ok;         /* Camera sensor SCCB */
    bool prox_ok;           /* MK-PB2016PS at 0x39 */
} boot_hud_test_t;

static esp_err_t render_boot_hud(esp_lcd_panel_handle_t panel, const boot_hud_test_t *test)
{
    /* After swap_xy + mirror_x the visible resolution is 480 wide x 320 tall. */
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    const size_t fb_bytes = (size_t)W * H * sizeof(uint16_t);

    uint16_t *fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "boot HUD: framebuffer alloc failed (%u bytes)", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }

    /* Colors. */
    const uint16_t BG     = 0x0018; /* deep navy */
    const uint16_t WHITE  = 0xFFFF;
    const uint16_t GREEN  = 0x07E0;
    const uint16_t RED    = 0xF800;
    const uint16_t YELLOW = 0xFFE0;
    const uint16_t CYAN   = 0x07FF;
    const uint16_t GRAY   = 0x8410;

    /* Fill background. */
    for (int i = 0; i < W * H; ++i) {
        fb[i] = BG;
    }

    /* Title bar — scale=3 (24px) */
    draw_string_rgb565(fb, W, H, 16, 12,
        "ESP-CLAW / Love2D Runtime v1.0.0", 3, CYAN, BG, false);

    /* Horizontal separator. */
    for (int x = 0; x < W; ++x) {
        fb[40 * W + x] = GRAY;
    }

    /* Peripherals self-test section — scale=2 (16px), start at y=50 */
    int y = 52;
    draw_string_rgb565(fb, W, H, 16, y, "[PERIPHERALS SELF-TEST]", 2, WHITE, BG, false);
    y += 22;

    /* Helper: draw a status line */
#define HUD_LINE(label, ok_val, addr_str) do { \
        draw_string_rgb565(fb, W, H, 20, y, label, 2, WHITE, BG, false); \
        draw_string_rgb565(fb, W, H, 240, y, \
            (ok_val) ? "[ OK ]" : "[FAIL]", 2, \
            (ok_val) ? GREEN : RED, BG, false); \
        draw_string_rgb565(fb, W, H, 310, y, addr_str, 2, GRAY, BG, false); \
        y += 18; \
    } while (0)

    HUD_LINE("* AW9523 Expander", test->expander_ok, "(0x59)");
    HUD_LINE("* LCD Backlight",   test->backlight_ok, "");
    {
        /* Touch: show vendor ID when detected */
        char buf[20];
        if (test->touch_ok) {
            snprintf(buf, sizeof(buf), "(0x38) ID=0x%02X", test->touch_vendor_id);
        } else {
            snprintf(buf, sizeof(buf), "(0x38) N/A");
        }
        draw_string_rgb565(fb, W, H, 20, y, "* FT6336U Touch", 2, WHITE, BG, false);
        draw_string_rgb565(fb, W, H, 240, y,
            test->touch_ok ? "[ OK ]" : "[FAIL]", 2,
            test->touch_ok ? GREEN : RED, BG, false);
        draw_string_rgb565(fb, W, H, 310, y, buf, 2, GRAY, BG, false);
        y += 18;
    }
    HUD_LINE("* ES8311 DAC Audio", test->dac_ok,  "(0x30)");
    HUD_LINE("* ES7210 Dual-Mic",  test->adc_ok,  "(0x40)");
    {
        draw_string_rgb565(fb, W, H, 20, y, "* Camera Module", 2, WHITE, BG, false);
        draw_string_rgb565(fb, W, H, 240, y,
            test->camera_ok ? "[ DETECTED ]" : "[ NOT FOUND ]", 2,
            test->camera_ok ? GREEN : GRAY, BG, false);
        y += 18;
    }
    HUD_LINE("* Proximity Sensor", test->prox_ok, "(0x39)");

    /* Status section */
    y += 8;
    for (int x = 0; x < W; ++x) {
        fb[y * W + x] = GRAY;
    }
    y += 6;
    draw_string_rgb565(fb, W, H, 16, y,
        "WiFi: Disconnected (Wait for Lua Config)", 2, YELLOW, BG, false);
    y += 20;
    draw_string_rgb565(fb, W, H, 16, y,
        "Loading /spiffs/main.lua ...", 2, CYAN, BG, false);

    /* Push to display. */
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "boot HUD: draw_bitmap failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "boot HUD: rendered %dx%d framebuffer OK", W, H);
    }
    heap_caps_free(fb);
    return ret;
}

#undef HUD_LINE

/* Play two brief 1.2 kHz boot beeps (70 ms each, with a gap) through the
 * speaker to confirm audio works.
 * Must be called after audio_dac device has been initialized by Board Manager.
 * Caller holds the AW9523 expander handle so we can toggle PA_CTRL here. */
static void play_boot_test_tone(esp_io_expander_handle_t expander)
{
    const uint32_t pa_mask = 1U << AW9523_PIN_PA_CTRL;

    dev_audio_codec_handles_t *dac = NULL;
    if (esp_board_device_get_handle("audio_dac", (void **)&dac) != ESP_OK ||
        dac == NULL || dac->codec_dev == NULL) {
        ESP_LOGW(TAG, "boot beep: audio_dac handle not available");
        return;
    }

    /* Open the codec device with PCM format matching the I2S peripheral
     * config (48000 Hz, 16-bit, stereo from board_peripherals.yaml).
     * Without esp_codec_dev_open(), esp_codec_dev_write() returns
     * ESP_ERR_INVALID_STATE because the driver has no active channel. */
    const int sample_rate = 48000;
    const int duration_ms = 70;
    const int freq_hz = 1200;
    const int gap_ms = 70;
    const int nsamples = sample_rate * duration_ms / 1000;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate,
    };
    esp_err_t ret = esp_codec_dev_open(dac->codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "boot beep: codec_dev_open failed: %s", esp_err_to_name(ret));
        return;
    }
    esp_codec_dev_set_out_vol(dac->codec_dev, 75.0);

    /* Enable amplifier. */
    esp_err_t pa_ret = esp_io_expander_set_level(expander, pa_mask, 1);
    ESP_LOGI(TAG, "PA_CTRL HIGH (amplifier on): %s", esp_err_to_name(pa_ret));
    vTaskDelay(pdMS_TO_TICKS(30));

    /* Generate 70 ms of 1.2 kHz sine wave, 16-bit interleaved stereo. */
    int16_t *buf = heap_caps_malloc((size_t)nsamples * 2 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "boot beep: buffer alloc failed (%d samples)", nsamples);
        goto close;
    }
    for (int i = 0; i < nsamples; ++i) {
        int16_t val = (int16_t)(sinf(2.0f * (float)M_PI * freq_hz * i / sample_rate) * 28000.0f);
        buf[i * 2]     = val;  /* left */
        buf[i * 2 + 1] = val;  /* right */
    }

    for (int beep = 0; beep < 2; ++beep) {
        ret = esp_codec_dev_write(dac->codec_dev, buf,
                                  (size_t)nsamples * 2 * sizeof(int16_t));
        ESP_LOGI(TAG, "boot beep #%d: wrote %d samples (%u bytes) -> %s",
                 beep + 1, nsamples, (unsigned)(nsamples * 2 * sizeof(int16_t)),
                 esp_err_to_name(ret));
        /* Wait for I2S DMA to drain before muting — otherwise the amplifier
         * cuts off the tail of the tone. */
        vTaskDelay(pdMS_TO_TICKS(duration_ms + gap_ms));
    }
    heap_caps_free(buf);

close:
    esp_codec_dev_close(dac->codec_dev);
    pa_ret = esp_io_expander_set_level(expander, pa_mask, 0);
    ESP_LOGI(TAG, "PA_CTRL LOW (amplifier muted): %s", esp_err_to_name(pa_ret));
}

static int display_lcd_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(device_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device_handle is NULL");

    esp_io_expander_handle_t *expander = NULL;
    ESP_RETURN_ON_ERROR(esp_board_device_get_handle("gpio_expander", (void **)&expander),
                        TAG, "AW9523 handle unavailable");

    /* Configure the AW9523 pins we drive as OUTPUT *before* touching them.
     *
     * ROOT CAUSE of the "driver returns ESP_OK but the panel never changes"
     * symptom: AW9523 pins power up as INPUTS. Earlier revisions called
     * esp_io_expander_set_level() on the reset pin without ever setting its
     * direction, so LCD_RST stayed floating and the ST7796 was never actually
     * reset. An unreset ST7796 ignores every command and pixel write, while
     * the ESP-side I80 driver happily reports success — exactly what we saw.
     *
     * The reference does this explicitly:
     *   aw.pinMode(8..11, OUTPUT);  // LCD_LEDK
     *   aw.pinMode(14, OUTPUT);     // LCD_RST
     */
    const uint32_t bl_mask =
        (1U << AW9523_PIN_LCD_LED_0) | (1U << AW9523_PIN_LCD_LED_1) |
        (1U << AW9523_PIN_LCD_LED_3);
    const uint32_t rst_mask = 1U << AW9523_PIN_LCD_RESET;

    esp_err_t dir_ret = esp_io_expander_set_dir(*expander, bl_mask | rst_mask,
                                                IO_EXPANDER_OUTPUT);
    ESP_LOGI(TAG, "AW9523 set_dir(OUTPUT) LEDK+RST mask=0x%04X -> %s",
             (unsigned)(bl_mask | rst_mask), esp_err_to_name(dir_ret));

    /* Backlight ON (LEDK = LED cathode, sink current to light the panel).
     * Kept before esp_lcd_new_i80_bus() as a matter of sequencing hygiene.
     * Historical note: an earlier revision mapped LCD_PIN_D1 to GPIO 4, which
     * is also the I2C SCL line, so creating the I80 bus stole the pin from the
     * I2C controller and broke every later AW9523 access. The schematic pin
     * map has no such overlap. */
    esp_err_t bl_ret = esp_io_expander_set_level(*expander, bl_mask, 0);
    ESP_LOGI(TAG, "backlight ON (P8-P11 LOW): %s", esp_err_to_name(bl_ret));

    /* Reset pulse, 200 ms low as in the reference (we previously used 20 ms). */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(*expander, rst_mask, 0),
                        TAG, "LCD reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(*expander, rst_mask, 1),
                        TAG, "LCD reset release failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "LCD reset pulse complete (P14 OUTPUT, 200ms low)");

    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .data_gpio_nums = {
            LCD_PIN_D0, LCD_PIN_D1, LCD_PIN_D2, LCD_PIN_D3,
            LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_MAX_TRANSFER_BYTES,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &s_i80_bus), TAG,
                        "I80 bus creation failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.swap_color_bytes = 1,
    };
    esp_err_t ret = esp_lcd_new_panel_io_i80(s_i80_bus, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, NULL);
        ESP_LOGE(TAG, "I80 panel IO creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7796(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        cleanup_display_lcd(NULL, io_handle);
        ESP_LOGE(TAG, "ST7796 panel creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_init(panel_handle);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_swap_xy(panel_handle, true);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_mirror(panel_handle, false, false);
    }
    if (ret == ESP_OK) {
        /* Note: earlier revisions called esp_lcd_panel_invert_color(true)
         * based on the moononournation Dev_Device_Pins reference marking
         * this panel as IPS. That was wrong — Arduino_GFX's "IPS=true"
         * flag is a software colorspace tweak, not INVON. Sending INVON
         * here turns the whole panel dark and every rendered pixel is
         * invisible against the black background. Leave native polarity. */
        ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    }
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "ST7796 initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Draw the Boot HUD (system self-test panel) and hand the panel over.
     *
     * The HUD is deliberately textual rather than a colour pattern. Boards
     * are commonly flashed over the air where serial output cannot be
     * captured, so on-screen text identifying the build is the only reliable
     * confirmation of which firmware is actually running. */
    boot_hud_test_t hud = {
        .expander_ok   = true,   /* we reached this point -> AW9523 works */
        .backlight_ok  = true,   /* backlight already toggled on above */
        .touch_ok      = false,
        .touch_vendor_id = 0,
        .dac_ok        = false,
        .adc_ok        = false,
        .camera_ok     = false,
        .prox_ok       = (s_prox_dev != NULL),
    };

    /* Probe FT6336U touch controller + camera SCCB on the shared I2C bus,
     * then init the proximity sensor (non-blocking, non-fatal). */
    void *i2c_periph = NULL;
    if (esp_board_periph_ref_handle("i2c_master", &i2c_periph) == ESP_OK) {
        i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)i2c_periph;

        uint8_t vend_id = 0;
        if (ft6336u_probe(i2c_bus, &vend_id) == ESP_OK) {
            hud.touch_ok = true;
            hud.touch_vendor_id = vend_id;
            ESP_LOGI(TAG, "FT6336U touch ACK at 0x38, vendor ID=0x%02X", vend_id);
        } else {
            ESP_LOGW(TAG, "FT6336U touch not responding at 0x38");
        }

        hud.camera_ok = camera_probe(i2c_bus);
        ad35_s3_init_proximity(*expander, i2c_bus);
        hud.prox_ok = (s_prox_dev != NULL);

        esp_board_periph_unref_handle("i2c_master");
    } else {
        ESP_LOGW(TAG, "No i2c_master handle for touch/camera/prox self-test");
    }

    /* Query audio codec handles to fill the DAC/ADC self-test fields. */
    {
        dev_audio_codec_handles_t *dac = NULL;
        if (esp_board_device_get_handle("audio_dac", (void **)&dac) != ESP_OK ||
            dac == NULL || dac->codec_dev == NULL) {
            ESP_LOGW(TAG, "audio_dac handle not available for HUD");
        } else {
            hud.dac_ok = true;
            ESP_LOGI(TAG, "ES8311 DAC at 0x30 OK");
        }
        dev_audio_codec_handles_t *adc = NULL;
        if (esp_board_device_get_handle("audio_adc", (void **)&adc) != ESP_OK ||
            adc == NULL || adc->codec_dev == NULL) {
            ESP_LOGW(TAG, "audio_adc handle not available for HUD");
        } else {
            hud.adc_ok = true;
            ESP_LOGI(TAG, "ES7210 ADC at 0x40 OK");
        }
    }

    (void)render_boot_hud(panel_handle, &hud);
    vTaskDelay(pdMS_TO_TICKS(1200));

    ret = esp_board_device_override_config("display_lcd", (void *)&s_lcd_config,
                                           sizeof(s_lcd_config));
    if (ret != ESP_OK) {
        cleanup_display_lcd(panel_handle, io_handle);
        ESP_LOGE(TAG, "Display configuration override failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Deferred speaker beeps run ~500ms after we return, when audio_dac
     * has been initialized by the Board Manager.  Uses a dedicated work task
     * (not a FreeRTOS timer) — the tone generation needs large malloc + I2S
     * writes, which exceed the Tmr Svc task stack. */
    s_tone_expander = *expander;
    xTaskCreate(tone_work_task, "tone", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);

    /* Start the Love2D runtime.  It allocates the PSRAM framebuffer, loads
     * /spiffs/main.lua (or the builtin fallback), and enters the ~60 FPS
     * game loop with touch polling on the shared I2C bus.
     *
     * The codec handle for love.audio is resolved lazily by the runtime via
     * esp_board_device_get_handle("audio_dac") once Board Manager finishes
     * initializing all devices. */
    {
        void *love_i2c = NULL;
        if (esp_board_periph_ref_handle("i2c_master", &love_i2c) == ESP_OK) {
            esp_err_t lret = lua_module_love2d_start(panel_handle, love_i2c);
            if (lret != ESP_OK) {
                ESP_LOGW(TAG, "Love2D runtime failed to start: %s", esp_err_to_name(lret));
            }
            /* Keep the bus referenced: the love loop holds it for touch. */
        } else {
            ESP_LOGW(TAG, "Love2D runtime skipped: no i2c_master handle");
        }
    }

    s_lcd_handles.io_handle = io_handle;
    s_lcd_handles.panel_handle = panel_handle;
    *device_handle = &s_lcd_handles;
    ESP_LOGI(TAG, "AD35-S3 ST7796 I80 LCD ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

static int display_lcd_deinit(void *device_handle)
{
    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)device_handle;
    cleanup_display_lcd(handles != NULL ? handles->panel_handle : NULL,
                        handles != NULL ? handles->io_handle : NULL);
    memset(&s_lcd_handles, 0, sizeof(s_lcd_handles));
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(display_lcd, display_lcd_init, display_lcd_deinit);