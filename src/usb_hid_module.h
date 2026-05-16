#ifndef USB_HID_MODULE_H_
#define USB_HID_MODULE_H_

#include <stdbool.h>

int usb_hid_module_init(void);
int usb_hid_module_set_enabled(bool enabled);
int usb_hid_module_release_all(void);
bool usb_hid_module_is_enabled(void);
bool usb_hid_module_is_ready(void);
bool usb_hid_module_has_vbus(void);

#endif /* USB_HID_MODULE_H_ */
