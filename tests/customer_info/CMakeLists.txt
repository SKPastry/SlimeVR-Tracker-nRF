# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(customer_info_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/system/customer_info_parser.c
)
target_include_directories(app PRIVATE ../../src)
target_compile_definitions(app PRIVATE CUSTOMER_INFO_PARSER_TEST=1)
