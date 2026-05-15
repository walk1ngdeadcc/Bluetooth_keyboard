#include <errno.h>
#include <stdbool.h>

#include <app_event_manager.h>
#include <caf/events/button_event.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "keymap.h"
#include "key_matrix.h"

#define KEY_MATRIX_NODE DT_NODELABEL(kbd_matrix)
#define KEY_MATRIX_ROWS DT_PROP_LEN(KEY_MATRIX_NODE, row_gpios)
#define KEY_MATRIX_COLS DT_PROP_LEN(KEY_MATRIX_NODE, col_gpios)

BUILD_ASSERT(DT_NODE_EXISTS(KEY_MATRIX_NODE), "kbd_matrix node must exist");

static const struct gpio_dt_spec row_gpios[KEY_MATRIX_ROWS] = {
	DT_FOREACH_PROP_ELEM_SEP(KEY_MATRIX_NODE, row_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static const struct gpio_dt_spec col_gpios[KEY_MATRIX_COLS] = {
	DT_FOREACH_PROP_ELEM_SEP(KEY_MATRIX_NODE, col_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static const uint16_t key_id_map[KEY_MATRIX_ROWS][KEY_MATRIX_COLS] = {
	{ UINT16_MAX, UINT16_MAX, UINT16_MAX, 0 },
	{ 1, 2, 3, 4 },
	{ 5, 6, 7, UINT16_MAX },
	{ 8, 9, 10, 11 },
	{ 12, 13, 14, UINT16_MAX },
	{ 15, 16, UINT16_MAX, 17 },
};

static uint32_t last_state[KEY_MATRIX_COLS];
static bool scanner_started;

K_THREAD_STACK_DEFINE(key_matrix_stack, 2048);
static struct k_thread key_matrix_thread_data;

static int prepare_rows(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(row_gpios); i++) {
		int ret = gpio_pin_configure(row_gpios[i].port,
					     row_gpios[i].pin,
					     GPIO_INPUT | GPIO_PULL_DOWN);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int set_all_cols_input(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(col_gpios); i++) {
		int ret = gpio_pin_configure(col_gpios[i].port, col_gpios[i].pin, GPIO_INPUT);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int drive_col_active(size_t col)
{
	int ret = gpio_pin_configure(col_gpios[col].port, col_gpios[col].pin, GPIO_OUTPUT);

	if (ret != 0) {
		return ret;
	}

	return gpio_pin_set_raw(col_gpios[col].port, col_gpios[col].pin, 1);
}

static int scan_once(uint32_t *state)
{
	int ret = prepare_rows();

	if (ret != 0) {
		return ret;
	}

	ret = set_all_cols_input();
	if (ret != 0) {
		return ret;
	}

	for (size_t col = 0; col < ARRAY_SIZE(col_gpios); col++) {
		uint32_t row_bits = 0U;

		ret = drive_col_active(col);
		if (ret != 0) {
			return ret;
		}

		k_busy_wait(50);

		for (size_t row = 0; row < ARRAY_SIZE(row_gpios); row++) {
			int raw = gpio_pin_get_raw(row_gpios[row].port, row_gpios[row].pin);

			if (raw < 0) {
				return raw;
			}

			if (raw != 0) {
				row_bits |= BIT(row);
			}
		}

		state[col] = row_bits;

		ret = gpio_pin_configure(col_gpios[col].port, col_gpios[col].pin, GPIO_INPUT);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static void report_changes(const uint32_t *state)
{
	for (size_t col = 0; col < ARRAY_SIZE(col_gpios); col++) {
		uint32_t changed = last_state[col] ^ state[col];

		if (changed == 0U) {
			last_state[col] = state[col];
			continue;
		}

		for (size_t row = 0; row < ARRAY_SIZE(row_gpios); row++) {
			if ((changed & BIT(row)) == 0U) {
				continue;
			}

			bool pressed = (state[col] & BIT(row)) != 0U;
			uint16_t key_id = key_id_map[row][col];

			if (key_id == UINT16_MAX) {
				continue;
			}

			struct button_event *event = new_button_event();

			event->key_id = key_id;
			event->pressed = pressed;
			APP_EVENT_SUBMIT(event);

			printk("matrix key %s: id=%u name=%s row=%d col=%d\n",
			       pressed ? "down" : "up", key_id,
			       keymap_name_get(key_id), (int)row, (int)col);
		}

		last_state[col] = state[col];
	}
}

static void key_matrix_thread(void *arg1, void *arg2, void *arg3)
{
	uint32_t state[KEY_MATRIX_COLS];

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	printk("matrix scanner started: cols active high, rows pull-down\n");

	while (true) {
		int ret = scan_once(state);

		if (ret != 0) {
			printk("matrix scan error: %d\n", ret);
			k_sleep(K_MSEC(200));
			continue;
		}

		report_changes(state);
		k_sleep(K_MSEC(15));
	}
}

int key_matrix_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(row_gpios); i++) {
		if (!gpio_is_ready_dt(&row_gpios[i])) {
			printk("row gpio %d is not ready\n", (int)i);
			return -ENODEV;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(col_gpios); i++) {
		if (!gpio_is_ready_dt(&col_gpios[i])) {
			printk("col gpio %d is not ready\n", (int)i);
			return -ENODEV;
		}
	}

	if (!scanner_started) {
		scanner_started = true;
		k_thread_create(&key_matrix_thread_data,
				key_matrix_stack,
				K_THREAD_STACK_SIZEOF(key_matrix_stack),
				key_matrix_thread,
				NULL, NULL, NULL,
				5, 0, K_NO_WAIT);
	}

	printk("key matrix ready: rows=%d cols=%d\n", KEY_MATRIX_ROWS, KEY_MATRIX_COLS);
	printk("scan mode: cols active high, rows pull-down\n");
	printk("RTT test: press any key to print row/col\n");

	return 0;
}
