# USB HID 模块

## 1. 功能

当前 USB HID 模块实现了：

- 矩阵按键作为 USB 键盘输入
- 旋钮作为 Consumer Control 音量加减输入
- `VOLUME_MUTE` 作为 Consumer Control 静音输入

## 2. 输入来源

模块统一订阅 `button_event`：

- 矩阵键：
  - `key_id 0~17`
- 旋钮：
  - `100 -> Volume Up`
  - `101 -> Volume Down`

## 3. HID 报告

### Keyboard Report

用于小键盘键位：

- `NUM_LOCK`
- `KP_SLASH`
- `KP_ASTERISK`
- `KP_MINUS`
- `KP_7`
- `KP_8`
- `KP_9`
- `KP_4`
- `KP_5`
- `KP_6`
- `KP_PLUS`
- `KP_1`
- `KP_2`
- `KP_3`
- `KP_0`
- `KP_DOT`
- `KP_ENTER`

### Consumer Control Report

用于：

- `VOLUME_MUTE`
- `VOLUME_UP`
- `VOLUME_DOWN`

## 4. 事件映射

### Consumer

- `0 -> Mute`
- `100 -> Volume Up`
- `101 -> Volume Down`

### Keyboard

- `1 -> Num Lock`
- `2 -> KP /`
- `3 -> KP *`
- `4 -> KP -`
- `5 -> KP 7`
- `6 -> KP 8`
- `7 -> KP 9`
- `8 -> KP 4`
- `9 -> KP 5`
- `10 -> KP 6`
- `11 -> KP +`
- `12 -> KP 1`
- `13 -> KP 2`
- `14 -> KP 3`
- `15 -> KP 0`
- `16 -> KP .`
- `17 -> KP Enter`

## 5. 代码位置

- 模块实现：[src/usb_hid_module.c](</D:/code/SGGProject/Bluetooth_keyboard/src/usb_hid_module.c:1>)
- 模块头文件：[src/usb_hid_module.h](</D:/code/SGGProject/Bluetooth_keyboard/src/usb_hid_module.h:1>)
- 键位映射：[src/keymap.c](</D:/code/SGGProject/Bluetooth_keyboard/src/keymap.c:1>)
- 初始化入口：[src/main.c](</D:/code/SGGProject/Bluetooth_keyboard/src/main.c:1>)

## 6. RTT 验证

启动预期：

```text
usb hid ready: keyboard report + consumer control report
```

验证方法：

1. 插上 USB 到电脑
2. 打开文本编辑器
3. 测试数字小键盘按键是否输入
4. 测试 `row0 col3` 是否静音
5. 测试旋钮右转是否音量加，左转是否音量减
