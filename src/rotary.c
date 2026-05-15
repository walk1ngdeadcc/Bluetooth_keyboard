#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <app_event_manager.h>

#define MODULE rotary
#include <caf/events/button_event.h>
#include <caf/events/keep_alive_event.h>
#include <caf/events/module_state_event.h>
#include <caf/events/power_event.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/usb/class/hid.h>

#include "rotary.h"

#define ROTARY_NODE DT_NODELABEL(rotary_qdec)
#define ROTARY_STEP_ANGLE_DEG 12
#define ROTARY_KEY_ID_VOLUME_UP 100
#define ROTARY_KEY_ID_VOLUME_DOWN 101

BUILD_ASSERT(DT_NODE_HAS_STATUS(ROTARY_NODE, okay),
	     "rotary_qdec node must be enabled");

static const struct device *const rotary_dev = DEVICE_DT_GET(ROTARY_NODE);
static struct k_spinlock rotary_lock;
static int32_t rotary_total_angle_deg;
static bool rotary_in_low_power;
static bool rotary_wake_sent;

static void rotary_send_volume_key(int32_t direction)
{
	uint16_t key_id = (direction > 0) ? ROTARY_KEY_ID_VOLUME_UP :
					    ROTARY_KEY_ID_VOLUME_DOWN;
	struct button_event *event;

	printk("rotary volume %s usage=0x%04x\n",
	       (direction > 0) ? "up" : "down",
	       (direction > 0) ? 0x00E9 : 0x00EA);

	event = new_button_event();
	event->key_id = key_id;
	event->pressed = true;
	APP_EVENT_SUBMIT(event);

	event = new_button_event();
	event->key_id = key_id;
	event->pressed = false;
	APP_EVENT_SUBMIT(event);
}

static void rotary_input_callback(struct input_event *evt, void *user_data)
{
	int32_t step_count;
	int32_t angle_delta;
	int32_t total_angle;
	const char *direction;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_REL || evt->code != INPUT_REL_WHEEL || evt->value == 0) {
		return;
	}

	if (rotary_in_low_power) {
		if (!rotary_wake_sent) {
			rotary_wake_sent = true;
			APP_EVENT_SUBMIT(new_wake_up_event());
		}
	} else {
		keep_alive();
	}

	rotary_send_volume_key(evt->value);

	step_count = abs(evt->value);
	angle_delta = evt->value * ROTARY_STEP_ANGLE_DEG;
	direction = (evt->value > 0) ? "clockwise" : "counterclockwise";

	k_spinlock_key_t key = k_spin_lock(&rotary_lock);

	rotary_total_angle_deg += angle_delta;
	total_angle = rotary_total_angle_deg;

	k_spin_unlock(&rotary_lock, key);

	printk("rotary %s steps=%d total_angle=%d度 value=%d\n",
	       direction, step_count, total_angle, evt->value);
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(ROTARY_NODE), rotary_input_callback, NULL);

int rotary_init(void)
{
	if (!device_is_ready(rotary_dev)) {
		return -ENODEV;
	}

	printk("rotary gpio-qdec ready: A=P0.10 B=P1.06 steps-per-period=2 sample=2000us idle-poll=2000us idle-timeout=200ms axis=INPUT_REL_WHEEL\n");
	module_set_state(MODULE_STATE_READY);

	return 0;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_power_down_event(aeh)) {
		rotary_in_low_power = true;
		rotary_wake_sent = false;
		module_set_state(MODULE_STATE_STANDBY);
		return false;
	}

	if (is_wake_up_event(aeh)) {
		rotary_in_low_power = false;
		rotary_wake_sent = false;
		module_set_state(MODULE_STATE_READY);
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, wake_up_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, power_down_event);
