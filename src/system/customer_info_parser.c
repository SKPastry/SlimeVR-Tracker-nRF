/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "customer_info_parser.h"

#if defined(CONFIG_SK_CHEESECAKE_CUSTOMER_INFO) || defined(CUSTOMER_INFO_PARSER_TEST)

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#define CUSTOMER_INFO_SCHEMA 1U
#define CUSTOMER_INFO_GENERATION 1U
#define CUSTOMER_INFO_CRC_OFFSET 0x3CU

static const uint8_t customer_info_magic[4] = {'S', 'K', 'T', '0'};

static bool is_batch_character(uint8_t value)
{
	return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
}

static bool batch_id_is_valid(const uint8_t *field)
{
	size_t length = 0;
	bool previous_was_hyphen = false;

	while (length < CUSTOMER_INFO_BATCH_ID_SIZE && field[length] != '\0') {
		uint8_t value = field[length];

		if (value == '-') {
			if (length == 0 || previous_was_hyphen) {
				return false;
			}
			previous_was_hyphen = true;
		} else if (is_batch_character(value)) {
			previous_was_hyphen = false;
		} else {
			return false;
		}
		length++;
	}

	if (length == 0 || length == CUSTOMER_INFO_BATCH_ID_SIZE || previous_was_hyphen) {
		return false;
	}

	for (size_t i = length; i < CUSTOMER_INFO_BATCH_ID_SIZE; i++) {
		if (field[i] != '\0') {
			return false;
		}
	}

	return true;
}

static bool is_leap_year(uint32_t year)
{
	return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static bool production_date_is_valid(uint32_t value)
{
	static const uint8_t days_per_month[] = {
		31,
		28,
		31,
		30,
		31,
		30,
		31,
		31,
		30,
		31,
		30,
		31,
	};
	uint32_t year = value / 10000U;
	uint32_t month = (value / 100U) % 100U;
	uint32_t day = value % 100U;
	uint32_t maximum_day;

	if (year < 1000U || year > 9999U || month < 1U || month > 12U) {
		return false;
	}

	maximum_day = days_per_month[month - 1U];
	if (month == 2U && is_leap_year(year)) {
		maximum_day++;
	}

	return day >= 1U && day <= maximum_day;
}

bool customer_info_record_is_empty(const uint8_t *record, size_t length)
{
	if (record == NULL || length != CUSTOMER_INFO_RECORD_SIZE) {
		return false;
	}

	for (size_t i = 0; i < length; i++) {
		if (record[i] != 0xFFU) {
			return false;
		}
	}

	return true;
}

enum customer_info_parse_status
customer_info_parse_record(const uint8_t *record, size_t length, struct customer_info *out)
{
	struct customer_info parsed = {0};
	const uint8_t *batch;
	uint32_t stored_crc;
	uint32_t calculated_crc;

	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (record == NULL || out == NULL || length != CUSTOMER_INFO_RECORD_SIZE) {
		return CUSTOMER_INFO_PARSE_BAD_LENGTH;
	}

	if (customer_info_record_is_empty(record, length)) {
		return CUSTOMER_INFO_PARSE_EMPTY;
	}
	if (memcmp(record, customer_info_magic, sizeof(customer_info_magic)) != 0) {
		return CUSTOMER_INFO_PARSE_BAD_MAGIC;
	}

	parsed.schema = sys_get_le16(&record[0x04]);
	if (parsed.schema != CUSTOMER_INFO_SCHEMA) {
		return CUSTOMER_INFO_PARSE_BAD_SCHEMA;
	}
	if (sys_get_le16(&record[0x06]) != CUSTOMER_INFO_RECORD_SIZE) {
		return CUSTOMER_INFO_PARSE_BAD_LENGTH;
	}

	parsed.product_id = sys_get_le16(&record[0x08]);
	parsed.hardware_revision = sys_get_le16(&record[0x0A]);
	parsed.generation = sys_get_le32(&record[0x0C]);
	if (parsed.generation != CUSTOMER_INFO_GENERATION) {
		return CUSTOMER_INFO_PARSE_BAD_GENERATION;
	}

	batch = &record[0x10];
	if (!batch_id_is_valid(batch)) {
		return CUSTOMER_INFO_PARSE_BAD_BATCH_ID;
	}

	parsed.production_date = sys_get_le32(&record[0x20]);
	if (!production_date_is_valid(parsed.production_date)) {
		return CUSTOMER_INFO_PARSE_BAD_PRODUCTION_DATE;
	}

	if (record[0x38] < 'A' || record[0x38] > 'Z' || record[0x39] < 'A' || record[0x39] > 'Z') {
		return CUSTOMER_INFO_PARSE_BAD_REGION_CODE;
	}
	if (record[0x3A] != 0xFFU || record[0x3B] != 0xFFU) {
		return CUSTOMER_INFO_PARSE_BAD_RESERVED;
	}

	stored_crc = sys_get_le32(&record[CUSTOMER_INFO_CRC_OFFSET]);
	calculated_crc = crc32_ieee(record, CUSTOMER_INFO_CRC_OFFSET);
	if (stored_crc != calculated_crc) {
		return CUSTOMER_INFO_PARSE_BAD_CRC;
	}

	memcpy(parsed.batch_id, batch, CUSTOMER_INFO_BATCH_ID_SIZE);
	memcpy(parsed.factory_app_version, &record[0x24], sizeof(parsed.factory_app_version));
	memcpy(parsed.provenance_sha256_prefix, &record[0x28], CUSTOMER_INFO_PROVENANCE_SIZE);
	parsed.region_code[0] = (char)record[0x38];
	parsed.region_code[1] = (char)record[0x39];
	parsed.region_code[2] = '\0';
	parsed.crc32 = stored_crc;
	*out = parsed;

	return CUSTOMER_INFO_PARSE_OK;
}

enum customer_info_state customer_info_evaluate_slots(
	const uint8_t *slot_a,
	size_t slot_a_length,
	const uint8_t *slot_b,
	size_t slot_b_length,
	uint16_t expected_product_id,
	uint16_t expected_hardware_revision,
	struct customer_info_result *out
)
{
	struct customer_info_result result = {
		.state = CUSTOMER_INFO_STATE_INVALID,
		.slot_a_status = CUSTOMER_INFO_PARSE_BAD_LENGTH,
		.expected_product_id = expected_product_id,
		.expected_hardware_revision = expected_hardware_revision,
	};

	if (out == NULL) {
		return CUSTOMER_INFO_STATE_INVALID;
	}

	result.slot_b_empty = customer_info_record_is_empty(slot_b, slot_b_length);
	result.slot_a_status = customer_info_parse_record(slot_a, slot_a_length, &result.info);

	if (slot_a_length != CUSTOMER_INFO_RECORD_SIZE || slot_b_length != CUSTOMER_INFO_RECORD_SIZE || slot_a == NULL
		|| slot_b == NULL) {
		result.state = CUSTOMER_INFO_STATE_INVALID;
	} else if (!result.slot_b_empty) {
		result.state = CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY;
	} else {
		switch (result.slot_a_status) {
		case CUSTOMER_INFO_PARSE_OK:
			if (result.info.product_id != expected_product_id
				|| result.info.hardware_revision != expected_hardware_revision) {
				result.state = CUSTOMER_INFO_STATE_IDENTITY_MISMATCH;
			} else {
				result.state = CUSTOMER_INFO_STATE_VALID;
			}
			break;
		case CUSTOMER_INFO_PARSE_EMPTY:
			result.state = CUSTOMER_INFO_STATE_ABSENT;
			break;
		case CUSTOMER_INFO_PARSE_BAD_MAGIC:
		case CUSTOMER_INFO_PARSE_BAD_SCHEMA:
			result.state = CUSTOMER_INFO_STATE_UNSUPPORTED;
			break;
		default:
			result.state = CUSTOMER_INFO_STATE_INVALID;
			break;
		}
	}

	*out = result;
	return result.state;
}

const char *customer_info_parse_status_str(enum customer_info_parse_status status)
{
	switch (status) {
	case CUSTOMER_INFO_PARSE_OK:
		return "valid";
	case CUSTOMER_INFO_PARSE_EMPTY:
		return "empty";
	case CUSTOMER_INFO_PARSE_BAD_MAGIC:
		return "bad-magic";
	case CUSTOMER_INFO_PARSE_BAD_SCHEMA:
		return "bad-schema";
	case CUSTOMER_INFO_PARSE_BAD_LENGTH:
		return "bad-length";
	case CUSTOMER_INFO_PARSE_BAD_GENERATION:
		return "bad-generation";
	case CUSTOMER_INFO_PARSE_BAD_BATCH_ID:
		return "bad-batch-id";
	case CUSTOMER_INFO_PARSE_BAD_PRODUCTION_DATE:
		return "bad-production-date";
	case CUSTOMER_INFO_PARSE_BAD_REGION_CODE:
		return "bad-region-code";
	case CUSTOMER_INFO_PARSE_BAD_RESERVED:
		return "bad-reserved";
	case CUSTOMER_INFO_PARSE_BAD_CRC:
		return "bad-crc";
	default:
		return "unknown";
	}
}

const char *customer_info_state_str(enum customer_info_state state)
{
	switch (state) {
	case CUSTOMER_INFO_STATE_UNINITIALIZED:
		return "uninitialized";
	case CUSTOMER_INFO_STATE_VALID:
		return "valid";
	case CUSTOMER_INFO_STATE_ABSENT:
		return "absent";
	case CUSTOMER_INFO_STATE_UNSUPPORTED:
		return "unsupported";
	case CUSTOMER_INFO_STATE_INVALID:
		return "invalid";
	case CUSTOMER_INFO_STATE_IDENTITY_MISMATCH:
		return "identity-mismatch";
	case CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY:
		return "unexpected-slot-b";
	default:
		return "unknown";
	}
}

#endif /* CONFIG_SK_CHEESECAKE_CUSTOMER_INFO || CUSTOMER_INFO_PARSER_TEST */
