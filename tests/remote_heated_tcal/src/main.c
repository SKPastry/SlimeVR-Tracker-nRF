/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/ztest.h>

#include "remote_heated_tcal_model.h"

BUILD_ASSERT(RT_PACKET_LEN == 13U);
BUILD_ASSERT(SK_ESB_EXT_CRC_OFFSET == RT_PACKET_LEN - 1U);
BUILD_ASSERT(SK_ESB_EXT_PING_STATUS_OFFSET < SK_ESB_EXT_CRC_OFFSET);
BUILD_ASSERT(SK_ESB_EXT_PONG_TARGET_OFFSET + sizeof(uint16_t) ==
	     SK_ESB_EXT_CRC_OFFSET);

static void refresh_crc(uint8_t packet[RT_PACKET_LEN])
{
	packet[SK_ESB_EXT_CRC_OFFSET] =
		rt_crc(packet, SK_ESB_EXT_CRC_OFFSET);
}

ZTEST(remote_heated_tcal_contract, test_production_wire_constants_and_offsets)
{
	/*
	 * These names come directly from src/connection/remote_tcal_protocol.h.
	 * Keeping the codec model on the production constants makes an accidental
	 * wire-layout edit fail here instead of silently testing a copied layout.
	 */
	zassert_equal(SK_ESB_REMOTE_TCAL_PACKET_LEN, 13);
	zassert_equal(SK_ESB_REMOTE_TCAL_PING_TYPE, 0xf0);
	zassert_equal(SK_ESB_REMOTE_TCAL_PONG_TYPE, 0xf1);
	zassert_equal(SK_ESB_REMOTE_TCAL_NORMAL_FLAG, 0);
	zassert_equal(SK_ESB_EXT_ESCAPE, 0xc8);
	zassert_equal(SK_ESB_EXT_MAGIC_0, 'S');
	zassert_equal(SK_ESB_EXT_MAGIC_1, 'K');
	zassert_equal(SK_ESB_EXT_VERSION, 1);

	zassert_equal(SK_ESB_EXT_PACKET_TYPE_OFFSET, 0);
	zassert_equal(SK_ESB_EXT_TRACKER_ID_OFFSET, 1);
	zassert_equal(SK_ESB_EXT_COUNTER_OFFSET, 2);
	zassert_equal(SK_ESB_EXT_PING_TIME_SYNC_OFFSET, 3);
	zassert_equal(SK_ESB_EXT_PONG_MAGIC_0_OFFSET, 3);
	zassert_equal(SK_ESB_EXT_PONG_MAGIC_1_OFFSET, 4);
	zassert_equal(SK_ESB_EXT_PONG_VERSION_OFFSET, 5);
	zassert_equal(SK_ESB_EXT_PONG_ACTION_OFFSET, 6);
	zassert_equal(SK_ESB_EXT_FLAG_OFFSET, 7);
	zassert_equal(SK_ESB_EXT_TRANSACTION_OFFSET, 8);
	zassert_equal(SK_ESB_EXT_PING_MAGIC_0_OFFSET, 8);
	zassert_equal(SK_ESB_EXT_PING_MAGIC_1_OFFSET, 9);
	zassert_equal(SK_ESB_EXT_PING_VERSION_OFFSET, 10);
	zassert_equal(SK_ESB_EXT_PONG_TARGET_OFFSET, 10);
	zassert_equal(SK_ESB_EXT_PING_RESULT_OFFSET, 10);
	zassert_equal(SK_ESB_EXT_PING_STATUS_OFFSET, 11);
	zassert_equal(SK_ESB_EXT_CRC_OFFSET, 12);

	zassert_equal(SK_ESB_HEATED_TCAL_START, 1);
	zassert_equal(SK_ESB_HEATED_TCAL_STOP, 2);
	zassert_equal(SK_ESB_HEATED_TCAL_ABORT, 3);
	zassert_equal(SK_ESB_HEATED_TCAL_OK, 0);
	zassert_equal(SK_ESB_HEATED_TCAL_CANCELED, 10);
	zassert_equal(SK_ESB_HEATED_TCAL_RESULT_MARKER, 0xa0);
	zassert_equal(SK_ESB_HEATED_TCAL_DEFAULT_TARGET, INT16_MIN);
}

ZTEST(remote_heated_tcal_contract, test_golden_start_pong_and_big_endian)
{
	const struct rt_payload command = {
		.transaction_id = 0x1234,
		.action = RT_ACTION_START,
		.target_centi_c = 2500,
	};
	const uint8_t expected[RT_PACKET_LEN] = {
		0xf1,
		0x05,
		0x7a,
		0x53,
		0x4b,
		0x01,
		0x01,
		0xc8,
		0x12,
		0x34,
		0x09,
		0xc4,
		0xca,
	};
	uint8_t encoded[RT_PACKET_LEN];
	struct rt_payload decoded = {0};

	rt_encode_command_pong(encoded, 5, 0x7a, &command);
	zassert_mem_equal(encoded, expected, sizeof(expected));
	zassert_equal(rt_decode_command_pong(encoded, &decoded), RT_DECODE_VALID);
	zassert_equal(decoded.transaction_id, command.transaction_id);
	zassert_equal(decoded.action, command.action);
	zassert_equal(decoded.target_centi_c, command.target_centi_c);
}

ZTEST(remote_heated_tcal_contract, test_default_and_signed_temperature_encoding)
{
	struct rt_payload command = {
		.transaction_id = 0x1234,
		.action = RT_ACTION_START,
		.target_centi_c = RT_DEFAULT_TARGET,
	};
	uint8_t packet[RT_PACKET_LEN];
	struct rt_payload decoded = {0};

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	zassert_equal(packet[10], 0x80);
	zassert_equal(packet[11], 0x00);
	zassert_equal(packet[12], 0x93);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_VALID);
	zassert_equal(decoded.target_centi_c, INT16_MIN);

	command.target_centi_c = -1234;
	rt_encode_command_pong(packet, 5, 0x7a, &command);
	zassert_equal(packet[10], 0xfb);
	zassert_equal(packet[11], 0x2e);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_VALID);
	zassert_equal(decoded.target_centi_c, -1234);
}

ZTEST(remote_heated_tcal_contract, test_magic_version_crc_and_shape_rejection)
{
	struct rt_payload command = {
		.transaction_id = 0x1234,
		.action = RT_ACTION_START,
		.target_centi_c = 2500,
	};
	uint8_t packet[RT_PACKET_LEN];
	struct rt_payload decoded;

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	packet[3] = 'X';
	refresh_crc(packet);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_IGNORE);

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	packet[5] = RT_VERSION + 1U;
	refresh_crc(packet);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_IGNORE);

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	packet[12] ^= 0x01;
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_IGNORE);

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	packet[8] = 0;
	packet[9] = 0;
	refresh_crc(packet);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_INVALID);

	rt_encode_command_pong(packet, 5, 0x7a, &command);
	packet[6] = RT_ACTION_STOP;
	refresh_crc(packet);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_INVALID);

	command.action = RT_ACTION_STOP;
	command.target_centi_c = 0;
	rt_encode_command_pong(packet, 5, 0x7a, &command);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_VALID);

	command.action = RT_ACTION_ABORT;
	rt_encode_command_pong(packet, 5, 0x7a, &command);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_VALID);
}

ZTEST(remote_heated_tcal_contract, test_golden_result_ping_and_marker_validation)
{
	struct rt_model model;
	uint8_t packet[RT_PACKET_LEN];
	uint8_t result;
	uint8_t status;
	const uint8_t expected[RT_PACKET_LEN] = {
		0xf0,
		0x05,
		0x7a,
		0x11,
		0x22,
		0x33,
		0x44,
		0xc8,
		0x12,
		0x34,
		0xa6,
		0x17,
		0x9a,
	};

	rt_model_init(&model);
	model.state = RT_STATE_RESULT_READY;
	model.payload.transaction_id = 0x1234;
	model.result = RT_RESULT_TIMEOUT;
	model.result_status = 0x17;
	rt_encode_ping(&model, packet, 5, 0x7a, 0x11223344, 0x17);

	zassert_mem_equal(packet, expected, sizeof(expected));
	zassert_equal(rt_decode_result_ping(packet, 0x1234, &result, &status), RT_DECODE_VALID);
	zassert_equal(result, RT_RESULT_TIMEOUT);
	zassert_equal(status, 0x17);

	/* A legacy Tracker echoing only 0xc8 must not look like a result. */
	packet[10] = 0;
	refresh_crc(packet);
	zassert_equal(rt_decode_result_ping(packet, 0x1234, &result, &status), RT_DECODE_IGNORE);

	memcpy(packet, expected, sizeof(packet));
	zassert_equal(rt_decode_result_ping(packet, 0x1235, &result, &status), RT_DECODE_IGNORE);

	packet[10] = RT_RESULT_MARKER | 0x0b;
	refresh_crc(packet);
	zassert_equal(rt_decode_result_ping(packet, 0x1234, &result, &status), RT_DECODE_IGNORE);
}

ZTEST(remote_heated_tcal_contract, test_result_status_is_latched_until_ack_and_on_replay)
{
	const struct rt_payload stop = {
		.transaction_id = 0x2345,
		.action = RT_ACTION_STOP,
		.target_centi_c = 0,
	};
	struct rt_model model;
	uint8_t packet[RT_PACKET_LEN];

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &stop, 0), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(
		rt_model_sensor_complete_with_status(
			&model, RT_RESULT_OK, 0x4f),
		RT_EVENT_RESULT_READY);

	rt_encode_ping(&model, packet, 5, 1, 0x11223344, 0x01);
	zassert_equal(packet[11], 0x4f);
	rt_encode_ping(&model, packet, 5, 2, 0x11223345, 0x51);
	zassert_equal(packet[11], 0x4f);

	rt_model_normal_pong(&model);
	zassert_equal(
		rt_model_receive(&model, &stop, 10),
		RT_EVENT_RESULT_READY);
	rt_encode_ping(&model, packet, 5, 3, 0x11223346, 0x01);
	zassert_equal(packet[11], 0x4f);
}

ZTEST(remote_heated_tcal_contract, test_stable_wire_stop_reason_encoding)
{
	zassert_equal(RT_STOP_NONE, 0);
	zassert_equal(RT_STOP_COMPLETE, 1);
	zassert_equal(RT_STOP_USER, 2);
	zassert_equal(RT_STOP_TIMEOUT, 3);
	zassert_equal(RT_STOP_POWER_LOST, 4);
	zassert_equal(RT_STOP_IMU_POWER_OFF, 5);
	zassert_equal(RT_STOP_TEMP_STALE, 6);
	zassert_equal(RT_STOP_OVERTEMP, 7);
	zassert_equal(RT_STOP_RISE_FAST, 8);
	zassert_equal(RT_STOP_HEATER_ERROR, 9);
	zassert_equal(RT_STOP_START_FAILED, 10);

	zassert_equal(rt_status(true, true, true, RT_STOP_HEATER_ERROR), 0x4f);
	/* Sampling cannot be asserted on wire while inactive. */
	zassert_equal(rt_status(true, false, true, RT_STOP_START_FAILED), 0x51);
}

ZTEST(remote_heated_tcal_contract, test_start_requires_two_confirmations_and_delay)
{
	const struct rt_payload start = {
		.transaction_id = 7,
		.action = RT_ACTION_START,
		.target_centi_c = RT_DEFAULT_TARGET,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 1000), RT_EVENT_NONE);
	zassert_equal(model.state, RT_STATE_WAIT_START);
	zassert_equal(model.execution_count, 0);

	zassert_equal(rt_model_receive(&model, &start, 2499), RT_EVENT_NONE);
	zassert_equal(model.matching_count, 2);
	zassert_equal(model.execution_count, 0);

	zassert_equal(rt_model_process(&model, 2500), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(model.state, RT_STATE_SENSOR_EXEC);
	zassert_equal(model.execution_count, 1);
}

ZTEST(remote_heated_tcal_contract, test_latest_confirmation_freshness_boundary)
{
	const struct rt_payload start = {
		.transaction_id = 8,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &start, 1), RT_EVENT_NONE);
	zassert_equal(rt_model_process(&model, 2001), RT_EVENT_SENSOR_SUBMIT);

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &start, 1), RT_EVENT_NONE);
	zassert_equal(rt_model_process(&model, 2002), RT_EVENT_NONE);
	zassert_equal(model.state, RT_STATE_WAIT_START);
	zassert_equal(model.execution_count, 0);
}

ZTEST(remote_heated_tcal_contract, test_early_result_is_forbidden)
{
	const struct rt_payload start = {
		.transaction_id = 9,
		.action = RT_ACTION_START,
		.target_centi_c = 2800,
	};
	struct rt_model model;
	uint8_t ping[RT_PACKET_LEN];
	const uint8_t expected_capability[RT_PACKET_LEN] = {
		0xf0,
		0x05,
		0x7a,
		0x11,
		0x22,
		0x33,
		0x44,
		0x00,
		0x53,
		0x4b,
		0x01,
		0x17,
		0x02,
	};

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 1000), RT_EVENT_NONE);
	rt_encode_ping(&model, ping, 5, 0x7a, 0x11223344, 0x17);
	zassert_mem_equal(ping, expected_capability, sizeof(expected_capability));

	zassert_equal(rt_model_receive(&model, &start, 2500), RT_EVENT_SENSOR_SUBMIT);
	rt_encode_ping(&model, ping, 5, 0x7a, 0x11223344, 0x17);
	zassert_equal(ping[7], RT_NORMAL_FLAG);
	zassert_equal(ping[8], RT_MAGIC_0);
	zassert_equal(ping[10], RT_VERSION);

	zassert_equal(rt_model_sensor_complete(&model, RT_RESULT_OK), RT_EVENT_RESULT_READY);
	rt_encode_ping(&model, ping, 5, 0x7a, 0x11223344, 0x17);
	zassert_equal(ping[7], RT_ESCAPE);
	zassert_equal(ping[10], RT_RESULT_MARKER | RT_RESULT_OK);
}

ZTEST(remote_heated_tcal_contract, test_duplicate_and_completed_replay_do_not_execute)
{
	const struct rt_payload start = {
		.transaction_id = 10,
		.action = RT_ACTION_START,
		.target_centi_c = 3200,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &start, 1500), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(model.execution_count, 1);

	zassert_equal(rt_model_receive(&model, &start, 1600), RT_EVENT_NONE);
	zassert_equal(model.execution_count, 1);
	zassert_equal(rt_model_sensor_complete(&model, RT_RESULT_OK), RT_EVENT_RESULT_READY);
	zassert_equal(rt_model_receive(&model, &start, 1700), RT_EVENT_NONE);
	zassert_equal(model.execution_count, 1);

	rt_model_normal_pong(&model);
	zassert_equal(model.state, RT_STATE_IDLE);
	zassert_equal(rt_model_receive(&model, &start, 1800), RT_EVENT_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_OK);
	zassert_equal(model.execution_count, 1);
}

ZTEST(remote_heated_tcal_contract, test_same_transaction_different_payload_is_invalid)
{
	const struct rt_payload original = {
		.transaction_id = 11,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	const struct rt_payload conflict = {
		.transaction_id = 11,
		.action = RT_ACTION_START,
		.target_centi_c = 3100,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &original, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &conflict, 100), RT_EVENT_RESULT_READY);
	zassert_equal(model.state, RT_STATE_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_INVALID);
	zassert_equal(model.execution_count, 0);
}

ZTEST(remote_heated_tcal_contract, test_stop_or_abort_preempts_unexecuted_start)
{
	const struct rt_payload start = {
		.transaction_id = 12,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	const struct rt_payload abort = {
		.transaction_id = 13,
		.action = RT_ACTION_ABORT,
		.target_centi_c = 0,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &abort, 100), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(model.state, RT_STATE_SENSOR_EXEC);
	zassert_equal(model.payload.transaction_id, abort.transaction_id);
	zassert_equal(model.payload.action, RT_ACTION_ABORT);
	zassert_equal(model.execution_count, 1);
}

ZTEST(remote_heated_tcal_contract, test_malformed_packet_does_not_replace_live_safety_action)
{
	const struct rt_payload stop = {
		.transaction_id = 20,
		.action = RT_ACTION_STOP,
		.target_centi_c = 0,
	};
	const struct rt_payload unrelated_invalid = {
		.transaction_id = 21,
		.action = 0xff,
		.target_centi_c = 1,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &stop, 0), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(
		rt_model_receive(&model, &unrelated_invalid, 1),
		RT_EVENT_NONE);
	zassert_equal(model.state, RT_STATE_SENSOR_EXEC);
	zassert_equal(model.payload.transaction_id, stop.transaction_id);

	zassert_equal(
		rt_model_sensor_complete(&model, RT_RESULT_OK),
		RT_EVENT_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_OK);
	zassert_equal(model.payload.transaction_id, stop.transaction_id);
}

ZTEST(remote_heated_tcal_contract, test_same_transaction_mismatch_waits_for_safety_completion)
{
	const struct rt_payload stop = {
		.transaction_id = 22,
		.action = RT_ACTION_STOP,
		.target_centi_c = 0,
	};
	const struct rt_payload mismatch = {
		.transaction_id = 22,
		.action = RT_ACTION_STOP,
		.target_centi_c = 1,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &stop, 0), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(rt_model_receive(&model, &mismatch, 1), RT_EVENT_NONE);
	zassert_equal(model.state, RT_STATE_SENSOR_EXEC);
	zassert_true(model.result_override_pending);

	zassert_equal(
		rt_model_sensor_complete(&model, RT_RESULT_OK),
		RT_EVENT_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_INVALID);

	/* A physical force-off failure has priority over the deferred mismatch. */
	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &stop, 0), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(rt_model_receive(&model, &mismatch, 1), RT_EVENT_NONE);
	zassert_equal(
		rt_model_sensor_complete(&model, RT_RESULT_HARDWARE_ERROR),
		RT_EVENT_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_HARDWARE_ERROR);
}

ZTEST(remote_heated_tcal_contract, test_safety_command_supersedes_unacknowledged_start_result)
{
	const struct rt_payload start = {
		.transaction_id = 23,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	const struct rt_payload abort = {
		.transaction_id = 24,
		.action = RT_ACTION_ABORT,
		.target_centi_c = 0,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 0), RT_EVENT_NONE);
	zassert_equal(rt_model_receive(&model, &start, 1500), RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(
		rt_model_sensor_complete(&model, RT_RESULT_OK),
		RT_EVENT_RESULT_READY);

	/*
	 * Receiver may replace START before acknowledging tx 23 with NORMAL.  The
	 * new safety transaction must not be trapped behind that old result.
	 */
	zassert_equal(
		rt_model_receive(&model, &abort, 1600),
		RT_EVENT_SENSOR_SUBMIT);
	zassert_equal(model.state, RT_STATE_SENSOR_EXEC);
	zassert_equal(model.payload.transaction_id, abort.transaction_id);
	zassert_equal(model.execution_count, 2);
}

ZTEST(remote_heated_tcal_contract, test_wait_timeout_and_normal_result_handshake)
{
	const struct rt_payload start = {
		.transaction_id = 14,
		.action = RT_ACTION_START,
		.target_centi_c = 3500,
	};
	struct rt_model model;
	uint8_t ping[RT_PACKET_LEN];

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 100), RT_EVENT_NONE);
	zassert_equal(rt_model_process(&model, 4100), RT_EVENT_RESULT_READY);
	zassert_equal(model.state, RT_STATE_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_TIMEOUT);
	zassert_equal(model.execution_count, 0);

	rt_encode_ping(&model, ping, 5, 1, 0xaabbccdd, 1);
	zassert_equal(ping[7], RT_ESCAPE);
	zassert_equal(ping[10], RT_RESULT_MARKER | RT_RESULT_TIMEOUT);

	/* Result remains visible until an ordinary PONG acknowledges it. */
	rt_encode_ping(&model, ping, 5, 2, 0xaabbccde, 1);
	zassert_equal(ping[7], RT_ESCAPE);
	rt_model_normal_pong(&model);
	zassert_equal(model.state, RT_STATE_IDLE);
	rt_encode_ping(&model, ping, 5, 3, 0xaabbccdf, 1);
	zassert_equal(ping[7], RT_NORMAL_FLAG);
	zassert_equal(ping[8], RT_MAGIC_0);
}

ZTEST(remote_heated_tcal_contract, test_normal_withdrawal_caches_cancel_and_delayed_start_cannot_restart)
{
	const struct rt_payload start = {
		.transaction_id = 15,
		.action = RT_ACTION_START,
		.target_centi_c = 3500,
	};
	struct rt_model model;

	rt_model_init(&model);
	zassert_equal(rt_model_receive(&model, &start, 100), RT_EVENT_NONE);
	zassert_equal(model.state, RT_STATE_WAIT_START);

	/* Withdrawal itself returns directly to IDLE without a result handshake. */
	rt_model_normal_pong(&model);
	zassert_equal(model.state, RT_STATE_IDLE);
	zassert_true(model.cached_valid);
	zassert_equal(model.cached_result, RT_RESULT_CANCELED);
	zassert_equal(model.execution_count, 0);

	/*
	 * A delayed copy can only replay CANCELED; it cannot establish a new
	 * two-confirmation window or submit work to the sensor thread.
	 */
	zassert_equal(
		rt_model_receive(&model, &start, 200),
		RT_EVENT_RESULT_READY);
	zassert_equal(model.state, RT_STATE_RESULT_READY);
	zassert_equal(model.result, RT_RESULT_CANCELED);
	zassert_equal(model.execution_count, 0);
}

ZTEST(remote_heated_tcal_contract, test_nonzero_transaction_wrap)
{
	zassert_equal(rt_next_nonzero_transaction(0), 1);
	zassert_equal(rt_next_nonzero_transaction(1), 2);
	zassert_equal(rt_next_nonzero_transaction(UINT16_MAX), 1);

	const struct rt_payload max_transaction = {
		.transaction_id = UINT16_MAX,
		.action = RT_ACTION_STOP,
		.target_centi_c = 0,
	};
	uint8_t packet[RT_PACKET_LEN];
	struct rt_payload decoded = {0};

	rt_encode_command_pong(packet, 15, 0xff, &max_transaction);
	zassert_equal(packet[8], 0xff);
	zassert_equal(packet[9], 0xff);
	zassert_equal(rt_decode_command_pong(packet, &decoded), RT_DECODE_VALID);
	zassert_equal(decoded.transaction_id, UINT16_MAX);
}

ZTEST(remote_heated_tcal_contract, test_full_alias_table_rejects_without_overwrite)
{
	struct rt_alias_model aliases = {0};
	const uint32_t expected[RT_ALIAS_CAPACITY] = {11, 12, 13};

	zassert_true(rt_alias_reserve(&aliases, 11));
	zassert_true(rt_alias_reserve(&aliases, 12));
	zassert_true(rt_alias_reserve(&aliases, 13));
	zassert_mem_equal(aliases.token, expected, sizeof(expected));

	zassert_false(rt_alias_reserve(&aliases, 14));
	zassert_mem_equal(aliases.token, expected, sizeof(expected));
	zassert_true(rt_alias_reserve(&aliases, 12));
	zassert_mem_equal(aliases.token, expected, sizeof(expected));
}

ZTEST(remote_heated_tcal_contract, test_stop_commit_resets_sub_threshold_accumulator)
{
	struct rt_accumulator_model accumulator = {0};
	const uint32_t minimum_samples = 4;

	rt_accumulator_add(&accumulator, minimum_samples - 1U);
	rt_accumulator_stop_commit(&accumulator, minimum_samples);
	zassert_equal(accumulator.sample_count, 0);
	zassert_equal(accumulator.committed_samples, 0);

	/* A new session must not inherit the prior three samples. */
	rt_accumulator_add(&accumulator, 2);
	zassert_equal(accumulator.sample_count, 2);
	rt_accumulator_stop_commit(&accumulator, minimum_samples);
	zassert_equal(accumulator.sample_count, 0);
	zassert_equal(accumulator.committed_samples, 0);

	rt_accumulator_add(&accumulator, minimum_samples);
	rt_accumulator_stop_commit(&accumulator, minimum_samples);
	zassert_equal(accumulator.committed_samples, minimum_samples);
}

ZTEST(remote_heated_tcal_contract, test_wait_start_critical_legacy_cancels_without_handshake)
{
	struct rt_safety_model model;

	rt_safety_model_init(&model);
	model.state = RT_STATE_WAIT_START;
	model.payload = (struct rt_payload){
		.transaction_id = 31,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};

	zassert_equal(
		rt_safety_critical_legacy(&model),
		RT_LEGACY_ACCEPT);
	zassert_equal(model.state, RT_STATE_IDLE);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_NONE);
	zassert_true(model.cached_valid);
	zassert_equal(model.cached_result, RT_RESULT_CANCELED);
	zassert_equal(model.abort_submit_count, 0);
	zassert_true(rt_safety_execute_legacy(&model));
}

ZTEST(remote_heated_tcal_contract, test_active_start_critical_legacy_waits_for_physical_safety)
{
	struct rt_safety_model model;

	rt_safety_model_init(&model);
	model.state = RT_STATE_SENSOR_EXEC;
	model.payload = (struct rt_payload){
		.transaction_id = 32,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	model.heater_active = true;
	model.pwm_safe = false;
	model.sensor_token = 41;

	zassert_equal(
		rt_safety_critical_legacy(&model),
		RT_LEGACY_DEFER);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_LEGACY);
	zassert_equal(model.sensor_token, 0);
	zassert_equal(model.abandoned_token_count, 1);
	zassert_not_equal(model.safety_token, 0);
	zassert_false(rt_safety_execute_legacy(&model));

	/* A failed zero-duty write is not proof of safety and forces a retry. */
	rt_safety_abort_complete(&model, false);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_LEGACY);
	zassert_equal(model.safety_token, 0);
	zassert_false(model.pwm_safe);
	zassert_false(rt_safety_execute_legacy(&model));

	rt_safety_retry_abort(&model, true);
	zassert_not_equal(model.safety_token, 0);
	rt_safety_abort_complete(&model, true);
	zassert_equal(model.state, RT_STATE_IDLE);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_NONE);
	zassert_true(model.pwm_safe);
	zassert_true(model.cached_valid);
	zassert_equal(model.cached_result, RT_RESULT_CANCELED);

	/* The Receiver can now repeat the legacy PONG; execution follows PWM=0. */
	zassert_equal(
		rt_safety_critical_legacy(&model),
		RT_LEGACY_ACCEPT);
	zassert_true(rt_safety_execute_legacy(&model));
	zassert_true(model.pwm_safe_order < model.legacy_execute_order);
}

ZTEST(remote_heated_tcal_contract, test_pairing_reset_retries_busy_abort_until_safe)
{
	struct rt_safety_model model;
	uint32_t generation;

	rt_safety_model_init(&model);
	model.state = RT_STATE_SENSOR_EXEC;
	model.payload = (struct rt_payload){
		.transaction_id = 33,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	model.heater_active = true;
	model.pwm_safe = false;
	model.sensor_token = 51;
	model.cached_valid = true;
	generation = model.generation;

	/* First and second mailbox attempts model -EBUSY. */
	rt_safety_pairing_reset(&model, false);
	zassert_equal(model.generation, generation + 1U);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_PAIRING);
	zassert_equal(model.sensor_token, 0);
	zassert_equal(model.abandoned_token_count, 1);
	zassert_equal(model.safety_token, 0);
	zassert_false(model.cached_valid);
	zassert_false(model.pwm_safe);

	rt_safety_retry_abort(&model, false);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_PAIRING);
	zassert_equal(model.safety_token, 0);
	zassert_false(model.pwm_safe);

	rt_safety_retry_abort(&model, true);
	zassert_not_equal(model.safety_token, 0);
	rt_safety_abort_complete(&model, true);
	zassert_equal(model.state, RT_STATE_IDLE);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_NONE);
	zassert_true(model.pwm_safe);
	zassert_false(model.cached_valid);
	zassert_equal(model.abort_submit_count, 3);
}

ZTEST(remote_heated_tcal_contract, test_pairing_generation_rejects_stale_recovery_publish)
{
	struct rt_safety_model model;
	struct rt_recovery_snapshot stale;

	rt_safety_model_init(&model);
	model.state = RT_STATE_SENSOR_EXEC;
	model.payload = (struct rt_payload){
		.transaction_id = 34,
		.action = RT_ACTION_START,
		.target_centi_c = 3000,
	};
	model.heater_active = true;
	model.pwm_safe = false;
	model.sensor_token = 61;
	stale = rt_safety_snapshot(&model);

	rt_safety_pairing_reset(&model, false);
	zassert_false(rt_safety_publish_recovery(&model, &stale, 62));
	zassert_equal(model.generation, stale.generation + 1U);
	zassert_equal(model.preempt_mode, RT_SAFETY_PREEMPT_PAIRING);
	zassert_equal(model.sensor_token, 0);
	zassert_not_equal(model.sensor_token, 62);
}

ZTEST(remote_heated_tcal_contract, test_repeated_safety_preemption_reclaims_all_aliases)
{
	struct rt_mailbox_alias_model mailbox = {0};

	/*
	 * Nine rounds are three times the production alias-table capacity.  Each
	 * round models START -> STOP/ABORT -> critical legacy takeover and checks
	 * both main/deferred identity branches plus the superseded alias slot.
	 */
	for (uint32_t round = 0; round < 9; round++) {
		uint32_t start_token = 100U + round * 3U;
		uint32_t stop_token = start_token + 1U;
		uint32_t abort_token = start_token + 2U;

		mailbox.main_token = start_token;
		zassert_true(
			rt_mailbox_accept_deferred_safety(
				&mailbox, stop_token));
		zassert_equal(rt_alias_count(&mailbox.aliases), 1);

		/* Replacing START abandons main and its reserved alias together. */
		rt_mailbox_transport_abandon(&mailbox, start_token);
		zassert_true(mailbox.main_detached);
		zassert_equal(rt_alias_count(&mailbox.aliases), 0);

		/*
		 * An ABORT upgrade can make the old STOP both a deferred identity
		 * and a superseded alias.  Abandon must clear both views.
		 */
		zassert_true(rt_alias_reserve(&mailbox.aliases, stop_token));
		mailbox.deferred_token = stop_token;
		rt_mailbox_transport_abandon(&mailbox, stop_token);
		zassert_true(mailbox.deferred_detached);
		zassert_equal(rt_alias_count(&mailbox.aliases), 0);

		/* The critical command now owns ABORT until its safe completion. */
		mailbox.deferred_token = abort_token;
		rt_mailbox_complete_safety(&mailbox);
		zassert_equal(rt_alias_count(&mailbox.aliases), 0);
	}

	zassert_true(rt_alias_reserve(&mailbox.aliases, 0xfeed));
	zassert_equal(rt_alias_count(&mailbox.aliases), 1);
	rt_alias_release(&mailbox.aliases, 0xfeed);
	zassert_equal(rt_alias_count(&mailbox.aliases), 0);
}

ZTEST(remote_heated_tcal_contract,
      test_heated_owner_blocks_external_sensitivity_and_auto_mutations)
{
	struct rt_mutation_owner_model owner = {
		.heated_owner = true,
		.auto_enabled = true,
		.sensitivity_scale = 1.0f,
	};

	zassert_false(rt_external_sensitivity_write(&owner, 1.25f));
	zassert_equal(owner.sensitivity_scale, 1.0f);
	zassert_false(rt_external_sensitivity_write(&owner, 1.0f),
		      "sensitivity reset is guarded by the same owner");
	zassert_false(rt_external_auto_write(&owner, false));
	zassert_true(owner.auto_enabled);

	/* Heated start/stop may still save and restore auto state internally. */
	rt_internal_heated_auto_write(&owner, false);
	zassert_false(owner.auto_enabled);
	rt_internal_heated_auto_write(&owner, true);
	zassert_true(owner.auto_enabled);

	owner.heated_owner = false;
	zassert_true(rt_external_sensitivity_write(&owner, 1.25f));
	zassert_equal(owner.sensitivity_scale, 1.25f);
	zassert_true(rt_external_sensitivity_write(&owner, 1.0f));
	zassert_equal(owner.sensitivity_scale, 1.0f);
	zassert_true(rt_external_auto_write(&owner, false));
	zassert_false(owner.auto_enabled);
	zassert_false(owner.mutation_owner);
}

ZTEST_SUITE(remote_heated_tcal_contract, NULL, NULL, NULL, NULL, NULL);
