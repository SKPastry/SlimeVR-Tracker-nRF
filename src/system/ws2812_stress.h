#ifndef SLIMENRF_SYSTEM_WS2812_STRESS
#define SLIMENRF_SYSTEM_WS2812_STRESS

#include <stdint.h>

#define WS2812_STRESS_DEFAULT_FRAMES 500000U
#define WS2812_STRESS_MAX_FRAMES     10000000U
#define WS2812_STRESS_WAKE_PERIOD_US 100U

enum ws2812_stress_state {
	WS2812_STRESS_IDLE,
	WS2812_STRESS_STARTING,
	WS2812_STRESS_RUNNING,
	WS2812_STRESS_STOPPING,
};

struct ws2812_stress_status {
	enum ws2812_stress_state state;
	uint32_t target_frames;
	uint32_t attempted_frames;
	uint32_t successful_frames;
	uint32_t failed_frames;
	uint32_t elapsed_ms;
	int last_error;
};

int ws2812_stress_start(uint32_t target_frames);
int ws2812_stress_stop(void);
void ws2812_stress_get_status(struct ws2812_stress_status *status);

#endif
