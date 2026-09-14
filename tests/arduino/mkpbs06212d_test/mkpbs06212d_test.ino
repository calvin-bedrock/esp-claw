/*
 * MKPBS06212D 分体式光传感器 — 寄存器扫描模式
 * 全总线扫描 + 对未知设备 dump 0x80-0xAE 寄存器区间
 */

#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA_PIN   5
#define I2C_SCL_PIN   4
#define I2C_FREQ      100000

#define AW9523_ADDR  0x59
#define ADDR_KNOWN   0x39  // MK-PB2016PS

uint8_t sensor_addr = 0;

// ---- AW9523 上电 ----
bool powerSensor(bool on) {
  Serial.print("[AW9523] VCC -> ");
  Serial.println(on ? "ON" : "OFF");
  // P1_4 = OUTPUT
  Wire.beginTransmission(AW9523_ADDR);
  Wire.write(0x05);
  Wire.write(0xEF);
  Wire.endTransmission();
  delay(5);
  // P1_4 = HIGH/LOW
  Wire.beginTransmission(AW9523_ADDR);
  Wire.write(0x03);
  Wire.write(on ? 0x10 : 0x00);
  Wire.endTransmission();
  delay(50);
  return true;
}

// ---- I2C 工具 ----
bool probeAddr(uint8_t addr) {
  Wire.beginTransmission(addr);
  delay(3);
  return (Wire.endTransmission() == 0);
}

uint8_t readByte(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(true);
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// ---- 寄存器 dump (0x80-0xAE) ----
void dumpRegs(uint8_t addr, uint8_t start, uint8_t end) {
  for (uint8_t r = start; r <= end; r++) {
    uint8_t v = readByte(addr, r);
    Serial.print(" ");
    if (v < 16) Serial.print("0");
    Serial.print(v, HEX);
    if ((r - start + 1) % 16 == 0) Serial.println();
  }
  if ((end - start + 1) % 16 != 0) Serial.println();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);

  Serial.println("=== MKPBS06212D 寄存器扫描 ===");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);

  // AW9523 上电
  powerSensor(false);
  delay(50);
  powerSensor(true);
  delay(50);

  // 探测总线
  Serial.println();
  Serial.println("--- I2C 总线设备 ---");
  for (uint8_t a = 1; a < 127; a++) {
    if (probeAddr(a)) {
      uint8_t id = readByte(a, 0x82);
      Serial.print("  Addr=0x");
      if (a < 16) Serial.print("0");
      Serial.print(a, HEX);
      Serial.print("  REG[0x82]=0x");
      if (id < 16) Serial.print("0");
      Serial.print(id, HEX);
      if (a == AW9523_ADDR)       Serial.print("  (AW9523)");
      else if (a == ADDR_KNOWN)    Serial.print("  (MK-PB2016PS)");
      else if (a == 0x18 || a == 0x40) Serial.print("  <<< 未知, 将 dump 寄存器");
      Serial.println();
    }
  }

  // ---- 深度 dump 0x18 和 0x40 ----
  uint8_t targets[] = {0x18, 0x40};
  for (int t = 0; t < 2; t++) {
    uint8_t a = targets[t];
    if (!probeAddr(a)) continue;

    Serial.println();
    Serial.print("=== 0x");
    if (a < 16) Serial.print("0");
    Serial.print(a, HEX);
    Serial.println(" 寄存器 dump 0x80-0xAE ===");
    Serial.println("  reg: 80 81 82 83 84 85 86 87 88 89 8A 8B 8C 8D 8E 8F");
    Serial.print("  0x8X: ");
    dumpRegs(a, 0x80, 0x8F);

    Serial.print("  0x9X: ");
    dumpRegs(a, 0x90, 0x9F);

    Serial.print("  0xAX: ");
    dumpRegs(a, 0xA0, 0xAE);
  }

  Serial.println();
  Serial.println("=== 对照 MKPBS06212D 手册 ===");
  Serial.println("  0x82=0x24 (DEV_ID), 0x83=0x00 (REV_ID)");
  Serial.println("  ADC_R=0xA0-1, ADC_G=0xA2-3, ADC_B=0xA4-5");
  Serial.println("  ADC_C=0xA6-7, ADC_IR=0xA8-9");
  Serial.println();
  Serial.println("如果 0x40 的 0x80-0xAE 匹配上述, 它就是 MKPBS06212D!");
  Serial.flush();
}

void loop() {
  delay(10000);
}