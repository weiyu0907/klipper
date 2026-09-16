// STM32U585 Stop2 low-power feasibility test (Milestone 2A)
//
// 目的僅有一個：證明 MCU 能在不掉電、不斷線的情況下自主進入 Stop2
// 並被 LPTIM1 喚醒回來繼續執行。範圍嚴格限制在「進出可行」，
// 本檔案刻意不做：
//   - 睡眠時長的時鐘飄移補償（LSI 未校準，週期會有 ±數% 誤差）
//   - DWT->CYCCNT 或任何其他儀器化量測
//   - 修改 board/irq.h 的 irq_wait()
//   - Klipper host 端斷線重連處理
// 觀測手段只有 LED3_R（PH10，MCU 直驅、active-low）以固定週期閃爍，
// 不依賴 SWD 或任何外部儀器。由 host console 下 "test_stop2" 手動觸發。
//
// Copyright (C) 2026  Weiyu Lin <weiyulin97@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h"
#include <stdint.h>
#include "command.h"            // DECL_COMMAND
#include "compiler.h"           // __visible
#include "board/irq.h"          // irq_disable, irq_enable
#include "board/armcm_boot.h"   // DECL_ARMCM_IRQ

// Step 0-4 的 SYSCLK 重建序列定義在 stm32u5.c，冷開機與 Stop2 喚醒共用
// 同一份，避免複製貼上兩份（見該檔內 stm32u5_sysclk_bringup() 的註解）。
// 比照 u5_main.c 對 stm32_clock_init() 的作法：直接前置宣告，不另開
// 共用標頭。
void stm32u5_sysclk_bringup(void);

/* ===== RCC / PWR：LSI + LPTIM1 喚醒源（RM0456 Rev4） ===== */
#define U5_RCC_BASE       0x46020C00UL
#define U5_PWR_BASE       0x46020800UL

#define U5_RCC_AHB2ENR1   (*(volatile uint32_t *)(U5_RCC_BASE + 0x08C))
#define U5_RCC_AHB3ENR    (*(volatile uint32_t *)(U5_RCC_BASE + 0x094))
#define U5_RCC_CCIPR3     (*(volatile uint32_t *)(U5_RCC_BASE + 0x0E8))
#define U5_RCC_APB3ENR    (*(volatile uint32_t *)(U5_RCC_BASE + 0x0A8))
#define U5_RCC_APB3SMENR  (*(volatile uint32_t *)(U5_RCC_BASE + 0x0D0))
#define U5_RCC_SRDAMR     (*(volatile uint32_t *)(U5_RCC_BASE + 0x0D8))
#define U5_RCC_BDCR       (*(volatile uint32_t *)(U5_RCC_BASE + 0x0F0))

#define U5_PWR_CR1        (*(volatile uint32_t *)(U5_PWR_BASE + 0x000))
#define U5_PWR_DBPR       (*(volatile uint32_t *)(U5_PWR_BASE + 0x028))

#define U5_LPTIM1_BASE    0x46004400UL
#define U5_LPTIM1_ISR     (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x00))
#define U5_LPTIM1_ICR     (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x04))
#define U5_LPTIM1_DIER    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x08))
#define U5_LPTIM1_CFGR    (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x0C))
#define U5_LPTIM1_CR      (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x10))
#define U5_LPTIM1_ARR     (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x18))
#define U5_LPTIM1_CNT     (*(volatile uint32_t *)(U5_LPTIM1_BASE + 0x1C))

#define U5_GPIOH_BASE     0x42021C00UL
#define U5_GPIOH_MODER    (*(volatile uint32_t *)(U5_GPIOH_BASE + 0x00))
#define U5_GPIOH_BSRR     (*(volatile uint32_t *)(U5_GPIOH_BASE + 0x18))

#define SCB_SCR           (*(volatile uint32_t *)0xE000ED10UL)
#define SCB_ICSR          (*(volatile uint32_t *)0xE000ED04UL)
#define SYSTICK_CTRL      (*(volatile uint32_t *)0xE000E010UL)
#define NVIC_ISER2        (*(volatile uint32_t *)0xE000E108UL)
#define NVIC_ICPR2        (*(volatile uint32_t *)0xE000E288UL)

#define LPTIM1_IRQn       67

/* LSI 標稱頻率 32kHz（未校準）。ARR 只有 16 bit、無 prescaler，
 * 故單次可設週期上限約 65535/32 ≈ 2047ms —— 對 2A 的可行性驗證足夠，
 * 時鐘飄移補償本就不在本里程碑範圍內，不為此另加 PRESC 分頻邏輯。 */
#define LSI_HZ_NOMINAL    32000u
#define LPTIM1_ARR_MAX    0xFFFFu

static uint32_t s_lptim1_arr;

/* ---- LED3_R = PH10，MCU 直驅、active-low ---- */
void led3_red(int on)
{
    U5_GPIOH_BSRR = on ? (1u << 26) : (1u << 10);
}

static void led3_init(void)
{
    /* AHB2ENR1 只用 |=：bit30/31 (SRAM2EN/SRAM3EN) 出廠即為 1，
     * 若曾經整顆覆寫成固定常數會把它們清成 0 → HardFault
     * （MSP 落在 SRAM3，實測過）。這裡只加 bit7 GPIOHEN，不動其他位。 */
    U5_RCC_AHB2ENR1 |= (1u << 7);
    (void)U5_RCC_AHB2ENR1;

    U5_GPIOH_MODER = (U5_GPIOH_MODER & ~(3u << 20)) | (1u << 20);
    led3_red(0);
}

/* ---- LSI + LPTIM1：Stop2 喚醒源初始化 ----
 * 只做「組態」：LSI 起振、時脈路徑、三個閘門、NVIC 致能。
 * 實際 ARR/CNTSTRT 由 stop2_once() 每次呼叫時才設定，
 * 避免週期性中斷在測試以外的時間打斷 Klipper 主流程。 */
void lptim1_wakeup_init(uint32_t ms)
{
    volatile uint32_t i;
    uint32_t arr;

    /* DBP 必須先設，否則 backup domain (BDCR) 的寫入會被硬體靜默吞掉、
     * LSION 永遠設不進去，之後就會卡死在等 LSIRDY 的迴圈裡。 */
    U5_PWR_DBPR |= (1u << 0);
    (void)U5_PWR_DBPR;

    /* LSI：RCC_BDCR bit26=LSION, bit27=LSIRDY。等待迴圈設上限，
     * 絕不用 while，避免硬體異常時開機/測試指令永久卡死。 */
    U5_RCC_BDCR |= (1u << 26);
    for (i = 0; i < 200000; i++) {
        if (U5_RCC_BDCR & (1u << 27))
            break;
    }

    /* LPTIM1 kernel clock = LSI：CCIPR3 bits[11:10] = 0b01 */
    U5_RCC_CCIPR3 = (U5_RCC_CCIPR3 & ~(3u << 10)) | (1u << 10);

    /* PWR 時脈（PWREN）：DBP 屬於 PWR，穩妥起見先確認 PWR 已上電。 */
    U5_RCC_AHB3ENR |= (1u << 2);
    (void)U5_RCC_AHB3ENR;

    /* 三個閘門缺一不可：
     *   APB3ENR   bit11 LPTIM1EN    一般時脈閘
     *   APB3SMENR bit11 LPTIM1SMEN  Sleep/Stop 模式時脈閘
     *   SRDAMR    bit11 LPTIM1AMEN  SRD 自主模式
     * 少任何一個，LPTIM1 在 Stop2 下就會停止計數，永遠叫不醒 CPU。 */
    U5_RCC_APB3ENR   |= (1u << 11);
    U5_RCC_APB3SMENR |= (1u << 11);
    U5_RCC_SRDAMR    |= (1u << 11);
    (void)U5_RCC_APB3ENR;

    /* WFI 只對「NVIC 已致能」的中斷有反應，這一步不能省。
     * LPTIM1_IRQn = 67 → ISER2 (IRQ 64-95) 的 bit(67-64)=bit3。 */
    NVIC_ISER2 = (1u << (LPTIM1_IRQn - 64));

    /* ms → LSI tick，夾在 [1, LPTIM1_ARR_MAX] 之間。 */
    arr = (ms * LSI_HZ_NOMINAL) / 1000u;
    if (arr < 1u)
        arr = 1u;
    if (arr > LPTIM1_ARR_MAX)
        arr = LPTIM1_ARR_MAX;
    s_lptim1_arr = arr;
}

/* LPTIM1 IRQ：只清旗標即可。真正用來判斷「是不是它把 CPU 叫醒的」
 * 是 stop2_once() 裡對 ISR 的直接輪詢，不依賴這支 handler 有沒有跑到
 * （PRIMASK=1 時 pending 中斷不會進來，要等 stop2_once() 結尾才重新
 * 打開，這支 handler 隨後才會補跑一次，冪等、無副作用）。 */
void __visible
LPTIM1_IRQHandler(void)
{
    U5_LPTIM1_ICR = (1u << 1);        /* ARRMCF */
}
DECL_ARMCM_IRQ(LPTIM1_IRQHandler, LPTIM1_IRQn);

/* Stop2 喚醒後只需要重建 SYSCLK（Step 0-4）：Step 5-8（HSI48/CRS/
 * USB 供電門檻/週邊時脈閘）在 Stop2 期間是保留狀態，原樣留著，
 * 重跑純屬浪費喚醒時間。Step 0-4 的實際內容與 stm32_clock_init()
 * 共用同一份函式，不在這裡複製貼上第二份。 */
void stm32u5_sysclk_restore(void)
{
    stm32u5_sysclk_bringup();
}

/* ===== Milestone 2A：Stop2 進出一次 =====
 * 呼叫前必須先跑過 lptim1_wakeup_init()，其設定的週期存在
 * s_lptim1_arr。流程：熄燈 → 武裝 LPTIM1 單次計數 → 關 SysTick →
 * 進 Stop2 → wfi 醒來 → 還原 SYSCLK/SysTick → 亮燈。 */
void stop2_once(void)
{
    volatile uint32_t i;
    uint32_t sysstate, scr, cr1;

    irq_disable();

    led3_red(0);

    /* LPTIM 設定序列（RM0456）：DIER 只能在 ENABLE=0 時寫，
     * ARR 只能在 ENABLE=1 之後寫、且要等 ARROK 才算生效。
     * CR/CFGR 是 LPTIM1 專屬、本檔獨佔的暫存器，不與其他驅動共用
     * 任何位元，因此直接整顆覆寫到已知狀態（歸零）是安全的——
     * 這與 RCC 系列共享型暫存器只能用 |=/&= 的規則並不衝突。 */
    U5_LPTIM1_CR   = 0;
    U5_LPTIM1_CFGR = 0;
    U5_LPTIM1_DIER = (1u << 1);            /* ARRMIE */
    U5_LPTIM1_CR   = (1u << 0);            /* ENABLE */
    U5_LPTIM1_ARR  = s_lptim1_arr;
    for (i = 0; i < 200000; i++) {
        if (U5_LPTIM1_ISR & (1u << 4))     /* ARROK */
            break;
    }
    U5_LPTIM1_ICR = (1u << 4);
    U5_LPTIM1_CR |= (1u << 1);             /* SNGSTRT：單次計數 */

    /* 關閉 SysTick 並清除其 pending。
     * WFI 規則：只要有「已致能且 pending」的中斷存在就不會真正睡眠。
     * Klipper 的 SysTick 週期極短，若放著不管，進 wfi 前它幾乎一定
     * 已經 pending，會讓下面整段 Stop2 序列變成徹底的 no-op。
     * SCB_ICSR bit25 = PENDSTCLR，寫 1 清除 SysTick 的 pending。 */
    sysstate = SYSTICK_CTRL;
    SYSTICK_CTRL = 0;
    SCB_ICSR = (1u << 25);

    /* 進入 Stop2：PWR_CR1 LPMS[2:0]=0b010，SCB_SCR SLEEPDEEP=1。
     * 這兩顆暫存器「寫了看不出來」（沒有可輪詢的 ready/ack 位），
     * 所以照規範讀回驗證；驗證失敗代表暫存器根本沒被寫進去
     * （位址算錯、或該時脈閘未開），直接放棄這次 Stop2、点亮
     * LED 常亮示警並提早返回——比裝作睡著了但其實在 Sleep 模式
     * 空轉更容易被 LED 觀察到（見檔尾 Q&A）。 */
    cr1 = (U5_PWR_CR1 & ~7u) | 0x02u;
    U5_PWR_CR1 = cr1;
    if ((U5_PWR_CR1 & 7u) != 0x02u) {
        led3_red(1);
        SYSTICK_CTRL = sysstate;
        U5_LPTIM1_CR = 0;
        irq_enable();
        return;
    }

    scr = SCB_SCR | (1u << 2);
    SCB_SCR = scr;
    if (!(SCB_SCR & (1u << 2))) {
        led3_red(1);
        U5_PWR_CR1 &= ~7u;
        SYSTICK_CTRL = sysstate;
        U5_LPTIM1_CR = 0;
        irq_enable();
        return;
    }

    /* dsb：SCB_SCR/PWR_CR1/LPTIM1 的寫入都經過 AHB/APB bus fabric，
     * 有可能還在飛行中尚未真正落地。wfi 是否進入 Stop2、還是退化成
     * 普通 Sleep，取決於 SLEEPDEEP 與 LPMS 在「執行 wfi 那一刻」是否
     * 已經生效——dsb 在此確保上面所有暫存器寫入都已完成，這是避免
     * 「根本沒進 Stop2」最關鍵的一行。
     * isb：wfi 醒來後立刻沖刷管線，確保接下來抓到的指令流反映喚醒
     * 後的真實狀態，而不是深度睡眠前殘留的預取結果。 */
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("wfi");
    __asm volatile ("isb" ::: "memory");

    /* 醒來的第一件事永遠是拆除 SLEEPDEEP/LPMS，而不是先做時鐘還原。
     * 原因：這兩顆是「進入」深度睡眠的開關，本身不會被硬體自動清除；
     * 如果留著，之後任何一次 wfi（例如中斷處理常式或排程器裡的閒置
     * wfi）都會意外重新掉進 Stop2。此時 SYSCLK 還沒接回 PLL1、
     * irq 也還沒重新打開，先拆線最安全，之後才輪到 clock/SysTick
     * 還原，最後才 irq_enable() 讓 pending 的 LPTIM1 IRQ 真正跑起來。 */
    SCB_SCR &= ~(1u << 2);
    U5_PWR_CR1 &= ~7u;

    stm32u5_sysclk_restore();

    SCB_ICSR = (1u << 25);
    SYSTICK_CTRL = sysstate;

    U5_LPTIM1_ICR = 0x3FFF;
    U5_LPTIM1_CR  = 0;
    NVIC_ICPR2    = (1u << 3);

    irq_enable();

    led3_red(1);
}

/* ===== host 端手動觸發指令 ===== */
static void busy_delay(uint32_t n)
{
    volatile uint32_t i;
    for (i = 0; i < n; i++)
        __asm volatile ("nop");
}

void
command_test_stop2(uint32_t *args)
{
    uint32_t period_ms = args[0];
    uint32_t cycles = args[1];

    if (period_ms == 0 || period_ms > 2000)
        period_ms = 2000;              /* LPTIM1 ARR 16bit/32kHz 上限 */
    if (cycles == 0 || cycles > 20)
        cycles = 5;

    led3_init();
    lptim1_wakeup_init(period_ms);

    while (cycles--) {
        stop2_once();
        /* 亮燈維持一小段固定時間，讓「醒著」的瞬間人眼可見；
         * 這段時間不算進 LPTIM1 的睡眠週期，只是純視覺用途。 */
        busy_delay(2000000);
    }
}
DECL_COMMAND(command_test_stop2, "test_stop2 period_ms=%u cycles=%u");
