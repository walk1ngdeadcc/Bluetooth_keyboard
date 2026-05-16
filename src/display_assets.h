#ifndef DISPLAY_ASSETS_H_
#define DISPLAY_ASSETS_H_

#include <stdint.h>

struct display_rgb332_bitmap {
	uint16_t width;
	uint16_t height;
	const uint8_t *data;
};

extern const struct display_rgb332_bitmap display_idle_bitmap;

#endif /* DISPLAY_ASSETS_H_ */
