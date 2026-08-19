/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SLIMENRF_SYSTEM_CUSTOMER_INFO_PARSER_H_
#define SLIMENRF_SYSTEM_CUSTOMER_INFO_PARSER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CUSTOMER_INFO_RECORD_SIZE 64U
#define CUSTOMER_INFO_BATCH_ID_SIZE 16U
#define CUSTOMER_INFO_PROVENANCE_SIZE 16U

enum customer_info_parse_status {
	CUSTOMER_INFO_PARSE_OK,
	CUSTOMER_INFO_PARSE_EMPTY,
	CUSTOMER_INFO_PARSE_BAD_MAGIC,
	CUSTOMER_INFO_PARSE_BAD_SCHEMA,
	CUSTOMER_INFO_PARSE_BAD_LENGTH,
	CUSTOMER_INFO_PARSE_BAD_GENERATION,
	CUSTOMER_INFO_PARSE_BAD_BATCH_ID,
	CUSTOMER_INFO_PARSE_BAD_PRODUCTION_DATE,
	CUSTOMER_INFO_PARSE_BAD_REGION_CODE,
	CUSTOMER_INFO_PARSE_BAD_RESERVED,
	CUSTOMER_INFO_PARSE_BAD_CRC,
};

enum customer_info_state {
	CUSTOMER_INFO_STATE_UNINITIALIZED,
	CUSTOMER_INFO_STATE_VALID,
	CUSTOMER_INFO_STATE_ABSENT,
	CUSTOMER_INFO_STATE_UNSUPPORTED,
	CUSTOMER_INFO_STATE_INVALID,
	CUSTOMER_INFO_STATE_IDENTITY_MISMATCH,
	CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY,
};

struct customer_info {
	uint16_t schema;
	uint16_t product_id;
	uint16_t hardware_revision;
	uint32_t generation;
	char batch_id[CUSTOMER_INFO_BATCH_ID_SIZE];
	uint32_t production_date;
	uint8_t factory_app_version[4];
	uint8_t provenance_sha256_prefix[CUSTOMER_INFO_PROVENANCE_SIZE];
	char region_code[3];
	uint32_t crc32;
};

struct customer_info_result {
	enum customer_info_state state;
	enum customer_info_parse_status slot_a_status;
	bool slot_b_empty;
	uint16_t expected_product_id;
	uint16_t expected_hardware_revision;
	struct customer_info info;
};

bool customer_info_record_is_empty(const uint8_t *record, size_t length);

enum customer_info_parse_status
customer_info_parse_record(const uint8_t *record, size_t length, struct customer_info *out);

enum customer_info_state customer_info_evaluate_slots(
	const uint8_t *slot_a,
	size_t slot_a_length,
	const uint8_t *slot_b,
	size_t slot_b_length,
	uint16_t expected_product_id,
	uint16_t expected_hardware_revision,
	struct customer_info_result *out
);

const char *customer_info_parse_status_str(enum customer_info_parse_status status);
const char *customer_info_state_str(enum customer_info_state state);

#endif /* SLIMENRF_SYSTEM_CUSTOMER_INFO_PARSER_H_ */
