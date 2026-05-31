#include "autoconf.h"
#include <stddef.h>
#include "sched.h"
#include "internal.h"

void stm32_clock_init(void);

/* RCC APB3ENR: enables LPUART1 (bit 6), RCC_BASE+0x0A8 */
#define U5_RCC_APB3ENR  ((volatile uint32_t *)0x46020CA8UL)
/* AHB2ENR1 for GPIO clocks */
#define U5_RCC_AHB2ENR1 ((volatile uint32_t *)0x46020C8CUL)

struct cline lookup_clock_line(uint32_t periph_base) {
    static volatile uint32_t dummy;
    if (periph_base == 0x46002400UL) /* LPUART1 */
        return (struct cline){ .en = U5_RCC_APB3ENR, .rst = NULL, .bit = (1u << 6) };
    /* GPIO and others: clock already enabled in stm32_clock_init */
    return (struct cline){ .en = &dummy, .rst = NULL, .bit = 0 };
}

void gpio_clock_enable(GPIO_TypeDef *regs) { }

uint32_t get_pclock_frequency(uint32_t periph_base) {
    if (periph_base == 0x46002400UL) /* LPUART1: HSI16 */
        return 16000000;
    return 160000000;
}

void bootloader_request(void) { }

void armcm_main(void) {
    stm32_clock_init();
    sched_main();
}
