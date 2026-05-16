#include <stdbool.h>

#include <app_event_manager.h>

#define MODULE transport_manager
#include <caf/events/module_state_event.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ble_hid_module.h"
#include "mode_event.h"
#include "transport_manager.h"
#include "usb_hid_module.h"

static enum app_mode active_mode = APP_MODE_USB;
static bool initialized;

static void transport_apply_mode(enum app_mode mode)
{
	int err;

	if (initialized && (mode == active_mode)) {
		return;
	}

	err = usb_hid_module_release_all();
	if (err != 0) {
		printk("transport: usb release failed: %d\n", err);
	}

	err = ble_hid_module_release_all();
	if (err != 0) {
		printk("transport: ble release failed: %d\n", err);
	}

	switch (mode) {
	case APP_MODE_USB:
		err = ble_hid_module_set_mode(false);
		if (err != 0) {
			printk("transport: disable ble failed: %d\n", err);
		}

		err = usb_hid_module_set_enabled(true);
		if (err != 0) {
			printk("transport: enable usb failed: %d\n", err);
		}
		break;

	case APP_MODE_BLE:
		err = usb_hid_module_set_enabled(false);
		if (err != 0) {
			printk("transport: disable usb failed: %d\n", err);
		}

		err = ble_hid_module_set_mode(true);
		if (err != 0) {
			printk("transport: enable ble failed: %d\n", err);
		}
		break;

	case APP_MODE_24G:
	default:
		err = usb_hid_module_set_enabled(false);
		if (err != 0) {
			printk("transport: disable usb failed: %d\n", err);
		}

		err = ble_hid_module_set_mode(false);
		if (err != 0) {
			printk("transport: disable ble failed: %d\n", err);
		}
		break;
	}

	active_mode = mode;
	initialized = true;
	printk("transport: active mode=%d\n", mode);
}

int transport_manager_init(void)
{
	module_set_state(MODULE_STATE_READY);
	return 0;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_mode_changed_event(aeh)) {
		const struct mode_changed_event *event = cast_mode_changed_event(aeh);

		transport_apply_mode(event->mode);
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, mode_changed_event);
