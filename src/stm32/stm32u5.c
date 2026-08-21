#include "autoconf.h"
#include <stdint.h>
#include "command.h"            // DECL_COMMAND, output
#include "board/armcm_boot.h"   // DECL_ARMCM_IRQ

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

/* PWR: VOSR at +0x0C, SVMCR at +0x10, SVMSR at +0x3C */
#define U5_PWR_VOSR     (*(volatile uint32_t *)(U5_PWR_BASE + 0x00C))
#define U5_PWR_SVMCR    (*(volatile uint32_t *)(U5_PWR_BASE + 0x010))
#define U5_PWR_SVMSR    (*(volatile uint32_t *)(U5_PWR_BASE + 0x03C))

/* Flash */
#define U5_FLASH_ACR    (*(volatile uint32_t *)(0x40022000UL))

/* CRS */
#define U5_CRS_BASE     0x40006000UL
#define U5_CRS_CR       (*(volatile uint32_t *)(U5_CRS_BASE + 0x000))

/* ===== Milestone 2A: Stop2 low-power ===== */
#define U5_RCC_APB3SMENR (*(volatile uint32_t *)(U5_RCC_BASE + 0x0D0))
#define U5_RCC_SRDAMR    (*(volatile uint32_t *)(U5_RCC_BASE + 0x0D8))
#define U5_RCC_BDCR      (*(volatile uint32_t *)(U5_RCC_BASE + 0x0F0))
#define U5_PWR_CR1       (*(volatile uint32_t *)(U5_PWR_BASE + 0x000))
#define U5_PWR_DBPR      (*(volatile uint32_t *)(U5_PWR_BASE + 0x028))

#define U5_LPTIM1_BASE   0x46004400UL
#define U5_LPTIM1_ISR    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x00))
#define U5_LPTIM1_ICR    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x04))
#define U5_LPTIM1_DIER   (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x08))
#define U5_LPTIM1_CFGR   (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x0C))
#define U5_LPTIM1_CR     (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x10))
#define U5_LPTIM1_ARR    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x18))
#define U5_LPTIM1_CNT    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x1C))

#define U5_GPIOH_BASE    0x42021C00UL
#define U5_GPIOH_MODER   (*(volatile uint32_t *)(U5_GPIOH_BASE + 0x00))
#define U5_GPIOH_BSRR    (*(volatile uint32_t *)(U5_GPIOH_BASE + 0x18))

#define SCB_SCR          (*(volatile uint32_t *)0xE000ED10UL)
#define NVIC_ISER2       (*(volatile uint32_t *)0xE000E108UL)
#define NVIC_ICPR2       (*(volatile uint32_t *)0xE000E288UL)
#define SYSTICK_CTRL     (*(volatile uint32_t *)0xE000E010UL)
#define SCB_ICSR         (*(volatile uint32_t *)0xE000ED04UL)
#define DWT_CYCCNT       (*(volatile uint32_t *)0xE0001004UL)

/* 量測結果，供 OpenOCD 讀取。DWT 在 Stop2 凍結，
 * 故 cyc_total 只反映清醒時間；與牆鐘之差即睡眠總長。 */
volatile uint32_t g_stop2_meas[8];
/* [0]magic [1]cyc_total [2]rounds [3]isr_last
 * [4]arrm_cnt [5]cnt_last [6]cyc_awake [7]spin_max */
#define LPTIM1_IRQn      67

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
     * PLL1DIVR fields store N-1, P-1, Q-1, R-1 (RM0456).
     *   PLL1N-1 = 9  → N=10 → VCO = 16×10 = 160 MHz
     *   PLL1R-1 = 0  → R=1  → SYSCLK = 160/1 = 160 MHz
     */
    U5_RCC_PLL1DIVR = (9u  << 0)    /* PLL1N-1: N=10 */
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

/* Stop2 喚醒專用：只重建 SYSCLK 路徑。
 * 原 stm32_clock_init() 的 Step 5-8（HSI48/CRS/SVMCR/週邊時脈閘）
 * 在 Stop2 下均為保留狀態，重跑純屬浪費喚醒時間。 */
static void stm32u5_sysclk_restore(void)
{
    U5_RCC_CR |= (1u << 8);                            /* HSI16ON */
    while (!(U5_RCC_CR & (1u << 10)));                 /* HSI16RDY */
    U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 1u;          /* SW = HSI16 */
    while ((U5_RCC_CFGR1 & (3u << 2)) != (1u << 2));

    stm32u5_voltage_scale1();
    stm32u5_flash_latency_set(4);
    stm32u5_pll1_init();

    U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 3u;          /* SW = PLL1R */
    while ((U5_RCC_CFGR1 & (3u << 2)) != (3u << 2));
}


/* ===== Milestone 2A: LSI + LPTIM1 開機初始化 =====
 * 只做「組態」，不啟動計數。ARR/DIER/CNTSTRT 由 test_stop2 執行時才設，
 * 避免 LPTIM1 週期中斷干擾 Klipper 步進時序。
 * 所有等待迴圈皆有上限，絕不使用 while，防止開機卡死。 */
static void stm32u5_lowpower_init(void)
{
    volatile uint32_t i;

    /* LSI: RCC_BDCR bit26=LSION, bit27=LSIRDY
     * BDCR 屬 backup domain，系統重置不清除。
     * 實測 LSION 不受 PWR_DBPR 的 DBP 保護，無需解鎖。 */
    U5_RCC_BDCR |= (1u << 26);
    for (i = 0; i < 200000; i++) {
        if (U5_RCC_BDCR & (1u << 27))
            break;
    }

    /* LPTIM1 kernel clock = LSI: CCIPR3 bits[11:10] = 0b01 */
    U5_RCC_CCIPR3 = (U5_RCC_CCIPR3 & ~(3u << 10)) | (1u << 10);

    /* 三個閘門缺一不可：
     *   APB3ENR   bit11 LPTIM1EN    一般時脈閘
     *   APB3SMENR bit11 LPTIM1SMEN  Sleep/Stop 模式時脈閘
     *   SRDAMR    bit11 LPTIM1AMEN  SRD 自主模式 */
    U5_RCC_APB3ENR   |= (1u << 11);
    U5_RCC_APB3SMENR |= (1u << 11);
    U5_RCC_SRDAMR    |= (1u << 11);
    (void)U5_RCC_APB3ENR;

    /* WFI 只對 NVIC 已致能的中斷有反應。LPTIM1_IRQn = 67 → ISER2 bit3 */
    NVIC_ISER2 = (1u << 3);
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

    /* Step 7: VDDUSB (bit28=USV) + VDDIO2 (bit29=IO2SV) supply valid
     * PWR_SVMCR offset=0x10, PWR_SVMSR offset=0x3C (NOT 0x88 which is PUCRH)
     * HARDWARE NOTE: Uno Q PCB leaves VDDIO2 unconnected → VDDIO2RDY never sets.
     * When VDDIO2 is wired to VDD, add: while (!(U5_PWR_SVMSR & (1u << 25))); */
    U5_PWR_SVMCR |= (1u << 28) | (1u << 29);

    /* Step 8: peripheral clock gates */
    U5_RCC_AHB2ENR1 |= (1u << 0)    /* GPIOA */
                     | (1u << 1)     /* GPIOB */
                     | (1u << 6)      /* GPIOG */
                     | (1u << 14);   /* USB OTG FS (OTGEN) */
    U5_RCC_CCIPR3    = (U5_RCC_CCIPR3 & ~(7u << 0)) | (2u << 0); /* LPUART1 ← HSI16 */
    U5_RCC_APB3ENR  |= (1u << 6);   /* LPUART1EN */
    (void)U5_RCC_APB3ENR;           /* fence: wait for APB3 clock gate */

    /* Step 9: LSI + LPTIM1 (Stop2 wakeup source) */
    stm32u5_lowpower_init();
}

/* ---- LED3_R = PH10, active-low ---- */
static void led3_red(int on)
{
    U5_GPIOH_BSRR = on ? (1u << 26) : (1u << 10);
}

static void led3_init(void)
{
    U5_RCC_AHB2ENR1 |= (1u << 7);                      /* GPIOHEN */
    (void)U5_RCC_AHB2ENR1;
    U5_GPIOH_MODER = (U5_GPIOH_MODER & ~(3u << 20)) | (1u << 20);
    led3_red(0);
}

/* ---- LSI ~32kHz，Stop2 下持續運行 ---- */
static void lsi_init(void)
{
    /* RCC_BDCR 屬 backup domain，寫入前必須解除保護，
     * 否則寫入被硬體靜默吞掉、讀回永遠是 0。 */
    U5_PWR_DBPR |= (1u << 0);                          /* DBP */
    (void)U5_PWR_DBPR;

    U5_RCC_BDCR |= (1u << 26);                         /* LSION */
    while (!(U5_RCC_BDCR & (1u << 27)));               /* LSIRDY */
}

/* ---- LPTIM1 ← LSI，Stop2 喚醒源 ---- */
static void lptim1_init(void)
{
    /* 時脈源：CCIPR3 bits[11:10] = 0b01 = LSI */
    U5_RCC_CCIPR3 = (U5_RCC_CCIPR3 & ~(3u << 10)) | (1u << 10);

    U5_RCC_APB3ENR   |= (1u << 11);   /* LPTIM1EN   一般時脈閘 */
    U5_RCC_APB3SMENR |= (1u << 11);   /* LPTIM1SMEN Stop 模式時脈閘 ★缺這條會叫不醒 */
    U5_RCC_SRDAMR    |= (1u << 11);   /* LPTIM1AMEN SRD 自主模式 ★同上 */
    (void)U5_RCC_APB3ENR;

    /* NVIC：WFI 只有在「NVIC 已致能」的中斷 pending 時才會喚醒 */
    NVIC_ISER2 = (1u << (LPTIM1_IRQn - 64));
}

/* LPTIM1 IRQ 只是佔位，實際旗標在 stop2_once() 內清除 */
void LPTIM1_IRQHandler(void)
{
    U5_LPTIM1_ICR = (1u << 1);        /* ARRMCF */
}
DECL_ARMCM_IRQ(LPTIM1_IRQHandler, LPTIM1_IRQn);





/* ===== Milestone 2A: Stop2 進出驗證 =====
 * LSI/LPTIM1 的閘門與 NVIC 已於 stm32u5_lowpower_init() 開機時設妥。
 * 此處只負責：設 ARR、啟動單次計數、進 Stop2、喚醒後恢復時脈。 */
static void stop2_once(uint32_t arr)
{
    volatile uint32_t i;
    uint32_t sysstate;

    __asm volatile ("cpsid i" ::: "memory");

    /* LPTIM 設定序列（RM0456）：
     * DIER 只能在 ENABLE=0 時寫；ARR 只能在 ENABLE=1 之後寫。 */
    U5_LPTIM1_CR   = 0;
    U5_LPTIM1_CFGR = 0;
    U5_LPTIM1_DIER = (1u << 1);            /* ARRMIE */
    U5_LPTIM1_CR   = (1u << 0);            /* ENABLE */
    U5_LPTIM1_ARR  = arr;
    for (i = 0; i < 200000; i++) {
        if (U5_LPTIM1_ISR & (1u << 4))     /* ARROK */
            break;
    }
    U5_LPTIM1_ICR = (1u << 4);
    U5_LPTIM1_CR |= (1u << 1);             /* SNGSTRT 單次 */

    /* 關閉 SysTick 並清除其 pending。
     * WFI 規則：已致能且 pending 的中斷存在時不進入睡眠。
     * PRIMASK=1 下 ISR 無法執行、pending 永遠清不掉，
     * 而 Klipper 的 SysTick 持續到期 -> WFI 恆為 no-op。
     * SCB_ICSR bit25 = PENDSTCLR，清 SysTick pending。 */
    sysstate = SYSTICK_CTRL;
    SYSTICK_CTRL = 0;
    SCB_ICSR = (1u << 25);

    /* 進入 Stop2 */
    U5_PWR_CR1 = (U5_PWR_CR1 & ~7u) | 0x02u;
    SCB_SCR   |= (1u << 2);
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("wfi");
    __asm volatile ("isb" ::: "memory");

    /* 醒來：SYSCLK 已掉回 MSIS */
    SCB_SCR &= ~(1u << 2);
    U5_PWR_CR1 &= ~7u;
    stm32u5_sysclk_restore();

    /* 恢復 SysTick */
    SCB_ICSR = (1u << 25);
    SYSTICK_CTRL = sysstate;

    /* wfi 返回瞬間的狀態：ARRM=1 證明是 LPTIM1 叫醒的 */
    g_stop2_meas[3] = U5_LPTIM1_ISR;
    g_stop2_meas[5] = U5_LPTIM1_CNT;
    if (U5_LPTIM1_ISR & (1u << 1))
        g_stop2_meas[4]++;

    U5_LPTIM1_ICR = 0x3FFF;
    U5_LPTIM1_CR  = 0;
    NVIC_ICPR2    = (1u << 3);

    __asm volatile ("cpsie i" ::: "memory");
}

static void busy_delay(uint32_t n)
{
    volatile uint32_t i;
    for (i = 0; i < n; i++)
        __asm volatile ("nop");
}

void command_test_stop2(uint32_t *args)
{
    uint32_t cycles = args[0];
    uint32_t cyc0, w0;
    if (cycles == 0 || cycles > 20)
        cycles = 5;

    led3_init();

    g_stop2_meas[0] = 0x57012A5A;
    g_stop2_meas[2] = 0;
    g_stop2_meas[4] = 0;
    g_stop2_meas[6] = 0;
    cyc0 = DWT_CYCCNT;

    led3_red(1);
    busy_delay(30000000);
    led3_red(0);

    while (cycles--) {
        led3_red(0);
        w0 = DWT_CYCCNT;
        stop2_once(64000);
        g_stop2_meas[6] += DWT_CYCCNT - w0;
        g_stop2_meas[2]++;          /* LSI ~32kHz → 約 2 秒 */
        led3_red(1);
        busy_delay(15000000);
    }

    led3_red(0);
    g_stop2_meas[1] = DWT_CYCCNT - cyc0;
}
DECL_COMMAND(command_test_stop2, "test_stop2 cycles=%u");
