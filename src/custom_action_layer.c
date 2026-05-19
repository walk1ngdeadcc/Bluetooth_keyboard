#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <app_event_manager.h>
#include <caf/events/button_event.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "ble_hid_module.h"
#include "ble_host_action_service.h"
#include "custom_action_layer.h"
#include "mode_event.h"
#include "transport_manager.h"
#include "usb_host_proto.h"

#define MODULE custom_action_layer

#define CUSTOM_ACTION_NUM_LOCK_KEY_ID 1U

static const uint8_t custom_action_slot_by_key_id[] = {
	[5] = 7,
	[6] = 8,
	[7] = 9,
	[8] = 4,
	[9] = 5,
	[10] = 6,
	[12] = 1,
	[13] = 2,
	[14] = 3,
};

static bool custom_action_num_lock_active;
static uint32_t custom_action_consumed_mask;

static void custom_action_clear_consumed(void)
{
	custom_action_consumed_mask = 0U;
}

static uint8_t custom_action_key_id_to_slot(uint16_t key_id)
{
	if (key_id >= ARRAY_SIZE(custom_action_slot_by_key_id)) {
		return 0U;
	}

	return custom_action_slot_by_key_id[key_id];
}

static bool custom_action_link_ready(void)
{
	switch (transport_manager_get_active_mode()) {
	case APP_MODE_USB:
		return usb_host_proto_is_active();

	case APP_MODE_BLE:
		return ble_hid_module_is_enabled() &&
		       ble_hid_module_is_connected() &&
		       ble_host_action_service_is_ready();

	case APP_MODE_24G:
	default:
		return false;
	}
}

static int custom_action_send_slot(uint8_t slot)
{
	switch (transport_manager_get_active_mode()) {
	case APP_MODE_USB:
		return usb_host_proto_send_action_trigger(slot);

	case APP_MODE_BLE:
		return ble_host_action_service_send_trigger(slot);

	case APP_MODE_24G:
	default:
		return -ENOTSUP;
	}
}

int custom_action_layer_init(void)
{
	custom_action_num_lock_active = false;
	custom_action_clear_consumed();
	printk("custom action layer ready\n");
	return 0;
}

void custom_action_layer_set_num_lock(bool enabled)
{
	if (custom_action_num_lock_active == enabled) {
		return;
	}

	custom_action_num_lock_active = enabled;
	custom_action_clear_consumed();
	printk("custom action numlock=%d\n", enabled);
}

bool custom_action_layer_is_num_lock_active(void)
{
	return custom_action_num_lock_active;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_mode_changed_event(aeh)) {
		custom_action_clear_consumed();
		return false;
	}

	if (!is_button_event(aeh)) {
		return false;
	}

	const struct button_event *event = cast_button_event(aeh);
	uint8_t slot = custom_action_key_id_to_slot(event->key_id);

	if ((event->key_id == CUSTOM_ACTION_NUM_LOCK_KEY_ID) && event->pressed) {
		custom_action_layer_set_num_lock(!custom_action_num_lock_active);
		return false;
	}

	if (event->key_id == CUSTOM_ACTION_NUM_LOCK_KEY_ID) {
		return false;
	}

	if (slot == 0U) {
		return false;
	}

	if ((custom_action_consumed_mask & BIT(event->key_id)) != 0U) {
		if (!event->pressed) {
			custom_action_consumed_mask &= ~BIT(event->key_id);
		}
		return true;
	}

	if (!event->pressed || !custom_action_num_lock_active || !custom_action_link_ready()) {
		return false;
	}

	if (custom_action_send_slot(slot) != 0) {
		return false;
	}

	custom_action_consumed_mask |= BIT(event->key_id);
	printk("custom action trigger: slot=%u key_id=%u mode=%d\n",
	       slot, event->key_id, transport_manager_get_active_mode());
	return true;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, button_event);
APP_EVENT_SUBSCRIBE(MODULE, mode_changed_event);
