#ifndef KEYMAP_H_
#define KEYMAP_H_

#include <stdint.h>

enum keymap_hid_type {
	KEYMAP_HID_TYPE_NONE = 0,
	KEYMAP_HID_TYPE_KEYBOARD,
	KEYMAP_HID_TYPE_CONSUMER,
};

struct keymap_hid_entry {
	enum keymap_hid_type type;
	uint16_t usage;
};

const char *keymap_name_get(uint16_t key_id);
const struct keymap_hid_entry *keymap_hid_get(uint16_t key_id);

#endif /* KEYMAP_H_ */
