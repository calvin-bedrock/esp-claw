-- Pixel Cat Demo - Hot reload test
-- Uses simplified display UI module (love2d style)

-- Access display functions
local gfx = require("display_ui")  -- Will be registered as module

-- Pixel cat states (16x16 binary masks)
local states = {"idle", "eyes", "swipe", "wakeup", "angry"}
local current_state = 0

function love.load()
    gfx.clear(0, 0, 0)  -- Black background
    gfx.print("Pixel Cat Demo", 10, 30)
end

function love.draw()
    gfx.clear(0, 0, 0)
    gfx.draw_cat(current_state, 120, 100)
    gfx.print("State: " .. states[current_state + 1], 10, 200)
    gfx.print("Double-tap or swipe to change", 10, 220)
end

function love.update(dt)
    -- State changes triggered by touch/voice events
    -- For demo, auto-cycle slowly
    -- (In real use, this would be triggered by pollTouch()/pollVoice())
end

-- External trigger function (called by ESP-Claw event system)
function set_cat_state(new_state)
    if new_state >= 0 and new_state <= 4 then
        current_state = new_state
        gfx.clear(0, 0, 0)
        gfx.draw_cat(current_state, 120, 100)
    end
end

-- List available states for UI
function get_states()
    return states
end
