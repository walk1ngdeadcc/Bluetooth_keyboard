#ifndef BLE_HID_MODULE_H_
#define BLE_HID_MODULE_H_

#include <stdbool.h>

int ble_hid_module_init(void);
int ble_hid_module_set_mode(bool enabled);
int ble_hid_module_release_all(void);
bool ble_hid_module_is_enabled(void);
bool ble_hid_module_is_ready(void);
bool ble_hid_module_is_connected(void);
bool ble_hid_module_is_advertising(void);

#endif /* BLE_HID_MODULE_H_ */
