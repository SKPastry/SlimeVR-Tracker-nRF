#include "globals.h"

#if defined(CONFIG_BOARD_SK_CHEESECAKE_NRF_P00) && defined(CONFIG_CRAZT_WS2812_STRIP_I2S)

#include "system/led.h"
#include "system/ws2812_stress.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define WS2812_STRESS_STACK_SIZE 1024
#define WS2812_STRESS_PRIORITY   (CONSOLE_THREAD_PRIORITY + 2)
#define WS2812_STRESS_SETTLE_MS  10
#define WS2812_STRESS_DRAIN_MS   5

#define STRIP_NODE DT_ALIAS(led_strip)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static const struct led_rgb stress_pixels[] = {
	{0x00, 0x00, 0x00},
	{0xff, 0x00, 0x00},
	{0x00, 0xff, 0x00},
	{0x00, 0x00, 0xff},
	{0x12, 0xa5, 0x5a},
};

static atomic_t stress_state;
static atomic_t target_frames;
static atomic_t attempted_frames;
static atomic_t successful_frames;
static atomic_t failed_frames;
static atomic_t start_ms;
static atomic_t elapsed_ms;
static atomic_t last_error;

K_SEM_DEFINE(stress_start_sem, 0, 1);

static void ws2812_stress_thread(void *, void *, void *);
K_THREAD_DEFINE(
	ws2812_stress_thread_id,
	WS2812_STRESS_STACK_SIZE,
	ws2812_stress_thread,
	NULL,
	NULL,
	NULL,
	WS2812_STRESS_PRIORITY,
	0,
	0
);

static void ws2812_stress_wake_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	/*
	 * This cuts short the driver's fixed k_usleep(), but cannot release a
	 * thread blocked on the TX slab. The fixed driver therefore still waits
	 * for the previous frame's real STOPPED callback.
	 */
	if (atomic_get(&stress_state) == WS2812_STRESS_RUNNING) {
		k_wakeup(ws2812_stress_thread_id);
	}
}

K_TIMER_DEFINE(ws2812_stress_wake_timer, ws2812_stress_wake_handler, NULL);

static void ws2812_stress_restore_led(void)
{
	static const struct sys_led_rgb_pptt restore_marker = {7777, 3333, 1111};

	/*
	 * The stress path bypasses led.c's pixel cache. Force one distinct update
	 * through the normal LED path before releasing the temporary priority slot.
	 */
	k_msleep(WS2812_STRESS_DRAIN_MS);
	set_led_rgb(SYS_LED_PATTERN_ON, restore_marker, SYS_LED_PRIORITY_HIGHEST);
	k_msleep(WS2812_STRESS_SETTLE_MS);
	set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_HIGHEST);
}

static void ws2812_stress_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&stress_start_sem, K_FOREVER);

		while (atomic_get(&stress_state) == WS2812_STRESS_RUNNING) {
			uint32_t attempted = (uint32_t)atomic_get(&attempted_frames);
			struct led_rgb pixel = stress_pixels[attempted % ARRAY_SIZE(stress_pixels)];
			int ret = led_strip_update_rgb(strip, &pixel, 1);

			attempted = (uint32_t)atomic_inc(&attempted_frames) + 1U;
			if (ret < 0) {
				atomic_inc(&failed_frames);
				atomic_set(&last_error, ret);
			} else {
				atomic_inc(&successful_frames);
			}

			if (attempted >= (uint32_t)atomic_get(&target_frames)) {
				(void)atomic_cas(
					&stress_state,
					WS2812_STRESS_RUNNING,
					WS2812_STRESS_STOPPING
				);
			}

			k_yield();
		}

		k_timer_stop(&ws2812_stress_wake_timer);
		atomic_set(
			&elapsed_ms,
			(atomic_val_t)(k_uptime_get_32() - (uint32_t)atomic_get(&start_ms))
		);
		ws2812_stress_restore_led();
		atomic_set(&stress_state, WS2812_STRESS_IDLE);
	}
}

int ws2812_stress_start(uint32_t requested_frames)
{
	static const struct sys_led_rgb_pptt off = {0, 0, 0};

	if (requested_frames == 0U || requested_frames > WS2812_STRESS_MAX_FRAMES) {
		return -EINVAL;
	}
	if (!device_is_ready(strip)) {
		return -ENODEV;
	}
	if (!atomic_cas(&stress_state, WS2812_STRESS_IDLE, WS2812_STRESS_STARTING)) {
		return -EBUSY;
	}

	set_led_rgb(SYS_LED_PATTERN_ON, off, SYS_LED_PRIORITY_HIGHEST);
	k_msleep(WS2812_STRESS_SETTLE_MS);

	atomic_set(&target_frames, (atomic_val_t)requested_frames);
	atomic_set(&attempted_frames, 0);
	atomic_set(&successful_frames, 0);
	atomic_set(&failed_frames, 0);
	atomic_set(&last_error, 0);
	atomic_set(&elapsed_ms, 0);
	atomic_set(&start_ms, (atomic_val_t)k_uptime_get_32());
	atomic_set(&stress_state, WS2812_STRESS_RUNNING);

	k_timer_start(
		&ws2812_stress_wake_timer,
		K_USEC(WS2812_STRESS_WAKE_PERIOD_US),
		K_USEC(WS2812_STRESS_WAKE_PERIOD_US)
	);
	k_sem_give(&stress_start_sem);

	return 0;
}

int ws2812_stress_stop(void)
{
	if (atomic_cas(&stress_state, WS2812_STRESS_RUNNING, WS2812_STRESS_STOPPING)) {
		k_timer_stop(&ws2812_stress_wake_timer);
		k_wakeup(ws2812_stress_thread_id);
		return 0;
	}

	return atomic_get(&stress_state) == WS2812_STRESS_IDLE ? -EALREADY : -EBUSY;
}

void ws2812_stress_get_status(struct ws2812_stress_status *status)
{
	uint32_t started = (uint32_t)atomic_get(&start_ms);

	status->state = (enum ws2812_stress_state)atomic_get(&stress_state);
	status->target_frames = (uint32_t)atomic_get(&target_frames);
	status->attempted_frames = (uint32_t)atomic_get(&attempted_frames);
	status->successful_frames = (uint32_t)atomic_get(&successful_frames);
	status->failed_frames = (uint32_t)atomic_get(&failed_frames);
	status->last_error = (int)atomic_get(&last_error);
	status->elapsed_ms = status->state == WS2812_STRESS_RUNNING ||
				     status->state == WS2812_STRESS_STOPPING
		? k_uptime_get_32() - started
		: (uint32_t)atomic_get(&elapsed_ms);
}

#endif
