#include <app_event_manager.h>

#define MODULE rgb_led
#include <caf/events/power_event.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
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
#define RGB_RESTORE_DELAY_MS 120
#define RGB_SETTINGS_THEME_KEY "rgb/theme"

#if !DT_NODE_HAS_PROP(RGB_USER_NODE, rgb_pwr_gpios)
#error "zephyr,user must define rgb-pwr-gpios"
#endif

struct rgb_color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

static const struct device *const rgb_strip = DEVICE_DT_GET(RGB_STRIP_NODE);
static const struct gpio_dt_spec rgb_power = GPIO_DT_SPEC_GET(RGB_USER_NODE, rgb_pwr_gpios);

static struct led_rgb rgb_pixels[RGB_LED_COUNT];
static struct k_work_delayable rgb_restore_work;
static struct rgb_color rgb_current_color;
static struct rgb_color rgb_theme_color;
static bool rgb_initialized;
static bool rgb_current_valid;
static bool rgb_power_enabled;
static bool rgb_settings_ready;
static bool rgb_theme_valid;

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

static int rgb_settings_ensure_ready(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_SETTINGS) || rgb_settings_ready) {
		return 0;
	}

	ret = settings_subsys_init();
	if (ret != 0) {
		printk("rgb settings init failed: %d\n", ret);
		return ret;
	}

	rgb_settings_ready = true;
	return 0;
}

static void rgb_store_current_color(uint8_t red, uint8_t green, uint8_t blue)
{
	rgb_current_color.red = red;
	rgb_current_color.green = green;
	rgb_current_color.blue = blue;
	rgb_current_valid = true;
}

static int rgb_apply_color(uint8_t red, uint8_t green, uint8_t blue)
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

	rgb_store_current_color(red, green, blue);
	printk("rgb set all: r=0x%02x g=0x%02x b=0x%02x\n", red, green, blue);
	return 0;
}

static int rgb_theme_load(void)
{
	struct rgb_color saved_theme;
	ssize_t len;
	int ret;

	rgb_theme_valid = false;

	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		return 0;
	}

	ret = rgb_settings_ensure_ready();
	if (ret != 0) {
		return ret;
	}

	len = settings_load_one(RGB_SETTINGS_THEME_KEY, &saved_theme, sizeof(saved_theme));
	if ((len == 0) || (len == -ENOENT)) {
		return 0;
	}

	if (len < 0) {
		printk("rgb theme load failed: %d\n", (int)len);
		return (int)len;
	}

	if ((size_t)len != sizeof(saved_theme)) {
		printk("rgb theme load invalid length: %d\n", (int)len);
		return -EINVAL;
	}

	rgb_theme_color = saved_theme;
	rgb_theme_valid = true;
	printk("rgb theme loaded: r=0x%02x g=0x%02x b=0x%02x\n",
	       saved_theme.red, saved_theme.green, saved_theme.blue);
	return 0;
}

static int rgb_theme_save(void)
{
	int ret;

	if (!rgb_theme_valid || !IS_ENABLED(CONFIG_SETTINGS)) {
		return 0;
	}

	ret = rgb_settings_ensure_ready();
	if (ret != 0) {
		return ret;
	}

	ret = settings_save_one(RGB_SETTINGS_THEME_KEY,
				&rgb_theme_color,
				sizeof(rgb_theme_color));
	if (ret != 0) {
		printk("rgb theme save failed: %d\n", ret);
		return ret;
	}

	printk("rgb theme saved: r=0x%02x g=0x%02x b=0x%02x\n",
	       rgb_theme_color.red, rgb_theme_color.green, rgb_theme_color.blue);
	return 0;
}

static int rgb_force_power_recover(void)
{
	int ret;

	if (!rgb_power_enabled) {
		return rgb_power_set(true);
	}

	ret = gpio_pin_set_dt(&rgb_power, 0);
	if (ret != 0) {
		return ret;
	}

	rgb_power_enabled = false;
	k_usleep(RGB_POWER_OFF_DELAY_US);

	return rgb_power_set(true);
}

static int rgb_restore_cached_color(void)
{
	if (rgb_theme_valid) {
		return rgb_apply_color(rgb_theme_color.red,
				      rgb_theme_color.green,
				      rgb_theme_color.blue);
	}

	if (rgb_current_valid) {
		return rgb_apply_color(rgb_current_color.red,
				      rgb_current_color.green,
				      rgb_current_color.blue);
	}

	return rgb_apply_color(RGB_DEFAULT_RED_LEVEL, 0U, 0U);
}

static void rgb_restore_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	ret = rgb_force_power_recover();
	if (ret != 0) {
		printk("rgb power recover failed: %d\n", ret);
		return;
	}

	ret = rgb_restore_cached_color();
	if (ret != 0) {
		printk("rgb restore failed: %d\n", ret);
	}
}

int rgb_led_set_all_red(uint8_t level)
{
	return rgb_apply_color(level, 0U, 0U);
}

int rgb_led_set_all(uint8_t red, uint8_t green, uint8_t blue)
{
	int ret;

	ret = rgb_apply_color(red, green, blue);
	if (ret != 0) {
		return ret;
	}

	rgb_theme_color.red = red;
	rgb_theme_color.green = green;
	rgb_theme_color.blue = blue;
	rgb_theme_valid = true;

	return rgb_theme_save();
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

int rgb_led_restore(void)
{
	if (!rgb_initialized) {
		return -EACCES;
	}

	return rgb_restore_cached_color();
}

void rgb_led_request_restore(void)
{
	if (!rgb_initialized) {
		return;
	}

	(void)k_work_reschedule(&rgb_restore_work, K_MSEC(RGB_RESTORE_DELAY_MS));
}

int rgb_led_init(void)
{
	int ret;

	if (rgb_initialized) {
		return rgb_led_restore();
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
	k_work_init_delayable(&rgb_restore_work, rgb_restore_work_handler);
	rgb_initialized = true;

	printk("rgb led strip ready: count=%u data=P0.%02u pwr=P0.%02u power_delay=%ums retries=%d\n",
	       (unsigned int)ARRAY_SIZE(rgb_pixels), RGB_DATA_PIN, rgb_power.pin,
	       RGB_POWER_ON_DELAY_MS, RGB_FRAME_RETRY_COUNT);

	ret = rgb_theme_load();
	if (ret != 0) {
		printk("rgb init continue with fallback color: %d\n", ret);
	}

	return rgb_led_restore();
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (!rgb_initialized) {
		return false;
	}

	if (is_wake_up_event(aeh)) {
		rgb_led_request_restore();
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, wake_up_event);
