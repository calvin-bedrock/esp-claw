# Love2D Lua Module

Minimal Love2D-compatible Lua API binding for ESP-Claw / AD35-S3.

## API

### `love.graphics`
- `love.graphics.clear(r, g, b)` — fill framebuffer with RGB color
- `love.graphics.rectangle(mode, x, y, w, h)` — mode: "fill" or "line"
- `love.graphics.circle(mode, x, y, radius)` — mode: "fill" or "line"
- `love.graphics.print(text, x, y)` — 16px monochrome text (2x scaled 8x8 font)
- `love.graphics.draw(drawable, x, y)` — draw table with .width/.height

### `love.audio`
- `love.audio.beep(freq, duration_ms)` — sine wave tone via ES8311 DAC
- `love.audio.play(wav_path)` — WAV streaming (stub)
- `love.audio.set_volume(vol)` — 0.0 to 100.0

### Event callbacks (defined in main.lua)
- `love.update(dt)` — called every frame
- `love.draw()` — called every frame after update
- `love.touchpressed(id, x, y, dx, dy)` — FT6336U touch press
- `love.touchreleased(id, x, y)` — touch release
- `love.touchmoved(id, x, y, dx, dy)` — touch drag
- `love.voice_wake(energy)` — mic energy wake (stub)

## Dependencies

- `cap_lua` — Lua runtime registration
- `esp_lcd` — panel framebuffer flush
- `esp_codec_dev` — audio DAC output
- `driver` — I2C master for FT6336U touch