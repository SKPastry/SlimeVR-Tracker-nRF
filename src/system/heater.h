#ifndef SLIMENRF_SYSTEM_HEATER
#define SLIMENRF_SYSTEM_HEATER

#include <stdbool.h>
#include <stdint.h>

struct heater_status {
	bool available;
	bool pwm_ready;
	bool imu_powered;
	bool enabled;
	uint16_t duty_pptt;
	uint16_t max_duty_pptt;
	int last_error;
};

bool heater_is_available(void);
bool heater_imu_powered(void);
int heater_set_duty_pptt(uint16_t duty_pptt);
void heater_force_off(void);
void heater_get_status(struct heater_status *status);

#endif
