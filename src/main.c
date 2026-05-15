#include <app_event_manager.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/sys/printk.h>

#include "power_module.h"
#include "rotary.h"

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

	printk("keyboard app ready\n");
	module_set_state(MODULE_STATE_READY);

	return 0;
}
