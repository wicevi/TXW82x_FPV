#ifndef __NE102_PMU_MAIN_H__
#define __NE102_PMU_MAIN_H__

#include "n32l40x.h"
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

/*
 * NE102 PMU (N32L403KBQ7) pin mapping.
 * Source: custom/doc/NE102 系统IO与连接关系.md section 4 (schematic page 6,
 * verified pin by pin). Pin numbers refer to the QFN32 package.
 */

/* ---- Outputs: power and peripheral control ---- */
#define PIN_PMU_PWR_3V3_PORT   GPIOA   /* pin11 high closes the 3V3 -> SYS_3V3 switch (main SoC power master) */
#define PIN_PMU_PWR_3V3        GPIO_PIN_5
#define PIN_PMU_DC_EN_PORT     GPIOA   /* pin10 DC-DC enables: 3V3 main rail (U6) + 1.2V core rail (U7) */
#define PIN_PMU_DC_EN          GPIO_PIN_4
#define PIN_PMU_RST_SYS_PORT   GPIOB   /* pin15 reset TXW828 (active low, wired to SoC NRST) */
#define PIN_PMU_RST_SYS        GPIO_PIN_1
#define PIN_PMU_Module_EN_PORT GPIOB   /* pin14 WiFi-Halow module supply enable */
#define PIN_PMU_Module_EN      GPIO_PIN_0
#define PIN_PMU_IR_EN_PORT     GPIOA   /* pin7 IR / fill-light branch enable (linked with CAM_EN) */
#define PIN_PMU_IR_EN          GPIO_PIN_1
#define PIN_PMU_PIR_CFG_PORT   GPIOA   /* pin9 PIR module configuration/enable */
#define PIN_PMU_PIR_CFG        GPIO_PIN_3
#define PIN_PMU_IRCUT_PORT     GPIOA   /* pin21 IR-CUT switching (dual-source with SoC PB15) */
#define PIN_PMU_IRCUT          GPIO_PIN_11
#define PIN_PMU_PWM_PORT       GPIOB   /* pin28 white fill-light PWM dimming (LEDW_PWM) */
#define PIN_PMU_PWM            GPIO_PIN_5

/* ---- Inputs: events and sensors ---- */
#define PIN_PMU_CFG_KEY_PORT   GPIOA   /* pin6 Tri_Key button (AON wakeup domain) */
#define PIN_PMU_CFG_KEY        GPIO_PIN_0
#define PIN_PMU_PIR_TRI_PORT   GPIOA   /* pin18 PIR trigger input (AON wakeup domain) */
#define PIN_PMU_PIR_TRI        GPIO_PIN_8
/* PA2 = PMU_ADC light sensor (ADC input, see TODO) */

/* ---- Debug UART: USART1 (default pins, AF4) ---- */
#define DBG_UART        USART1
#define DBG_UART_PERIPH RCC_APB2_PERIPH_USART1
#define DBG_UART_GPIO   GPIOA
#define DBG_UART_GPIOEN RCC_APB2_PERIPH_GPIOA
#define DBG_TX_PIN      GPIO_PIN_9   /* PA9 Debug_TX */
#define DBG_RX_PIN      GPIO_PIN_10  /* PA10 Debug_RX */

/* ---- Command UART to the TXW828 SoC (PB6 = PMU_TX / PB7 = PMU_RX, pins 29/30) ----
 * TODO: the UART peripheral and AF number for PB6/PB7 must be confirmed
 * against the datasheet pin-mux table before implementation
 * (N32L40x AF mapping differs from STM32: USART1 PA9/PA10 is AF4).
 */

void pmu_log_init(void);
void pmu_log(const char *fmt, ...);
void pmu_delay_ms(uint32_t ms);

#endif /* __NE102_PMU_MAIN_H__ */
