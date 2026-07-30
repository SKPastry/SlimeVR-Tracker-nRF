/* SPDX-License-Identifier: MIT */

#include "remote_heated_tcal_model.h"

#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>

static bool payload_equal(const struct rt_payload *a, const struct rt_payload *b)
{
	return a->transaction_id == b->transaction_id && a->action == b->action && a->target_centi_c == b->target_centi_c;
}

static bool payload_valid(const struct rt_payload *payload)
{
	bool action_valid
		= payload->action == RT_ACTION_START || payload->action == RT_ACTION_STOP || payload->action == RT_ACTION_ABORT;

	return payload->transaction_id != 0U && action_valid
		&& (payload->action == RT_ACTION_START || payload->target_centi_c == 0);
}

static void cache_result(
	struct rt_model *model,
	const struct rt_payload *payload,
	uint8_t result,
	uint8_t status
)
{
	model->state = RT_STATE_RESULT_READY;
	model->payload = *payload;
	model->result = result;
	model->result_status = status;
	model->cached_valid = true;
	model->cached_payload = *payload;
	model->cached_result = result;
	model->cached_result_status = status;
	model->result_override_pending = false;
}

static enum rt_event submit_sensor(struct rt_model *model, const struct rt_payload *payload)
{
	model->state = RT_STATE_SENSOR_EXEC;
	model->payload = *payload;
	model->execution_count++;
	return RT_EVENT_SENSOR_SUBMIT;
}

uint8_t rt_crc(const uint8_t *data, size_t len)
{
	return crc8_ccitt(0x07, data, len);
}

uint8_t rt_status(bool supported, bool active, bool sampling, enum rt_stop_reason reason)
{
	return (supported ? 0x01U : 0U) |
	       (active ? 0x02U : 0U) |
	       (active && sampling ? 0x04U : 0U) |
	       (((uint8_t)reason & 0x1fU) << 3);
}

void rt_encode_command_pong(
	uint8_t out[RT_PACKET_LEN],
	uint8_t tracker_id,
	uint8_t counter,
	const struct rt_payload *payload
)
{
	memset(out, 0, RT_PACKET_LEN);
	out[SK_ESB_EXT_PACKET_TYPE_OFFSET] = RT_PONG_TYPE;
	out[SK_ESB_EXT_TRACKER_ID_OFFSET] = tracker_id;
	out[SK_ESB_EXT_COUNTER_OFFSET] = counter;
	out[SK_ESB_EXT_PONG_MAGIC_0_OFFSET] = RT_MAGIC_0;
	out[SK_ESB_EXT_PONG_MAGIC_1_OFFSET] = RT_MAGIC_1;
	out[SK_ESB_EXT_PONG_VERSION_OFFSET] = RT_VERSION;
	out[SK_ESB_EXT_PONG_ACTION_OFFSET] = payload->action;
	out[SK_ESB_EXT_FLAG_OFFSET] = RT_ESCAPE;
	sys_put_be16(
		payload->transaction_id,
		&out[SK_ESB_EXT_TRANSACTION_OFFSET]);
	sys_put_be16(
		(uint16_t)payload->target_centi_c,
		&out[SK_ESB_EXT_PONG_TARGET_OFFSET]);
	out[SK_ESB_EXT_CRC_OFFSET] = rt_crc(out, RT_PACKET_LEN - 1U);
}

enum rt_decode rt_decode_command_pong(const uint8_t packet[RT_PACKET_LEN], struct rt_payload *payload)
{
	if (packet[SK_ESB_EXT_PACKET_TYPE_OFFSET] != RT_PONG_TYPE ||
	    packet[SK_ESB_EXT_CRC_OFFSET] != rt_crc(packet, RT_PACKET_LEN - 1U)) {
		return RT_DECODE_IGNORE;
	}
	if (packet[SK_ESB_EXT_FLAG_OFFSET] == RT_NORMAL_FLAG) {
		return RT_DECODE_NORMAL;
	}
	if (packet[SK_ESB_EXT_FLAG_OFFSET] != RT_ESCAPE ||
	    packet[SK_ESB_EXT_PONG_MAGIC_0_OFFSET] != RT_MAGIC_0 ||
	    packet[SK_ESB_EXT_PONG_MAGIC_1_OFFSET] != RT_MAGIC_1 ||
	    packet[SK_ESB_EXT_PONG_VERSION_OFFSET] != RT_VERSION) {
		return RT_DECODE_IGNORE;
	}

	payload->transaction_id =
		sys_get_be16(&packet[SK_ESB_EXT_TRANSACTION_OFFSET]);
	payload->action = packet[SK_ESB_EXT_PONG_ACTION_OFFSET];
	payload->target_centi_c =
		(int16_t)sys_get_be16(&packet[SK_ESB_EXT_PONG_TARGET_OFFSET]);

	return payload_valid(payload) ? RT_DECODE_VALID : RT_DECODE_INVALID;
}

void rt_encode_ping(
	const struct rt_model *model,
	uint8_t out[RT_PACKET_LEN],
	uint8_t tracker_id,
	uint8_t counter,
	uint32_t sync_ticks,
	uint8_t status
)
{
	memset(out, 0, RT_PACKET_LEN);
	out[SK_ESB_EXT_PACKET_TYPE_OFFSET] = RT_PING_TYPE;
	out[SK_ESB_EXT_TRACKER_ID_OFFSET] = tracker_id;
	out[SK_ESB_EXT_COUNTER_OFFSET] = counter;
	sys_put_be32(sync_ticks, &out[SK_ESB_EXT_PING_TIME_SYNC_OFFSET]);

	if (model->state == RT_STATE_RESULT_READY) {
		out[SK_ESB_EXT_FLAG_OFFSET] = RT_ESCAPE;
		sys_put_be16(
			model->payload.transaction_id,
			&out[SK_ESB_EXT_TRANSACTION_OFFSET]);
		out[SK_ESB_EXT_PING_RESULT_OFFSET] =
			RT_RESULT_MARKER | (model->result & 0x0fU);
		out[SK_ESB_EXT_PING_STATUS_OFFSET] = model->result_status;
	} else {
		out[SK_ESB_EXT_FLAG_OFFSET] = RT_NORMAL_FLAG;
		out[SK_ESB_EXT_PING_MAGIC_0_OFFSET] = RT_MAGIC_0;
		out[SK_ESB_EXT_PING_MAGIC_1_OFFSET] = RT_MAGIC_1;
		out[SK_ESB_EXT_PING_VERSION_OFFSET] = RT_VERSION;
		out[SK_ESB_EXT_PING_STATUS_OFFSET] = status;
	}
	out[SK_ESB_EXT_CRC_OFFSET] = rt_crc(out, RT_PACKET_LEN - 1U);
}

enum rt_decode rt_decode_result_ping(
	const uint8_t packet[RT_PACKET_LEN],
	uint16_t expected_transaction_id,
	uint8_t *result,
	uint8_t *status
)
{
	if (packet[SK_ESB_EXT_PACKET_TYPE_OFFSET] != RT_PING_TYPE ||
	    packet[SK_ESB_EXT_CRC_OFFSET] != rt_crc(packet, RT_PACKET_LEN - 1U)) {
		return RT_DECODE_IGNORE;
	}
	if (packet[SK_ESB_EXT_FLAG_OFFSET] != RT_ESCAPE ||
	    sys_get_be16(&packet[SK_ESB_EXT_TRANSACTION_OFFSET]) !=
		    expected_transaction_id ||
	    (packet[SK_ESB_EXT_PING_RESULT_OFFSET] & 0xf0U) !=
		    RT_RESULT_MARKER ||
	    (packet[SK_ESB_EXT_PING_RESULT_OFFSET] & 0x0fU) >
		    RT_RESULT_CANCELED) {
		return RT_DECODE_IGNORE;
	}

	*result = packet[SK_ESB_EXT_PING_RESULT_OFFSET] & 0x0fU;
	*status = packet[SK_ESB_EXT_PING_STATUS_OFFSET];
	return RT_DECODE_VALID;
}

void rt_model_init(struct rt_model *model)
{
	memset(model, 0, sizeof(*model));
	model->state = RT_STATE_IDLE;
}

enum rt_event rt_model_process(struct rt_model *model, uint32_t now_ms)
{
	if (model->state != RT_STATE_WAIT_START) {
		return RT_EVENT_NONE;
	}

	uint32_t since_first = now_ms - model->first_rx_ms;
	uint32_t since_match = now_ms - model->last_match_ms;

	if (since_first >= RT_WAIT_MAX_MS) {
		cache_result(model, &model->payload, RT_RESULT_TIMEOUT, 0);
		return RT_EVENT_RESULT_READY;
	}
	if (model->matching_count >= 2U && since_first >= RT_START_CONFIRM_DELAY_MS && since_match <= RT_MATCH_FRESH_MS) {
		return submit_sensor(model, &model->payload);
	}
	return RT_EVENT_NONE;
}

enum rt_event rt_model_receive(struct rt_model *model, const struct rt_payload *payload, uint32_t now_ms)
{
	if (!payload_valid(payload)) {
		if (model->state == RT_STATE_IDLE && payload->transaction_id != 0U) {
			cache_result(model, payload, RT_RESULT_INVALID, 0);
			return RT_EVENT_RESULT_READY;
		}
		if (payload->transaction_id == 0U ||
		    payload->transaction_id != model->payload.transaction_id) {
			return RT_EVENT_NONE;
		}
		if (model->state == RT_STATE_SENSOR_EXEC) {
			model->result_override_pending = true;
			model->result_override_payload = *payload;
			model->result_override = RT_RESULT_INVALID;
			return RT_EVENT_NONE;
		}
		cache_result(model, payload, RT_RESULT_INVALID, 0);
		return RT_EVENT_RESULT_READY;
	}

	switch (model->state) {
	case RT_STATE_IDLE:
		if (model->cached_valid && payload->transaction_id == model->cached_payload.transaction_id) {
			if (payload_equal(payload, &model->cached_payload)) {
				cache_result(
					model,
					payload,
					model->cached_result,
					model->cached_result_status);
			} else {
				cache_result(model, payload, RT_RESULT_INVALID, 0);
			}
			return RT_EVENT_RESULT_READY;
		}
		if (payload->action == RT_ACTION_START) {
			model->state = RT_STATE_WAIT_START;
			model->payload = *payload;
			model->matching_count = 1U;
			model->first_rx_ms = now_ms;
			model->last_match_ms = now_ms;
			return RT_EVENT_NONE;
		}
		return submit_sensor(model, payload);

	case RT_STATE_WAIT_START:
		if (payload_equal(payload, &model->payload)) {
			if (model->matching_count != UINT8_MAX) {
				model->matching_count++;
			}
			model->last_match_ms = now_ms;
			return rt_model_process(model, now_ms);
		}
		if (payload->transaction_id == model->payload.transaction_id) {
			cache_result(model, payload, RT_RESULT_INVALID, 0);
			return RT_EVENT_RESULT_READY;
		}
		if (payload->action == RT_ACTION_STOP || payload->action == RT_ACTION_ABORT) {
			return submit_sensor(model, payload);
		}
		cache_result(model, payload, RT_RESULT_BUSY, 0);
		return RT_EVENT_RESULT_READY;

	case RT_STATE_SENSOR_EXEC:
		if (payload_equal(payload, &model->payload)) {
			return RT_EVENT_NONE;
		}
		if (payload->transaction_id == model->payload.transaction_id) {
			model->result_override_pending = true;
			model->result_override_payload = *payload;
			model->result_override = RT_RESULT_INVALID;
			return RT_EVENT_NONE;
		}
		return RT_EVENT_NONE;

	case RT_STATE_RESULT_READY:
		if (payload_equal(payload, &model->payload)) {
			return RT_EVENT_NONE;
		}
		if (payload->transaction_id == model->payload.transaction_id) {
			cache_result(model, payload, RT_RESULT_INVALID, 0);
			return RT_EVENT_RESULT_READY;
		}
		if (payload->action == RT_ACTION_STOP ||
		    payload->action == RT_ACTION_ABORT) {
			return submit_sensor(model, payload);
		}
		return RT_EVENT_NONE;
	}

	return RT_EVENT_NONE;
}

enum rt_event rt_model_sensor_complete(struct rt_model *model, uint8_t result)
{
	return rt_model_sensor_complete_with_status(model, result, 0);
}

enum rt_event rt_model_sensor_complete_with_status(
	struct rt_model *model,
	uint8_t result,
	uint8_t status
)
{
	if (model->state != RT_STATE_SENSOR_EXEC || result > RT_RESULT_CANCELED) {
		return RT_EVENT_NONE;
	}
	if (result == RT_RESULT_OK && model->result_override_pending) {
		cache_result(
			model,
			&model->result_override_payload,
			model->result_override,
			status);
	} else {
		cache_result(model, &model->payload, result, status);
	}
	return RT_EVENT_RESULT_READY;
}

void rt_model_normal_pong(struct rt_model *model)
{
	if (model->state == RT_STATE_RESULT_READY) {
		model->state = RT_STATE_IDLE;
	} else if (model->state == RT_STATE_WAIT_START) {
		model->cached_valid = true;
		model->cached_payload = model->payload;
		model->cached_result = RT_RESULT_CANCELED;
		model->cached_result_status = 0;
		model->state = RT_STATE_IDLE;
	}
}

bool rt_alias_reserve(struct rt_alias_model *model, uint32_t token)
{
	if (token == 0U) {
		return true;
	}

	size_t free_slot = RT_ALIAS_CAPACITY;
	for (size_t i = 0; i < RT_ALIAS_CAPACITY; i++) {
		if (model->token[i] == token) {
			return true;
		}
		if (model->token[i] == 0U && free_slot == RT_ALIAS_CAPACITY) {
			free_slot = i;
		}
	}
	if (free_slot == RT_ALIAS_CAPACITY) {
		return false;
	}
	model->token[free_slot] = token;
	return true;
}

void rt_alias_release(struct rt_alias_model *model, uint32_t token)
{
	for (size_t i = 0; i < RT_ALIAS_CAPACITY; i++) {
		if (model->token[i] == token) {
			model->token[i] = 0;
			return;
		}
	}
}

size_t rt_alias_count(const struct rt_alias_model *model)
{
	size_t count = 0;

	for (size_t i = 0; i < RT_ALIAS_CAPACITY; i++) {
		if (model->token[i] != 0U) {
			count++;
		}
	}
	return count;
}

void rt_accumulator_add(struct rt_accumulator_model *model, uint32_t sample_count)
{
	model->sample_count += sample_count;
}

void rt_accumulator_stop_commit(struct rt_accumulator_model *model, uint32_t minimum_samples)
{
	if (model->sample_count >= minimum_samples) {
		model->committed_samples += model->sample_count;
	}
	model->sample_count = 0;
}

void rt_safety_model_init(struct rt_safety_model *model)
{
	memset(model, 0, sizeof(*model));
	model->state = RT_STATE_IDLE;
	model->generation = 1;
	model->next_token = 100;
	model->pwm_safe = true;
}

static void safety_submit_abort(struct rt_safety_model *model, bool accepted)
{
	model->abort_submit_count++;
	if (accepted) {
		model->next_token++;
		model->safety_token = model->next_token;
		model->state = RT_STATE_SENSOR_EXEC;
	}
}

enum rt_legacy_decision rt_safety_critical_legacy(struct rt_safety_model *model)
{
	if (model->preempt_mode != RT_SAFETY_PREEMPT_NONE) {
		return RT_LEGACY_DEFER;
	}
	if (model->state == RT_STATE_WAIT_START) {
		model->cached_valid = true;
		model->cached_result = RT_RESULT_CANCELED;
		model->state = RT_STATE_IDLE;
		model->sensor_token = 0;
		return RT_LEGACY_ACCEPT;
	}
	if (model->state == RT_STATE_IDLE && !model->heater_active) {
		return RT_LEGACY_ACCEPT;
	}
	if (model->state == RT_STATE_RESULT_READY &&
	    model->payload.action != RT_ACTION_START &&
	    !model->heater_active) {
		model->cached_valid = true;
		model->cached_result = RT_RESULT_CANCELED;
		model->state = RT_STATE_IDLE;
		return RT_LEGACY_ACCEPT;
	}

	model->preempt_mode = RT_SAFETY_PREEMPT_LEGACY;
	if (model->sensor_token != 0U) {
		model->sensor_token = 0;
		model->abandoned_token_count++;
	}
	safety_submit_abort(model, true);
	return RT_LEGACY_DEFER;
}

void rt_safety_pairing_reset(struct rt_safety_model *model, bool abort_submit_accepted)
{
	model->generation++;
	model->cached_valid = false;
	model->preempt_mode = RT_SAFETY_PREEMPT_PAIRING;
	if (model->sensor_token != 0U) {
		model->sensor_token = 0;
		model->abandoned_token_count++;
	}
	model->safety_token = 0;
	safety_submit_abort(model, abort_submit_accepted);
}

void rt_safety_retry_abort(struct rt_safety_model *model, bool accepted)
{
	if (model->preempt_mode != RT_SAFETY_PREEMPT_NONE &&
	    model->safety_token == 0U) {
		safety_submit_abort(model, accepted);
	}
}

void rt_safety_abort_complete(struct rt_safety_model *model, bool physical_safe)
{
	if (model->preempt_mode == RT_SAFETY_PREEMPT_NONE ||
	    model->safety_token == 0U) {
		return;
	}
	model->safety_token = 0;
	if (!physical_safe) {
		return;
	}

	model->heater_active = false;
	model->pwm_safe = true;
	model->pwm_safe_order = ++model->order;
	if (model->preempt_mode == RT_SAFETY_PREEMPT_LEGACY) {
		model->cached_valid = true;
		model->cached_result = RT_RESULT_CANCELED;
	} else {
		model->cached_valid = false;
	}
	model->preempt_mode = RT_SAFETY_PREEMPT_NONE;
	model->state = RT_STATE_IDLE;
}

bool rt_safety_execute_legacy(struct rt_safety_model *model)
{
	if (model->state != RT_STATE_IDLE ||
	    model->preempt_mode != RT_SAFETY_PREEMPT_NONE ||
	    !model->pwm_safe) {
		return false;
	}
	model->legacy_execute_order = ++model->order;
	return true;
}

struct rt_recovery_snapshot rt_safety_snapshot(const struct rt_safety_model *model)
{
	return (struct rt_recovery_snapshot){
		.state = model->state,
		.generation = model->generation,
		.sensor_token = model->sensor_token,
	};
}

bool rt_safety_publish_recovery(
	struct rt_safety_model *model,
	const struct rt_recovery_snapshot *snapshot,
	uint32_t replacement_token
)
{
	if (model->generation != snapshot->generation ||
	    model->state != snapshot->state ||
	    model->sensor_token != snapshot->sensor_token ||
	    model->preempt_mode != RT_SAFETY_PREEMPT_NONE) {
		return false;
	}
	model->sensor_token = replacement_token;
	model->state = RT_STATE_SENSOR_EXEC;
	return true;
}

bool rt_mailbox_accept_deferred_safety(
	struct rt_mailbox_alias_model *model,
	uint32_t safety_token
)
{
	if (model->main_token == 0U || model->deferred_token != 0U ||
	    !rt_alias_reserve(&model->aliases, model->main_token)) {
		return false;
	}
	model->deferred_token = safety_token;
	model->deferred_detached = false;
	return true;
}

void rt_mailbox_transport_abandon(
	struct rt_mailbox_alias_model *model,
	uint32_t token
)
{
	if (model->main_token == token) {
		model->main_detached = true;
	}
	if (model->deferred_token == token) {
		model->deferred_detached = true;
	}
	/* Alias cleanup is unconditional even when main/deferred also matched. */
	rt_alias_release(&model->aliases, token);
}

void rt_mailbox_complete_safety(struct rt_mailbox_alias_model *model)
{
	model->main_token = 0;
	model->deferred_token = 0;
	model->main_detached = false;
	model->deferred_detached = false;
}

static bool rt_external_mutation_claim(
	struct rt_mutation_owner_model *model)
{
	if (model->heated_owner || model->mutation_owner) {
		return false;
	}
	model->mutation_owner = true;
	return true;
}

bool rt_external_sensitivity_write(
	struct rt_mutation_owner_model *model,
	float sensitivity_scale)
{
	if (!rt_external_mutation_claim(model)) {
		return false;
	}
	model->sensitivity_scale = sensitivity_scale;
	model->mutation_owner = false;
	return true;
}

bool rt_external_auto_write(
	struct rt_mutation_owner_model *model,
	bool enabled)
{
	if (!rt_external_mutation_claim(model)) {
		return false;
	}
	model->auto_enabled = enabled;
	model->mutation_owner = false;
	return true;
}

void rt_internal_heated_auto_write(
	struct rt_mutation_owner_model *model,
	bool enabled)
{
	/* Heated start/stop already owns calibration and must not self-deadlock. */
	model->auto_enabled = enabled;
}

uint16_t rt_next_nonzero_transaction(uint16_t current)
{
	current++;
	return current == 0U ? 1U : current;
}
