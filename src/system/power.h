#ifndef SLIMENRF_SYSTEM_POWER
#define SLIMENRF_SYSTEM_POWER

#include <stdbool.h>
#include <stdint.h>

/* Why a power state change was requested. Stored in the power-off record. */
enum sys_off_reason {
	SYS_OFF_REASON_UNKNOWN = 0,
	SYS_OFF_REASON_ACTIVITY_TIMEOUT,   /* no motion for the activity timeout */
	SYS_OFF_REASON_IDLE_WAKE_TIMEOUT,  /* low-activity wake-on-motion session ended */
	SYS_OFF_REASON_IMU_TIMEOUT,        /* no IMU data (CONFIG_USE_IMU_TIMEOUT) */
	SYS_OFF_REASON_CONNECTION_TIMEOUT, /* receiver not responding */
	SYS_OFF_REASON_PAIRING_TIMEOUT,    /* pairing mode timed out */
	SYS_OFF_REASON_COMMAND,            /* receiver or console shutdown/reboot command */
	SYS_OFF_REASON_BATTERY_EMPTY,      /* debounced 0% battery */
	SYS_OFF_REASON_DOCKED,
	SYS_OFF_REASON_USER_BUTTON,        /* button press / long press */
	SYS_OFF_REASON_BOOT_DEBOUNCE,      /* button released right after boot from shutdown */
	SYS_OFF_REASON_WOM_SETUP_FAILED,   /* reboot fallback: IMU wake-up could not be armed */
	SYS_OFF_REASON_SENSOR_FAILURE,     /* sensor scan/init gave up */
	SYS_OFF_REASON_MAG_TOGGLE,         /* magnetometer enable/disable needs re-init */
	SYS_OFF_REASON_DFU,
	SYS_OFF_REASON_OTA,
	SYS_OFF_REASON_COUNT
};

void sys_interface_suspend(void);
void sys_interface_resume(void);

void sys_request_WOM(bool force, bool immediate, enum sys_off_reason reason);
void sys_request_system_off(bool immediate, enum sys_off_reason reason);
void sys_request_system_reboot(bool immediate, enum sys_off_reason reason);

/* Power-off forensics: records are kept in retained RAM and mirrored to NVS.
 * Prints the most recent record (or all records) at INFO level. */
void sys_power_off_log_print(bool all);
const char *sys_off_reason_name(enum sys_off_reason reason);

bool vin_read(void);
bool vbus_read(void);

#endif
