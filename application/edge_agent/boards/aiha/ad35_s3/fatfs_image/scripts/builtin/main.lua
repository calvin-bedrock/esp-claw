-- ============================================================================
-- ESP-Claw / Love2D Runtime — builtin demo main.lua
--
-- This is the fallback script used when the Love2D runtime boots on the
-- AD35-S3.  It is baked into the read-only /system partition and serves as
-- both a smoke test for love.graphics / love.audio / touch and as a template
-- that users replace with their own game in /spiffs/main.lua.
--
-- A writable copy lives at /spiffs/main.lua (DATA root) so it can be edited
-- at runtime without rebuilding the firmware.
-- ============================================================================

-- Demo state
local t = 0
local x, y = 240, 160
local ball_r = 40
local color_idx = 1
local colors = {
    {0, 180, 255},  -- cyan
    {255, 80, 80},  -- red
    {80, 255, 80},  -- green
    {255, 220, 0},  -- yellow
}

love.load = function()
    print("Love2D demo: main.lua loaded")
end

love.update = function(dt)
    t = t + dt
end

love.draw = function()
    local c = colors[color_idx]
    -- Clear to deep navy
    love.graphics.clear(10, 14, 40)

    -- Title
    love.graphics.print("ESP-CLAW Love2D Runtime v1.0.0", 20, 20)

    -- Animated circle (bounce)
    x = math.floor(240 + 180 * math.sin(t * 2.0))
    y = math.floor(160 + 80 * math.cos(t * 1.7))
    love.graphics.circle("fill", x, y, ball_r)

    -- A rectangle that pulses
    local w = 60 + 40 * math.sin(t * 3.0)
    love.graphics.rectangle("fill", 200, 260, 100 + w, 24)

    -- FPS + hint
    love.graphics.print("Touch to change color", 20, 290)
end

-- Touch handlers
love.touchpressed = function(id, tx, ty)
    print("touchpressed", id, tx, ty)
    color_idx = (color_idx % #colors) + 1
end

love.touchreleased = function(id, tx, ty)
    print("touchreleased", id, tx, ty)
end

love.touchmoved = function(id, tx, ty, dx, dy)
    -- optional
end

-- Voice wake stub — the Love2D runtime fires this on mic energy spikes
love.voice_wake = function(energy)
    print("voice_wake", energy)
end
