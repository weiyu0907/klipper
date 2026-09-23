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

★ **DS13086 Rev 10（July 2024）已取得**，`tWUSTOP2` 不再是空白，見
第 7 節完整查表記錄。本節用查到的數字重算。

```
total_latency = tWUSTOP2（DS13086 Table 74，LDO，該列 Max）
              + restore_us（實測，本輪 max）
```

適用列（Table 74，p.210-211）：`twu(Stop 2)` / `All SRAMs retained` /
`Wake-up in FLASH, SRAM4FWU=0 in PWR_CR2, ICACHE OFF`——選這一列的理由：
韌體（`stm32u5.c`/`stm32u5_lowpower.c`）沒有任何地方寫 `PWR_CR2` 的
`SRAM4FWU`/`FLASHFWU`、也沒有動 `ICACHE_CR`，三者都維持 POR 預設
（`SRAM4FWU=0`、`FLASHFWU=0`、ICACHE OFF），且 klipper 一路從 Flash
執行（不是 SRAM2 執行），跟這一列條件精確吻合。`REGSEL`（LDO/SMPS
選擇）同樣沒被韌體動過，維持 POR 預設 LDO，所以用 Table 74（LDO）不用
Table 75（SMPS）。

這一列在 `STOPWUCK=1`（HSI16 喚醒）有精確數字，但 `STOPWUCK=0`
（MSIS 4MHz 喚醒）**沒有剛好對應的列**——Table 74 這一列只列了
`MSI 24 MHz`（Max 25.0µs）跟 `MSI 1 MHz`（Max 60.0µs）兩個 MSI 頻率，
沒有 4MHz。用這兩個當上下界（假設 `tWUSTOP2` 隨喚醒時脈頻率單調變化
——頻率愈低愈慢，這個假設 DS13086 沒有明講，但物理上合理，且跟
`MSI 24MHz`（25.0µs）< `MSI 1MHz`（60.0µs）這個已知模式一致，沒有
反例）：

| STOPWUCK | tWUSTOP2 (Max, µs) | restore_us (實測 max, µs) | total_latency max (µs) |
|---|---|---|---|
| `=1`（HSI16，Table74 精確列） | **25.0** | 96.625（`p400,c20`；`p100,c50` 為 94.562） | **121.625** |
| `=0`（MSIS 4MHz，無精確列，用 MSI24MHz/MSI1MHz 當界） | **25.0 ~ 60.0**（界，非精確值） | 120.938（`p100,c50`；`p400,c20` 為 119.875） | **145.938 ~ 180.938** |

### 三檔判讀（`total_latency` max，非僅 `restore_us`）

| 檔位 | 範圍 | `STOPWUCK=1` | `STOPWUCK=0` |
|---|---|---|---|
| 優 | `< 150 µs` | ✅ **121.625 µs，確定落在這裡**（`tWUSTOP2` 是精確列） | ⚠️ 下界 145.938 µs 落在這裡，但上界 180.938 µs 不是——**卡在優/中邊界，因為 `tWUSTOP2(MSIS 4MHz)` 沒有精確值，無法確定** |
| 中 | `150-320 µs` | 未觸及 | ⚠️ 若真實值接近上界，落在這裡 |
| 差 | `> 320 µs` | 未觸及 | 未觸及（即使用最悲觀的上界 180.938µs 也遠低於 320µs） |

**對 C5 的 `wake_latency < 320µs` 判準**：★ 兩種 `STOPWUCK` 設定、
即使用最悲觀的界（`STOPWUCK=0` 上界 180.938µs），`total_latency` 都
遠低於 320µs（只用掉判準的 57%），**這一關兩種設定都穩穩通過**，
不需要 `tWUSTOP2(MSIS 4MHz)` 的精確值就能下這個結論。更細的優/中
分級才需要精確值（見上表）。

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

### ★★ `s0` 下降的意義：軟體可量段的下降，淨收益見下方 DS13086 更新

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
跟著變化***。

★ **DS13086 拿到後，這個問題不再是完全空白**（見第 5 節、第 7 節）：
`STOPWUCK=1` 的 `tWUSTOP2` 有精確列（Table 74，Max 25.0µs），
`STOPWUCK=0` 沒有精確列，只有 `MSI 24MHz`/`MSI 1MHz` 兩個界
（25.0~60.0µs，假設隨頻率單調）。把兩種設定的 `total_latency` max
算出來比較（第 5 節）：`STOPWUCK=1` 是 121.625µs；`STOPWUCK=0` 即使
用**對它最有利**的下界（`tWUSTOP2=25.0µs`，跟 `STOPWUCK=1` 那列同一個
數字，等於假設 MSIS 4MHz 跟 MSI 24MHz 一樣快，這是刻意偏袒
`STOPWUCK=0` 的算法），也是 145.938µs——**比 `STOPWUCK=1` 的
121.625µs 還高 24.3µs**。也就是說，即使把「`tWUSTOP2` 轉移」這個
疑慮讓到最極端（假設轉移後的 `tWUSTOP2(MSIS 4MHz)` 跟 HSI16 那列一樣
快），`s0` 省下來的 25.312µs 依然沒有被完全「吃回去」——**`STOPWUCK=1`
的淨收益就 `total_latency` 的 max 而言是正的，不是「完全未知」**。

這個結論仍有兩個限制，如實列出：
1. 依賴「`tWUSTOP2` 隨喚醒時脈頻率單調」這個物理上合理但 DS13086
   沒有明講的假設（見第 5 節）；
2. `restore_us` 的 max 是本輪 70 次量測裡的軟體實測值，`tWUSTOP2` 的
   Max 是 ST 的 characterization 規格值（3V、涵蓋溫度範圍），兩者的
   統計基礎不完全對等，只是目前能湊出來最接近的比較。

★★ 但這**不影響**下面 (A)/(B) 兩個假說的討論——那是「Stop2 睡眠期間
HSI16 有沒有真的被關掉」的耗電問題，`total_latency` 的淨收益是延遲
問題，兩者互相獨立，見下段。

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

### ★ DS13086 `tSU(HSI16)`：支持 (A)，不是證明

Table 82（`tsu(HSI16)`，DS13086 p.220）：**Typ 2.5µs、Max 3.6µs**——
這是 HSI16 從關閉狀態起振到 ready 的時間。跟 `s0`（33.750µs，
`STOPWUCK=0` 下量到，兩組、共 70 次一致）比：`tsu(HSI16)` 的 Max
（3.6µs）只有 `s0` 的**約 1/9.4**（33.750 / 3.6 ≈ 9.4 倍）。

這個數字**支持假說 (A)**：如果 HSI16 真的是從關閉狀態重新啟動
（(A) 的前提），起振本身最多只要 3.6µs，遠比 135 cycles（33.750µs，
@MSIS 4MHz）短——代表就算 (A) 成立，Step 0 迴圈前段的指令執行開銷
（`RCC_CR |= HSI16ON` 到第一次檢查 `HSIRDY` 之間的指令）本身就足以
「蓋過」整個起振時間，`HSIRDY` 檢查第一次就會過，跟本輪觀測到的
「135 cycles 恆定、無抖動」一致。

**但這不是證明**：(B)（HSI16 全程沒關）底下，`HSIRDY` 同樣會第一次
就過，跟 (A) 的觀測結果**完全無法區分**——`tsu(HSI16)` 只是排除了
「(A) 不成立是因為起振比 135 cycles 還久」這個反例，沒有辦法反過來
證明 HSI16 真的有被關掉。區分 (A)/(B) 仍然需要上一段講的電流量測，
`tsu(HSI16)` 只是讓 (A) 這個假說本身在時間量級上站得住腳，不再是
純推測。

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

## 7. 外部規格缺口（DS13086）—— 已查到

### 狀態

★ **DS13086 Rev 10（STM32U585 datasheet，2024 年 7 月）已由使用者手動
下載並放到 `~/refs/DS13086_stm32u585ai.pdf`**（自動化下載當時走不通的
原因——Akamai 反爬蟲擋下非瀏覽器 `curl` 請求——詳見
[[2b_step0_lpuart_lse]] C1 的原始嘗試記錄，那份記錄本身保持不變，仍然
正確描述「當時」的狀態）。用 `pdfinfo`/`pdftotext` 等等的 poppler-utils
需要 `sudo apt install`、但沒有終端機密碼可用，改用已裝好的 Python
`PyMuPDF`（`fitz`）套件抽取全文字（`doc.get_text()`），逐頁存成
`~/refs/DS13086.txt`（350 頁）。**PDF 與 txt 都留在 `~/refs/`，不進
repo。**

版本確認（PDF 第 1/2/350 頁頁腳）：**`DS13086 Rev 10`，`July 2024`**。

**RM0456 Rev 7 仍是本系列文件另一個已查證的規格來源**，兩者互補：
RM0456 給暫存器行為/公式，DS13086 給實際數字。

### 查到的四張表（逐字引用，含頁碼）

**Table 74. Low-power mode wake-up timings on LDO** (p.210-211)——本板
`REGSEL` 沒被韌體動過，維持 POR 預設 LDO，所以用這張不用 Table 75
（SMPS）。適用列：`twu(Stop 2)` / `All SRAMs retained` /
`Wake-up in FLASH, SRAM4FWU=0 in PWR_CR2, ICACHE OFF`（理由見第 5 節）：

| Conditions | Typ (3V,25°C) | Max (3V) |
|---|---|---|
| MSI 24 MHz | 23.0 µs | 25.0 µs(2) |
| HSI 16 MHz | 22.5 µs | 25.0 µs |
| MSI 1 MHz | 57.0 µs | 60.0 µs |

（註 2：Tested in production at 130°C；其餘 typ/max 為 characterization
值，非量產測試。**沒有 MSI 4MHz 這一列**——本板 `STOPWUCK=0` 用的正是
MSIS 4MHz，DS13086 對這個精確頻率沒有直接數字，只能用 MSI 24MHz/
MSI 1MHz 當界，細節與對 `total_latency` 的影響見第 5 節。）

**Table 77. Wake-up time using USART/LPUART** (p.214)：

> `tWUUSART`/`tWULPUART`：「Wake-up time needed to calculate the maximum
> USART/LPUART baud rate that is needed to wake up from Stop mode when
> the USART/LPUART kernel clock source is HSI16/MSI.」Typ/Max 欄位是
> `-`/`(2)`（無直接數字）。註 2：「This wake-up time is the HSI16
> (see Table 82) or the MSI (see Table 83) oscillator maximum startup
> time.」

即 `tWULPUART` 本身沒有獨立數字，**等於 Table 82 的 `tsu(HSI16)`**
（本板 LPUART kernel clock 用的是 HSI16，不是 MSI，見
[[2b_step0_lpuart_lse]] A1）。

**Table 81. LSE oscillator characteristics** (p.219)：

| Symbol | Parameter | Conditions | Min | Typ | Max | Unit |
|---|---|---|---|---|---|---|
| `tSU(LSE)`(4) | Startup time | VDD is stabilized | - | **2** | - | s |

（註 4：「`tSU(LSE)` is the startup time measured from the moment it is
enabled (by software) to a stabilized 32.768 kHz oscillation is reached.
This value is measured for a standard crystal and it can vary
significantly with the crystal manufacturer.」只有 Typ，沒有 Min/Max。）

**Table 82. HSI16 oscillator characteristics** (p.220)：

| Symbol | Parameter | Conditions | Min | Typ | Max | Unit |
|---|---|---|---|---|---|---|
| `fHSI16` | 出廠校準後頻率 | VDD=3.0V, TJ=30°C | 15.92 | 16 | 16.08 | MHz |
| `fHSI16`(1) | | TJ=-10~100°C, 1.58≤VDD≤3.6V | 15.84 | - | 16.16 | MHz |
| `tsu(HSI16)`(2) | HSI16 oscillator startup time | - | - | **2.5** | **3.6** | µs |
| `tstab(HSI16)`(2) | HSI16 oscillator stabilization time | At 1% of target frequency | - | 4 | 6 | µs |

（`fHSI16` 在 TJ=-10~100°C 這一列，15.84~16.16MHz 相對 16MHz 標稱值
剛好是 **±1.0%**——跟 RM0456 A2 公式範例引用的「HSI inaccuracy 1%」
精確吻合，代表那個「範例」數字其實就是這顆晶片延伸溫度範圍下的真實
規格，不是隨便舉的例子。）

### 受影響結論：重算

**1. `tWUSTOP2` → 第 5 節 `total_latency`**：已用 Table 74 重算，兩種
`STOPWUCK` 設定的 `total_latency` max 都遠低於 320µs 判準；`STOPWUCK=1`
可以確定落在「優（<150µs）」檔，`STOPWUCK=0` 因為沒有精確的 MSIS
4MHz 列，卡在優/中邊界，細節見第 5 節、第 6 節。

**2. `tWULPUART` → [[2b_step0_lpuart_lse]] A2 的 `BaudMax`**：用
`tsu(HSI16)` 真實值重算（公式不變：`Tbit min = tWULPUART / (11 ×
DWUmax)`，`DWUmax = 3.41% - 1% = 2.41%`，`3.41%` 是 RM 公式本身的
LPUART receiver tolerance 常數，`1%` 現在確認是真實規格見上）：

| tWULPUART 取值 | Tbit min | BaudMax |
|---|---|---|
| RM0456 範例（3µs，舊版沿用） | 11.32 µs | 88.36 kbaud |
| **DS13086 實際 Typ（2.5µs）** | 9.430 µs | **106.04 kbaud** |
| **DS13086 實際 Max（3.6µs）** | 13.580 µs | **73.64 kbaud** |

真實 Max 算出的 73.64 kbaud **比 RM 範例的 88.36 kbaud 還低**（margin
更緊，不是更寬），[[2b_step0_lpuart_lse]] A2 對方案 (c) 在 250000 baud
下直接用 HSI16 從關閉狀態喚醒的疑慮**不但沒有被真實數字打消，反而更
嚴重**——250000 baud 是這個真實 `BaudMax` 上限的 **3.4 倍**（用 Max
tWULPUART 算）到 2.35 倍（用 Typ 算）。[[2b_step0_lpuart_lse]] 的 C1
交叉引用（「這四個參數 DS13086 才能補」）保持有效，只是現在這四個
參數已經有真實數字可以代入，原文件本身不需要改，讀者跟著連結過來
即可在這裡看到重算結果。

**3. `tSU(HSI16)` → 第 6 節 (A)/(B) 假說**：`tsu(HSI16)` Max=3.6µs
遠小於 `s0`=33.750µs（約 1/9.4），**支持假說 (A)**（`s0` 受指令執行
時間所限，不是等振盪器）——但只是支持，不是證明，(A)/(B) 在這個
數字下依然無法區分，需要電流量測才能分辨，細節見第 6 節新增小節。

**4. `tSU(LSE)` → [[2b_step0_lpuart_lse]] part B 的「<0.5s」實測上界**：
★ **這裡出現一個沒預期到的落差**：DS13086 的 `tSU(LSE)` Typ 是
**2 秒**，比本輪實測的「<0.5 秒（含 SWD 連線開銷）」上界**慢了至少
4 倍**——順序反過來了：規格書 typ 比實測上界還慢，不是「實測落在
規格範圍內」這種正常情況。可能的原因（本輪都沒有進一步驗證，如實
列出，不猜哪個對）：
   - 這片板子的實際 LSE 晶體起振比「standard crystal」（datasheet 註
     4 講的參考晶體）快，datasheet 自己也講 typ 值「can vary
     significantly with the crystal manufacturer」；
   - part B 量到的「<0.5s」可能量到的是 `LSERDY` 旗標第一次被設定的
     時間點，不是 datasheet 定義的「stabilized 32.768kHz oscillation」
     （旗標 ready 不一定等於真正穩定，兩者對「起振完成」的定義可能
     不同）；
   - part B 的量測方法本身含 SWD 連線開銷，精確度/取樣頻率可能不足以
     抓到 2 秒等級的事件邊界（如果實際起振真的要 ~2s，量測方法有沒有
     可能誤判成 <0.5s，本輪沒有覆核）。

   **這個落差本身就是一個發現，需要在 [[2b_step0_lpuart_lse]] 未來
   修訂時處理**（本檔案的任務範圍只更新這一份文件，不動
   `2b_step0_lpuart_lse.md`，但交叉引用讀者需要知道這個矛盾存在，
   不能只當作「實測比規格快，皆大歡喜」略過）。方案 (b)（LSE + 9600
   動態切換）如果依賴「LSE 起振快」這個假設來安排切換時機，這個 4
   倍落差是需要優先釐清的風險，比 `tWUSTOP2`/`tWULPUART` 兩項更
   需要後續動作。

---

## 8. 我無法判定的事

1. ~~`total_latency` 完整數字算不出來：`tWUSTOP2` 這一段完全沒有真實
   數字（DS13086 拿不到），只能報告 `restore_us` 這個已知分量~~ ★
   **DS13086 已取得，`STOPWUCK=1` 已有精確 `tWUSTOP2`（Table 74 HSI16
   列，Max 25.0µs），`total_latency` max 算出來是 121.625µs**（第 5
   節）。**仍無法判定的部分縮小為**：`STOPWUCK=0`（MSIS 4MHz）沒有
   精確的 `tWUSTOP2` 列，只能用 MSI 24MHz/MSI 1MHz 當界
   （25.0~60.0µs），導致 `total_latency` 卡在 145.938~180.938µs
   之間，優/中兩檔判讀無法確定是哪一檔（雖然兩種可能都遠低於
   320µs，這一關穩穩通過）。要縮小這個界，需要 ST 提供更細的頻率
   對照表，或自行用示波器/邏輯分析儀直接量測 GPIO 翻轉時間點來抓
   `tWUSTOP2` 本身，本輪都沒有做。
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
   實測~~ ~~已在第 6 節（G5）測過，但淨收益依然未知~~ ★ **已在第 6 節
   （G5）測過，DS13086 拿到後，淨收益從「完全未知」變成「max 而言
   有正收益，但仍有兩個未消除的假設」**：`s0`（軟體可見段）降了
   25.312µs，用 DS13086 的 `tWUSTOP2` 重算 `total_latency` max，
   `STOPWUCK=1`（121.625µs）比 `STOPWUCK=0`（即使用最偏袒
   `STOPWUCK=0` 的下界，145.938µs）低 24.3µs（第 5、6 節）——但這
   依賴「`tWUSTOP2` 隨喚醒時脈頻率單調」的假設（DS13086 沒明講），
   且是拿軟體實測 max 跟規格書 characterization Max 比，兩者統計基礎
   不完全對等。另外，`HSI16` 精度不如校準過的振盪器、對開機初期時序
   穩定性有沒有影響，這點本輪也還是沒測——G5 只跑了 70 次（跟 G3
   對等的兩組），樣本數不足以看出這種精度問題可能造成的長尾影響。
5. **這組數字的代表性**：只在同一塊板子、同一次連續測試（兩組合計
   70 次）裡量到，沒有跨 session、跨溫度、跨板卡的重複驗證，
   不確定是否能代表這顆晶片/這個設計的通用行為。
