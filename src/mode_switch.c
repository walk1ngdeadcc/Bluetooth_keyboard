#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <app_event_manager.h>

#include "mode_event.h"
#include "mode_switch.h"

#define MODE_ADC_NODE DT_PATH(zephyr_user)

#define MODE_SAMPLE_INTERVAL_MS 500
#define MODE_STABLE_COUNT 2

#define MODE_USB_THRESHOLD_MV 825
#define MODE_BLE_THRESHOLD_MV 2475

struct mode_sample {
	int32_t voltage_mv;
	enum app_mode state;
};

static const struct adc_dt_spec mode_adc = ADC_DT_SPEC_GET_BY_NAME(MODE_ADC_NODE, mode);
static struct k_work_delayable mode_sample_work;
static enum app_mode current_mode;
static enum app_mode pending_mode;
static uint8_t pending_count;
static bool mode_initialized;

static enum app_mode mode_switch_decode(int32_t voltage_mv)
{
	if (voltage_mv < MODE_USB_THRESHOLD_MV) {
		return APP_MODE_USB;
	}

	if (voltage_mv < MODE_BLE_THRESHOLD_MV) {
		return APP_MODE_24G;
	}

	return APP_MODE_BLE;
}

static const char *mode_switch_name(enum app_mode state)
{
	switch (state) {
	case APP_MODE_USB:
		return "USB";
	case APP_MODE_24G:
		return "2.4G";
	case APP_MODE_BLE:
	default:
		return "BLE";
	}
}

static int mode_switch_read_sample(struct mode_sample *sample)
{
	int16_t sample_buffer;
	struct adc_sequence sequence = {
		.buffer = &sample_buffer,
		.buffer_size = sizeof(sample_buffer),
	};
	int32_t voltage_mv;
	int err;

	err = adc_sequence_init_dt(&mode_adc, &sequence);
	if (err != 0) {
		return err;
	}

	err = adc_read_dt(&mode_adc, &sequence);
	if (err != 0) {
		return err;
	}

	voltage_mv = sample_buffer;
	err = adc_raw_to_millivolts_dt(&mode_adc, &voltage_mv);
	if (err != 0) {
		return err;
	}

	sample->voltage_mv = voltage_mv;
	sample->state = mode_switch_decode(voltage_mv);

	return 0;
}

static void mode_switch_submit(enum app_mode state, int32_t voltage_mv)
{
	struct mode_changed_event *event = new_mode_changed_event();

	event->mode = state;
	event->voltage_mv = voltage_mv;
	APP_EVENT_SUBMIT(event);

	printk("mode switch: %s, voltage=%dmV\n",
	       mode_switch_name(state), voltage_mv);
}

static void mode_sample_work_handler(struct k_work *work)
{
	struct mode_sample sample;
	int err;

	ARG_UNUSED(work);

	err = mode_switch_read_sample(&sample);
	if (err != 0) {
		printk("mode adc read failed: %d\n", err);
		goto reschedule;
	}

	if (!mode_initialized) {
		mode_initialized = true;
		current_mode = sample.state;
		pending_mode = sample.state;
		pending_count = 0;
		mode_switch_submit(sample.state, sample.voltage_mv);
		goto reschedule;
	}

	if (sample.state == current_mode) {
		pending_mode = current_mode;
		pending_count = 0;
		goto reschedule;
	}

	if (sample.state != pending_mode) {
		pending_mode = sample.state;
		pending_count = 1;
		goto reschedule;
	}

	if (pending_count < UINT8_MAX) {
		pending_count++;
	}

	if (pending_count >= MODE_STABLE_COUNT) {
		current_mode = sample.state;
		pending_mode = sample.state;
		pending_count = 0;
		mode_switch_submit(sample.state, sample.voltage_mv);
	}

reschedule:
	k_work_reschedule(&mode_sample_work, K_MSEC(MODE_SAMPLE_INTERVAL_MS));
}

int mode_switch_init(void)
{
	int err;

	if (!adc_is_ready_dt(&mode_adc)) {
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&mode_adc);
	if (err != 0) {
		return err;
	}

	k_work_init_delayable(&mode_sample_work, mode_sample_work_handler);
	k_work_schedule(&mode_sample_work, K_NO_WAIT);

	printk("mode switch ready: adc=P0.29(AIN5) interval=%dms usb<%dmV 2.4G<%dmV ble>=%dmV\n",
	       MODE_SAMPLE_INTERVAL_MS, MODE_USB_THRESHOLD_MV,
	       MODE_BLE_THRESHOLD_MV, MODE_BLE_THRESHOLD_MV);

	return 0;
}

enum app_mode mode_switch_get_mode(void)
{
	return current_mode;
}
