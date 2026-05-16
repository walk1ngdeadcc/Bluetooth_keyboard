#include <stdbool.h>
#include <stdint.h>

#include <app_event_manager.h>
#include <caf/events/button_event.h>

#define MODULE usb_hid_module
#include <caf/events/module_state_event.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <nrfx_power.h>

#include "keymap.h"
#include "usb_hid_module.h"

#define USB_VID_ZEPHYR_PROJECT 0x2FE3
#define USB_PID_BLUETOOTH_KEYBOARD 0x0007

#define USB_HID_KEYBOARD_REPORT_ID 1
#define USB_HID_CONSUMER_REPORT_ID 2

enum keyboard_report_index {
	KBD_REPORT_ID_IDX = 0,
	KBD_MODIFIER_IDX,
	KBD_RESERVED_IDX,
	KBD_KEY0_IDX,
	KBD_KEY1_IDX,
	KBD_KEY2_IDX,
	KBD_KEY3_IDX,
	KBD_KEY4_IDX,
	KBD_KEY5_IDX,
	KBD_REPORT_SIZE,
};

static const uint8_t hid_report_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_KEYBOARD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
		HID_REPORT_ID(USB_HID_KEYBOARD_REPORT_ID),
		HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP_KEYPAD),
		HID_USAGE_MIN8(0xE0),
		HID_USAGE_MAX8(0xE7),
		HID_LOGICAL_MIN8(0),
		HID_LOGICAL_MAX8(1),
		HID_REPORT_SIZE(1),
		HID_REPORT_COUNT(8),
		HID_INPUT(0x02),
		HID_REPORT_SIZE(8),
		HID_REPORT_COUNT(1),
		HID_INPUT(0x03),
		HID_REPORT_SIZE(1),
		HID_REPORT_COUNT(5),
		HID_USAGE_PAGE(HID_USAGE_GEN_LEDS),
		HID_USAGE_MIN8(1),
		HID_USAGE_MAX8(5),
		HID_OUTPUT(0x02),
		HID_REPORT_SIZE(3),
		HID_REPORT_COUNT(1),
		HID_OUTPUT(0x03),
		HID_REPORT_SIZE(8),
		HID_REPORT_COUNT(6),
		HID_LOGICAL_MIN8(0),
		HID_LOGICAL_MAX8(101),
		HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP_KEYPAD),
		HID_USAGE_MIN8(0),
		HID_USAGE_MAX8(101),
		HID_INPUT(0x00),
	HID_END_COLLECTION,

	HID_USAGE_PAGE(0x0C),
	HID_USAGE16(0x0001),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
		HID_REPORT_ID(USB_HID_CONSUMER_REPORT_ID),
		HID_LOGICAL_MIN8(0),
		HID_LOGICAL_MAX16(0xFF, 0x03),
		HID_USAGE_MIN8(0),
		HID_USAGE_MAX16(0xFF, 0x03),
		HID_REPORT_SIZE(16),
		HID_REPORT_COUNT(1),
		HID_INPUT(0x00),
	HID_END_COLLECTION,
};

USBD_DEVICE_DEFINE(sample_usbd,
		   DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_nrf_usbd)),
		   USB_VID_ZEPHYR_PROJECT, USB_PID_BLUETOOTH_KEYBOARD);

USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, "SGG");
USBD_DESC_PRODUCT_DEFINE(sample_product, "Bluetooth Keyboard");
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(sample_fs_config, 0, 100, &fs_cfg_desc);

static const struct device *hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
static struct usbd_context *usb_ctx;
static uint8_t keyboard_report[KBD_REPORT_SIZE];
static uint8_t consumer_report[3];
static bool usb_ready;
static bool usb_enabled;
static bool usb_vbus_present;

static void usb_sync_vbus_state(void)
{
	if ((usb_ctx == NULL) || !usbd_can_detect_vbus(usb_ctx)) {
		return;
	}

#if NRF_POWER_HAS_USBREG
	usb_vbus_present =
		(nrfx_power_usbstatus_get() != NRFX_POWER_USB_STATE_DISCONNECTED);
#endif
}

static int usb_hid_setup_device(void)
{
	int err;

	err = usbd_add_descriptor(&sample_usbd, &sample_lang);
	if (err != 0) {
		return err;
	}

	err = usbd_add_descriptor(&sample_usbd, &sample_mfr);
	if (err != 0) {
		return err;
	}

	err = usbd_add_descriptor(&sample_usbd, &sample_product);
	if (err != 0) {
		return err;
	}

	err = usbd_add_configuration(&sample_usbd, USBD_SPEED_FS, &sample_fs_config);
	if (err != 0) {
		return err;
	}

	err = usbd_register_all_classes(&sample_usbd, USBD_SPEED_FS, 1, NULL);
	if (err != 0) {
		return err;
	}

	err = usbd_init(&sample_usbd);
	if (err != 0) {
		return err;
	}

	usb_ctx = &sample_usbd;
	return 0;
}

static void usb_status_cb(struct usbd_context *const usbd_ctx,
			  const struct usbd_msg *const msg)
{
	ARG_UNUSED(usbd_ctx);

	if (msg->type == USBD_MSG_CONFIGURATION) {
		usb_ready = usb_enabled && (msg->status != 0);
	}

	if (usbd_can_detect_vbus(usb_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			usb_vbus_present = true;
			if (usb_enabled) {
				(void)usbd_enable(usb_ctx);
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			usb_vbus_present = false;
			usb_ready = false;
			(void)usbd_disable(usb_ctx);
		}
	}
}

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	usb_ready = usb_enabled && ready;
}

static int kb_get_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(type);
	ARG_UNUSED(id);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);
	return 0;
}

static int kb_set_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(type);
	ARG_UNUSED(id);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);
	return 0;
}

static void kb_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(duration);
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	return 0;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(proto);
}

static void kb_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);
}

static struct hid_device_ops hid_ops = {
	.iface_ready = kb_iface_ready,
	.get_report = kb_get_report,
	.set_report = kb_set_report,
	.set_idle = kb_set_idle,
	.get_idle = kb_get_idle,
	.set_protocol = kb_set_protocol,
	.output_report = kb_output_report,
};

static int usb_hid_submit(const uint8_t *report, size_t size)
{
	if (!usb_enabled || !usb_ready) {
		return -EAGAIN;
	}

	return hid_device_submit_report(hid_dev, size, report);
}

static void keyboard_report_clear_usage(uint8_t usage)
{
	for (size_t i = KBD_KEY0_IDX; i < KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == usage) {
			keyboard_report[i] = 0;
		}
	}
}

static int keyboard_report_set_usage(uint8_t usage)
{
	for (size_t i = KBD_KEY0_IDX; i < KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == usage) {
			return 0;
		}
	}

	for (size_t i = KBD_KEY0_IDX; i < KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == 0) {
			keyboard_report[i] = usage;
			return 0;
		}
	}

	return -ENOBUFS;
}

static void send_consumer_usage(uint16_t usage)
{
	int err;

	consumer_report[0] = USB_HID_CONSUMER_REPORT_ID;
	consumer_report[1] = usage & 0xFF;
	consumer_report[2] = (usage >> 8) & 0xFF;

	err = usb_hid_submit(consumer_report, sizeof(consumer_report));
	if (err != 0) {
		printk("usb hid consumer submit failed: %d\n", err);
		return;
	}

	consumer_report[0] = USB_HID_CONSUMER_REPORT_ID;
	consumer_report[1] = 0;
	consumer_report[2] = 0;
	err = usb_hid_submit(consumer_report, sizeof(consumer_report));
	if (err != 0) {
		printk("usb hid consumer release failed: %d\n", err);
	}
}

static void handle_button_event(const struct button_event *event)
{
	const struct keymap_hid_entry *map = keymap_hid_get(event->key_id);
	int err;

	if (!usb_enabled) {
		return;
	}

	if (map == NULL) {
		return;
	}

	if (map->type == KEYMAP_HID_TYPE_CONSUMER) {
		if (event->pressed) {
			send_consumer_usage(map->usage);
		}
		return;
	}

	if (map->type != KEYMAP_HID_TYPE_KEYBOARD) {
		return;
	}

	if (event->pressed) {
		err = keyboard_report_set_usage((uint8_t)map->usage);
		if (err != 0) {
			printk("usb hid keyboard full: key_id=%u\n", event->key_id);
			return;
		}
	} else {
		keyboard_report_clear_usage((uint8_t)map->usage);
	}

	keyboard_report[KBD_REPORT_ID_IDX] = USB_HID_KEYBOARD_REPORT_ID;
	err = usb_hid_submit(keyboard_report, sizeof(keyboard_report));
	if (err != 0) {
		printk("usb hid keyboard submit failed: %d\n", err);
	}
}

int usb_hid_module_init(void)
{
	int err;

	if (!device_is_ready(hid_dev)) {
		return -ENODEV;
	}

	keyboard_report[KBD_REPORT_ID_IDX] = USB_HID_KEYBOARD_REPORT_ID;

	err = hid_device_register(hid_dev, hid_report_desc, sizeof(hid_report_desc), &hid_ops);
	if (err != 0) {
		return err;
	}

	err = usb_hid_setup_device();
	if (err != 0) {
		return err;
	}

	err = usbd_msg_register_cb(usb_ctx, usb_status_cb);
	if (err != 0) {
		return err;
	}

	usb_sync_vbus_state();

	if (usb_enabled &&
	    (!usbd_can_detect_vbus(usb_ctx) || usb_vbus_present)) {
		err = usbd_enable(usb_ctx);
		if (err != 0) {
			return err;
		}
	}

	printk("usb hid ready: device-next keyboard + consumer control\n");
	module_set_state(MODULE_STATE_READY);

	return 0;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_button_event(aeh)) {
		handle_button_event(cast_button_event(aeh));
		return false;
	}

	return false;
}

int usb_hid_module_release_all(void)
{
	int err;

	memset(&keyboard_report[KBD_MODIFIER_IDX], 0,
	       sizeof(keyboard_report) - KBD_MODIFIER_IDX);
	keyboard_report[KBD_REPORT_ID_IDX] = USB_HID_KEYBOARD_REPORT_ID;

	err = usb_hid_submit(keyboard_report, sizeof(keyboard_report));
	if ((err != 0) && (err != -EAGAIN)) {
		printk("usb hid keyboard release failed: %d\n", err);
	}

	consumer_report[0] = USB_HID_CONSUMER_REPORT_ID;
	consumer_report[1] = 0;
	consumer_report[2] = 0;
	err = usb_hid_submit(consumer_report, sizeof(consumer_report));
	if ((err != 0) && (err != -EAGAIN)) {
		printk("usb hid consumer release failed: %d\n", err);
	}

	return 0;
}

int usb_hid_module_set_enabled(bool enabled)
{
	int err = 0;

	if (!enabled) {
		(void)usb_hid_module_release_all();
		usb_enabled = false;
		if (usb_ctx != NULL) {
			err = usbd_disable(usb_ctx);
			if ((err != 0) && (err != -EALREADY)) {
				return err;
			}
		}
		usb_ready = false;
		return 0;
	}

	usb_enabled = true;

	if (usb_ctx == NULL) {
		return 0;
	}

	usb_sync_vbus_state();

	if (!usbd_can_detect_vbus(usb_ctx) || usb_vbus_present) {
		err = usbd_enable(usb_ctx);
		if ((err != 0) && (err != -EALREADY)) {
			return err;
		}
	}

	return 0;
}

bool usb_hid_module_is_enabled(void)
{
	return usb_enabled;
}

bool usb_hid_module_is_ready(void)
{
	return usb_ready;
}

bool usb_hid_module_has_vbus(void)
{
	return usb_vbus_present;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);
