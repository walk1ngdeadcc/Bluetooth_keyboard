# USB 上位机 RGB 控制实现说明

## 1. 目标

当前这一版的目标是：

- 保留原有 `USB HID` 键盘功能
- 额外新增一条 `USB CDC ACM` 通信通道
- 让上位机通过协议下发 `ThemeRgb`，直接修改 17 颗按键灯颜色

这一版暂不做：

- `BLE NUS` 通道
- `TimeSync`
- `Bitmap`
- `LedState`
- `FunctionKeyEvent`
- 单键 RGB 配置

## 2. 参考资料

协议来源：

- [上位机通信协议.md](</D:/内网通文件/项目/尚硅谷嵌入式项目之蓝牙键盘/2.资料/3.键盘上位机软件/上位机通信协议.md>)

上位机测试脚本：

- [host_cli.py](</D:/内网通文件/项目/尚硅谷嵌入式项目之蓝牙键盘/2.资料/3.键盘上位机软件/upper_computer_tools/host_cli.py>)

## 3. 当前实现范围

当前固件只实现了最小闭环：

- `HelloReq`
- `HelloRsp`
- `ThemeRgb`
- `Response`

其中：

- `HelloReq` 用于会话握手
- `HelloRsp` 返回协议版本、VID、PID、能力位
- `ThemeRgb` 用于设置全键颜色
- `Response` 返回处理结果

当前 `HelloRsp.capability_flags = 0x04`，表示当前只开放 `ThemeRgb` 能力。

## 4. 传输方式

当前实现采用：

- 物理层：`USB CDC ACM`
- 帧格式：`0x55 0xAA + Len(1B) + Payload`
- 消息编码：按协议文档中的 protobuf 字段号手工编解码

说明：

- 这条通道和原有 `USB HID` 键盘共存
- 电脑连接后会同时枚举出键盘设备和一个串口设备
- 串口仅用于上位机协议，不参与键值 HID 上报

## 5. 工程落点

### 5.1 新增模块

- 协议模块：[src/usb_host_proto.c](</D:/code/SGGProject/Bluetooth_keyboard/src/usb_host_proto.c:1>)
- 头文件：[src/usb_host_proto.h](</D:/code/SGGProject/Bluetooth_keyboard/src/usb_host_proto.h:1>)

职责：

- 管理 `CDC ACM` 串口收发
- 做帧同步与拆包
- 解析 `HelloReq` / `ThemeRgb`
- 生成 `HelloRsp` / `Response`
- 调用 RGB 模块改色

### 5.2 RGB 模块扩展

新增全键 RGB 设置接口：

- [src/rgb_led.h](</D:/code/SGGProject/Bluetooth_keyboard/src/rgb_led.h:6>)
- [src/rgb_led.c](</D:/code/SGGProject/Bluetooth_keyboard/src/rgb_led.c:209>)

新增接口：

```c
int rgb_led_set_all(uint8_t red, uint8_t green, uint8_t blue);
```

作用：

- 保持原有上电自检和全红逻辑不变
- 允许协议层直接设置 17 颗灯的统一颜色

### 5.3 启动流程接入

主流程里新增协议模块初始化：

- [src/main.c](</D:/code/SGGProject/Bluetooth_keyboard/src/main.c:52>)

执行顺序：

1. `rgb_led_init()`
2. `usb_hid_module_init()`
3. `usb_host_proto_init()`

这样做的目的是：

- 先保证 RGB 模块已经可用
- 再启用协议通道
- 避免协议先收到颜色命令，但 RGB 还没初始化

### 5.4 USB 设备树接入

在现有 `&usbd` 下新增 `cdc_acm_uart0`：

- [app.overlay](</D:/code/SGGProject/Bluetooth_keyboard/app.overlay:32>)

作用：

- 在同一个 USB Device Controller 上同时挂 `HID + CDC ACM`

### 5.5 Kconfig 接入

新增配置位于：

- [prj.conf](</D:/code/SGGProject/Bluetooth_keyboard/prj.conf>)

关键选项：

- `CONFIG_SERIAL=y`
- `CONFIG_UART_INTERRUPT_DRIVEN=y`
- `CONFIG_UART_LINE_CTRL=y`
- `CONFIG_USBD_CDC_ACM_CLASS=y`
- `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n`

### 5.6 USB 状态联动

为了不新增第二套 USB 设备管理，协议模块复用了现有 HID 模块的 USB 状态回调：

- [src/usb_hid_module.c](</D:/code/SGGProject/Bluetooth_keyboard/src/usb_hid_module.c:158>)

作用：

- 接收 `DTR` 变化
- 接收 `VBUS` / `RESET` 状态
- 控制协议会话从 `DOWN -> WAIT_HELLO -> ACTIVE`

## 6. 会话行为

### 6.1 状态机

当前协议通道只有 3 个状态：

- `DOWN`
- `WAIT_HELLO`
- `ACTIVE`

行为如下：

- USB 未连接或串口未打开：`DOWN`
- 上位机打开串口并拉起 `DTR`：进入 `WAIT_HELLO`
- 收到合法 `HelloReq`：进入 `ACTIVE`
- 断开串口、USB 拔出、模式切走 USB：回到 `DOWN`

### 6.2 当前支持的消息

#### HelloReq

要求：

- `protocol_version = 1`

处理结果：

- 合法：回复 `HelloRsp`
- 非法：回复 `Response(INVALID_PARAM)`

#### ThemeRgb

要求：

- `red <= 255`
- `green <= 255`
- `blue <= 255`
- 会话状态必须已进入 `ACTIVE`

处理结果：

- 合法：调用 `rgb_led_set_all(r, g, b)`，再回复 `Response(OK)`
- 参数越界：回复 `Response(INVALID_PARAM)`
- 未握手：回复 `Response(NOT_READY)`

## 7. 上位机使用方式

### 7.1 前提条件

- 键盘拨到 `USB` 档
- 固件已经刷入当前版本
- 电脑识别到一个新的 `CDC ACM` 串口

### 7.2 扫描端口

```bash
python host_cli.py scan
```

如果握手成功，脚本会打印 `HelloRsp` 信息。

### 7.3 设置颜色

全红：

```bash
python host_cli.py COM3 rgb 255 0 0
```

全绿：

```bash
python host_cli.py COM3 rgb 0 255 0
```

全蓝：

```bash
python host_cli.py COM3 rgb 0 0 255
```

自定义颜色：

```bash
python host_cli.py COM3 rgb 76 158 245
```

说明：

- `COM3` 仅为示例，实际以电脑识别到的串口号为准
- 上位机脚本会先发 `HelloReq`，再发 `ThemeRgb`

## 8. 验证方案

### 8.1 编译验证

当前工程已完成整编通过，目标板：

- `Bluetooth_keyboard/nrf52840`

### 8.2 功能验证

按下面顺序验证：

1. 上电后确认 RGB 自检仍然正常
2. 拨到 `USB` 档并连接电脑
3. 确认系统出现新的 `CDC ACM` 串口
4. 执行 `python host_cli.py scan`
5. 执行 `python host_cli.py COMx rgb 255 0 0`
6. 再分别验证绿色、蓝色、自定义颜色

预期结果：

- `scan` 能收到 `HelloRsp`
- `rgb` 命令返回成功
- 17 颗按键灯统一切换到指定颜色
- 键盘 HID 输入功能仍然正常

### 8.3 日志验证

建议观察 RTT 日志，关键日志包括：

- `usb host proto ready: cdc_acm_uart0`
- `usb host proto link ready, wait hello`
- `usb host proto active`
- `usb host proto theme rgb: r=... g=... b=...`

## 9. 当前限制

- 只支持 `USB` 通道，不支持 `BLE NUS`
- 只支持全键统一颜色，不支持单键颜色
- 只支持 `ThemeRgb`，其余协议消息未实现
- 切到 `BLE` 或 `2.4G` 档时，USB 会被关闭，CDC 串口也会消失

## 10. 后续建议

下一步建议按下面顺序继续扩展：

1. 补 `TimeSync`
2. 让上位机 GUI 按同样协议直接对接
3. 如有需要，再补 `BLE NUS`
4. 最后再考虑单键 RGB 或更复杂灯效
