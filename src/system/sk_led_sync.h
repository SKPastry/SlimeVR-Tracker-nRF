/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SK_LED_SYNC_H
#define SK_LED_SYNC_H

#include <stdbool.h>
#include <stdint.h>

/* Both periods divide 2^32, so the network clock can wrap without a jump. */
#define SK_LED_SYNC_SLOW_TICKS (4U * 32768U)
#define SK_LED_SYNC_FAST_TICKS (2U * 32768U)

#if defined(CONFIG_SK_LED_SYNC)
struct sk_led_clock_sample {
	/* Same sampling instant, in 32768 Hz ticks modulo 2^32. */
	uint32_t local_ticks;
	uint32_t network_ticks;
	/* A synchronized network_ticks value of zero is valid. */
	bool synchronized;
};

/* Private ESB adapter. Always supplies local_ticks; when unsynchronized,
 * network_ticks equals local_ticks. Read-only, called by the LED thread on
 * a single-core target, not from an ISR. sample must be non-NULL. */
void sk_led_sync_read_clock(struct sk_led_clock_sample *sample);

/* Called only by the LED thread. period_ticks is one of the constants above.
 * previous_state is ignored when enabled; retained for the disabled path. */
int sk_led_sync_phase(int previous_state, uint32_t period_ticks);
#else
static inline int sk_led_sync_phase(int previous_state, uint32_t period_ticks)
{
	(void)period_ticks;
	return (previous_state + 1) % 1000;
}
#endif

#endif /* SK_LED_SYNC_H */
