#include "globals.h"

#include "heater.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(heater, LOG_LEVEL_INF);

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)
#define HEATER_PWM_NODE DT_ALIAS(imu_heater)

#ifndef CONFIG_SYSTEM_IMU_HEATER_MAX_DUTY_PPTT
#define CONFIG_SYSTEM_IMU_HEATER_MAX_DUTY_PPTT 0
#endif

#ifndef CONFIG_SYSTEM_IMU_HEATER_PERIOD_MS
#define CONFIG_SYSTEM_IMU_HEATER_PERIOD_MS 20
#endif

#if IS_ENABLED(CONFIG_SYSTEM_IMU_HEATER) && DT_NODE_HAS_PROP(HEATER_PWM_NODE, pwms)
#define HEATER_PWM_EXISTS 1
static const struct pwm_dt_spec heater_pwm = PWM_DT_SPEC_GET(HEATER_PWM_NODE);
#else
#define HEATER_PWM_EXISTS 0
#endif

#if IS_ENABLED(CONFIG_SYSTEM_IMU_HEATER) && DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, pwr_gpios)
#define HEATER_PWR_GPIO_EXISTS 1
#define HEATER_PWR_GPIO_PIN DT_GPIO_PIN(ZEPHYR_USER_NODE, pwr_gpios)
#define HEATER_PWR_GPIO_PORT_NUM DT_PROP(DT_GPIO_CTLR(ZEPHYR_USER_NODE, pwr_gpios), port)
#define HEATER_PWR_GPIO NRF_GPIO_PIN_MAP(HEATER_PWR_GPIO_PORT_NUM, HEATER_PWR_GPIO_PIN)
#define HEATER_PWR_GPIO_ACTIVE_LOW ((DT_GPIO_FLAGS(ZEPHYR_USER_NODE, pwr_gpios) & GPIO_ACTIVE_LOW) != 0)
#else
#define HEATER_PWR_GPIO_EXISTS 0
#endif

static uint16_t heater_duty_pptt;
static int heater_last_error;
K_MUTEX_DEFINE(heater_lock);

#define HEATER_CONTROL_SAFETY_INHIBITED BIT(0)
#define HEATER_CONTROL_SUSPENDED        BIT(1)

/*
 * The heater driver is also called from power-management paths.  Set the
 * interlock before waiting for heater_lock so an in-flight non-zero write can
 * finish only before the subsequent checked zero write, never after it.
 */
static atomic_t heater_control_state =
	ATOMIC_INIT(HEATER_CONTROL_SAFETY_INHIBITED);

static bool heater_pwm_ready(void)
{
#if HEATER_PWM_EXISTS
	return device_is_ready(heater_pwm.dev);
#else
	return false;
#endif
}

bool heater_is_available(void)
{
	return IS_ENABLED(CONFIG_SYSTEM_IMU_HEATER) && HEATER_PWM_EXISTS && heater_pwm_ready();
}

bool heater_imu_powered(void)
{
#if HEATER_PWR_GPIO_EXISTS
	bool raw = nrf_gpio_pin_out_read(HEATER_PWR_GPIO) != 0;
	return HEATER_PWR_GPIO_ACTIVE_LOW ? !raw : raw;
#else
	return true;
#endif
}

int heater_set_duty_pptt(uint16_t duty_pptt)
{
#if HEATER_PWM_EXISTS
	k_mutex_lock(&heater_lock, K_FOREVER);
	if (duty_pptt > 0 && atomic_get(&heater_control_state) != 0) {
		heater_last_error = -EACCES;
		k_mutex_unlock(&heater_lock);
		return heater_last_error;
	}

	if (!device_is_ready(heater_pwm.dev)) {
		heater_last_error = -ENODEV;
		k_mutex_unlock(&heater_lock);
		return heater_last_error;
	}

	if (!heater_imu_powered()) {
		int off_err = pwm_set_pulse_dt(&heater_pwm, 0);
		if (off_err == 0) {
			heater_duty_pptt = 0;
		}
		heater_last_error = off_err != 0 ? off_err : -EIO;
		k_mutex_unlock(&heater_lock);
		return heater_last_error;
	}

	if (duty_pptt > CONFIG_SYSTEM_IMU_HEATER_MAX_DUTY_PPTT) {
		duty_pptt = CONFIG_SYSTEM_IMU_HEATER_MAX_DUTY_PPTT;
	}

	uint32_t period = PWM_MSEC(CONFIG_SYSTEM_IMU_HEATER_PERIOD_MS);
	uint32_t pulse = (uint32_t)(((uint64_t)period * duty_pptt) / 10000U);
	int err = pwm_set(heater_pwm.dev, heater_pwm.channel, period, pulse, heater_pwm.flags);
	if (err) {
		heater_last_error = err;
		k_mutex_unlock(&heater_lock);
		return err;
	}

	heater_duty_pptt = duty_pptt;
	heater_last_error = 0;
	k_mutex_unlock(&heater_lock);
	return 0;
#else
	ARG_UNUSED(duty_pptt);
	heater_last_error = -ENOTSUP;
	return heater_last_error;
#endif
}

static int heater_write_zero_locked(void)
{
#if HEATER_PWM_EXISTS
	if (!device_is_ready(heater_pwm.dev)) {
		heater_last_error = -ENODEV;
		return heater_last_error;
	}
	int err = pwm_set_pulse_dt(&heater_pwm, 0);
	if (err) {
		heater_last_error = err;
		return err;
	}
	heater_duty_pptt = 0;
	heater_last_error = 0;
	return 0;
#else
	heater_last_error = -ENOTSUP;
	return heater_last_error;
#endif
}

int heater_force_off_checked(void)
{
	atomic_or(&heater_control_state, HEATER_CONTROL_SAFETY_INHIBITED);
	k_mutex_lock(&heater_lock, K_FOREVER);
	int err = heater_write_zero_locked();
	k_mutex_unlock(&heater_lock);
	return err;
}

void heater_force_off(void)
{
	(void)heater_force_off_checked();
}

int heater_suspend_control(void)
{
	atomic_or(
		&heater_control_state,
		HEATER_CONTROL_SAFETY_INHIBITED | HEATER_CONTROL_SUSPENDED);
	return heater_force_off_checked();
}

void heater_resume_control(void)
{
	atomic_and(&heater_control_state, ~HEATER_CONTROL_SUSPENDED);
}

int heater_prepare_control(void)
{
	k_mutex_lock(&heater_lock, K_FOREVER);

	if ((atomic_get(&heater_control_state) & HEATER_CONTROL_SUSPENDED) != 0) {
		heater_last_error = -EBUSY;
		k_mutex_unlock(&heater_lock);
		return heater_last_error;
	}

	/*
	 * Establish a known-zero baseline before clearing the safety latch.  CAS
	 * prevents a concurrent suspend request from being accidentally cleared.
	 */
	int err = heater_write_zero_locked();
	if (err == 0) {
		while (true) {
			atomic_val_t old_state = atomic_get(&heater_control_state);
			if ((old_state & HEATER_CONTROL_SUSPENDED) != 0) {
				err = -EBUSY;
				heater_last_error = err;
				break;
			}
			if (atomic_cas(
				    &heater_control_state,
				    old_state,
				    old_state & ~HEATER_CONTROL_SAFETY_INHIBITED)) {
				break;
			}
		}
	}

	k_mutex_unlock(&heater_lock);
	return err;
}

void heater_get_status(struct heater_status *status)
{
	if (!status) {
		return;
	}

	k_mutex_lock(&heater_lock, K_FOREVER);
	status->available = heater_is_available();
	status->pwm_ready = heater_pwm_ready();
	status->imu_powered = heater_imu_powered();
	status->enabled = heater_duty_pptt > 0;
	status->duty_pptt = heater_duty_pptt;
	status->max_duty_pptt = IS_ENABLED(CONFIG_SYSTEM_IMU_HEATER) ?
		CONFIG_SYSTEM_IMU_HEATER_MAX_DUTY_PPTT : 0;
	status->last_error = heater_last_error;
	k_mutex_unlock(&heater_lock);
}

static int heater_init(void)
{
	heater_force_off();
	return 0;
}

SYS_INIT(heater_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
