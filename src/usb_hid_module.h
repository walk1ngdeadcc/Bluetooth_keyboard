#ifndef USB_HID_MODULE_H_
#define USB_HID_MODULE_H_

#include <stdbool.h>

int usb_hid_module_init(void);
int usb_hid_module_set_enabled(bool enabled);
int usb_hid_module_release_all(void);

#endif /* USB_HID_MODULE_H_ */
