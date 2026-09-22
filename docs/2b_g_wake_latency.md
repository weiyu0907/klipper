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

## 6. G5：`STOPWUCK=1` 對照測試（不改韌體，純暫存器覆蓋）

延續第 5 節「待測優化方向」。方法：不改任何程式碼，直接用 openocd 對
`RCC_CFGR1`（`0x46020C1C`）做 RMW，只把 `STOPWUCK`（bit4）設成 1，跑
跟 G3 完全相同的兩組測試，量測完再改回來，全程沒有 make/燒錄。

### G5-0：唯讀查證

`stm32u5_sysclk_bringup()` 裡所有 `U5_RCC_CFGR1` 寫入（`src/stm32/
stm32u5.c`，commit `2e4838a5` 行號）：

```
行191: U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 1u;   /* SW=HSI16，只改 bits[1:0] */
行213: U5_RCC_CFGR1 = (U5_RCC_CFGR1 & ~3u) | 3u;   /* SW=PLL1R，只改 bits[1:0] */
```

兩處都是 `& ~3u | X` 的 RMW，只動 `SW[1:0]`（bit0-1），**`STOPWUCK`
（bit4）跟其他位元完全不會被這兩處寫入影響**——沒有整字覆寫，確認
可以安全地在外部改 `STOPWUCK`、不擔心 restore 序列把它蓋掉。

### G5-1/G5-2：設定與量測

```
$ sudo openocd ... -c "halt" -c "mdw 0x46020C1C" \
    -c "mww 0x46020C1C 0x0000001F" -c "mdw 0x46020C1C" -c "resume" ...
0x46020c1c: 0000000f
0x46020c1c: 0000001f
```

`0x0F→0x1F`，只多了 bit4，`SW`/`SWS`（bit0-3，仍是 `1111`）不變。

```
=== STOPWUCK=1，test_stop2 period_ms=100 cycles=50 ===
005.868: uptime high=64 clock=1806487235
023.358: stop2_lat_max n=50 s0=135 s1=817 s2=36 s3=466 s4=59
023.358: stop2_lat_sum n=50 s0=6750 s1=40850 s2=1800 s3=21719 s4=2950
038.879: uptime high=65 clock=2070492967

=== STOPWUCK=1，test_stop2 period_ms=400 cycles=20 ===
006.437: uptime high=67 clock=3486930165
021.376: stop2_lat_max n=20 s0=135 s1=833 s2=36 s3=483 s4=59
021.376: stop2_lat_sum n=20 s0=2700 s1=16356 s2=720 s3=8776 s4=1180
029.449: uptime high=68 clock=1703411392
```

`n` 都等於各自要求的 `cycles`（50、20），無 `entry_fail`。`get_uptime`
的 `high` 兩組都單調遞增（64→65、67→68），**沒有重置**。

測完立刻再讀一次確認 `STOPWUCK` 沒被 restore 序列清掉：

```
$ sudo openocd ... -c "mdw 0x46020C1C" ...
0x46020c1c: 0000001f
```

仍是 `0x1F`，符合 G5-0 查證的結論。

### 換算（`STOPWUCK=1` 時 `s0` 也跑在 HSI16，`s0_us = s0/16e6`）

| 組 | | s0 | s1 | s2 | s3 | s4 | 合計 |
|---|---|---|---|---|---|---|---|
| `p100,c50` (n=50) | avg | 8.438 | 51.062 | 2.250 | 27.149 | 3.688 | **92.586** |
| | max | 8.438 | 51.062 | 2.250 | 29.125 | 3.688 | **94.562** |
| `p400,c20` (n=20) | avg | 8.438 | 51.112 | 2.250 | 27.425 | 3.688 | **92.912** |
| | max | 8.438 | 52.062 | 2.250 | 30.188 | 3.688 | **96.625** |

### G5-4：逐段對照表 `STOPWUCK=0` vs `STOPWUCK=1`

| 段 | STOPWUCK=0（G3，µs） | STOPWUCK=1（G5，µs） | 差 | 判讀 |
|---|---|---|---|---|
| s0 | 33.750（兩組一致） | 8.438（兩組一致） | **−25.312µs（−75%）** | 見下方「軟體可量段」說明 |
| s1 | 51.062（兩組一致） | 51.062 / 51.112-52.062 | p100 組 0%；p400 組 max +1.96% | 低於 2% 門檻，未達異常標準，但接近，如實記錄 |
| s2 | 2.250（兩組一致） | 2.250（兩組一致） | 0% | 無變化 |
| s3 | avg 27.478-27.489 / max 29.125-30.188 | avg 27.149-27.425 / max 29.125-30.188 | avg 約 −0.2% ~ −1.2% | 落在 PLL 鎖定本身的抖動範圍內，非系統性差異 |
| s4 | 3.688（兩組一致） | 3.688（兩組一致） | 0% | 無變化 |
| **合計（avg）** | 118.228-118.239 | 92.586-92.912 | **約 −25.4µs** | 幾乎全部來自 s0 |

`s0` 的 cycle 數（135）在 `STOPWUCK=0`/`STOPWUCK=1` 兩種條件下**完全
相同**，只有換算用的除頻基準從 4MHz 改成 16MHz——代表 Step 0 迴圈本身
執行的指令數/次數沒有變化，純粹是「喚醒後 CPU 從哪個時脈開始跑」
變快了 4 倍，讓同樣的 cycle 數對應到更短的真實時間。`s1`/`s2`/`s4`
完全不受影響（符合預期：這幾段的工作內容跟喚醒時脈選擇無關）。`s1`
在 `p400,c20` 組出現一次 `max` +1.96% 的小幅上升，剛好卡在 2% 門檻
之下，本輪判定**不算顯著變化**，但沒有到能完全排除是巧合的程度，
如實記錄、不強行解釋成雜訊或系統性效應。`s3` 的變化全部落在既有的
PLL 鎖定抖動範圍內，沒有系統性差異。

### ★★ `s0` 下降的意義：這只是「軟體可量段」的下降，不是淨收益

**`s0` 從 33.750µs 降到 8.438µs，這個下降完全發生在軟體可以用
`DWT->CYCCNT` 量到的範圍內**——也就是「`wfi` 返回、`isb` 之後」到
「Step 0 確認 `SWS=HSI16`」這一段。但 `STOPWUCK=1` 真正改變的是**硬體
在 Stop2 退出當下自動選哪顆振盪器來恢復 CPU 時脈**，這件事本身可能
（本輪無法證實或證偽）發生在 `c0`（我們軟體讀 `DWT->CYCCNT` 的最早
時間點）**之前**——也就是說，如果 `STOPWUCK=1` 底下硬體自動選的
HSI16 本身需要起振時間，這段起振時間很可能落在
[[2b_step0_lpuart_lse]] C1 提過、完全沒有數字的 `tWUSTOP2` 這個硬體
喚醒延遲區間裡，而不是我們能量到的 `s0`。

**換句話說：`s0` 少掉的 25.312µs，有可能只是把原本在「軟體可見的
Step 0 等待」裡的耗時，轉移到「軟體完全看不到的 `tWUSTOP2`」裡去了
——如果真是這樣，`total_latency`（`tWUSTOP2 + restore_us`）的
淨變化可能遠小於 `s0` 這 25.312µs 的降幅，甚至可能是零（如果
`tWUSTOP2` 剛好多花了差不多的時間去起振 HSI16）。本測試只能量到
`restore_us`（軟體可見段）的變化，***沒有辦法量到 `tWUSTOP2` 有沒有
跟著變化，因此本測試無法給出 `STOPWUCK=1` 的淨收益，只能報告「軟體
可見段減少了多少」這個片面數字***。

### `s0` 恆為 135 cycles：兩種都能成立的解釋，本數據無法區分

`s0` 的 cycle 數在**兩種 STOPWUCK 條件、兩組測試、合計 140 次**量測裡
全部固定在 135 cycles，一次不差。★ 修正前一版文件的推論——這個現象
**至少有兩種互相獨立、都能單獨解釋觀測結果的假說**，本輪的數據沒辦法
區分是哪一種：

- **(A) `s0` 被指令執行時間限住**：Step 0 開頭那幾行指令（`RCC_CR |=
  HSI16ON`、迴圈設置、第一次 `HSIRDY` 檢查……）本身在 MSIS 4MHz
  （搭配當時的 Flash wait state 設定）下執行，光是跑到「第一次真的去
  檢查 `HSIRDY`」這一步，累積的指令執行時間可能就已經比 HSI16 實際
  起振所需的時間更長——也就是說，HSI16 這次確實是從關閉狀態重新啟動，
  但因為輪詢迴圈前面的指令本身跑得夠久，等真正檢查旗標時它早就
  ready 了，135 cycles 反映的是「指令執行開銷」，不是「硬體等待」。
  這個假說下，**HSI16 在 Stop2 睡眠期間確實有被關掉，省電**。
- **(B) HSI16 在 Stop2 期間根本沒被關閉**：韌體的 Stop2 進入序列
  （`stop2_once()`）只處理 `PWR_CR1`/`SCB_SCR`，沒有動到 `RCC_CR` 的
  `HSI16ON` 位元——如果 HSI16 因此在整個 Stop2 睡眠期間持續開著，
  Step 0 的 `HSIRDY` 檢查自然永遠一次就過。這個假說下，**Stop2 睡眠
  期間的實際耗電比「HSI16 真的關掉」的理想情況更高**，這牽涉到
  Milestone 2B 真正在意的能效主題，不只是延遲數字好不好看。

兩種假說都能完美解釋「135 cycles 恆定不變」這個現象，光看 CYCCNT
數字**無法區分**是哪一種。**區分方式：直接量測 Stop2 睡眠期間的
實際電流**——若是 (B)，HSI16 持續運轉會讓睡眠電流比 (A) 的情況高出
百 µA 級（HSI16 這類內部 RC 振盪器的典型電流量級），用電流表/示波器
接在供電路徑上量測即可分辨。**這個測量本輪沒有做，列為待辦**，而且
直接跟 2B 真正關心的能效最佳化主題掛鉤——如果是 (B)，那麼在深入
研究 `STOPWUCK`/`s0` 這種微秒級的延遲數字之前，應該先把 HSI16 真的
關掉，那才是真正省電的地方，延遲數字反而是次要的。

### G5 結論

`STOPWUCK=1` 讓軟體可量段（`restore_us`，即 `s0+s1+s2+s3+s4`）的
`avg` 從 **118.2 µs 降到約 92.9 µs**（`p100,c50`: 118.239→92.586；
`p400,c20`: 118.228→92.912），`s1`/`s2`/`s3`/`s4` 四段本身不變（`s1`
在 p400 組一次 +1.96% 的小波動除外，未達異常門檻），降幅幾乎全部
來自 `s0`。**建議 2B 韌體正式採用 `STOPWUCK=1`**——即使無法排除
「淨收益可能被 `tWUSTOP2` 抵銷」這個上面反覆強調的限制，`STOPWUCK=1`
至少不會讓已知的軟體可量段變差，而且是一行程式碼的改動
（`RCC_CFGR1` 加一個 `|=`，不影響任何既有的 RMW 寫法），改動成本低、
沒有觀察到副作用（`s1..s4` 不變、無 `entry_fail`、無重置），沒有
理由不採用。

`restore_us` 五段裡，**`s1`（VOS Range1 + EPOD booster）是目前最大的
一段**（51.06 µs，占 `STOPWUCK=1` 版 `restore_us` 的約 43%，比
`STOPWUCK=0` 版的 43% 占比也差不多，因為 `s1` 本身沒被 `STOPWUCK`
影響），**列為下一個優化目標**。第一步建議：查 Stop2 進入前後
`PWR_VOSR` 的 VOS Range 是否本來就有保留（如果 Stop2 睡眠期間
Range 1 設定沒被清掉，Step 1 的 `VOSRDY`/`BOOSTRDY` 兩個輪詢理論上
應該可以像 `STOPWUCK` 對 `s0` 的效果一樣，一次就過，不需要真的等硬體
穩壓——但這純屬本輪根據 `s0` 案例類推的假設，`s1` 目前完全沒有像
`STOPWUCK` 這樣可以直接測試的開關可以驗證，需要另外設計量測方式）。

---

## 7. 外部規格缺口（DS13086）

### 狀態

**DS13086 Rev 10（STM32U585 datasheet）未取得。** 嘗試從 st.com 的
datasheet 資源端點自動下載，四種 `curl` 方式全部失敗（預設、
`--http1.1`、HTTP/2 加瀏覽器 headers、HTTP/1.1 加瀏覽器 headers），
分別以逾時、`HTTP/2 stream ... INTERNAL_ERROR`、再逾時收場——一般
網路連線本身沒問題（google.com、st.com 首頁都正常回應），問題出在
這個特定資源端點對非瀏覽器自動化請求的處理，很可能是 Akamai 反爬蟲
機制擋下。詳見 [[2b_step0_lpuart_lse]] C1 的原始嘗試記錄。

**RM0456 Rev 7 是本系列文件目前唯一已查證的規格來源**，`refs/`
目錄（PC 本機 `~/klipper/refs/`）只有 `rm0456.pdf`/`rm0456.txt` 跟
CMSIS header，沒有任何 datasheet 檔案。

### 缺少的參數與受影響的結論

| 參數 | DS13086 位置 | 受影響的結論 |
|---|---|---|
| `tWUSTOP2` | Table 74/75 | `total_latency = tWUSTOP2 + restore_us` 無法給出完整數字（第 5 節），只能報告 `restore_us` 這個已知分量 |
| `tWULPUART` | Table 77 | [[2b_step0_lpuart_lse]] A2 算出的 `BaudMax≈88.36 kbaud` 仍是 RM0456 自己的範例值（`tWULPUART=3µs`、HSI16 inaccuracy 1% 都是範例數字），沒有用 STM32U585 的實際規格值重算過 |
| `tSU(HSI16)` | Table 82 | 沒有規格值可以驗證本文件第 6 節「`s0` 恆為 135 cycles」的 (A)/(B) 兩種假說——如果 `tSU(HSI16)` 的規格值本身就小於 Step 0 前段指令在 MSIS 4MHz 下的執行時間，會直接支持假說 (A)；反之則傾向假說 (B)，但目前完全沒有這個數字可以比對 |
| `tSU(LSE)` | Table 81 | [[2b_step0_lpuart_lse]] part B 的 LSE 起振只有「≤0.5 秒（含 SWD 連線開銷）」這個實測上界，沒有規格書的典型值/最大值可以對照，不知道這次量到的「很快」是正常範圍還是board特例 |

### 補齊方式

這四張表都需要真正的 datasheet 內容，自動化下載目前走不通，補齊方式：

1. 用一般瀏覽器（有 cookie/JS 執行能力，能通過反爬蟲機制）手動下載
   DS13086 PDF。
2. 放到 `~/refs/`（PC 本機，**不進 repo**，跟 `rm0456.txt`/
   `stm32u585xx.h` 同一個目錄邏輯）。
3. `pdftotext -layout DS13086_stm32u585ai.pdf DS13086.txt` 轉成文字。
4. `grep -n -E "Table 7[4-7]\.|Table 8[12]\."` 之類的關鍵字定位這四張
   表，逐字引用數值、條件、typ/max、頁碼，比照本系列文件對 RM0456 的
   查證方式（附行號/頁碼，不推估、找不到就明說）。

補齊後，第 5 節的 `total_latency` 三檔判讀、A2 的 `BaudMax`、第 6 節的
(A)/(B) 假說，都需要用真實數字重新檢視一次結論是否還成立。

---

## 8. 我無法判定的事

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
4. ~~`STOPWUCK=1` 的實際效果：只是列出來當作待測方向，本輪完全沒
   實測~~ ★ **已在第 6 節（G5）測過，但淨收益依然未知**：`s0`（軟體
   可見段）確實降了 25.312µs，但如第 6 節詳述，這個降幅有可能只是把
   耗時轉移到量不到的 `tWUSTOP2` 裡，`total_latency` 的真正淨變化
   本輪拿不到。另外，`HSI16` 精度不如校準過的振盪器、對開機初期時序
   穩定性有沒有影響，這點本輪也還是沒測——G5 只跑了 70 次（跟 G3
   對等的兩組），樣本數不足以看出這種精度問題可能造成的長尾影響。
5. **這組數字的代表性**：只在同一塊板子、同一次連續測試（兩組合計
   70 次）裡量到，沒有跨 session、跨溫度、跨板卡的重複驗證，
   不確定是否能代表這顆晶片/這個設計的通用行為。
