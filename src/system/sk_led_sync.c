/* SPDX-License-Identifier: Apache-2.0 */
#include "sk_led_sync.h"

#if defined(CONFIG_SK_LED_SYNC)
/* Owned by the LED thread. Keep the last offset during sync loss, including
 * while another LED pattern is showing. Zero gives local phase before sync. */
static uint32_t held_offset;

int sk_led_sync_phase(int previous_state, uint32_t period_ticks)
{
	(void)previous_state;
	struct sk_led_clock_sample sample;

	sk_led_sync_read_clock(&sample);
	if (sample.synchronized) {
		held_offset = sample.network_ticks - sample.local_ticks;
	}

	uint32_t ticks = sample.local_ticks + held_offset;
	return (int)((uint64_t)(ticks % period_ticks) * 1000U / period_ticks);
}
#endif
