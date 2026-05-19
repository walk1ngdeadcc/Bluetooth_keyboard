#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/printk.h>

#include "ble_host_action_service.h"

#define BLE_HOST_ACTION_SERVICE_UUID_VAL \
	BT_UUID_128_ENCODE(0x8b1f7d00, 0xc66d, 0x4f18, 0xb7d5, 0x4a8e2c1d9001)
#define BLE_HOST_ACTION_TRIGGER_UUID_VAL \
	BT_UUID_128_ENCODE(0x8b1f7d01, 0xc66d, 0x4f18, 0xb7d5, 0x4a8e2c1d9001)

#define BLE_HOST_ACTION_SERVICE_UUID BT_UUID_DECLARE_128(BLE_HOST_ACTION_SERVICE_UUID_VAL)
#define BLE_HOST_ACTION_TRIGGER_UUID BT_UUID_DECLARE_128(BLE_HOST_ACTION_TRIGGER_UUID_VAL)

#define BLE_HOST_ACTION_TRIGGER_ATTR_IDX 2

static bool ble_host_action_notify_enabled;
static uint8_t ble_host_action_last_trigger;

static ssize_t ble_host_action_read(struct bt_conn *conn,
				     const struct bt_gatt_attr *attr,
				     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &ble_host_action_last_trigger,
				 sizeof(ble_host_action_last_trigger));
}

static void ble_host_action_cccd_changed(const struct bt_gatt_attr *attr,
					    uint16_t value)
{
	(void)attr;

	ble_host_action_notify_enabled = ((value & BT_GATT_CCC_NOTIFY) != 0U);
	printk("ble host action notify=%d\n", ble_host_action_notify_enabled);
}

BT_GATT_SERVICE_DEFINE(ble_host_action_svc,
	BT_GATT_PRIMARY_SERVICE(BLE_HOST_ACTION_SERVICE_UUID),
	BT_GATT_CHARACTERISTIC(BLE_HOST_ACTION_TRIGGER_UUID,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       ble_host_action_read, NULL, NULL),
	BT_GATT_CCC(ble_host_action_cccd_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

int ble_host_action_service_init(void)
{
	ble_host_action_notify_enabled = false;
	ble_host_action_last_trigger = 0U;
	printk("ble host action service ready\n");
	return 0;
}

bool ble_host_action_service_is_ready(void)
{
	return ble_host_action_notify_enabled;
}

int ble_host_action_service_send_trigger(uint8_t slot)
{
	if ((slot == 0U) || (slot > 9U)) {
		return -EINVAL;
	}

	if (!ble_host_action_notify_enabled) {
		return -EACCES;
	}

	ble_host_action_last_trigger = slot;
	return bt_gatt_notify(NULL,
			      &ble_host_action_svc.attrs[BLE_HOST_ACTION_TRIGGER_ATTR_IDX],
			      &ble_host_action_last_trigger,
			      sizeof(ble_host_action_last_trigger));
}
