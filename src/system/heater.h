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
/*
 * Force a checked zero-duty write and latch non-zero writes off.  The latch is
 * deliberately retained after success; a new sensor-thread control session
 * must call heater_prepare_control() before it can drive the heater again.
 */
int heater_force_off_checked(void);
void heater_force_off(void);
/*
 * Suspend is a stronger lifecycle gate: heater_prepare_control() cannot clear
 * it.  Resume only removes the lifecycle bit; the safety latch remains until a
 * new, checked control session is prepared.
 */
int heater_suspend_control(void);
void heater_resume_control(void);
int heater_prepare_control(void);
void heater_get_status(struct heater_status *status);

#endif
