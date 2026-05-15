#include <stdint.h>

#include <zephyr/sys/util.h>

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

const char *keymap_name_get(uint16_t key_id)
{
	if (key_id >= ARRAY_SIZE(keymap)) {
		return "UNKNOWN";
	}

	return keymap[key_id];
}
