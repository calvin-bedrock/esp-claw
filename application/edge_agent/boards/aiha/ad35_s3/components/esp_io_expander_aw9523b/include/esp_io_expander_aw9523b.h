/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AW9523B_IO_COUNT (16)

#define AW9523B_REG_INPUT0    0x00
#define AW9523B_REG_INPUT1    0x01
#define AW9523B_REG_OUTPUT0   0x02
#define AW9523B_REG_OUTPUT1   0x03
#define AW9523B_REG_CONFIG0   0x04
#define AW9523B_REG_CONFIG1   0x05
#define AW9523B_REG_INTR0     0x06
#define AW9523B_REG_INTR1     0x07
#define AW9523B_REG_ID        0x10
#define AW9523B_REG_GCR       0x11
#define AW9523B_REG_LEDMODE0  0x12
#define AW9523B_REG_LEDMODE1  0x13
#define AW9523B_REG_SOFTRESET 0x7F

typedef struct {
    esp_io_expander_t base;
    i2c_master_dev_handle_t i2c_handle;
    struct {
        uint16_t direction;
        uint16_t output;
    } regs;
} esp_io_expander_aw9523b_t;

esp_err_t esp_io_expander_new_aw9523b(i2c_master_bus_handle_t i2c_bus,
                                      uint32_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret);

esp_err_t esp_io_expander_aw9523b_write_reg(esp_io_expander_handle_t handle,
                                            uint8_t reg_addr,
                                            uint8_t *data,
                                            size_t data_len);

#ifdef __cplusplus
}
#endif
