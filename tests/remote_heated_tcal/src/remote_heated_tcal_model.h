/* SPDX-License-Identifier: MIT */
#ifndef REMOTE_HEATED_TCAL_MODEL_H
#define REMOTE_HEATED_TCAL_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remote_tcal_protocol.h"

#define RT_PACKET_LEN SK_ESB_REMOTE_TCAL_PACKET_LEN
#define RT_PING_TYPE SK_ESB_REMOTE_TCAL_PING_TYPE
#define RT_PONG_TYPE SK_ESB_REMOTE_TCAL_PONG_TYPE
#define RT_NORMAL_FLAG SK_ESB_REMOTE_TCAL_NORMAL_FLAG
#define RT_ESCAPE SK_ESB_EXT_ESCAPE
#define RT_MAGIC_0 SK_ESB_EXT_MAGIC_0
#define RT_MAGIC_1 SK_ESB_EXT_MAGIC_1
#define RT_VERSION SK_ESB_EXT_VERSION
#define RT_RESULT_MARKER SK_ESB_HEATED_TCAL_RESULT_MARKER
#define RT_DEFAULT_TARGET SK_ESB_HEATED_TCAL_DEFAULT_TARGET

#define RT_START_CONFIRM_DELAY_MS 1500U
#define RT_MATCH_FRESH_MS 2000U
#define RT_WAIT_MAX_MS 4000U
#define RT_ALIAS_CAPACITY 3U

enum rt_action {
	RT_ACTION_START = SK_ESB_HEATED_TCAL_START,
	RT_ACTION_STOP = SK_ESB_HEATED_TCAL_STOP,
	RT_ACTION_ABORT = SK_ESB_HEATED_TCAL_ABORT,
};

enum rt_result {
	RT_RESULT_OK = SK_ESB_HEATED_TCAL_OK,
	RT_RESULT_INVALID = SK_ESB_HEATED_TCAL_INVALID,
	RT_RESULT_UNSUPPORTED = SK_ESB_HEATED_TCAL_UNSUPPORTED,
	RT_RESULT_BUSY = SK_ESB_HEATED_TCAL_BUSY,
	RT_RESULT_POWER_REQUIRED = SK_ESB_HEATED_TCAL_POWER_REQUIRED,
	RT_RESULT_NOT_READY = SK_ESB_HEATED_TCAL_NOT_READY,
	RT_RESULT_TIMEOUT = SK_ESB_HEATED_TCAL_TIMEOUT,
	RT_RESULT_HARDWARE_ERROR = SK_ESB_HEATED_TCAL_HARDWARE_ERROR,
	RT_RESULT_NOT_ACTIVE = SK_ESB_HEATED_TCAL_NOT_ACTIVE,
	RT_RESULT_INTERNAL = SK_ESB_HEATED_TCAL_INTERNAL,
	RT_RESULT_CANCELED = SK_ESB_HEATED_TCAL_CANCELED,
};

enum rt_stop_reason {
	RT_STOP_NONE = SK_ESB_HEATED_TCAL_STOP_NONE,
	RT_STOP_COMPLETE = SK_ESB_HEATED_TCAL_STOP_COMPLETE,
	RT_STOP_USER = SK_ESB_HEATED_TCAL_STOP_USER,
	RT_STOP_TIMEOUT = SK_ESB_HEATED_TCAL_STOP_TIMEOUT,
	RT_STOP_POWER_LOST = SK_ESB_HEATED_TCAL_STOP_POWER_LOST,
	RT_STOP_IMU_POWER_OFF = SK_ESB_HEATED_TCAL_STOP_IMU_POWER_OFF,
	RT_STOP_TEMP_STALE = SK_ESB_HEATED_TCAL_STOP_TEMP_STALE,
	RT_STOP_OVERTEMP = SK_ESB_HEATED_TCAL_STOP_OVERTEMP,
	RT_STOP_RISE_FAST = SK_ESB_HEATED_TCAL_STOP_RISE_FAST,
	RT_STOP_HEATER_ERROR = SK_ESB_HEATED_TCAL_STOP_HEATER_ERROR,
	RT_STOP_START_FAILED = SK_ESB_HEATED_TCAL_STOP_START_FAILED,
};

enum rt_decode {
	RT_DECODE_IGNORE,
	RT_DECODE_NORMAL,
	RT_DECODE_VALID,
	RT_DECODE_INVALID,
};

enum rt_state {
	RT_STATE_IDLE,
	RT_STATE_WAIT_START,
	RT_STATE_SENSOR_EXEC,
	RT_STATE_RESULT_READY,
};

enum rt_event {
	RT_EVENT_NONE,
	RT_EVENT_SENSOR_SUBMIT,
	RT_EVENT_RESULT_READY,
};

enum rt_legacy_decision {
	RT_LEGACY_ACCEPT,
	RT_LEGACY_DEFER,
};

enum rt_safety_preempt_mode {
	RT_SAFETY_PREEMPT_NONE,
	RT_SAFETY_PREEMPT_LEGACY,
	RT_SAFETY_PREEMPT_PAIRING,
};

struct rt_payload {
	uint16_t transaction_id;
	uint8_t action;
	int16_t target_centi_c;
};

struct rt_model {
	enum rt_state state;
	struct rt_payload payload;
	uint8_t matching_count;
	uint32_t first_rx_ms;
	uint32_t last_match_ms;
	uint8_t result;
	uint8_t result_status;
	uint32_t execution_count;
	bool result_override_pending;
	struct rt_payload result_override_payload;
	uint8_t result_override;

	bool cached_valid;
	struct rt_payload cached_payload;
	uint8_t cached_result;
	uint8_t cached_result_status;
};

struct rt_alias_model {
	uint32_t token[RT_ALIAS_CAPACITY];
};

struct rt_accumulator_model {
	uint32_t sample_count;
	uint32_t committed_samples;
};

struct rt_safety_model {
	enum rt_state state;
	uint32_t generation;
	struct rt_payload payload;
	bool cached_valid;
	uint8_t cached_result;
	enum rt_safety_preempt_mode preempt_mode;
	bool heater_active;
	bool pwm_safe;
	uint32_t sensor_token;
	uint32_t safety_token;
	uint32_t next_token;
	uint32_t abandoned_token_count;
	uint32_t abort_submit_count;
	uint32_t order;
	uint32_t pwm_safe_order;
	uint32_t legacy_execute_order;
};

struct rt_recovery_snapshot {
	enum rt_state state;
	uint32_t generation;
	uint32_t sensor_token;
};

struct rt_mailbox_alias_model {
	struct rt_alias_model aliases;
	uint32_t main_token;
	uint32_t deferred_token;
	bool main_detached;
	bool deferred_detached;
};

struct rt_mutation_owner_model {
	bool heated_owner;
	bool mutation_owner;
	bool auto_enabled;
	float sensitivity_scale;
};

uint8_t rt_crc(const uint8_t *data, size_t len);
uint8_t rt_status(bool supported, bool active, bool sampling, enum rt_stop_reason reason);

void rt_encode_command_pong(
	uint8_t out[RT_PACKET_LEN],
	uint8_t tracker_id,
	uint8_t counter,
	const struct rt_payload *payload
);
enum rt_decode rt_decode_command_pong(const uint8_t packet[RT_PACKET_LEN], struct rt_payload *payload);

void rt_encode_ping(
	const struct rt_model *model,
	uint8_t out[RT_PACKET_LEN],
	uint8_t tracker_id,
	uint8_t counter,
	uint32_t sync_ticks,
	uint8_t status
);
enum rt_decode rt_decode_result_ping(
	const uint8_t packet[RT_PACKET_LEN],
	uint16_t expected_transaction_id,
	uint8_t *result,
	uint8_t *status
);

void rt_model_init(struct rt_model *model);
enum rt_event rt_model_receive(struct rt_model *model, const struct rt_payload *payload, uint32_t now_ms);
enum rt_event rt_model_process(struct rt_model *model, uint32_t now_ms);
enum rt_event rt_model_sensor_complete(struct rt_model *model, uint8_t result);
enum rt_event rt_model_sensor_complete_with_status(
	struct rt_model *model,
	uint8_t result,
	uint8_t status
);
void rt_model_normal_pong(struct rt_model *model);

bool rt_alias_reserve(struct rt_alias_model *model, uint32_t token);
void rt_alias_release(struct rt_alias_model *model, uint32_t token);
size_t rt_alias_count(const struct rt_alias_model *model);
void rt_accumulator_add(struct rt_accumulator_model *model, uint32_t sample_count);
void rt_accumulator_stop_commit(struct rt_accumulator_model *model, uint32_t minimum_samples);

void rt_safety_model_init(struct rt_safety_model *model);
enum rt_legacy_decision rt_safety_critical_legacy(struct rt_safety_model *model);
void rt_safety_pairing_reset(struct rt_safety_model *model, bool abort_submit_accepted);
void rt_safety_retry_abort(struct rt_safety_model *model, bool accepted);
void rt_safety_abort_complete(struct rt_safety_model *model, bool physical_safe);
bool rt_safety_execute_legacy(struct rt_safety_model *model);
struct rt_recovery_snapshot rt_safety_snapshot(const struct rt_safety_model *model);
bool rt_safety_publish_recovery(
	struct rt_safety_model *model,
	const struct rt_recovery_snapshot *snapshot,
	uint32_t replacement_token
);

bool rt_mailbox_accept_deferred_safety(
	struct rt_mailbox_alias_model *model,
	uint32_t safety_token
);
void rt_mailbox_transport_abandon(
	struct rt_mailbox_alias_model *model,
	uint32_t token
);
void rt_mailbox_complete_safety(struct rt_mailbox_alias_model *model);

bool rt_external_sensitivity_write(
	struct rt_mutation_owner_model *model,
	float sensitivity_scale
);
bool rt_external_auto_write(
	struct rt_mutation_owner_model *model,
	bool enabled
);
void rt_internal_heated_auto_write(
	struct rt_mutation_owner_model *model,
	bool enabled
);

uint16_t rt_next_nonzero_transaction(uint16_t current);

#endif /* REMOTE_HEATED_TCAL_MODEL_H */
