# RGB 灯带实现与验证方案

## 1. 目标

第一阶段目标只做一件事：

- 上电后让全部 `17` 颗 `WS2812B-Mini-V3J` 统一显示红色

这一版先不做：

- 按键联动灯效
- 分区灯效
- 睡眠/唤醒联动
- 单颗 LED 与 `key_id` 的映射管理

## 2. 已确认硬件条件

根据你给的原理图和仓库现状，当前已确认：

- `RGB_PWR_EN` 接 `P0.13`
- `RGB_DATA` 接 `P0.20`
- LED 型号为 `WS2812B-Mini-V3J`
- LED 串联总数暂按 `17` 颗实现
- 当前工程里 `P0.20` 还没有被别的模块占用
- 当前工程里 `spi3` 已经给屏幕使用，不适合再复用给 RGB

从灯带电源控制图看，`RGB_PWR_EN` 是`高电平使能`：

- `P0.13 = 1` 时，Q4 导通，把 Q2 栅极拉低，`RGB_VCC_OUT` 上电
- `P0.13 = 0` 时，Q2 关断，`RGB_VCC_OUT` 断电

现有工程相关入口：

- 初始化主流程：[src/main.c](</D:/code/SGGProject/Bluetooth_keyboard/src/main.c:17>)
- 主 CMake：[CMakeLists.txt](</D:/code/SGGProject/Bluetooth_keyboard/CMakeLists.txt:8>)
- 板级 DTS：[boards/cc/Bluetooth_keyboard/Bluetooth_keyboard.dts](</D:/code/SGGProject/Bluetooth_keyboard/boards/cc/Bluetooth_keyboard/Bluetooth_keyboard.dts:9>)
- pinctrl：[boards/cc/Bluetooth_keyboard/Bluetooth_keyboard-pinctrl.dtsi](</D:/code/SGGProject/Bluetooth_keyboard/boards/cc/Bluetooth_keyboard/Bluetooth_keyboard-pinctrl.dtsi:1>)

## 3. 手册里对实现最关键的约束

手册路径：

- [WS2812B-Mini-V3J.pdf](</D:/内网通文件/项目/尚硅谷嵌入式项目之蓝牙键盘/2.资料/2.芯片手册/WS2812B-Mini-V3J.pdf>)

本次实现最关键的参数是：

- 供电范围：`3.7V ~ 5.3V`
- 数据速率：`800Kbps`
- 发送顺序：`GRB`
- `RES` 复位时间：低电平 `> 280us`
- 输入高电平阈值：`VIH >= 0.65 * VDD`

这个 `VIH` 很重要：

- 如果 `RGB_VCC_OUT` 实际是 `5V`
- 那么 WS2812 的最小高电平阈值就是 `3.25V`
- `nRF52840` 的 GPIO 数据高电平来自 `3.3V`
- 逻辑裕量会非常小

所以第一轮验证必须确认：

- `RGB_VCC_OUT` 实际电压是多少
- `P0.20` 的高电平在带载情况下是否稳定
- 首颗灯是否存在偶发错色、丢帧、全亮不响应的问题

## 4. 推荐实现方案

### 4.1 第一阶段推荐选型

第一阶段建议优先用 `Zephyr LED Strip + worldsemi,ws2812-gpio`。

原因：

- 你目前明确给出的数据脚只有 `P0.20`
- `ws2812-gpio` 只需要一根数据线，最贴合现有硬件
- 这一阶段只是“全键常亮红色”，刷新频率极低
- 代码改动最少，最适合先把硬件链路打通

不选 `ws2812-spi` 作为第一版的原因：

- SPI 方案通常还要占一个 `SCK`
- 你当前没有给出 RGB 可用的额外时钟引脚
- `spi3` 已被屏幕占用

结论：

- 第一版先用 `GPIO` 驱动把全红跑起来
- 如果后续要做高刷新动画、呼吸灯、跑马灯，再切到独立 `SPI` 实例

### 4.2 软件改动点

建议改动这些文件：

- `prj.conf`
- `boards/cc/Bluetooth_keyboard/Bluetooth_keyboard.dts`
- 新增 `src/rgb_led.c`
- 新增 `src/rgb_led.h`
- `src/main.c`
- `CMakeLists.txt`

### 4.3 DTS 方案

在板级 DTS 里补两类信息：

1. 把 RGB 灯条声明成 `zephyr,led-strip`
2. 给应用层留一个 `RGB_PWR_EN` 的 GPIO 描述

建议结构如下：

```dts
#include <zephyr/dt-bindings/led/led.h>

/ {
	chosen {
		zephyr,led-strip = &kb_rgb;
	};

	rgb_ctrl: rgb-ctrl {
		power-gpios = <&gpio0 13 GPIO_ACTIVE_HIGH>;
	};

	kb_rgb: ws2812 {
		compatible = "worldsemi,ws2812-gpio";
		status = "okay";
		chain-length = <17>;
		color-mapping = <LED_COLOR_ID_GREEN
				 LED_COLOR_ID_RED
				 LED_COLOR_ID_BLUE>;
		reset-delay = <300>;
		gpios = <&gpio0 20 GPIO_ACTIVE_HIGH>;
	};
};
```

这里的关键点：

- `chain-length = <17>` 先按你给的信息写死
- `color-mapping` 必须按手册写成 `GRB`
- `reset-delay = <300>` 给 `280us` 要求留一点余量
- `P0.20` 直接作为单线数据脚

### 4.4 Kconfig 方案

`prj.conf` 里建议增加：

```conf
CONFIG_LED_STRIP=y
CONFIG_WS2812_STRIP_GPIO=y
```

第一阶段不需要再额外开复杂日志配置。

### 4.5 模块设计

建议新增一个轻量模块：

```c
int rgb_led_init(void);
int rgb_led_set_all_red(uint8_t level);
int rgb_led_off(void);
```

实现流程建议：

1. 读取 `rgb_ctrl.power-gpios`
2. 把 `P0.13` 配成 `GPIO_OUTPUT_INACTIVE`
3. 获取 `zephyr,led-strip` 设备并检查 `device_is_ready()`
4. 拉高 `P0.13` 给 `RGB_VCC_OUT` 上电
5. 延时 `1ms ~ 10ms`
6. 组一个 `17` 长度的 `struct led_rgb pixels[]`
7. 全部写成红色
8. 调 `led_strip_update_rgb()`

建议初始化时序放在 `power_module_init()` 之后，因为：

- 电源模块已经先完成 IP5306 保活和 ADC 初始化
- RGB 负载不应该比电源初始化更早挂上去

在 [src/main.c](</D:/code/SGGProject/Bluetooth_keyboard/src/main.c:17>) 里，推荐插在 `power_module_init()` 成功之后。

### 4.6 亮度建议

第一版不要直接用 `255` 全亮首测。

原因：

- 红灯单颗满亮电流要按较高上限做保守估算
- `17` 颗一起点亮时，总电流可能明显上升
- 首测阶段更需要先验证链路正确，再放大亮度

建议首版默认值：

- `red = 0x20`
- `green = 0x00`
- `blue = 0x00`

验证电流和稳定性没问题后，再逐步提高到：

- `0x40`
- `0x80`
- 最后再评估是否允许 `0xFF`

### 4.7 为什么这一版不做键位映射

当前代码里的按键逻辑 ID 一共有 `18` 个：

- 参考 [src/key_matrix.c](</D:/code/SGGProject/Bluetooth_keyboard/src/key_matrix.c:30>)

但你提供的信息里 LED 串是 `17` 颗。

这意味着：

- “按键逻辑顺序”和“灯珠物理顺序”大概率不是 1:1
- 做“全红”完全不受影响
- 但后续做“按键对应单灯”前，必须补一份 `LED index -> 实际按键位置` 映射表

## 5. 备选方案

如果后续你要做高频动画，建议第二阶段切换到 `ws2812-spi`：

- 单次刷新更稳定
- 不需要在发送期间长时间关中断
- 对蓝牙和复杂 UI 干扰更小

但它的前提是：

- 额外确认一个可用的 `SCK`
- 新增一个独立 SPI 实例，例如 `spi2`
- 在 pinctrl 里给这个 SPI 单独配引脚

所以这不是首版 bring-up 的最优路径。

## 6. 验证方案

### 6.1 上电前静态检查

先确认这几件事：

- `P0.13` 没有被别的外设复用
- `P0.20` 没有被别的外设复用
- `RGB_VCC_OUT` 与 `GND` 没有短路
- 首颗 `WS2812` 的 `DIN` 确认就是 `P0.20` 这一路

### 6.2 电源使能验证

步骤：

1. 固件先只配置 `P0.13`
2. 输出低电平
3. 测 `RGB_VCC_OUT`
4. 再输出高电平
5. 再测 `RGB_VCC_OUT`

预期：

- `P0.13 = 0` 时，`RGB_VCC_OUT` 断电或接近 `0V`
- `P0.13 = 1` 时，`RGB_VCC_OUT` 正常上电

如果这一步不对，先不要查 WS2812 驱动，先查电源控制极性和焊接。

### 6.3 数据波形验证

建议用示波器看 `P0.20`：

- 是否真的有数据脉冲
- 空闲后是否有 `> 280us` 的低电平复位窗口
- 上电后第一帧是否只发一次或少量几次

如果固件按 `GRB` 发送，首轮测试建议只发纯红：

- `G=0`
- `R=0x20`
- `B=0`

预期现象：

- 所有灯显示红色
- 不应显示成绿色或蓝色

如果颜色不对，第一优先检查：

- `color-mapping` 是否写成 `GRB`

### 6.4 逻辑电平裕量验证

这是本方案里最关键的硬件风险项。

步骤：

1. 测 `RGB_VCC_OUT` 直流电压
2. 测 `P0.20` 高电平峰值
3. 对照手册 `VIH >= 0.65 * VDD`

判定建议：

- 如果 `RGB_VCC_OUT <= 4.2V`，`3.3V` 数据高电平通常更有把握
- 如果 `RGB_VCC_OUT` 接近 `5V`，则 `P0.20` 逻辑高电平裕量非常小，要重点观察稳定性

若出现这些现象：

- 首颗灯偶发不亮
- 偶发错色
- 反复上电表现不一致
- 某些时候全串不响应

优先怀疑：

- `3.3V -> 5V` 数据电平裕量不足

这时的处理建议是：

- 确认 RGB 供电是否其实不是 5V
- 如果确实是 5V，考虑补数据电平转换

### 6.5 整串点亮验证

按下面顺序做：

1. 全灭
2. 全红低亮度 `0x20`
3. 全红中亮度 `0x40`
4. 全红更高亮度 `0x80`

每一步都观察：

- 是否全部 `17` 颗都亮
- 是否有中间某颗之后全灭
- 是否有个别灯颜色偏差
- 是否有闪烁

如果“前几颗亮，后面全灭”，优先怀疑：

- 某一颗 `DOUT -> DIN` 级联链路断开

### 6.6 电流与温升验证

建议用电源表或 USB 表记录：

- 待机电流
- 打开 RGB 低亮红灯电流
- 打开 RGB 中亮红灯电流
- 更高亮度时的电流

观察点：

- 亮度升高时系统是否重启
- `IP5306` 保活是否异常
- 屏幕、蓝牙、按键扫描是否受影响
- RGB 区域是否明显发热

### 6.7 软件日志验证

建议 RGB 模块加几条最小化日志：

```text
rgb led strip ready: count=17 data=P0.20 pwr=P0.13
rgb power on
rgb set all red: level=0x20
```

如果 `led_strip_update_rgb()` 返回错误，也要打印：

```text
rgb update failed: <err>
```

## 7. 实施顺序建议

建议按这个顺序推进：

1. 只做 `P0.13` 电源开关验证
2. 接入 `ws2812-gpio`，先点亮全红低亮度
3. 验证 `17` 颗全亮和颜色正确
4. 验证电流与稳定性
5. 再决定默认亮度
6. 最后再考虑睡眠熄灯、模式联动、按键灯效

## 8. 我建议的默认结论

如果现在就开始写代码，我建议按下面的结论直接落：

- 驱动方案：`worldsemi,ws2812-gpio`
- 数据脚：`P0.20`
- 电源使能脚：`P0.13`
- 灯珠数量：`17`
- 颜色顺序：`GRB`
- 首版默认亮度：`R=0x20`
- 初始化时机：`power_module_init()` 之后

这样改动最小，最适合先把“全键亮红灯”稳定点亮。
