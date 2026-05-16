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

#include "ble_hid_module.h"
#include "display_assets.h"
#include "display_module.h"
#include "mode_event.h"
#include "power_module.h"
#include "transport_manager.h"
#include "usb_hid_module.h"

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

#define COLOR_BG COLOR_RGB565(2, 4, 6)
#define COLOR_TEXT COLOR_RGB565(244, 247, 250)
#define COLOR_MUTED COLOR_RGB565(134, 145, 156)
#define COLOR_PANEL_BG COLOR_RGB565(16, 20, 26)
#define COLOR_PANEL_BORDER COLOR_RGB565(58, 64, 74)
#define COLOR_DIVIDER COLOR_RGB565(42, 48, 58)
#define COLOR_LOW COLOR_RGB565(232, 95, 76)
#define COLOR_MID COLOR_RGB565(234, 179, 66)
#define COLOR_GOOD COLOR_RGB565(88, 230, 108)
#define COLOR_USB COLOR_RGB565(66, 150, 255)
#define COLOR_BLE COLOR_RGB565(78, 214, 164)
#define COLOR_USB_BG COLOR_RGB565(8, 22, 40)
#define COLOR_BLE_BG COLOR_RGB565(8, 28, 24)
#define COLOR_DOT_OFF COLOR_RGB565(92, 99, 108)

#define DIVIDER_X1 108
#define DIVIDER_X2 208
#define DIVIDER_Y 18
#define DIVIDER_H (DISPLAY_HEIGHT - (DIVIDER_Y * 2))

#define LEFT_SECTION_X 12
#define LEFT_SECTION_W (DIVIDER_X1 - LEFT_SECTION_X - 10)
#define CENTER_SECTION_X (DIVIDER_X1 + 12)
#define CENTER_SECTION_W (DIVIDER_X2 - CENTER_SECTION_X - 12)
#define RIGHT_SECTION_X (DIVIDER_X2 + 12)
#define RIGHT_SECTION_W (DISPLAY_WIDTH - RIGHT_SECTION_X - 12)

#define BATTERY_ICON_X (LEFT_SECTION_X + 4)
#define BATTERY_ICON_Y 24
#define BATTERY_ICON_W (LEFT_SECTION_W - 8)
#define BATTERY_ICON_H 44

#define BATTERY_TEXT_Y 80
#define BATTERY_LABEL_Y 130

#define CENTER_VISUAL_Y 30
#define CENTER_STATUS_Y 116

#define MODE_TITLE_Y 18
#define MODE_ROW_X RIGHT_SECTION_X
#define MODE_ROW_W RIGHT_SECTION_W
#define MODE_ROW_H 38
#define MODE_USB_Y 50
#define MODE_BLE_Y 98

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
	bool usb_enabled;
	bool usb_ready;
	bool usb_vbus_present;
	bool ble_enabled;
	bool ble_ready;
	bool ble_connected;
	bool ble_advertising;
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

static uint16_t mode_fill_color(enum app_mode mode, bool selected)
{
	if (!selected) {
		return COLOR_PANEL_BG;
	}

	switch (mode) {
	case APP_MODE_USB:
		return COLOR_USB_BG;
	case APP_MODE_BLE:
	default:
		return COLOR_BLE_BG;
	}
}

static uint16_t mode_border_color(enum app_mode mode, bool selected)
{
	if (!selected) {
		return COLOR_PANEL_BORDER;
	}

	switch (mode) {
	case APP_MODE_USB:
		return COLOR_USB;
	case APP_MODE_BLE:
	default:
		return COLOR_BLE;
	}
}

static uint16_t mode_indicator_color(const struct display_state *state,
					 enum app_mode mode)
{
	switch (mode) {
	case APP_MODE_USB:
		if (state->usb_ready) {
			return COLOR_GOOD;
		}

		if (state->usb_vbus_present) {
			return COLOR_USB;
		}

		return COLOR_DOT_OFF;

	case APP_MODE_BLE:
	default:
		if (state->ble_connected) {
			return COLOR_GOOD;
		}

		if (state->ble_enabled && state->ble_advertising) {
			return COLOR_BLE;
		}

		if (state->ble_enabled && state->ble_ready) {
			return COLOR_MUTED;
		}

		return COLOR_DOT_OFF;
	}
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

static bool point_in_round_rect(int px, int py, int x, int y, int w, int h,
			       int radius)
{
	int local_x;
	int local_y;
	int cx;
	int cy;
	int dx;
	int dy;

	if ((px < x) || (px >= (x + w)) || (py < y) || (py >= (y + h))) {
		return false;
	}

	radius = MIN(radius, MIN(w, h) / 2);
	if (radius <= 0) {
		return true;
	}

	local_x = px - x;
	local_y = py - y;

	if ((local_x >= radius && local_x < (w - radius)) ||
	    (local_y >= radius && local_y < (h - radius))) {
		return true;
	}

	cx = (local_x < radius) ? (radius - 1) : (w - radius);
	cy = (local_y < radius) ? (radius - 1) : (h - radius);
	dx = local_x - cx;
	dy = local_y - cy;

	return (dx * dx + dy * dy) <= (radius * radius);
}

static void fill_round_rect(int strip_y, int strip_h, int x, int y, int w, int h,
			    int radius, uint16_t color)
{
	int x0 = CLAMP(x, 0, DISPLAY_WIDTH);
	int x1 = CLAMP(x + w, 0, DISPLAY_WIDTH);
	int y0 = MAX(y, strip_y);
	int y1 = MIN(y + h, strip_y + strip_h);

	if ((w <= 0) || (h <= 0) || (x0 >= x1) || (y0 >= y1)) {
		return;
	}

	for (int gy = y0; gy < y1; gy++) {
		uint16_t *row = &strip_buffer[(gy - strip_y) * DISPLAY_WIDTH];

		for (int gx = x0; gx < x1; gx++) {
			if (point_in_round_rect(gx, gy, x, y, w, h, radius)) {
				row[gx] = color;
			}
		}
	}
}

static void draw_round_rect_outline(int strip_y, int strip_h, int x, int y, int w,
				    int h, int radius, int thickness,
				    uint16_t border_color, uint16_t inner_color)
{
	fill_round_rect(strip_y, strip_h, x, y, w, h, radius, border_color);

	if ((w <= (thickness * 2)) || (h <= (thickness * 2))) {
		return;
	}

	fill_round_rect(strip_y, strip_h, x + thickness, y + thickness,
			w - (thickness * 2), h - (thickness * 2),
			MAX(radius - thickness, 0), inner_color);
}

static void fill_circle(int strip_y, int strip_h, int cx, int cy, int radius,
		       uint16_t color)
{
	int x0 = CLAMP(cx - radius, 0, DISPLAY_WIDTH);
	int x1 = CLAMP(cx + radius + 1, 0, DISPLAY_WIDTH);
	int y0 = MAX(cy - radius, strip_y);
	int y1 = MIN(cy + radius + 1, strip_y + strip_h);
	int radius_sq = radius * radius;

	if ((radius <= 0) || (x0 >= x1) || (y0 >= y1)) {
		return;
	}

	for (int gy = y0; gy < y1; gy++) {
		int dy = gy - cy;
		uint16_t *row = &strip_buffer[(gy - strip_y) * DISPLAY_WIDTH];

		for (int gx = x0; gx < x1; gx++) {
			int dx = gx - cx;

			if ((dx * dx + dy * dy) <= radius_sq) {
				row[gx] = color;
			}
		}
	}
}

static void draw_circle_ring(int strip_y, int strip_h, int cx, int cy, int radius,
			     int thickness, uint16_t ring_color,
			     uint16_t fill_color)
{
	fill_circle(strip_y, strip_h, cx, cy, radius, ring_color);

	if (radius > thickness) {
		fill_circle(strip_y, strip_h, cx, cy, radius - thickness, fill_color);
	}
}

static int int_abs(int value)
{
	return (value < 0) ? -value : value;
}

static void draw_line(int strip_y, int strip_h, int x0, int y0, int x1, int y1,
		      int thickness, uint16_t color)
{
	int dx = int_abs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -int_abs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;

	while (true) {
		fill_rect(strip_y, strip_h, x0 - (thickness / 2),
			  y0 - (thickness / 2), thickness, thickness, color);

		if ((x0 == x1) && (y0 == y1)) {
			break;
		}

		int e2 = err * 2;

		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}

		if (e2 <= dx) {
			err += dx;
			y0 += sy;
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

			fill_rect(strip_y, strip_h, x + (col * scale),
				  y + (row * scale), scale, scale, color);
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

static void draw_text_box_centered(int strip_y, int strip_h, int x, int w, int y,
				   const char *text, uint8_t scale,
				   uint16_t color)
{
	int width = text_width(text, scale);
	int start_x = x + (w - width) / 2;

	draw_text(strip_y, strip_h, MAX(start_x, x), y, text, scale, color);
}

static uint16_t rgb332_to_rgb565(uint8_t pixel)
{
	uint8_t r = (pixel >> 5) & 0x07U;
	uint8_t g = (pixel >> 2) & 0x07U;
	uint8_t b = pixel & 0x03U;
	uint8_t red = (r << 5) | (r << 2) | (r >> 1);
	uint8_t green = (g << 5) | (g << 2) | (g >> 1);
	uint8_t blue = (b << 6) | (b << 4) | (b << 2) | b;

	return COLOR_RGB565(red, green, blue);
}

static void draw_rgb332_bitmap(int strip_y, int strip_h, int x, int y,
			       const struct display_rgb332_bitmap *bitmap)
{
	int x0;
	int x1;
	int y0;
	int y1;

	if (bitmap == NULL) {
		return;
	}

	x0 = CLAMP(x, 0, DISPLAY_WIDTH);
	x1 = CLAMP(x + bitmap->width, 0, DISPLAY_WIDTH);
	y0 = MAX(y, strip_y);
	y1 = MIN(y + bitmap->height, strip_y + strip_h);

	if ((x0 >= x1) || (y0 >= y1)) {
		return;
	}

	for (int gy = y0; gy < y1; gy++) {
		int src_y = gy - y;
		const uint8_t *src = &bitmap->data[src_y * bitmap->width + (x0 - x)];
		uint16_t *dst = &strip_buffer[(gy - strip_y) * DISPLAY_WIDTH + x0];

		for (int gx = x0; gx < x1; gx++) {
			*dst++ = rgb332_to_rgb565(*src++);
		}
	}
}

static void draw_battery_icon(int strip_y, int strip_h, int x, int y, int w, int h,
			      bool valid, int percent, uint16_t level_color)
{
	int body_w = w - 12;
	int body_radius = 9;
	int body_x = x;
	int cap_x = x + body_w;
	int cap_y = y + (h / 4);
	int cap_h = h / 2;
	int inner_x = body_x + 6;
	int inner_y = y + 6;
	int inner_w = body_w - 12;
	int inner_h = h - 12;

	draw_round_rect_outline(strip_y, strip_h, body_x, y, body_w, h, body_radius, 3,
				COLOR_TEXT, COLOR_BG);
	draw_round_rect_outline(strip_y, strip_h, cap_x, cap_y, 10, cap_h, 3, 2,
				COLOR_TEXT, COLOR_BG);
	fill_round_rect(strip_y, strip_h, inner_x, inner_y, inner_w, inner_h, 5,
			COLOR_PANEL_BG);

	if (valid && (percent > 0)) {
		int fill_w = (inner_w * CLAMP(percent, 0, 100)) / 100;

		if (fill_w > 0) {
			fill_round_rect(strip_y, strip_h, inner_x, inner_y, fill_w, inner_h, 5,
					level_color);
		}
	}

	for (int i = 1; i < 4; i++) {
		int separator_x = inner_x + (i * inner_w) / 4;

		fill_rect(strip_y, strip_h, separator_x - 1, inner_y + 2, 3,
			  inner_h - 4, COLOR_BG);
	}
}

static void draw_charge_symbol(int strip_y, int strip_h, int center_x, int center_y,
			       uint16_t color)
{
	draw_line(strip_y, strip_h, center_x + 8, center_y - 18,
		  center_x - 2, center_y - 2, 4, color);
	draw_line(strip_y, strip_h, center_x - 2, center_y - 2,
		  center_x + 9, center_y - 2, 4, color);
	draw_line(strip_y, strip_h, center_x + 9, center_y - 2,
		  center_x - 8, center_y + 18, 4, color);
	draw_line(strip_y, strip_h, center_x - 8, center_y + 18,
		  center_x + 1, center_y + 6, 4, color);
}

static void render_left_section(int strip_y, int strip_h,
				const struct display_state *state)
{
	char battery_text[8];
	uint16_t level_color = state->battery_valid ?
				battery_color(state->battery_percent) : COLOR_MUTED;
	uint8_t text_scale;

	if (state->battery_valid) {
		snprintk(battery_text, sizeof(battery_text), "%d%%",
			 state->battery_percent);
	} else {
		snprintk(battery_text, sizeof(battery_text), "--%%");
	}

	draw_battery_icon(strip_y, strip_h, BATTERY_ICON_X, BATTERY_ICON_Y,
			  BATTERY_ICON_W, BATTERY_ICON_H, state->battery_valid,
			  state->battery_percent, level_color);

	text_scale = (text_width(battery_text, 4) <= LEFT_SECTION_W) ? 4 : 3;
	draw_text_box_centered(strip_y, strip_h, LEFT_SECTION_X, LEFT_SECTION_W,
			       BATTERY_TEXT_Y, battery_text, text_scale,
			       state->battery_valid ? COLOR_TEXT : COLOR_MUTED);
	draw_text_box_centered(strip_y, strip_h, LEFT_SECTION_X, LEFT_SECTION_W,
			       BATTERY_LABEL_Y, "BAT", 2, COLOR_MUTED);
}

static void render_center_section(int strip_y, int strip_h,
				  const struct display_state *state)
{
	int frame_x;
	int frame_y;
	int image_x;
	int image_y;
	int center_x = CENTER_SECTION_X + (CENTER_SECTION_W / 2);
	uint16_t status_color = COLOR_GOOD;

	if (state->charging || state->full) {
		draw_circle_ring(strip_y, strip_h, center_x, 58, 34, 6,
				 status_color, COLOR_BG);
		draw_charge_symbol(strip_y, strip_h, center_x, 58, status_color);
		draw_text_box_centered(strip_y, strip_h, CENTER_SECTION_X,
				       CENTER_SECTION_W, CENTER_STATUS_Y,
				       state->full ? "FULL" : "CHG",
				       state->full ? 3 : 4, status_color);
		return;
	}

	image_x = CENTER_SECTION_X +
		  (CENTER_SECTION_W - display_idle_bitmap.width) / 2;
	image_y = CENTER_VISUAL_Y;
	frame_x = image_x - 3;
	frame_y = image_y - 3;

	fill_round_rect(strip_y, strip_h, frame_x, frame_y,
			display_idle_bitmap.width + 6,
			display_idle_bitmap.height + 6, 8, COLOR_PANEL_BG);
	draw_round_rect_outline(strip_y, strip_h, frame_x, frame_y,
				display_idle_bitmap.width + 6,
				display_idle_bitmap.height + 6, 8, 1,
				COLOR_PANEL_BORDER, COLOR_PANEL_BG);
	draw_rgb332_bitmap(strip_y, strip_h, image_x, image_y,
			   &display_idle_bitmap);
}

static void render_mode_row(int strip_y, int strip_h, int y, const char *label,
			    enum app_mode mode, const struct display_state *state)
{
	bool selected = (state->mode == mode);
	uint16_t fill = mode_fill_color(mode, selected);
	uint16_t border = mode_border_color(mode, selected);
	uint16_t indicator = mode_indicator_color(state, mode);
	int dot_center_x = MODE_ROW_X + MODE_ROW_W - 14;
	int dot_center_y = y + (MODE_ROW_H / 2);

	fill_round_rect(strip_y, strip_h, MODE_ROW_X, y, MODE_ROW_W, MODE_ROW_H, 9,
			fill);
	draw_round_rect_outline(strip_y, strip_h, MODE_ROW_X, y, MODE_ROW_W,
				MODE_ROW_H, 9, selected ? 2 : 1, border, fill);
	draw_text_box_centered(strip_y, strip_h, MODE_ROW_X + 6, MODE_ROW_W - 26,
			       y + 8, label, 3, COLOR_TEXT);
	fill_circle(strip_y, strip_h, dot_center_x, dot_center_y, 5, indicator);
}

static void render_right_section(int strip_y, int strip_h,
				 const struct display_state *state)
{
	draw_text_box_centered(strip_y, strip_h, RIGHT_SECTION_X, RIGHT_SECTION_W,
			       MODE_TITLE_Y, "MODE", 3, COLOR_TEXT);
	render_mode_row(strip_y, strip_h, MODE_USB_Y, "USB", APP_MODE_USB, state);
	render_mode_row(strip_y, strip_h, MODE_BLE_Y, "BLE", APP_MODE_BLE, state);
}

static void collect_state(struct display_state *state)
{
	struct power_module_status power_status;

	power_module_get_status(&power_status);

	state->battery_valid = power_status.valid;
	state->charging = power_status.charging;
	state->full = power_status.full;
	state->battery_percent = power_status.battery_percent;
	state->mode = transport_manager_get_active_mode();
	state->usb_enabled = usb_hid_module_is_enabled();
	state->usb_ready = usb_hid_module_is_ready();
	state->usb_vbus_present = usb_hid_module_has_vbus();
	state->ble_enabled = ble_hid_module_is_enabled();
	state->ble_ready = ble_hid_module_is_ready();
	state->ble_connected = ble_hid_module_is_connected();
	state->ble_advertising = ble_hid_module_is_advertising();
}

static bool state_equals(const struct display_state *lhs,
			 const struct display_state *rhs)
{
	return (lhs->battery_valid == rhs->battery_valid) &&
	       (lhs->charging == rhs->charging) &&
	       (lhs->full == rhs->full) &&
	       (lhs->battery_percent == rhs->battery_percent) &&
	       (lhs->mode == rhs->mode) &&
	       (lhs->usb_enabled == rhs->usb_enabled) &&
	       (lhs->usb_ready == rhs->usb_ready) &&
	       (lhs->usb_vbus_present == rhs->usb_vbus_present) &&
	       (lhs->ble_enabled == rhs->ble_enabled) &&
	       (lhs->ble_ready == rhs->ble_ready) &&
	       (lhs->ble_connected == rhs->ble_connected) &&
	       (lhs->ble_advertising == rhs->ble_advertising);
}

static void render_strip(int strip_y, int strip_h, const struct display_state *state)
{
	for (int i = 0; i < (DISPLAY_WIDTH * strip_h); i++) {
		strip_buffer[i] = COLOR_BG;
	}

	fill_rect(strip_y, strip_h, DIVIDER_X1, DIVIDER_Y, 2, DIVIDER_H, COLOR_DIVIDER);
	fill_rect(strip_y, strip_h, DIVIDER_X2, DIVIDER_Y, 2, DIVIDER_H, COLOR_DIVIDER);

	render_left_section(strip_y, strip_h, state);
	render_center_section(strip_y, strip_h, state);
	render_right_section(strip_y, strip_h, state);
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
