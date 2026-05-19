#ifndef USB_HOST_PROTO_H_
#define USB_HOST_PROTO_H_

#include <stdbool.h>
#include <stdint.h>

struct usbd_msg;

int usb_host_proto_init(void);
void usb_host_proto_handle_usbd_msg(const struct usbd_msg *msg);
void usb_host_proto_set_usb_enabled(bool enabled);
bool usb_host_proto_is_active(void);
int usb_host_proto_send_action_trigger(uint8_t slot);

#endif /* USB_HOST_PROTO_H_ */
