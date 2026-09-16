local gfx = require("display_ui")
local current_state = 0  -- 0=idle, 1=eyes, 2=swipe, 3=wake, 4=angry

function render_idle()
    gfx.clear(0, 0, 0)
    gfx.draw_cat(current_state, 120, 100)
    gfx.print("Cat: IDLE", 10, 280)
    gfx.print("tap | swipe | voice", 10, 300)
end

-- External trigger (from ESP-Claw event router or touch/voice event)
function set_state(s)
    if s >= 0 and s <= 4 then
        current_state = s
        gfx.clear(0, 0, 0)
        gfx.draw_cat(current_state, 120, 100)
        gfx.print("State: " .. tostring(s), 10, 280)
    end
end

function get_states()
    return {"idle", "eyes", "swipe", "wakeup", "angry"}
end

render_idle()
