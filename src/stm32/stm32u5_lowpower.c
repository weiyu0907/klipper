// STM32U585 Stop2 low-power feasibility test (Milestone 2A)
//
// 目的僅有一個：證明 MCU 能在不掉電、不斷線的情況下自主進入 Stop2
// 並被 LPTIM1 喚醒回來繼續執行。範圍嚴格限制在「進出可行」，
// 本檔案刻意不做：
//   - 睡眠時長的時鐘飄移補償（LSI 未校準，週期會有 ±數% 誤差）
//   - DWT->CYCCNT 或任何其他儀器化量測
//   - 修改 board/irq.h 的 irq_wait()
//   - Klipper host 端斷線重連處理
// 觀測手段是 LED3（PH10/11/12，MCU 直驅、active-low）的三色狀態燈，
// 不依賴 SWD 或任何外部儀器。由 host console 下 "test_stop2" 手動觸發。
//
// ══════════════════════════════════════════════════════════════════
// ★★★ 板上測試前必讀，不照做測試會被 klippy 中途砍掉 ★★★
// ══════════════════════════════════════════════════════════════════
// Stop2 期間核心時脈停止，DWT->CYCCNT 與所有以它為底的計時全部凍結；
// 更關鍵的是 LPUART1 在 Stop2 下也是死的（本里程碑刻意不設它的
// APB3SMENR/SRDAMR 自主模式時脈閘，範圍只到 LPTIM1）。若 klipper.service
// 仍在跑，host 端每秒一次的 get_clock 收不到回應，MCU 只要睡超過
// klipper 的逾時門檻（通常一兩百毫秒），host 就會判定 "Lost
// communication with MCU" 並重啟韌體——測試會在中途被砍掉，而且
// 症狀會被誤判成 Stop2 本身壞掉，其實只是 host 端逾時重啟。
//
// 正確測法（一定要先停 klipper.service，改用裸 console 手動戳）：
//   sudo systemctl stop klipper
//   ~/klippy-env/bin/python3 ~/klipper/klippy/console.py /dev/ttyHS1
//   # 進入 console 之後下：
//   test_stop2 period_ms=2000 cycles=5
// 絕對不要在 klipper.service 正常運行、host 持續戳 get_clock 的情況下
// 測試 Stop2。
//
// Copyright (C) 2026  Weiyu Lin <weiyulin97@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h"
#include <stdint.h>
#include "command.h"            // DECL_COMMAND, sendf
#include "compiler.h"           // __visible
#include "board/irq.h"          // irq_disable, irq_enable
#include "board/armcm_boot.h"   // DECL_ARMCM_IRQ
#include "internal.h"           // IWDG

// Step 0-4 的 SYSCLK 重建序列定義在 stm32u5.c，冷開機與 Stop2 喚醒共用
// 同一份，避免複製貼上兩份（見該檔內 stm32u5_sysclk_bringup() 的註解）。
// 比照 u5_main.c 對 stm32_clock_init() 的作法：直接前置宣告，不另開
// 共用標頭。回傳值非 0 代表途中至少有一段忙等逾時（見該函式內註解：
// 逾時不會讓函式提早返回，只會讓對應那一段的 while 放棄，序列仍會
// 跑到底，SYSCLK 最差就是停在切換前的來源，不會卡在半設定狀態）。
uint32_t stm32u5_sysclk_bringup(void);

/* ===== RCC / PWR：LSI + LPTIM1 喚醒源、SYSCLK 驗證（RM0456 Rev4） ===== */
#define U5_RCC_BASE       0x46020C00UL
#define U5_PWR_BASE       0x46020800UL

#define U5_RCC_CR         (*(volatile uint32_t *)(U5_RCC_BASE + 0x000))
#define U5_RCC_CFGR1      (*(volatile uint32_t *)(U5_RCC_BASE + 0x01C))
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

/* LSI 標稱頻率 32kHz——(f) 這是規格書標稱值，本里程碑未做任何實測
 * 校準，ARR 換算出來的週期會跟牆鐘有 ±數% 落差，此屬預期、不是 bug。
 * ARR 只有 16 bit、無 prescaler，故單次可設週期上限約
 * 65535/32000*1000 ≈ 2047ms，對可行性驗證足夠，不為此另加 PRESC
 * 分頻邏輯（分頻補償同樣不在本里程碑範圍內）。 */
#define LSI_HZ_NOMINAL    32000u
#define LPTIM1_ARR_MAX    0xFFFFu

/* 所有忙等迴圈共用的逾時上限（次數，非時間）。(e) 任何輪詢都必須有
 * 上限，寧可誤判逾時也不要真的卡死在這裡出不來。 */
#define U5_WAIT_LOOPS     200000u

static uint32_t s_lptim1_arr;

/* 進出 Stop2 一次的完整診斷快照，供 command_test_stop2() 以 sendf()
 * 回報給 host，做論文數據用。 */
struct stop2_status {
    uint32_t cycle;
    uint32_t sws;              /* RCC_CFGR1 SWS[3:2]，3=PLL1R 才算真的鎖上 */
    uint32_t pll1rdy;          /* RCC_CR bit25 */
    uint32_t pwr_cr1;          /* 收尾後的 PWR_CR1 讀回值 */
    uint32_t scb_scr;          /* 收尾後的 SCB_SCR 讀回值 */
    uint32_t restore_timeout;  /* stm32u5_sysclk_restore() 內任一段忙等逾時 */
    uint32_t lptim_timeout;    /* LPTIM1 DIEROK/ARROK 輪詢逾時 */
    uint32_t entry_fail;       /* 進入 Stop2 前 PWR_CR1/SCB_SCR 寫入讀回驗證失敗 */
};
static struct stop2_status s_status;

/* ---- LED3_R/G/B = PH10/PH11/PH12，MCU 直驅、active-low ----
 * BSRR：亮 = 寫 bit(16+n) 的 reset 位，熄 = 寫 bit n 的 set 位。 */
void led3_red(int on)
{
    U5_GPIOH_BSRR = on ? (1u << 26) : (1u << 10);
}

void led3_green(int on)
{
    U5_GPIOH_BSRR = on ? (1u << 27) : (1u << 11);
}

void led3_blue(int on)
{
    U5_GPIOH_BSRR = on ? (1u << 28) : (1u << 12);
}

static void led3_init(void)
{
    /* AHB2ENR1 只用 |=：bit30/31 (SRAM2EN/SRAM3EN) 出廠即為 1，
     * 若曾經整顆覆寫成固定常數會把它們清成 0 → HardFault
     * （MSP 落在 SRAM3，實測過）。這裡只加 bit7 GPIOHEN，不動其他位。 */
    U5_RCC_AHB2ENR1 |= (1u << 7);
    (void)U5_RCC_AHB2ENR1;

    /* PH10/11/12 皆設為一般輸出 (MODER = 0b01) */
    U5_GPIOH_MODER = (U5_GPIOH_MODER
                       & ~((3u << 20) | (3u << 22) | (3u << 24)))
                    | (1u << 20) | (1u << 22) | (1u << 24);
    led3_red(0);
    led3_green(0);
    led3_blue(0);
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

    /* LSI：RCC_BDCR bit26=LSION, bit27=LSIRDY。(e) 等待迴圈設上限。 */
    U5_RCC_BDCR |= (1u << 26);
    for (i = 0; i < U5_WAIT_LOOPS; i++) {
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

    /* ms → LSI tick，夾在 [1, LPTIM1_ARR_MAX] 之間。(f) LSI_HZ_NOMINAL
     * 是規格標稱值，不是實測值，換算出來的週期本就會有落差。 */
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
 * 共用同一份函式，不在這裡複製貼上第二份。回傳值：非 0 = bringup
 * 內部至少有一段忙等逾時（例如 PLL1RDY 一直不來）。 */
uint32_t stm32u5_sysclk_restore(void)
{
    return stm32u5_sysclk_bringup();
}

/* 收集一次 Stop2 進出的診斷快照。sws/pll1rdy/pwr_cr1/scb_scr 是單純
 * 暫存器讀取、無副作用，四個退出路徑都呼叫這支也不會互相干擾。 */
static void
stop2_capture_status(uint32_t entry_fail, uint32_t restore_timeout,
                      uint32_t lptim_timeout)
{
    s_status.sws             = (U5_RCC_CFGR1 >> 2) & 3u;
    s_status.pll1rdy         = (U5_RCC_CR >> 25) & 1u;
    s_status.pwr_cr1         = U5_PWR_CR1;
    s_status.scb_scr         = SCB_SCR;
    s_status.restore_timeout = restore_timeout;
    s_status.lptim_timeout   = lptim_timeout;
    s_status.entry_fail      = entry_fail;
}

/* ===== Milestone 2A：Stop2 進出一次 =====
 * 呼叫前必須先跑過 lptim1_wakeup_init()，其設定的週期存在
 * s_lptim1_arr。流程：熄燈 → 武裝 LPTIM1 單次計數 → 關 SysTick →
 * 進 Stop2 → wfi 醒來 → 還原 SYSCLK/SysTick → 依讀回結果點對應的燈。
 *
 * IWDG 餵狗（wfi 前後各一次 IWDG->KR=0xAAAA）只解除「限制 A：多輪
 * 累積耗時超過 IWDG 預算」——watchdog_reset() 是 DECL_TASK，只有排程器
 * 的 task loop 跑到它才會餵狗，而 command_test_stop2() 的多輪迴圈整段
 * 都在同一次 command handler 呼叫裡跑完，中途不會把控制權還給排程器，
 * 所以要在這裡手動補餵。「限制 B：單次 Stop2 睡眠不能超過 IWDG 逾時」
 * 依然存在且無法用韌體解決——核心整個停在 wfi 期間，兩次手動餵狗中間
 * 的空窗依舊是同一顆 IWDG 倒數，若單次睡眠本身就超過逾時，狗必定咬人。 */
void stop2_once(void)
{
    volatile uint32_t i;
    uint32_t sysstate, scr, cr1;
    uint32_t lptim_timeout = 0;
    uint32_t restore_timeout;

    irq_disable();

    led3_red(0);
    led3_green(0);
    led3_blue(0);

    /* LPTIM 寫入時機稽核（位元編號查證自 stm32u585xx.h，見
     * docs/stop2_2a_review.md 的逐條結論）：
     *   (a) CFGR 只能在 ENABLE=0 時寫，enable 之後寫會被忽略
     *   (c) 本版 LPTIM 寫 DIER 之後要輪詢 ISR.DIEROK 才算生效
     *   (b) ARR 必須在 ENABLE=1 之後才寫，寫完輪詢 ISR.ARROK
     *   (d) 啟動順序 ENABLE=1 → 等（DIER/ARR 的 OK 輪詢本身就是這個
     *       「等」，不必再疊加一段任意延遲）→ SNGSTRT=1
     * CR/CFGR/DIER/ARR 是 LPTIM1 專屬、本檔獨佔的暫存器，不與其他
     * 驅動共用任何位元，因此直接整顆覆寫到已知狀態（歸零）是安全的
     * ——這與 RCC 系列共享型暫存器只能用 |=/&= 的規則並不衝突。 */
    U5_LPTIM1_CR   = 0;                    /* ENABLE=0，才能寫 CFGR */
    U5_LPTIM1_CFGR = 0;                    /* (a) */
    U5_LPTIM1_CR   = (1u << 0);            /* ENABLE=1，DIER/ARR 才能寫 */

    U5_LPTIM1_DIER = (1u << 1);            /* ARRMIE */
    for (i = 0; i < U5_WAIT_LOOPS; i++) {
        if (U5_LPTIM1_ISR & (1u << 24))    /* (c) DIEROK */
            break;
    }
    if (i >= U5_WAIT_LOOPS)
        lptim_timeout = 1;
    U5_LPTIM1_ICR = (1u << 24);            /* DIEROKCF */

    U5_LPTIM1_ARR  = s_lptim1_arr;
    for (i = 0; i < U5_WAIT_LOOPS; i++) {
        if (U5_LPTIM1_ISR & (1u << 4))     /* (b) ARROK */
            break;
    }
    if (i >= U5_WAIT_LOOPS)
        lptim_timeout = 1;
    U5_LPTIM1_ICR = (1u << 4);             /* ARROKCF */

    U5_LPTIM1_CR |= (1u << 1);             /* (d) SNGSTRT：單次計數 */

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
     * （位址算錯、或該時脈閘未開），直接放棄這次 Stop2、點亮
     * LED3_R 常亮示警並提早返回。這個分支刻意不中止外層的 cycles
     * 迴圈——讓它繼續一輪一輪跑，觀察上會呈現「幾乎瞬間又亮起」的
     * 快速閃爍，這正是判斷「根本沒進 Stop2」的訊號（見 LED 對照表）。 */
    cr1 = (U5_PWR_CR1 & ~7u) | 0x02u;
    U5_PWR_CR1 = cr1;
    if ((U5_PWR_CR1 & 7u) != 0x02u) {
        SYSTICK_CTRL = sysstate;
        U5_LPTIM1_CR = 0;
        stop2_capture_status(1, 0, lptim_timeout);
        led3_red(1);
        irq_enable();
        return;
    }

    scr = SCB_SCR | (1u << 2);
    SCB_SCR = scr;
    if (!(SCB_SCR & (1u << 2))) {
        U5_PWR_CR1 &= ~7u;
        SYSTICK_CTRL = sysstate;
        U5_LPTIM1_CR = 0;
        stop2_capture_status(1, 0, lptim_timeout);
        led3_red(1);
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
    IWDG->KR = 0xAAAA;     /* 睡前餵飽，讓倒數從滿格開始（見限制 A/B 說明） */
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("wfi");
    __asm volatile ("isb" ::: "memory");
    IWDG->KR = 0xAAAA;     /* 醒來立刻餵，在 sysclk_restore() 之前 */

    /* 醒來的第一件事永遠是拆除 SLEEPDEEP/LPMS，而不是先做時鐘還原。
     * 原因：這兩顆是「進入」深度睡眠的開關，本身不會被硬體自動清除；
     * 如果留著，之後任何一次 wfi（例如中斷處理常式或排程器裡的閒置
     * wfi）都會意外重新掉進 Stop2。此時 SYSCLK 還沒接回 PLL1、
     * irq 也還沒重新打開，先拆線最安全，之後才輪到 clock/SysTick
     * 還原，最後才 irq_enable() 讓 pending 的 LPTIM1 IRQ 真正跑起來。 */
    SCB_SCR &= ~(1u << 2);
    U5_PWR_CR1 &= ~7u;

    /* restore_timeout 只代表「過程中有沒有哪一段忙等放棄」；下面的
     * s_status.sws 才是「結果到底對不對」的直接證據——兩者都要看，
     * 純看 LED3_R 有沒有閃根本分不出這裡的問題（見問題 1 的說明）。 */
    restore_timeout = stm32u5_sysclk_restore();

    SCB_ICSR = (1u << 25);
    SYSTICK_CTRL = sysstate;

    U5_LPTIM1_ICR = 0x3FFF;
    U5_LPTIM1_CR  = 0;
    NVIC_ICPR2    = (1u << 3);

    irq_enable();

    stop2_capture_status(0, restore_timeout, lptim_timeout);

    /* 三色狀態燈——這裡是唯一能分辨「看起來醒了但其實只是慢了 40 倍」
     * 的地方（LPTIM1 掛在 LSI 上，跟 SYSCLK 是否鎖回 PLL1 完全無關，
     * 光看 LED3_R 有沒有照週期閃騙不了人）：
     *   restore 忙等逾時                → LED3_B 恆亮
     *   沒逾時但 SWS 讀回不是 PLL1R(3)  → LED3_G 恆亮
     *   兩者皆正常                      → LED3_R 亮（正常「醒著」瞬間）
     * 這三種都是「終止態」：一旦點亮 G 或 B，呼叫端
     * (command_test_stop2) 會中止後續 cycles，讓燈維持在錯誤狀態
     * 給人看，不要再蓋掉它。 */
    if (restore_timeout) {
        led3_blue(1);
    } else if (s_status.sws != 3u) {
        led3_green(1);
    } else {
        led3_red(1);
    }
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
    uint32_t n;

    if (period_ms == 0 || period_ms > 2000)
        period_ms = 2000;              /* LPTIM1 ARR 16bit/32kHz 上限 */
    if (cycles == 0 || cycles > 20)
        cycles = 5;

    led3_init();
    lptim1_wakeup_init(period_ms);

    for (n = 1; n <= cycles; n++) {
        stop2_once();
        s_status.cycle = n;

        /* 這組數字要能進 log 當論文數據：每一輪不論成敗都送一次。 */
        sendf("stop2_status cycle=%u sws=%u pll1rdy=%u pwr_cr1=%u"
              " scb_scr=%u restore_timeout=%u lptim_timeout=%u"
              " entry_fail=%u"
              , s_status.cycle, s_status.sws, s_status.pll1rdy
              , s_status.pwr_cr1, s_status.scb_scr
              , s_status.restore_timeout, s_status.lptim_timeout
              , s_status.entry_fail);

        /* restore_timeout 或 SWS 不是 PLL1R：終止態，中止後續 cycles，
         * 讓 LED3_B/G 維持點亮供離線判讀。entry_fail 不中止——維持
         * 原本的快速閃爍診斷（見 stop2_once() 內註解）。 */
        if (s_status.restore_timeout || s_status.sws != 3u)
            break;

        /* 亮燈維持一小段固定時間，讓「醒著」的瞬間人眼可見；
         * 這段時間不算進 LPTIM1 的睡眠週期，只是純視覺用途。 */
        busy_delay(2000000);
    }
}
DECL_COMMAND(command_test_stop2, "test_stop2 period_ms=%u cycles=%u");
