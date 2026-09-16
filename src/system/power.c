#include "globals.h"
#include "sensor/sensor.h"
#include "sensor/calibration/calibration.h"
#include "battery.h"
#include "battery_tracker.h"
#include "connection/connection.h"
#include "system.h"
#include "uptime.h"
#include "led.h"
#include "connection/esb.h"
#include "system/esb_ota.h"
#include "watchdog.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>
#include <zephyr/pm/device.h>
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_spim.h>
#include <hal/nrf_twim.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "power.h"
#include "power_battery.h"
#include "clock_control.h"


enum sys_regulator {
	SYS_REGULATOR_DCDC,
	SYS_REGULATOR_LDO
};

static bool plugged = false;
static bool power_init = false;

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#include "nrf_gpio_util.h" /* after LOG_MODULE_REGISTER: helpers use LOG_INF */

static bool sys_WOM(bool force, enum sys_off_reason reason);
static bool sys_system_off(enum sys_off_reason reason);
static void sys_system_reboot(enum sys_off_reason reason);

enum sys_power_request {
	SYS_POWER_REQ_NONE = 0,
	SYS_POWER_REQ_WOM = 1,
	SYS_POWER_REQ_WOM_FORCE = 2,
	SYS_POWER_REQ_SYSTEM_OFF = 3,
	SYS_POWER_REQ_REBOOT = 4,
};

static int sys_power_state_request(enum sys_power_request id, enum sys_off_reason reason);
static enum sys_power_request sys_power_state_peek(enum sys_off_reason *reason);
static void sys_power_state_clear(void);

/* Battery voltage captured for the power-off record */
static int last_battery_mV = 0;
static bool power_off_recorded = false;
static void power_off_record(enum power_off_path path, enum sys_off_reason reason, uint8_t wom_result,
			     const uint8_t *wom_regs, uint8_t int0_config, uint32_t int0_pin_cnf);

K_THREAD_DEFINE(disable_DFU_thread_id, 128, sys_skip_dfu, NULL, NULL, NULL, DISABLE_DFU_THREAD_PRIORITY, 0, 500); // skip DFU if the system is running correctly

static void power_thread(void);
K_THREAD_DEFINE(power_thread_id, 1024, power_thread, NULL, NULL, NULL, POWER_THREAD_PRIORITY, 0, 0);

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, int0_gpios)
#define IMU_INT_EXISTS true
#else
#warning "IMU wake up GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, dcdc_gpios)
#define DCDC_EN_EXISTS true
static const struct gpio_dt_spec dcdc_en = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, dcdc_gpios);
#else
#pragma message "DCDC enable GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, ldo_gpios)
#define LDO_EN_EXISTS true
static const struct gpio_dt_spec ldo_en = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, ldo_gpios);
#else
#pragma message "LDO enable GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, pwr_gpios)
#define PWR_EXISTS true
static const struct gpio_dt_spec pwr = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, pwr_gpios);
#else
#pragma message "Power GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, int0_gpios)
#define INT0_EXISTS true
static const struct gpio_dt_spec int0 __attribute__((unused)) = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, int0_gpios);
#else
#pragma message "INT0 GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, clk_gpios)
#define CLK_EXISTS true
static const struct gpio_dt_spec clk __attribute__((unused)) = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, clk_gpios);
#else
#pragma message "CLK GPIO does not exist"
#endif
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, vcc_gpios)
#define VCC_EXISTS true
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, vcc_gpios);
#else
#pragma message "VCC GPIO does not exist"
#endif

#define ADAFRUIT_BOOTLOADER (CONFIG_BUILD_OUTPUT_UF2 && !CONFIG_BOOTLOADER_MCUBOOT)

/* CS/VCC -> Hi-Z (GPIO_DISCONNECTED); pwr enable -> driven inactive. */
static void sys_disconnect_interface_pins(void)
{
#if DT_SPI_DEV_HAS_CS_GPIOS(DT_NODELABEL(imu_spi))
	const struct gpio_dt_spec imu_cs = GPIO_DT_SPEC_GET_BY_IDX(
		DT_BUS(DT_NODELABEL(imu_spi)), cs_gpios, DT_REG_ADDR_RAW(DT_NODELABEL(imu_spi)));
	nrf_gpio_configure_dt_log("Disconnected IMU CS", &imu_cs, GPIO_DISCONNECTED);
#endif
#if DT_SPI_DEV_HAS_CS_GPIOS(DT_NODELABEL(mag_spi))
	const struct gpio_dt_spec mag_cs = GPIO_DT_SPEC_GET_BY_IDX(
		DT_BUS(DT_NODELABEL(mag_spi)), cs_gpios, DT_REG_ADDR_RAW(DT_NODELABEL(mag_spi)));
	nrf_gpio_configure_dt_log("Disconnected Magnetometer CS", &mag_cs, GPIO_DISCONNECTED);
#endif
/*
	TODO: for promicro, leaving ext_vcc on draws ~50uA, disconnect works, pulldown may be more reliable
	what to do about boards that use ext_vcc? it is not expected to leave on during WOM
*/
#if PWR_EXISTS
	nrf_gpio_configure_dt_log("Disabled power GPIO", &pwr, GPIO_OUTPUT_INACTIVE);
#endif
#if VCC_EXISTS
	/* Hi-Z (same as nrf_gpio_cfg_default); not OUTPUT_INACTIVE — see TODO above. */
	nrf_gpio_configure_dt_log("Disconnected VCC GPIO", &vcc, GPIO_DISCONNECTED);
#endif
}

void sys_interface_suspend(void)
{
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(imu_spi)))
	const struct device *const pm_spi_imu = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(imu_spi)));
	pm_device_action_run(pm_spi_imu, PM_DEVICE_ACTION_SUSPEND);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(imu)))
	const struct device *const pm_i2c_imu = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(imu)));
	pm_device_action_run(pm_i2c_imu, PM_DEVICE_ACTION_SUSPEND);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(mag_spi)))
	const struct device *const pm_spi_mag = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(mag_spi)));
	pm_device_action_run(pm_spi_mag, PM_DEVICE_ACTION_SUSPEND);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(mag)))
	const struct device *const pm_i2c_mag = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(mag)));
	pm_device_action_run(pm_i2c_mag, PM_DEVICE_ACTION_SUSPEND);
#endif
}

void sys_interface_resume(void)
{
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(imu_spi)))
	const struct device *const pm_spi_imu = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(imu_spi)));
	pm_device_action_run(pm_spi_imu, PM_DEVICE_ACTION_RESUME);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(imu)))
	const struct device *const pm_i2c_imu = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(imu)));
	pm_device_action_run(pm_i2c_imu, PM_DEVICE_ACTION_RESUME);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(mag_spi)))
	const struct device *const pm_spi_mag = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(mag_spi)));
	pm_device_action_run(pm_spi_mag, PM_DEVICE_ACTION_RESUME);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_PARENT(DT_NODELABEL(mag)))
	const struct device *const pm_i2c_mag = DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(mag)));
	pm_device_action_run(pm_i2c_mag, PM_DEVICE_ACTION_RESUME);
#endif
}

// TODO: the gpio sense is weird, maybe the device will turn back on immediately after shutdown or after (attempting to) enter WOM
// TODO: there should be a better system of how to handle all system_off cases and all the sense pins
// TODO: just changed it make sure to test it thanks

// TODO: should the tracker start again if docking state changes?
// TODO: keep sending battery state while plugged and docked?
// TODO: on some boards there is actual power path, try to use the LED in this case
// TODO: usually charging, i would flash LED but that will drain the battery while it is charging..
// TODO: should not really shut off while plugged in

static void configure_system_off(void)
{
	if (get_status(SYS_STATUS_SENSOR_ERROR))
		LOG_WRN("Entering new power state while sensor error is raised");
	if (get_status(SYS_STATUS_SYSTEM_ERROR))
		LOG_WRN("Entering new power state while system error is raised");
	/* Freeze online-mag commits before the final warm-NVS flush. */
	sensor_calibration_online_mag_prepare_power_down();
	clock_pre_shutdown();
	main_imu_suspend();
	sensor_shutdown();
	set_led(SYS_LED_PATTERN_OFF_FORCE, SYS_LED_PRIORITY_HIGHEST);
	float actual_clock_rate;
	set_sensor_clock(false, 0, &actual_clock_rate);
	// Configure interrupts
	configure_sense_pins();
}

static void set_regulator(enum sys_regulator regulator)
{
#if DCDC_EN_EXISTS
	bool use_dcdc = regulator == SYS_REGULATOR_DCDC;
	if (use_dcdc)
	{
		gpio_pin_set_dt(&dcdc_en, 1);
		LOG_INF("Enabled DCDC");
	}
#endif
#if LDO_EN_EXISTS
	bool use_ldo = regulator == SYS_REGULATOR_LDO;
	gpio_pin_set_dt(&ldo_en, use_ldo);
	LOG_INF("%s", use_ldo ? "Enabled LDO" : "Disabled LDO");
#endif
#if DCDC_EN_EXISTS
	if (!use_dcdc)
	{
		gpio_pin_set_dt(&dcdc_en, 0);
		LOG_INF("Disabled DCDC");
	}
#endif
}

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_twim)
static void __maybe_unused disconnect_twim_pins(uintptr_t reg)
{
	NRF_TWIM_Type *twim = (NRF_TWIM_Type *)reg;

	nrf_psel_cfg_default("Disconnected I2C SCL", nrf_twim_scl_pin_get(twim));
	nrf_psel_cfg_default("Disconnected I2C SDA", nrf_twim_sda_pin_get(twim));
}
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_spim)
static void __maybe_unused disconnect_spim_pins(uintptr_t reg)
{
	NRF_SPIM_Type *spim = (NRF_SPIM_Type *)reg;

	nrf_psel_cfg_default("Disconnected SPI SCK", nrf_spim_sck_pin_get(spim));
	nrf_psel_cfg_default("Disconnected SPI MOSI", nrf_spim_mosi_pin_get(spim));
	nrf_psel_cfg_default("Disconnected SPI MISO", nrf_spim_miso_pin_get(spim));
}
#endif

#define IS_TRACKER_SENSOR_NODE(node)                                                                   \
	((DT_NODE_EXISTS(DT_NODELABEL(imu)) && DT_SAME_NODE(node, DT_NODELABEL(imu))) ||                \
	 (DT_NODE_EXISTS(DT_NODELABEL(imu_spi)) && DT_SAME_NODE(node, DT_NODELABEL(imu_spi))) ||        \
	 (DT_NODE_EXISTS(DT_NODELABEL(mag)) && DT_SAME_NODE(node, DT_NODELABEL(mag))) ||                \
	 (DT_NODE_EXISTS(DT_NODELABEL(mag_spi)) && DT_SAME_NODE(node, DT_NODELABEL(mag_spi))))

#define SENSOR_BUS_FOREIGN_CHILD(child) +!IS_TRACKER_SENSOR_NODE(child)

/* Other okay children (flash, PMIC, display, ...) share this bus. */
#define SENSOR_BUS_HAS_FOREIGN_CHILD(bus)                                                              \
	(0 DT_FOREACH_CHILD_STATUS_OKAY(bus, SENSOR_BUS_FOREIGN_CHILD))

#define DISCONNECT_NRF_BUS_PINS(bus)                                                                   \
	IF_ENABLED(DT_NODE_HAS_COMPAT(bus, nordic_nrf_twim),                                           \
		   (disconnect_twim_pins(DT_REG_ADDR(bus));))                                          \
	IF_ENABLED(DT_NODE_HAS_COMPAT(bus, nordic_nrf_spim),                                           \
		   (disconnect_spim_pins(DT_REG_ADDR(bus));))

#define DISCONNECT_SENSOR_DEV_BUS(dev_id)                                                              \
	do {                                                                                           \
		if (SENSOR_BUS_HAS_FOREIGN_CHILD(DT_BUS(dev_id)) == 0) {                               \
			DISCONNECT_NRF_BUS_PINS(DT_BUS(dev_id));                                       \
		}                                                                                      \
	} while (0)

static void disconnect_sensor_pins(void)
{
#if CONFIG_DISABLE_SENSOR_GPIOS_ON_SHUTDOWN
	LOG_INF("Disconnecting sensor GPIOs");
#if DT_NODE_EXISTS(DT_NODELABEL(imu)) && DT_NODE_HAS_STATUS_OKAY(DT_BUS(DT_NODELABEL(imu)))
	DISCONNECT_SENSOR_DEV_BUS(DT_NODELABEL(imu));
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(imu_spi)) && DT_NODE_HAS_STATUS_OKAY(DT_BUS(DT_NODELABEL(imu_spi)))
	DISCONNECT_SENSOR_DEV_BUS(DT_NODELABEL(imu_spi));
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(mag)) && DT_NODE_HAS_STATUS_OKAY(DT_BUS(DT_NODELABEL(mag)))
	DISCONNECT_SENSOR_DEV_BUS(DT_NODELABEL(mag));
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(mag_spi)) && DT_NODE_HAS_STATUS_OKAY(DT_BUS(DT_NODELABEL(mag_spi)))
	DISCONNECT_SENSOR_DEV_BUS(DT_NODELABEL(mag_spi));
#endif
	LOG_INF("All sensor GPIO pins disconnected");
#endif
}

#undef IS_TRACKER_SENSOR_NODE
#undef SENSOR_BUS_FOREIGN_CHILD
#undef SENSOR_BUS_HAS_FOREIGN_CHILD
#undef DISCONNECT_NRF_BUS_PINS
#undef DISCONNECT_SENSOR_DEV_BUS

static void wait_for_logging(void)
{
#if CONFIG_LOG_BACKEND_UART
	// only UART backend is disabled usually
	const struct log_backend *uart_backend = log_backend_get_by_name("log_backend_uart");
	if (!uart_backend)
		return;
	bool uart_active = log_backend_is_active(uart_backend);
	if (uart_active)
	{
		LOG_INF("Delayed for UART backend");
		k_msleep(200);
	}
#endif
}

#if IMU_INT_EXISTS && CONFIG_DELAY_SLEEP_ON_STATUS
static int64_t system_off_timeout = 0;
#endif

void sys_request_WOM(bool force, bool immediate, enum sys_off_reason reason)
{
	if (immediate)
	{
		sys_WOM(force, reason);
		return;
	}
	if (force) {
		sys_power_state_request(SYS_POWER_REQ_WOM_FORCE, reason);
	} else {
		sys_power_state_request(SYS_POWER_REQ_WOM, reason);
	}
}

void sys_request_system_off(bool immediate, enum sys_off_reason reason)
{
	if (immediate)
	{
		sys_system_off(reason);
		return;
	}
	sys_power_state_request(SYS_POWER_REQ_SYSTEM_OFF, reason);
}

void sys_request_system_reboot(bool immediate, enum sys_off_reason reason)
{
	if (immediate)
	{
		sys_system_reboot(reason);
		return;
	}
	sys_power_state_request(SYS_POWER_REQ_REBOOT, reason);
}

static const char *const sys_off_reason_names[SYS_OFF_REASON_COUNT] = {
	[SYS_OFF_REASON_UNKNOWN] = "unknown",
	[SYS_OFF_REASON_ACTIVITY_TIMEOUT] = "activity_timeout",
	[SYS_OFF_REASON_IDLE_WAKE_TIMEOUT] = "idle_wake_timeout",
	[SYS_OFF_REASON_IMU_TIMEOUT] = "imu_timeout",
	[SYS_OFF_REASON_CONNECTION_TIMEOUT] = "connection_timeout",
	[SYS_OFF_REASON_PAIRING_TIMEOUT] = "pairing_timeout",
	[SYS_OFF_REASON_COMMAND] = "command",
	[SYS_OFF_REASON_BATTERY_EMPTY] = "battery_empty",
	[SYS_OFF_REASON_DOCKED] = "docked",
	[SYS_OFF_REASON_USER_BUTTON] = "user_button",
	[SYS_OFF_REASON_BOOT_DEBOUNCE] = "boot_debounce",
	[SYS_OFF_REASON_WOM_SETUP_FAILED] = "wom_setup_failed",
	[SYS_OFF_REASON_SENSOR_FAILURE] = "sensor_failure",
	[SYS_OFF_REASON_MAG_TOGGLE] = "mag_toggle",
	[SYS_OFF_REASON_DFU] = "dfu",
	[SYS_OFF_REASON_OTA] = "ota",
};

const char *sys_off_reason_name(enum sys_off_reason reason)
{
	if (reason >= SYS_OFF_REASON_COUNT || sys_off_reason_names[reason] == NULL) {
		return "invalid";
	}
	return sys_off_reason_names[reason];
}

static const char *power_off_path_name(uint8_t path)
{
	switch (path) {
	case POWER_OFF_PATH_WOM:
		return "wom";
	case POWER_OFF_PATH_SYSTEM_OFF:
		return "system_off";
	case POWER_OFF_PATH_REBOOT:
		return "reboot";
	default:
		return "none";
	}
}

static const char *sensor_wom_result_name(uint8_t result)
{
	switch (result) {
	case SENSOR_WOM_NOT_ATTEMPTED:
		return "not_attempted";
	case SENSOR_WOM_VERIFIED:
		return "verified";
	case SENSOR_WOM_UNVERIFIED:
		return "unverified";
	case SENSOR_WOM_SETUP_FAILED:
		return "setup_failed";
	case SENSOR_WOM_VERIFY_FAILED:
		return "verify_failed";
	default:
		return "invalid";
	}
}

/* Append a power-off record to the retained ring and mirror it to NVS. Must be
 * called before sys_poweroff()/sys_reboot(); the next boot prints it. */
static void power_off_record(enum power_off_path path, enum sys_off_reason reason, uint8_t wom_result,
			     const uint8_t *wom_regs, uint8_t int0_config, uint32_t int0_pin_cnf)
{
	struct power_off_log *log = &retained->power_off_log;
	if (log->magic != POWER_OFF_LOG_MAGIC || log->next >= POWER_OFF_LOG_DEPTH || log->count > POWER_OFF_LOG_DEPTH) {
		memset(log, 0, sizeof(*log));
		log->magic = POWER_OFF_LOG_MAGIC;
	}
	struct power_off_record *rec = &log->rec[log->next];
	memset(rec, 0, sizeof(*rec));
	rec->seq = ++log->seq;
	rec->path = path;
	rec->reason = reason;
	rec->wom_result = wom_result;
	int imu_id = sensor_get_imu_id();
	rec->imu_id = imu_id < 0 ? 0xFF : (uint8_t)imu_id;
	rec->int0_config = int0_config;
	rec->wom_flags = sensor_get_wom_session_flags();
	if (wom_regs) {
		memcpy(rec->wom_regs, wom_regs, sizeof(rec->wom_regs));
	}
	rec->idle_wake_streak = retained->wom_idle_wake_streak;
	rec->wdt_reset_count = watchdog_get_reset_count();
	int16_t pptt = power_battery_current_pptt();
	rec->battery_pct = pptt < 0 ? 0xFF : (uint8_t)(pptt / 100);
	rec->battery_mv = (uint16_t)CLAMP(last_battery_mV, 0, UINT16_MAX);
	rec->int0_pin_cnf = int0_pin_cnf;
	rec->boot_resetreas = watchdog_get_boot_resetreas();
	rec->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	log->next = (log->next + 1) % POWER_OFF_LOG_DEPTH;
	if (log->count < POWER_OFF_LOG_DEPTH) {
		log->count++;
	}
	power_off_recorded = true;
	LOG_INF("Power-off record #%u: path=%s reason=%s wom=%s", rec->seq, power_off_path_name(rec->path),
		sys_off_reason_name(rec->reason), sensor_wom_result_name(rec->wom_result));
	/* Retained RAM is outside the CRC; NVS mirror survives pin reset / battery removal. */
	sys_write(POWER_OFF_LOG_ID, NULL, log, sizeof(*log));
}

static void power_off_record_print(const struct power_off_record *rec, const char *prefix)
{
	LOG_INF("%s #%u: path=%s reason=%s wom=%s imu=%u regs=%02X %02X %02X %02X int0_cfg=0x%02X pin_cnf=0x%08X",
		prefix, rec->seq, power_off_path_name(rec->path), sys_off_reason_name(rec->reason),
		sensor_wom_result_name(rec->wom_result), rec->imu_id, rec->wom_regs[0], rec->wom_regs[1], rec->wom_regs[2],
		rec->wom_regs[3], rec->int0_config, rec->int0_pin_cnf);
	LOG_INF("%s #%u: batt=%umV/%u%% wom_flags=0x%02X idle_streak=%u wdt=%u resetreas=0x%08X uptime=%us", prefix,
		rec->seq, rec->battery_mv, rec->battery_pct, rec->wom_flags, rec->idle_wake_streak, rec->wdt_reset_count,
		rec->boot_resetreas, rec->uptime_s);
}

void sys_power_off_log_print(bool all)
{
	const struct power_off_log *log = &retained->power_off_log;
	if (log->magic != POWER_OFF_LOG_MAGIC || log->count == 0 || log->count > POWER_OFF_LOG_DEPTH
	    || log->next >= POWER_OFF_LOG_DEPTH) {
		LOG_INF("Last power-off: no record");
		return;
	}
	uint8_t shown = all ? log->count : 1;
	for (uint8_t i = 0; i < shown; i++) {
		/* newest first */
		uint8_t idx = (log->next + POWER_OFF_LOG_DEPTH - 1 - i) % POWER_OFF_LOG_DEPTH;
		power_off_record_print(&log->rec[idx], i == 0 ? "Last power-off" : "Earlier power-off");
	}
	LOG_INF("This boot: resetreas=0x%08X wdt_resets=%u", watchdog_get_boot_resetreas(), watchdog_get_reset_count());
}

/* Returns true when the power request is consumed; false to keep it queued. */
static bool sys_WOM(bool force, enum sys_off_reason reason) // TODO: if IMU interrupt does not exist what does the system do?
{
	LOG_INF("IMU wake up requested");
	/* Block sleep during OTA (active or suppressed) */
	if (esb_ota_is_active() || connection_get_ota_suppressed()) {
		LOG_INF("IMU wake up blocked by OTA");
		return true; /* consume; sensor re-requests after next idle cycle */
	}
#if IMU_INT_EXISTS
#if CONFIG_DELAY_SLEEP_ON_STATUS
	if (!force && (!esb_ready() || !status_ready())) // Wait for esb to pair in case the user is still trying to pair the device
	{
		if (!system_off_timeout)
			system_off_timeout = k_uptime_get() + 30000; // allow system off after 30 seconds if status errors are still active
		if (k_uptime_get() < system_off_timeout)
		{
			LOG_INF("IMU wake up not available, waiting on ESB/status ready");
			return false; /* keep request so power_thread retries after timeout */
		}
		LOG_INF("ESB/status ready timed out");
	}
#endif
	configure_system_off(); // Common subsystem shutdown and prepare sense pins
	sys_flush_warm(); /* adaptive cal → NVS before retained-only sleep */
	sensor_calibration_online_mag_retained_save();
	sensor_record_wom_sleep();
	sensor_retained_write();
#if WOM_USE_DCDC // In case DCDC is more efficient in the ~10-100uA range
	set_regulator(SYS_REGULATOR_DCDC); // Make sure DCDC is selected
#else
	set_regulator(SYS_REGULATOR_LDO); // Switch to LDO
#endif
	// Set system off
	uint8_t wom_result = SENSOR_WOM_NOT_ATTEMPTED;
	uint8_t wom_regs[SENSOR_WOM_REGS] = {0};
	uint8_t pin_config = sensor_setup_WOM(&wom_result, wom_regs); // enable WOM feature, verified by read-back
	if (pin_config == 0xFF) {
		/* Already past configure_system_off; cannot restore cleanly. Never enter
		 * System OFF with wake-up unarmed: that sleep could only be ended by the button. */
		LOG_ERR("IMU wake up setup failed after shutdown prep (%s), rebooting", sensor_wom_result_name(wom_result));
		power_off_record(POWER_OFF_PATH_WOM, reason, wom_result, wom_regs, 0xFF, 0);
		sys_system_reboot(SYS_OFF_REASON_WOM_SETUP_FAILED);
		return true;
	}
	LOG_INF("Configured IMU wake up (%s)", sensor_wom_result_name(wom_result));
#if CONFIG_SENSOR_FAST_WOM_WAKE && NRF_POWER_HAS_GPREGRET \
	&& (defined(POWER_GPREGRET2_GPREGRET_Msk) || defined(POWER_GPREGRET_MaxCount))
	if (pin_config != 0)
		nrf_power_gpregret_set(NRF_POWER, 1, SENSOR_WOM_FAST_WAKE_GPREGRET);
#endif
	// Configure WOM interrupt
	uint32_t int0_gpios = NRF_DT_GPIOS_TO_PSEL(ZEPHYR_USER_NODE, int0_gpios);
	LOG_INF("Wake up GPIO " NRF_ABS_PIN_LOG_FMT ", config: %u", NRF_ABS_PIN_LOG_ARGS(int0_gpios),
		pin_config);
	nrf_gpio_cfg_input(int0_gpios, (pin_config >> 4) & 0xF);
	nrf_gpio_cfg_sense_set(int0_gpios, pin_config & 0xF);
	uint32_t int0_pin = int0_gpios;
	uint32_t int0_pin_cnf = nrf_gpio_pin_port_decode(&int0_pin)->PIN_CNF[int0_pin];
	LOG_INF("Configured IMU wake up GPIO (PIN_CNF 0x%08X)", int0_pin_cnf);
	power_off_record(POWER_OFF_PATH_WOM, reason, wom_result, wom_regs, pin_config, int0_pin_cnf);
	LOG_INF("Powering off nRF");
	sys_update_battery_tracker(power_battery_current_pptt(), power_battery_device_plugged());
//	retained_update();
	wait_for_logging();
#if ADAFRUIT_BOOTLOADER // if using Adafruit bootloader, always skip dfu for next boot
	sys_skip_dfu();
#endif
	sys_poweroff();
	return true;
#else
	ARG_UNUSED(reason);
	LOG_WRN("IMU wake up GPIO does not exist");
	LOG_WRN("IMU wake up not available");
	return true;
#endif
}

/* Returns true when the request is consumed; false to keep it queued. */
static bool sys_system_off(enum sys_off_reason reason) // TODO: add timeout
{
	LOG_INF("System off requested (%s)", sys_off_reason_name(reason));
	/* Block shutdown during OTA (active or suppressed) */
	if (esb_ota_is_active() || connection_get_ota_suppressed()) {
		LOG_INF("System off blocked by OTA");
		return false; /* keep queued until OTA finishes */
	}
	configure_system_off(); // Common subsystem shutdown and prepare sense pins
	sys_flush_warm(); /* persist warm cal before session clear / power loss */
	sensor_calibration_online_mag_cold_start();
#if CONFIG_SENSOR_USE_TCAL
	// Reset boot calibration state so it will recalibrate on next boot
	sensor_boot_cal_reset();
	sensor_fusion_invalidate();
#endif
	// sensor_fusion_update_bias(NULL);
	// sensor_retained_write();
	set_regulator(SYS_REGULATOR_LDO); // Switch to LDO
	// Set system off
#if IMU_INT_EXISTS
	/* Idle: input buffer off + pulldown (not Hi-Z cfg_default). */
	uint32_t int0_gpios = NRF_DT_GPIOS_TO_PSEL(ZEPHYR_USER_NODE, int0_gpios);
	LOG_INF("Wake up GPIO " NRF_ABS_PIN_LOG_FMT, NRF_ABS_PIN_LOG_ARGS(int0_gpios));
	nrf_gpio_cfg(int0_gpios, NRF_GPIO_PIN_DIR_INPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
	LOG_INF("Configured IMU wake-up GPIO idle (pulldown)");
	uint32_t int0_pin = int0_gpios;
	uint32_t int0_pin_cnf = nrf_gpio_pin_port_decode(&int0_pin)->PIN_CNF[int0_pin];
#else
	uint32_t int0_pin_cnf = 0;
#endif
	/* TODO: only an improvement during shutdown? causes higher usage in WOM */
	sys_disconnect_interface_pins();
	power_off_record(POWER_OFF_PATH_SYSTEM_OFF, reason, SENSOR_WOM_NOT_ATTEMPTED, NULL, 0xFF, int0_pin_cnf);
	LOG_INF("Powering off nRF");
#if CONFIG_DISABLE_SENSOR_GPIOS_ON_SHUTDOWN
	disconnect_sensor_pins();
#endif
	sys_update_battery_tracker(power_battery_current_pptt(), power_battery_device_plugged());
	// retained_update();
	wait_for_logging();
#if ADAFRUIT_BOOTLOADER // if using Adafruit bootloader, always skip dfu for next boot
	sys_skip_dfu();
#endif
	sys_poweroff();
	return true;
}

static void sys_system_reboot(enum sys_off_reason reason) // TODO: add timeout
{
	LOG_INF("System reboot requested (%s)", sys_off_reason_name(reason));
	if (!power_off_recorded) {
		/* A failed IMU wake-up attempt records itself before falling back to a reboot. */
		power_off_record(POWER_OFF_PATH_REBOOT, reason, SENSOR_WOM_NOT_ATTEMPTED, NULL, 0xFF, 0);
	}
	configure_system_off(); // Common subsystem shutdown and prepare sense pins
	sys_flush_warm(); /* persist warm cal before reboot (covers OTA reboot path) */
	sensor_calibration_online_mag_cold_start();
#if CONFIG_SENSOR_USE_TCAL
	// Reset boot calibration state so it will recalibrate on next boot
	sensor_boot_cal_reset();
#endif
	sensor_retained_write();
	// Set system reboot
	LOG_INF("Rebooting nRF");
	sys_update_battery_tracker(power_battery_current_pptt(), power_battery_device_plugged());
//	retained_update();
	wait_for_logging();
#if ADAFRUIT_BOOTLOADER // if using Adafruit bootloader, always skip dfu for next boot
	sys_skip_dfu();
#endif
	sys_reboot(SYS_REBOOT_COLD);
}

static enum sys_power_request power_request = SYS_POWER_REQ_NONE;
static enum sys_off_reason power_request_reason = SYS_OFF_REASON_UNKNOWN;
static K_SEM_DEFINE(power_wake_sem, 0, 1);

static int sys_power_state_request(enum sys_power_request id, enum sys_off_reason reason)
{
	if (id == SYS_POWER_REQ_NONE) {
		return -1;
	}
	if (power_request != SYS_POWER_REQ_NONE) {
		LOG_ERR("System is already entering a new power state");
		return -1;
	}
	power_request_reason = reason;
	power_request = id;
	k_sem_give(&power_wake_sem);
	return 0;
}

static enum sys_power_request sys_power_state_peek(enum sys_off_reason *reason)
{
	*reason = power_request_reason;
	return power_request;
}

static void sys_power_state_clear(void)
{
	power_request = SYS_POWER_REQ_NONE;
}

bool vin_read(void) // blocking
{
	while (!power_init)
		k_usleep(1); // wait for first battery read
	return plugged;
}

bool vbus_read(void)
{
#ifdef POWER_USBREGSTATUS_VBUSDETECT_Msk
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
#else
	return vin_read();
#endif
}


// TODO: this thread is handling reading charging state, battery state, dock state, and setting status/led
// TODO: should be separated to be more clear in its function?
// TODO: call into other thread for handling the system state
static void power_thread(void)
{
	static bool boot_success_checked = false;
	static bool watchdog_registered = false;
	static bool ota_gpregret_logged = false;

	/* Register power thread with watchdog (watchdog is initialized via SYS_INIT) */
	if (!watchdog_registered) {
		watchdog_registered = true;
		watchdog_register_thread(WDT_CHANNEL_POWER, 0);
	}

	while (1)
	{
		/* Log OTA RAM engine GPREGRET and the previous power-off record once, after USB console is ready (~5s) */
		if (!ota_gpregret_logged && system_uptime_since_boot_ms() > 5000) {
			ota_gpregret_logged = true;
			uint8_t gp = watchdog_get_ota_gpregret();
			if (gp == 0xDE) {
				LOG_INF("OTA RAM engine completed (GPREGRET=0x%02X)", gp);
			} else if (gp >= 0xD0 && gp < 0xDE) {
				LOG_WRN("OTA RAM engine GPREGRET=0x%02X (last stage before reset)", gp);
			}
			sys_power_off_log_print(false);
		}

		/* After 60 seconds of successful operation, mark boot as successful.
		 * This is long enough to ensure the system is truly stable before
		 * clearing the WDT reset counter, allowing multiple WDT resets to
		 * accumulate and eventually trigger DFU mode if there's a persistent issue.
		 */
		if (!boot_success_checked && system_uptime_since_boot_ms() > 60000) {
			boot_success_checked = true;
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
			if (!boot_is_img_confirmed()) {
				int err = boot_write_img_confirmed();
				if (err) {
					LOG_ERR("Failed to confirm MCUboot image: %d", err);
				} else {
					LOG_INF("MCUboot test image confirmed");
				}
			}
#endif
			watchdog_mark_boot_success();
		}

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart0))
		const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
		pm_device_action_run(uart, PM_DEVICE_ACTION_SUSPEND);
#endif
		enum sys_off_reason requested_reason;
		enum sys_power_request requested = sys_power_state_peek(&requested_reason);
		bool consumed = true;
		switch (requested) {
		case SYS_POWER_REQ_WOM:
			consumed = sys_WOM(false, requested_reason);
			break;
		case SYS_POWER_REQ_WOM_FORCE:
			consumed = sys_WOM(true, requested_reason);
			break;
		case SYS_POWER_REQ_SYSTEM_OFF:
			consumed = sys_system_off(requested_reason);
			break;
		case SYS_POWER_REQ_REBOOT:
			sys_system_reboot(requested_reason);
			break;
		case SYS_POWER_REQ_NONE:
		default:
			break;
		}
		if (consumed) {
			sys_power_state_clear();
		}

		bool docked = dock_read();
		bool charging = chg_read();
		bool charged = stby_read();
		bool pmic_plugged = false;
		int charger_state_err = battery_charger_state(&pmic_plugged, &charging, &charged);
		if (charger_state_err != 0 && charger_state_err != -ENOTSUP) {
			LOG_WRN("Failed to read charger state: %d", charger_state_err);
		}

		int battery_mV;
		int16_t battery_pptt = read_batt_mV(&battery_mV);
		if (battery_pptt < 0)
			LOG_ERR("Failed to read battery voltage: %d", battery_pptt);
		bool battery_pptt_valid = power_battery_pptt_is_valid(battery_pptt);

		bool abnormal_reading = battery_mV < 100 || battery_mV > 6000;
		bool battery_available = battery_mV > 1500 && !abnormal_reading; // Keep working without the battery connected, otherwise it is obviously too dead to boot system
		// Separate detection of vin
		if (!plugged && battery_mV > 4300 && !abnormal_reading)
			plugged = true;
		else if ((plugged && battery_mV <= 4250) || abnormal_reading)
			plugged = false;
#ifdef POWER_USBREGSTATUS_VBUSDETECT_Msk
		bool usb_plugged = NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk;
#else
		bool usb_plugged = false;
#endif
		int64_t now_ms = k_uptime_get();
		bool raw_device_plugged = charging || charged || plugged || usb_plugged || pmic_plugged;
		bool plug_state_debouncing = power_battery_update_plugged_state(raw_device_plugged, now_ms);
		bool plug_signal_settling = power_battery_plug_signal_settling(plug_state_debouncing, now_ms);
		int32_t average_pptt = power_battery_average_pptt();
		bool battery_discharged = !plug_signal_settling && battery_available
			&& (average_pptt >= 0 ? average_pptt : battery_pptt) == 0;
		last_battery_mV = battery_mV;

		power_battery_set_charged(charged); // TODO: timer on device_plugged could be used to infer charged state
		bool device_plugged = power_battery_device_plugged();
		bool device_charged = power_battery_device_charged();

		if (!power_init)
		{
			// log battery state once
			if (battery_available)
				LOG_INF("Battery %u%% (%d mV)", battery_pptt / 100, battery_mV);
			else
				LOG_INF("Battery not available (%d mV)", battery_mV);
			if (abnormal_reading)
			{
				LOG_ERR("Battery voltage reading is abnormal");
				set_status(SYS_STATUS_SYSTEM_ERROR, true);
			}
			set_regulator(SYS_REGULATOR_DCDC); // Switch to DCDC
			power_init = true;
		}

		if ((battery_discharged && !device_plugged) || docked) // TODO: docked may or may not also mean device_plugged due to charging
		{
			if (battery_discharged)
			{
				LOG_WRN("Discharged battery");
				sys_update_battery_tracker(0, device_plugged);
			}
			/* Genuinely empty battery / docked: full system off (no motion wake) is intended. */
			sys_request_system_off(true, battery_discharged ? SYS_OFF_REASON_BATTERY_EMPTY : SYS_OFF_REASON_DOCKED);
		}

		power_battery_feed_and_track(battery_pptt_valid, plug_signal_settling, battery_pptt,
					     battery_available, battery_mV);

		int16_t calibrated_battery_pptt = power_battery_calibrated_pptt();
		connection_update_battery(
			battery_available,
			device_plugged,
			device_charged,
			calibrated_battery_pptt >= 0 ? (uint32_t)calibrated_battery_pptt : 0,
			battery_mV
		);

		if (charging)
			set_led(SYS_LED_PATTERN_PULSE_PERSIST, SYS_LED_PRIORITY_SYSTEM);
		else if (charged)
			set_led(SYS_LED_PATTERN_ON_PERSIST, SYS_LED_PRIORITY_SYSTEM);
		else if (plugged || usb_plugged || pmic_plugged)
			set_led(SYS_LED_PATTERN_PULSE_PERSIST, SYS_LED_PRIORITY_SYSTEM);
		else if (power_battery_is_low())
			set_led(SYS_LED_PATTERN_LONG_PERSIST, SYS_LED_PRIORITY_SYSTEM);
		else
			set_led(SYS_LED_PATTERN_ACTIVE_PERSIST, SYS_LED_PRIORITY_SYSTEM);
//			set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_SYSTEM);

		/* Feed watchdog at end of each loop iteration */
		watchdog_feed(WDT_CHANNEL_POWER);

		(void)k_sem_take(&power_wake_sem, K_MSEC(100));
	}
}
