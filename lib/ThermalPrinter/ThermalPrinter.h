/********************************************************
 * ThermalPrinter.h - 热敏打印机驱动库
 *
 * 适用于 ESP32-S3 + jx-2r-17ll 打印头 + SS8833 电机驱动
 *
 * 特性：
 *   - 自适应加热时间——根据实际加热点数动态计算 STB 脉宽
 *   - 温度补偿——根据热敏电阻 ADC 读数自动调整加热时间
 *   - 4 相 8 拍步进驱动——SS8833 电机驱动兼容
 *
 * 接线说明（立创实战派S3）：
 *   电机：EN=47, A1=16, A2=15, B1=17, B2=18
 *   打印头：PVH=48, DAT=6, CLK=7, LAT=43, STB=5
 *   传感器：纸检测(IO3, ADC), 温度检测(IO4, ADC)
 *
 * 注意事项：
 *   - IO3 硬件损坏（立创实战派S3 设计问题），纸传感器功能不可用
 *   - PVH 用于给打印头加热供电，发热量大，注意散热
 *   - STB 脉宽不宜超过 30ms，否则可能烧坏打印头
 *   - MCLK 树赖 PVH 使能，必须先 PVH=HI 再发数据
 *
 * 使用示例：
 *   ThermalPrinter printer;
 *   printer.begin();
 *   printer.printLine(lineBuf);         // 打印一行（自适应加热）
 *   printer.feedSteps(100);             // 走纸100步
 *   printer.printImage(data, w, h);     // 打印位图
 ********************************************************/

#ifndef THERMAL_PRINTER_H
#define THERMAL_PRINTER_H

#include <Arduino.h>

// ===== 打印头参数（勿改，除非换打印头）=====
#define TP_DOTS_PER_LINE  384    // 每行打印点数（jx-2r-17ll 固定）
#define TP_BYTES_PER_LINE (TP_DOTS_PER_LINE / 8)  // 48 字节

// ===== 默认引脚（可根据实际接线修改）=====
#define TP_DEFAULT_MOTOR_EN   47   // 电机使能（兼纸传感器供电，低有效）
#define TP_DEFAULT_MOTOR_A1   16   // 电机绕组 A+ (SS8833 OUT1)
#define TP_DEFAULT_MOTOR_A2   15   // 电机绕组 A- (SS8833 OUT2)
#define TP_DEFAULT_MOTOR_B1   17   // 电机绕组 B+ (SS8833 OUT3)
#define TP_DEFAULT_MOTOR_B2   18   // 电机绕组 B- (SS8833 OUT4)
#define TP_DEFAULT_PVH        48   // 打印头加热供电（P-MOS 控制，高有效）
#define TP_DEFAULT_DAT        6    // 移位寄存器数据 
#define TP_DEFAULT_CLK        7    // 移位寄存器时钟 
#define TP_DEFAULT_LAT        43   // 移位寄存器锁存 
#define TP_DEFAULT_STB        5    // 打印头加热选通（STB，高有效）
#define TP_DEFAULT_PAPER      3    // 纸传感器 ADC 输入（⚠️ 立创实战派S3 此IO硬件损坏）
#define TP_DEFAULT_TEMP       4    // 温度传感器 ADC 输入（热敏电阻）

// ============================================================
// ===== ThermalPrinter 类 =====
// ============================================================
class ThermalPrinter {
public:
  // ===== 配置结构体 =====
  // 【修改打印参数改这里】
  struct Config {
    // ---- 引脚 ----
    uint8_t motorEN  = TP_DEFAULT_MOTOR_EN;
    uint8_t motorA1  = TP_DEFAULT_MOTOR_A1;
    uint8_t motorA2  = TP_DEFAULT_MOTOR_A2;
    uint8_t motorB1  = TP_DEFAULT_MOTOR_B1;
    uint8_t motorB2  = TP_DEFAULT_MOTOR_B2;
    uint8_t pvh      = TP_DEFAULT_PVH;
    uint8_t dat      = TP_DEFAULT_DAT;
    uint8_t clk      = TP_DEFAULT_CLK;
    uint8_t lat      = TP_DEFAULT_LAT;
    uint8_t stb      = TP_DEFAULT_STB;
    uint8_t paper    = TP_DEFAULT_PAPER;
    uint8_t temp     = TP_DEFAULT_TEMP;

    // ---- 加热参数（影响打印质量和速度）----
    // 【调节打印深度的参数】
    uint16_t heatTimeMaxMs  = 10;   // 最大加热时间(ms)：全部 384 点都打时
                                     //   ⚠️ 超过 30ms 可能烧坏打印头！
                                     //   越大→越深（黑）但越慢，且温度升高
    uint16_t heatTimeMinMs  = 5;   // 最小加热时间(ms)：只有少量点时
                                     //   越小→速度越快但可能浅色断线
    uint16_t motorDelayUs   = 2000; // 步进电机间隔(us)
                                     //   越小→走纸越快但力矩小可能卡纸
                                     //   越大→力矩大但走纸慢且有噪音
    uint8_t  stepsPerLine   = 4;    // 每行走纸步数
                                     //   4步≈0.13mm（标准打印精度）
                                     //   更多步→行间距大，更少步→行重叠
    uint8_t  heatPasses     = 1;    // 加热次数
                                     //   1=单次加热（标准）
                                     //   2=两次加热（加深深色，适合图片）
    uint16_t lineCoolMs     = 0;    // ★ 每行打印后的额外冷却(ms)
                                     //   0=不额外等（标准）
                                     //   电源带不动打印头（打一半电机停/字断）时调到 5~15，
                                     //   用"降平均电流"换稳定 —— 代价是打印变慢。
    bool     splitDots      = false; // ★ 大黑块行拆成"左半 + 右半"两次加热
                                     //   全黑一行 384 点同时通电 ≈ 2~3A，电池保护板会直接
                                     //   过流保护（现象：纸走一下就不动、只打出顶上一条）。
                                     //   拆成两次 → 峰值电流减半（每次最多 192 点），
                                     //   总能量不变、黑度基本不变，代价是打印变慢。
    uint16_t splitThreshold = 200;   //   点数超过它才拆（小图案不拆，不白慢）

    // ---- 诊断：打印期间量电池电压（可选）----
    // ★ 设成电池 ADC 引脚（见 ai_config.h PIN_BAT_ADC=9）后，每打一行顺手读一次，
    //   打完在串口报"空闲电压 vs 打印中最低电压"—— 不用示波器就能看出电源塌没塌。
    //   255 = 关闭（默认，库不依赖任何板级配置）。
    uint8_t  vbatPin       = 255;

    // ---- ★ VH 供电极性（2026-10-04 排障新增）----
    //   GPIO48 那颗 P-MOS 的栅极接法决定极性：
    //     false = 高有效（栅极拉低=关，拉高=开）—— 库的原有行为
    //     true  = 低有效（栅极上拉到电池、MCU 拉低才导通，和原理图 VDDCTL/Q3 同款接法）
    //   ★ 极性写反的症状恰好就是当前故障：打印中 VH 实际被关断 →
    //     电机没 VM 没力、打印头不加热、几乎无电流（VBAT 不掉）、NTC 读数乱飘。
    bool     pvhActiveLow  = false;
  };

  ThermalPrinter();
  explicit ThermalPrinter(const Config& cfg);

  // ---- 初始化 ----
  // 初始化所有 GPIO（输出模式/初始值）
  // 必须在第一次打印前调用一次
  void begin();

  // ---- 电机控制 ----
  void motorStep();                     // 单步前进
  void motorStepReverse();              // 单步后退（退纸）
  void motorRunSteps(uint32_t steps, bool forward = true);  // 连续跑N步
  void motorOff();                      // 关闭线圈（省电、防过热）

  // ---- 电源控制 ----
  void powerOn();                       // 开 PVH（打印头加热供电）
  void powerOff();                      // 关 PVH（必须关！否则持续发热）

  // ---- 走纸 ----
  void feedSteps(uint32_t steps);       // 走纸 N 步（只能正转）
  void feedLines(uint16_t lines);       // 走纸 N 行（= N * stepsPerLine 步）

  // ---- 打印 ----
  // 打印一行(48字节) + 走纸
  // data: 48字节位图数据，每字节对应8个打印点
  //   bit7=最左点, bit0=最右点, 1=加热打印, 0=空白
  void printLine(const uint8_t* data);

  // 打印一行，不走纸（用于预制多行+一次性走纸）
  void printLineNoFeed(const uint8_t* data);

  // 打印整张位图
  // data: 位图数据（每字节8点，每行 bytesPerRow 字节）
  // width: 位图宽度（像素），不能超过 384
  // height: 位图高度（像素），即总行数
  void printImage(const uint8_t* data, uint16_t width, uint16_t height);

  // ---- 传感器 ----
  bool hasPaper();                // 检纸（⚠️ IO3 硬件损坏，不可用）
  int  readPaperADC();            // 读取纸传感器 ADC 原始值
  int  readTempADC();             // 读取温度传感器 ADC 原始值

  // ---- 参数动态调节 ----
  void setHeatTimeMax(uint16_t ms);     // 改最大加热时间
  void setHeatTimeMin(uint16_t ms);     // 改最小加热时间
  void setMotorDelay(uint16_t us);       // 改步进间隔
  void setStepsPerLine(uint8_t steps);  // 改每行走纸步数
  void setHeatPasses(uint8_t passes);   // 改加热次数
  void setLineCool(uint16_t ms);         // ★ 改每行额外冷却时间
  void setSplitDots(bool on);            // ★ 开关"大黑块行分两次加热"（降峰值电流）
  void setSplitThreshold(uint16_t n);    // ★ 改"超过多少点才拆"的门限
  void setVhActiveLow(bool low);         // ★ 改 VH 供电极性（见 Config.pvhActiveLow 注释）
                                         //   （只有 setSplitDots 没有它 = 门限写了也没用）
  // ★★ VH 保持闩锁（老陈 2026-10-04：打印/走纸全程 GPIO48 必须恒高）
  //   原因：电机驱动 AB8833 的 VM 脚就是 VH（原理图 U16-12），VH 断一下电机就丢电丢步。
  //   用法：把一段完整动作包起来 ——  vhHold(true); …printImage…feedLines…  vhHold(false);
  //   期间所有 powerOff()（printImage 结尾、feedSteps 结尾…）都不会真正关 VH，
  //   整段动作 VH 只开一次、关一次。
  void vhHold(bool on);
  void setVbatPin(uint8_t pin);          // ★ 诊断：打印期间监视电池电压（255=关）
  uint16_t getHeatTimeMax() const;
  uint16_t getHeatTimeMin() const;
  uint16_t getMotorDelay() const;
  uint8_t  getStepsPerLine() const;
  uint8_t  getHeatPasses() const;
  uint16_t getLineCool() const;
  uint16_t getSplitThreshold() const;

  // ---- 状态查询 ----
  bool isPowered() const;         // 打印头是否正在供电

  // ---- 兼容旧接口 ----
  void setHeatTime(uint16_t ms);         // 同 setHeatTimeMax
  uint16_t getHeatTime() const;          // 同 getHeatTimeMax

private:
  Config _cfg;
  uint8_t _motorPos = 0;   // 当前步进相位 (0~7)
  bool _powered = false;   // PVH 状态
  bool _vhHold  = false;   // ★ VH 保持闩锁：true 时 powerOff() 不真正关 VH

  // 4相8拍步进序列（SS8833 双H桥）
  // 数组: [AIN1, AIN2, BIN1, BIN2] → 对应 SS8833 的 IN1~IN4
  // 8拍序列提供更大的转矩和更精确的步进
  static const uint8_t STEP_SEQ[8][4];

  // ---- 内部函数 ----
  void sendLine(const uint8_t* data);          // 发送48字节到移位寄存器
  void fireStrobe(uint16_t dotCount);          // LAT 锁存 + STB 加热
  uint16_t calcHeatTime(uint16_t dotCount, int tempADC);  // 计算加热时间
  static uint16_t countDots(const uint8_t* data);         // 统计加热点数
};

#endif // THERMAL_PRINTER_H
