# NumLock 自定义功能实现说明

## 目标

按下 `Num Lock` 后，小键盘 `1~9` 进入自定义功能层。

- USB 模式：
  - 通过现有 `CDC ACM` 上位机协议发送“槽位触发”
- BLE 模式：
  - 通过自定义 `GATT Notify` 服务发送“槽位触发”

上位机收到槽位后，自行映射为：

- 打开网页
- 打开软件
- 执行脚本
- 其他自定义动作

固件本身不保存“打开哪个网页/软件”，只负责把 `1~9` 变成 `slot1~slot9` 触发。

## 行为规则

### 1. NumLock 状态

- 本地在按下 `Num Lock` 时会先切换一份功能层状态
- 如果主机随后通过 HID Output Report 回写了 `Num Lock LED` 状态，固件会以主机回写状态为准

### 2. 1~9 拦截条件

只有同时满足下面条件时，`1~9` 才会被功能层拦截：

- `NumLock` 当前为开启
- 当前传输模式对应的上位机链路已就绪

否则保持原行为，继续发送普通小键盘数字。

### 3. 回退策略

- USB 模式下，如果 `CDC ACM` 协议还没有完成 `Hello`
  - `1~9` 继续当普通数字发出
- BLE 模式下，如果上位机没有订阅自定义通知特征
  - `1~9` 继续当普通数字发出

## 键位与槽位映射

功能层只接管 `1~9`：

- `KP_1 -> slot 1`
- `KP_2 -> slot 2`
- `KP_3 -> slot 3`
- `KP_4 -> slot 4`
- `KP_5 -> slot 5`
- `KP_6 -> slot 6`
- `KP_7 -> slot 7`
- `KP_8 -> slot 8`
- `KP_9 -> slot 9`

对应 `key_id`：

- `key_id 12 -> slot 1`
- `key_id 13 -> slot 2`
- `key_id 14 -> slot 3`
- `key_id 8 -> slot 4`
- `key_id 9 -> slot 5`
- `key_id 10 -> slot 6`
- `key_id 5 -> slot 7`
- `key_id 6 -> slot 8`
- `key_id 7 -> slot 9`

## USB 协议

沿用现有帧格式：

- 帧头：`55 AA`
- 长度：`Len`
- Payload：类 protobuf 的 TLV 编码

### 新增能力位

- `USB_HOST_PROTO_CAP_CUSTOM_ACTION = 0x08`

### 新增顶层字段

- `PROTO_FIELD_ACTION_TRIGGER = 9`

### ActionTrigger Body

- `field 1 = slot`

示意：

- `ActionTrigger { slot: 1 }`
- `ActionTrigger { slot: 9 }`

设备只在按下时发送一次，不在松开时重复发。

相关代码：

- [src/usb_host_proto.c](/d:/code/SGGProject/Bluetooth_keyboard/src/usb_host_proto.c:519)
- [src/custom_action_layer.c](/d:/code/SGGProject/Bluetooth_keyboard/src/custom_action_layer.c:68)

## BLE 自定义服务

### Service UUID

- `8b1f7d00-c66d-4f18-b7d5-4a8e2c1d9001`

### Trigger Characteristic UUID

- `8b1f7d01-c66d-4f18-b7d5-4a8e2c1d9001`

### Characteristic 属性

- `Read`
- `Notify`

### Notify Payload

- `1 byte`
- 取值范围：`1~9`

例如：

- `0x01` 表示触发 `slot1`
- `0x09` 表示触发 `slot9`

上位机只要连接该 BLE 设备并订阅这个特征，就能收到功能层触发。

相关代码：

- [src/ble_host_action_service.c](/d:/code/SGGProject/Bluetooth_keyboard/src/ble_host_action_service.c:11)
- [src/custom_action_layer.c](/d:/code/SGGProject/Bluetooth_keyboard/src/custom_action_layer.c:51)

## NumLock 状态来源

### USB

从 HID Output Report 解析 `Num Lock LED`：

- [src/usb_hid_module.c](/d:/code/SGGProject/Bluetooth_keyboard/src/usb_hid_module.c:191)

### BLE

从 HIDS Output Report / Boot Keyboard Output Report 解析：

- [src/ble_hid_module.c](/d:/code/SGGProject/Bluetooth_keyboard/src/ble_hid_module.c:632)

## 验证建议

### USB

1. 切到 USB 模式
2. 上位机先完成 `Hello`
3. 按一次 `Num Lock`
4. 按 `KP_1`
5. 确认上位机收到 `slot1`
6. 再按一次 `Num Lock`
7. 按 `KP_1`
8. 确认恢复为普通数字键输入

### BLE

1. 切到 BLE 模式并连接 PC
2. 上位机订阅自定义 Trigger Characteristic
3. 按一次 `Num Lock`
4. 按 `KP_7`
5. 确认收到 `0x07`
6. 取消订阅后再按 `KP_7`
7. 确认按键回退为普通数字输入
