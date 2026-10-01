# Milestone 2B D — 除錯模式假象：DBG_STOP 讓本專案至今所有「Stop2」測試都不是真正的 Stop2

本文件記錄一個方法學層級的重大發現：本專案從 Milestone 2A 到 2B step 4
為止，所有透過 OpenOCD 燒錄/量測的 Stop2 測試，MCU 實際上都在
`DBGMCU_CR.DBG_STOP=1` 的狀態下執行——這個位元會讓**所有時脈與振盪器
在 Stop 模式期間繼續運作**，核心實際上只是停在一般 Sleep，從未真正進入
省電意義上的 Stop2。這代表過去所有「量到 Stop2 行為」的結論，都可能只
是除錯模式的假象，不是真正低功耗模式下的行為。

本文件只整理既有發現、不做新測試、不碰硬體。

---

## 1. 發現：OpenOCD 每次連線都會把 DBG_STOP/DBG_STANDBY 設起來

來源：`/opt/openocd/stm32x5x_common.cfg`（這是本板實際燒錄流程用的檔案，
`uno_q.cfg` → `source [find target/stm32u5x.cfg]` → `source [find
stm32x5x_common.cfg]`，不是隨便哪個同名檔案）。

第 120-140 行，`$_TARGETNAME configure -event examine-end`（逐字）：

```tcl
$_TARGETNAME configure -event examine-end {
global _ENABLE_LOW_POWER
global _STOP_WATCHDOG

	if { $_ENABLE_LOW_POWER == 1 } {
		# Enable debug during low power modes (uses more power)
		# DBGMCU_CR |= DBG_STANDBY | DBG_STOP
		mmw 0xE0044004 0x00000006 0        # 第 127 行
	} else {
		# Disable debug during low power modes
		# DBGMCU_CR |= ~(DBG_STANDBY | DBG_STOP)
		mmw 0xE0044004 0 0x00000006
	}
	if { $_STOP_WATCHDOG == 1 } {
		# Stop watchdog counters during halt
		# DBGMCU_APB1_FZ |= DBG_IWDG_STOP | DBG_WWDG_STOP
		mmw 0xE0044008 0x00001800 0         # 第 136 行
	} else {
		...
	}
```

`_ENABLE_LOW_POWER` 在本板的實際環境下恆為 1（每次連線後讀
`DBGMCU_CR` 都是 `0x6`，見下方 R3 量測，以及本輪之前每一次用
`mdw 0xE0044004` 的結果），所以**每一次 OpenOCD 連線（燒錄、halt、
mdw 任何東西）都會無條件把 `DBGMCU_CR` 的 `DBG_STOP`/`DBG_STANDBY`
設成 1**。這不是本專案的設定失誤，是 OpenOCD 官方 `stm32x5x_common.cfg`
的標準行為（給一般除錯情境用的預設），只是我們一直沒注意到它跟
「量測真正 Stop2 行為」這個目的互相衝突。

> 註：先前（壓縮前）的筆記曾引用「第 96-98 行」，經本輪重新核對
> `/opt/openocd/stm32x5x_common.cfg` 現場內容後確認不準確（可能是記錯
> 版本或路徑），上面的 113-140 行／127／136 行是本輪直接用 `sed`/`grep`
> 從板上現場取得、逐字核對過的結果，以此為準。

### RM0456 對 `DBG_STOP` 的原文

**DBGMCU_CR Bit 1，§ "DBGMCU configuration register"（行 237736-237741）**：

> Bit 1 DBG_STOP: Allows debug in Stop mode
>   0: normal operation
>      All clocks are disabled automatically in Stop mode.
>   1: automatic clock stop disabled
>      All active clocks and oscillators continue to run during Stop mode, allowing full debug
>      capability. On exit from Stop mode, the clock settings are set to the Stop mode exit state.

**行 34121-34122**：

> The CPU deep-sleep mode can be overridden for debugging by setting the DBG_STOP or
> DBG_STANDBY bit in the DBGMCU_CR register.

**§75.12.2 "Low-power mode emulation"，行 237451-237457（RM 自己的警告）**：

> When the device enters either Stop mode (clocks are stopped) or Standby mode (core
> power is switched off), the debugger can no longer access the debug access port and loses
> the connection with the device. To avoid this, the debugger (or software) can set
> DBG_STANDBY and/or DBG_STOP in DBGMCU_CR. These bits, when set, maintain the
> clock and power to the processor while the device is in the corresponding low-power mode.
> The processor remains in Sleep mode, and exits the low-power mode in the normal way.
> **Peripheral devices continue to operate, so the device behaviour may not be identical to
> the one in the actual low-power mode.**

RM0456 自己明講：設了這個位元之後，核心「remains in Sleep mode」（不是
真的 Stop），且「device behaviour may not be identical to the one in the
actual low-power mode」。這句話就是本文件存在的理由。

---

## 2. 量化證據

### 2.1 `DBG_STOP=1`（即本專案至今所有測試的實際條件）：`cyc_across_sleep` 與 `arr_used` 成正比

2B step 5a 的 `stop2_once_budget()` 在 `wfi` 前後各讀一次
`DWT->CYCCNT`，相減得到 `cyc_across_sleep`——理論上，若 Stop2 期間
`CYCCNT` 真的凍結，這個值應該是一個跟睡多久無關的小常數（純粹是
`dsb`/`wfi`/`isb` 本身的執行開銷）。實測結果完全不是這樣：

**S1（`max_sleep_ms=5 cycles=20`）**

| cycle | arr_used | cyc_across | ratio=cyc/arr |
|---|---|---|---|
| 1 | 160 | 21002 | 131.26 |
| 2 | 160 | 21012 | 131.33 |
| 3 | 160 | 21047 | 131.54 |
| 4 | 160 | 21043 | 131.52 |
| 5 | 160 | 21032 | 131.45 |
| 6 | 160 | 21020 | 131.38 |
| 7 | 160 | 21010 | 131.31 |
| 8 | 160 | 21014 | 131.34 |
| 9 | 160 | 21023 | 131.39 |
| 10 | 160 | 21042 | 131.51 |
| 11 | 160 | 21037 | 131.48 |
| 12 | 160 | 21015 | 131.34 |
| 13 | **82** | **11019** | 134.38 |
| 14 | 160 | 21022 | 131.39 |
| 15 | 160 | 21010 | 131.31 |
| 16 | 160 | 21016 | 131.35 |
| 17 | 160 | 21019 | 131.37 |
| 18 | 160 | 21016 | 131.35 |
| 19 | 160 | 21026 | 131.41 |
| 20 | 160 | 21029 | 131.43 |

**S2（`max_sleep_ms=20 cycles=20`）**

| cycle | arr_used | cyc_across | ratio |
|---|---|---|---|
| 1 | 640 | 82536 | 128.96 |
| 2 | 640 | 82619 | 129.09 |
| 3 | 640 | 82578 | 129.03 |
| 4 | 640 | 82618 | 129.09 |
| 5 | 640 | 82656 | 129.15 |
| 6 | 640 | 82673 | 129.18 |
| 7 | **344** | **44640** | 129.77 |
| 8 | 640 | 82623 | 129.10 |
| 9 | 640 | 82534 | 128.96 |
| 10 | 640 | 82674 | 129.18 |
| 11 | 640 | 82692 | 129.21 |
| 12 | 640 | 82665 | 129.16 |
| 13 | 640 | 82617 | 129.09 |
| 14 | 640 | 82597 | 129.06 |
| 15 | **152** | **20006** | 131.62 |
| 16 | **540** | **69779** | 129.22 |
| 17 | 640 | 82650 | 129.14 |
| 18 | 640 | 82671 | 129.17 |
| 19 | 640 | 82633 | 129.11 |
| 20 | 640 | 82606 | 129.07 |

**S3（`max_sleep_ms=50 cycles=20`）**

| cycle | arr_used | 實際 LPTIM CNT | cyc_across | ratio |
|---|---|---|---|---|
| 1 | 208 | — | 27156 | 130.56 |
| 2 | 594 | — | 76675 | 129.08 |
| 3 | 970 | — | 124939 | 128.80 |
| 4 | 1337 | — | 172102 | 128.72 |
| 5 | 1600 | — | 205783 | 128.61 |
| 6 | 1600 | **341**（UART 提前喚醒，未跑滿 ARR） | 44080 | **129.27**（用實際 CNT=341 算） |
| 7 | 1600 | **437**（同上） | 56442 | **129.16**（用實際 CNT=437 算） |
| 8 | 1600 | — | 205985 | 128.74 |
| 9 | 1600 | — | 205862 | 128.66 |
| 10 | 308 | — | 40017 | 129.93 |
| 11 | 692 | — | 89272 | 129.00 |
| 12 | 1065 | — | 137156 | 128.78 |
| 13 | 1429 | — | 183833 | 128.64 |
| 14 | 1600 | — | 205954 | 128.72 |
| 15 | 1600 | — | 205934 | 128.71 |
| 16 | 1600 | — | 206080 | 128.80 |
| 17 | 1600 | — | 205742 | 128.59 |
| 18 | — | — | 0（`skipped=1`，budget 太小） | N/A |
| 19 | 385 | — | 49906 | 129.63 |
| 20 | 766 | — | 98819 | 129.01 |

**判讀**：比值集中在 **128.6-131.6**（S1 因 ARR 較小、固定開銷佔比較高，
整體略偏高到 131-134），線性回歸（用 arr=82,cyc=11019 與
arr=1600,cyc=206080 兩個最寬點）：

```
slope ≈ (206080-11019)/(1600-82) ≈ 128.5
intercept ≈ 11019 - 128.5×82 ≈ 482
cyc_across ≈ 128.5 × arr_used + 482
```

代入檢驗：arr=640 → 128.5×640+482=82722（實測 82534-82692，吻合）；
arr=344 → 482+44204=44686（實測 44640，吻合）。**S3 第 6/7 輪用「實際
LPTIM CNT」而非「程式設定的 ARR」算出的比值（129.27、129.16）跟其他
用完整 ARR match 算出的比值落在同一個範圍**，這是兩條獨立路徑
（完整 match vs. 提前被 UART 打斷的部分計數）互相印證，不是巧合。

換算成頻率：128.5 × `f_LSI`（標稱 32 kHz）≈ **4.11-4.13 MHz**。這個
數字本身不是本文件要解的問題（不排除是 MSIS 預設 4 MHz——RM0456
§11.4.7 提過 MSIS 開機/喚醒預設跑在 4 MHz 附近——在除錯模式下沒有真的
降到 Stop2 該有的狀態，這只是一個觀察，不是結論），重點是：
**`CYCCNT` 在整個「Stop2」期間持續在跑，不是凍結的**，跟 2B step 3
設計文件的核心前提直接矛盾。

### 2.2 `DBG_STOP=0`（D2 對照實驗）：MCU 完全失去回應，兩種非侵入式手段都救不回來

依照既定的 D2 安全協定（清 `DBG_STOP` 的那次連線結束後不再開任何
openocd 連線、不 reset run，全程只用 `console.py`），實際執行序列：

1. 用單次 OpenOCD 連線把 `DBGMCU_CR` 從 `0x6` 清成 `0x4`
   （`0xE0044004`，只清 `DBG_STOP` 位元），確認無殘留 openocd 程序
   （`pgrep -af openocd` 為空）。
2. 透過 `console.py` 送出：
   ```
   get_uptime                              → high=136（正常回應）
   test_stop2_budget max_sleep_ms=5 cycles=20
   get_uptime                              → 無回應（逾時）
   ```
   完整原始輸出（`d2_full.log`）：
   ```
   002.528: stats count=154 sum=431559 sumsq=6262983
   002.994: uptime high=136 clock=4231246568
   ```
   送出 `test_stop2_budget` 之後，**沒有任何 `stop2b_*` 遙測，也沒有
   第二次 `get_uptime` 回應**，即使等待遠超過測試本身應花的時間。
3. 嘗試用 OpenOCD 重新連線（純 `init`，未用 `connect_assert_srst`、
   未 reset）：兩次獨立嘗試，結果一致：
   ```
   Error: Error connecting DP: cannot read IDR
   ```
4. 嘗試重新用 `console.py` 連線：
   ```
   INFO:root:Timeout on connect
   ERROR:root:Wait for identify_response
   serialhdl.error: Serial connection closed
   ```
   連最基本的 identify 握手都沒有回應。
5. R1（硬體 SRST 路徑）：核對實際燒錄用的 `/home/arduino/uno_q.cfg`
   （`adapter gpio srst -chip 1 38`，`linuxgpiod` 驅動），用
   `reset_config srst_only srst_nogate connect_assert_srst` +
   `init` + `reset halt`——**在連線前就先拉住 NRST**，結果仍是
   `Error connecting DP: cannot read IDR`。
6. R2：確認無殘留 openocd 程序、klipper service 為 inactive 後，
   執行 `ssh unoq "sudo shutdown -h now"`，確認 ping/SSH 都逾時
   （乾淨關機），**手動拔電源、等 10 秒、重新上電**（整板電源循環，
   不只是 MCU）。

結論：**`DBG_STOP=0` 下，一次 5ms 預算的 `test_stop2_budget` 測試就讓
MCU 完全無法透過 SWD 或 UART 存取，連拉 NRST（硬體層級的
`connect_assert_srst`）都救不回來，只有整板斷電重來才恢復**。這跟
`DBG_STOP=1` 下連續跑 60 輪（S1+S2+S3）、`overdue=0`、毫無異常的結果
是天壤之別。

### 2.3 R3：電源循環恢復後的唯讀量測

（`init` + `halt` + `mdw` + `resume` 讀取；純 `mdw` 不 halt 讀回全 0，
見第 5 節）

| 暫存器 | 位址 | 讀值 | 解讀 |
|---|---|---|---|
| `RCC_CSR` | `0x46020CF4` | `0x0C004400` | `BORRSTF`(27)=1、`PINRSTF`(26)=1，其餘 reset flag 全 0——乾淨的 POR，符合「整板斷電重來」的預期（RM0456 §11.8.50，行40294） |
| `DBGMCU_CR` | `0xE0044004` | `0x00000006` | `DBG_STOP`/`DBG_STANDBY` 都是 1——OpenOCD 這次重連的 `examine-end` 又把它設回去了，符合第 1 節的行為 |
| `FLASH_OPTR` | `0x40022040` | `0x1BEFF8AA` | 見下方分析 |

`FLASH_OPTR` 解碼（RM0456 §7.9.13，行23387-23397）：

- Bit 18 `IWDG_STDBY` = 1（Standby 模式下繼續跑）
- **Bit 17 `IWDG_STOP` = 1**（**Stop 模式下繼續跑，不是凍結**）
- Bit 16 `IWDG_SW` = 1（軟體獨立看門狗，由 `src/stm32/watchdog.c:26-28`
  在開機時用 `IWDG->KR=0xCCCC` 主動啟動，`RLR=0x0FFF`，對應 2A 文件
  量到的 410-512ms 邊界）
- Bit 31 `TZEN` = 0，與 `flash.log` 的 "TZEN = 0" 互相印證，確認這次
  讀值位址正確、不是誤讀

重要性見第 4 節「未解矛盾」。

---

## 3. 受影響的既有文件清單

原則（照你定的）：**發生在核心喚醒之後的量測（`restore_us`、
`s0`-`s4`）不受影響；依賴「Stop 期間時脈停止」這個前提的量測全部作廢
或待重驗**。已在下列各檔開頭加一行警告，連結回本文件，原內容不刪除。

| 檔案 | 受影響結論 | 不受影響 | 狀態 |
|---|---|---|---|
| `stop2_2a_watchdog_fix.md` | LSI 精確頻率估計（階段5/6.5）、IWDG 410-512ms 邊界（階段6）、整夜耐久測試（階段7）——全部建立在「MCU 真的進了 Stop2」的前提上，`DBG_STOP=0` 的 D2 結果顯示這個前提可能從一開始就不成立 | 階段1的程式碼改動本身（IWDG 餵狗時機修正）是獨立的正確性修復，不依賴這個前提 | **待重驗**（需要在 `DBG_STOP=0` 下重跑） |
| `stop2_2a_final_measurements.md` | LSI 四個估計值並列比較、整夜耐久、HSI16/LSI 整夜穩定性對照——同上，全部假設測試期間是真正的 Stop2 | 無 | **待重驗** |
| `stop2_2a_hwtest.md` | IWDG 逾時邊界掃描、「Stop2 進出是否成立」這一節的結論本身現在存疑 | 無（這份文件的核心內容就是 Stop2 邊界量測） | **待重驗** |
| `stop2_2a_review.md` | 無——四個問題（PLL relock 檢查、忙等 timeout、SYSCLK 來源回報、LPTIM1 寫入時機）都是程式碼健壯性修復，跟 `DBG_STOP` 無關，修復本身仍然成立 | 全部 | **不受影響** |
| `2b_g_wake_latency.md` G3（`total_latency`/`restore_us`、三檔判讀） | 無——`restore_us` 是喚醒後才開始量，`tWUSTOP2` 是查表值，不依賴 `CYCCNT` 在睡眠期間的行為 | 全部 | **不受影響** |
| `2b_g_wake_latency.md` G5（`STOPWUCK=1` 對照、`restore_us` 118.2→92.9µs） | 無——同樣是喚醒後的軟體可量段比較 | 全部 | **不受影響** |
| `2b_g_wake_latency.md` G6（直接讀 `RCC_CR` 判定 HSI16 在 Stop2 期間是否關掉，結論「(B) 成立，HSI16 全程沒關」） | **結論作廢**——`cr_at_wake` 是在 `DBG_STOP=1` 下讀的，「HSI16 全程沒關」在這個條件下是必然結果（`DBG_STOP=1` 本來就會讓所有振盪器保持開啟），不能證明真正 Stop2 下 HSI16 的行為 | 無 | **作廢，待 D3 重驗**（D3 本輪尚未執行） |
| `2b_step3_clock_design.md` | 整個補償設計的前提 | 見該檔獨立更新（第4節下方） | **前提待驗證**（已於原檔加註，見下） |

`2b_step5a_budget_sleep.md` 尚未建立（按你先前的指示，寫文件被延後到
D 系列查完為止），這裡先不處理；等真正要寫的時候，S1-S3 的
`overdue=0` 安全結論可以照寫，但「`CYCCNT` 在 Stop2 期間的行為」整節
要引用本文件，不能用「凍結」這個預設。

---

## 4. 未解矛盾：`IWDG_STOP=1` 卻沒有在 D2 期間把 MCU 救回來

第 2.3 節讀到 `FLASH_OPTR.IWDG_STOP=1`——照 option byte 設定，IWDG
應該在 Stop 模式下繼續倒數，`RLR=0x0FFF` 對應 410-512ms 逾時，D2 卡住
的時間遠超過這個數字，照理說 IWDG 應該早就把 MCU 重置、重開機後
`console.py` 應該能重新握手成功。但觀察到的是完全沒有回應，直到整板
斷電。三個假說：

### 假說 1：真正 Stop2 下 LSI 被關閉，IWDG 因此停擺——**本輪已用 RM 文字排除**

查 RM0456 §11.4.8 "LSI clock"（行33320-33332）：

> The LSI RC acts as a low-power clock source that can be kept running in Stop and Standby
> modes for the independent watchdog (IWDG) and RTC... The LSI RC can be switched on and
> off using LSION in RCC_BDCR.

`RCC_BDCR.LSIRDY` 位元定義（行40158-40162）：

> This bit is set when the LSI is used by IWDG or RTC, **even if LSION = 0**.

以及 §61.1 "IWDG introduction"（行168003）：

> The IWDG is clocked by an independent clock, and stays active even if the main clock fails...
> allowing the IWDG to remain functional even in low power modes.

Table 622（行168031）明確列出 STM32U5 的 IWDG 支援 "Capability to work
in system Stop"。**RM0456 明文規定：只要 IWDG 被啟用，LSI 就會被強制
保持運作，不受 `LSION` 影響，且 IWDG 本身被設計成在 Stop 模式下也能
正常倒數並觸發重置**。這跟我們的韌體設定完全吻合（`IWDG_SW=1` 軟體
啟用、`watchdog_init()` 在開機就呼叫了 `IWDG->KR=0xCCCC`）。

**假說 1 排除**：按 RM0456 的文字，不存在「真正 Stop2 下 LSI 關閉導致
IWDG 停擺」這條路徑——規格明講 IWDG 啟用本身就會鎖住 LSI 開著。

### 假說 2：實際進入了比 Stop2 更深的模式（Standby/Shutdown）

`stop2_once_budget()` 寫入 `PWR_CR1` 的 `LPMS` 欄位理論上應該是
Stop2 對應的編碼（`s_budget_status.pwr_cr1_before=2`，與 S1-S3 在
`DBG_STOP=1` 下量到的值一致，若 `DBG_STOP=0` 時這個值被錯誤地寫成
Standby/Shutdown 對應的編碼，就會導致完全不同的喚醒/重置語意——
例如 Shutdown 模式連 IWDG 都會失去供電，RM0456 §11.4.4 的模式比較表
需要查）。

**可區分的驗證方法**：在 `DBG_STOP=0` 條件下，於**寫入 `PWR_CR1` 之後、
`wfi` 之前**（即 `stop2_once_budget()` 現有的 `pwr_cr1_before` 擷取點）
把這個值透過 UART 送出（在真的執行 `wfi` 之前，確保至少這筆遙測送得
出去），比對是否真的是 Stop2 的編碼（`LPMS=010`），排除寫錯暫存器
欄位的可能。這不需要新的硬體操作，只需要調整既有程式碼的 sendf
時機（搬到 wfi 之前），風險上可控，但仍然是「改程式碼」，不在本輪
純文件範圍內，列為下一步選項。

### 假說 3：GPIO38 的 NRST 驅動實際上無效

R1 已經用 `connect_assert_srst` 試過，結果跟不用 SRST 時一樣是
`cannot read IDR`——但這本身**無法區分**「NRST 線路真的沒接到 MCU
或驅動不動」跟「NRST 有效拉低、MCU 真的被重置了，但重置後的開機流程
又在更早的地方卡住，根本來不及把 SWD 的除錯電源域/UART 帶起來」。

**可區分的驗證方法**：示波器或邏輯分析儀直接量測 GPIO38（QRB2210 側）
與 MCU H3 腳（NRST，電氣上應該是同一個網路）在 `connect_assert_srst`
期間的電壓波形——如果 GPIO38 被拉低但 H3 沒有對應反應（或者根本量不到
電壓變化），代表線路或驅動層有問題（硬體接線、電平不匹配、或
`linuxgpiod` 的 GPIO 方向設定錯誤）；如果兩者一致同步拉低又放開，
代表 NRST 確實生效，問題在重置後的開機流程本身，需要另外追查。這個
驗證需要額外的量測設備，不是純 mdw/mww 能做到的，列為待辦。

---

## 5. 方法學限制：`linuxgpiod` bitbang 轉接器對執行中核心的非侵入式讀取不可靠

本輪 R3 一開始用純 `mdw`（不先 `halt`）讀 `RCC_CSR`/`DBGMCU_CR`/
`FLASH_OPTR`，三個位址全部讀回 `0x00000000`——包含 `FLASH_OPTR`，這個
暫存器的 ST 出廠值不可能是全 0，且跟同一時間 `flash.log` 記錄的
"RDP level 0 (0xAA)" 矛盾（若 `FLASH_OPTR` 真的是 0，`RDP[7:0]` 應該
讀到 `0x00` 而不是 `0xAA`）。加上 `halt` 之後同一批位址重讀，全部變成
合理的非零值（`FLASH_ACR=0x00000104`、`FLASH_OPTR=0x1BEFF8AA`、
`RCC_CSR=0x0C004400`），且與 `run_stop2.sh` 既有的 `init; halt; mdw;
resume` 手法完全一致。

**結論：這個板子用的 `linuxgpiod` SWD bitbang 轉接器,在目標核心正在
執行（非 halt）時做記憶體存取不可靠,會靜默回傳全 0 而不是報錯**。
之後任何需要讀暫存器的場合,一律要用 `init; halt; mdw ...; resume`
的順序,不能只用裸 `mdw`。這跟 Stop2 本身無關,是這個特定轉接器/
openocd 版本組合的限制,但影響了本輪至少一次量測的初始判讀,值得記錄
下來避免重蹈覆轍。
