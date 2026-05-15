#include <caf/gpio_pins.h>

/*
 * Included only by CAF buttons module.
 * This object makes multiple inclusion fail at link time.
 */
const struct {} buttons_def_include_once;

static const struct gpio_pin col[] = {
	{ .port = 0, .pin = 5 },
	{ .port = 0, .pin = 6 },
	{ .port = 0, .pin = 26 },
	{ .port = 0, .pin = 30 },
};

static const struct gpio_pin row[] = {
	{ .port = 0, .pin = 15 },
	{ .port = 0, .pin = 7 },
	{ .port = 0, .pin = 12 },
	{ .port = 0, .pin = 4 },
	{ .port = 1, .pin = 9 },
	{ .port = 0, .pin = 8 },
};
