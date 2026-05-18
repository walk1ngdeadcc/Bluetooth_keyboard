#include <app_event_manager.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/sys/printk.h>

#include "key_matrix.h"
#include "ble_hid_module.h"
#include "display_module.h"
#include "mode_switch.h"
#include "power_module.h"
#include "rgb_led.h"
#include "rotary.h"
#include "transport_manager.h"
#include "usb_hid_module.h"

int main(void)
{
	int ret;

	if (app_event_manager_init()) {
		printk("Application Event Manager init failed\n");
		return -1;
	}

	ret = rotary_init();
	if (ret != 0) {
		printk("rotary init failed: %d\n", ret);
		return ret;
	}

	ret = power_module_init();
	if (ret != 0) {
		printk("power module init failed: %d\n", ret);
		return ret;
	}

	ret = rgb_led_init();
	if (ret != 0) {
		printk("rgb led init failed: %d\n", ret);
		return ret;
	}

	ret = usb_hid_module_init();
	if (ret != 0) {
		printk("usb hid init failed: %d\n", ret);
		return ret;
	}

	ret = ble_hid_module_init();
	if (ret != 0) {
		printk("ble hid init failed: %d\n", ret);
		return ret;
	}

	ret = transport_manager_init();
	if (ret != 0) {
		printk("transport manager init failed: %d\n", ret);
		return ret;
	}

	ret = display_module_init();
	if (ret != 0) {
		printk("display module init failed: %d\n", ret);
	}

	ret = mode_switch_init();
	if (ret != 0) {
		printk("mode switch init failed: %d\n", ret);
		return ret;
	}

	ret = key_matrix_init();
	if (ret != 0) {
		printk("key matrix init failed: %d\n", ret);
		return ret;
	}

	printk("keyboard app ready\n");
	module_set_state(MODULE_STATE_READY);

	return 0;
}
