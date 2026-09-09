#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sk_led_sync.h"

_Static_assert(SK_LED_SYNC_SLOW_TICKS == 131072U, "slow pulse must last four seconds");
_Static_assert(SK_LED_SYNC_FAST_TICKS == 65536U, "fast pulse must last two seconds");

static struct sk_led_clock_sample clock_sample;
static unsigned int checks;

void sk_led_sync_read_clock(struct sk_led_clock_sample *sample)
{
	*sample = clock_sample;
}

static void expect_phase(const char *scenario, uint32_t local_ticks,
			 uint32_t network_ticks, bool synchronized,
			 uint32_t period_ticks, int previous_state, int expected)
{
	clock_sample = (struct sk_led_clock_sample) {
		.local_ticks = local_ticks,
		.network_ticks = network_ticks,
		.synchronized = synchronized,
	};
	int actual = sk_led_sync_phase(previous_state, period_ticks);
	checks++;
	if (actual != expected) {
		fprintf(stderr, "FAIL: %s: expected phase %d, got %d\n",
			scenario, expected, actual);
		exit(EXIT_FAILURE);
	}
}

/* Must run first: the production helper deliberately has no reset API. */
static void startup_without_synchronization(void)
{
	expect_phase("startup ignores invalid network sample", 0U, 98304U, false,
		     SK_LED_SYNC_SLOW_TICKS, 749, 0);
	expect_phase("startup follows local time", 32768U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 0, 250);
	expect_phase("startup fast period", 32768U, UINT32_MAX, false,
		     SK_LED_SYNC_FAST_TICKS, 250, 500);
	expect_phase("startup slow period wraps", 131072U, 98304U, false,
		     SK_LED_SYNC_SLOW_TICKS, 999, 0);
}

static void shared_network_time_overrides_local_uptime(void)
{
	const uint32_t local_uptimes[] = {0U, 17U, 50000000U, UINT32_MAX};
	for (unsigned int i = 0; i < sizeof(local_uptimes) / sizeof(local_uptimes[0]); i++) {
		expect_phase("different uptimes share slow phase", local_uptimes[i], 32768U, true,
			     SK_LED_SYNC_SLOW_TICKS, (int)i, 250);
		expect_phase("different uptimes share fast phase", local_uptimes[i], 32768U, true,
			     SK_LED_SYNC_FAST_TICKS, 999 - (int)i, 500);
	}
	expect_phase("last slow tick", 1U, 131071U, true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 999);
	expect_phase("four-second boundary", 2U, 131072U, true,
		     SK_LED_SYNC_SLOW_TICKS, 999, 0);
	expect_phase("last fast tick", 3U, 65535U, true,
		     SK_LED_SYNC_FAST_TICKS, 0, 999);
	expect_phase("two-second boundary", 4U, 65536U, true,
		     SK_LED_SYNC_FAST_TICKS, 999, 0);
}

static void zero_is_a_valid_synchronized_timestamp(void)
{
	expect_phase("valid network zero resets offset", 100000U, 0U, true,
		     SK_LED_SYNC_SLOW_TICKS, 750, 0);
	expect_phase("zero sample offset remains usable after loss", 132768U, UINT32_MAX, false,
		     SK_LED_SYNC_SLOW_TICKS, 0, 250);
}

static void clock_wrap_keeps_phase_continuous(void)
{
	expect_phase("local clock just before wrap", UINT32_C(0xffffc000), 49152U, true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 375);
	expect_phase("local clock wraps while unsynchronized", 0U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 375, 500);
	expect_phase("local clock advances after wrap", 16384U, UINT32_MAX, false,
		     SK_LED_SYNC_SLOW_TICKS, 500, 625);

	expect_phase("network clock just before wrap", 100000U, UINT32_C(0xffffc000), true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 875);
	expect_phase("held network estimate wraps to zero", 116384U, 1U, false,
		     SK_LED_SYNC_SLOW_TICKS, 875, 0);
	expect_phase("held network estimate advances after wrap", 132768U, UINT32_MAX, false,
		     SK_LED_SYNC_SLOW_TICKS, 0, 125);
}

static void loss_of_sync_preserves_offset_and_local_drift(void)
{
	expect_phase("loss test acquires network offset", 900000U, 950272U, true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 250);
	expect_phase("invalid network jumps do not move phase", 900000U, UINT32_MAX, false,
		     SK_LED_SYNC_SLOW_TICKS, 250, 250);
	expect_phase("half-second local advance after loss", 916384U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 250, 375);
	expect_phase("two-second local advance after loss", 965536U, 123U, false,
		     SK_LED_SYNC_SLOW_TICKS, 375, 750);
	expect_phase("long loss advances across multiple periods", 1391520U, 800000U, false,
		     SK_LED_SYNC_SLOW_TICKS, 750, 0);
}

static void phase_does_not_accumulate_callback_count(void)
{
	expect_phase("irregular callbacks acquire zero phase", 1000U, 0U, true,
		     SK_LED_SYNC_SLOW_TICKS, 850, 0);
	for (int previous = 0; previous < 1000; previous += 37) {
		expect_phase("repeated calls at same time do not advance", 1000U, 999999U, false,
			     SK_LED_SYNC_SLOW_TICKS, previous, 0);
	}
	expect_phase("one tick does not round phase upward", 1001U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 999, 0);
	expect_phase("irregular 1500-tick gap", 2500U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 611, 11);
	expect_phase("eighth-second gap rounds down", 9192U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 11, 62);
	expect_phase("one-second gap", 33768U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 888, 250);
	expect_phase("full period after irregular callbacks", 132072U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 250, 0);
}

static void resynchronization_is_immediate(void)
{
	expect_phase("resync test starts on network zero", 700000U, 0U, true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 0);
	expect_phase("resync test free-runs after loss", 732768U, 123U, false,
		     SK_LED_SYNC_SLOW_TICKS, 0, 250);
	expect_phase("new network sample immediately changes phase", 732768U, 98304U, true,
		     SK_LED_SYNC_SLOW_TICKS, 250, 750);
	expect_phase("subsequent loss uses newest offset", 765536U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 750, 0);
}

static void period_changes_and_prompt_gaps_keep_network_alignment(void)
{
	expect_phase("period-switch acquires offset", 400000U, 32768U, true,
		     SK_LED_SYNC_SLOW_TICKS, 0, 250);
	expect_phase("switch to fast without a fresh sync", 400000U, 0U, false,
		     SK_LED_SYNC_FAST_TICKS, 250, 500);
	expect_phase("switch back without counter carryover", 400000U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 500, 250);
	expect_phase("fast boundary after period switch", 432768U, 0U, false,
		     SK_LED_SYNC_FAST_TICKS, 250, 0);
	expect_phase("same instant is slow midpoint", 432768U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 0, 500);

	/* A prompt owns the LED for nine seconds; no pulse-helper calls occur. */
	expect_phase("resume slow pulse after prompt skips elapsed time", 727680U, 0U, false,
		     SK_LED_SYNC_SLOW_TICKS, 19, 750);
	expect_phase("resume fast pulse uses the same held clock", 727680U, 0U, false,
		     SK_LED_SYNC_FAST_TICKS, 750, 500);
}

int main(void)
{
	startup_without_synchronization();
	shared_network_time_overrides_local_uptime();
	zero_is_a_valid_synchronized_timestamp();
	clock_wrap_keeps_phase_continuous();
	loss_of_sync_preserves_offset_and_local_drift();
	phase_does_not_accumulate_callback_count();
	resynchronization_is_immediate();
	period_changes_and_prompt_gaps_keep_network_alignment();
	printf("PASS: %u synchronized LED phase checks\n", checks);
	return EXIT_SUCCESS;
}
