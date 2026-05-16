#include <zephyr/sys/util.h>

#include "mode_event.h"

static const char *const mode_name[] = {
	[APP_MODE_USB] = "USB",
	[APP_MODE_24G] = "2.4G",
	[APP_MODE_BLE] = "BLE",
};

static void log_mode_changed_event(const struct app_event_header *aeh)
{
	const struct mode_changed_event *event = cast_mode_changed_event(aeh);

	BUILD_ASSERT(ARRAY_SIZE(mode_name) == 3, "unexpected mode enum size");
	__ASSERT_NO_MSG(event->mode <= APP_MODE_BLE);

	APP_EVENT_MANAGER_LOG(aeh, "mode=%s voltage=%d", mode_name[event->mode],
			      event->voltage_mv);
}

APP_EVENT_TYPE_DEFINE(mode_changed_event,
		      log_mode_changed_event,
		      NULL,
		      APP_EVENT_FLAGS_CREATE(APP_EVENT_TYPE_FLAGS_INIT_LOG_ENABLE));
