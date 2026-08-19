/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SLIMENRF_SYSTEM_CUSTOMER_INFO_H_
#define SLIMENRF_SYSTEM_CUSTOMER_INFO_H_

#include "customer_info_parser.h"

void customer_info_init(void);
const struct customer_info_result *customer_info_get_result(void);
void customer_info_log_status(void);
void customer_info_print(void);

#endif /* SLIMENRF_SYSTEM_CUSTOMER_INFO_H_ */
