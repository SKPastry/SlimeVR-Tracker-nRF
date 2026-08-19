/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "customer_info.h"

#if defined(CONFIG_SK_CHEESECAKE_CUSTOMER_INFO)

#include <nrfx.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(customer_info, LOG_LEVEL_INF);

#define CUSTOMER_INFO_SLOT_WORDS (CUSTOMER_INFO_RECORD_SIZE / sizeof(uint32_t))
#define CUSTOMER_INFO_SLOT_B_WORD_OFFSET CUSTOMER_INFO_SLOT_WORDS

static struct customer_info_result cached_result = {
	.state = CUSTOMER_INFO_STATE_UNINITIALIZED,
};
static char cached_provenance_hex[CUSTOMER_INFO_PROVENANCE_SIZE * 2U + 1U];

static void read_customer_slot(size_t word_offset, uint8_t output[CUSTOMER_INFO_RECORD_SIZE])
{
	for (size_t i = 0; i < CUSTOMER_INFO_SLOT_WORDS; i++) {
		uint32_t word = NRF_UICR->CUSTOMER[word_offset + i];

		sys_put_le32(word, &output[i * sizeof(word)]);
	}
}

static void format_provenance(void)
{
	static const char digits[] = "0123456789ABCDEF";

	for (size_t i = 0; i < CUSTOMER_INFO_PROVENANCE_SIZE; i++) {
		uint8_t value = cached_result.info.provenance_sha256_prefix[i];

		cached_provenance_hex[i * 2U] = digits[value >> 4];
		cached_provenance_hex[i * 2U + 1U] = digits[value & 0x0FU];
	}
	cached_provenance_hex[sizeof(cached_provenance_hex) - 1U] = '\0';
}

void customer_info_init(void)
{
	uint8_t slot_a[CUSTOMER_INFO_RECORD_SIZE];
	uint8_t slot_b[CUSTOMER_INFO_RECORD_SIZE];

	if (cached_result.state != CUSTOMER_INFO_STATE_UNINITIALIZED) {
		return;
	}

	read_customer_slot(0, slot_a);
	read_customer_slot(CUSTOMER_INFO_SLOT_B_WORD_OFFSET, slot_b);
	customer_info_evaluate_slots(
		slot_a,
		sizeof(slot_a),
		slot_b,
		sizeof(slot_b),
		CONFIG_SK_CHEESECAKE_CUSTOMER_PRODUCT_ID,
		CONFIG_SK_CHEESECAKE_CUSTOMER_HARDWARE_REVISION,
		&cached_result
	);
	if (cached_result.state == CUSTOMER_INFO_STATE_VALID) {
		format_provenance();
	} else {
		cached_provenance_hex[0] = '\0';
	}
}

const struct customer_info_result *customer_info_get_result(void)
{
	return &cached_result;
}

#define CUSTOMER_INFO_LOG_AUDIT(log_fn)                                                                                \
	log_fn(                                                                                                            \
		"CUSTOMER identity: product=%u hardware=%u region=%s batch=%s",                                                \
		cached_result.info.product_id,                                                                                 \
		cached_result.info.hardware_revision,                                                                          \
		cached_result.info.region_code,                                                                                \
		cached_result.info.batch_id                                                                                    \
	);                                                                                                                 \
	log_fn(                                                                                                            \
		"CUSTOMER factory: date=%u app=%u.%u.%u.%u",                                                                   \
		cached_result.info.production_date,                                                                            \
		cached_result.info.factory_app_version[0],                                                                     \
		cached_result.info.factory_app_version[1],                                                                     \
		cached_result.info.factory_app_version[2],                                                                     \
		cached_result.info.factory_app_version[3]                                                                      \
	);                                                                                                                 \
	log_fn("CUSTOMER provenance: sha256_prefix=%s crc32=%08X", cached_provenance_hex, cached_result.info.crc32)

void customer_info_log_status(void)
{
	switch (cached_result.state) {
	case CUSTOMER_INFO_STATE_VALID:
		LOG_INF(
			"CUSTOMER: valid sample (SKT0/schema=%u slot=A generation=%u)",
			cached_result.info.schema,
			cached_result.info.generation
		);
		CUSTOMER_INFO_LOG_AUDIT(LOG_INF);
		break;
	case CUSTOMER_INFO_STATE_ABSENT:
		LOG_WRN("CUSTOMER: absent (slots A/B erased)");
		break;
	case CUSTOMER_INFO_STATE_IDENTITY_MISMATCH:
		LOG_WRN(
			"CUSTOMER: identity-mismatch (actual product=%u hardware=%u expected product=%u hardware=%u)",
			cached_result.info.product_id,
			cached_result.info.hardware_revision,
			cached_result.expected_product_id,
			cached_result.expected_hardware_revision
		);
		break;
	case CUSTOMER_INFO_STATE_UNSUPPORTED:
		LOG_WRN(
			"CUSTOMER: unsupported (slot=A reason=%s)",
			customer_info_parse_status_str(cached_result.slot_a_status)
		);
		break;
	case CUSTOMER_INFO_STATE_INVALID:
		LOG_WRN("CUSTOMER: invalid (slot=A reason=%s)", customer_info_parse_status_str(cached_result.slot_a_status));
		break;
	case CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY:
		LOG_WRN(
			"CUSTOMER: unexpected-slot-b (slot B must remain erased; slot A status=%s)",
			customer_info_parse_status_str(cached_result.slot_a_status)
		);
		break;
	default:
		LOG_WRN("CUSTOMER: uninitialized");
		break;
	}
}

static void print_audit(void)
{
	printk(
		"CUSTOMER identity: product=%u hardware=%u region=%s batch=%s\n",
		cached_result.info.product_id,
		cached_result.info.hardware_revision,
		cached_result.info.region_code,
		cached_result.info.batch_id
	);
	printk(
		"CUSTOMER factory: date=%u app=%u.%u.%u.%u\n",
		cached_result.info.production_date,
		cached_result.info.factory_app_version[0],
		cached_result.info.factory_app_version[1],
		cached_result.info.factory_app_version[2],
		cached_result.info.factory_app_version[3]
	);
	printk("CUSTOMER provenance: sha256_prefix=%s crc32=%08X\n", cached_provenance_hex, cached_result.info.crc32);
}

void customer_info_print(void)
{
	switch (cached_result.state) {
	case CUSTOMER_INFO_STATE_VALID:
		printk(
			"CUSTOMER: valid sample (SKT0/schema=%u slot=A generation=%u)\n",
			cached_result.info.schema,
			cached_result.info.generation
		);
		print_audit();
		break;
	case CUSTOMER_INFO_STATE_ABSENT:
		printk("CUSTOMER: absent (slots A/B erased)\n");
		break;
	case CUSTOMER_INFO_STATE_IDENTITY_MISMATCH:
		printk(
			"CUSTOMER: identity-mismatch (actual product=%u hardware=%u expected product=%u hardware=%u)\n",
			cached_result.info.product_id,
			cached_result.info.hardware_revision,
			cached_result.expected_product_id,
			cached_result.expected_hardware_revision
		);
		break;
	case CUSTOMER_INFO_STATE_UNSUPPORTED:
		printk(
			"CUSTOMER: unsupported (slot=A reason=%s)\n",
			customer_info_parse_status_str(cached_result.slot_a_status)
		);
		break;
	case CUSTOMER_INFO_STATE_INVALID:
		printk("CUSTOMER: invalid (slot=A reason=%s)\n", customer_info_parse_status_str(cached_result.slot_a_status));
		break;
	case CUSTOMER_INFO_STATE_SLOT_B_NOT_EMPTY:
		printk(
			"CUSTOMER: unexpected-slot-b (slot B must remain erased; slot A status=%s)\n",
			customer_info_parse_status_str(cached_result.slot_a_status)
		);
		break;
	default:
		printk("CUSTOMER: uninitialized\n");
		break;
	}
}

#endif /* CONFIG_SK_CHEESECAKE_CUSTOMER_INFO */
