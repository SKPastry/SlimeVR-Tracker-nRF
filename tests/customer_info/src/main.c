/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "system/customer_info_parser.h"

#include "fixtures.h"

static const uint8_t expected_factory_app_version[4] = {0U, 1U, 1U, 2U};
static const uint8_t expected_provenance[CUSTOMER_INFO_PROVENANCE_SIZE] = {
	0x00,
	0x01,
	0x02,
	0x03,
	0x04,
	0x05,
	0x06,
	0x07,
	0x08,
	0x09,
	0x0a,
	0x0b,
	0x0c,
	0x0d,
	0x0e,
	0x0f,
};

static void assert_common_golden_fields(const struct customer_info *info)
{
	zassert_equal(info->schema, 1U);
	zassert_equal(info->product_id, 1U);
	zassert_equal(info->generation, 1U);
	zassert_equal(info->production_date, 20260819U);
	zassert_mem_equal(info->factory_app_version, expected_factory_app_version, sizeof(expected_factory_app_version));
	zassert_mem_equal(info->provenance_sha256_prefix, expected_provenance, sizeof(expected_provenance));
	zassert_str_equal(info->region_code, "CN");
}

ZTEST(customer_info_parser, test_p00_golden_record)
{
	struct customer_info info;

	zassert_equal(
		customer_info_parse_record(customer_info_p00_golden, CUSTOMER_INFO_RECORD_SIZE, &info),
		CUSTOMER_INFO_PARSE_OK
	);
	assert_common_golden_fields(&info);
	zassert_equal(info.hardware_revision, 0U);
	zassert_str_equal(info.batch_id, "P00-EXAMPLE-01");
	zassert_equal(info.crc32, 0xa07be55bU);
}

ZTEST(customer_info_parser, test_p10_golden_record)
{
	struct customer_info info;

	zassert_equal(
		customer_info_parse_record(customer_info_p10_golden, CUSTOMER_INFO_RECORD_SIZE, &info),
		CUSTOMER_INFO_PARSE_OK
	);
	assert_common_golden_fields(&info);
	zassert_equal(info.hardware_revision, 10U);
	zassert_str_equal(info.batch_id, "P10-EXAMPLE-01");
	zassert_equal(info.crc32, 0xced5fc0dU);
}

ZTEST(customer_info_parser, test_erased_record_is_empty)
{
	uint8_t erased[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info info;

	memset(erased, 0xff, sizeof(erased));
	zassert_true(customer_info_record_is_empty(erased, sizeof(erased)));
	zassert_equal(customer_info_parse_record(erased, sizeof(erased), &info), CUSTOMER_INFO_PARSE_EMPTY);

	erased[17] = 0xfe;
	zassert_false(customer_info_record_is_empty(erased, sizeof(erased)));
	zassert_not_equal(customer_info_parse_record(erased, sizeof(erased), &info), CUSTOMER_INFO_PARSE_EMPTY);
}

ZTEST(customer_info_parser, test_unsupported_magic_and_schema_are_distinct)
{
	uint8_t record[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info info;

	memcpy(record, customer_info_p00_golden, sizeof(record));
	memcpy(record, "SKB1", 4U);
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_MAGIC);

	memcpy(record, customer_info_p00_golden, sizeof(record));
	memcpy(record, "NOPE", 4U);
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_MAGIC);

	memcpy(record, customer_info_p00_golden, sizeof(record));
	record[4] = 0U;
	record[5] = 0U;
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_SCHEMA);

	memcpy(record, customer_info_p00_golden, sizeof(record));
	record[4] = 2U;
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_SCHEMA);
}

ZTEST(customer_info_parser, test_each_invalid_field_has_a_stable_status)
{
	static const struct {
		size_t offset;
		uint8_t value;
		enum customer_info_parse_status expected;
	} mutations[] = {
		{0x06, 0x3f, CUSTOMER_INFO_PARSE_BAD_LENGTH},
		{0x0c, 0x02, CUSTOMER_INFO_PARSE_BAD_GENERATION},
		{0x10, 'p', CUSTOMER_INFO_PARSE_BAD_BATCH_ID},
		{0x23, 0x00, CUSTOMER_INFO_PARSE_BAD_PRODUCTION_DATE},
		{0x38, 'c', CUSTOMER_INFO_PARSE_BAD_REGION_CODE},
		{0x3a, 0x00, CUSTOMER_INFO_PARSE_BAD_RESERVED},
		{0x28, 0x01, CUSTOMER_INFO_PARSE_BAD_CRC},
	};
	struct customer_info info;

	for (size_t i = 0; i < ARRAY_SIZE(mutations); ++i) {
		uint8_t record[CUSTOMER_INFO_RECORD_SIZE];

		memcpy(record, customer_info_p00_golden, sizeof(record));
		record[mutations[i].offset] = mutations[i].value;
		zassert_equal(
			customer_info_parse_record(record, sizeof(record), &info),
			mutations[i].expected,
			"mutation index %zu",
			i
		);
	}
}

ZTEST(customer_info_parser, test_batch_id_requires_terminator_and_zero_padding)
{
	uint8_t record[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info info;

	memcpy(record, customer_info_p00_golden, sizeof(record));
	record[0x1e] = 'X';
	record[0x1f] = 'X';
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_BATCH_ID);

	memcpy(record, customer_info_p00_golden, sizeof(record));
	record[0x1f] = 'X';
	zassert_equal(customer_info_parse_record(record, sizeof(record), &info), CUSTOMER_INFO_PARSE_BAD_BATCH_ID);
}

ZTEST(customer_info_parser, test_unaligned_input_and_explicit_length_guard)
{
	uint8_t storage[CUSTOMER_INFO_RECORD_SIZE + 1U];
	struct customer_info info;
	uint8_t *const unaligned = &storage[1];

	memcpy(unaligned, customer_info_p10_golden, CUSTOMER_INFO_RECORD_SIZE);
	zassert_equal(customer_info_parse_record(unaligned, CUSTOMER_INFO_RECORD_SIZE, &info), CUSTOMER_INFO_PARSE_OK);
	zassert_equal(info.hardware_revision, 10U);

	for (size_t length = 0; length < CUSTOMER_INFO_RECORD_SIZE; ++length) {
		zassert_equal(
			customer_info_parse_record(unaligned, length, &info),
			CUSTOMER_INFO_PARSE_BAD_LENGTH,
			"short length %zu was accepted",
			length
		);
	}
	zassert_equal(
		customer_info_parse_record(unaligned, CUSTOMER_INFO_RECORD_SIZE + 1U, &info),
		CUSTOMER_INFO_PARSE_BAD_LENGTH
	);
}

ZTEST(customer_info_parser, test_null_inputs_are_rejected_without_dereference)
{
	uint8_t erased[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info info;
	struct customer_info_result result;

	memset(erased, 0xff, sizeof(erased));
	zassert_false(customer_info_record_is_empty(NULL, CUSTOMER_INFO_RECORD_SIZE));
	zassert_equal(customer_info_parse_record(NULL, CUSTOMER_INFO_RECORD_SIZE, &info), CUSTOMER_INFO_PARSE_BAD_LENGTH);
	zassert_equal(customer_info_parse_record(erased, CUSTOMER_INFO_RECORD_SIZE, NULL), CUSTOMER_INFO_PARSE_BAD_LENGTH);
	zassert_equal(
		customer_info_evaluate_slots(NULL, CUSTOMER_INFO_RECORD_SIZE, erased, sizeof(erased), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_INVALID
	);
	zassert_equal(
		customer_info_evaluate_slots(erased, sizeof(erased), erased, sizeof(erased), 1U, 0U, NULL),
		CUSTOMER_INFO_STATE_INVALID
	);
}

ZTEST(customer_info_parser, test_every_single_byte_bitflip_is_rejected)
{
	struct customer_info info;

	for (size_t offset = 0; offset < CUSTOMER_INFO_RECORD_SIZE; ++offset) {
		uint8_t record[CUSTOMER_INFO_RECORD_SIZE];

		memcpy(record, customer_info_p10_golden, sizeof(record));
		record[offset] ^= 0x01U;
		zassert_not_equal(
			customer_info_parse_record(record, sizeof(record), &info),
			CUSTOMER_INFO_PARSE_OK,
			"bit flip at byte %zu was accepted",
			offset
		);
	}
}

ZTEST(customer_info_parser, test_slot_evaluation_accepts_each_board_identity)
{
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memset(slot_b, 0xff, sizeof(slot_b));
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p00_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b),
			1U,
			0U,
			&result
		),
		CUSTOMER_INFO_STATE_VALID
	);
	zassert_equal(result.state, CUSTOMER_INFO_STATE_VALID);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_OK);
	zassert_true(result.slot_b_empty);
	zassert_equal(result.info.hardware_revision, 0U);
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p10_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b),
			1U,
			10U,
			&result
		),
		CUSTOMER_INFO_STATE_VALID
	);
	zassert_equal(result.info.hardware_revision, 10U);
}

ZTEST(customer_info_parser, test_slot_evaluation_reports_absent)
{
	uint8_t slot_a[CUSTOMER_INFO_RECORD_SIZE];
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memset(slot_a, 0xff, sizeof(slot_a));
	memset(slot_b, 0xff, sizeof(slot_b));
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_ABSENT
	);
	zassert_equal(result.state, CUSTOMER_INFO_STATE_ABSENT);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_EMPTY);
	zassert_true(result.slot_b_empty);
}

ZTEST(customer_info_parser, test_slot_evaluation_separates_unsupported_and_invalid)
{
	uint8_t slot_a[CUSTOMER_INFO_RECORD_SIZE];
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memset(slot_b, 0xff, sizeof(slot_b));
	memcpy(slot_a, customer_info_p00_golden, sizeof(slot_a));
	memcpy(slot_a, "SKB1", 4U);
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_UNSUPPORTED
	);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_BAD_MAGIC);

	memcpy(slot_a, customer_info_p00_golden, sizeof(slot_a));
	slot_a[4] = 0U;
	slot_a[5] = 0U;
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_UNSUPPORTED
	);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_BAD_SCHEMA);

	memcpy(slot_a, customer_info_p00_golden, sizeof(slot_a));
	slot_a[0x28] ^= 0x01U;
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_INVALID
	);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_BAD_CRC);
}

ZTEST(customer_info_parser, test_slot_evaluation_reports_identity_mismatch)
{
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memset(slot_b, 0xff, sizeof(slot_b));
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p00_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b),
			1U,
			10U,
			&result
		),
		CUSTOMER_INFO_STATE_IDENTITY_MISMATCH
	);
	zassert_equal(result.info.product_id, 1U);
	zassert_equal(result.info.hardware_revision, 0U);
	zassert_equal(result.expected_product_id, 1U);
	zassert_equal(result.expected_hardware_revision, 10U);
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p10_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b),
			1U,
			0U,
			&result
		),
		CUSTOMER_INFO_STATE_IDENTITY_MISMATCH
	);
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p00_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b),
			2U,
			0U,
			&result
		),
		CUSTOMER_INFO_STATE_IDENTITY_MISMATCH
	);
}

ZTEST(customer_info_parser, test_nonempty_slot_b_has_highest_priority)
{
	uint8_t slot_a[CUSTOMER_INFO_RECORD_SIZE];
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memcpy(slot_a, customer_info_p00_golden, sizeof(slot_a));
	memset(slot_b, 0xff, sizeof(slot_b));
	slot_b[7] = 0xfe;
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY
	);
	zassert_false(result.slot_b_empty);
	zassert_equal(result.slot_a_status, CUSTOMER_INFO_PARSE_OK);

	/* Slot B policy wins even when slot A is itself unsupported. */
	memcpy(slot_a, "SKB1", 4U);
	zassert_equal(
		customer_info_evaluate_slots(slot_a, sizeof(slot_a), slot_b, sizeof(slot_b), 1U, 0U, &result),
		CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY
	);
}

ZTEST(customer_info_parser, test_slot_evaluation_rejects_short_buffers)
{
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];
	struct customer_info_result result;

	memset(slot_b, 0xff, sizeof(slot_b));
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p00_golden,
			CUSTOMER_INFO_RECORD_SIZE - 1U,
			slot_b,
			sizeof(slot_b),
			1U,
			0U,
			&result
		),
		CUSTOMER_INFO_STATE_INVALID
	);
	zassert_equal(
		customer_info_evaluate_slots(
			customer_info_p00_golden,
			CUSTOMER_INFO_RECORD_SIZE,
			slot_b,
			sizeof(slot_b) - 1U,
			1U,
			0U,
			&result
		),
		CUSTOMER_INFO_STATE_INVALID
	);
}

ZTEST_SUITE(customer_info_parser, NULL, NULL, NULL, NULL, NULL);
