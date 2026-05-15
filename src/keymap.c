#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/usb/class/hid.h>

#include "keymap.h"

static const char *const keymap[] = {
	"VOLUME_MUTE",
	"NUM_LOCK",
	"KP_SLASH",
	"KP_ASTERISK",
	"KP_MINUS",
	"KP_7",
	"KP_8",
	"KP_9",
	"KP_4",
	"KP_5",
	"KP_6",
	"KP_PLUS",
	"KP_1",
	"KP_2",
	"KP_3",
	"KP_0",
	"KP_DOT",
	"KP_ENTER",
};

static const struct keymap_hid_entry hid_keymap[] = {
	[0] = { KEYMAP_HID_TYPE_CONSUMER, 0x00E2 },
	[1] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_NUMLOCK },
	[2] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KPSLASH },
	[3] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KPASTERISK },
	[4] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KPMINUS },
	[5] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_7 },
	[6] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_8 },
	[7] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_9 },
	[8] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_4 },
	[9] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_5 },
	[10] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_6 },
	[11] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KPPLUS },
	[12] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_1 },
	[13] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_2 },
	[14] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_3 },
	[15] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KP_0 },
	[16] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_DOT },
	[17] = { KEYMAP_HID_TYPE_KEYBOARD, HID_KEY_KPENTER },
	[100] = { KEYMAP_HID_TYPE_CONSUMER, 0x00E9 },
	[101] = { KEYMAP_HID_TYPE_CONSUMER, 0x00EA },
};

const char *keymap_name_get(uint16_t key_id)
{
	if (key_id >= ARRAY_SIZE(keymap)) {
		return "UNKNOWN";
	}

	return keymap[key_id];
}

const struct keymap_hid_entry *keymap_hid_get(uint16_t key_id)
{
	if (key_id >= ARRAY_SIZE(hid_keymap)) {
		return NULL;
	}

	if (hid_keymap[key_id].type == KEYMAP_HID_TYPE_NONE) {
		return NULL;
	}

	return &hid_keymap[key_id];
}
