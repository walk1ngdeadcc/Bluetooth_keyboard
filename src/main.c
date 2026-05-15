#include <app_event_manager.h>

#define MODULE main
#include <caf/events/module_state_event.h>

#include <zephyr/sys/printk.h>

int main(void)
{
	if (app_event_manager_init()) {
		printk("Application Event Manager init failed\n");
		return -1;
	}

	printk("keyboard app ready\n");
	module_set_state(MODULE_STATE_READY);

	return 0;
}
