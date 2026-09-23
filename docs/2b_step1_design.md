# Milestone 2B step 1 — c1 設計盤點（唯讀）

延續 [[2b_g_wake_latency]] 第 7 節（G6）：`HSIRDY` 在 Stop2 醒來瞬間、
任何軟體寫入之前就是 1，證實假說 (B)——HSI16 在這份韌體的 Stop2 睡眠
期間全程沒有關閉。方案 **c1** 把這個既有事實當前提，不追求「按需喚醒
HSI16」（那是 [[2b_step0_lpuart_lse]] A2 討論的另一種、目前不成立的
情境），改成問：**既然 HSI16 已經在跑，LPUART1 要怎麼設定，才能在
Stop2 睡眠期間直接用 SRD autonomous mode 收 250000 baud，讓 host 送
封包時 MCU 能正確喚醒、不掉資料？**

本輪純查證＋讀碼，**沒有改韌體、沒有碰暫存器**。

---

## 1. RM0456 Rev 7 查證（附行號）

### 1a. LPUART1 在 Stop2 下運作需要哪些 RCC 致能位元

三顆暫存器、都是 bit 6（跟現有 `stm32u5_lowpower.c` 裡 LPTIM1 用
bit 11 是同一組三連發模式，位址完全對應）：

| 暫存器 | offset | Bit | 名稱 | 作用 | RM0456 行號 |
|---|---|---|---|---|---|
| `RCC_APB3ENR` | `0x0A8` | 6 | `LPUART1EN` | LPUART1 一般匯流排時脈致能——沒這個位元，連暫存器都存取不到 | 38216-38219 |
| `RCC_APB3SMENR` | `0x0D0` | 6 | `LPUART1SMEN` | Sleep/Stop 模式下時脈閘控是否放行——註解明講「must be set to allow the peripheral to wake up from Stop modes」 | 39384-39389 |
| `RCC_SRDAMR` | `0x0D8` | 6 | `LPUART1AMEN` | Stop 0/1/2 模式下的 **autonomous mode** 致能——註解同樣是「must be set to allow the peripheral to wake up from Stop modes」，但這是獨立於 `LPUART1SMEN` 的另一顆開關，管的是「SRD（Smart Run Domain）autonomous 子系統」這條路徑，不是單純的匯流排時脈閘 | 39541-39546 |

**`LPUART1SMEN` 跟 `LPUART1AMEN` 的差異**（RM0456 沒有直接對照表，
但從兩處註解與所在暫存器名稱可以推斷）：`APB3SMENR` 這組暫存器是
「Sleep/Stop 模式下要不要維持 APB 匯流排時脈閘」的通用機制，涵蓋
`APB3ENR` 底下所有周邊；`SRDAMR`（Smart Run Domain Autonomous Mode
Register）則是**只對「有 autonomous mode 能力」的周邊**（LPUART1、
LPTIM1/3/4、I2C3、SPI3、ADC4、DAC1、ADF1 等，行 28383）額外開的一條
「即使核心睡著，周邊自己請求 APB/kernel clock 去檢查狀態、產生中斷/
DMA」的路徑（§67.4.15，見 1c）。兩個都要設，缺一個都不能從 Stop2
喚醒——現有韌體（見第 2 節）**兩個都沒設**。

### 1b. UESM（CR1）在 U5 上是否存在、作用為何

**存在**。`LPUART_CR1` bit 1，行 188386-188394：

> Bit 1 UESM: LPUART enable in low-power mode
> When this bit is cleared, the LPUART cannot request its kernel clock
> and is not functional in low-power mode.
> When this bit is set, the LPUART can wake up the MCU from low-power
> mode.
> Note: The UESM bit must be set at the initialization phase.

§67.4.15「LPUART autonomous mode」（行 187954-187960）補一句更明確的
時機要求：「The UESM bit must be set prior to entering low-power
mode.」——這是**跟 `LPUART1SMEN`/`LPUART1AMEN` 平行的第三個必要條件**，
一個在 RCC（決定時脈閘放不放行），一個在 LPUART 自己的 CR1（決定
周邊本身願不願意在低功耗模式下運作）。少一個都不會動。

### 1c. FIFO 模式（CR1.FIFOEN）與 RXFT/RXFF 中斷在 Stop2 下的行為

`FIFOEN`：`LPUART_CR1` bit 29，行 188234-188239，單純的 FIFO 開關。

§67.4.15 接收段（行 188008-188018）逐行列出「FIFO 模式下，APB clock
（進而喚醒）在什麼條件下被請求」：

> If the FIFO mode is enabled, the APB clock is requested when
> – The RxFIFO is full (RXFF = 1) and the corresponding interrupt is
>   enabled (RXFFIE = 1)
> – The RxFIFO threshold is reached (RXFT = 1) and the corresponding
>   interrupt is enabled (RXFTIE = 1)
> – The RxFIFO is not empty (RXFNE = 1) and the corresponding
>   interrupt or DMA is enabled (RXFNEIE = 1)

三選一（或並存）。`RXFTIE`（CR3 bit 28，行 188763-188768）跟
`RXFTCFG[2:0]`（CR3 bits 27:25，行 188781-188789，可選 1/8, 1/4,
1/2, 3/4, 7/8, full 六種深度）**只能在 `UE=0` 時寫**（行 188789 明講
「This bit can only be written when the LPUART is disabled (UE=0)」，
`RXFTCFG` 也有同樣限制），所以 init 時序必須是「先配 `FIFOEN`/
`RXFTCFG`/`RXFTIE`（`UE=0`），最後才把 `UE` 一起設起來」，不能像現有
`serial_init()` 那樣一次寫死 `CR1_FLAGS`（見第 2 節）。

★ **`RXFF` 在 Stop 模式下有一個文件明講的陷阱**（Table 694 註 2，
行 188122-188126）：

> In Stop mode, LPUART_RDR is not clocked. As a result, this register
> is not written and once n data are received and written in the
> RXFIFO, the RXFF interrupt is asserted (RXFF flag is not set).

也就是說 `RXFF` 中斷**會觸發**，但 `RXFF` 這個**旗標本身在 Stop 模式下
不會被設起來**（因為它的定義依賴 `LPUART_RDR` 有沒有多收到 n+1 筆，
而 `RDR` 這時沒被 clock）——如果 ISR 依賴讀 `RXFF` 旗標來判斷「是不是
滿了」會撲空。**結論：Stop2 下要用 `RXFT`，不要用 `RXFF`**，`RXFT`
沒有這個「旗標不會被設」的陷阱（`RXFT` 的定義是「收到 `RXFTCFG` 筆」，
不依賴 `RDR`）。

`RXNE`（非 FIFO 模式）/`RXFNE`（FIFO 模式）共用同一個 bit（ISR
bit 5，現有韌體 `internal.h` 定義成 `USART_ISR_RXNE = 0x0020`，見第
2 節），FIFO 模式下語意從「收到一筆」變成「FIFO 非空」，這是
Table 694 裡「Exit from Stop = Yes」的另一個可用事件，但每收一筆就會
再度變成非空、可能每 byte 觸發一次喚醒——如果目的是「累積一批再喚醒」
（呼應 [[2b_g_wake_latency]] 第 5 節提過的 320µs＝8 級 FIFO 緩衝時間
判準），應該用 `RXFT` 設一個接近滿（例如 7/8 或 full）的門檻，不是
`RXFNE`。

### 1d. 喚醒核心的事件旗標與 NVIC 需求

**建議用 `RXFT`（CR3.RXFTIE=1，`RXFTCFG` 設到接近/等於 FIFO 深度）
當喚醒事件**，理由見 1c（`RXFF` 有旗標陷阱、`RXFNE` 太頻繁）。

NVIC 端：`LPUART1_IRQn = 66`（`internal.h:85`，跟 RM0456 中斷向量表
「66 LPUART1 LPUART1 global interrupt」，行 60303，位址 `0x148` 對得
上），**已經在 `serial_init()` 裡透過 `armcm_enable_irq()`
（`src/generic/armcm_boot.h:13-17`，內部呼叫 CMSIS `NVIC_EnableIRQ`）
致能過一次，且 `stop2_once()` 全程沒有再碰過這顆 IRQ 的 NVIC 位元**
（見第 3 節）——理論上不需要為了 Stop2 額外做 NVIC 設定。

但要注意 `stop2_once()` 從函式開頭 `irq_disable()`（`cpsid i`，
PRIMASK=1）到收尾 `irq_enable()` 之間全程遮罩：這**不影響 `wfi` 本身
能不能被喚醒**（Cortex-M 架構上，`wfi` 的喚醒條件是「有 NVIC 致能的
中斷變成 pending」，跟 PRIMASK 無關，PRIMASK 只決定醒來後會不會真的
跳進 ISR 執行）——這正是現有 LPTIM1 那段沿用的模式（見
`stop2_once()` 開頭註解，`src/stm32/stm32u5_lowpower.c:220-223`）。
如果 c1 要用同一套「PRIMASK=1 全程遮罩、醒來後直接輪詢旗標」的模式，
需要輪詢 `LPUART1->ISR` 的 `RXFT` 旗標，而不是依賴 `LPUART1_IRQHandler`
真的跑過。

---

## 2. 現有韌體盤點

### `src/stm32/stm32f0_serial.c`（LPUART1 的實際驅動，儘管檔名是
`stm32f0_serial.c`——這個檔案是 F0/G0/G4/H7/U5 共用的通用 USART/
LPUART 驅動，透過 `#elif CONFIG_STM32_SERIAL_LPUART1` 分支選中
`LPUART1`）：

- `serial_init()`（行 169-189）：
  ```c
  USARTx->CR3 = USART_CR3_OVRDIS;     // 只關掉 ORE 中斷，沒有 FIFO/RXFT 設定
  USARTx->CR1 = CR1_FLAGS;            // UE|RE|TE|RXNEIE，一次寫死
  armcm_enable_irq(USARTx_IRQHandler, USARTx_IRQn, 0);
  ```
  `CR1_FLAGS`（行 144-145）= `USART_CR1_UE | USART_CR1_RE | USART_CR1_TE
  | USART_CR1_RXNEIE`——**沒有 `UESM`、沒有 `FIFOEN`**，`CR3` 也**沒有
  `RXFTIE`/`RXFTCFG`**。
- `USARTx_IRQHandler()`（行 147-161）：
  ```c
  uint32_t sr = USARTx->ISR;
  if (sr & USART_ISR_RXNE)
      serial_rx_byte(USARTx->RDR);
  ```
  **只讀一筆就返回，不是 `while` 排空。** 目前 FIFO 沒開，`RXNE`
  語意上就是「一筆」，這樣寫沒問題；但如果照 1c/1d 改成 FIFO+`RXFT`
  門檻式喚醒（一次醒來時 FIFO 裡可能已經堆了到門檻深度那麼多筆），
  這支 handler **必須改成 `while (USARTx->ISR & USART_ISR_RXNE)`
  迴圈排空**，否則一次中斷只讀一筆，其餘筆數留在 FIFO 裡，下一筆
  進來才會再觸發（如果門檻設定在 FIFO 深度以下，語意上還能撐著；
  如果門檻設到 full，多出來的資料會被覆蓋／`ORE` 溢位，且目前
  `OVRDIS=1` 讓溢位完全靜默，見第 4 節風險）。

### `src/stm32/internal.h`（STM32U585 手動暴力映射區，行 69-100）

`LPUART_TypeDef` 是手刻的 struct（`CR1/CR2/CR3/BRR/PRESC/_res/RQR/
ISR/ICR/RDR/TDR`，跟 RM0456 §67.7 的暫存器順序、offset 一致），但只
定義了目前用得到的位元巨集：

```c
#define USART_ISR_RXNE   0x0020U
#define USART_ISR_TXE    0x0080U
#define USART_ISR_ORE    0x0008U
#define USART_CR1_UE     0x0001U
#define USART_CR1_RE     0x0004U
#define USART_CR1_TE     0x0008U
#define USART_CR1_RXNEIE 0x0020U
#define USART_CR1_TXEIE  0x0080U
#define USART_CR3_OVRDIS 0x1000U
```

**沒有 `USART_CR1_UESM`（應為 `0x0002`）、`USART_CR1_FIFOEN`（應為
`0x20000000`）、`USART_CR3_RXFTIE`（應為 `0x10000000`）、
`USART_CR3_RXFTCFG` 相關巨集**——這些位元巨集本身在這份手刻標頭裡
還不存在，要做 c1 得先補上。

### `src/stm32/u5_main.c`（`lookup_clock_line()`／`get_pclock_frequency()`）

```c
if (periph_base == 0x46002400UL) /* LPUART1 */
    return (struct cline){ .en = U5_RCC_APB3ENR, .rst = NULL, .bit = (1u << 6) };
```

`enable_pclock()`（`src/stm32/clockline.c:12-24`）只會動 `.en` 這一顆
暫存器——也就是說**目前整個呼叫鏈只設了 `LPUART1EN`（`APB3ENR`
bit 6），`LPUART1SMEN`（`APB3SMENR`）跟 `LPUART1AMEN`（`SRDAMR`）
完全沒有地方寫過**，跟 1a 查到的缺口精確對應。`struct cline` 這個
抽象只支援「一顆 enable + 一顆 optional reset」，不支援「三顆並列
enable」這種 LPTIM1/LPUART1 才有的 Stop2-wake 模式——現有 LPTIM1
的作法是**繞過 `cline`/`enable_pclock`，直接在
`stm32u5_lowpower.c` 手動連寫三顆暫存器**（見下方 3、和第 4 節的
架構筆記）。

### `get_pclock_frequency()`

```c
if (periph_base == 0x46002400UL) /* LPUART1: HSI16 */
    return 16000000;
```

確認 LPUART1 kernel clock = HSI16、16MHz 固定值（跟 `stm32u5.c:254`
`U5_RCC_CCIPR3 = (... ) | (2u << 0)` 選 HSI16 一致），`BRR` 計算
（`serial_init()` 裡 `DIV_ROUND_CLOSEST(pclk*256, baud)`）用的就是這
個 16MHz，跟 FIFO/UESM 設定無關，不需要因為 c1 而改。

---

## 3. `stop2_once()` 現況：完全沒碰 LPUART1

逐行核對 `src/stm32/stm32u5_lowpower.c` 的 `stop2_once()`
（272-424 行）：只寫 `LPTIM1_CR/CFGR/DIER/ARR`、`SYSTICK_CTRL`、
`SCB_ICSR`、`PWR_CR1`、`SCB_SCR`、`IWDG->KR`——**沒有任何一行碰
`LPUART1`（`0x46002400`）或 `NVIC_ISER2` 裡 `LPUART1_IRQn`（bit 2）
對應的位元**。換句話說：

- **好消息**：進 Stop2 前不需要「先停掉 LPUART」——目前的 Stop2
  進入序列本來就不會動 LPUART1，跟 c1「LPUART1 要在 Stop2 期間持續
  工作」的需求不衝突，不需要在 `stop2_once()` 裡新增「停用」邏輯。
- **缺口**：也因為完全沒碰，`UESM`/`FIFOEN`/`RXFTIE`/`RXFTCFG`/
  `LPUART1SMEN`/`LPUART1AMEN` 這些**必須在「進入 Stop2 之前」就設好**
  的位元，現在完全沒有任何程式碼路徑會去設——不是「`stop2_once()`
  需要拆掉什麼」，而是「`serial_init()`（冷開機一次性初始化）跟/或
  一個新的 Stop2-wake 專用初始化函式，需要在 c1 生效前就把這些位元
  設好，`stop2_once()` 本身大概率不用改」。
- `stop2_once()` 目前唯一的喚醒來源判斷邏輯是輪詢 `LPTIM1->ISR`
  （透過 `command_test_stop2()` 外層邏輯判讀 `entry_fail`/
  `restore_timeout`/`sws`，`stop2_once()` 本身不分辨「這輪是被誰
  叫醒的」）——如果 c1 要讓 LPUART1 也能獨立喚醒 MCU（不透過
  `test_stop2` 那條人工排定週期的路徑），**現有函式沒有任何機制去
  分辨「這次是 LPTIM1 逾時、還是 LPUART1 收到資料」**，這是第 4 節
  列的風險之一。

---

## 4. c1 所需的暫存器設定清單（總表）

| 暫存器 | Bit | 動作 | 時機限制 | RM0456 行號 |
|---|---|---|---|---|
| `RCC_APB3ENR` | 6 (`LPUART1EN`) | 設 1（**已經設好**，`enable_pclock` 現有路徑已做） | 任意 | 38216 |
| `RCC_APB3SMENR` | 6 (`LPUART1SMEN`) | 設 1（**缺**） | 進 Stop2 前 | 39384 |
| `RCC_SRDAMR` | 6 (`LPUART1AMEN`) | 設 1（**缺**） | 進 Stop2 前 | 39541 |
| `LPUART1_CR1` | 1 (`UESM`) | 設 1（**缺**） | 初始化階段，且需 `UE=0` 時的整體 CR1 規劃裡一併設 | 188386 |
| `LPUART1_CR1` | 29 (`FIFOEN`) | 設 1（**缺**） | `UE=0` 時寫 | 188234 |
| `LPUART1_CR3` | 27:25 (`RXFTCFG`) | 設到接近/等於 FIFO 深度（例如 `101`=full，或 `100`=7/8） | `UE=0` 時寫 | 188781 |
| `LPUART1_CR3` | 28 (`RXFTIE`) | 設 1（**缺**） | 任意，建議跟 `RXFTCFG` 同批 | 188763 |
| `LPUART1_CR1` | 0 (`UE`) | 維持現有邏輯，但要排在 `FIFOEN`/`RXFTCFG` 之後才寫 | — | 既有 |
| NVIC | `LPUART1_IRQn`(66) | **已致能**（`serial_init()` 既有呼叫），c1 不用新增 | — | `armcm_boot.h:13-17` |

---

## 5. 需要修改的韌體檔案與函式清單（供下一步實作參考，本輪不動）

1. **`src/stm32/internal.h`**：補 `USART_CR1_UESM`(`0x0002`)、
   `USART_CR1_FIFOEN`(`0x20000000`)、`USART_CR3_RXFTIE`
   (`0x10000000`)、`USART_CR3_RXFTCFG_Pos`/對應位遮罩 幾個巨集。
2. **`src/stm32/stm32f0_serial.c`**：
   - `serial_init()`：拆成兩段寫 `CR1`——先寫 `FIFOEN`（`UE=0`）跟
     `CR3` 的 `RXFTCFG`/`RXFTIE`，最後才寫入含 `UE|RE|TE|RXNEIE|
     UESM` 的完整 `CR1`。這個檔案是 F0/G0/G4/H7/U5 共用的，改動要用
     `#if defined(LPUART_BRR)` 或新的 `CONFIG_MACH_STM32U585` 判斷
     包住，不能影響其他 MCU 系列的既有行為。
   - `USARTx_IRQHandler()`：`if (sr & USART_ISR_RXNE)` 改成
     `while (USARTx->ISR & USART_ISR_RXNE)` 排空迴圈（理由見第 2
     節）。
3. **`src/stm32/u5_main.c`** 或新增一小段初始化（比照
   `stm32u5_lowpower.c` 裡 `lptim1_wakeup_init()` 手動連寫三顆 RCC
   暫存器的模式）：設 `RCC_APB3SMENR`/`RCC_SRDAMR` 的 bit 6。放在
   `u5_main.c`（`lookup_clock_line()` 擴充，但目前 `struct cline`
   不支援三連發）還是新開一個函式（比照 LPTIM1 的作法，繞過
   `cline`）是一個待決定的架構選擇，本輪不下結論。
4. **`src/stm32/stm32u5_lowpower.c`**：`stop2_once()` 本身大機率
   **不需要改動**（見第 3 節），但如果要讓 c1 有能力分辨「這輪是
   LPTIM1 逾時還是 LPUART1 收資料叫醒」，`stop2_capture_status()`/
   `struct stop2_status` 可能需要新增一個欄位記錄
   `LPUART1->ISR & USART_ISR_RXFT`（比照 G6 新增 `cr_at_wake` 的
   模式），這屬於「量測/除錯」而非「c1 運作本身必要」，列為可選。

---

## 6. 風險與未知項

1. **`RXFF` 的旗標陷阱**（1c 已詳述）：確定要用 `RXFT`，不用
   `RXFF`，這點已經有 RM0456 明文佐證，不算開放風險，只是必須遵守
   的限制。
2. **`OVRDIS=1` 跟門檻式喚醒的張力**：目前 `serial_init()` 設
   `USART_CR3_OVRDIS`，把溢位（`ORE`）中斷整個關掉。RM0456
   §67.4.15 接收段有一條專門講溢位在低功耗模式下的行為（行
   188019-188021）：

   > The APB clock is requested in reception mode when an overrun
   > error occurs (ORE = 1). The EIE bit must be set to enable the
   > generation an interrupt and waking up the MCU, and the OVRDIS
   > bit must remain cleared.

   溢位事件要能喚醒 MCU，需要 `EIE=1` 且 `OVRDIS` 必須保持清除
   （`OVRDIS=0`）——跟現有 `OVRDIS=1` 直接衝突。如果 c1 用門檻式喚醒（例如
   `RXFTCFG=full`），host 端送資料的速度如果在 MCU 這次醒來、排空、
   回到下一次 Stop2 睡眠之間又超過 FIFO 深度，會不會溢位、溢位後
   `OVRDIS=1` 會不會讓這筆遺失被完全靜默（沒有任何旗標/中斷通知）
   ——**這是本輪查證留下的最大未知項**，需要先決定「c1 要不要保留
   `OVRDIS=1`」這個取捨，才能往下設計。
3. **`stop2_once()` 目前只認得 LPTIM1 這一個喚醒來源**（第 3 節）：
   c1 若要接進 Klipper 真正的閒置排程（不是現在這種
   `command_test_stop2()` 人工測試路徑），需要一套「分辨這輪 WFI
   是被誰叫醒」的機制，現有程式碼完全沒有這塊，屬於**未設計**的
   範圍，不只是「小改動」。
4. **`RXFTCFG` 深度選擇沒有結論**：選 `full`（8/8）能爭取到最長的
   320µs 緩衝視窗（呼應 [[2b_g_wake_latency]] 第 5 節算出的
   `total_latency` 121.625~180.938µs，兩者都在 320µs 之內有餘裕），
   但「滿了才醒」代表最壞情況下（收到第 1 筆到收到第 8 筆之間）resp
   latency 會被拉到接近一整個 FIFO 深度的時間，這對 Klipper 協定的
   即時性有沒有影響，本輪沒有分析。選較淺的門檻（例如 1/2 或 1/4）
   反應更即時，但喚醒更頻繁、Stop2 睡眠時間被切得更碎，兩者的取捨
   需要實測資料才能下結論，本輪只列出選項，不下決定。
5. **本輪完全沒有驗證 `LPUART1AMEN`/`LPUART1SMEN`/`UESM` 三個位元
   實際生效後，LPUART1 的 kernel clock request 路徑會不會跟 G6 確認
   的「HSI16 全程開著」產生任何交互作用**——理論上不會（HSI16 已經
   在跑，autonomous mode 的「請求 kernel clock」步驟等於瞬間就能
   拿到），但這是推論，不是實測，真正的行為要等這些位元被實際設起來
   才能確認，本輪純屬讀碼／查表，沒有碰過任何一顆暫存器。

---

## 7. 驗證方式（供下一步實作後使用，本輪未執行）

**核心判準**：MCU 進 Stop2 睡眠期間，host 持續送 Klipper 封包
（正常的心跳/查詢流量即可，不需要額外造流量），檢查 Moonraker
`GET /printer/objects/query?mcu` 回傳的 `last_stats`：

```
bytes_invalid == 0      # 沒有收到解不出來的封包（framing/parity/
                         # 排空邏輯出錯都會反映在這裡）
bytes_retransmit == 0   # host 沒有因為逾時/序號對不上而重送
```

這兩個數字非零，直接對應第 6 節列的風險：`bytes_invalid` 上升指向
排空邏輯（`while` 迴圈沒改對、或 `RXFT` 門檻設定跟 FIFO 深度沒對齊）
或溢位（風險 2）；`bytes_retransmit` 上升指向喚醒延遲太長、host 端
逾時重送（呼應 `total_latency` vs 320µs 判準，[[2b_g_wake_latency]]
第 5 節）。

跟現有 G3/G5/G6 的驗證手法一致：`ssh unoq` 讀 Moonraker API，
`state=ready` 是前提，`bytes_retransmit`/`bytes_invalid` 是主判準，
延續同一套判讀習慣，不需要另外設計新的驗證框架。
