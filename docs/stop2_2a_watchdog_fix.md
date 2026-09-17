# Stop2 Milestone 2A — IWDG 餵狗修復 + LSI 精確量測

延續 `5a208532`。前兩輪（`98c213cb`、`5a208532`）已確立：Stop2 進出本身
成立（每一次完成的循環都回報 `sws=3 pll1rdy=1 restore_timeout=0
lptim_timeout=0 entry_fail=0`），觀察到的所有重置都來自 IWDG，不是
Stop2 序列本身壞掉。本輪做真正的程式碼修改（餵狗）+ 完整硬體量測。

範圍：**只解除限制 A（多輪累積耗時超過 IWDG 預算）**。限制
B（單次 Stop2 睡眠本身不能超過 IWDG 逾時）在核心停在 `wfi` 期間物理上
無法用韌體解決，本輪目標是精確量出它現在的邊界，而不是消除它。

---

## 階段 0：前置與安全網

```
$ ssh arduino@192.168.40.120 'ls -l ~/klipper_asflashed_20260816.bin'
-rw-r--r-- 1 root root 65536  9月 17 11:10 /home/arduino/klipper_asflashed_20260816.bin

$ ssh arduino@192.168.40.120 'sudo systemctl stop klipper; pgrep -x openocd || echo NO_OPENOCD'
NO_OPENOCD
```

回復韌體存在、passwordless sudo 可用、無殘留 openocd，安全網齊備。

---

## 階段 1：程式碼修改（IWDG 餵狗）

只改 `src/stm32/stm32u5_lowpower.c`，commit `6b13ef47`：

```diff
--- a/src/stm32/stm32u5_lowpower.c
+++ b/src/stm32/stm32u5_lowpower.c
@@ -39,6 +39,7 @@
 #include "compiler.h"           // __visible
 #include "board/irq.h"          // irq_disable, irq_enable
 #include "board/armcm_boot.h"   // DECL_ARMCM_IRQ
+#include "internal.h"           // IWDG
 
 // Step 0-4 的 SYSCLK 重建序列定義在 stm32u5.c，冷開機與 Stop2 喚醒共用
 // 同一份，避免複製貼上兩份（見該檔內 stm32u5_sysclk_bringup() 的註解）。
@@ -239,7 +240,15 @@ stop2_capture_status(uint32_t entry_fail, uint32_t restore_timeout,
 /* ===== Milestone 2A：Stop2 進出一次 =====
  * 呼叫前必須先跑過 lptim1_wakeup_init()，其設定的週期存在
  * s_lptim1_arr。流程：熄燈 → 武裝 LPTIM1 單次計數 → 關 SysTick →
- * 進 Stop2 → wfi 醒來 → 還原 SYSCLK/SysTick → 依讀回結果點對應的燈。 */
+ * 進 Stop2 → wfi 醒來 → 還原 SYSCLK/SysTick → 依讀回結果點對應的燈。
+ *
+ * IWDG 餵狗（wfi 前後各一次 IWDG->KR=0xAAAA）只解除「限制 A：多輪
+ * 累積耗時超過 IWDG 預算」——watchdog_reset() 是 DECL_TASK，只有排程器
+ * 的 task loop 跑到它才會餵狗，而 command_test_stop2() 的多輪迴圈整段
+ * 都在同一次 command handler 呼叫裡跑完，中途不會把控制權還給排程器，
+ * 所以要在這裡手動補餵。「限制 B：單次 Stop2 睡眠不能超過 IWDG 逾時」
+ * 依然存在且無法用韌體解決——核心整個停在 wfi 期間，兩次手動餵狗中間
+ * 的空窗依舊是同一顆 IWDG 倒數，若單次睡眠本身就超過逾時，狗必定咬人。 */
 void stop2_once(void)
 {
     volatile uint32_t i;
@@ -333,9 +342,11 @@ void stop2_once(void)
      * 「根本沒進 Stop2」最關鍵的一行。
      * isb：wfi 醒來後立刻沖刷管線，確保接下來抓到的指令流反映喚醒
      * 後的真實狀態，而不是深度睡眠前殘留的預取結果。 */
+    IWDG->KR = 0xAAAA;     /* 睡前餵飽，讓倒數從滿格開始（見限制 A/B 說明） */
     __asm volatile ("dsb" ::: "memory");
     __asm volatile ("wfi");
     __asm volatile ("isb" ::: "memory");
+    IWDG->KR = 0xAAAA;     /* 醒來立刻餵，在 sysclk_restore() 之前 */
 
     /* 醒來的第一件事永遠是拆除 SLEEPDEEP/LPMS，而不是先做時鐘還原。
      * 原因：這兩顆是「進入」深度睡眠的開關，本身不會被硬體自動清除；
```

`dsb`/`wfi`/`isb` 相對順序、`SLEEPDEEP` 清除時機皆未變動；`IWDG` 存取方式
（`IWDG->KR = 0xAAAA`）比照 `src/stm32/watchdog.c` 既有寫法。commit、push
到 `origin/stop2-milestone-2a`。

### 追加：clamp 提高（另一個獨立 commit）

執行階段 5 時發現 `cycles>20` 會被靜默 clamp 成 `cycles=5`（既有邏輯，
不是本輪新增），導致 `cycles=50` 的量測實際上只跑了 5 輪就結束，算出離譜
的 `f_LSI≈340kHz`。回報給人工確認後，比照建議把上限提高到 200，commit
`e9985ad2`：

```diff
--- a/src/stm32/stm32u5_lowpower.c
+++ b/src/stm32/stm32u5_lowpower.c
@@ -408,7 +408,7 @@ command_test_stop2(uint32_t *args)
 
     if (period_ms == 0 || period_ms > 2000)
         period_ms = 2000;              /* LPTIM1 ARR 16bit/32kHz 上限 */
-    if (cycles == 0 || cycles > 20)
+    if (cycles == 0 || cycles > 200)
         cycles = 5;
 
     led3_init();
```

同時人工指出：command handler 執行期間 MCU 對 host 完全無回應，過長的
單次靜默期會讓 `console.py` 的 clocksync 估計器大幅偏移、需要很久才收斂，
也有讓序列層判定失聯的風險。因此把單次指令的累積睡眠上限訂為 5 秒，
階段 5／7 的參數相應調整（見各自章節），不再使用原規格的
`cycles=50`／`cycles=100`。

---

## 階段 2：建置與燒錄

兩次建置/燒錄（IWDG 餵狗、clamp 提高各一次），流程相同：

```
$ git push arduino@192.168.40.120:/home/arduino/klipper stop2-milestone-2a:refs/heads/incoming
$ ssh arduino@192.168.40.120 'cd ~/klipper && git merge --ff-only incoming && git branch -d incoming'
```

第一次（IWDG 餵狗，`6b13ef47`）：

```
  Compiling out/src/stm32/stm32u5_lowpower.o
Version: v0.13.0-473-g6b13ef479
  Linking out/klipper.elf
  Creating bin file out/klipper.bin
-rwxrwxr-x 1 arduino arduino 36040 ... out/klipper.bin
   text	   data	    bss	    dec	    hex	filename
  35987	     52	   1136	  37175	   9137	out/klipper.elf
```

```
** Programming Finished **
** Verify Started **
** Verified OK **
```

identify 確認：

```
Loaded 131 commands (v0.13.0-473-g6b13ef479 / ...)
006.479: uptime high=0 clock=2318391969
```

第二次（clamp 提高，`e9985ad2`）：

```
Version: v0.13.0-474-ge9985ad22
-rwxrwxr-x 1 arduino arduino 36040 ... out/klipper.bin
   text	   data	    bss	    dec	    hex	filename
  35987	     52	   1136	  37175	   9137	out/klipper.elf
```

```
** Verified OK **
```

identify 確認：`Loaded 131 commands (v0.13.0-474-ge9985ad22 / ...)`。

兩次 text 皆在 35000-50000 範圍內、`git describe` 都對得上，未觸發回復
流程。

---

## 階段 3：韌體假設的 LSI 頻率

```
$ grep -n "LSI_HZ_NOMINAL" src/stm32/stm32u5_lowpower.c
95:#define LSI_HZ_NOMINAL    32000u
...
196:    arr = (ms * LSI_HZ_NOMINAL) / 1000u;
```

韌體換算 ARR 時假設 **LSI = 32000 Hz**，這是下面所有比值換算成絕對 Hz
的基準值。

---

## 階段 4：每輪固定開銷的線性回歸

`period_ms=200` 固定，掃 `cycles = 1, 2, 4, 8, 16`。每檔獨立跑，之間
`sleep 5`。全部使用 IWDG 餵狗後的韌體（`6b13ef47`），全數無重置。

### 原始輸出（逐字，`connected` 之後的部分；HELP 文字與 MCU config 略）

```
=====REGRESSION_cycles=1=====
005.330: stats count=113 sum=210788 sumsq=2225824
006.429: uptime high=1 clock=3081464223
009.639: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.512: stats count=56 sum=30919270 sumsq=4294967295
015.495: stats count=56 sum=39379 sumsq=145916
019.440: uptime high=2 clock=843375529
020.477: stats count=56 sum=40803 sumsq=160486
025.460: stats count=55 sum=38730 sumsq=143789
（中間 INFO:root:Resetting prediction variance 略，見完整終端輸出）

=====REGRESSION_cycles=2=====
004.801: stats count=149 sum=323891 sumsq=3629332
006.433: uptime high=3 clock=177109493
009.643: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.035: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.224: stats count=53 sum=61748067 sumsq=4294967295
015.266: stats count=57 sum=40765 sumsq=160053
019.444: uptime high=3 clock=2201850206

=====REGRESSION_cycles=4=====
004.602: stats count=148 sum=323541 sumsq=3629549
006.427: uptime high=4 clock=1529051940
009.637: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.028: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.420: stop2_status cycle=3 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.813: stop2_status cycle=4 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.002: stats count=56 sum=123458707 sumsq=4294967295
016.050: stats count=57 sum=42148 sumsq=173510
019.437: uptime high=4 clock=3489638384
021.046: stats count=58 sum=41708 sumsq=160394

=====REGRESSION_cycles=8=====
005.433: stats count=113 sum=208158 sumsq=2186477
006.425: uptime high=5 clock=2828467588
009.635: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.027: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.419: stop2_status cycle=3 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.810: stop2_status cycle=4 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.203: stop2_status cycle=5 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.595: stop2_status cycle=6 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.987: stop2_status cycle=7 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
012.378: stop2_status cycle=8 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
012.568: stats count=46 sum=246872887 sumsq=4294967295
017.593: stats count=64 sum=49500 sumsq=208753
019.436: uptime high=6 clock=365981704
022.576: stats count=56 sum=40823 sumsq=160794

=====REGRESSION_cycles=16=====
002.020: stats count=145 sum=317421 sumsq=3582831
006.435: uptime high=6 clock=3979097774
007.003: stats count=56 sum=40879 sumsq=161396
009.647: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.039: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.431: stop2_status cycle=3 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.823: stop2_status cycle=4 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.215: stop2_status cycle=5 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.607: stop2_status cycle=6 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
011.998: stop2_status cycle=7 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
012.391: stop2_status cycle=8 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
012.783: stop2_status cycle=9 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.175: stop2_status cycle=10 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.566: stop2_status cycle=11 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.958: stop2_status cycle=12 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.350: stop2_status cycle=13 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.742: stop2_status cycle=14 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
015.134: stop2_status cycle=15 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
015.526: stop2_status cycle=16 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
015.715: stats count=28 sum=493700380 sumsq=4294967295
019.449: uptime high=7 clock=1260477874
020.760: stats count=71 sum=60912 sumsq=291440
025.742: stats count=55 sum=38772 sumsq=144280
```

（`INFO:root:Resetting prediction variance ...` 診斷行每檔都大量出現，
與前兩輪一致，此處全部省略，不影響下面用 `get_uptime` 前後兩筆算
`mcu_ticks` 的方法——那個方法本來就不依賴這些行。）

### 每檔 mcu_ticks / slept_sec

全部 `reset=False`（`mcu_ticks` 皆為正值）：

| cycles | mcu_ticks | wall_sec | mcu_sec | slept_sec |
|---|---|---|---|---|
| 1  | 2056878602 | 13.011 | 12.8555 | 0.1555 |
| 2  | 2024740713 | 13.011 | 12.6546 | 0.3564 |
| 4  | 1960586444 | 13.010 | 12.2537 | 0.7563 |
| 8  | 1832481412 | 13.011 | 11.4530 | 1.5580 |
| 16 | 1576347396 | 13.014 |  9.8522 | 3.1618 |

### 線性回歸：`slept_sec = a × cycles + b`

```
a (斜率，每輪秒數) = 0.200410
b (截距，秒)       = -0.044934
R²                 = 1.000000
```

殘差表：

| cycles | observed (s) | predicted (s) | residual (s) |
|---|---|---|---|
| 1  | 0.1555 | 0.1555 | +0.00003 |
| 2  | 0.3564 | 0.3559 | +0.00048 |
| 4  | 0.7563 | 0.7567 | -0.00037 |
| 8  | 1.5580 | 1.5583 | -0.00035 |
| 16 | 3.1618 | 3.1616 | +0.00021 |

擬合幾乎完美（`R²=1.000000`，殘差全部 <0.5ms），確認「每輪固定開銷」
（LPTIM 重新武裝 + `busy_delay` + `sendf`）確實存在，且對 200ms
這個 period 而言相對很小。

**`a` 與 0.200 比較（任務要求的算法）**：

```
ratio(f_假設/f_LSI) = a / 0.200 = 1.00205
f_LSI = 32000 / ratio = 31934.5 Hz
```

**補充推導（不是任務原本要求，但數學上免費拿到，交叉驗證用）**：截距
`b` 之所以是負值，是因為 `slept_sec` 的計算把 `mcu_ticks` 除以「假設」
的 160,000,000 Hz，而實際 SYSCLK/HSI16（前兩輪已發現）比標稱值快約
0.35%。用截距 `b = wall×(1-r)`（`r`=實際頻率/160MHz、`wall`≈13.0116s
的平均）與斜率 `a = 0.200×(32000/f_LSI)×r` 聯立求解：

```
r (SYSCLK/HSI16 偏差) = 1.003453
f_LSI（修正後）= 0.200 × 32000 × r / a = 32044.8 Hz
```

兩種算法（任務原本要求的 naive 版、聯立修正版）分別給出 `31934.5 Hz` 與
`32044.8 Hz`，彼此相差僅約 110Hz（0.3%），互相印證方向一致；下面階段 5
的精確量測會取代這兩個粗估。

---

## 階段 5：LSI 精確量測（修正參數：`cycles=20`，5 次獨立測量）

原規格 `cycles=50` 被既有的 `cycles>20 → 5` clamp 靜默截斷（見階段 1
「追加」小節），改用 `period_ms=200 cycles=20`（累積睡眠 4 秒），跑 5 次
獨立測量，每次之間 `sleep 5`。

### 原始輸出（逐字，`connected` 之後；HELP/MCU config 略，`Resetting
prediction variance` 診斷行略——不影響下方算法）

```
=====LSI_RUN_1=====
003.718: stats count=147 sum=321342 sumsq=3612916
006.438: uptime high=1 clock=3341679793
008.701: stats count=56 sum=40878 sumsq=161384
009.648: stop2_status cycle=1 ... 017.094: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
017.678: stats count=25 sum=617155932 sumsq=4294967295
022.661: stats count=55 sum=35403 sumsq=101429
024.449: uptime high=2 clock=1297154570
027.643: stats count=64 sum=53377 sumsq=246077
032.626: stats count=56 sum=39330 sumsq=145196

=====LSI_RUN_2=====
002.072: stats count=144 sum=316120 sumsq=3575745
006.441: uptime high=3 clock=616403504
007.055: stats count=56 sum=40876 sumsq=161492
009.651: stop2_status cycle=1 ... 017.098: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
017.292: stats count=27 sum=617132716 sumsq=4294967295
022.312: stats count=68 sum=57244 sumsq=271871
024.451: uptime high=3 clock=2866450629
027.295: stats count=65 sum=55433 sumsq=263862
032.278: stats count=56 sum=39373 sumsq=145699

=====LSI_RUN_3=====
001.698: stats count=146 sum=318208 sumsq=3585601
006.445: uptime high=4 clock=2190208723
006.681: stats count=56 sum=40878 sumsq=161384
009.655: stop2_status cycle=1 ... 017.102: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
017.297: stats count=31 sum=617132083 sumsq=4294967295
022.336: stats count=68 sum=57244 sumsq=271871
024.456: uptime high=5 clock=145492823
027.319: stats count=66 sum=55928 sumsq=265130
032.302: stats count=55 sum=38792 sumsq=144519

=====LSI_RUN_4=====
001.699: stats count=146 sum=318064 sumsq=3584754
006.442: uptime high=5 clock=3766655743
006.682: stats count=56 sum=40836 sumsq=160876
009.652: stop2_status cycle=1 ... 017.098: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
017.292: stats count=31 sum=617133821 sumsq=4294967295
022.335: stats count=68 sum=57264 sumsq=272360
024.453: uptime high=6 clock=1722130765
027.318: stats count=66 sum=55846 sumsq=264008
032.300: stats count=55 sum=38730 sumsq=143789

=====LSI_RUN_5=====
001.728: stats count=145 sum=317685 sumsq=3588626
006.430: uptime high=7 clock=1042104569
006.711: stats count=56 sum=40856 sumsq=161116
009.640: stop2_status cycle=1 ... 017.087: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
017.281: stats count=31 sum=617140021 sumsq=4294967295
022.266: stats count=68 sum=57244 sumsq=271871
024.441: uptime high=7 clock=3292259999
027.249: stats count=66 sum=55851 sumsq=264082
032.232: stats count=55 sum=38770 sumsq=144405
```

每一輪 `cycle=1` 到 `cycle=20` 全數完整送達，皆為
`sws=3 pll1rdy=1 restore_timeout=0 lptim_timeout=0 entry_fail=0`
（中間 18 行按 cycle 遞增，內容跟頭尾一致，此處以 `...` 省略，完整逐字
稿與前四階段相同的重複格式，未省略任何欄位種類）。

### 每次測量結果

| run | mcu_ticks | wall_sec | slept_sec | ratio (vs 4.0s) | f_LSI (Hz) |
|---|---|---|---|---|---|
| 1 | 2250442073 | 18.011 | 3.9457 | 0.98643 | 32440.1 |
| 2 | 2250047125 | 18.010 | 3.9472 | 0.98680 | 32428.0 |
| 3 | 2250251396 | 18.011 | 3.9469 | 0.98673 | 32430.3 |
| 4 | 2250442318 | 18.011 | 3.9457 | 0.98643 | 32440.1 |
| 5 | 2250155430 | 18.011 | 3.9475 | 0.98688 | 32425.4 |

全部 `reset=False`（`mcu_ticks` 皆為正值，且五次數值高度一致，波動
<0.02%）。

```
平均 slept_sec = 3.9466s（stdev 0.00084s）
平均 ratio     = 0.98666（stdev 0.000210）
平均 f_LSI     = 32432.8 Hz（stdev 6.91 Hz，約 0.021%）
```

精度驗證：CYCCNT 在 Stop2 停走，`slept_sec` 只計入睡眠、自動排除清醒
開銷；累積 4 秒睡眠、console 時間戳解析度約 1ms，理論比值精度約
1ms/4000ms=0.025%，與實測 stdev（0.021%）吻合，符合人工判斷的預期。

---

## 階段 6：限制 B 的新邊界（`cycles=1`，`period_ms` 掃描）

`period_ms = 400, 450, 475, 500, 510, 525, 550`，每檔 `sleep 20`。全部
在 IWDG 餵狗生效後的韌體上執行。

### 原始輸出（逐字，`connected` 之後；HELP/MCU config、`Resetting
prediction variance` 略）

```
=====LIMITB_period=400=====
003.459: stats count=148 sum=321927 sumsq=3614254
006.442: uptime high=9 clock=2832167817
008.442: stats count=56 sum=40836 sumsq=160876
009.857: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.824: stats count=55 sum=31711662 sumsq=4294967295
018.806: stats count=55 sum=38779 sumsq=144509
023.790: stats count=55 sum=38792 sumsq=144519
028.773: stats count=56 sum=39372 sumsq=145687
029.453: uptime high=10 clock=2167420393
033.756: stats count=56 sum=40825 sumsq=171234
038.739: stats count=56 sum=40735 sumsq=169822
043.722: stats count=56 sum=39412 sumsq=146303

=====LIMITB_period=450=====
003.109: stats count=146 sum=319406 sumsq=3598285
006.426: uptime high=11 clock=2295811237
008.093: stats count=56 sum=40836 sumsq=160876
009.892: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.524: stats count=56 sum=31920659 sumsq=4294967295
018.507: stats count=56 sum=39339 sumsq=145300
023.490: stats count=55 sum=38770 sumsq=144405
028.473: stats count=55 sum=38750 sumsq=144021
029.437: uptime high=12 clock=1622958518
033.456: stats count=56 sum=40783 sumsq=160254
038.439: stats count=55 sum=38730 sumsq=143789
043.422: stats count=55 sum=38730 sumsq=143789

=====LIMITB_period=475=====
002.929: stats count=145 sum=318784 sumsq=3596619
006.444: uptime high=13 clock=1737556672
007.912: stats count=57 sum=41499 sumsq=162893
009.934: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.369: stats count=56 sum=32021053 sumsq=4294967295
018.352: stats count=55 sum=38739 sumsq=143893
023.335: stats count=56 sum=39350 sumsq=145504
028.318: stats count=55 sum=38770 sumsq=144329
029.454: uptime high=14 clock=1060756076
033.301: stats count=56 sum=40783 sumsq=160254
038.284: stats count=56 sum=39330 sumsq=145196
043.267: stats count=55 sum=38730 sumsq=143789

=====LIMITB_period=500=====
002.753: stats count=148 sum=320606 sumsq=3600840
006.448: uptime high=15 clock=1176653300
007.736: stats count=56 sum=40899 sumsq=161643
009.965: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.217: stats count=56 sum=32125767 sumsq=4294967295
018.200: stats count=55 sum=38740 sumsq=143905
023.183: stats count=55 sum=38772 sumsq=144280
028.166: stats count=56 sum=39392 sumsq=145995
029.459: uptime high=16 clock=496022228
033.149: stats count=56 sum=40845 sumsq=160984
038.133: stats count=55 sum=38772 sumsq=144280
043.116: stats count=56 sum=39372 sumsq=145687

=====LIMITB_period=510=====
002.597: stats count=145 sum=318233 sumsq=3595914
006.442: uptime high=17 clock=610813382
007.580: stats count=56 sum=40856 sumsq=161184
009.968: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.071: stats count=55 sum=32166215 sumsq=4294967295
018.054: stats count=57 sum=41303 sumsq=160653
023.037: stats count=55 sum=38772 sumsq=144280
028.020: stats count=55 sum=38772 sumsq=144280
029.452: uptime high=17 clock=4223390682
033.003: stats count=57 sum=41465 sumsq=162768
037.986: stats count=55 sum=38792 sumsq=144519
042.969: stats count=55 sum=38772 sumsq=144280

=====LIMITB_period=525=====
002.422: stats count=146 sum=319406 sumsq=3598285
006.445: uptime high=19 clock=49360740
007.405: stats count=57 sum=41496 sumsq=163221
（此後 45 秒視窗內完全沒有其他輸出——無 cycle=1、無 starting、無第二個
get_uptime，逐字照抄，沒有省略任何行）

=====LIMITB_period=550=====
004.259: stats count=148 sum=323439 sumsq=3628639
006.444: uptime high=1 clock=3255731803
009.242: stats count=56 sum=40878 sumsq=161384
019.940: stats count=65 sum=47261 sumsq=162343
024.923: stats count=55 sum=38730 sumsq=153818
029.455: uptime high=0 clock=3127528879
029.905: stats count=57 sum=42746 sumsq=185336
034.888: stats count=55 sum=38730 sumsq=143789
039.871: stats count=55 sum=38770 sumsq=144405
```

### 判讀

| period_ms | 結果 | mcu_ticks | handler_dur (s) | 說明 |
|---|---|---|---|---|
| 400 | 成功 | 3630219872 (正) | 0.415 | cycle=1 送達，high 9→10 正常溢位 |
| 450 | 成功 | 3622114577 (正) | 0.466 | cycle=1 送達，high 11→12 正常溢位 |
| 475 | 成功 | 3618166700 (正) | 0.490 | cycle=1 送達，high 13→14 正常溢位 |
| 500 | 成功 | 3614336224 (正) | 0.517 | cycle=1 送達，high 15→16 正常溢位 |
| **510** | **成功（最後一個）** | 3612577300 (正) | **0.526** | cycle=1 送達，high 同為 17（無溢位，clock 值直接變大） |
| 525 | **無法判定** | — | — | 只有一筆 uptime，之後 45 秒視窗完全靜默；沒有第二筆讀數，`mcu_ticks` 無法計算 |
| **550** | **確認重置** | -4423170220 (負) | 無 cycle=1 | high 1→0；**這次連 cycle=1 都沒送達**，代表重置發生在單次 wfi 睡眠期間，不是睡完之後才被咬 |

`period_ms=550` 這筆特別重要：這是本次整個 Milestone 2A 系列量測中，
**第一次觀察到「連第一輪都沒完成」的失敗**——前幾輪（`98c213cb`、
`5a208532`）裡，所有「最終被重置」的案例，`cycle=1` 都成功送達過，重置
永遠發生在「睡完之後」，因為那時候的重置成因是「距離上次餵狗的累積時間」
（包含指令開始前就已經消耗掉的預算），而不是單次 `wfi` 本身太長。加了
IWDG 餵狗之後，`wfi` 前才剛餵飽，這個「指令開始前的預算」問題被完全
排除，`period_ms=550` 這次的失敗因此第一次乾淨地反映出「純粹的限制
B」：**單次睡眠本身的長度就超過了 IWDG 硬體逾時**，狗在 `wfi` 期間直接
咬人，連醒來執行 `sysclk_restore()`/`sendf()` 的機會都沒有。

`period_ms=525` 無法判定的原因很可能是同一件事（睡眠中被咬），但因為
連線在這裡完全沒留下第二筆 `get_uptime`，嚴格說不能確認；下一檔
（`period_ms=550`）一開始的 `high=1`（剛重開機不久的數值），是
`period_ms=525` 這次很可能也重置過的間接佐證，但這是跨檔案的旁證，不是
`525` 這一檔自己單獨可判定的直接證據，報告裡明確分開標註。

### T_boundary 與 f_LSI

用最後一個確認成功（`period_ms=510`，`handler_dur=0.526s`）與第一個
確認重置（`period_ms=550`，睡眠中被咬，沒有 `handler_dur` 可用，只能用
其請求的睡眠長度 `0.550s` 當作上界）：

```
T_boundary ∈ (0.526s, 0.550s]
f_LSI(0.550s) = 16384/0.550 = 29789.1 Hz
f_LSI(0.526s) = 16384/0.526 = 31148.7 Hz
```

即 **這個方法給出的 f_LSI 區間是 [29789, 31149] Hz**。

---

## 階段 6.5：與階段 5 並列比較（★ 三個獨立估計值）

| 來源 | 方法 | f_LSI 估計 |
|---|---|---|
| `5a208532`（上一輪） | `cycles=1` 邊界掃描，用單次事件的 print 時間戳做 `(T_ok, T_fail]` 括號，累積睡眠僅約 0.5 秒 | [30970, 31752] Hz |
| **本輪階段 6** | 同樣方法（`cycles=1` 邊界掃描），但邊界點不同（`510`/`550`），累積睡眠仍僅約 0.5 秒 | [29789, 31149] Hz |
| **本輪階段 5** | `cycles=20` 累積約 4 秒睡眠，直接用 `get_uptime` 硬體 tick 差除以 160MHz，5 次獨立測量取平均 | **32432.8 ± 6.91 Hz**（stdev 0.021%） |

三者**不完全一致**：階段 5 的值（32433 Hz）高於階段 6 與 `5a208532` 給出
的兩個區間的上緣（31149 / 31752 Hz），兩個「邊界掃描」方法彼此之間倒是
互相重疊、內部一致。不自己選一個，把可能的系統性原因列出來：

1. **信號長度差一個數量級**：階段 5 的訊號是 4 秒的凍結時間，階段
   6／`5a208532` 的訊號只有約 0.5 秒——同樣的時間戳量測誤差（估計約
   1ms 等級），在短訊號上造成的相對誤差是長訊號的 8 倍，這是最直接的
   精度差異來源。
2. **測量方式不同源**：階段 5 全程只用 `get_uptime` 回傳的原始硬體
   tick（`high`/`clock`），沒有依賴任何一筆訊息的「印出時間戳」；階段
   6／`5a208532` 的 `T_boundary` 則是直接拿 `stop2_status`/`starting`
   這類事件的**印出時間戳**做差。這兩種資料來源在方法論上不對等——
   前面兩輪已經觀察到，`console.py` 印出的時間戳是透過 host 端的
   clock-sync 估計模型換算出來的，而這個模型在 Stop2 睡眠前後會經歷
   一段時間的「重新估計」擾動（`Resetting prediction variance` 那一長串
   訊息就是證據）；`cycle=1` 的時間戳正好落在這段擾動剛開始的地方，
   有系統性高估的可能——這正是本輪 stage6 補充說明裡提到的「handler
   內的時間會略低估真正的 IWDG 逾時」那個效應的另一面：印出時間戳本身
   也可能被這段擾動汙染，方向不確定（可能高估也可能低估單一事件的
   絕對值），但階段 5 完全不使用這類「事件時間戳」，只用兩個相隔甚遠、
   模型已經穩定的 `get_uptime` 讀數做差，理論上不受這個效應影響。
3. **兩個獨立輪次（`5a208532` 與本輪階段 6）用了同一種易受擾動污染的
   方法，卻互相吻合**：這可能代表印出時間戳的擾動是「系統性、可重現」
   的（同樣條件下每次都往同一個方向偏一樣多），而不是隨機雜訊——如果
   屬實，這反而說明用這個方法量到的區間本身很穩定，只是相對於真值有
   一個固定的偏移量，而不是偶然湊巧。

**哪個最可信：階段 5**。理由：(a) 唯一不依賴任何「事件印出時間戳」的
方法，直接用硬體 tick 差；(b) 訊號長度是另外兩者的 8 倍，量測解析度
造成的相對誤差小一個數量級；(c) 5 次獨立重複測量彼此高度一致（stdev
僅 0.021%），內部精度遠優於另外兩者；(d) 階段 4 的線性回歸（同樣用
`get_uptime` 硬體 tick，不用事件時間戳）修正版算出的 `32044.8 Hz` 與
階段 5 的 `32432.8 Hz` 落在同一數量級、方向一致，形成獨立交叉驗證
（雖然階段 4 訊號更短、精度較低，不能單獨當精確值用，但可以佐證階段
5 沒有離譜偏差）。

---

## 限制 A / 限制 B：修改前後量化對照

| | 修改前（`5a208532`） | 修改後（本輪） |
|---|---|---|
| 限制 A（累積預算） | `period_ms=150`：`cycles=1` 成功，`cycles=2`（僅 300ms 累積）就重置 | 本輪 `period_ms=200`：`cycles=16`（3.2s 累積）、階段 5 `cycles=20`（4s 累積）、階段 7 `cycles=20`（2s 累積，20 次重複中 19 次乾淨）皆不再因累積效應重置 |
| 限制 B（單次上限） | `period_ms=300` 成功、`period_ms=350` 起最終都會被咬（但都是睡完之後才咬，因為預算裡混了「指令開始前已消耗掉的量」） | `period_ms=510` 成功、`period_ms=550` 直接在睡眠中被咬（`cycle=1` 都送不出來）——邊界從約 [503,543]ms（`5a208532` 修正值）左右，變成乾淨的 [510,550]ms，且失敗模式從「事後咬」變成「當場咬」，證明限制 B 現在只反映硬體 IWDG 逾時本身，不再混雜指令啟動前的預算消耗 |

限制 A 實質上已解除（本輪找不到任何因累積效應觸發的重置）；限制 B 仍在，
且量測方法比修改前更乾淨（不再有「事後咬」的模糊地帶）。

---

## 階段 7：耐久測試（修正參數：`period_ms=100 cycles=20`，累積睡眠 2 秒，連續 20 次）

原規格 `cycles=100`（10 秒累積）同樣會被 clamp 影響超出設計意圖的風險
（雖然 100≤200 不會被 clamp 截斷，但人工考量到單次靜默期過長的
clocksync/序列層風險），改為 `period_ms=100 cycles=20`（累積 2 秒），
連續跑 20 次，每次之間 `get_uptime` + `sleep 5`。

### 結果總覽

**19/20 次乾淨完成，全部 20 輪 `stop2_status` 皆為
`sws=3 pll1rdy=1 restore_timeout=0 lptim_timeout=0 entry_fail=0`，且用
`get_uptime` 的 `mcu_ticks`（皆為正值）確認無重置。**

第 1 次有疑點（不是嚴格意義下「確認重置」，見下方說明）。

### 逐次原始輸出（逐字；HELP/MCU config、`Resetting prediction
variance` 略；每次的 `cycle=1..20` 內容格式一致，中間省略號代表跟頭尾
同格式的重複行，欄位種類沒有被省略）

```
=====ENDURANCE_RUN_1=====
006.429: uptime high=6 clock=3369875303
009.536: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
... (cycle=2 ~ cycle=19，皆為相同旗標)
015.039: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
（此後在 25 秒視窗內，沒有再出現任何 "uptime" 行——第二個 get_uptime 的
回應始終沒有送達，逐字照抄，沒有省略）

=====ENDURANCE_RUN_2=====
006.424: uptime high=0 clock=3401965499
009.532: stop2_status cycle=1 ... 015.035: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.443: uptime high=1 clock=714560694

=====ENDURANCE_RUN_3=====
006.425: uptime high=1 clock=3690489308
009.533: stop2_status cycle=1 ... 015.035: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.443: uptime high=2 clock=1003082205

=====ENDURANCE_RUN_4=====
006.436: uptime high=2 clock=3972421800
009.544: stop2_status cycle=1 ... 015.046: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.456: uptime high=3 clock=1285210064

=====ENDURANCE_RUN_5=====
006.448: uptime high=3 clock=4253606349
009.556: stop2_status cycle=1 ... 015.059: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.468: uptime high=4 clock=1566331221

=====ENDURANCE_RUN_6=====
006.443: uptime high=5 clock=241678780
009.551: stop2_status cycle=1 ... 015.053: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.463: uptime high=5 clock=1849291297

=====ENDURANCE_RUN_7=====
006.443: uptime high=6 clock=525354751
009.551: stop2_status cycle=1 ... 015.053: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.463: uptime high=6 clock=2132948904

=====ENDURANCE_RUN_8=====
006.443: uptime high=7 clock=807165018
009.551: stop2_status cycle=1 ... 015.054: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.463: uptime high=7 clock=2414674341

=====ENDURANCE_RUN_9=====
006.440: uptime high=8 clock=1090642480
009.548: stop2_status cycle=1 ... 015.050: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.459: uptime high=8 clock=2698353215

=====ENDURANCE_RUN_10=====
006.441: uptime high=9 clock=1375139144
009.549: stop2_status cycle=1 ... 015.051: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.460: uptime high=9 clock=2982755825

=====ENDURANCE_RUN_11=====
006.439: uptime high=10 clock=1658040389
009.546: stop2_status cycle=1 ... 015.048: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.457: uptime high=10 clock=3265582374

=====ENDURANCE_RUN_12=====
006.436: uptime high=11 clock=1941019838
009.544: stop2_status cycle=1 ... 015.046: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.455: uptime high=11 clock=3548744161

=====ENDURANCE_RUN_13=====
006.446: uptime high=12 clock=2224213090
009.554: stop2_status cycle=1 ... 015.057: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.466: uptime high=12 clock=3832044750

=====ENDURANCE_RUN_14=====
006.444: uptime high=13 clock=2512423704
009.552: stop2_status cycle=1 ... 015.054: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.463: uptime high=13 clock=4120165240

=====ENDURANCE_RUN_15=====
006.438: uptime high=14 clock=2789370719
009.546: stop2_status cycle=1 ... 015.049: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.458: uptime high=15 clock=102253990

=====ENDURANCE_RUN_16=====
006.428: uptime high=15 clock=3075549875
009.536: stop2_status cycle=1 ... 015.038: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.446: uptime high=16 clock=388307320

=====ENDURANCE_RUN_17=====
006.429: uptime high=16 clock=3357145280
009.536: stop2_status cycle=1 ... 015.038: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.446: uptime high=17 clock=669620238

=====ENDURANCE_RUN_18=====
006.432: uptime high=17 clock=3644202513
009.539: stop2_status cycle=1 ... 015.041: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.450: uptime high=18 clock=956772397

=====ENDURANCE_RUN_19=====
006.444: uptime high=18 clock=3927151451
009.551: stop2_status cycle=1 ... 015.053: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.463: uptime high=19 clock=1239985755

=====ENDURANCE_RUN_20=====
006.442: uptime high=19 clock=4210060135
009.550: stop2_status cycle=1 ... 015.052: stop2_status cycle=20 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
018.461: uptime high=20 clock=1522859186
```

### 判讀

| run | 完成輪數 | 重置判定 | 備註 |
|---|---|---|---|
| 1 | 20/20 | **無法直接判定**（缺第二筆 `get_uptime`） | 見下方說明 |
| 2-20 | 20/20 | 確認無重置（`mcu_ticks` 皆為正、彼此高度一致 ~1.6075e9） | 每次都完整 20 輪 |

第 1 次的最後一筆 `stop2_status`（`cycle=20`，`sws=3 pll1rdy=1
restore_timeout=0 lptim_timeout=0 entry_fail=0`）本身完全正常，但這次
測量在整整 25 秒視窗內都沒有等到第二個 `get_uptime` 回應，**依規則
（重置一律以 mcu_ticks 判定）此次本身無法直接判定**。但有一個跨檔的
旁證：第 2 次的**第一筆** `get_uptime`（測試開始前，本應延續第 1 次
結束時的狀態）顯示 `high=0`——如果第 1 次真的完全正常收尾，第 2 次
開始時的 `high` 應該延續第 1 次的 `high=6`（或再往上一點），不應該掉回
0。這個旁證指向：第 1 次很可能在 `cycle=20` 成功送出**之後**（例如
`busy_delay` 尾聲或指令返回途中）發生了一次重置，導致 MCU 重開機、
`high` 歸零，而我方視窗結束前沒能等到重開機後的 `get_uptime` 回應。

**這件事本身值得認真看待**：如果真的發生了，代表即使加了 IWDG
餵狗，仍存在某個**尚未定位**的路徑會在 `stop2_once()` 已經完整正確跑完
（回到 `command_test_stop2()` 的迴圈之後）觸發重置——而且只在 20 次
裡發生了 1 次（或 0 次，如果純屬視窗太短的巧合），無法排除是隨機的
瞬態事件。老實說：**這件事沒有被本輪的量測方法確定，也沒有被推翻**，
列在下一節的「無法判定」清單裡，不下結論。

---

## 階段 8：確認沒搞壞正常運作

```
$ ssh arduino@192.168.40.120 'sudo systemctl start klipper && sleep 20 && curl -s http://localhost:7125/printer/info; echo; echo ---; curl -s "http://localhost:7125/printer/objects/query?mcu"; echo'
{"result":{"state":"ready","state_message":"Printer is ready","hostname":"hunter","klipper_path":"/home/arduino/klipper","python_path":"/home/arduino/klippy-env/bin/python","process_id":9370,...,"software_version":"v0.13.0-474-ge9985ad22","cpu_info":"4 core ?"}}
---
{"result":{"eventtime":16736.589346063,"status":{"mcu":{"mcu_version":"v0.13.0-474-ge9985ad22",...,"last_stats":{"mcu_awake":0.0,"mcu_task_avg":4e-6,"mcu_task_stddev":3e-6,"bytes_write":1040,"bytes_read":5042,"bytes_retransmit":0,"bytes_invalid":0,"send_seq":130,"receive_seq":130,"retransmit_seq":0,"srtt":0.004,"rttvar":0.0,"rto":0.025,"ready_bytes":0,"upcoming_bytes":0,"freq":160545090}}}}}
```

驗收：`state=ready` ✓、`bytes_retransmit=0` ✓、`bytes_invalid=0` ✓、
`srtt=0.004`（在 0.003-0.005 範圍內）✓。

`freq=160545090` Hz——這是本系列第四筆 HSI16 溫漂數據點，與前面推算的
「SYSCLK/HSI16 比標稱 160MHz 快約 0.34-0.35%」吻合
（160545090/160000000=1.00341）。

---

## 我無法從這組數據判定的事

1. **階段 7 第 1 次是否真的重置**：只有跨檔旁證（下一次的 `high=0`），
   本次自己缺第二筆 `get_uptime`，無法用規定的方法直接判定；也無法
   排除純粹是我方 25 秒觀測視窗不夠長導致沒等到回應（雖然其餘 19 次
   同樣的視窗長度都等到了，這點削弱「純視窗太短」的解釋力，但不能
   當作證據排除它）。
2. **若階段 7 第 1 次真的重置，是什麼路徑造成的**：IWDG 餵狗理論上應該
   完全消除 `stop2_once()` 内部的累積效應；如果重置真的發生在
   `cycle=20` 送出之後，那個時間點程式已經離開 `stop2_once()`、進入
   `command_test_stop2()` 迴圈尾端或已經返回——這段路徑本輪沒有插入
   額外餵狗，理論上此時控制權應該已經還給排程器、`watchdog_reset()`
   應該很快就會被排到；如果真的還是被咬，代表對排程器行為的假設可能
   有遺漏，但這是推測，沒有更多數據佐證或推翻。
3. **`period_ms=525` 是否真的重置**：45 秒視窗完全靜默，缺乏任何直接
   或間接（同一檔內）的判定依據；跨檔旁證（下一檔 `period_ms=550` 開頭
   `high=1`）指向「大概率是」，但不是嚴格判定。
4. **限制 B 更精確的邊界**：卡在 `(510ms, 550ms]`（或用 `525` 的旁證
   推得可能更接近 `(510ms, 525ms]`），沒有在這個區間內做更細的掃描
   （例如 515、520ms）進一步收斂，也沒有必要——已達到量測方法本身的
   解析度上限（見下一條）。
5. **`5a208532`／本輪階段 6 這兩種「事件時間戳」方法系統性偏低的確切
   機制與偏移量**：目前只有「clock-sync 估計模型在 Stop2 睡眠後有一段
   重新收斂期」這個定性解釋，且是沿用前兩輪已經觀察到的現象，本輪沒有
   做進一步的實驗去分離出這個效應本身的量值。
6. **HSI16/SYSCLK 溫漂的長期穩定性**：本輪拿到第四筆數據點
   （160545090Hz），加上前面三輪的數字，都落在 160.5-160.6MHz 附近、
   高度一致，但這只是在室溫、短時間內（幾天）的觀察，沒有涵蓋溫度
   循環或長期老化。
7. **階段 5 五次測量的 `f_LSI`（32432.8±6.9Hz）本身是否受 SYSCLK/HSI16
   溫漂影響**：階段 5 的算法（`get_uptime` tick 差除以「假設的」
   160,000,000 Hz）理論上跟 SYSCLK 實際頻率無關（`get_uptime` 的 tick
   是硬體 timer 直接輸出，不涉及任何 LSI 相關運算）——但這個「無關」
   的假設本身沒有另外設計實驗去驗證，只是基於暫存器架構的推論。

---

## 收尾

```
$ ssh arduino@192.168.40.120 'sudo systemctl start klipper && sleep 20 && curl -s http://localhost:7125/printer/info'
{"result":{"state":"ready",...}}
```

`klipper.service` 確認 running、`state=ready`。
