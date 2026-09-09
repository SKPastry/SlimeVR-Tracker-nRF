#include <stdio.h>
#include <stdlib.h>

#include "sk_led_sync.h"

#ifdef CONFIG_SK_LED_SYNC
#error "This test must compile with SK LED synchronization disabled"
#endif

/* Intentionally no sk_led_sync_read_clock definition or sync-module linkage. */
int main(void)
{
	int state = 0;
	for (int step = 1; step <= 2001; step++) {
		unsigned int period = step % 2 ? SK_LED_SYNC_SLOW_TICKS : SK_LED_SYNC_FAST_TICKS;
		state = sk_led_sync_phase(state, period);
		if (state != step % 1000) {
			fprintf(stderr, "FAIL: disabled step %d: expected %d, got %d\n",
				step, step % 1000, state);
			return EXIT_FAILURE;
		}
	}
	puts("PASS: disabled LED phase retains legacy counter behavior without a clock dependency");
	return EXIT_SUCCESS;
}
