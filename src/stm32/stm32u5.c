#include "autoconf.h"
#include <stdint.h>

/* ===== STM32U585 RCC / PWR bare-metal registers (RM0456 Rev4) ===== */
#define U5_RCC_BASE     0x46020C00UL
#define U5_PWR_BASE     0x46020800UL

/* RCC clock control */
#define U5_RCC_CR       (*(volatile uint32_t *)(U5_RCC_BASE + 0x000))
/* RCC_CFGR1: SW[1:0] bits[1:0], SWS[1:0] bits[3:2] */
#define U5_RCC_CFGR1    (*(volatile uint32_t *)(U5_RCC_BASE + 0x01C))
/* PLL1 */
#define U5_RCC_PLL1CFGR (*(volatile uint32_t *)(U5_RCC_BASE + 0x028))
#define U5_RCC_PLL1DIVR (*(volatile uint32_t *)(U5_RCC_BASE + 0x034))
/* Peripheral clock enables */
#define U5_RCC_AHB2ENR1 (*(volatile uint32_t *)(U5_RCC_BASE + 0x08C))
#define U5_RCC_AHB3ENR  (*(volatile uint32_t *)(U5_RCC_BASE + 0x094))
#define U5_RCC_APB1ENR1 (*(volatile uint32_t *)(U5_RCC_BASE + 0x09C))
#define U5_RCC_APB3ENR  (*(volatile uint32_t *)(U5_RCC_BASE + 0x0A8))
/* Clock source selectors */
#define U5_RCC_CCIPR1   (*(volatile uint32_t *)(U5_RCC_BASE + 0x0E0))
#define U5_RCC_CCIPR3   (*(volatile uint32_t *)(U5_RCC_BASE + 0x0E8))

/* PWR: VOSR at +0x0C, SVMCR at +0x10 */
#define U5_PWR_VOSR     (*(volatile uint32_t *)(U5_PWR_BASE + 0x00C))
#define U5_PWR_SVMCR    (*(volatile uint32_t *)(U5_PWR_BASE + 0x010))

/* Flash */
#define U5_FLASH_ACR    (*(volatile uint32_t *)(0x40022000UL))

/* CRS */
#define U5_CRS_BASE     0x40006000UL
#define U5_CRS_CR       (*(volatile uint32_t *)(U5_CRS_BASE + 0x000))


static void stm32u5_voltage_scale1(void)
{
    /* PWR clock: RCC_AHB3ENR bit2 = PWREN */
    U5_RCC_AHB3ENR |= (1u << 2);
    (void)U5_RCC_AHB3ENR;

    /* EPOD booster clock = PLL1 input / PLL1MBOOST.
     * PLL1SRC and PLLMBOOST MUST be set before enabling BOOSTEN,
     * otherwise BOOSTRDY never asserts. */
    U5_RCC_PLL1CFGR = (U5_RCC_PLL1CFGR & ~((3u << 0) | (0xFu << 12)))
                    | (2u << 0)     /* PLL1SRC = HSI16 */
                    | (0u << 12);   /* PLL1MBOOST = /1 (HSI16 <= 16 MHz) */

    /* VOSR bits[17:16] = 0b11 → VOS Range 1 */
    U5_PWR_VOSR = (U5_PWR_VOSR & ~(3u << 16)) | (3u << 16);
    while (!(U5_PWR_VOSR & (1u << 15)));    /* VOSRDY bit15 */

    /* EPOD booster required for 160MHz: BOOSTEN bit18 */
    U5_PWR_VOSR |= (1u << 18);
    while (!(U5_PWR_VOSR & (1u << 14)));    /* BOOSTRDY bit14 */
}

static void stm32u5_flash_latency_set(uint32_t ws)
{
    /* LATENCY[3:0] at bits[3:0], PRFTEN at bit8 */
    U5_FLASH_ACR = (U5_FLASH_ACR & ~0xFu) | (ws & 0xFu) | (1u << 8);
    while ((U5_FLASH_ACR & 0xFu) != (ws & 0xFu));
}

static void stm32u5_pll1_init(void)
{
    /* Disable PLL1 */
    U5_RCC_CR &= ~(1u << 24);
    while (U5_RCC_CR & (1u << 25));

    /*
     * PLL1CFGR (complete config, PLL1 disabled):
     *   [1:0]  PLL1SRC  = 0b10  → HSI16
     *   [3:2]  PLL1RGE  = 0b11  → input 8-16 MHz
     *   [11:8] PLL1M    = 0     → /1
     *   [15:12] MBOOST  = 0     → /1
     *   [18]   PLL1REN  = 1     → enable R output (SYSCLK)
     */
    U5_RCC_PLL1CFGR = (2u << 0)    /* PLL1SRC = HSI16 */
                    | (3u << 2)     /* PLL1RGE = 8-16 MHz */
                    | (0u << 8)     /* PLL1M   = /1     */
                    | (0u << 12)    /* PLL1MBOOST = /1  */
                    | (1u << 18);   /* PLL1REN          */

    /*
     * PLL1DIVR:
     *   bits[8:0]   PLL1N = 10  → VCO = 16 × 10 = 160 MHz
     *   bits[30:24] PLL1R = 0   → SYSCLK = VCO / (0+1) = 160 MHz
     */
    U5_RCC_PLL1DIVR = (10u << 0)    /* PLL1N */
                    | (0u  << 9)     /* PLL1P */
                    | (0u  << 16)    /* PLL1Q */
                    | (0u  << 24);   /* PLL1R = /1 */

    /* Enable PLL1 */
    U5_RCC_CR |= (1u << 24);
    while (!(U5_RCC_CR & (1u << 25)));
}

static void stm32u5_hsi48_init(void)
{
    /* HSI48ON = RCC_CR bit12, HSI48RDY = RCC_CR bit13 */
    U5_RCC_CR |= (1u << 12);
    while (!(U5_RCC_CR & (1u << 13)));

    /* CRS auto-trim via USB SOF */
    U5_RCC_APB1ENR1 |= (1u << 24);     /* CRSEN */
    U5_CRS_CR |= (1u << 5)             /* AUTOTRIMEN */
              |  (1u << 6);             /* CEN */
}

void stm32_clock_init(void)
{
    /* Enable FPU: CP10 + CP11 full access (CPACR bits[23:20] = 0b1111) */
    *((volatile uint32_t *)0xE000ED88UL) |= (0xFu << 20);
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");

    /* Step 0: switch to HSI16 as interim SYSCLK */
    U5_RCC_CR |= (1u << 8);
    while (!(U5_RCC_CR & (1u << 10)));
    U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 1u;         /* SW = HSI16 */
    while ((U5_RCC_CFGR1 & (3u << 2)) != (1u << 2));   /* SWS = HSI16 */

    /* Step 1: VOS Range 1 + EPOD booster (prerequisite for 160 MHz) */
    stm32u5_voltage_scale1();

    /* Step 2: Flash latency 4WS (160 MHz @ VOS1) */
    stm32u5_flash_latency_set(4);

    /* Step 3: PLL1 → 160 MHz */
    stm32u5_pll1_init();

    /* Step 4: switch SYSCLK → PLL1R */
    U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 3u;         /* SW = PLL1R */
    while ((U5_RCC_CFGR1 & (3u << 2)) != (3u << 2));   /* SWS = PLL1R */

    /* Step 5: HSI48 + CRS (USB 48 MHz reference) */
    stm32u5_hsi48_init();

    /* Step 6: USB clock source = HSI48 (CCIPR1 ICLKSEL bits[27:26] = 0b00) */
    U5_RCC_CCIPR1 &= ~(3u << 26);

    /* Step 7: unlock VDDUSB supply (PWR_SVMCR bit28 = USV) */
    U5_PWR_SVMCR |= (1u << 28);

    /* Step 8: peripheral clock gates */
    U5_RCC_AHB2ENR1 |= (1u << 0)    /* GPIOA */
                     | (1u << 1)     /* GPIOB */
                     | (1u << 6)      /* GPIOG */
                     | (1u << 14);   /* USB OTG FS (OTGEN) */
    U5_RCC_CCIPR3    = (U5_RCC_CCIPR3 & ~(7u << 0)) | (2u << 0); /* LPUART1 ← HSI16 */
    U5_RCC_APB3ENR  |= (1u << 6);   /* LPUART1EN */
    (void)U5_RCC_APB3ENR;           /* fence: wait for APB3 clock gate */
}
