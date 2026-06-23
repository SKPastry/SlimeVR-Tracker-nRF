/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/init.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define SYSOFF_GPIO_PIN DT_GPIO_PIN(ZEPHYR_USER_NODE, sysoff_gpios)
#define SYSOFF_GPIO_PORT_NUM DT_PROP(DT_GPIO_CTLR(ZEPHYR_USER_NODE, sysoff_gpios), port)
#define SYSOFF_GPIO NRF_GPIO_PIN_MAP(SYSOFF_GPIO_PORT_NUM, SYSOFF_GPIO_PIN)

#define HEAT_EN_GPIO_PIN DT_GPIO_PIN(ZEPHYR_USER_NODE, heat_en_gpios)
#define HEAT_EN_GPIO_PORT_NUM DT_PROP(DT_GPIO_CTLR(ZEPHYR_USER_NODE, heat_en_gpios), port)
#define HEAT_EN_GPIO NRF_GPIO_PIN_MAP(HEAT_EN_GPIO_PORT_NUM, HEAT_EN_GPIO_PIN)

#define LED_EN_GPIO_PIN DT_GPIO_PIN(ZEPHYR_USER_NODE, led_en_gpios)
#define LED_EN_GPIO_PORT_NUM DT_PROP(DT_GPIO_CTLR(ZEPHYR_USER_NODE, led_en_gpios), port)
#define LED_EN_GPIO NRF_GPIO_PIN_MAP(LED_EN_GPIO_PORT_NUM, LED_EN_GPIO_PIN)

static void board_output_off(uint32_t pin)
{
	nrf_gpio_pin_clear(pin);
	nrf_gpio_cfg(
		pin,
		NRF_GPIO_PIN_DIR_OUTPUT,
		NRF_GPIO_PIN_INPUT_DISCONNECT,
		NRF_GPIO_PIN_NOPULL,
		NRF_GPIO_PIN_S0S1,
		NRF_GPIO_PIN_NOSENSE
	);
}

void board_early_init_hook(void)
{
	board_output_off(HEAT_EN_GPIO);
	board_output_off(LED_EN_GPIO);

	if ((nrf_power_mainregstatus_get(NRF_POWER) == NRF_POWER_MAINREGSTATUS_HIGH) &&
	    ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) ==
	     (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos))) {
		NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
		while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
			__NOP();
		}

		NRF_UICR->REGOUT0 =
			(NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) |
			(UICR_REGOUT0_VOUT_2V7 << UICR_REGOUT0_VOUT_Pos);

		NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
		while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
			__NOP();
		}

		NVIC_SystemReset();
	}
}

static int board_sk_cheesecake_nrf_p00_init(void)
{
	board_output_off(HEAT_EN_GPIO);
	board_output_off(LED_EN_GPIO);

	/* Keep the active-high external power cutoff inactive during normal operation. */
	board_output_off(SYSOFF_GPIO);

	return 0;
}

SYS_INIT(board_sk_cheesecake_nrf_p00_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
