#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <app_event_manager.h>

#define MODULE display_module
#include <caf/events/power_event.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "display_module.h"
#include "mode_event.h"
#include "mode_switch.h"
#include "power_module.h"

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define DISPLAY_USER_NODE DT_PATH(zephyr_user)

#define DISPLAY_REFRESH_INTERVAL_MS 1000
#define DISPLAY_THREAD_STACK_SIZE 2048
#define DISPLAY_THREAD_PRIORITY 7
#define DISPLAY_STRIP_HEIGHT 24
#define DISPLAY_BACKLIGHT_PERCENT 100

#define DISPLAY_WIDTH DT_PROP(DISPLAY_NODE, width)
#define DISPLAY_HEIGHT DT_PROP(DISPLAY_NODE, height)

#define DISPLAY_FLAG_INITIALIZED 0
#define DISPLAY_FLAG_READY 1
#define DISPLAY_FLAG_SLEEPING 2
#define DISPLAY_FLAG_FORCE_REFRESH 3

#define COLOR_RGB565(r, g, b) \
	((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | \
	 (((uint16_t)(b) & 0xF8U) >> 3))

#define COLOR_BG COLOR_RGB565(8, 16, 24)
#define COLOR_FG COLOR_RGB565(245, 248, 250)
#define COLOR_MUTED COLOR_RGB565(144, 156, 169)
#define COLOR_BAR_BG COLOR_RGB565(36, 44, 56)
#define COLOR_LOW COLOR_RGB565(232, 95, 76)
#define COLOR_MID COLOR_RGB565(234, 179, 66)
#define COLOR_GOOD COLOR_RGB565(82, 196, 120)
#define COLOR_USB COLOR_RGB565(89, 174, 255)
#define COLOR_BLE COLOR_RGB565(64, 214, 145)
#define COLOR_24G COLOR_RGB565(242, 168, 60)

#define BATTERY_TEXT_Y 18
#define STATUS_TEXT_Y 58
#define MODE_TEXT_Y 100
#define BAR_X 20
#define BAR_Y 145
#define BAR_WIDTH (DISPLAY_WIDTH - (BAR_X * 2))
#define BAR_HEIGHT 12

BUILD_ASSERT(DT_NODE_HAS_STATUS(DISPLAY_NODE, okay), "display node must be enabled");

static const struct device *const display_dev = DEVICE_DT_GET(DISPLAY_NODE);
static const struct pwm_dt_spec backlight_pwm =
	PWM_DT_SPEC_GET_BY_NAME(DISPLAY_USER_NODE, lcd_backlight);

struct display_state {
	bool battery_valid;
	bool charging;
	bool full;
	int battery_percent;
	enum app_mode mode;
};

static atomic_t display_flags;
static struct display_state last_state;
static bool last_state_valid;
static struct k_work_q display_work_q;
static struct k_work_delayable display_refresh_work;
static struct display_capabilities display_caps;
static uint16_t strip_buffer[DISPLAY_WIDTH * DISPLAY_STRIP_HEIGHT];

K_THREAD_STACK_DEFINE(display_thread_stack, DISPLAY_THREAD_STACK_SIZE);

static const uint8_t glyph_space[7];
static const uint8_t glyph_dash[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
static const uint8_t glyph_dot[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };
static const uint8_t glyph_percent[7] = { 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13 };
static const uint8_t glyph_0[7] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E };
static const uint8_t glyph_1[7] = { 0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F };
static const uint8_t glyph_2[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F };
static const uint8_t glyph_3[7] = { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E };
static const uint8_t glyph_4[7] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 };
static const uint8_t glyph_5[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E };
static const uint8_t glyph_6[7] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E };
static const uint8_t glyph_7[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 };
static const uint8_t glyph_8[7] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E };
static const uint8_t glyph_9[7] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C };
static const uint8_t glyph_A[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
static const uint8_t glyph_B[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
static const uint8_t glyph_C[7] = { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F };
static const uint8_t glyph_D[7] = { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E };
static const uint8_t glyph_E[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
static const uint8_t glyph_F[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 };
static const uint8_t glyph_G[7] = { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F };
static const uint8_t glyph_H[7] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
static const uint8_t glyph_L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
static const uint8_t glyph_M[7] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 };
static const uint8_t glyph_O[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
static const uint8_t glyph_S[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
static const uint8_t glyph_T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
static const uint8_t glyph_U[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };

static const uint8_t *glyph_for_char(char c)
{
	switch (c) {
	case '0':
		return glyph_0;
	case '1':
		return glyph_1;
	case '2':
		return glyph_2;
	case '3':
		return glyph_3;
	case '4':
		return glyph_4;
	case '5':
		return glyph_5;
	case '6':
		return glyph_6;
	case '7':
		return glyph_7;
	case '8':
		return glyph_8;
	case '9':
		return glyph_9;
	case 'A':
		return glyph_A;
	case 'B':
		return glyph_B;
	case 'C':
		return glyph_C;
	case 'D':
		return glyph_D;
	case 'E':
		return glyph_E;
	case 'F':
		return glyph_F;
	case 'G':
		return glyph_G;
	case 'H':
		return glyph_H;
	case 'L':
		return glyph_L;
	case 'M':
		return glyph_M;
	case 'O':
		return glyph_O;
	case 'S':
		return glyph_S;
	case 'T':
		return glyph_T;
	case 'U':
		return glyph_U;
	case '-':
		return glyph_dash;
	case '.':
		return glyph_dot;
	case '%':
		return glyph_percent;
	case ' ':
	default:
		return glyph_space;
	}
}

static const char *mode_text(enum app_mode mode)
{
	switch (mode) {
	case APP_MODE_USB:
		return "USB";
	case APP_MODE_24G:
		return "2.4G";
	case APP_MODE_BLE:
	default:
		return "BLE";
	}
}

static uint16_t mode_color(enum app_mode mode)
{
	switch (mode) {
	case APP_MODE_USB:
		return COLOR_USB;
	case APP_MODE_24G:
		return COLOR_24G;
	case APP_MODE_BLE:
	default:
		return COLOR_BLE;
	}
}

static uint16_t battery_color(int percent)
{
	if (percent <= 20) {
		return COLOR_LOW;
	}

	if (percent <= 50) {
		return COLOR_MID;
	}

	return COLOR_GOOD;
}

static int set_backlight_percent(uint8_t percent)
{
	uint32_t pulse;

	if (!device_is_ready(backlight_pwm.dev)) {
		return -ENODEV;
	}

	pulse = ((uint64_t)backlight_pwm.period * percent) / 100U;
	return pwm_set_pulse_dt(&backlight_pwm, pulse);
}

static void fill_rect(int strip_y, int strip_h, int x, int y, int w, int h,
		      uint16_t color)
{
	int x0 = CLAMP(x, 0, DISPLAY_WIDTH);
	int x1 = CLAMP(x + w, 0, DISPLAY_WIDTH);
	int y0 = MAX(y, strip_y);
	int y1 = MIN(y + h, strip_y + strip_h);

	if ((x0 >= x1) || (y0 >= y1)) {
		return;
	}

	for (int gy = y0; gy < y1; gy++) {
		uint16_t *row = &strip_buffer[(gy - strip_y) * DISPLAY_WIDTH + x0];

		for (int gx = x0; gx < x1; gx++) {
			*row++ = color;
		}
	}
}

static void draw_char(int strip_y, int strip_h, int x, int y, char c,
		      uint8_t scale, uint16_t color)
{
	const uint8_t *glyph = glyph_for_char(c);

	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < 5; col++) {
			if ((glyph[row] & BIT(4 - col)) == 0U) {
				continue;
			}

			fill_rect(strip_y, strip_h,
				  x + (col * scale),
				  y + (row * scale),
				  scale, scale, color);
		}
	}
}

static int text_width(const char *text, uint8_t scale)
{
	size_t len = strlen(text);

	if (len == 0U) {
		return 0;
	}

	return (int)(len * (6U * scale)) - (int)scale;
}

static void draw_text(int strip_y, int strip_h, int x, int y, const char *text,
		      uint8_t scale, uint16_t color)
{
	while (*text != '\0') {
		draw_char(strip_y, strip_h, x, y, *text, scale, color);
		x += 6 * scale;
		text++;
	}
}

static void draw_text_centered(int strip_y, int strip_h, int y, const char *text,
			       uint8_t scale, uint16_t color)
{
	int x = (DISPLAY_WIDTH - text_width(text, scale)) / 2;

	draw_text(strip_y, strip_h, MAX(x, 0), y, text, scale, color);
}

static void collect_state(struct display_state *state)
{
	struct power_module_status power_status;

	power_module_get_status(&power_status);

	state->battery_valid = power_status.valid;
	state->charging = power_status.charging;
	state->full = power_status.full;
	state->battery_percent = power_status.battery_percent;
	state->mode = mode_switch_get_mode();
}

static bool state_equals(const struct display_state *lhs,
			 const struct display_state *rhs)
{
	return (lhs->battery_valid == rhs->battery_valid) &&
	       (lhs->charging == rhs->charging) &&
	       (lhs->full == rhs->full) &&
	       (lhs->battery_percent == rhs->battery_percent) &&
	       (lhs->mode == rhs->mode);
}

static void render_strip(int strip_y, int strip_h, const struct display_state *state)
{
	char battery_text[16];
	char mode_line[16];
	const char *status_text;
	uint16_t battery_fg;
	int inner_width;
	int fill_width;

	for (int i = 0; i < (DISPLAY_WIDTH * strip_h); i++) {
		strip_buffer[i] = COLOR_BG;
	}

	if (state->battery_valid) {
		snprintk(battery_text, sizeof(battery_text), "BAT %d%%",
			 state->battery_percent);
	} else {
		snprintk(battery_text, sizeof(battery_text), "BAT --%%");
	}

	if (state->full) {
		status_text = "FULL";
	} else if (state->charging) {
		status_text = "CHG";
	} else {
		status_text = "";
	}

	battery_fg = state->battery_valid ? battery_color(state->battery_percent) : COLOR_FG;
	draw_text_centered(strip_y, strip_h, BATTERY_TEXT_Y, battery_text, 4, battery_fg);

	if (status_text[0] != '\0') {
		draw_text_centered(strip_y, strip_h, STATUS_TEXT_Y, status_text, 3,
				   COLOR_MUTED);
	}

	fill_rect(strip_y, strip_h, BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, COLOR_MUTED);
	fill_rect(strip_y, strip_h, BAR_X + 2, BAR_Y + 2, BAR_WIDTH - 4, BAR_HEIGHT - 4,
		  COLOR_BAR_BG);

	if (state->battery_valid && (state->battery_percent > 0)) {
		inner_width = BAR_WIDTH - 4;
		fill_width = (inner_width * state->battery_percent) / 100;
		fill_rect(strip_y, strip_h, BAR_X + 2, BAR_Y + 2, fill_width,
			  BAR_HEIGHT - 4, battery_fg);
	}

	snprintk(mode_line, sizeof(mode_line), "MODE %s", mode_text(state->mode));

	draw_text_centered(strip_y, strip_h, MODE_TEXT_Y, mode_line, 4,
			   mode_color(state->mode));
}

static int render_display(const struct display_state *state)
{
	int err = 0;

	for (int strip_y = 0; strip_y < DISPLAY_HEIGHT; strip_y += DISPLAY_STRIP_HEIGHT) {
		struct display_buffer_descriptor desc;
		int strip_h = MIN(DISPLAY_STRIP_HEIGHT, DISPLAY_HEIGHT - strip_y);

		render_strip(strip_y, strip_h, state);

		desc.buf_size = DISPLAY_WIDTH * strip_h * sizeof(strip_buffer[0]);
		desc.width = DISPLAY_WIDTH;
		desc.height = strip_h;
		desc.pitch = DISPLAY_WIDTH;
		desc.frame_incomplete = (strip_y + strip_h) < DISPLAY_HEIGHT;

		err = display_write(display_dev, 0, strip_y, &desc, strip_buffer);
		if (err != 0) {
			printk("display write failed: %d\n", err);
			return err;
		}
	}

	if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_SLEEPING)) {
		err = display_blanking_off(display_dev);
		if (err != 0) {
			printk("display blanking off failed: %d\n", err);
			return err;
		}

		err = set_backlight_percent(DISPLAY_BACKLIGHT_PERCENT);
		if ((err != 0) && (err != -ENODEV)) {
			printk("display backlight set failed: %d\n", err);
			return err;
		}
	}

	return 0;
}

static void display_enter_sleep(void)
{
	int err;

	if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_READY)) {
		return;
	}

	err = set_backlight_percent(0);
	if ((err != 0) && (err != -ENODEV)) {
		printk("display backlight off failed: %d\n", err);
	}

	err = display_blanking_on(display_dev);
	if (err != 0) {
		printk("display blanking on failed: %d\n", err);
	}
}

static void display_refresh_work_handler(struct k_work *work)
{
	struct display_state current_state;
	bool force_refresh;
	int err;

	ARG_UNUSED(work);

	if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_READY)) {
		return;
	}

	if (atomic_test_bit(&display_flags, DISPLAY_FLAG_SLEEPING)) {
		return;
	}

	collect_state(&current_state);
	force_refresh = atomic_test_and_clear_bit(&display_flags,
						       DISPLAY_FLAG_FORCE_REFRESH);

	if (!force_refresh && last_state_valid && state_equals(&current_state, &last_state)) {
		goto reschedule;
	}

	err = render_display(&current_state);
	if (err == 0) {
		last_state = current_state;
		last_state_valid = true;
	}

reschedule:
	k_work_reschedule_for_queue(&display_work_q, &display_refresh_work,
				    K_MSEC(DISPLAY_REFRESH_INTERVAL_MS));
}

int display_module_init(void)
{
	int err;

	k_work_queue_start(&display_work_q, display_thread_stack,
			   K_THREAD_STACK_SIZEOF(display_thread_stack),
			   DISPLAY_THREAD_PRIORITY, NULL);
	k_work_init_delayable(&display_refresh_work, display_refresh_work_handler);

	atomic_set_bit(&display_flags, DISPLAY_FLAG_INITIALIZED);
	atomic_set_bit(&display_flags, DISPLAY_FLAG_FORCE_REFRESH);

	if (!device_is_ready(display_dev)) {
		printk("display not ready, keep keyboard running\n");
		return 0;
	}

	if (!device_is_ready(backlight_pwm.dev)) {
		printk("display backlight pwm not ready, continue without dimming control\n");
	}

	display_get_capabilities(display_dev, &display_caps);
	if (display_caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		printk("display pixel format unsupported: 0x%x\n",
		       display_caps.current_pixel_format);
		return 0;
	}

	err = display_blanking_on(display_dev);
	if (err != 0) {
		printk("display blanking on failed: %d\n", err);
		return 0;
	}

	err = set_backlight_percent(0);
	if ((err != 0) && (err != -ENODEV)) {
		printk("display backlight init failed: %d\n", err);
	}

	atomic_set_bit(&display_flags, DISPLAY_FLAG_READY);
	k_work_schedule_for_queue(&display_work_q, &display_refresh_work, K_NO_WAIT);

	printk("display ready: %ux%u spi3 cs=P0.02 dc=P0.03 rst=P1.10 bl=P1.11\n",
	       display_caps.x_resolution, display_caps.y_resolution);
	return 0;
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_INITIALIZED)) {
		return false;
	}

	if (is_power_down_event(aeh)) {
		atomic_set_bit(&display_flags, DISPLAY_FLAG_SLEEPING);
		atomic_set_bit(&display_flags, DISPLAY_FLAG_FORCE_REFRESH);
		(void)k_work_cancel_delayable(&display_refresh_work);
		display_enter_sleep();
		return false;
	}

	if (is_wake_up_event(aeh)) {
		if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_READY)) {
			return false;
		}

		atomic_clear_bit(&display_flags, DISPLAY_FLAG_SLEEPING);
		atomic_set_bit(&display_flags, DISPLAY_FLAG_FORCE_REFRESH);
		k_work_reschedule_for_queue(&display_work_q, &display_refresh_work,
					    K_NO_WAIT);
		return false;
	}

	if (is_mode_changed_event(aeh)) {
		if (!atomic_test_bit(&display_flags, DISPLAY_FLAG_READY) ||
		    atomic_test_bit(&display_flags, DISPLAY_FLAG_SLEEPING)) {
			return false;
		}

		atomic_set_bit(&display_flags, DISPLAY_FLAG_FORCE_REFRESH);
		k_work_reschedule_for_queue(&display_work_q, &display_refresh_work,
					    K_NO_WAIT);
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, mode_changed_event);
APP_EVENT_SUBSCRIBE(MODULE, wake_up_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, power_down_event);
