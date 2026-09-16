/*
 * SPDX-FileCopyrightText: 2026 Calvin Bedrock
 * SPDX-License-Identifier: Apache-2.0
 *
 * love.audio implementation.
 * Wraps the existing esp_codec_dev output for beep tones and streaming.
 *
 * PA_CTRL (amplifier enable) is controlled externally by setup_device.c
 * — the amplifier is enabled once before the Love2D runtime starts and
 * stays on during the session.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_codec_dev_types.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "love_audio";

static esp_codec_dev_handle_t s_codec = NULL;

/* ---- Public setters ---- */

void love_set_codec_handle(void *codec_dev_handle)
{
    s_codec = (esp_codec_dev_handle_t)codec_dev_handle;
}

/* ---- Lua bindings ---- */

/**
 * love.audio.beep(freq, duration_ms)
 * Generate a pure sine wave tone.
 */
int lua_love_audio_beep(lua_State *L)
{
    int freq_hz = (int)luaL_checkinteger(L, 1);
    int duration_ms = (int)luaL_checkinteger(L, 2);

    if (s_codec == NULL) {
        lua_pushboolean(L, false);
        return 1;
    }

    const int sample_rate = 48000;
    const int nsamples = sample_rate * duration_ms / 1000;
    if (nsamples <= 0) {
        lua_pushboolean(L, false);
        return 1;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate,
    };

    esp_err_t ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "codec_dev_open: %s", esp_err_to_name(ret));
        lua_pushboolean(L, false);
        return 1;
    }

    size_t buf_bytes = (size_t)nsamples * 2 * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        esp_codec_dev_close(s_codec);
        lua_pushboolean(L, false);
        return 1;
    }

    for (int i = 0; i < nsamples; ++i) {
        int16_t val = (int16_t)(sinf(2.0f * (float)M_PI * freq_hz * i / sample_rate) * 28000.0f);
        buf[i * 2]     = val;
        buf[i * 2 + 1] = val;
    }

    esp_codec_dev_set_out_vol(s_codec, 50.0f);
    ret = esp_codec_dev_write(s_codec, buf, buf_bytes);
    heap_caps_free(buf);

    vTaskDelay(pdMS_TO_TICKS(duration_ms + 50));
    esp_codec_dev_close(s_codec);

    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

/**
 * love.audio.play(wav_path) — stub.
 */
int lua_love_audio_play(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    ESP_LOGW(TAG, "love.audio.play(\"%s\"): not implemented (WAV streaming)", path);
    lua_pushboolean(L, false);
    return 1;
}

/**
 * love.audio.set_volume(vol) — 0.0 to 100.0.
 */
int lua_love_audio_set_volume(lua_State *L)
{
    float vol = (float)luaL_checknumber(L, 1);
    vol = vol < 0.0f ? 0.0f : (vol > 100.0f ? 100.0f : vol);
    if (s_codec != NULL) {
        esp_err_t ret = esp_codec_dev_set_out_vol(s_codec, vol);
        lua_pushboolean(L, ret == ESP_OK);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}