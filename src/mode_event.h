#ifndef MODE_EVENT_H_
#define MODE_EVENT_H_

#include <stdint.h>

#include <app_event_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_mode {
	APP_MODE_USB = 0,
	APP_MODE_24G,
	APP_MODE_BLE,
};

struct mode_changed_event {
	struct app_event_header header;
	enum app_mode mode;
	int32_t voltage_mv;
};

APP_EVENT_TYPE_DECLARE(mode_changed_event);

#ifdef __cplusplus
}
#endif

#endif /* MODE_EVENT_H_ */
