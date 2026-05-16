#include <stdbool.h>
#include <stdint.h>

#include <app_event_manager.h>

#define MODULE power_module
#include <caf/events/keep_alive_event.h>
#include <caf/events/module_state_event.h>
#include <caf/events/power_event.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "power_module.h"

#define IP5306_NODE DT_NODELABEL(ip5306)
#define BATTERY_ADC_NODE DT_PATH(zephyr_user)

#define IP5306_I2C_ADDR_7BIT 0x75
#define IP5306_REG_SYS_CTL2 0x02
#define IP5306_REG_READ0 0x70
#define IP5306_REG_READ1 0x71

#define IP5306_READ0_CHARGE_EN BIT(3)
#define IP5306_READ1_CHARGE_FULL BIT(3)
#define IP5306_SYS_CTL2_LIGHT_SHUTDOWN_MASK GENMASK(3, 2)
#define IP5306_SYS_CTL2_LIGHT_SHUTDOWN_64S (BIT(3) | BIT(2))

#define POWER_POLL_INTERVAL_MS 1000//读取电压时间间隔

#define POWER_KEEPALIVE_INTERVAL_MS 2000 //发送脉冲保活间隔
#define IP5306_KEY_PULSE_MS 100//脉冲宽度
#define BAT_ADC_SETTLE_MS 10//BAT_ADC_EN 使能后等待时间
#define IP5306_PROBE_RETRY_COUNT 5
#define IP5306_PROBE_RETRY_DELAY_MS 50

#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200
#define BATTERY_DIVIDER_NUM 2
#define BAT_ADC_EN_ACTIVE_LEVEL 1

enum battery_charge_state {
	BATTERY_NOT_CHARGING = 0,
	BATTERY_CHARGING,
	BATTERY_FULL,
};

static const struct i2c_dt_spec ip5306_i2c = I2C_DT_SPEC_GET(IP5306_NODE);
static const struct gpio_dt_spec ip5306_key =
	GPIO_DT_SPEC_GET(IP5306_NODE, key_gpios);
static const struct gpio_dt_spec bat_adc_en =
	GPIO_DT_SPEC_GET(IP5306_NODE, bat_adc_en_gpios);
static const struct adc_dt_spec bat_adc = ADC_DT_SPEC_GET(BATTERY_ADC_NODE);

static struct k_work_delayable power_poll_work;
static bool power_in_low_power;
static bool power_wake_sent;
static enum battery_charge_state last_charge_state = -1;
static int last_percent = -1;
static uint32_t last_keepalive_ms;
static int32_t last_battery_mv = -1;
static int32_t last_bat_adc_node_mv = -1;
static int16_t last_bat_adc_raw = 0;

static const char *battery_charge_state_name(enum battery_charge_state state)
{
	switch (state) {
	case BATTERY_CHARGING:
		return "充电中";
	case BATTERY_FULL:
		return "已充满";
	case BATTERY_NOT_CHARGING:
	default:
		return "未充电";
	}
}

static int ip5306_reg_read(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte_dt(&ip5306_i2c, reg, val);
}

static int ip5306_reg_update(uint8_t reg, uint8_t mask, uint8_t value)
{
	return i2c_reg_update_byte_dt(&ip5306_i2c, reg, mask, value);
}

static int ip5306_keepalive_pulse(void)
{
	int err;

	err = gpio_pin_set_dt(&ip5306_key, 0);
	if (err != 0) {
		return err;
	}

	k_msleep(IP5306_KEY_PULSE_MS);

	return gpio_pin_set_dt(&ip5306_key, 1);
}

static int ip5306_wait_ready(uint8_t *sys_ctl2)
{
	int err;

	err = ip5306_keepalive_pulse();
	if (err != 0) {
		return err;
	}

	for (int i = 0; i < IP5306_PROBE_RETRY_COUNT; i++) {
		err = ip5306_reg_read(IP5306_REG_SYS_CTL2, sys_ctl2);
		if (err == 0) {
			return 0;
		}

		k_msleep(IP5306_PROBE_RETRY_DELAY_MS);
	}

	return err;
}

static enum battery_charge_state ip5306_decode_state(uint8_t reg70, uint8_t reg71)
{
	if ((reg71 & IP5306_READ1_CHARGE_FULL) != 0U) {
		return BATTERY_FULL;
	}

	if ((reg70 & IP5306_READ0_CHARGE_EN) != 0U) {
		return BATTERY_CHARGING;
	}

	return BATTERY_NOT_CHARGING;
}

static int battery_percent_from_mv(int32_t mv)
{
	int32_t raw;
	int32_t rounded;

	if (mv <= BATTERY_EMPTY_MV) {
		return 0;
	}

	if (mv >= BATTERY_FULL_MV) {
		return 100;
	}

	raw = (mv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
	rounded = ((raw + 5) / 10) * 10;

	if (rounded < 0) {
		return 0;
	}

	if (rounded > 100) {
		return 100;
	}

	return rounded;
}

static int battery_read_mv(int32_t *battery_mv)
{
	int16_t sample_buffer;
	struct adc_sequence sequence = {
		.buffer = &sample_buffer,
		.buffer_size = sizeof(sample_buffer),
	};
	struct adc_sequence discard_sequence = {
		.buffer = &sample_buffer,
		.buffer_size = sizeof(sample_buffer),
	};
	int32_t mv;
	int err;

	err = gpio_pin_set_dt(&bat_adc_en, BAT_ADC_EN_ACTIVE_LEVEL);
	if (err != 0) {
		return err;
	}

	k_msleep(BAT_ADC_SETTLE_MS);

	err = adc_sequence_init_dt(&bat_adc, &sequence);
	if (err != 0) {
		gpio_pin_set_dt(&bat_adc_en, !BAT_ADC_EN_ACTIVE_LEVEL);
		return err;
	}

	discard_sequence.channels = sequence.channels;
	discard_sequence.resolution = sequence.resolution;
	discard_sequence.oversampling = sequence.oversampling;
	discard_sequence.calibrate = false;
	discard_sequence.options = NULL;

	err = adc_read_dt(&bat_adc, &discard_sequence);
	if (err != 0) {
		gpio_pin_set_dt(&bat_adc_en, !BAT_ADC_EN_ACTIVE_LEVEL);
		return err;
	}

	err = adc_read_dt(&bat_adc, &sequence);
	if (err != 0) {
		gpio_pin_set_dt(&bat_adc_en, !BAT_ADC_EN_ACTIVE_LEVEL);
		return err;
	}

	mv = sample_buffer;
	err = adc_raw_to_millivolts_dt(&bat_adc, &mv);

	gpio_pin_set_dt(&bat_adc_en, !BAT_ADC_EN_ACTIVE_LEVEL);

	if (err != 0) {
		return err;
	}

	last_bat_adc_raw = sample_buffer;
	last_bat_adc_node_mv = mv;
	last_battery_mv = mv * BATTERY_DIVIDER_NUM;
	*battery_mv = last_battery_mv;
	return 0;
}

static void power_poll_work_handler(struct k_work *work)
{
	uint8_t reg70;
	uint8_t reg71;
	enum battery_charge_state charge_state;
	int32_t battery_mv;
	int battery_percent;
	int64_t now_ms;
	int err;

	ARG_UNUSED(work);

	err = ip5306_reg_read(IP5306_REG_READ0, &reg70);
	if (err != 0) {
		printk("ip5306 read 0x70 failed: %d\n", err);
		goto reschedule;
	}

	err = ip5306_reg_read(IP5306_REG_READ1, &reg71);
	if (err != 0) {
		printk("ip5306 read 0x71 failed: %d\n", err);
		goto reschedule;
	}

	err = battery_read_mv(&battery_mv);
	if (err != 0) {
		printk("battery adc read failed: %d\n", err);
		goto reschedule;
	}

	charge_state = ip5306_decode_state(reg70, reg71);
	battery_percent = battery_percent_from_mv(battery_mv);

	if (charge_state != last_charge_state || battery_percent != last_percent) {
		printk("battery status=%s adc_raw=%d adc_node=%dmV voltage=%dmV level=%d%% reg70=0x%02x reg71=0x%02x\n",
		       battery_charge_state_name(charge_state), last_bat_adc_raw,
		       last_bat_adc_node_mv, battery_mv, battery_percent,
		       reg70, reg71);
		last_charge_state = charge_state;
		last_percent = battery_percent;
	}

	now_ms = k_uptime_get();

	if (power_in_low_power) {
		if (!power_wake_sent && charge_state != BATTERY_NOT_CHARGING) {
			power_wake_sent = true;
			APP_EVENT_SUBMIT(new_wake_up_event());
		}
	} else {
		if ((uint32_t)(now_ms - last_keepalive_ms) >= POWER_KEEPALIVE_INTERVAL_MS) {
			keep_alive();
			err = ip5306_keepalive_pulse();
			if (err != 0) {
				printk("ip5306 keepalive pulse failed: %d\n", err);
			} else {
				last_keepalive_ms = (uint32_t)now_ms;
			}
		}
	}

reschedule:
	k_work_reschedule(&power_poll_work, K_MSEC(POWER_POLL_INTERVAL_MS));
}

int power_module_init(void)
{
	uint8_t sys_ctl2;
	int err;

	if (!i2c_is_ready_dt(&ip5306_i2c)) {
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&ip5306_key) || !gpio_is_ready_dt(&bat_adc_en)) {
		return -ENODEV;
	}

	if (!adc_is_ready_dt(&bat_adc)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&ip5306_key, GPIO_OUTPUT_HIGH);
	if (err != 0) {
		return err;
	}

	err = gpio_pin_configure_dt(&bat_adc_en, GPIO_OUTPUT_LOW);
	if (err != 0) {
		return err;
	}

	err = gpio_pin_set_dt(&bat_adc_en, !BAT_ADC_EN_ACTIVE_LEVEL);
	if (err != 0) {
		return err;
	}

	err = adc_channel_setup_dt(&bat_adc);
	if (err != 0) {
		return err;
	}

	err = ip5306_wait_ready(&sys_ctl2);
	if (err != 0) {
		return err;
	}

	err = ip5306_reg_update(IP5306_REG_SYS_CTL2,
				IP5306_SYS_CTL2_LIGHT_SHUTDOWN_MASK,
				IP5306_SYS_CTL2_LIGHT_SHUTDOWN_64S);
	if (err != 0) {
		return err;
	}

	k_work_init_delayable(&power_poll_work, power_poll_work_handler);
	k_work_schedule(&power_poll_work, K_NO_WAIT);

	printk("ip5306 ready: i2c=0x%02x key=P0.22(active low) bat_adc_en=P0.09 bat_adc=P0.31\n",
	       IP5306_I2C_ADDR_7BIT);
	printk("ip5306 sys_ctl2: old=0x%02x keepalive_pulse=%dms period=%dms poll=%dms bat_adc_en_active=%d\n",
	       sys_ctl2, IP5306_KEY_PULSE_MS, POWER_KEEPALIVE_INTERVAL_MS,
	       POWER_POLL_INTERVAL_MS, BAT_ADC_EN_ACTIVE_LEVEL);
	module_set_state(MODULE_STATE_READY);

	return 0;
}

void power_module_get_status(struct power_module_status *status)
{
	unsigned int key;

	if (status == NULL) {
		return;
	}

	key = irq_lock();
	status->valid = (last_percent >= 0) && (last_battery_mv >= 0) &&
			(last_charge_state >= BATTERY_NOT_CHARGING);
	status->charging = (last_charge_state == BATTERY_CHARGING);
	status->full = (last_charge_state == BATTERY_FULL);
	status->battery_percent = (last_percent >= 0) ? last_percent : 0;
	status->battery_mv = last_battery_mv;
	irq_unlock(key);
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	if (is_power_down_event(aeh)) {
		power_in_low_power = true;
		power_wake_sent = false;
		module_set_state(MODULE_STATE_STANDBY);
		return false;
	}

	if (is_wake_up_event(aeh)) {
		power_in_low_power = false;
		power_wake_sent = false;
		module_set_state(MODULE_STATE_READY);
		return false;
	}

	return false;
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, wake_up_event);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, power_down_event);
