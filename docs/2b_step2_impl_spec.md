# Milestone 2B step 2 — c1 實作規格

延續 [[2b_step1_design]]。本輪把 step 1 列出的開放問題（EXTI 需不需要、
UESM 跟 SMEN/AMEN 的分工、FIFO 排空要看哪個旗標、TX 完成怎麼確認）
查證清楚，並把 step 1 沒下的三個設計決策定案（見下），產出可以直接
照著改的規格。規格已經過使用者核准，**進入實作，但分兩階段燒錄**
（見第 0 節），每階段各自 `make` 後停下等待燒錄批准，不會一次改完
兩階段再一起燒。

## 0. 實作分階段（使用者核准的計畫）

**階段 1**（本輪先做）：`internal.h` 補位元巨集、`serial_init()`
（`UESM`/`FIFOEN`/`RXFTCFG=000`/`RXFTIE`）、ISR 改
`while (RXFNE)` 排空迴圈、`u5_main.c` 加 `APB3SMENR`＋`SRDAMR` 的
`LPUART1` 位元。**不動 `stop2_once()`**——這一階段結束時，LPUART1
的 autonomous/FIFO 設定全部到位，但 Stop2 進入序列本身還是舊版，
目的是**先確認 FIFO/UESM 這些改動不會破壞現有的 250000 baud 正常
通訊**，跟 Stop2 喚醒行為本身分開驗證，出問題時範圍容易鎖定。

階段 1 驗證：燒錄後 `state=ready`、`bytes_retransmit=0`、
`bytes_invalid=0`，**持續觀察至少 10 分鐘**，確認 FIFO 改動沒有
破壞正常通訊。**這一階段通過才做階段 2**。

**階段 2**（階段 1 通過後）：`stop2_once()` 加 `wfi` 前等待
`TC=1`（含逾時，逾時要計數並透過 `test_stop2` 回報，見第 3 節的
`tc_timeout=%u`），並加 Stop2 期間收資料的測試。

每一階段都是 `make` 後停下等待燒錄批准，不逾越。

## 設計決策（已定）

1. **`OVRDIS` 維持 1。** RM0456 的「`OVRDIS=0` + `EIE=1`」只適用於
   「溢位事件本身當作喚醒源」這個情境（見 A/D 查證），本設計用
   `RXFT`（門檻式）當喚醒源，兩者是互斥的兩條不同路徑，不衝突——
   `OVRDIS=1` 可以照舊維持，不需要為了支援 `RXFT` 喚醒而改動。
2. **喚醒源：`CR3.RXFTIE=1` + `RXFTCFG=000`（1/8 深度＝1 byte）。**
   理由（第 3 節有完整算式）：`total_latency`（喚醒延遲）約
   121.6~180.9µs（[[2b_g_wake_latency]] 第 5 節），換算成 250000
   baud 的 byte 數約 3.0~4.5 byte，FIFO 深度只有 8 byte——**門檻設淺
   （1 byte）才能在喚醒延遲這段空窗期還有餘裕**；若設深（例如
   7/8 或 full），門檻觸發時 FIFO 已經接近/完全滿了，喚醒延遲期間
   進來的 3~4.5 byte 會直接溢位。
3. **不用 `RXFF`。** RM0456 行 188122-188126 已指出 Stop 模式下
   `LPUART_RDR` 不被 clock，`RXFF` 中斷會觸發但旗標本身不會被設，
   軟體如果依賴讀 `RXFF` 判斷會撲空——`RXFT` 沒有這個陷阱。

---

## 查證（唯讀，附 RM0456 行號）

### A. LPUART1 從 Stop2 喚醒是否需要 EXTI direct line？

**不需要。** 查證方式：搜尋 RM0456 全文「EXTI」跟「LPUART」的共同
出現位置——**零筆結果**，整份 RM0456 完全沒有把 LPUART1 跟 EXTI
放在一起講過。再交叉核對 Table 189「EXTI line connections」
（行 60668-60694）——這張表列出 STM32U5 全部 EXTI 線的來源：
`0-15`＝GPIO、`16`＝PVD、`17/18`＝COMP1/2、`19-22`＝電壓監控、
`23-25`＝LSECSS/MSI_PLL_UNLOCK/IWDG——**沒有任何一條 EXTI 線對應
LPUART1 或任何 UART**。

**架構層級的原因**：STM32U5 的 LPUART1 屬於「SRD（Smart Run Domain）
autonomous」周邊（RM0456 行 28383 的清單），這一類周邊有自己的
「autonomous mode」機制（§67.4.15），能在核心睡著時自己去請求
kernel/APB clock、產生中斷直接送進 NVIC——這條路徑**取代**了舊系列
（F4/L4 等）那種「靠 EXTI 邊緣偵測 RX 腳位變化來喚醒、軟體再手動
重建通訊」的機制。**結論：A 的答案是「不需要」，NVIC 的
`LPUART1_IRQn`（66）加上 autonomous mode 三個致能位元（1a/B）就夠，
不用碰 `EXTI_IMR1`/`EXTI_RTSR1`/`EXTI_PR1`。**

（若真的需要，症狀跟目前決定的差異見第 3 節風險清單最後一項。）

### B. UESM 是否為 Stop 下接收的必要條件，以及跟 APB3SMENR/SRDAMR 的關係

**是必要條件，而且是三者中「總開關」的角色。** RM0456 行
188386-188389：

> When this bit is cleared, the LPUART cannot request its kernel clock
> and is not functional in low-power mode.
> When this bit is set, the LPUART can wake up the MCU from low-power
> mode.

**三顆位元的分工**（RM0456沒有直接列對照表，以下是逐條核對三處
註解、暫存器命名後的結論）：

| 位元 | 管什麼 | 沒設會怎樣 |
|---|---|---|
| `LPUART1SMEN`（`RCC_APB3SMENR` bit6，行 39384-39389） | **匯流排時脈閘控**要不要在 Sleep/Stop 期間放行——決定 LPUART1 在低功耗模式下「能不能被 clock 到」這件事本身 | 即使 `UESM`/`LPUART1AMEN` 都設了，Stop 模式下 APB clock gate 還是關的，周邊拿不到 clock，等於整個機制的電源開關沒開 |
| `LPUART1AMEN`（`RCC_SRDAMR` bit6，行 39541-39546） | **autonomous mode 這條請求路徑**本身在 Stop 0/1/2 模式下開不開——是 SRD 子系統專屬的第二層開關，跟 `SMEN` 是並列、不是包含關係 | 即使 `SMEN` 開了、時脈閘控放行，沒有 `AMEN` 的話周邊還是不能主動「請求」clock，autonomous 機制整個不會啟動 |
| `UESM`（`LPUART_CR1` bit1） | **周邊本身**願不願意在低功耗模式下運作、要不要嘗試喚醒 MCU——這是 LPUART1 自己的邏輯開關，不在 RCC 裡 | RM0456 原文明講：清掉這個位元，LPUART「不能請求它的 kernel clock」，**不管 RCC 那兩個位元設了沒有，LPUART 自己都不會在低功耗模式下動作** |

三者都要 `=1`：`SMEN`/`AMEN` 管的是「RCC 這一層允不允許這個周邊在
Stop 模式下拿到時脈」，`UESM` 管的是「周邊自己要不要用這個允許」。
少一個都會卡住整條鏈，語意上沒有誰能單獨頂替誰。

### C. FIFO 啟用後，ISR 排空看哪個旗標；ICR 要清哪些旗標

`LPUART_ISR`（行 189096，offset `0x1C`，reset `0x0080 00C0`）在
`FIFOEN=1` 時的 bit 5 是 **`RXFNE`**（Receive FIFO Not Empty），
跟 `FIFOEN=0` 時同一個 bit 5 的 `RXNE` 是**同一個實體位元、不同語意
命名**（現有 `internal.h` 的巨集 `USART_ISR_RXNE = 0x0020` 數值本身
不用改，FIFO 開了之後這顆巨集實際代表的就是 `RXFNE`，語意上建議
改名但數值相容）。

**清旗標的方式**：Table 694（行 188133-188139，`RXFNE` 那一列）：

> Read RDR until RXFIFO empty or write 1 in RXFRQ

`RXFT`（門檻旗標，`LPUART_ISR` bit 26，行 189140-189149）跟 `RXFNE`
一樣：Table 694（行 188143-188145）：

> Interrupt clear method: Read RDR

**兩者都不需要寫 `ICR`**——單純把 `RDR` 讀到 FIFO 空為止，旗標會
跟著自動清掉。這代表 ISR 排空迴圈只需要：

```c
while (USARTx->ISR & USART_ISR_RXNE /* 即 RXFNE，FIFO 開啟後同一個 bit */)
    serial_rx_byte(USARTx->RDR);
```

不需要額外操作 `LPUART_ICR`（`ICR` 是給 `ORE`/`IDLE`/`PE`/`NE`/`FE`/
`CMF`/`CTSIF`/`TCCF` 這類「寫 1 清除」型旗標用的，`RXFNE`/`RXFT`
不屬於這類，`OVRDIS=1` 也讓 `ORE` 這條路徑整個不會被觸發，見設計
決策 1）。

### D. 進 Stop2 前 TX 是否必須完成

**必須，而且要軟體主動等，不能假設 autonomous 機制會自己處理。**

`TC`（Transmission complete）：`LPUART_ISR` bit 6（行 189273-189283，
offset `0x1C`，reset 值 `0x0080 00C0` 的 `0xC0`＝bit6+bit7 皆為 1，
代表通電/重置後傳輸器閒置時 `TC` 預設就是 1）。

RM0456 對「TX 完成後才能進低功耗模式」有明文要求（USART 章節，行
183305-183310，原則通用於整個 USART/LPUART 家族，行文脈絡是 DMA
傳輸但陳述的物理原因——「進低功耗模式時周邊時脈會被切斷」——不限定
DMA 路徑）：

> In transmission mode, ... the TC flag can be monitored to make sure
> that the USART communication has completed. This is required to
> avoid corrupting the last transmission before disabling the USART or
> before the system enters a low-power mode when the peripheral clock
> is disabled. Software must wait until TC = 1.

**本設計沒有啟用 TX 方向的 autonomous 機制**（沒有設
`LPUART_AUTOCR` 的 `TRIGSEL`、沒有給 TX 開 DMA、`CR1`/`CR3` 也沒有
給 `TXFTIE`/`TXFNFIE` 之外額外的 TX 低功耗保活設定）——§67.4.15
「LPUART transmission mode」段（[[2b_step1_design]] 已引用）描述的
「TX 在 Stop 模式下能自動請求 APB clock」機制，前提是對應的
interrupt/DMA 有致能；本設計的 TX 路徑（`serial_enable_tx_irq()`）
只在**主動送資料時**才開 `TXEIE`，送完就關（見
`stm32f0_serial.c:153-160`），**不是為了「Stop 模式下讓 TX 自動跑完」
設計的**，沒有證據支持「即使 Stop2 進入時 TX 還沒送完，autonomous
機制也會自動把它送完」這個假設成立，依照 RM0456 的明文建議，**保守
作法是進 `wfi` 前明確輪詢 `TC=1`**（含逾時保護，見第 2 節
`stop2_once()` 改法）。

---

## 1. 暫存器設定清單

| 暫存器 | Bit | 設定值 | 時機 | RM0456 行號 |
|---|---|---|---|---|
| `RCC_APB3ENR` | 6 `LPUART1EN` | 1（**已設**，`enable_pclock` 既有路徑） | 任意 | 38216 |
| `RCC_APB3SMENR` | 6 `LPUART1SMEN` | 1（新增） | 任意，建議冷開機初始化時、進 Stop2 前皆已生效即可 | 39384 |
| `RCC_SRDAMR` | 6 `LPUART1AMEN` | 1（新增） | 同上 | 39541 |
| `LPUART1_CR3` | 27:25 `RXFTCFG` | `000`（1/8＝1 byte） | **`UE=0` 期間** | 188781 |
| `LPUART1_CR3` | 28 `RXFTIE` | 1 | **`UE=0` 期間**（跟 `RXFTCFG` 同一批寫，`RXFTIE` 本身沒有 `UE=0` 限制，但為求一致跟 `RXFTCFG` 一起在 `UE=0` 時寫最單純） | 188763 |
| `LPUART1_CR3` | 12 `OVRDIS` | 1（**維持既有值不動**） | `UE=0` 期間（既有 `serial_init()` 已經在 `UE=0` 時序裡做，不用改） | 188824 |
| `LPUART1_CR1` | 29 `FIFOEN` | 1（新增） | **`UE=0` 期間** | 188234 |
| `LPUART1_CR1` | 1 `UESM` | 1（新增） | 初始化階段，需與 `UE` 一起或在 `UE=0` 時設好 | 188386 |
| `LPUART1_CR1` | 0 `UE`／2 `RE`／3 `TE`／5 `RXNEIE` | 維持既有邏輯 | 排在上面幾項之後才寫 | 既有 |
| `LPUART1_ISR` | 6 `TC` | 只讀，不寫 | `stop2_once()` 進 `wfi` 前輪詢 | 189273 |
| NVIC | `LPUART1_IRQn`(66) | **已致能**（`serial_init()` 既有 `armcm_enable_irq()`），不用新增 | — | RM0456 行 60303（向量表）；`armcm_boot.h:13-17` |
| EXTI | 不涉及 | **不需要任何設定**（見查證 A） | — | — |

---

## 2. 需要修改的檔案與函式

### `src/stm32/internal.h`：補位元巨集

現有巨集只到 `USART_CR3_OVRDIS`（`0x1000`）。需要新增：

```c
#define USART_CR1_UESM    0x0002U   /* CR1 bit1，行 188386 */
#define USART_CR1_FIFOEN  0x20000000U /* CR1 bit29，行 188234 */
#define USART_CR3_RXFTIE  0x10000000U /* CR3 bit28，行 188763 */
#define USART_CR3_RXFTCFG_000  0x00000000U /* CR3 bits27:25 = 000，1/8 深度，行 188782 */
#define USART_ISR_TC      0x0040U   /* ISR bit6，行 189273 */
```

`USART_ISR_RXNE`（既有 `0x0020`）數值不變，FIFO 開啟後代表 `RXFNE`，
沿用同一個巨集即可，不需要新巨集（避免重複定義同一個 bit）。

### `src/stm32/stm32f0_serial.c`：`serial_init()` 改法

現有：

```c
USARTx->CR3 = USART_CR3_OVRDIS;
USARTx->CR1 = CR1_FLAGS;
armcm_enable_irq(USARTx_IRQHandler, USARTx_IRQn, 0);
```

改法（`UE=0` 這件事本來就成立——`CR1` 在這之前從未被寫過，`UE` 預設
是 0，所以「先寫 `RXFTCFG`/`FIFOEN`、最後才寫含 `UE` 的完整 `CR1`」
這個順序**用現有初始化流程本來就自然滿足，不需要額外插入「先清
UE」的步驟**）：

```c
#if defined(LPUART_BRR)
    // U5/LPUART1: 需要 FIFO + RXFT 門檻式喚醒（2B c1），這幾個位元
    // 只能在 UE=0 時寫，此時 CR1 UE 本來就還是 reset 值 0
    USARTx->CR3 = USART_CR3_OVRDIS | USART_CR3_RXFTCFG_000
                  | USART_CR3_RXFTIE;
    USARTx->CR1 = USART_CR1_FIFOEN;   // 先開 FIFO，UE 還是 0
    USARTx->CR1 = CR1_FLAGS | USART_CR1_UESM | USART_CR1_FIFOEN;
#else
    USARTx->CR3 = USART_CR3_OVRDIS;
    USARTx->CR1 = CR1_FLAGS;
#endif
    armcm_enable_irq(USARTx_IRQHandler, USARTx_IRQn, 0);
```

用 `#if defined(LPUART_BRR)` 包住（這個檔案裡 `LPUART_BRR` 只有
`CONFIG_STM32_SERIAL_LPUART1` 分支會定義，見檔案行 104）——**只影響
LPUART1 這個分支，不動其他 MCU 系列既有的 USART 初始化路徑**，符合
[[2b_step1_design]] 第 5 節列的限制。

`USARTx_IRQHandler()` 改法（現有單筆讀取改成排空迴圈，理由見查證
C）：

```c
void
USARTx_IRQHandler(void)
{
    uint32_t sr = USARTx->ISR;
    while (sr & USART_ISR_RXNE) {
        serial_rx_byte(USARTx->RDR);
        sr = USARTx->ISR;
    }
    if (sr & USART_ISR_TXE && USARTx->CR1 & USART_CR1_TXEIE) {
        uint8_t data;
        int ret = serial_get_tx_byte(&data);
        if (ret)
            USARTx->CR1 = CR1_FLAGS;
        else
            USARTx->TDR = data;
    }
}
```

（`while` 迴圈用重新讀 `ISR` 而不是單純判斷 `RDR` 是否還有資料，
是因為 `RXFNE`/`RXNE` 本來就是「FIFO 非空」這個狀態旗標本身，讀
`ISR` 才能正確反映排空後的狀態；這個改法對 `FIFOEN=0` 的其他 MCU
系列一樣安全——非 FIFO 模式下這個迴圈頂多跑一次，行為跟現有程式碼
完全相同。）

### `src/stm32/u5_main.c`：`lookup_clock_line()` 沒辦法直接加

現有 `struct cline` 只支援「一顆 enable 暫存器 + 一顆 optional
reset」，`enable_pclock()`（`clockline.c:12-24`）呼叫模式是「查一次
`cline`、寫一次 `.en`」，不支援「同時寫三顆不同暫存器」——跟
`LPTIM1` 目前的作法一致：`lptim1_wakeup_init()`
（`stm32u5_lowpower.c:172-218`）**完全繞過 `cline`/`enable_pclock`**，
直接手動連寫 `U5_RCC_APB3ENR`/`U5_RCC_APB3SMENR`/`U5_RCC_SRDAMR` 三顆
（該檔行 201-204）。

**跟進同一個模式**，不擴充 `struct cline` 這個共用抽象（擴充它會
影響其他所有呼叫 `enable_pclock()` 的周邊，風險/影響面不成比例）。
具體做法：在 `serial_init()`（`stm32f0_serial.c`，一樣包在
`#if defined(LPUART_BRR)` 底下）於呼叫 `enable_pclock()` 之後，直接
補兩行：

```c
#if defined(LPUART_BRR)
    enable_pclock((uint32_t)USARTx);       // 既有：只設 LPUART1EN
    U5_RCC_APB3SMENR |= (1u << 6);         // 新增：LPUART1SMEN
    U5_RCC_SRDAMR    |= (1u << 6);         // 新增：LPUART1AMEN
#else
    enable_pclock((uint32_t)USARTx);
#endif
```

`U5_RCC_APB3SMENR`/`U5_RCC_SRDAMR` 這兩個巨集目前定義在
`stm32u5_lowpower.c`（行 67-68），`stm32f0_serial.c` 要用需要自己
另外定義同樣的位址巨集（`0x46020CD0`/`0x46020CD8`），或是把這兩個
巨集搬到 `internal.h` 給兩個檔案共用——**這是一個小的架構選擇，本輪
先列出來，留到實作階段再定**。

### `src/stm32/stm32u5_lowpower.c`：`stop2_once()` 加 TC 等待（**階段 2**）

查證 D 的結論：進 `wfi` 前要確認 `TC=1`，含逾時保護。插入位置：現有
`irq_disable()` 之後、`dsb`/`wfi` 之前（跟 `IWDG->KR = 0xAAAA;` 睡前
餵狗同一段，順序上建議放在餵狗之前，避免逾時等待吃掉太多 IWDG
預算）。

★ 逾時不是無害動作，要能看到發生頻率，比照 `restore_timeout`/
`lptim_timeout` 既有的「per-round 記一個 flag + 跑完整組
`cycles` 後總計」兩層模式，新增 `struct stop2_status.tc_timeout`
（0/1，per round）跟 `command_test_stop2()` 裡的累加計數
`tc_timeout_n`，最終送一則獨立的 `sendf`：

```c
/* struct stop2_status 新增一個欄位，跟 entry_fail/restore_timeout/
 * lptim_timeout 同一組「per-round 診斷旗標」 */
uint32_t tc_timeout;
```

```c
{
    /* 2B c1：進 Stop2 前確認 TX 已送完（TC=1），避免 Stop2 期間
     * peripheral clock 被切斷、最後一筆傳輸被截斷（RM0456 行
     * 183305-183310）。含逾時保護，沿用檔案既有的 U5_WAIT_LOOPS
     * 上限慣例（見 (e) 條款），逾時不是致命錯誤（不阻擋、不中止
     * Stop2 流程），但必須算進 tc_timeout 讓 host 端看得到頻率。 */
    volatile uint32_t j;
    uint32_t tc_to = 0;
    for (j = 0; j < U5_WAIT_LOOPS; j++) {
        if (LPUART1->ISR & USART_ISR_TC)
            break;
    }
    if (j >= U5_WAIT_LOOPS)
        tc_to = 1;
    s_status.tc_timeout = tc_to;   /* stop2_capture_status() 呼叫前先存，
                                     * 或把 tc_to 當參數傳進
                                     * stop2_capture_status()，比照
                                     * restore_timeout/lptim_timeout
                                     * 既有的傳參模式，實作時擇一 */
}
IWDG->KR = 0xAAAA;
__asm volatile ("dsb" ::: "memory");
__asm volatile ("wfi");
```

`command_test_stop2()` 端：比照既有 `seg_max`/`seg_sum`/
`cr_first`/`cr_last` 的累加模式，新增 `tc_timeout_n`，每輪累加
`s_status.tc_timeout`，跑完整組 `cycles` 後送一則獨立 `sendf`：

```c
uint32_t tc_timeout_n = 0;
...
for (n = 1; n <= cycles; n++) {
    stop2_once();
    ...
    tc_timeout_n += s_status.tc_timeout;
    ...
}
...
sendf("stop2_tc_timeout n=%u tc_timeout=%u", agg_n, tc_timeout_n);
```

`n=%u` 用既有的 `agg_n`（跑完整組、`!entry_fail` 的輪數，跟
`stop2_lat_max`/`stop2_lat_sum` 的 `n` 同一個基準），`tc_timeout=%u`
是這組測試裡逾時發生的次數——0 代表這組測試完全沒逾時，非 0 就是
第 6 節「若核心不醒」SOP 之外、另一個需要正視的訊號：TC 逾時代表
TX 沒有在合理時間內送完，即使本規格選擇「逾時不阻擋」，頻繁發生
就代表 3.1 節的 TX race 風險不是理論案例，需要回頭檢視。

---

## 3. 風險清單

### 3.1 TX race（查證 D 的延伸）

如果 `command_test_stop2()`／未來的閒置排程在 TX 還在跑（`TC=0`）
時就呼叫 `stop2_once()`，沒有第 2 節的等待邏輯，理論上有機會在字元
傳輸中途進入 Stop2、peripheral clock 被切、最後一個 byte 被截斷、
host 端收到不完整的 frame（`bytes_invalid` 上升）。加了逾時等待後
這個風險被降低但**不是零**——如果逾時發生（`TC` 真的等不到 1，例如
硬體異常或誤判），韌體選擇不阻擋繼續進 Stop2，這種情況下風險依然
存在，只是機率被大幅壓低到「逾時」這種邊緣案例。

### 3.2 FIFO 溢位餘裕計算（驗證設計決策 2）

參數：250000 baud，一個 byte（start+8 data+stop，不含 parity）＝
10 bit period＝**40µs/byte**；FIFO 深度 8 byte（RM0456 Table 686，
[[2b_step1_design]] 已引用）；`total_latency`（喚醒延遲）
121.625µs（`STOPWUCK=1`）~180.938µs（`STOPWUCK=0` 最悲觀界）
（[[2b_g_wake_latency]] 第 5 節）。

**`RXTFCFG=000`（門檻 1 byte，本設計採用）**：

```
觸發時刻：第 1 byte 進 FIFO，RXFT 立刻觸發
喚醒延遲期間又進來的 byte 數 = total_latency / 40µs
  = 121.625/40 ≈ 3.04 byte（STOPWUCK=1）
  = 180.938/40 ≈ 4.52 byte（STOPWUCK=0 最悲觀）
CPU 開始排空時，FIFO 裡累積 ≈ 1 + 3.04~4.52 ≈ 4.0~5.5 byte
FIFO 容量 8 byte，餘裕 ≈ 8 − 5.5 ≈ 2.5 byte（約 100µs）
```

**對照：如果門檻設深（例如 `RXFTCFG=101`＝full＝8 byte）**：

```
觸發時刻：FIFO 已經滿（8 byte）才觸發
RM0456 行 189149-189150 給的餘裕：滿了之後還能再收 1 byte 不算溢位
（(RXFIFO size+1)th 不算溢位，(RXFIFO size+2)th 才算）
喚醒延遲期間又進來的 byte 數：同上，3.04~4.52 byte
遠超過「多 1 byte」的餘裕 → 溢位幾乎必然發生
```

這組算式直接驗證了設計決策 2（淺門檻）：**深門檻在本專案實測的
`total_latency` 量級下會確定溢位，淺門檻（1 byte）留有約 2.5 byte
（~100µs）的餘裕**，跟已完成的 G6/DS13086 量測數字（
[[2b_g_wake_latency]] 第 5、7 節）直接掛鉤，不是憑空假設。

★ 這個算式假設 host 端用固定速率連續灌滿速資料（最壞情境）；
Klipper 實際流量通常是零星的指令/回應，不會真的整段時間都在跑
250000 baud 滿速——**第 4 節的壓力測試就是為了驗證這個理論算式在
真實流量下是否成立**。

### 3.3 若 EXTI 為必要而未設會出現的症狀（本設計判定不需要，見查證 A）

假設性情境（供實作階段診斷比對用）：如果 U5 的 LPUART1 其實跟舊
系列一樣需要 EXTI 邊緣偵測才能喚醒核心，而本規格判斷錯誤、沒有設定
`EXTI_IMR1`/`RTSR1`，預期症狀跟「FIFO 排空邏輯寫錯」會長得不一樣，
可以用來區分：

- **EXTI 缺失型症狀**：MCU 在 Stop2 期間完全不會被 RX 事件喚醒，
  host 端送出的封包會**持續等不到回應直到逾時**——`bytes_retransmit`
  上升為主要訊號，`bytes_invalid` 通常不會動（因為 MCU 根本沒醒來
  處理過那些位元組，不存在「收到但解析錯誤」的資料）；`get_uptime`
  觀察不到任何 MCU 端活動對應到 host 送出的時間點。
- **FIFO 排空/門檻設定錯誤型症狀**（更可能是本設計實際會踩到的
  問題）：MCU **有**醒來（`bytes_retransmit` 不一定動），但部分
  資料遺失或錯位，`bytes_invalid` 上升；如果是溢位（3.2 節的深門檻
  情境），行為會是「陣發性」的——只有在 host 連續送多筆超過餘裕的
  情況下才出現，單筆指令测试可能完全正常，需要靠第 4 節的壓力測試
  才能觸發。

本設計已用查證 A 排除 EXTI 缺失的可能性，這裡列出症狀差異是為了
「如果实作後量測結果怎麼看都不像 3.2 節算出來的那種陣發性溢位樣式，
反而更像完全沒醒來」時，能快速回頭懷疑查證 A 的結論是否有誤，而不是
在 FIFO/門檻邏輯裡找不存在的 bug。

---

## 4. 驗證計畫

### 4.1 基本驗證（沿用 G3/G5/G6 既有手法）

Stop2 期間 host 持續送 Klipper 封包（正常心跳/查詢流量即可），檢查
Moonraker `GET /printer/objects/query?mcu` 的 `last_stats`：

```
bytes_invalid == 0
bytes_retransmit == 0
```

跟 [[2b_step1_design]] 第 7 節訂的判準一致，不重複設計新框架。

### 4.2 壓力測試：睡眠中連續送多封包

基本驗證的流量型態（心跳/查詢）通常是零星的，覆蓋不到 3.2 節算式
假設的「連續多 byte 湧入」情境。壓力測試方法：

1. 讓 MCU 進入 Stop2（透過 c1 實際配置後的閒置路徑，或沿用
   `test_stop2` 類的手動觸發，視實作階段進度而定）。
2. Host 端在 MCU 睡眠期間，**背靠背連續送出多筆指令**（不等前一筆
   回應就送下一筆，或用短間隔連續送出，模擬 3.2 節「連續灌滿速
   資料」的最壞情境），單次測試建議至少涵蓋跨越 FIFO 容量
   （8 byte）等級的資料量（例如連續送 3-5 筆典型長度的 Klipper
   指令，確保累積位元組數超過 8 byte）。
3. 測試後檢查同一組 `bytes_invalid`/`bytes_retransmit`，並比對
   `send_seq`/`receive_seq` 是否對等（現有 stats 欄位，
   [[2b_g_wake_latency]] 各節已經在用）。
4. 如果 3.2 節算式成立，這組測試應該仍然乾淨通過（因為設計決策 2
   的淺門檻在計算上留了約 2.5 byte 餘裕）；如果出現
   `bytes_invalid` 上升，代表實測的喚醒延遲或排空效率比第 5 節/
   3.2 節算式假設的更差，需要回頭檢視是不是有沒算進去的額外延遲
   （例如 ISR 進入本身的延遲、`irq_enable()` 到 ISR 真正執行之間的
   間隔——這些在 3.2 節的算式裡沒有單獨列出，只用 `total_latency`
   概算）。

兩組測試（4.1 基本、4.2 壓力）都通過，才視為 c1 的 RX 路徑驗證完成；
TX race（3.1）目前沒有專門的自動化驗證手法，暫時只能靠 4.1/4.2
測試期間如果剛好命中「Stop2 進入時 TX 未完成」這個時間點、
觀察是否出現對應的資料錯誤來間接佐證，**沒有專門針對 3.1 設計的
定向測試，列為本規格的已知覆蓋缺口**。

---

## 5. 若核心不醒：分辨 SOP

如果實作後量測發現 MCU 在 Stop2 期間對 host 送來的資料完全沒反應
（比 3.3 節「FIFO 排空/門檻設定錯誤」的陣發性症狀更極端——完全
沒醒過），**先分辨是「有收到但喚醒路徑不通」還是「連收都沒收到」，
這兩條路要查的東西完全不同，事先寫好避免亂試**：

```
$ sudo systemctl stop klipper
$ sudo openocd -f /home/arduino/uno_q.cfg -c "halt" \
    -c "mdw 0x4600241C" -c "resume" -c "shutdown"
```

`0x4600241C` = `LPUART1_ISR`（`LPUART1` base `0x46002400` + offset
`0x1C`，RM0456 行 189098）。讀回值逐位元判讀（`FIFOEN=1` 時的
layout，RM0456 行 189108）：

- **`RXFT`（bit 26，`0x04000000`）或 `RXFNE`（bit 5，`0x00000020`）
  已經置位** → **資料有收到，FIFO 裡躺著東西，但喚醒路徑不通**。
  這代表 LPUART1 本身正常運作（`UESM`/`RCC` 三個致能位元、`FIFOEN`/
  `RXFTCFG` 都生效），問題出在「FIFO 有東西之後，為什麼沒有把 CPU
  從 `wfi` 叫醒」。**查證 A（不需要 EXTI）在這個情境下算是被
  推翻的負面證據——回頭檢查**：
  1. `NVIC_ISER2` 裡 `LPUART1_IRQn`(66) 對應的 bit（`66-64=2`）是否
     真的是 1（`mdw` 讀 NVIC ISER2，位址視 CMSIS 定義，比照
     `stop2_once()` 裡 `NVIC_ICPR2`/`NVIC_ISER2` 既有用法）；
  2. 重新檢視查證 A 的結論是否真的適用這顆矽片/這個 revision（本輪
     查證只查了 RM0456 文字，沒有拿示波器/邏輯分析儀實測過「RXFT
     觸發瞬間到 NVIC pending 之間」有沒有中間步驟被遺漏）；
  3. `RXFTIE`（`CR3` bit28）是不是真的寫進去了、有沒有被後續某次
     `CR3` 整顆覆寫蓋掉（本規格的改法是 `CR3 = OVRDIS | RXFTCFG_000
     | RXFTIE` 一次寫齊，理論上不會，但要確認沒有其他程式碼路徑
     後來又寫了一次 `CR3`）。
- **旗標全空**（`RXFT`/`RXFNE`/`RXFF` 都是 0） → **連收都沒收到**，
  問題出在資料根本沒進到 LPUART1，或 LPUART1 根本沒在低功耗模式下
  運作。查：
  1. `SMEN`/`AMEN`（`RCC_APB3SMENR`/`RCC_SRDAMR` bit6）是否真的被
     設起來（`mdw 0x46020CD0`/`mdw 0x46020CD8`，核對 bit6）；
  2. `UESM`（`LPUART1_CR1` bit1，`mdw 0x46002400`）是否真的被設起來
     （檢查有沒有被 `serial_init()` 之後的某次 `CR1` 寫入覆蓋掉——
     跟 `TE`/`RE` 一樣容易在整顆覆寫時漏掉）；
  3. 供電/GPIO 層級：`PG7`/`PG8`（`RESERVE_PINS_serial`）腳位功能
     是否還是 LPUART1（`gpio_peripheral()` 呼叫有沒有被某處改動）；
  4. 用示波器/邏輯分析儀直接量 `PG8`（RX 腳）在 host 送資料時有沒有
     真的出現訊號——排除「host 端根本沒送到板子」這種更底層的
     可能性。

這兩條路徑查的暫存器完全不重疊（第一條查 NVIC/中斷路徑，第二條查
RCC 致能鏈跟腳位），**先用 `mdw 0x4600241C` 分岔，再決定往哪條路
查，不要兩條同時亂試**。

---

## 6. Step 2 通過標準

**Step 2 的通過標準是「Stop2 期間不掉 byte」**——即第 4 節 4.1/4.2
兩組測試（穩態流量 + 連續多封包壓力測試）都乾淨通過
（`bytes_invalid=0`、`bytes_retransmit=0`），加上階段 2 的
`tc_timeout` 計數為 0 或落在可接受範圍（本規格沒有明訂「可接受的
非零次數」，若出現非零，回到第 3.1 節、視發生頻率決定是否要繼續
往下、還是先解決 TX race 再繼續）。

★ **明確不包含「klippy 長期連線」這個判準**：`console.py`（第 4 節
用來驅動測試的工具）**不會跑 clocksync**（Klipper host 端用來把
MCU 的 `clock`／`CYCCNT` 換算回真實時間的機制），而 Stop2 睡眠期間
`DWT->CYCCNT` 是停的（[[2b_g_wake_latency]] 開頭就講過
「Stop2 期間核心時脈停止，`DWT->CYCCNT` 與所有以它為底的計時全部
凍結」）——`CYCCNT` 一停，`clocksync` 拿掉的時間基準就會失準，這個
失準怎麼補償（讓真正的 `klippy`／Moonraker 長時間連線在 Stop2 睡眠
下依然能正確換算時間戳），**是 step 3 的主體工作，不在 step 2
範圍內**。Step 2 只驗證「LPUART1 這一層的收發正確性」，不驗證
「長時間掛著 klippy daemon、Stop2 睡眠會不會把 clocksync 搞壞」——
兩件事分開驗證，避免 step 2 的結論被 step 3 才要解決的問題污染。
