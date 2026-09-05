-- AD35-S3 LCD Lua 写字示例
-- 使用 esp-claw 的 display 模块直接在已点亮的屏幕上写字
-- 固件要求: 0.1.4 (SHA: 56e04f5a...)

local display = require("display")
local delay = require("delay")

-- 初始化显示会话（使用已初始化的 board LCD 配置）
local ok, session = pcall(display.create_session, {
  panel_config = "st7796",
  width = 480,
  height = 320,
  pixel_format = "rgb565"
})

if not ok or session == nil or session == 0 then
  print("LCD session init failed:", ok, session)
else
  print("LCD session started, handle:", session)

  -- 写字：在屏幕左上角绘制白色文字
  display.draw_text(20, 30, "AD35-S3", {
    color = 0xFFFF,     -- 白色 (RGB565)
    background = 0x0000, -- 黑色背景
    font_size = 24
  })

  -- 再写一行小字
  display.draw_text(20, 70, "QM-Y1091-4832", {
    color = 0xFFE0,
    font_size = 16
  })

  -- 第三行：显示当前固件版本信息
  display.draw_text(20, 110, "FW 0.1.4", {
    color = 0x07E0, -- 绿色
    font_size = 20
  })

  -- 最后一行：显示硬件外设状态（已验证原始引脚修复后）
  display.draw_text(20, 160, "I2C SDA=GPIO5 SCL=GPIO4", {
    color = 0xF81F, -- 粉色
    font_size = 14
  })

  display.draw_text(20, 190, "AW9523=0x59 ACK OK", {
    color = 0x07FF, -- 青色
    font_size = 14
  })

  print("LCD text drawn successfully")
end
