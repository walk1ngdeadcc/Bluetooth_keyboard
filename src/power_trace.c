#include <app_event_manager.h>
#include <caf/events/power_event.h>

#include <zephyr/sys/printk.h>

#define MODULE power_trace

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_power_down_event(aeh)) {
		printk("进入低功耗\n");
		return false;
	}

	if (is_wake_up_event(aeh)) {
		printk("退出低功耗\n");
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, power_down_event);
APP_EVENT_SUBSCRIBE(MODULE, wake_up_event);
