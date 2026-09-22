# Milestone 2B G — Stop2 喚醒延遲遙測

延續 [[2b_step0_lpuart_lse]]。目的：直接量測 Stop2 喚醒（`wfi` 返回到
`stm32u5_sysclk_restore()` 完成）實際花多久，取代 C1/C5 裡完全空白的
`wake_latency` 數字。會改韌體，已依指示在燒錄前停下讓使用者過目 diff，
批准後才燒錄。

---

## 1. G0 唯讀查證：喚醒時脈與 LPTIM 段可用性

### STOPWUCK：喚醒時脈是 MSIS，不是 HSI16

RM0456 §11.8.6「RCC_CFGR1」（offset `0x01C`＝`0x46020C1C`，行 34870-34873）
`Bit 4 STOPWUCK`（行 34946，行 34957-34958）：`0=MSIS`、`1=HSI16`。

```
$ sudo openocd ... -c "reset run" -c "sleep 2000" -c "halt" -c "mdw 0x46020C1C" ...
0x46020c1c: 0000000f
```

`0x0F` → `SW[1:0]=11`(PLL1)、`SWS[1:0]=11`(PLL1)、**`STOPWUCK=0`（MSIS）**。

`RCC_ICSCR1`（offset `0x008`）讀值 `0x4407B5EC`：`MSISRANGE=0100`(~4MHz)，
但 `MSIRGSEL=0`——代表這個 range 從沒被軟體接管過，一路維持 RM0456 講的
「NRST/POR 後永遠是 4MHz」這個預設（行 40384-40386）。`RCC_CFGR1` 的
`STOPWUCK` 附近的 note（行 34627-34629）也確認：只要 MSIS range ≤24MHz，
Stop 喚醒時這個 range 會被保留，不會被硬體改掉。**結論：喚醒時脈是
MSIS，標稱 4MHz（未校準）。**

### LPTIM one-shot：ARR match 後 CNT 停止，不續數

`lptim1_wakeup_init()`／`stop2_once()` 用的是 `SNGSTRT`（單次計數，
`src/stm32/stm32u5_lowpower.c:297` 原本的行號，見 commit `e9985ad2`），
不是 `CNTSTRT`。RM0456「One-shot mode」（行 162528-162529）：「the timer
is started from a trigger event and **stops when an LPTIM update event
is generated**」——match 後計數器停止，不繼續跑。依原訂判準：**放棄
LPTIM 段，只做 CYCCNT 段**，`sendf` 格式也對應拿掉 `lpt_min`/`lpt_max`。

### G3-pre：PLL 鎖定前先切 HSI16——觸發分段量測

`stm32u5_sysclk_bringup()`（`src/stm32/stm32u5.c:170-207`，commit
`e9985ad2` 行號）：

```
Step 0: HSI16ON → 等 HSI16RDY → SW=01(HSI16) → 等 SWS=01(HSI16)
Step 1: VOS Range1 + EPOD booster
Step 2: Flash latency 4WS
Step 3: PLL1 init → 鎖定
Step 4: SW=11(PLL1R) → 等 SWS=11(PLL1R)
```

確認 **PLL 鎖定前確實先切了 HSI16**（Step 0），觸發任務原訂的停止條件
——原本規劃的單一 `restore_cyc = c1 - c0` 會橫跨 MSIS(4MHz)→
HSI16(16MHz)→PLL1R(160MHz) 三種計數速率，用單一頻率換算會嚴重失真，
因此改採 G1b 的分段方案。

---

## 2. G1b 韌體改動

Commit：`2e4838a5`（`src/stm32/stm32u5.c` +13 行、
`src/stm32/stm32u5_lowpower.c` +73/-5 行）。

`stm32u5.c`：新增 `volatile uint32_t stm32u5_bringup_mark[4]`，在
Step 0/1/2/3 各自完成處寫入 `DWT->CYCCNT`。

`stm32u5_lowpower.c`：
- `struct stop2_status` 的 `restore_cyc` 換成 `s0,s1,s2,s3,s4` 五個欄位
- `stop2_once()` 算出五段差值：
  ```
  s0 = mark[0] - c0        （wfi 醒來 → Step0 完成，MSIS→HSI16 切換點）
  s1 = mark[1] - mark[0]   （Step1：VOS+EPOD，HSI16 上）
  s2 = mark[2] - mark[1]   （Step2：Flash 4WS，HSI16 上）
  s3 = mark[3] - mark[2]   （Step3：PLL1 鎖定，HSI16 上）
  s4 = c1 - mark[3]        （Step4：切 PLL1R + 等待）
  ```
- `command_test_stop2()` 逐輪累積每段的 `max`/`sum`（`entry_fail` 輪次
  不計入），結束時拆成兩則 `sendf`：`stop2_lat_max`／`stop2_lat_sum`

`irq_disable()`/`irq_enable()`（`src/generic/armcm_irq.c`）分別是
`cpsid i`/`cpsie i`，`stop2_once()` 從函式最開頭到 c1 讀取、segment
計算**之後**才 `irq_enable()`，全程 PRIMASK=1。Cortex-M 的 `wfi` 對
pending 中斷敏感但 PRIMASK=1 時不會真的跳進 ISR，**s0-s4 量測全程不會
被中斷灌大**，`max` 跟 `avg` 一樣可信。

---

## 3. G2b Build

```
$ make clean && make
...
Version: v0.13.0-474-ge9985ad22-dirty-20260922_030212-hunter
$ size out/klipper.elf
   text    data     bss     dec     hex
  36319      52    1172   37543    92a7
```

只有既有的 `__CORTEX_M` 重定義警告（跟本次改動無關）。`text` 比 G2 版
（燒錄前一輪的 `restore_cyc` 單欄位版）+132 bytes，`bss` +32 bytes（新增
`stm32u5_bringup_mark[4]` 16 bytes + `struct stop2_status` 多 4 個
`uint32_t` 16 bytes，精確吻合）。

---

## 4. G3：燒錄與量測

```
$ sudo openocd ... program out/klipper.bin verify reset exit 0x08000000
** Verified OK **

$ sudo systemctl start klipper && sleep 20 && curl -s .../printer/info
{"result":{"state":"ready", "software_version":"v0.13.0-474-ge9985ad22-dirty", ...}}
{'bytes_retransmit': 0, 'bytes_invalid': 0, ...}
```

**燒錄版本字串**：`v0.13.0-474-ge9985ad22-dirty-20260922_030212-hunter`
**對應 commit hash**：`2e4838a5`

### 原始遙測輸出

```
=== test_stop2 period_ms=100 cycles=50 ===
006.116: uptime high=2 clock=212856738
023.607: stop2_lat_max n=50 s0=135 s1=817 s2=36 s3=483 s4=59
023.607: stop2_lat_sum n=50 s0=6750 s1=40850 s2=1800 s3=21991 s4=2950
039.128: uptime high=3 clock=416048261

=== test_stop2 period_ms=400 cycles=20 ===
006.416: uptime high=4 clock=2768998715
021.356: stop2_lat_max n=20 s0=135 s1=817 s2=36 s3=466 s4=59
021.356: stop2_lat_sum n=20 s0=2700 s1=16340 s2=720 s3=8793 s4=1180
029.428: uptime high=5 clock=887243472
```

兩組 `n` 都等於各自要求的 `cycles` 數（50、20），代表**沒有任何一輪
`entry_fail`**，全部乾淨進出 Stop2。

**重置檢查**：`get_uptime` 的 `high` 欄位在兩組測試前後都單調遞增
（第一組 2→3，第二組 4→5），`clock` 也在合理範圍內變化，**沒有觀察到
`mcu_ticks` 倒退，兩組測試期間都沒有 MCU 重置**。

### 換算（`S0_us = s0/4e6`，`S1..S4_us = sN/16e6`，皆標稱頻率未校準）

| 組 | n | 段 | max (cycles) | avg (cycles) | max (µs) | avg (µs) |
|---|---|---|---|---|---|---|
| `p100,c50` | 50 | s0 (MSIS) | 135 | 135.00 | 33.750 | 33.750 |
| | | s1 (HSI16) | 817 | 817.00 | 51.062 | 51.062 |
| | | s2 (HSI16) | 36 | 36.00 | 2.250 | 2.250 |
| | | s3 (HSI16) | 483 | 439.82 | 30.188 | 27.489 |
| | | s4 (HSI16) | 59 | 59.00 | 3.688 | 3.688 |
| | | **合計** | — | — | **120.938** | **118.239** |
| `p400,c20` | 20 | s0 (MSIS) | 135 | 135.00 | 33.750 | 33.750 |
| | | s1 (HSI16) | 817 | 817.00 | 51.062 | 51.062 |
| | | s2 (HSI16) | 36 | 36.00 | 2.250 | 2.250 |
| | | s3 (HSI16) | 466 | 439.65 | 29.125 | 27.478 |
| | | s4 (HSI16) | 59 | 59.00 | 3.688 | 3.688 |
| | | **合計** | — | — | **119.875** | **118.228** |

★ **`s0`、`s1`、`s2`、`s4` 這四段在兩組、共 70 次量測裡 `max` 跟
`avg` 完全相等**（例如 `s1` 兩組都是恰好 817 cycles，一次不差）——代表
這四段是固定週期迴圈組成的**完全確定性**操作，沒有觀察到任何抖動。
這也意味著這四段的 `min` 雖然本輪沒有單獨記錄（G1b 只設計了
`max`/`sum`），但既然 `max=avg`，數學上 `min` 必然也等於同一個值，
三者相等，不需要另外量。**只有 `s3`（PLL1 鎖定）有真實抖動**：
`max` 比 `avg` 高約 6-10%（`p100,c50` 高 43.2 cycles/約 2.7µs；
`p400,c20` 高 26.4 cycles/約 1.65µs），符合類比 PLL 鎖定時間本身就會
有小幅抖動的物理預期；`s3` 的真實 `min` 本輪同樣沒有單獨記錄，只知道
它比 `avg`（439.65-439.82 cycles）更低，確切值本輪拿不到。

### 兩組比較：恢復時間是否與睡眠長度相關

**幾乎沒有差異**：`p100,c50`（total sleep 5s，50 次短睡眠）跟
`p400,c20`（total sleep 8s，20 次長睡眠）算出來的 `restore_us`
（`avg`）只差 **0.011µs**（118.239 vs 118.228），`s0/s1/s2/s4` 四段
`max` 完全相同，只有 `s3` 的 `max` 有微小差異（483 vs 466 cycles，
約 1.06µs），落在同一種 PLL 鎖定抖動的量級內，不是系統性差異。

**結論：`stm32u5_sysclk_restore()` 的耗時跟 Stop2 睡多久（`period_ms`）
沒有關聯**，符合物理預期——這段程式碼做的是固定的暫存器操作序列 +
PLL 鎖定，跟睡眠時長無關，只跟「醒來那一刻開始，要重建 SYSCLK 需要
走過哪些步驟」有關。

---

## 5. `total_latency` 與判讀

```
total_latency = tWUSTOP2（未量，DS13086 未取得，見 [[2b_step0_lpuart_lse]] C1）
              + restore_us（實測，本輪）
```

`tWUSTOP2` 這一段（Stop2 exit 到執行第一條指令的硬體喚醒延遲）
**沒有真實數字**——[[2b_step0_lpuart_lse]] 的 C1 已經確認 DS13086
在這個環境裡完全找不到，`restore_us` 是唯一有真實量測值的那一段。
`total_latency` 因此**無法算出完整數字**，只能報告已知的那一部分：

```
restore_us ≈ 118-121 µs（兩組一致，max 上界 120.938 / 119.875 µs）
```

### 三檔判讀（`restore_max`）

| 檔位 | 範圍 | 本輪結果 |
|---|---|---|
| 優 | `< 150 µs` | ✅ **兩組都落在這裡**（120.938 µs、119.875 µs） |
| 中 | `150-320 µs` | 未觸及 |
| 差 | `> 320 µs` | 未觸及 |

`restore_us` 這個已知分量本身遠低於 150µs 這個最寬鬆的判準，也遠低於
C5 提過的 `wake_latency < 320µs` 整體判準——但因為 `tWUSTOP2` 未知，
**還不能宣稱整個 `total_latency` 過了 320µs 這一關**，只能說「已知的
`restore_us` 分量幫這個判準留出了約 200µs 的餘裕（320 − 120 ≈ 200µs）
給未知的 `tWUSTOP2`」，這個餘裕夠不夠，要等真的拿到 datasheet 數字
才能確認。

### `STOPWUCK` 條件說明

★ 本輪 `STOPWUCK=0`（MSIS 4MHz 喚醒）是**這次量測時的既有條件**，
本輪**沒有改動**這個位元。`STOPWUCK=1`（改用 HSI16 當喚醒時脈，
理論上能跳過 `s0` 那段 MSIS 4MHz 的慢速區間，直接從 16MHz 起算，
可能可以縮短 `restore_us`）是**列為待測的優化方向**，本輪不改、不測，
留給下一輪決定要不要嘗試。

---

## 6. 我無法判定的事

1. **`total_latency` 完整數字算不出來**：`tWUSTOP2` 這一段完全沒有
   真實數字（DS13086 拿不到），只能報告 `restore_us` 這個已知分量。
2. **`s3`（PLL1 鎖定）的真實 `min`**：本輪只記錄了 `max`/`sum`，`min`
   沒有另外追蹤，只知道它存在（因為 `max>avg`），確切數值需要改韌體
   多加一個 `seg_min[5]` 才能量到。
3. **`s0`/`s1`/`s2`/`s4` 完全零抖動這件事，本身有沒有意外因素**：70
   次量測（兩組合計）裡這四段連續一次不差，理論上合理（固定次數的
   暫存器輪詢迴圈，硬體 ready 時機本身如果穩定，迴圈次數就會穩定），
   但本輪沒有進一步驗證是不是巧合（例如兩組測試前後溫度/電壓條件
   幾乎沒變，如果换一個時間點/溫度再測一次，這種零抖動會不會被打破，
   沒有測過）。
4. **`STOPWUCK=1` 的實際效果**：只是列出來當作待測方向，本輪完全沒
   實測，不知道改了之後 `s0` 那段會縮短多少、或會不會引入新的問題
   （例如 HSI16 精度不如校準過的振盪器，對開機初期的時序穩定性有沒有
   影響，未知）。
5. **這組數字的代表性**：只在同一塊板子、同一次連續測試（兩組合計
   70 次）裡量到，沒有跨 session、跨溫度、跨板卡的重複驗證，
   不確定是否能代表這顆晶片/這個設計的通用行為。
