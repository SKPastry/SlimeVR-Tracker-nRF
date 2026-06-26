# SPDX-License-Identifier: Apache-2.0

if(CONFIG_CRAZT_WS2812_STRIP_I2S)
  target_sources(app PRIVATE crazt_ws2812_i2s.c)
endif()
