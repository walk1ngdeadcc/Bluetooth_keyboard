#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "rgb_led.h"

#if !DT_HAS_CHOSEN(zephyr_led_strip)
#error "zephyr,led-strip chosen node must be defined"
#endif

#define RGB_STRIP_NODE DT_CHOSEN(zephyr_led_strip)
#define RGB_USER_NODE DT_PATH(zephyr_user)
#define RGB_LED_COUNT DT_PROP(RGB_STRIP_NODE, chain_length)
#define RGB_DATA_PIN 20U
#define RGB_DEFAULT_RED_LEVEL 0x20U
#define RGB_POWER_ON_DELAY_MS 10
#define RGB_POWER_OFF_DELAY_US 300
#define RGB_FRAME_RETRY_COUNT 3
#define RGB_FRAME_RETRY_DELAY_MS 2
#define RGB_KEY_COUNT 17U
#define RGB_SELF_TEST_LEVEL 0x20U
#define RGB_SELF_TEST_HOLD_MS 120

#if !DT_NODE_HAS_PROP(RGB_USER_NODE, rgb_pwr_gpios)
#error "zephyr,user must define rgb-pwr-gpios"
#endif

static const struct device *const rgb_strip = DEVICE_DT_GET(RGB_STRIP_NODE);
static const struct gpio_dt_spec rgb_power = GPIO_DT_SPEC_GET(RGB_USER_NODE, rgb_pwr_gpios);

static struct led_rgb rgb_pixels[RGB_LED_COUNT];
static bool rgb_initialized;
static bool rgb_power_enabled;

/*
 * Baseline mapping:
 * key1..key17 -> led[0]..led[16]
 * This is kept local to the RGB module so later single-key lighting
 * logic can reuse the same mapping without touching other modules.
 */
static const uint8_t rgb_led_index_by_key_id[RGB_KEY_COUNT + 1] = {
	[1] = 0,
	[2] = 1,
	[3] = 2,
	[4] = 3,
	[5] = 4,
	[6] = 5,
	[7] = 6,
	[8] = 7,
	[9] = 8,
	[10] = 9,
	[11] = 10,
	[12] = 11,
	[13] = 12,
	[14] = 13,
	[15] = 14,
	[16] = 15,
	[17] = 16,
};

static void rgb_fill_all(uint8_t red, uint8_t green, uint8_t blue)
{
	for (size_t i = 0; i < ARRAY_SIZE(rgb_pixels); i++) {
		rgb_pixels[i].r = red;
		rgb_pixels[i].g = green;
		rgb_pixels[i].b = blue;
	}
}

static int rgb_power_set(bool enabled)
{
	int ret;

	if (rgb_power_enabled == enabled) {
		return 0;
	}

	ret = gpio_pin_set_dt(&rgb_power, enabled ? 1 : 0);
	if (ret != 0) {
		return ret;
	}

	rgb_power_enabled = enabled;

	if (enabled) {
		printk("rgb power on\n");
		k_msleep(RGB_POWER_ON_DELAY_MS);
	} else {
		printk("rgb power off\n");
	}

	return 0;
}

static int rgb_push(void)
{
	int ret = 0;

	for (int i = 0; i < RGB_FRAME_RETRY_COUNT; i++) {
		ret = led_strip_update_rgb(rgb_strip, rgb_pixels, ARRAY_SIZE(rgb_pixels));
		if (ret != 0) {
			return ret;
		}

		if (i + 1 < RGB_FRAME_RETRY_COUNT) {
			k_msleep(RGB_FRAME_RETRY_DELAY_MS);
		}
	}

	return 0;
}

static int rgb_key_id_to_led_index(uint8_t key_id, uint8_t *led_index)
{
	if ((key_id == 0U) || (key_id >= ARRAY_SIZE(rgb_led_index_by_key_id))) {
		return -EINVAL;
	}

	*led_index = rgb_led_index_by_key_id[key_id];

	if (*led_index >= ARRAY_SIZE(rgb_pixels)) {
		return -ERANGE;
	}

	return 0;
}

static int rgb_set_only_led_red(uint8_t led_index, uint8_t level)
{
	int ret;

	if (led_index >= ARRAY_SIZE(rgb_pixels)) {
		return -ERANGE;
	}

	rgb_fill_all(0U, 0U, 0U);
	rgb_pixels[led_index].r = level;
	ret = rgb_push();
	if (ret != 0) {
		printk("rgb single led update failed: led=%u err=%d\n", led_index, ret);
		return ret;
	}

	return 0;
}

static int rgb_run_key_self_test(void)
{
	int ret;

	printk("rgb key self-test start: key1->led0 ... key17->led16\n");

	for (uint8_t key_id = 1; key_id <= RGB_KEY_COUNT; key_id++) {
		uint8_t led_index;

		ret = rgb_key_id_to_led_index(key_id, &led_index);
		if (ret != 0) {
			printk("rgb key map invalid: key=%u err=%d\n", key_id, ret);
			return ret;
		}

		ret = rgb_set_only_led_red(led_index, RGB_SELF_TEST_LEVEL);
		if (ret != 0) {
			return ret;
		}

		printk("rgb key self-test: key=%u led=%u\n", key_id, led_index);
		k_msleep(RGB_SELF_TEST_HOLD_MS);
	}

	printk("rgb key self-test done\n");
	return 0;
}

int rgb_led_set_all_red(uint8_t level)
{
	int ret;

	if (!rgb_initialized) {
		return -EACCES;
	}

	ret = rgb_power_set(true);
	if (ret != 0) {
		return ret;
	}

	rgb_fill_all(level, 0U, 0U);

	ret = rgb_push();
	if (ret != 0) {
		printk("rgb update failed: %d\n", ret);
		return ret;
	}

	printk("rgb set all red: level=0x%02x\n", level);
	return 0;
}

int rgb_led_set_all(uint8_t red, uint8_t green, uint8_t blue)
{
	int ret;

	if (!rgb_initialized) {
		return -EACCES;
	}

	ret = rgb_power_set(true);
	if (ret != 0) {
		return ret;
	}

	rgb_fill_all(red, green, blue);

	ret = rgb_push();
	if (ret != 0) {
		printk("rgb update failed: %d\n", ret);
		return ret;
	}

	printk("rgb set all: r=0x%02x g=0x%02x b=0x%02x\n", red, green, blue);
	return 0;
}

int rgb_led_off(void)
{
	int ret;

	if (!rgb_initialized) {
		return -EACCES;
	}

	if (!rgb_power_enabled) {
		return 0;
	}

	rgb_fill_all(0U, 0U, 0U);
	ret = rgb_push();
	if (ret != 0) {
		printk("rgb off update failed: %d\n", ret);
		return ret;
	}

	k_usleep(RGB_POWER_OFF_DELAY_US);
	return rgb_power_set(false);
}

int rgb_led_init(void)
{
	int ret;

	if (rgb_initialized) {
		return rgb_led_set_all_red(RGB_DEFAULT_RED_LEVEL);
	}

	if (!gpio_is_ready_dt(&rgb_power)) {
		return -ENODEV;
	}

	if (!device_is_ready(rgb_strip)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&rgb_power, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}

	memset(rgb_pixels, 0, sizeof(rgb_pixels));
	rgb_initialized = true;

	printk("rgb led strip ready: count=%u data=P0.%02u pwr=P0.%02u power_delay=%ums retries=%d\n",
	       (unsigned int)ARRAY_SIZE(rgb_pixels), RGB_DATA_PIN, rgb_power.pin,
	       RGB_POWER_ON_DELAY_MS, RGB_FRAME_RETRY_COUNT);

	ret = rgb_power_set(true);
	if (ret != 0) {
		return ret;
	}

	ret = rgb_run_key_self_test();
	if (ret != 0) {
		return ret;
	}

	return rgb_led_set_all_red(RGB_DEFAULT_RED_LEVEL);
}
