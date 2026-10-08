/*
 * NE102 PMU interrupt service routines.
 * Only SysTick is used (uptime timebase). Extend here when EXTI
 * (PIR/button), USART or RTC events are added.
 */
#include "n32l40x_it.h"

/* Defined in main.c, incremented here every 1 ms */
extern volatile uint32_t g_uptime_ms;

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    while (1) {
    }
}

void MemManage_Handler(void)
{
    while (1) {
    }
}

void BusFault_Handler(void)
{
    while (1) {
    }
}

void UsageFault_Handler(void)
{
    while (1) {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
    g_uptime_ms++;
}
