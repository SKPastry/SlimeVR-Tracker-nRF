#include "globals.h"

#include <math.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>

#include "led.h"

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

static void led_thread(void);
K_THREAD_DEFINE(led_thread_id, 512, led_thread, NULL, NULL, NULL, 6, 0, 0);

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, led_en_gpios)
#define LED_EN_EXISTS true
static const struct gpio_dt_spec led_en = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, led_en_gpios);
#endif

#if CONFIG_LED_STRIP
#define LED_STRIP_EXISTS true
#include <zephyr/drivers/led_strip.h>
#define STRIP_NODE DT_ALIAS(led_strip)
static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
#define LED_STRIP_POWER_ON_DELAY_US 2000
#define LED_STRIP_POWER_OFF_DELAY_US 1000
#define LED_STRIP_MIN_UPDATE_INTERVAL_MS 20
#endif

#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, led_gpios)
#define LED_EXISTS true
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, led_gpios);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led0))
#ifndef LED_EXISTS
#define LED_EXISTS true
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#else
#define LED0_EXISTS true
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif
#endif
#ifndef LED_EXISTS
#ifndef LED_STRIP_EXISTS
#warning "LED GPIO does not exist"
// static const struct gpio_dt_spec led = {0};
#endif
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led1))
#define LED1_EXISTS true
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led2))
#define LED2_EXISTS true
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
#endif
#if DT_NODE_EXISTS(DT_ALIAS(led3))
#define LED3_EXISTS true
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
#endif

#if DT_NODE_EXISTS(DT_ALIAS(pwm_led0))
#define PWM_LED_EXISTS true
static const struct pwm_dt_spec pwm_led = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
#else
#ifndef LED_STRIP_EXISTS
#warning "PWM LED node does not exist"
#endif
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pwm_led1))
#define PWM_LED1_EXISTS true
static const struct pwm_dt_spec pwm_led1 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led1));
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pwm_led2))
#define PWM_LED2_EXISTS true
static const struct pwm_dt_spec pwm_led2 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led2));
#endif

static enum sys_led_pattern current_led_pattern;
static int current_priority;

#if LED_EXISTS || LED_STRIP_EXISTS
static enum sys_led_pattern led_patterns[SYS_LED_PATTERN_DEPTH]
	= {[0 ...(SYS_LED_PATTERN_DEPTH - 1)] = SYS_LED_PATTERN_OFF};
static int led_pattern_state;
#ifdef LED_EN_EXISTS
static bool led_powered;
#else
static bool led_powered = true;
#endif

static const char *led_pattern_name(enum sys_led_pattern pattern)
{
	switch (pattern) {
	case SYS_LED_PATTERN_OFF_FORCE:
		return "OFF_FORCE";
	case SYS_LED_PATTERN_OFF:
		return "OFF";
	case SYS_LED_PATTERN_ON:
		return "ON";
	case SYS_LED_PATTERN_SHORT:
		return "SHORT";
	case SYS_LED_PATTERN_LONG:
		return "LONG";
	case SYS_LED_PATTERN_FLASH:
		return "FLASH";
	case SYS_LED_PATTERN_ONESHOT_POWERON:
		return "ONESHOT_POWERON";
	case SYS_LED_PATTERN_ONESHOT_POWEROFF:
		return "ONESHOT_POWEROFF";
	case SYS_LED_PATTERN_ONESHOT_PROGRESS:
		return "ONESHOT_PROGRESS";
	case SYS_LED_PATTERN_ONESHOT_COMPLETE:
		return "ONESHOT_COMPLETE";
	case SYS_LED_PATTERN_ONESHOT_PING:
		return "ONESHOT_PING";
	case SYS_LED_PATTERN_ON_PERSIST:
		return "ON_PERSIST";
	case SYS_LED_PATTERN_LONG_PERSIST:
		return "LONG_PERSIST";
	case SYS_LED_PATTERN_PULSE_PERSIST:
		return "PULSE_PERSIST";
	case SYS_LED_PATTERN_ACTIVE_PERSIST:
		return "ACTIVE_PERSIST";
	case SYS_LED_PATTERN_ERROR_A:
		return "ERROR_A";
	case SYS_LED_PATTERN_ERROR_B:
		return "ERROR_B";
	case SYS_LED_PATTERN_ERROR_C:
		return "ERROR_C";
	case SYS_LED_PATTERN_ERROR_D:
		return "ERROR_D";
	default:
		return "UNKNOWN";
	}
}

static const char *led_priority_name(int priority)
{
	switch (priority) {
	case SYS_LED_PRIORITY_HIGHEST:
		return "BOOT/HIGHEST";
	case SYS_LED_PRIORITY_SENSOR:
		return "SENSOR";
	case SYS_LED_PRIORITY_CONNECTION:
		return "CONNECTION";
	case SYS_LED_PRIORITY_STATUS:
		return "STATUS";
	case SYS_LED_PRIORITY_SYSTEM:
		return "SYSTEM";
	default:
		return "NONE";
	}
}

static const char *led_caller_name(void)
{
#if defined(CONFIG_THREAD_NAME)
	const char *name = k_thread_name_get(k_current_get());

	return name ? name : "?";
#else
	return "?";
#endif
}

#ifdef LED_STRIP_EXISTS
static struct led_rgb last_strip_pixel;
static bool last_strip_pixel_valid;
static bool force_next_strip_update;
static int64_t last_strip_update_ms;
static int64_t last_strip_error_log_ms = -1000;

static bool led_strip_pixel_equal(const struct led_rgb *a, const struct led_rgb *b)
{
	return a->r == b->r && a->g == b->g && a->b == b->b;
}

static int led_strip_update_checked(const struct led_rgb *pixel, bool force)
{
	int64_t now = k_uptime_get();
	bool force_next = force_next_strip_update;
	bool black = pixel->r == 0 && pixel->g == 0 && pixel->b == 0;
	bool log_update = force || force_next || black;
	int64_t elapsed_ms = last_strip_pixel_valid ? now - last_strip_update_ms : -1;
	bool force_update = force || force_next_strip_update
		|| black;
	int ret;

	if (!force_update && last_strip_pixel_valid) {
		if (led_strip_pixel_equal(pixel, &last_strip_pixel)) {
			return 0;
		}
		if (now - last_strip_update_ms < LED_STRIP_MIN_UPDATE_INTERVAL_MS) {
			return 0;
		}
	}

	if (log_update) {
		LOG_DBG(
			"LED strip update tx: rgb=%u,%u,%u force=%d force_next=%d black=%d last_valid=%d dt=%lld",
			pixel->r,
			pixel->g,
			pixel->b,
			force,
			force_next,
			black,
			last_strip_pixel_valid,
			(long long)elapsed_ms
		);
	}

	ret = led_strip_update_rgb(strip, (struct led_rgb *)pixel, 1);
	if (ret < 0) {
		if (log_update) {
			LOG_ERR("LED strip update tx failed: rgb=%u,%u,%u ret=%d",
				pixel->r, pixel->g, pixel->b, ret);
		}
		if (now - last_strip_error_log_ms >= 1000) {
			last_strip_error_log_ms = now;
			LOG_ERR("LED strip update failed: %d", ret);
		}
		return ret;
	}

	if (log_update) {
		LOG_DBG("LED strip update tx ok: rgb=%u,%u,%u ret=%d",
			pixel->r, pixel->g, pixel->b, ret);
	}

	last_strip_pixel = *pixel;
	last_strip_pixel_valid = true;
	last_strip_update_ms = now;
	force_next_strip_update = false;

	return 0;
}

static void led_strip_force_off(void)
{
	static const struct led_rgb off_pixel[1] = {{0, 0, 0}};

	if (!led_powered) {
		LOG_INF("LED strip force off skipped: power already disabled");
		return;
	}

	LOG_INF("LED strip force off: writing black");
	(void)led_strip_update_checked(off_pixel, true);
	k_usleep(LED_STRIP_POWER_OFF_DELAY_US);
	last_strip_pixel_valid = false;
}
#endif

static int led_pin_init(void)
{
	LOG_DBG("led_pin_init");
#if LED_EXISTS
	gpio_pin_configure_dt(&led, GPIO_OUTPUT);
	gpio_pin_set_dt(&led, 0);
#endif
#if LED0_EXISTS
	gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
	gpio_pin_set_dt(&led0, 0);
#endif
#if LED1_EXISTS
	gpio_pin_configure_dt(&led1, GPIO_OUTPUT);
	gpio_pin_set_dt(&led1, 0);
#endif
#if LED2_EXISTS
	gpio_pin_configure_dt(&led2, GPIO_OUTPUT);
	gpio_pin_set_dt(&led2, 0);
#endif
#if LED3_EXISTS
	gpio_pin_configure_dt(&led3, GPIO_OUTPUT);
	gpio_pin_set_dt(&led3, 0);
#endif
	return 0;
}

SYS_INIT(led_pin_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static void led_pin_reset(void)
{
	LOG_DBG("led_pin_reset");
#if LED_EXISTS
	gpio_pin_configure_dt(&led, GPIO_DISCONNECTED);
#endif
#if LED0_EXISTS
	gpio_pin_configure_dt(&led0, GPIO_DISCONNECTED);
#endif
#if LED1_EXISTS
	gpio_pin_configure_dt(&led1, GPIO_DISCONNECTED);
#endif
#if LED2_EXISTS
	gpio_pin_configure_dt(&led2, GPIO_DISCONNECTED);
#endif
#if LED3_EXISTS
	gpio_pin_configure_dt(&led3, GPIO_DISCONNECTED);
#endif
}

static void led_suspend(void)
{
	LOG_INF("LED suspend: effective=%s priority=%s",
		led_pattern_name(current_led_pattern), led_priority_name(current_priority));
#ifdef LED_STRIP_EXISTS
	led_strip_force_off();
	pm_device_action_run(strip, PM_DEVICE_ACTION_SUSPEND);
#endif
#ifdef PWM_LED_EXISTS
	pm_device_action_run(pwm_led.dev, PM_DEVICE_ACTION_SUSPEND);
#endif
#ifdef PWM_LED1_EXISTS
	pm_device_action_run(pwm_led1.dev, PM_DEVICE_ACTION_SUSPEND);
#endif
#ifdef PWM_LED2_EXISTS
	pm_device_action_run(pwm_led2.dev, PM_DEVICE_ACTION_SUSPEND);
#endif
	led_pin_reset();
	// disable power
#ifdef LED_EN_EXISTS
	gpio_pin_configure_dt(&led_en, GPIO_OUTPUT);
	gpio_pin_set_dt(&led_en, 0);
	led_powered = false;
	LOG_INF("LED power disabled");
#endif
}

static void led_resume(void)
{
	LOG_INF("LED resume: effective=%s priority=%s",
		led_pattern_name(current_led_pattern), led_priority_name(current_priority));
	// enable power
#ifdef LED_EN_EXISTS
	bool was_powered = led_powered;

	gpio_pin_configure_dt(&led_en, GPIO_OUTPUT);
	gpio_pin_set_dt(&led_en, 1);
	led_powered = true;
	LOG_INF("LED power enabled: was_powered=%d", was_powered);
#ifdef LED_STRIP_EXISTS
	if (!was_powered) {
		k_usleep(LED_STRIP_POWER_ON_DELAY_US);
		last_strip_pixel_valid = false;
		force_next_strip_update = true;
	}
#endif
#endif
#ifdef LED_STRIP_EXISTS
	pm_device_action_run(strip, PM_DEVICE_ACTION_RESUME);
#endif
#ifdef PWM_LED_EXISTS
	pm_device_action_run(pwm_led.dev, PM_DEVICE_ACTION_RESUME);
#endif
#ifdef PWM_LED1_EXISTS
	pm_device_action_run(pwm_led1.dev, PM_DEVICE_ACTION_RESUME);
#endif
#ifdef PWM_LED2_EXISTS
	pm_device_action_run(pwm_led2.dev, PM_DEVICE_ACTION_RESUME);
#endif
	led_pin_init();
}

#ifdef LED_STRIP_EXISTS
#define LED_RGB_COLOR
#else
#ifdef CONFIG_LED_RGB_COLOR
#define LED_RGB_COLOR
#define LED_RG_COLOR
#endif

#if PWM_LED_EXISTS && PWM_LED1_EXISTS && PWM_LED2_EXISTS
#define LED_TRI_COLOR
#else
#undef LED_RGB_COLOR
#undef LED_TRI_COLOR
#if PWM_LED_EXISTS && PWM_LED1_EXISTS
#define LED_DUAL_COLOR
#else
#undef LED_RG_COLOR
#undef LED_DUAL_COLOR
#endif
#endif
#endif

#ifdef LED_RGB_COLOR
static int led_pwm_period[5][3] = {
	{CONFIG_LED_DEFAULT_COLOR_R, CONFIG_LED_DEFAULT_COLOR_G, CONFIG_LED_DEFAULT_COLOR_B}, // Default
	{0, 10000, 0},                                                                        // Success
	{10000, 0, 0},                                                                        // Error
	{8000, 2000, 0},                                                                      // Charging
	{0, 0, 10000},                                                                        // Pairing
};
#elif defined(LED_TRI_COLOR)
static int led_pwm_period[5][3] = {
	{0, 0, 10000},   // Default
	{0, 10000, 0},   // Success
	{10000, 0, 0},   // Error
	{6000, 4000, 0}, // Charging
	{0, 0, 10000},   // Pairing
};
#elif defined(LED_RG_COLOR)
static int led_pwm_period[5][2] = {
	{CONFIG_LED_DEFAULT_COLOR_R, CONFIG_LED_DEFAULT_COLOR_G}, // Default
	{0, 10000},                                               // Success
	{10000, 0},                                               // Error
	{8000, 2000},                                             // Charging
	{4000, 6000},                                             // Pairing
};
#elif defined(LED_DUAL_COLOR)
static int led_pwm_period[5][2] = {
	{0, 10000},   // Default
	{0, 10000},   // Success
	{10000, 0},   // Error
	{6000, 4000}, // Charging
	{0, 10000},   // Pairing
};
#else
static int led_pwm_period[5][1] = {
	{10000}, // Default
	{10000}, // Success
	{10000}, // Error
	{10000}, // Charging
	{10000}, // Pairing
};
#endif

// Using brightness and value if PWM is supported, otherwise value is coerced to on/off
// TODO: use computed constants for high/low brightness and color values
static void led_pin_set(enum sys_led_color color, int brightness_pptt, int value_pptt)
{
	LOG_DBG("led_pin_set: color %d, brightness %d, global %d, value %d",
		color, brightness_pptt, CONFIG_LED_GLOBAL_BRIGHTNESS_PPTT, value_pptt);
	if (brightness_pptt < 0) {
		brightness_pptt = 0;
	} else if (brightness_pptt > 10000) {
		brightness_pptt = 10000;
	}
	if (value_pptt < 0) {
		value_pptt = 0;
	} else if (value_pptt > 10000) {
		value_pptt = 10000;
	}
#if LED_STRIP_EXISTS
	static struct led_rgb pixel[1];
	value_pptt = value_pptt * brightness_pptt / 10000;
	value_pptt = value_pptt * CONFIG_LED_GLOBAL_BRIGHTNESS_PPTT / 10000;
	pixel[0].r = 255 * (led_pwm_period[color][0] * value_pptt / 10000) / 10000;
	pixel[0].g = 255 * (led_pwm_period[color][1] * value_pptt / 10000) / 10000;
	pixel[0].b = 255 * (led_pwm_period[color][2] * value_pptt / 10000) / 10000;
	(void)led_strip_update_checked(pixel, false);
#elif PWM_LED_EXISTS
	value_pptt = value_pptt * brightness_pptt / 10000;
	value_pptt = value_pptt * CONFIG_LED_GLOBAL_BRIGHTNESS_PPTT / 10000;
	// only supporting color if PWM is supported
	pwm_set_pulse_dt(&pwm_led, pwm_led.period / 10000 * (led_pwm_period[color][0] * value_pptt / 10000));
#if PWM_LED1_EXISTS
	pwm_set_pulse_dt(&pwm_led1, pwm_led1.period / 10000 * (led_pwm_period[color][1] * value_pptt / 10000));
#if PWM_LED2_EXISTS
	pwm_set_pulse_dt(&pwm_led2, pwm_led2.period / 10000 * (led_pwm_period[color][2] * value_pptt / 10000));
#endif
#endif
#else
	gpio_pin_set_dt(&led, value_pptt > 5000);
#endif
}
#endif

void set_led(enum sys_led_pattern led_pattern, int priority)
{
	LOG_DBG("set_led: current_led_pattern %d, current_priority %d", current_led_pattern, current_priority);
	LOG_DBG("set_led: pattern %d, priority %d", led_pattern, priority);
#if LED_EXISTS || LED_STRIP_EXISTS
	enum sys_led_pattern requested_pattern = led_pattern;
	int requested_priority = priority;
	enum sys_led_pattern previous_effective = current_led_pattern;
	int previous_effective_priority = current_priority;
	int target_priority = priority;
	bool effective_found = false;
	enum sys_led_pattern previous_slot;

	if (led_pattern <= SYS_LED_PATTERN_OFF && k_current_get() == led_thread_id) {
		target_priority = current_priority;
	}

	previous_slot = led_patterns[target_priority];
	if (led_pattern <= SYS_LED_PATTERN_OFF && k_current_get() == led_thread_id) {
		led_patterns[current_priority] = led_pattern;
	} else {
		led_patterns[priority] = led_pattern;
	}
	if (previous_slot != led_patterns[target_priority]) {
		LOG_INF("LED request: slot=%s %s->%s request=%s/%s caller=%s",
			led_priority_name(target_priority),
			led_pattern_name(previous_slot),
			led_pattern_name(led_patterns[target_priority]),
			led_pattern_name(requested_pattern),
			led_priority_name(requested_priority),
			led_caller_name());
	}
	for (priority = 0; priority < SYS_LED_PATTERN_DEPTH; priority++) {
		if (led_patterns[priority] == SYS_LED_PATTERN_OFF) {
			continue;
		}
		led_pattern = led_patterns[priority];
		effective_found = true;
		break;
	}
	if (!effective_found) {
		priority = SYS_LED_PATTERN_DEPTH;
	}
	if (led_pattern == current_led_pattern && led_pattern > SYS_LED_PATTERN_OFF) {
		if (previous_slot != led_patterns[target_priority]) {
			LOG_INF("LED effective unchanged: %s priority=%s",
				led_pattern_name(current_led_pattern), led_priority_name(current_priority));
		}
		return;
	}
	current_led_pattern = led_pattern;
	current_priority = priority;
	led_pattern_state = 0;
#ifdef LED_STRIP_EXISTS
	force_next_strip_update = true;
#endif
	LOG_INF("LED effective: %s/%s -> %s/%s",
		led_pattern_name(previous_effective),
		led_priority_name(previous_effective_priority),
		led_pattern_name(current_led_pattern),
		led_priority_name(current_priority));
	if (current_led_pattern <= SYS_LED_PATTERN_OFF) {
		led_suspend();
		k_thread_suspend(led_thread_id);
		LOG_DBG("set_led: suspended led_thread_id");
	} else if (k_current_get() != led_thread_id) // do not suspend if called from thread
	{
		k_thread_suspend(led_thread_id);
		LOG_DBG("set_led: suspended led_thread_id");
		led_resume();
		k_thread_resume(led_thread_id);
		k_wakeup(led_thread_id);
		LOG_DBG("set_led: resumed led_thread_id");
	} else {
		led_resume();
		k_thread_resume(led_thread_id);
		k_wakeup(led_thread_id);
		LOG_DBG("set_led: resumed led_thread_id");
	}
#endif
}

static void led_thread(void)
{
#if !LED_EXISTS && !LED_STRIP_EXISTS
	LOG_WRN("LED GPIO does not exist");
	return;
#else
	while (1) {
		enum sys_led_pattern handled_pattern = current_led_pattern;

		LOG_DBG("led_thread: current_led_pattern %d", handled_pattern);
		switch (handled_pattern) {
		case SYS_LED_PATTERN_ON:
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, 10000);
			if (current_led_pattern == handled_pattern) {
				k_thread_suspend(led_thread_id);
			}
			break;
		case SYS_LED_PATTERN_SHORT:
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_PAIRING, 10000, led_pattern_state * 10000);
			k_msleep(led_pattern_state == 1 ? 100 : 900);
			break;
		case SYS_LED_PATTERN_LONG:
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, led_pattern_state * 10000);
			k_msleep(500);
			break;
		case SYS_LED_PATTERN_FLASH:
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, led_pattern_state * 10000);
			k_msleep(200);
			break;

		case SYS_LED_PATTERN_ONESHOT_POWERON:
			led_pattern_state++;
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, !(led_pattern_state % 2) * 10000);
			if (led_pattern_state == 7) {
				set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_HIGHEST);
			} else {
				k_msleep(200);
			}
			break;
		case SYS_LED_PATTERN_ONESHOT_POWEROFF:
			if (led_pattern_state++ > 0) {
				led_pin_set(
					SYS_LED_COLOR_DEFAULT,
					(202 - led_pattern_state) * 50,
					(led_pattern_state != 202 ? 10000 : 0)
				);
			} else {
				led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, 0);
			}
			if (led_pattern_state == 202) {
				set_led(SYS_LED_PATTERN_OFF_FORCE, SYS_LED_PRIORITY_HIGHEST);
			} else if (led_pattern_state == 1) {
				k_msleep(250);
			} else {
				k_msleep(5);
			}
			break;
		case SYS_LED_PATTERN_ONESHOT_PROGRESS:
			led_pattern_state++;
			led_pin_set(SYS_LED_COLOR_SUCCESS, 10000, !(led_pattern_state % 2) * 10000);
			if (led_pattern_state == 5) {
				set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_HIGHEST);
			} else {
				k_msleep(200);
			}
			break;
		case SYS_LED_PATTERN_ONESHOT_COMPLETE:
			led_pattern_state++;
			led_pin_set(SYS_LED_COLOR_SUCCESS, 10000, !(led_pattern_state % 2) * 10000);
			if (led_pattern_state == 9) {
				set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_HIGHEST);
			} else {
				k_msleep(200);
			}
			break;
		case SYS_LED_PATTERN_ONESHOT_PING:
			led_pattern_state++;
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, (led_pattern_state % 2) * 10000);
			if (led_pattern_state == 20) { // 10 flashes (states 1-20), turn off at 20
				set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_HIGHEST);
			} else {
				k_msleep(200);
			}
			break;

		case SYS_LED_PATTERN_ON_PERSIST:
			led_pin_set(SYS_LED_COLOR_SUCCESS, 2000, 10000);
			if (current_led_pattern == handled_pattern) {
				k_thread_suspend(led_thread_id);
			}
			break;
		case SYS_LED_PATTERN_LONG_PERSIST:
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_CHARGING, 2000, led_pattern_state * 10000);
			k_msleep(500);
			break;
		case SYS_LED_PATTERN_PULSE_PERSIST:
			led_pattern_state = (led_pattern_state + 1) % 1000;
			//			float led_value = sinf(led_pattern_state * (M_PI / 1000));
			//			led_pin_set(SYS_LED_COLOR_CHARGING, 10000, led_value * 10000);
			int led_value = led_pattern_state > 500 ? 1000 - led_pattern_state : led_pattern_state;
			if (led_value < 200) {
				led_value = (led_value) * 30;
			} else if (led_value < 300) {
				led_value = (led_value - 200) * 20 + 6000;
			} else if (led_value < 400) {
				led_value = (led_value - 300) * 15 + 8000;
			} else {
				led_value = (led_value - 400) * 5 + 9500;
			}
			led_pin_set(SYS_LED_COLOR_CHARGING, 10000, led_value);
			k_msleep(5);
			break;
		case SYS_LED_PATTERN_ACTIVE_PERSIST: // off duration first because the device may turn on multiple times rapidly
											 // and waste battery power
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_DEFAULT, 10000, !led_pattern_state * 10000);
			k_msleep(led_pattern_state ? 9700 : 300);
			break;

		case SYS_LED_PATTERN_ERROR_A: // TODO: should this use 20% duty cycle?
			led_pattern_state = (led_pattern_state + 1) % 10;
			led_pin_set(SYS_LED_COLOR_ERROR, 10000, (led_pattern_state < 4 && led_pattern_state % 2) * 10000);
			k_msleep(500);
			break;
		case SYS_LED_PATTERN_ERROR_B:
			led_pattern_state = (led_pattern_state + 1) % 10;
			led_pin_set(SYS_LED_COLOR_ERROR, 10000, (led_pattern_state < 6 && led_pattern_state % 2) * 10000);
			k_msleep(500);
			break;
		case SYS_LED_PATTERN_ERROR_C:
			led_pattern_state = (led_pattern_state + 1) % 10;
			led_pin_set(SYS_LED_COLOR_ERROR, 10000, (led_pattern_state < 8 && led_pattern_state % 2) * 10000);
			k_msleep(500);
			break;
		case SYS_LED_PATTERN_ERROR_D:
			led_pattern_state = (led_pattern_state + 1) % 2;
			led_pin_set(SYS_LED_COLOR_ERROR, 10000, led_pattern_state * 10000);
			k_msleep(500);
			break;

		default:
			LOG_DBG("led_thread: suspending led_thread_id");
			k_thread_suspend(led_thread_id);
		}
	}
#endif
}
