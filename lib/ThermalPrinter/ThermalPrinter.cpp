/********************************************************
 * ThermalPrinter.cpp - 热敏打印机驱动库实现
 *
 * ===== 工作原理 =====
 *
 * 1. 数据加载：
 *    DAT/CLK 串行将一行的 384bit (48字节) 发送到
 *    3 片 74HC595 移位寄存器（级联输出）
 *
 * 2. 锁存：
 *    LAT 上升沿将移位寄存器数据锁存到输出端
 *    此时打印头 384 个加热点准备好（加热/不加热）
 *
 * 3. 加热：
 *    STB=HI → 打印头开始加热
 *    PVH 提供加热电流（~10W 峰值功耗）
 *    加热时间根据加热点数动态计算
 *    STB=LO → 停止加热
 *
 * 4. 走纸：
 *    SS8833 驱动步进电机走纸 N 步
 *
 * ===== 自适应加热算法 =====
 *
 *   加热时间 = heatMin + (heatMax - heatMin) * (dotCount / 384)
 *   0 点：跳过 STB（最快）
 *   少量点：短脉宽（速度快）
 *   大量点：长脉宽（保证深度）
 *
 *   温度补偿：
 *     tempADC < nomADC（热）→ 减少加热时间
 *     tempADC > nomADC（冷）→ 增加加热时间
 *
 * ===== 调试历程 =====
 *   - PVH 必须拉高才能加热（P-MOS 控制，高电平导通）
 *   - motorEN 拉低使能电机（低有效）
 *   - 纸传感器 IO3 硬件损坏无法使用
 *   - 加热时间超过 30ms 可能烧打印头
 *   - 热量累计：连续打印多行时温度会不断升高
 ********************************************************/

#include "ThermalPrinter.h"

// ===== 4相8拍步进序列 =====
// 每个数组元素: [AIN1, AIN2, BIN1, BIN2]
// 0,1 = 绕组A正转/反转, 2,3 = 绕组B正转/反转
// 8拍比4拍更平滑、力矩更大
const uint8_t ThermalPrinter::STEP_SEQ[8][4] = {
  {0, 1, 1, 0},  // 第0拍: A- B+
  {0, 0, 1, 0},  // 第1拍: B+
  {1, 0, 1, 0},  // 第2拍: A+ B+
  {1, 0, 0, 0},  // 第3拍: A+
  {1, 0, 0, 1},  // 第4拍: A+ B-
  {0, 0, 0, 1},  // 第5拍: B-
  {0, 1, 0, 1},  // 第6拍: A- B-
  {0, 1, 0, 0},  // 第7拍: A-
};

ThermalPrinter::ThermalPrinter() {}

ThermalPrinter::ThermalPrinter(const Config& cfg) : _cfg(cfg) {}

// ============================================================
// 初始化
// ============================================================
void ThermalPrinter::begin() {
  // 电机引脚（SS8833 控制）
  pinMode(_cfg.motorEN, OUTPUT);
  pinMode(_cfg.motorA1, OUTPUT);
  pinMode(_cfg.motorA2, OUTPUT);
  pinMode(_cfg.motorB1, OUTPUT);
  pinMode(_cfg.motorB2, OUTPUT);
  // 打印头控制引脚
  pinMode(_cfg.pvh, OUTPUT);    // P-MOS 加热供电
  pinMode(_cfg.dat, OUTPUT);    // 74HC595 数据
  pinMode(_cfg.clk, OUTPUT);    // 74HC595 时钟
  pinMode(_cfg.lat, OUTPUT);    // 74HC595 锁存
  pinMode(_cfg.stb, OUTPUT);    // 加热选通
  // 传感器（ADC 输入模式）
  pinMode(_cfg.paper, INPUT);
  pinMode(_cfg.temp, INPUT);

  // 所有输出初始化为关断状态
  // ★ 3V3 供电轨（VDDCTL=GPIO47，低=开）：常开，给打印头逻辑 + 电机驱动 AB8833 +
  //   纸传感器供电。**这里一开到底，之后任何地方都不再关它**（见 powerOff 注释）。
  digitalWrite(_cfg.motorEN, LOW);
  motorOff();                        // 释放电机线圈（线圈不通电，但驱动芯片保持供电）
  // 关闭加热供电（⚠️ 按极性写"关"电平：低有效时"关"= HIGH）
  digitalWrite(_cfg.pvh, _cfg.pvhActiveLow ? HIGH : LOW);
  digitalWrite(_cfg.lat, HIGH);     // 锁存器高（不锁存）
  digitalWrite(_cfg.stb, LOW);      // 加热关闭
  digitalWrite(_cfg.clk, LOW);      // 时钟低
  digitalWrite(_cfg.dat, LOW);      // 数据低
  _powered = false;
}

// ============================================================
// 步进电机控制
// ============================================================

// 单步前进
// 【如需修改步进方向】
//   反转 STEP_SEQ 数组顺序即可
void ThermalPrinter::motorStep() {
  digitalWrite(_cfg.motorA1, STEP_SEQ[_motorPos][0]);
  digitalWrite(_cfg.motorA2, STEP_SEQ[_motorPos][1]);
  digitalWrite(_cfg.motorB1, STEP_SEQ[_motorPos][2]);
  digitalWrite(_cfg.motorB2, STEP_SEQ[_motorPos][3]);
  _motorPos++;
  if (_motorPos >= 8) _motorPos = 0;  // 循环
  delayMicroseconds(_cfg.motorDelayUs);
}

// 单步后退
void ThermalPrinter::motorStepReverse() {
  if (_motorPos == 0) _motorPos = 7;
  else _motorPos--;
  digitalWrite(_cfg.motorA1, STEP_SEQ[_motorPos][0]);
  digitalWrite(_cfg.motorA2, STEP_SEQ[_motorPos][1]);
  digitalWrite(_cfg.motorB1, STEP_SEQ[_motorPos][2]);
  digitalWrite(_cfg.motorB2, STEP_SEQ[_motorPos][3]);
  delayMicroseconds(_cfg.motorDelayUs);
}

// 跑 N 步
void ThermalPrinter::motorRunSteps(uint32_t steps, bool forward) {
  powerOn();       // PVH=HI + 电机使能
  delay(50);
  for (uint32_t i = 0; i < steps; i++) {
    if (forward) motorStep();
    else motorStepReverse();
  }
  motorOff();      // 释放线圈
  powerOff();      // PVH=LO
}

// 释放电机线圈
// 防止长时间通电发热 + 省电
void ThermalPrinter::motorOff() {
  digitalWrite(_cfg.motorA1, LOW);
  digitalWrite(_cfg.motorA2, LOW);
  digitalWrite(_cfg.motorB1, LOW);
  digitalWrite(_cfg.motorB2, LOW);
}

// ============================================================
// 电源控制
// ============================================================

void ThermalPrinter::powerOn() {
  if (_powered) return;
  digitalWrite(_cfg.pvh, _cfg.pvhActiveLow ? LOW : HIGH);  // 开启加热供电（极性可配）
  // ★ 3V3 供电（VDDCTL=GPIO47，低=开）：这条电给「打印头逻辑 + 电机驱动 AB8833 +
  //   纸传感器」共用。**打印期间必须一直输出 3.3V**——时通时断会让电机驱动丢状态、
  //   电机不转（老陈 2026-10-04 实测：Q3 输出在通断之间跳）。
  digitalWrite(_cfg.motorEN, LOW);
  _powered = true;
  delay(20);   // 等 3V3 轨稳住再动作（电源上升沿 + 去耦电容充电）
}

void ThermalPrinter::powerOff() {
  // ★★ 关键修正（2026-10-04）：**不关 3V3 轨**！
  //   原来这里写 motorEN=HIGH（失能电机），顺带把 VDDCTL 拉到高 → Q3 关断 →
  //   整条 3V3 掉电。结果打印期间 3V3 反复通断：电机驱动复位、走纸丢步、
  //   打印头逻辑无电。老陈的要求就是"打印时一直输出 3.3V"，所以这里只关加热 VH。
  //   （3V3 轨在 begin() 里一开到底，空闲时也不关——代价只是几 mA 待机电流。）
  //
  // ★★ 又一处修正（2026-10-04 下午）：**VH 保持闩锁期间不关 VH**。
  //   老陈实测"VH 时通时断"——打印头打完一段 powerOff 关一次、走纸 feedSteps
  //   开头再开一次，中间就断了。而电机 AB8833 的 VM 正是 VH（原理图 U16-12），
  //   VH 一断电机立刻丢电丢步。上层用 vhHold(true/false) 把"打印+走纸"包起来，
  //   这里就不再真正关 VH —— 整段动作 GPIO48 恒定拉高，一次都不跳。
  if (_vhHold) return;                                     // ★ 保持期间：不动 VH
  digitalWrite(_cfg.pvh, _cfg.pvhActiveLow ? HIGH : LOW);  // 只关加热供电 ⚠️ 必须关！
  _powered = false;
}

void ThermalPrinter::vhHold(bool on) {
  _vhHold = on;
  if (on) {
    powerOn();      // 立即确保 VH 已开（已开则提前返回，不重复写）
  } else {
    powerOff();     // 闩锁已清，这里会真正关一次 VH
  }
}

void ThermalPrinter::setVhActiveLow(bool low) {
  _cfg.pvhActiveLow = low;
}

// ============================================================
// 走纸
// ============================================================

// 走纸 N 步
void ThermalPrinter::feedSteps(uint32_t steps) {
  powerOn();
  delay(10);
  for (uint32_t i = 0; i < steps; i++) motorStep();
  motorOff();
  powerOff();
}

// 走纸 N 行
void ThermalPrinter::feedLines(uint16_t lines) {
  feedSteps((uint32_t)lines * _cfg.stepsPerLine);
}

// ============================================================
// 打印头操作
// ============================================================

// ---- 统计加热点数 ----
// 遍历48字节，用 Kernighan 算法统计置1的位数
uint16_t ThermalPrinter::countDots(const uint8_t* data) {
  uint16_t count = 0;
  for (int i = 0; i < TP_BYTES_PER_LINE; i++) {
    uint8_t b = data[i];
    while (b) {
      count++;
      b &= b - 1;  // 清除最低位的1
    }
  }
  return count;
}

// ---- 发送一行数据到 74HC595 移位寄存器 ----
// 48字节 = 384bit，MSB 先发
void ThermalPrinter::sendLine(const uint8_t* data) {
  for (int i = 0; i < TP_BYTES_PER_LINE; i++) {
    uint8_t b = data[i];
    for (int bit = 7; bit >= 0; bit--) {  // MSB first
      digitalWrite(_cfg.dat, (b >> bit) & 1);
      digitalWrite(_cfg.clk, HIGH);
      delayMicroseconds(1);
      digitalWrite(_cfg.clk, LOW);
      delayMicroseconds(1);
    }
  }
}

// ---- 计算自适应加热时间 ----
// 算法：
//   基础时间 = heatMin + 线性插值(点数/384)
//   温度补偿：热时减少，冷时增加
// 【如需修改温度补偿行为】
//   调整 nomADC（基准温度 ADC 值）
//   heatAdd（低温额外加热）
//   coolSub（高温减少加热）
uint16_t ThermalPrinter::calcHeatTime(uint16_t dotCount, int tempADC) {
  // 基础加热时间（按点数线性插值）
  uint16_t base = _cfg.heatTimeMinMs +
    (uint32_t)(_cfg.heatTimeMaxMs - _cfg.heatTimeMinMs) * dotCount / TP_DOTS_PER_LINE;

  // ---- 温度传感器安全检查 ----
  // ADC=0 → 传感器未接或短路到 GND，跳过补偿，使用基础时间
  // ADC>4000 → 传感器开路到 VCC，同样跳过
  if (tempADC < 50 || tempADC > 4000) {
    return base;
  }

  // ---- 温度补偿参数 ----
  // 【如需调节温度补偿强度，改这里】
  int nomADC = 280;          // 基准温度 ADC：打印头在正常工作温度时的 ADC 值
                               //   热敏电阻 NTC：温度越高→电阻越低→ADC 越小
                               //   280 ≈ ~50°C（典型工作温度）
  int heatAdd = 3;            // 低温补偿系数：ADC 每比 nomADC 大1，加 XX ms
                               //   调大→低温时加热更深，适合冬天或长时间连续打印
  int coolSub = 3;            // 高温衰减系数：ADC 每比 nomADC 小1，减 XX ms
                               //   调大→高温时散热更好，防止烧打印头

  if (tempADC < nomADC) {
    // 偏热（ADC 小→温度高）
    int diff = nomADC - tempADC;
    int reduction = diff * coolSub;
    if (reduction > base) reduction = base - 1;  // 至少保留1ms
    base = (base > reduction) ? (base - reduction) : 1;
  } else {
    // 偏冷（ADC 大→温度低）
    int diff = tempADC - nomADC;
    uint16_t addition = diff * heatAdd;
    uint16_t newHeat = base + addition;
    if (newHeat < base) newHeat = 65000;  // 溢出保护
    base = (newHeat > _cfg.heatTimeMaxMs) ? _cfg.heatTimeMaxMs : newHeat;
  }
  return base;
}

// ---- 锁存 + 加热 ----
// LAT 上升沿锁存数据到 74HC595 输出
// STB=HI 期间打印头加热
// 【如需消除打印重影】
//   调整 fireStrobe 中的 heatMs 计算
//   或在加热之间增加冷却 delay
void ThermalPrinter::fireStrobe(uint16_t dotCount) {
  if (dotCount == 0) return;  // 无点可打，跳过

  int tempADC = readTempADC();
  uint16_t heatMs = calcHeatTime(dotCount, tempADC);

  // LAT 上升沿锁存
  digitalWrite(_cfg.lat, LOW);
  delayMicroseconds(10);
  digitalWrite(_cfg.lat, HIGH);

  // 加热（可能多次 passes）
  for (int p = 0; p < _cfg.heatPasses; p++) {
    digitalWrite(_cfg.stb, HIGH);    // 开始加热
    delay(heatMs);                    // 保持加热
    digitalWrite(_cfg.stb, LOW);     // 停止加热
    if (p < _cfg.heatPasses - 1) delay(2);  // 两次加热间的冷却
  }
  delay(5);  // 加热后散热
}

// ============================================================
// 打印一行（不走纸）
// ============================================================
// 【如需修改打印流程】
//   如果要实现多行合并加热（节省时间）：
//     先连续 printLineNoFeed 多次，再一次性走纸
void ThermalPrinter::printLineNoFeed(const uint8_t* data) {
  powerOn();                                 // PVH=HI
  uint16_t dots = countDots(data);
  sendLine(data);                            // 发数据到 74HC595
  fireStrobe(dots);                          // 锁存 + 加热
  // 注意：这里不关 PVH，留给下次调用或手动关
}

// ============================================================
// 打印一行 + 走纸
// ============================================================
void ThermalPrinter::printLine(const uint8_t* data) {
  printLineNoFeed(data);
  // 走纸（步数 = stepsPerLine，可调节行间距）
  uint8_t steps = _cfg.stepsPerLine > 0 ? _cfg.stepsPerLine : 4;
  for (int s = 0; s < steps; s++) motorStep();
  motorOff();
  powerOff();  // ⚠️ 必须关 PVH
}

// ============================================================
// 打印位图
// ============================================================
// 【位图数据格式】
//   每行宽度 = width 像素，对齐到8位（bytesPerRow 字节）
//   每字节: bit7=最左, bit0=最右, 1=打黑, 0=空白
//   最大宽度：TP_DOTS_PER_LINE (384)
// 【如需打印灰度图像】
//   可以用抖动算法将灰度转为二值位图
void ThermalPrinter::printImage(const uint8_t* data, uint16_t width, uint16_t height) {
  if (width > TP_DOTS_PER_LINE) width = TP_DOTS_PER_LINE;
  uint16_t bytesPerRow = (width + 7) / 8;  // 对齐到整字节
  uint8_t lineBuf[TP_BYTES_PER_LINE];      // 48 字节行缓冲

  // ★ 2026-10-04 诊断：打印头故障排查
  //   tempADC 决定加热时间（偏热会被压到 1ms → 打不出/极淡），这里打出来一眼可见；
  //   每 48 行报一次进度 —— 卡在第几行、卡在哪个图案上一目了然。
  const int      temp0 = readTempADC();
  const uint32_t t0    = millis();
  Serial.printf("[TP] 开始 w=%u h=%u tempADC=%d heatMax=%u heatMin=%u steps=%u cool=%u split=%d/%u\n",
                (unsigned)width, (unsigned)height, temp0,
                (unsigned)_cfg.heatTimeMaxMs, (unsigned)_cfg.heatTimeMinMs,
                (unsigned)_cfg.stepsPerLine, (unsigned)_cfg.lineCoolMs,
                _cfg.splitDots ? 1 : 0, (unsigned)_cfg.splitThreshold);

  powerOn();
  delay(150);  // 等待 PVH 稳定

  const uint16_t halfBytes = TP_BYTES_PER_LINE / 2;   // 24 字节 = 左半/右半
  int splitRows = 0;
  uint16_t maxFire = 0;      // ★ 单次加热的最多点数 = 峰值电流的直接指标
                             //   验证供电时看这个数：它 ≤192 才有把握不触发过流保护
  // ★ 打印期间的电池电压采样（软件万用表）
  uint16_t vbatMin = 4095, vbatMax = 0, vbatIdle = 0;
  bool     vbatSamples = false;
  if (_cfg.vbatPin != 255) vbatIdle = (uint16_t)analogRead(_cfg.vbatPin);

  for (uint16_t y = 0; y < height; y++) {
    // 清空行缓冲
    memset(lineBuf, 0, TP_BYTES_PER_LINE);
    // 拷贝图像数据
    memcpy(lineBuf, data + y * bytesPerRow, bytesPerRow);
    uint16_t dots = countDots(lineBuf);

    if (_cfg.splitDots && dots > _cfg.splitThreshold) {
      // ★ 分两次加热：左半 / 右半。峰值电流从 384 点降到 ≤192 点，
      //   专治"电池保护板过流保护 → 纸走一下就不动"。
      uint8_t half[TP_BYTES_PER_LINE];
      uint16_t dL, dR;
      memcpy(half, lineBuf, TP_BYTES_PER_LINE);
      memset(half + halfBytes, 0, halfBytes);           // 只留左半
      dL = countDots(half);
      sendLine(half);  fireStrobe(dL);
      memcpy(half, lineBuf, TP_BYTES_PER_LINE);
      memset(half, 0, halfBytes);                       // 只留右半
      dR = countDots(half);
      sendLine(half);  fireStrobe(dR);
      if (dL > maxFire) maxFire = dL;
      if (dR > maxFire) maxFire = dR;
      splitRows++;
    } else {
      sendLine(lineBuf);
      fireStrobe(dots);
      if (dots > maxFire) maxFire = dots;
    }

    // ★ 每行加热完立刻量一次电池电压（此时负载最重，最能看出塌没塌）
    if (_cfg.vbatPin != 255) {
      uint16_t v = (uint16_t)analogRead(_cfg.vbatPin);
      if (v < vbatMin) vbatMin = v;
      if (v > vbatMax) vbatMax = v;
      vbatSamples = true;
    }

    uint8_t steps = _cfg.stepsPerLine > 0 ? _cfg.stepsPerLine : 4;
    for (int s = 0; s < steps; s++) motorStep();
    // ★ 额外冷却（默认 0）：电源带不动时调大，降平均电流
    if (_cfg.lineCoolMs) delay(_cfg.lineCoolMs);

    if ((y & 47) == 47)   // 每 48 行报一次
      Serial.printf("[TP]  …%u/%u 行  已用 %ums tempADC=%d\n", (unsigned)(y + 1),
                    (unsigned)height, (unsigned)(millis() - t0), readTempADC());
  }

  motorOff();
  powerOff();
  Serial.printf("[TP] 结束 %u 行  共 %ums  单次最多 %u 点%s\n", (unsigned)height,
                (unsigned)(millis() - t0), (unsigned)maxFire,
                splitRows ? "（有行已分两次加热）" : "");
  if (_cfg.vbatPin != 255 && vbatSamples) {
    // ★ 打印期间的电池电压"软件万用表"：电源塌了这里一眼可见。
    //   分压 100k/100k（见 ai_config.h PIN_BAT_ADC）→ VBAT = ADC × 2 × 3300 / 4095 mV
    int mvMin = (int)((uint32_t)vbatMin * 2 * 3300 / 4095);
    int mvMax = (int)((uint32_t)vbatMax * 2 * 3300 / 4095);
    Serial.printf("[TP] VBAT 空闲=%dmV  打印中 最低=%dmV(ADC%d) 最高=%dmV  ← 掉到 3.3V 以下=电源带不动\n",
                  (int)((uint32_t)vbatIdle * 2 * 3300 / 4095), mvMin, vbatMin, mvMax);
  }
}

// ============================================================
// 传感器
// ============================================================

// ---- 纸传感器 ----
// ⚠️ 立创实战派S3 的 IO3 物理损坏，此功能不可用
bool ThermalPrinter::hasPaper() {
  return readPaperADC() < 3000;  // 有纸时 ADC 低电平（< 3000）
}

int ThermalPrinter::readPaperADC() {
  int sum = 0;
  for (int i = 0; i < 3; i++) {
    sum += analogRead(_cfg.paper);
    delayMicroseconds(100);
  }
  return sum / 3;  // 3次取平均去噪声
}

// ---- 温度传感器 ----
// 热敏电阻 NTC：温度越高→电阻越低→ADC 读数越小
// 正常打印时 ADC ≈ 250~350
// 过热时 ADC < 200（建议暂停打印）
int ThermalPrinter::readTempADC() {
  return analogRead(_cfg.temp);
}

// ============================================================
// 参数调节（运行时修改）
// ============================================================
void ThermalPrinter::setHeatTimeMax(uint16_t ms) { _cfg.heatTimeMaxMs = ms; }
void ThermalPrinter::setHeatTimeMin(uint16_t ms) { _cfg.heatTimeMinMs = ms; }
void ThermalPrinter::setMotorDelay(uint16_t us) { _cfg.motorDelayUs = us; }
void ThermalPrinter::setStepsPerLine(uint8_t steps) { _cfg.stepsPerLine = steps; }
void ThermalPrinter::setHeatPasses(uint8_t passes) { _cfg.heatPasses = passes; }
void ThermalPrinter::setLineCool(uint16_t ms) { _cfg.lineCoolMs = ms; }
void ThermalPrinter::setSplitDots(bool on) { _cfg.splitDots = on; }
void ThermalPrinter::setSplitThreshold(uint16_t n) { _cfg.splitThreshold = n; }
void ThermalPrinter::setVbatPin(uint8_t pin) { _cfg.vbatPin = pin; if (pin != 255) pinMode(pin, INPUT); }
uint16_t ThermalPrinter::getSplitThreshold() const { return _cfg.splitThreshold; }
uint16_t ThermalPrinter::getLineCool() const { return _cfg.lineCoolMs; }
uint16_t ThermalPrinter::getHeatTimeMax() const { return _cfg.heatTimeMaxMs; }
uint16_t ThermalPrinter::getHeatTimeMin() const { return _cfg.heatTimeMinMs; }
uint16_t ThermalPrinter::getMotorDelay() const { return _cfg.motorDelayUs; }
uint8_t  ThermalPrinter::getStepsPerLine() const { return _cfg.stepsPerLine; }
uint8_t  ThermalPrinter::getHeatPasses() const { return _cfg.heatPasses; }
bool ThermalPrinter::isPowered() const { return _powered; }

// ---- 旧接口兼容 ----
void ThermalPrinter::setHeatTime(uint16_t ms) { _cfg.heatTimeMaxMs = ms; }
uint16_t ThermalPrinter::getHeatTime() const { return _cfg.heatTimeMaxMs; }
