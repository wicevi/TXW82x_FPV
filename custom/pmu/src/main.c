/*
 * NE102 PMU firmware (N32L403KBQ7) - minimal bring-up build
 *
 * Scope (intentionally minimal, full PMU logic comes later):
 *   1. Drive PMU_DC_EN (PA4) high right after boot: it enables the DC-DC
 *      converters - the 3V3 main rail (U6) and the 1.2V core rail (U7,
 *      VIN = 3V3) - then, after the rails settle, drive PMU_PWR_3V3 (PA5)
 *      high to close the 3V3 -> SYS_3V3 switch. The SoC domain is only
 *      fully up after both steps, in this order.
 *   2. Leave every other pin untouched: after reset all GPIOs are floating
 *      inputs (high-Z). Pins wired to or shared with the SoC (PB1 RST_SYS,
 *      PB6/PB7 command UART, PA11 IRCUT, PB5 fill-light PWM, PA1 IR_EN)
 *      must NOT be driven while the TXW828 side is being brought up
 *      separately.
 *   3. Print the uptime every second over the PMU's own debug UART
 *      (USART1 PA9/PA10 -> T2/T3 test points; not shared with the SoC).
 *
 * Reference: Nations.N32L40x_Library.2.3.0 examples, EVAL bsp log.c
 */
#include "main.h"

/* Milliseconds since boot, incremented by SysTick_Handler (n32l40x_it.c) */
volatile uint32_t g_uptime_ms = 0;

/* Crude busy-wait millisecond delay (empty-loop estimate at 64 MHz;
 * good enough for bring-up, not for real timing) */
void pmu_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 6400;
    while (n--) {
        __NOP();
    }
}

/* Debug UART: USART1 PA9(TX)/PA10(RX), 115200, AF4.
 * These pins go to the PMU debug/test points only and are not shared with
 * the SoC, so driving them is safe. */
void pmu_log_init(void)
{
    GPIO_InitType gpio;
    USART_InitType uart;

    GPIO_InitStruct(&gpio);
    USART_StructInit(&uart);

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO | DBG_UART_GPIOEN | DBG_UART_PERIPH, ENABLE);

    gpio.Pin        = DBG_TX_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Slew_Rate = GPIO_Slew_Rate_High;
    gpio.GPIO_Alternate = GPIO_AF4_USART1;
    GPIO_InitPeripheral(DBG_UART_GPIO, &gpio);

    uart.BaudRate            = 115200;
    uart.WordLength          = USART_WL_8B;
    uart.StopBits            = USART_STPB_1;
    uart.Parity              = USART_PE_NO;
    uart.HardwareFlowControl = USART_HFCTRL_NONE;
    uart.Mode                = USART_MODE_TX;
    USART_Init(DBG_UART, &uart);
    USART_Enable(DBG_UART, ENABLE);
}

/* Blocking log output (do not call from ISRs or low-power paths) */
void pmu_log(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    int len, i;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len <= 0) {
        return;
    }
    if (len >= (int)sizeof(buf) - 1) {
        len = sizeof(buf) - 2; /* keep room for the line ending */
    }
    buf[len++] = '\r';
    buf[len++] = '\n';

    for (i = 0; i < len; i++) {
        while (USART_GetFlagStatus(DBG_UART, USART_FLAG_TXDE) == RESET) {
        }
        USART_SendData(DBG_UART, (uint8_t)buf[i]);
    }
}

/*
 * Power up the TXW828 domain in two ordered steps:
 *   1. PMU_DC_EN (PA4) high: enables the DC-DC converters - the 3V3 main
 *      rail (U6 MP1462) and the 1.2V core rail (U7 MP1601, VIN = 3V3).
 *   2. After the rails settle, PMU_PWR_3V3 (PA5) high closes the 3V3 ->
 *      SYS_3V3 switch (the switch has nothing to pass before step 1).
 * Both pins are configured as outputs driving low first (reset ODR value),
 * so both switches start open.
 *
 * NOTE on reset: the TXW828 NRST (pin 4) is held/released by the board RC
 * reset circuit - the PMU does NOT control the hardware reset line.
 * PMU_RST_SYS (PB1 -> TXW828 PB14 GPIO) is a firmware-level reset channel
 * only; it stays high-Z in this minimal firmware.
 */
static void soc_power_on(void)
{
    GPIO_InitType gpio;

    GPIO_InitStruct(&gpio);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA, ENABLE);

    gpio.Pin        = PIN_PMU_DC_EN | PIN_PMU_PWR_3V3;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Slew_Rate = GPIO_Slew_Rate_Low;
    GPIO_InitPeripheral(GPIOA, &gpio); /* both outputs low, switches open */

    GPIO_SetBits(PIN_PMU_DC_EN_PORT, PIN_PMU_DC_EN);     /* 3V3 + 1.2V rails up */
    pmu_delay_ms(5);
    GPIO_SetBits(PIN_PMU_PWR_3V3_PORT, PIN_PMU_PWR_3V3); /* 3V3 -> SYS_3V3 */
}

/*
 * Park every pin wired to or shared with the TXW828 in high-Z (floating
 * input, no internal pull). After reset all GPIOs are already floating
 * inputs; this restates the intent explicitly so the bring-up contract is
 * visible in code and any later pin config has to consciously override it.
 *
 * Covered pins (see custom/doc/NE102 系统IO与连接关系.md):
 *   PB6 PMU_TX       -> TXW828 PB13 (command UART)
 *   PB7 PMU_RX       -> TXW828 PB12 (command UART)
 *   PA11 PMU_IRCUT   -> shares the IR-CUT switch node with TXW828 PB15
 *   PB5 PMU_PWM      -> shares the fill-light PWM with TXW828
 *   PA1 PMU_IR_EN    -> IR/fill-light branch (TXW828 CAM_EN is in the same
 *                       power path)
 * PA5/PA4 (power enables) are driven high by soc_power_on() instead.
 */
static void txw_shared_pins_highz(void)
{
    GPIO_InitType gpio;

    GPIO_InitStruct(&gpio);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA | RCC_APB2_PERIPH_GPIOB, ENABLE);

    gpio.GPIO_Mode = GPIO_Mode_Input;
    gpio.GPIO_Pull = GPIO_No_Pull;

    gpio.Pin = PIN_PMU_RST_SYS | PIN_PMU_Module_EN | PIN_PMU_PWM;
    GPIO_InitPeripheral(GPIOB, &gpio);
    gpio.Pin = PIN_PMU_IR_EN | PIN_PMU_PIR_CFG | PIN_PMU_IRCUT;
    GPIO_InitPeripheral(GPIOA, &gpio);
}

/**
 * @brief  Main program.
 */
int main(void)
{
    pmu_log_init();
    soc_power_on();
    txw_shared_pins_highz();

    pmu_log("=== NE102 PMU bring-up (N32L403KBQ7) build %s %s ===", __DATE__, __TIME__);
    pmu_log("[PMU] PMU_DC_EN + PMU_PWR_3V3 high: 3V3/1V2 rails up, TXW828 domain on; all other pins high-Z");

    /* 1 ms timebase for the uptime counter */
    SysTick_Config(SystemCoreClock / 1000);

    while (1) {
        static uint32_t last_sec = 0xFFFFFFFF;
        uint32_t sec = g_uptime_ms / 1000;

        if (sec != last_sec) {
            last_sec = sec;
            pmu_log("[PMU] uptime %02u:%02u:%02u (%u s)",
                    (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60), (unsigned)(sec % 60), (unsigned)sec);
        }

        pmu_delay_ms(1);
    }
}
