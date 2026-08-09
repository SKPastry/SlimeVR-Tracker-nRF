#include "globals.h"

#include <math.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>

#include "led.h"

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

static void led_thread(void);
K_THREAD_DEFINE(led_thread_id, 512, led_thread, NULL, NULL, NULL, LED_THREAD_PRIORITY, 0, 0);

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

static enum sys_led_pattern current_led_pattern = SYS_LED_PATTERN_OFF;
static int current_priority = SYS_LED_PATTERN_DEPTH;

#if LED_EXISTS || LED_STRIP_EXISTS
K_MUTEX_DEFINE(led_force_off_lock);
K_SEM_DEFINE(led_wake_sem, 1, 1);
K_SEM_DEFINE(led_applied_sem, 0, 1);

struct led_slot {
	enum sys_led_pattern pattern;
	enum sys_led_color color;
	bool has_color;
	struct sys_led_rgb_pptt rgb;
	bool has_rgb;
};

struct led_state_snapshot {
	enum sys_led_pattern pattern;
	enum sys_led_color color;
	bool has_color;
	struct sys_led_rgb_pptt rgb;
	bool has_rgb;
	int priority;
	uint32_t generation;
	uint32_t animation_epoch;
};

static struct led_slot led_slots[SYS_LED_PATTERN_DEPTH]
	= {[0 ...(SYS_LED_PATTERN_DEPTH - 1)] = {
		   .pattern = SYS_LED_PATTERN_OFF,
		   .color = SYS_LED_COLOR_DEFAULT,
		   .has_color = false,
		   .rgb = {0, 0, 0},
		   .has_rgb = false,
	   }};
static enum sys_led_color current_led_color;
static bool current_led_has_color;
static struct sys_led_rgb_pptt current_led_rgb;
static bool current_led_has_rgb;
static uint32_t led_generation = 1;
static uint32_t led_animation_epoch = 1;
static uint32_t led_applied_generation;
static enum sys_led_pattern led_applied_pattern = SYS_LED_PATTERN_OFF;
static bool led_shutdown_latched;
#ifdef LED_EN_EXISTS
static bool led_powered;
#elif defined(LED_STRIP_EXISTS)
static bool led_powered = true;
#endif

static bool led_rgb_equal(const struct sys_led_rgb_pptt *a, const struct sys_led_rgb_pptt *b)
{
	return a->r == b->r && a->g == b->g && a->b == b->b;
}

static struct sys_led_rgb_pptt led_rgb_clamp(struct sys_led_rgb_pptt color)
{
	if (color.r > 10000) {
		color.r = 10000;
	}
	if (color.g > 10000) {
		color.g = 10000;
	}
	if (color.b > 10000) {
		color.b = 10000;
	}

	return color;
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
	bool black = pixel->r == 0 && pixel->g == 0 && pixel->b == 0;
	bool force_update = force || force_next_strip_update || black;
	int ret;

	if (!force_update && last_strip_pixel_valid) {
		if (led_strip_pixel_equal(pixel, &last_strip_pixel)) {
			return 0;
		}
		if (now - last_strip_update_ms < LED_STRIP_MIN_UPDATE_INTERVAL_MS) {
			return 0;
		}
	}

	ret = led_strip_update_rgb(strip, (struct led_rgb *)pixel, 1);
	if (ret < 0) {
		if (now - last_strip_error_log_ms >= 1000) {
			last_strip_error_log_ms = now;
			LOG_ERR("LED strip update failed: rgb=%u,%u,%u ret=%d", pixel->r, pixel->g, pixel->b, ret);
		}
		return ret;
	}

	last_strip_pixel = *pixel;
	last_strip_pixel_valid = true;
	last_strip_update_ms = now;
	force_next_strip_update = false;

	return 0;
}

#ifndef LED_EN_EXISTS
static void led_strip_force_off(void)
{
	static const struct led_rgb off_pixel[1] = {{0, 0, 0}};

	if (!led_powered) {
		return;
	}

	(void)led_strip_update_checked(off_pixel, true);
	k_usleep(LED_STRIP_POWER_OFF_DELAY_US);
	last_strip_pixel_valid = false;
}
#endif
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

static void led_hw_suspend(void)
{
#ifdef LED_EN_EXISTS
	/* Power-gated strips must go dark even if their transport is wedged. */
	int ret = gpio_pin_configure_dt(&led_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to disable LED power: %d", ret);
	}
	led_powered = false;
#endif
#ifdef LED_STRIP_EXISTS
#ifndef LED_EN_EXISTS
	led_strip_force_off();
#endif
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
}

static bool led_hw_resume(void)
{
	// enable power
#ifdef LED_EN_EXISTS
	unsigned int key = irq_lock();
	if (led_shutdown_latched) {
		irq_unlock(key);
		return false;
	}
	bool was_powered = led_powered;
	int ret = gpio_pin_configure_dt(&led_en, GPIO_OUTPUT_ACTIVE);
	led_powered = true;
	irq_unlock(key);
	if (ret < 0) {
		LOG_ERR("Failed to enable LED power: %d", ret);
	}
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
	return true;
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
static int led_pwm_period[SYS_LED_COLOR_COUNT][3] = {
	{CONFIG_LED_DEFAULT_COLOR_R, CONFIG_LED_DEFAULT_COLOR_G, CONFIG_LED_DEFAULT_COLOR_B}, // Default
	{0, 10000, 0},                                                                        // Success
	{10000, 0, 0},                                                                        // Error
	{8000, 2000, 0},                                                                      // Charging
	{0, 0, 10000},                                                                        // Pairing
	{0, 8000, 10000},                                                                     // Calibration
	{0, 10000, 4500},                                                                     // Calibration stable
	{4500, 0, 10000},                                                                     // Debug
};
#elif defined(LED_TRI_COLOR)
static int led_pwm_period[SYS_LED_COLOR_COUNT][3] = {
	{0, 0, 10000},   // Default
	{0, 10000, 0},   // Success
	{10000, 0, 0},   // Error
	{6000, 4000, 0}, // Charging
	{0, 0, 10000},   // Pairing
	{0, 4000, 10000}, // Calibration
	{0, 10000, 4500}, // Calibration stable
	{4500, 0, 10000}, // Debug
};
#elif defined(LED_RG_COLOR)
static int led_pwm_period[SYS_LED_COLOR_COUNT][2] = {
	{CONFIG_LED_DEFAULT_COLOR_R, CONFIG_LED_DEFAULT_COLOR_G}, // Default
	{0, 10000},                                               // Success
	{10000, 0},                                               // Error
	{8000, 2000},                                             // Charging
	{4000, 6000},                                             // Pairing
	{0, 10000},                                               // Calibration
	{0, 10000},                                               // Calibration stable
	{10000, 0},                                               // Debug
};
#elif defined(LED_DUAL_COLOR)
static int led_pwm_period[SYS_LED_COLOR_COUNT][2] = {
	{0, 10000},   // Default
	{0, 10000},   // Success
	{10000, 0},   // Error
	{6000, 4000}, // Charging
	{0, 10000},   // Pairing
	{0, 10000},   // Calibration
	{0, 10000},   // Calibration stable
	{10000, 0},   // Debug
};
#else
static int led_pwm_period[SYS_LED_COLOR_COUNT][1] = {
	{10000}, // Default
	{10000}, // Success
	{10000}, // Error
	{10000}, // Charging
	{10000}, // Pairing
	{10000}, // Calibration
	{10000}, // Calibration stable
	{10000}, // Debug
};
#endif

static struct sys_led_rgb_pptt led_named_color_rgb(enum sys_led_color color)
{
#if defined(LED_RGB_COLOR) || defined(LED_TRI_COLOR)
	return (struct sys_led_rgb_pptt){
		.r = led_pwm_period[color][0],
		.g = led_pwm_period[color][1],
		.b = led_pwm_period[color][2],
	};
#elif defined(LED_RG_COLOR) || defined(LED_DUAL_COLOR)
	return (struct sys_led_rgb_pptt){
		.r = led_pwm_period[color][0],
		.g = led_pwm_period[color][1],
		.b = 0,
	};
#else
	return (struct sys_led_rgb_pptt){
		.r = led_pwm_period[color][0],
		.g = led_pwm_period[color][0],
		.b = led_pwm_period[color][0],
	};
#endif
}

static bool led_pattern_is_same_effect(
	enum sys_led_pattern a_pattern,
	enum sys_led_color a_color,
	bool a_has_color,
	struct sys_led_rgb_pptt a_rgb,
	bool a_has_rgb,
	enum sys_led_pattern b_pattern,
	enum sys_led_color b_color,
	bool b_has_color,
	struct sys_led_rgb_pptt b_rgb,
	bool b_has_rgb
)
{
	return a_pattern == b_pattern &&
	       a_has_color == b_has_color &&
	       a_has_rgb == b_has_rgb &&
	       (!a_has_color || a_color == b_color) &&
	       (!a_has_rgb || led_rgb_equal(&a_rgb, &b_rgb));
}

static bool led_pattern_is_oneshot(enum sys_led_pattern pattern)
{
	return pattern >= SYS_LED_PATTERN_ONESHOT_POWERON &&
	       pattern <= SYS_LED_PATTERN_ONESHOT_ERROR;
}

/* Interrupts must be locked. */
static void led_select_effect_locked(struct led_state_snapshot *selected)
{
	*selected = (struct led_state_snapshot){
		.pattern = SYS_LED_PATTERN_OFF,
		.color = SYS_LED_COLOR_DEFAULT,
		.has_color = false,
		.rgb = {0, 0, 0},
		.has_rgb = false,
		.priority = SYS_LED_PATTERN_DEPTH,
	};

	for (int priority = 0; priority < SYS_LED_PATTERN_DEPTH; priority++) {
		if (led_slots[priority].pattern == SYS_LED_PATTERN_OFF) {
			continue;
		}

		selected->pattern = led_slots[priority].pattern;
		selected->color = led_slots[priority].color;
		selected->has_color = led_slots[priority].has_color;
		selected->rgb = led_slots[priority].rgb;
		selected->has_rgb = led_slots[priority].has_rgb;
		selected->priority = priority;
		break;
	}
}

/* Interrupts must be locked. */
static bool led_publish_effect_locked(const struct led_state_snapshot *selected)
{
	bool same_effect = led_pattern_is_same_effect(
		current_led_pattern,
		current_led_color,
		current_led_has_color,
		current_led_rgb,
		current_led_has_rgb,
		selected->pattern,
		selected->color,
		selected->has_color,
		selected->rgb,
		selected->has_rgb
	);
	bool owner_changed = current_priority != selected->priority;
	bool restart_animation = current_led_pattern != selected->pattern ||
		(owner_changed && led_pattern_is_oneshot(selected->pattern));

	if (same_effect && !restart_animation) {
		/* A persistent effect does not need an early extra frame merely because
		 * an identical request became the effective priority slot.
		 */
		current_priority = selected->priority;
		return false;
	}

	current_led_pattern = selected->pattern;
	current_priority = selected->priority;
	current_led_color = selected->color;
	current_led_has_color = selected->has_color;
	current_led_rgb = selected->rgb;
	current_led_has_rgb = selected->has_rgb;
	if (restart_animation) {
		led_animation_epoch++;
		if (led_animation_epoch == 0) {
			led_animation_epoch = 1;
		}
	}
	led_generation++;
	if (led_generation == 0) {
		led_generation = 1;
	}

	/* k_sem_give() is ISR-safe. Publishing it before irq_unlock() makes the
	 * state change and its wakeup indivisible with respect to thread aborts.
	 */
	k_sem_give(&led_wake_sem);
	return true;
}

/* Interrupts must be locked. */
static void led_update_slot_locked(
	int priority,
	enum sys_led_pattern led_pattern,
	enum sys_led_color color,
	bool has_color,
	struct sys_led_rgb_pptt rgb,
	bool has_rgb
)
{
	led_slots[priority].pattern = led_pattern;
	led_slots[priority].color = color;
	led_slots[priority].rgb = rgb;
	led_slots[priority].has_rgb = has_rgb && led_pattern > SYS_LED_PATTERN_OFF;
	led_slots[priority].has_color = has_color &&
		!led_slots[priority].has_rgb &&
		led_pattern > SYS_LED_PATTERN_OFF;
}

static uint32_t led_request(
	enum sys_led_pattern led_pattern,
	enum sys_led_color color,
	bool has_color,
	struct sys_led_rgb_pptt rgb,
	bool has_rgb,
	int priority
)
{
	if (priority < 0 || priority >= SYS_LED_PATTERN_DEPTH) {
		LOG_WRN("LED request ignored: invalid priority %d", priority);
		return 0;
	}

	rgb = led_rgb_clamp(rgb);
	unsigned int key = irq_lock();
	if (led_shutdown_latched) {
		uint32_t generation = led_generation;
		irq_unlock(key);
		return generation;
	}

	led_update_slot_locked(priority, led_pattern, color, has_color, rgb, has_rgb);

	struct led_state_snapshot selected;
	led_select_effect_locked(&selected);
	(void)led_publish_effect_locked(&selected);
	uint32_t generation = led_generation;
	irq_unlock(key);

	return generation;
}

static void led_snapshot_get(struct led_state_snapshot *snapshot)
{
	unsigned int key = irq_lock();
	/* Coalesce every token already represented by this latest snapshot. */
	(void)k_sem_take(&led_wake_sem, K_NO_WAIT);
	snapshot->pattern = current_led_pattern;
	snapshot->color = current_led_color;
	snapshot->has_color = current_led_has_color;
	snapshot->rgb = current_led_rgb;
	snapshot->has_rgb = current_led_has_rgb;
	snapshot->priority = current_priority;
	snapshot->generation = led_generation;
	snapshot->animation_epoch = led_animation_epoch;
	irq_unlock(key);
}

static void led_complete_oneshot(
	const struct led_state_snapshot *handled,
	enum sys_led_pattern finished_pattern
)
{
	unsigned int key = irq_lock();
	if (current_led_pattern == handled->pattern &&
	    current_priority == handled->priority &&
	    led_generation == handled->generation &&
	    handled->priority >= 0 && handled->priority < SYS_LED_PATTERN_DEPTH) {
		led_update_slot_locked(
			handled->priority,
			finished_pattern,
			SYS_LED_COLOR_DEFAULT,
			false,
			(struct sys_led_rgb_pptt){0, 0, 0},
			false
		);
		struct led_state_snapshot selected;
		led_select_effect_locked(&selected);
		(void)led_publish_effect_locked(&selected);
	}
	irq_unlock(key);
}

static void led_mark_applied(const struct led_state_snapshot *handled)
{
	unsigned int key = irq_lock();
	led_applied_generation = handled->generation;
	led_applied_pattern = handled->pattern;
	k_sem_give(&led_applied_sem);
	irq_unlock(key);
}

static uint32_t led_latch_force_off_request(bool *strip_power_cut)
{
	*strip_power_cut = false;
#if defined(LED_EN_EXISTS) && defined(LED_STRIP_EXISTS)
	int power_ret;
#endif
	unsigned int key = irq_lock();
	led_shutdown_latched = true;
	led_update_slot_locked(
		SYS_LED_PRIORITY_HIGHEST,
		SYS_LED_PATTERN_OFF_FORCE,
		SYS_LED_COLOR_DEFAULT,
		false,
		(struct sys_led_rgb_pptt){0, 0, 0},
		false
	);
	struct led_state_snapshot selected;
	led_select_effect_locked(&selected);
	(void)led_publish_effect_locked(&selected);
	uint32_t generation = led_generation;
	irq_unlock(key);
#if defined(LED_EN_EXISTS) && defined(LED_STRIP_EXISTS)
	/* P00 escape hatch: after the latch is visible, no stale worker snapshot
	 * can enable the strip. Cut its power without waiting for I2S/PM cleanup.
	 */
	power_ret = gpio_pin_configure_dt(&led_en, GPIO_OUTPUT_INACTIVE);
	led_powered = false;
	*strip_power_cut = power_ret >= 0;
	if (power_ret < 0) {
		LOG_ERR("Failed to cut LED strip power: %d", power_ret);
	}
#endif

	return generation;
}

// Using brightness and value if PWM is supported, otherwise value is coerced to on/off
// TODO: use computed constants for high/low brightness and color values
static void led_pin_set(
	const struct led_state_snapshot *state,
	enum sys_led_color default_color,
	int brightness_pptt,
	int value_pptt
)
{
	enum sys_led_color color = state->has_color ? state->color : default_color;
	struct sys_led_rgb_pptt rgb = state->has_rgb ? state->rgb : led_named_color_rgb(color);

	rgb = led_rgb_clamp(rgb);
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
	pixel[0].r = 255 * (rgb.r * value_pptt / 10000) / 10000;
	pixel[0].g = 255 * (rgb.g * value_pptt / 10000) / 10000;
	pixel[0].b = 255 * (rgb.b * value_pptt / 10000) / 10000;
	(void)led_strip_update_checked(pixel, false);
#elif PWM_LED_EXISTS
	value_pptt = value_pptt * brightness_pptt / 10000;
	value_pptt = value_pptt * CONFIG_LED_GLOBAL_BRIGHTNESS_PPTT / 10000;
	// only supporting color if PWM is supported
#if PWM_LED1_EXISTS
	pwm_set_pulse_dt(&pwm_led, pwm_led.period / 10000 * (rgb.r * value_pptt / 10000));
	pwm_set_pulse_dt(&pwm_led1, pwm_led1.period / 10000 * (rgb.g * value_pptt / 10000));
#if PWM_LED2_EXISTS
	pwm_set_pulse_dt(&pwm_led2, pwm_led2.period / 10000 * (rgb.b * value_pptt / 10000));
#endif
#else
	int mono_pptt = rgb.r;

	if (rgb.g > mono_pptt) {
		mono_pptt = rgb.g;
	}
	if (rgb.b > mono_pptt) {
		mono_pptt = rgb.b;
	}
	pwm_set_pulse_dt(&pwm_led, pwm_led.period / 10000 * (mono_pptt * value_pptt / 10000));
#endif
#else
	int mono_pptt = rgb.r;

	if (rgb.g > mono_pptt) {
		mono_pptt = rgb.g;
	}
	if (rgb.b > mono_pptt) {
		mono_pptt = rgb.b;
	}
	gpio_pin_set_dt(&led, mono_pptt * value_pptt / 10000 > 5000);
#endif
}

static int led_breathe_value_pptt(int pattern_state)
{
	int led_value = pattern_state > 500 ? 1000 - pattern_state : pattern_state;

	if (led_value < 200) {
		led_value = led_value * 30;
	} else if (led_value < 300) {
		led_value = (led_value - 200) * 20 + 6000;
	} else if (led_value < 400) {
		led_value = (led_value - 300) * 15 + 8000;
	} else {
		led_value = (led_value - 400) * 5 + 9500;
	}

	return led_value;
}
#endif

void set_led(enum sys_led_pattern led_pattern, int priority)
{
	LOG_DBG("set_led: pattern=%d priority=%d", led_pattern, priority);
#if LED_EXISTS || LED_STRIP_EXISTS
	(void)led_request(
		led_pattern,
		SYS_LED_COLOR_DEFAULT,
		false,
		(struct sys_led_rgb_pptt){0, 0, 0},
		false,
		priority
	);
#endif
}

void set_led_color(enum sys_led_pattern led_pattern, enum sys_led_color color, int priority)
{
	LOG_DBG("set_led_color: pattern=%d color=%d priority=%d", led_pattern, color, priority);
#if LED_EXISTS || LED_STRIP_EXISTS
	(void)led_request(
		led_pattern,
		color,
		true,
		(struct sys_led_rgb_pptt){0, 0, 0},
		false,
		priority
	);
#endif
}

void set_led_rgb(enum sys_led_pattern led_pattern, struct sys_led_rgb_pptt color, int priority)
{
	LOG_DBG(
		"set_led_rgb: pattern=%d rgb=%u,%u,%u priority=%d",
		led_pattern,
		(unsigned int)color.r,
		(unsigned int)color.g,
		(unsigned int)color.b,
		priority
	);
#if LED_EXISTS || LED_STRIP_EXISTS
	(void)led_request(led_pattern, SYS_LED_COLOR_DEFAULT, false, color, true, priority);
#endif
}

void led_force_off_sync(void)
{
#if LED_EXISTS || LED_STRIP_EXISTS
	k_mutex_lock(&led_force_off_lock, K_FOREVER);
	bool strip_power_cut;
	uint32_t target_generation = led_latch_force_off_request(&strip_power_cut);
	if (strip_power_cut) {
		k_mutex_unlock(&led_force_off_lock);
		return;
	}
	for (;;) {
		unsigned int key = irq_lock();
		bool applied = led_applied_generation == target_generation &&
			led_applied_pattern <= SYS_LED_PATTERN_OFF;
		bool still_forced = led_generation == target_generation &&
			current_led_pattern <= SYS_LED_PATTERN_OFF;
		irq_unlock(key);

		if (applied && still_forced) {
			k_mutex_unlock(&led_force_off_lock);
			return;
		}

		(void)k_sem_take(&led_applied_sem, K_FOREVER);
	}
#endif
}

static void led_thread(void)
{
#if !LED_EXISTS && !LED_STRIP_EXISTS
	LOG_WRN("LED GPIO does not exist");
	return;
#else
	int pattern_state = 0;
	uint32_t last_animation_epoch = 0;
	uint32_t last_applied_generation = 0;
	bool hw_state_known = false;
	bool hw_suspended = false;
	k_timeout_t wait = K_FOREVER;
#ifdef LED_STRIP_EXISTS
	uint32_t last_generation = 0;
#endif

	while (1) {
		(void)k_sem_take(&led_wake_sem, wait);

		struct led_state_snapshot handled;
		led_snapshot_get(&handled);

		if (handled.animation_epoch != last_animation_epoch) {
			pattern_state = 0;
		}
		last_animation_epoch = handled.animation_epoch;

#ifdef LED_STRIP_EXISTS
		if (handled.generation != last_generation) {
			force_next_strip_update = true;
			last_generation = handled.generation;
		}
#endif

		if (handled.pattern <= SYS_LED_PATTERN_OFF) {
			if (!hw_state_known || !hw_suspended) {
				led_hw_suspend();
				hw_state_known = true;
				hw_suspended = true;
			}
			if (handled.generation != last_applied_generation) {
				led_mark_applied(&handled);
				last_applied_generation = handled.generation;
			}
			wait = K_FOREVER;
			continue;
		}

		if (!hw_state_known || hw_suspended) {
			if (!led_hw_resume()) {
				hw_state_known = true;
				hw_suspended = true;
				wait = K_NO_WAIT;
				continue;
			}
			hw_state_known = true;
			hw_suspended = false;
		}

		bool oneshot_finished = false;
		enum sys_led_pattern finished_pattern = SYS_LED_PATTERN_OFF;

		switch (handled.pattern) {
		case SYS_LED_PATTERN_ON:
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, 10000);
			wait = K_FOREVER;
			break;
		case SYS_LED_PATTERN_SHORT:
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_PAIRING, 10000, pattern_state * 10000);
			wait = K_MSEC(pattern_state == 1 ? 100 : 900);
			break;
		case SYS_LED_PATTERN_LONG:
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, pattern_state * 10000);
			wait = K_MSEC(500);
			break;
		case SYS_LED_PATTERN_FLASH:
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, pattern_state * 10000);
			wait = K_MSEC(200);
			break;
		case SYS_LED_PATTERN_BREATH_SLOW:
			pattern_state = (pattern_state + 1) % 1000;
			led_pin_set(
				&handled,
				SYS_LED_COLOR_DEFAULT,
				10000,
				led_breathe_value_pptt(pattern_state)
			);
			wait = K_MSEC(5);
			break;
		case SYS_LED_PATTERN_BREATH_FAST:
			pattern_state = (pattern_state + 1) % 1000;
			led_pin_set(
				&handled,
				SYS_LED_COLOR_DEFAULT,
				10000,
				led_breathe_value_pptt(pattern_state)
			);
			wait = K_MSEC(2);
			break;

		case SYS_LED_PATTERN_ONESHOT_POWERON:
			pattern_state++;
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, !(pattern_state % 2) * 10000);
			oneshot_finished = pattern_state >= 7;
			wait = K_MSEC(200);
			break;
		case SYS_LED_PATTERN_ONESHOT_POWEROFF:
			if (pattern_state++ > 0) {
				led_pin_set(
					&handled,
					SYS_LED_COLOR_DEFAULT,
					(202 - pattern_state) * 50,
					(pattern_state != 202 ? 10000 : 0)
				);
			} else {
				led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, 0);
			}
			oneshot_finished = pattern_state >= 202;
			finished_pattern = SYS_LED_PATTERN_OFF_FORCE;
			wait = pattern_state == 1 ? K_MSEC(250) : K_MSEC(5);
			break;
		case SYS_LED_PATTERN_ONESHOT_PROGRESS:
			pattern_state++;
			led_pin_set(&handled, SYS_LED_COLOR_SUCCESS, 10000, !(pattern_state % 2) * 10000);
			oneshot_finished = pattern_state >= 5;
			wait = K_MSEC(200);
			break;
		case SYS_LED_PATTERN_ONESHOT_COMPLETE:
			pattern_state++;
			led_pin_set(&handled, SYS_LED_COLOR_SUCCESS, 10000, !(pattern_state % 2) * 10000);
			oneshot_finished = pattern_state >= 9;
			wait = K_MSEC(200);
			break;
		case SYS_LED_PATTERN_ONESHOT_PING:
			pattern_state++;
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, (pattern_state % 2) * 10000);
			oneshot_finished = pattern_state >= 20; // 10 flashes (states 1-20)
			wait = K_MSEC(200);
			break;
		case SYS_LED_PATTERN_ONESHOT_ERROR:
			pattern_state++;
			led_pin_set(&handled, SYS_LED_COLOR_ERROR, 10000, (pattern_state % 2) * 10000);
			oneshot_finished = pattern_state >= 6;
			wait = K_MSEC(150);
			break;

		case SYS_LED_PATTERN_ON_PERSIST:
			led_pin_set(&handled, SYS_LED_COLOR_SUCCESS, 2000, 10000);
			wait = K_FOREVER;
			break;
		case SYS_LED_PATTERN_LONG_PERSIST:
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_CHARGING, 2000, pattern_state * 10000);
			wait = K_MSEC(500);
			break;
		case SYS_LED_PATTERN_PULSE_PERSIST:
			pattern_state = (pattern_state + 1) % 1000;
			//			float led_value = sinf(pattern_state * (M_PI / 1000));
			//			led_pin_set(SYS_LED_COLOR_CHARGING, 10000, led_value * 10000);
			led_pin_set(
				&handled,
				SYS_LED_COLOR_CHARGING,
				10000,
				led_breathe_value_pptt(pattern_state)
			);
			wait = K_MSEC(5);
			break;
		case SYS_LED_PATTERN_ACTIVE_PERSIST: // off duration first because the device may turn on multiple times rapidly
										 // and waste battery power
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_DEFAULT, 10000, !pattern_state * 10000);
			wait = K_MSEC(pattern_state ? 9700 : 300);
			break;

		case SYS_LED_PATTERN_ERROR_A: // TODO: should this use 20% duty cycle?
			pattern_state = (pattern_state + 1) % 10;
			led_pin_set(
				&handled,
				SYS_LED_COLOR_ERROR,
				10000,
				(pattern_state < 4 && pattern_state % 2) * 10000
			);
			wait = K_MSEC(500);
			break;
		case SYS_LED_PATTERN_ERROR_B:
			pattern_state = (pattern_state + 1) % 10;
			led_pin_set(
				&handled,
				SYS_LED_COLOR_ERROR,
				10000,
				(pattern_state < 6 && pattern_state % 2) * 10000
			);
			wait = K_MSEC(500);
			break;
		case SYS_LED_PATTERN_ERROR_C:
			pattern_state = (pattern_state + 1) % 10;
			led_pin_set(
				&handled,
				SYS_LED_COLOR_ERROR,
				10000,
				(pattern_state < 8 && pattern_state % 2) * 10000
			);
			wait = K_MSEC(500);
			break;
		case SYS_LED_PATTERN_ERROR_D:
			pattern_state = (pattern_state + 1) % 2;
			led_pin_set(&handled, SYS_LED_COLOR_ERROR, 10000, pattern_state * 10000);
			wait = K_MSEC(500);
			break;

		default:
			LOG_WRN("Unsupported LED pattern %d", handled.pattern);
			led_hw_suspend();
			hw_state_known = true;
			hw_suspended = true;
			wait = K_FOREVER;
			break;
		}

		if (handled.generation != last_applied_generation) {
			led_mark_applied(&handled);
			last_applied_generation = handled.generation;
		}
		if (oneshot_finished) {
			led_complete_oneshot(&handled, finished_pattern);
			wait = K_NO_WAIT;
		}
	}
#endif
}
