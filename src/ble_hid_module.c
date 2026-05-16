#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <app_event_manager.h>
#include <caf/events/button_event.h>

#define MODULE ble_hid_module
#include <caf/events/module_state_event.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <bluetooth/adv_prov.h>
#include <bluetooth/adv_prov/swift_pair.h>
#include <bluetooth/services/hids.h>

#include "ble_hid_module.h"
#include "keymap.h"

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION 0x0101

#define BLE_HID_KEYBOARD_REPORT_ID 1
#define BLE_HID_CONSUMER_REPORT_ID 2

#define BLE_HID_BOOT_KEYBOARD_REPORT_SIZE 8
#define BLE_HID_CONSUMER_REPORT_SIZE 2

#define BLE_HID_INPUT_REP_IDX_KEYBOARD 0
#define BLE_HID_INPUT_REP_IDX_CONSUMER 1

#define BLE_ADV_FAST_INT_MIN BT_GAP_ADV_FAST_INT_MIN_2
#define BLE_ADV_FAST_INT_MAX BT_GAP_ADV_FAST_INT_MAX_2

enum ble_keyboard_report_index {
	BLE_KBD_MODIFIER_IDX = 0,
	BLE_KBD_RESERVED_IDX,
	BLE_KBD_KEY0_IDX,
	BLE_KBD_KEY1_IDX,
	BLE_KBD_KEY2_IDX,
	BLE_KBD_KEY3_IDX,
	BLE_KBD_KEY4_IDX,
	BLE_KBD_KEY5_IDX,
	BLE_KBD_REPORT_SIZE,
};

BT_HIDS_DEF(hids_obj,
	    1,
	    BLE_HID_BOOT_KEYBOARD_REPORT_SIZE,
	    BLE_HID_CONSUMER_REPORT_SIZE);

static const uint8_t hid_report_desc[] = {
	0x05, 0x01,
	0x09, 0x06,
	0xA1, 0x01,
	0x85, BLE_HID_KEYBOARD_REPORT_ID,
	0x05, 0x07,
	0x19, 0xE0,
	0x29, 0xE7,
	0x15, 0x00,
	0x25, 0x01,
	0x75, 0x01,
	0x95, 0x08,
	0x81, 0x02,
	0x95, 0x01,
	0x75, 0x08,
	0x81, 0x01,
	0x95, 0x05,
	0x75, 0x01,
	0x05, 0x08,
	0x19, 0x01,
	0x29, 0x05,
	0x91, 0x02,
	0x95, 0x01,
	0x75, 0x03,
	0x91, 0x01,
	0x95, 0x06,
	0x75, 0x08,
	0x15, 0x00,
	0x25, 0x65,
	0x05, 0x07,
	0x19, 0x00,
	0x29, 0x65,
	0x81, 0x00,
	0xC0,
	0x05, 0x0C,
	0x09, 0x01,
	0xA1, 0x01,
	0x85, BLE_HID_CONSUMER_REPORT_ID,
	0x15, 0x00,
	0x26, 0xFF, 0x03,
	0x19, 0x00,
	0x2A, 0xFF, 0x03,
	0x75, 0x10,
	0x95, 0x01,
	0x81, 0x00,
	0xC0,
};

static const struct bt_data adv_flags[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xFF,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xFF),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static struct bt_conn *active_conn;
static bool active_conn_boot_mode;
static bool active_conn_peer_bonded;
static uint8_t keyboard_report[BLE_KBD_REPORT_SIZE];
static bool ble_ready;
static bool ble_mode_enabled;
static bool advertising;
static bool settings_loaded;
static bool bt_ready_initialized;

static void advertising_update(void);

static void disconnect_active_conn(void)
{
	if (active_conn == NULL) {
		return;
	}

	int err = bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if ((err != 0) && (err != -ENOTCONN)) {
		printk("ble hid disconnect failed: %d\n", err);
	}
}

static void bond_count_cb(const struct bt_bond_info *info, void *user_data)
{
	size_t *count = user_data;

	ARG_UNUSED(info);

	(*count)++;
}

static void accept_list_add(const struct bt_bond_info *info, void *user_data)
{
	size_t *count = user_data;
	int err;

	err = bt_le_filter_accept_list_add(&info->addr);
	if ((err != 0) && (err != -EALREADY)) {
		char addr[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&info->addr, addr, sizeof(addr));
		printk("ble accept list add failed: %s err=%d\n", addr, err);
		return;
	}

	(*count)++;
}

static size_t bonded_peer_count(void)
{
	size_t count = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_count_cb, &count);
	return count;
}

struct bond_match_ctx {
	const bt_addr_le_t *addr;
	bool found;
};

static void bond_match_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bond_match_ctx *ctx = user_data;

	if (!ctx->found && (bt_addr_le_cmp(&info->addr, ctx->addr) == 0)) {
		ctx->found = true;
	}
}

static bool peer_is_bonded(const bt_addr_le_t *addr)
{
	struct bond_match_ctx ctx = {
		.addr = addr,
		.found = false,
	};

	bt_foreach_bond(BT_ID_DEFAULT, bond_match_cb, &ctx);
	return ctx.found;
}

static int build_adv_payload(struct bt_data *ad, size_t *ad_len,
			     struct bt_data *sd, size_t *sd_len,
			     bool pairing, bool in_grace_period)
{
	size_t local_ad_len = 0;
	size_t local_sd_len = 0;
	struct bt_le_adv_prov_adv_state state = {
		.pairing_mode = pairing,
		.in_grace_period = in_grace_period,
		.rpa_rotated = false,
		.new_adv_session = true,
		.adv_handle = 0,
	};
	struct bt_le_adv_prov_feedback feedback = { 0 };
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(adv_flags); i++) {
		ad[local_ad_len++] = adv_flags[i];
	}

	if (IS_ENABLED(CONFIG_BT_ADV_PROV)) {
		size_t prov_len = bt_le_adv_prov_get_ad_prov_cnt();
		size_t sd_prov_len = bt_le_adv_prov_get_sd_prov_cnt();

		if (prov_len > 0) {
			err = bt_le_adv_prov_get_ad(&ad[local_ad_len], &prov_len, &state,
						      &feedback);
			if (err != 0) {
				return err;
			}

			local_ad_len += prov_len;
		}

		if (sd_prov_len > 0) {
			err = bt_le_adv_prov_get_sd(sd, &sd_prov_len, &state, &feedback);
			if (err != 0) {
				return err;
			}

			local_sd_len += sd_prov_len;
		}
	}

	sd[local_sd_len++] =
		(struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME,
					DEVICE_NAME_LEN);

	*ad_len = local_ad_len;
	*sd_len = local_sd_len;

	return 0;
}

static void advertising_stop(void)
{
	int err;

	if (!advertising) {
		return;
	}

	err = bt_le_adv_stop();
	if ((err != 0) && (err != -EALREADY)) {
		printk("ble adv stop failed: %d\n", err);
		return;
	}

	advertising = false;
}

static void advertising_start(bool pairing)
{
	struct bt_data ad[8];
	struct bt_data sd[8];
	size_t ad_len = 0;
	size_t sd_len = 0;
	struct bt_le_adv_param adv_param = {
		.id = BT_ID_DEFAULT,
		.sid = 0,
		.secondary_max_skip = 0,
		.options = BT_LE_ADV_OPT_CONN,
		.interval_min = BLE_ADV_FAST_INT_MIN,
		.interval_max = BLE_ADV_FAST_INT_MAX,
		.peer = NULL,
	};
	size_t bond_count;
	int err;

	if (!ble_mode_enabled || !ble_ready || (active_conn != NULL)) {
		return;
	}

	advertising_stop();

	if (IS_ENABLED(CONFIG_BT_ADV_PROV_SWIFT_PAIR)) {
		bt_le_adv_prov_swift_pair_enable(pairing);
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		err = bt_le_filter_accept_list_clear();
		if ((err != 0) && (err != -EALREADY)) {
			printk("ble accept list clear failed: %d\n", err);
		}
	}

	bond_count = 0;
	if (!pairing) {
		if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
			err = bt_le_filter_accept_list_clear();
			if ((err != 0) && (err != -EALREADY)) {
				printk("ble accept list clear failed: %d\n", err);
			}

			bt_foreach_bond(BT_ID_DEFAULT, accept_list_add, &bond_count);
		} else {
			bond_count = bonded_peer_count();
		}

		if (bond_count > 0U) {
			adv_param.options |=
				BT_LE_ADV_OPT_FILTER_CONN | BT_LE_ADV_OPT_FILTER_SCAN_REQ;
		}
	}

	err = build_adv_payload(ad, &ad_len, sd, &sd_len, pairing, false);
	if (err != 0) {
		printk("ble adv payload build failed: %d\n", err);
		return;
	}

	err = bt_le_adv_start(&adv_param, ad, ad_len, sd, sd_len);
	if (err != 0) {
		printk("ble adv start failed: %d\n", err);
		return;
	}

	advertising = true;
	printk("ble advertising: %s bond_count=%u\n",
	       pairing ? "pairing" : "reconnect", (unsigned int)bond_count);
}

static void advertising_update(void)
{
	bool have_bond = false;

	if (!ble_mode_enabled || !ble_ready) {
		advertising_stop();
		return;
	}

	if (active_conn != NULL) {
		advertising_stop();
		return;
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		have_bond = (bonded_peer_count() > 0U);
	}

	advertising_start(!have_bond);
}

static void keyboard_report_clear_usage(uint8_t usage)
{
	for (size_t i = BLE_KBD_KEY0_IDX; i < BLE_KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == usage) {
			keyboard_report[i] = 0;
		}
	}
}

static int keyboard_report_set_usage(uint8_t usage)
{
	for (size_t i = BLE_KBD_KEY0_IDX; i < BLE_KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == usage) {
			return 0;
		}
	}

	for (size_t i = BLE_KBD_KEY0_IDX; i < BLE_KBD_REPORT_SIZE; i++) {
		if (keyboard_report[i] == 0) {
			keyboard_report[i] = usage;
			return 0;
		}
	}

	return -ENOBUFS;
}

static int send_keyboard_report(void)
{
	if ((active_conn == NULL) || !ble_mode_enabled) {
		return -EAGAIN;
	}

	if (active_conn_boot_mode) {
		return bt_hids_boot_kb_inp_rep_send(&hids_obj, active_conn,
						    keyboard_report,
						    sizeof(keyboard_report), NULL);
	}

	return bt_hids_inp_rep_send(&hids_obj, active_conn,
				    BLE_HID_INPUT_REP_IDX_KEYBOARD,
				    keyboard_report, sizeof(keyboard_report), NULL);
}

static int send_consumer_report(uint16_t usage)
{
	uint8_t report[BLE_HID_CONSUMER_REPORT_SIZE];

	if ((active_conn == NULL) || !ble_mode_enabled) {
		return -EAGAIN;
	}

	sys_put_le16(usage, report);
	return bt_hids_inp_rep_send(&hids_obj, active_conn,
				    BLE_HID_INPUT_REP_IDX_CONSUMER,
				    report, sizeof(report), NULL);
}

static int send_consumer_usage(uint16_t usage)
{
	int err;

	err = send_consumer_report(usage);
	if (err != 0) {
		return err;
	}

	return send_consumer_report(0);
}

static void handle_button_event(const struct button_event *event)
{
	const struct keymap_hid_entry *map = keymap_hid_get(event->key_id);
	int err;

	if (!ble_mode_enabled || (active_conn == NULL) || (map == NULL)) {
		return;
	}

	if (map->type == KEYMAP_HID_TYPE_CONSUMER) {
		if (event->pressed) {
			err = send_consumer_usage(map->usage);
			if (err != 0) {
				printk("ble consumer send failed: %d\n", err);
			}
		}
		return;
	}

	if (map->type != KEYMAP_HID_TYPE_KEYBOARD) {
		return;
	}

	if (event->pressed) {
		err = keyboard_report_set_usage((uint8_t)map->usage);
		if (err != 0) {
			printk("ble keyboard full: key_id=%u\n", event->key_id);
			return;
		}
	} else {
		keyboard_report_clear_usage((uint8_t)map->usage);
	}

	err = send_keyboard_report();
	if (err != 0) {
		printk("ble keyboard send failed: %d\n", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int hids_err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		printk("ble connected failed: %s err=0x%02x %s\n",
		       addr, err, bt_hci_err_to_str(err));
		advertising_update();
		return;
	}

	if (active_conn != NULL) {
		printk("ble extra connection refused: %s\n", addr);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	active_conn = bt_conn_ref(conn);
	active_conn_boot_mode = false;
	active_conn_peer_bonded = peer_is_bonded(bt_conn_get_dst(conn));
	advertising = false;

	hids_err = bt_hids_connected(&hids_obj, conn);
	if (hids_err != 0) {
		printk("ble hids connected notify failed: %d\n", hids_err);
	}

	/*
	 * For a brand new peer, let the host initiate pairing when it touches
	 * encrypted HIDS attributes. This avoids immediately taking the
	 * security-request path that commonly fails with PIN_OR_KEY_MISSING
	 * when the host still has a stale bond from an older firmware session.
	 */
	if (active_conn_peer_bonded) {
		hids_err = bt_conn_set_security(conn, BT_SECURITY_L2);
		if ((hids_err != 0) && (hids_err != -EALREADY)) {
			printk("ble security request failed: %d\n", hids_err);
		}
	}

	printk("ble connected: %s%s\n", addr,
	       active_conn_peer_bonded ? "" : " (unbonded)");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int hids_err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	hids_err = bt_hids_disconnected(&hids_obj, conn);
	if (hids_err != 0) {
		printk("ble hids disconnected notify failed: %d\n", hids_err);
	}

	if (active_conn == conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
		active_conn_boot_mode = false;
		active_conn_peer_bonded = false;
	}

	memset(keyboard_report, 0, sizeof(keyboard_report));
	printk("ble disconnected: %s reason=0x%02x %s\n", addr, reason,
	       bt_hci_err_to_str(reason));

	advertising_update();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err == 0) {
		if (conn == active_conn) {
			active_conn_peer_bonded = true;
		}
		printk("ble security: %s level=%u\n", addr, level);
	} else {
		printk("ble security failed: %s level=%u err=%d %s\n", addr, level,
		       err, bt_security_err_to_str(err));

		if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			bool peer_bonded = peer_is_bonded(bt_conn_get_dst(conn));

			if (peer_bonded) {
				int unpair_err = bt_unpair(BT_ID_DEFAULT,
						       bt_conn_get_dst(conn));

				if (unpair_err == 0) {
					if (conn == active_conn) {
						active_conn_peer_bonded = false;
					}
					printk("ble cleared stale local bond: %s\n", addr);
				} else {
					printk("ble clear local bond failed: %s err=%d\n",
					       addr, unpair_err);
				}
			} else {
				printk("ble pairing hint: host may still have an old bond, "
				       "delete this device on the host and pair again\n");
			}
		}
	}
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("ble pairing cancelled: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (bonded && (conn == active_conn)) {
		active_conn_peer_bonded = true;
	}
	printk("ble pairing complete: %s bonded=%d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("ble pairing failed: %s reason=%d %s\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static const struct bt_conn_auth_cb ble_auth_callbacks = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb ble_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

BT_CONN_CB_DEFINE(ble_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void keyboard_leds_output_report_handler(struct bt_hids_rep *rep,
						struct bt_conn *conn,
						bool write)
{
	ARG_UNUSED(rep);
	ARG_UNUSED(conn);
	ARG_UNUSED(write);
}

static void boot_keyboard_output_report_handler(struct bt_hids_rep *rep,
						struct bt_conn *conn,
						bool write)
{
	ARG_UNUSED(rep);
	ARG_UNUSED(conn);
	ARG_UNUSED(write);
}

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	if (conn != active_conn) {
		return;
	}

	active_conn_boot_mode = (evt == BT_HIDS_PM_EVT_BOOT_MODE_ENTERED);
}

static int hids_init(void)
{
	struct bt_hids_init_param init = { 0 };
	struct bt_hids_inp_rep *input_report;
	struct bt_hids_outp_feat_rep *output_report;

	init.rep_map.data = hid_report_desc;
	init.rep_map.size = sizeof(hid_report_desc);
	init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	init.info.b_country_code = 0x00;
	init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

	input_report = &init.inp_rep_group_init.reports[0];
	input_report[BLE_HID_INPUT_REP_IDX_KEYBOARD].id = BLE_HID_KEYBOARD_REPORT_ID;
	input_report[BLE_HID_INPUT_REP_IDX_KEYBOARD].size =
		BLE_HID_BOOT_KEYBOARD_REPORT_SIZE;
	input_report[BLE_HID_INPUT_REP_IDX_CONSUMER].id = BLE_HID_CONSUMER_REPORT_ID;
	input_report[BLE_HID_INPUT_REP_IDX_CONSUMER].size =
		BLE_HID_CONSUMER_REPORT_SIZE;
	init.inp_rep_group_init.cnt = 2;

	output_report = &init.outp_rep_group_init.reports[0];
	output_report->id = BLE_HID_KEYBOARD_REPORT_ID;
	output_report->size = 1;
	output_report->handler = keyboard_leds_output_report_handler;
	init.outp_rep_group_init.cnt = 1;

	init.is_kb = true;
	init.boot_kb_outp_rep_handler = boot_keyboard_output_report_handler;
	init.pm_evt_handler = hids_pm_evt_handler;

	return bt_hids_init(&hids_obj, &init);
}

static int bt_stack_init(void)
{
	int err;

	if (bt_ready_initialized) {
		return 0;
	}

	err = bt_enable(NULL);
	if (err != 0) {
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS) && !settings_loaded) {
		err = settings_load();
		if (err != 0) {
			return err;
		}
		settings_loaded = true;
	}

	err = bt_conn_auth_cb_register(&ble_auth_callbacks);
	if (err != 0) {
		return err;
	}

	err = bt_conn_auth_info_cb_register(&ble_auth_info_callbacks);
	if (err != 0) {
		return err;
	}

	if (IS_ENABLED(CONFIG_BT_BAS)) {
		(void)bt_bas_set_battery_level(100);
	}

	err = hids_init();
	if (err != 0) {
		return err;
	}

	bt_ready_initialized = true;
	ble_ready = true;
	printk("ble hid ready\n");
	return 0;
}

int ble_hid_module_init(void)
{
	int err = bt_stack_init();

	if (err != 0) {
		return err;
	}

	module_set_state(MODULE_STATE_READY);
	return 0;
}

int ble_hid_module_set_mode(bool enabled)
{
	int err;

	err = bt_stack_init();
	if (err != 0) {
		return err;
	}

	ble_mode_enabled = enabled;

	if (!enabled) {
		advertising_stop();
		disconnect_active_conn();
		memset(keyboard_report, 0, sizeof(keyboard_report));
		active_conn_boot_mode = false;
		return 0;
	}

	advertising_update();
	return 0;
}

int ble_hid_module_release_all(void)
{
	int err = 0;

	memset(keyboard_report, 0, sizeof(keyboard_report));

	if ((active_conn != NULL) && ble_mode_enabled) {
		err = send_keyboard_report();
		if ((err != 0) && (err != -EAGAIN)) {
			printk("ble release keyboard failed: %d\n", err);
		}
	}

	if ((active_conn != NULL) && ble_mode_enabled) {
		err = send_consumer_report(0);
		if ((err != 0) && (err != -EAGAIN)) {
			printk("ble release consumer failed: %d\n", err);
		}
	}

	return 0;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_button_event(aeh)) {
		handle_button_event(cast_button_event(aeh));
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);
