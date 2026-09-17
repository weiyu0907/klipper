# Stop2 Milestone 2A — 硬體遙測掃描（IWDG 邊界）

> **⚠️ 參數設計有誤，適用範圍更正**：以下從「前置檢查」到「無法從這組數據
> 判定的事」為止的第一輪掃描，六組全部用 `cycles≥3`，總睡眠時間
> （`cycles×period_ms`）全部 ≥1000ms，必然超過 IWDG 的 410-512ms 預算，
> 所以**測不到「單次 Stop2 睡眠上限」（限制 B）**，只能證明「多輪累積會
> 撞到 IWDG」（限制 A）這個較弱的結論，且該節內對 `period_ms` 邊界、LSI
> 反推的推論都不成立（沒有做，也不應該用那節的數字去反推 LSI）。這一節
> 保留不刪，僅供對照；正確分離限制 A / B 的量測與 LSI 反推見文件最下方
> 新增的「cycles=1 邊界掃描」章節，結論以那一節為準。

固件：`v0.13.0-470-gb48410cc2`（commit `b48410cc`），已燒錄並 verify 通過（見
`docs/stop2_2a_hwtest.md` 前一輪的燒錄紀錄，此檔案不重複貼）。

**方法**：全程只看 host 端遙測（`get_uptime` 的 `high`/`clock`、`test_stop2`
的 `stop2_status` sendf），不看 LED、不臆測 LED。重置判定**一律**以
`get_uptime` 兩次讀數的 `mcu_ticks`（`(high1<<32|clock1) - (high0<<32|clock0)`）
正負為準；`pwr_cr1`/`scb_scr` 只作為輔助欄位記錄，不用來判斷是否重置。

**執行方式**：`console.py` 是互動 REPL，用 `(printf ...; sleep N; ...) | timeout M console.py`
的管道餵指令。第一次嘗試在連線建立前就送指令，被判定為
`Unknown command`（連線握手約需 6-7 秒）；改成先 `sleep 7` 讓連線穩定後
才送第一個 `get_uptime`，之後每一檔都採此寫法，全數成功送達。

---

## 前置檢查

```
$ ssh arduino@192.168.40.120 'sudo systemctl stop klipper; pgrep -x openocd || echo NO_OPENOCD'
NO_OPENOCD
```

## 管道機制驗證（第一次嘗試失敗，第二次修正後成功）

```
$ ssh arduino@192.168.40.120 '(printf "get_uptime\n"; sleep 3) | timeout 15 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
Error: Unknown command: get_uptime
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
WARNING:root:got {'count': 141, 'sum': 310972, 'sumsq': 3536707, '#name': 'stats', '#sent_time': 5404.874201219333, '#receive_time': 5404.910200906667}
====================       connected       ====================
006.166: stats count=57 sum=42709 sumsq=185126
011.150: stats count=57 sum=41335 sumsq=171229
```

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3) | timeout 20 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
004.635: stats count=149 sum=323932 sumsq=3629799
006.431: uptime high=43 clock=3604721347
009.619: stats count=56 sum=40836 sumsq=160876
014.603: stats count=56 sum=39392 sumsq=145995
```

`exit 124` 是我方 `timeout` 主動收工，不代表指令失敗；`console.py` 本身
是持續運行的 REPL，管道 EOF 後它仍會等到 `timeout` 秒數到才被砍掉，這是
預期行為，以下各檔一律如此，不重複註記。

---

## 掃描原始輸出（逐字）

以下 6 檔全部照規格的 `sleep` 值執行，檔與檔之間額外 `sleep 5`。

### 檔 1：period_ms=100 cycles=10（sleep 15）

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=100 cycles=10\n"; sleep 15; printf "get_uptime\n"; sleep 3) | timeout 40 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
005.564: stats count=87 sum=126947 sumsq=1169185
006.447: uptime high=45 clock=2068157535
009.554: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
009.844: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.942: stats count=55 sum=38104 sumsq=135778
INFO:root:Resetting prediction variance 5499.079: freq=160517889 diff=1660504667 stddev=120069.828
INFO:root:Resetting prediction variance 5500.064: freq=220857362 diff=500492366 stddev=160000.000
INFO:root:Resetting prediction variance 5501.050: freq=232143567 diff=213493467 stddev=160000.000
INFO:root:Resetting prediction variance 5502.035: freq=235661390 diff=68798189 stddev=160000.000
INFO:root:Resetting prediction variance 5503.021: freq=236559606 diff=-24979495 stddev=160000.000
024.458: uptime high=0 clock=2324771085
024.926: stats count=57 sum=42788 sumsq=186287
INFO:root:Resetting prediction variance 5504.006: freq=236288380 diff=-93716319 stddev=160000.000
INFO:root:Resetting prediction variance 5505.977: freq=234203799 diff=-191955690 stddev=160000.000
INFO:root:Resetting prediction variance 5506.961: freq=232797622 diff=-228967450 stddev=160000.000
INFO:root:Resetting prediction variance 5507.945: freq=231278262 diff=-260579542 stddev=160000.000
INFO:root:Resetting prediction variance 5508.929: freq=229695251 diff=-287855852 stddev=160000.000
029.909: stats count=56 sum=39372 sumsq=145687
INFO:root:Resetting prediction variance 5509.913: freq=228080408 diff=-311551842 stddev=160000.000
INFO:root:Resetting prediction variance 5510.897: freq=226454786 diff=-332209062 stddev=160000.000
INFO:root:Resetting prediction variance 5511.881: freq=224832714 diff=-350210331 stddev=160000.000
INFO:root:Resetting prediction variance 5512.865: freq=223224287 diff=-365929478 stddev=160000.000
INFO:root:Resetting prediction variance 5513.848: freq=221636338 diff=-379612158 stddev=160000.000
034.893: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5514.832: freq=220073744 diff=-391492078 stddev=160000.000
INFO:root:Resetting prediction variance 5515.816: freq=218539850 diff=-401743603 stddev=160000.000
INFO:root:Resetting prediction variance 5516.800: freq=217036988 diff=-410536340 stddev=160000.000
INFO:root:Resetting prediction variance 5517.784: freq=215566677 diff=-417999828 stddev=160000.000
```

**判讀**：cycle=1、cycle=2 送達（sws=3, pll1rdy=1, 兩個 timeout 旗標皆
0，entry_fail=0）。cycle=3 之後完全沒有出現。第二次 `get_uptime` 回
`high=0`（第一次是 `high=45`）→ `mcu_ticks` 為負 → **判定重置**。

### 檔 2：period_ms=200 cycles=5（sleep 15）

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=200 cycles=5\n"; sleep 15; printf "get_uptime\n"; sleep 3) | timeout 40 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
004.892: stats count=147 sum=322791 sumsq=3627381
006.448: uptime high=4 clock=1469730921
009.658: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
009.976: starting
014.960: stats count=56 sum=38380 sumsq=138208
019.945: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5618.807: freq=160507759 diff=2258848135 stddev=120040.968
INFO:root:Resetting prediction variance 5619.792: freq=242583808 diff=680949109 stddev=160000.000
INFO:root:Resetting prediction variance 5620.778: freq=257938656 diff=290501188 stddev=160000.000
INFO:root:Resetting prediction variance 5621.763: freq=262725218 diff=93645035 stddev=160000.000
INFO:root:Resetting prediction variance 5622.749: freq=263947800 diff=-33925433 stddev=160000.000
024.458: uptime high=0 clock=2324441905
024.928: stats count=57 sum=41425 sumsq=172641
INFO:root:Resetting prediction variance 5623.733: freq=263579447 diff=-127378871 stddev=160000.000
INFO:root:Resetting prediction variance 5624.719: freq=262391643 diff=-200866469 stddev=160000.000
INFO:root:Resetting prediction variance 5625.704: freq=260745648 diff=-261028545 stddev=160000.000
INFO:root:Resetting prediction variance 5626.688: freq=258833500 diff=-311386402 stddev=160000.000
INFO:root:Resetting prediction variance 5627.672: freq=256767244 diff=-354395684 stddev=160000.000
INFO:root:Resetting prediction variance 5628.656: freq=254614306 diff=-391500820 stddev=160000.000
029.912: stats count=56 sum=40735 sumsq=169822
INFO:root:Resetting prediction variance 5629.640: freq=252418016 diff=-423723759 stddev=160000.000
INFO:root:Resetting prediction variance 5630.624: freq=250207090 diff=-451815143 stddev=160000.000
INFO:root:Resetting prediction variance 5631.608: freq=248001015 diff=-476332880 stddev=160000.000
INFO:root:Resetting prediction variance 5632.592: freq=245813306 diff=-497726075 stddev=160000.000
INFO:root:Resetting prediction variance 5633.575: freq=243653393 diff=-516358830 stddev=160000.000
034.896: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5634.559: freq=241527869 diff=-532523874 stddev=160000.000
INFO:root:Resetting prediction variance 5635.543: freq=239441361 diff=-546480798 stddev=160000.000
INFO:root:Resetting prediction variance 5636.527: freq=237397018 diff=-558444095 stddev=160000.000
INFO:root:Resetting prediction variance 5637.511: freq=235396944 diff=-568604296 stddev=160000.000
```

**判讀**：只有 cycle=1 送達，緊接著 `009.976: starting`——這是 Klipper
韌體重開機的啟動橫幅。第二次 `get_uptime` 回 `high=0`（第一次
`high=4`）→ **判定重置**。`starting` 出現在 command 送出後約 0.53 秒
（見下方「IWDG 逾時邊界」的計算）。

### 檔 3：period_ms=300 cycles=5（sleep 20）

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=300 cycles=5\n"; sleep 20; printf "get_uptime\n"; sleep 3) | timeout 45 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
004.241: stats count=148 sum=323330 sumsq=3628342
006.449: uptime high=1 clock=4059423739
009.224: stats count=56 sum=40876 sumsq=161492
009.761: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.944: stats count=66 sum=47097 sumsq=154402
INFO:root:Resetting prediction variance 5674.380: freq=160527177 diff=-330943417 stddev=120075.505
INFO:root:Resetting prediction variance 5675.366: freq=148503001 diff=-99774238 stddev=160000.000
INFO:root:Resetting prediction variance 5676.351: freq=146253318 diff=-42576912 stddev=160000.000
INFO:root:Resetting prediction variance 5677.337: freq=145551828 diff=-13738721 stddev=160000.000
INFO:root:Resetting prediction variance 5678.323: freq=145372474 diff=4949037 stddev=160000.000
024.928: stats count=55 sum=38772 sumsq=154769
INFO:root:Resetting prediction variance 5679.308: freq=145426206 diff=18650114 stddev=160000.000
INFO:root:Resetting prediction variance 5680.294: freq=145600110 diff=29401566 stddev=160000.000
INFO:root:Resetting prediction variance 5681.279: freq=145841028 diff=38210306 stddev=160000.000
INFO:root:Resetting prediction variance 5682.265: freq=146120918 diff=45604402 stddev=160000.000
INFO:root:Resetting prediction variance 5683.250: freq=146423531 diff=51919148 stddev=160000.000
029.459: uptime high=0 clock=3127177475
INFO:root:Resetting prediction variance 5684.235: freq=146738945 diff=57367848 stddev=160000.000
029.912: stats count=58 sum=43388 sumsq=187694
INFO:root:Resetting prediction variance 5685.221: freq=147060784 diff=62101331 stddev=160000.000
INFO:root:Resetting prediction variance 5686.206: freq=147384833 diff=66225155 stddev=160000.000
INFO:root:Resetting prediction variance 5687.190: freq=147708207 diff=69794728 stddev=160000.000
INFO:root:Resetting prediction variance 5688.174: freq=148028756 diff=72912190 stddev=160000.000
INFO:root:Resetting prediction variance 5689.158: freq=148345144 diff=75625552 stddev=160000.000
034.896: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5690.142: freq=148656413 diff=77975803 stddev=160000.000
INFO:root:Resetting prediction variance 5691.126: freq=148961892 diff=80003683 stddev=160000.000
INFO:root:Resetting prediction variance 5692.110: freq=149261130 diff=81745262 stddev=160000.000
INFO:root:Resetting prediction variance 5693.094: freq=149553845 diff=83231920 stddev=160000.000
INFO:root:Resetting prediction variance 5694.078: freq=149839882 diff=84476702 stddev=160000.000
039.880: stats count=55 sum=38812 sumsq=144896
INFO:root:Resetting prediction variance 5695.061: freq=150119129 diff=85510173 stddev=160000.000
INFO:root:Resetting prediction variance 5696.045: freq=150391568 diff=86341257 stddev=160000.000
INFO:root:Resetting prediction variance 5697.029: freq=150657198 diff=86994213 stddev=160000.000
INFO:root:Resetting prediction variance 5698.013: freq=150916075 diff=87484459 stddev=160000.000
```

**判讀**：只有 cycle=1 送達。第二次 `get_uptime` 回 `high=0`（第一次
`high=1`）→ **判定重置**。

### 檔 4：period_ms=400 cycles=3（sleep 15）—— 資料不完整

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=400 cycles=3\n"; sleep 15; printf "get_uptime\n"; sleep 3) | timeout 40 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
001.749: stats count=144 sum=316841 sumsq=3581776
006.448: uptime high=2 clock=164304713
006.733: stats count=57 sum=41496 sumsq=163221
009.862: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
```

輸出到此為止，`timeout 40` 到期後管線結束，**沒有再出現任何行**：沒有
`starting` 橫幅、沒有更多 `stats`、也沒有第二個 `get_uptime` 回應。這一
檔的重置與否**無法判定**——見「無法從這組數據判定的事」。原因很可能是
我方 `sleep 15`（加上前後固定的 7+3+3 秒緩衝，總視窗 40 秒）沒有留夠時間
讓「重置 → 重開機 → host 重新握手 → 送出第二個 get_uptime 的回應」這一
整串流程走完，而不是 MCU 真的卡死；但因為視窗內沒有任何可用證據，不能
下任何結論，只能誠實記錄「不完整」。

### 檔 5：period_ms=450 cycles=3（sleep 15）

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=450 cycles=3\n"; sleep 15; printf "get_uptime\n"; sleep 3) | timeout 40 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
====================       connected       ====================
001.717: stats count=145 sum=317461 sumsq=3583273
006.441: uptime high=2 clock=3368359523
006.700: stats count=56 sum=40836 sumsq=160876
009.907: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.937: stats count=65 sum=47165 sumsq=161497
INFO:root:Resetting prediction variance 5810.481: freq=160526498 diff=360011544 stddev=120024.349
INFO:root:Resetting prediction variance 5811.467: freq=173606372 diff=108533627 stddev=160000.000
INFO:root:Resetting prediction variance 5812.452: freq=176053520 diff=46304604 stddev=160000.000
INFO:root:Resetting prediction variance 5813.438: freq=176816420 diff=14926086 stddev=160000.000
INFO:root:Resetting prediction variance 5814.423: freq=177011274 diff=-5404132 stddev=160000.000
024.450: uptime high=0 clock=2324494155
024.921: stats count=56 sum=40825 sumsq=171234
INFO:root:Resetting prediction variance 5815.409: freq=176952601 diff=-20324624 stddev=160000.000
INFO:root:Resetting prediction variance 5816.395: freq=176763080 diff=-32028899 stddev=160000.000
INFO:root:Resetting prediction variance 5817.381: freq=176500631 diff=-41604796 stddev=160000.000
INFO:root:Resetting prediction variance 5818.364: freq=176195874 diff=-49628242 stddev=160000.000
INFO:root:Resetting prediction variance 5819.348: freq=175866580 diff=-56480130 stddev=160000.000
INFO:root:Resetting prediction variance 5820.332: freq=175523492 diff=-62396518 stddev=160000.000
029.904: stats count=57 sum=41335 sumsq=171229
INFO:root:Resetting prediction variance 5821.316: freq=175173481 diff=-67538407 stddev=160000.000
INFO:root:Resetting prediction variance 5822.300: freq=174821106 diff=-72016876 stddev=160000.000
INFO:root:Resetting prediction variance 5823.284: freq=174469501 diff=-75929179 stddev=160000.000
INFO:root:Resetting prediction variance 5824.268: freq=174120805 diff=-79340655 stddev=160000.000
INFO:root:Resetting prediction variance 5825.252: freq=173776533 diff=-82316825 stddev=160000.000
034.888: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5826.236: freq=173437717 diff=-84899473 stddev=160000.000
INFO:root:Resetting prediction variance 5827.219: freq=173105101 diff=-87125670 stddev=160000.000
INFO:root:Resetting prediction variance 5828.203: freq=172779202 diff=-89034763 stddev=160000.000
INFO:root:Resetting prediction variance 5829.187: freq=172460354 diff=-90657073 stddev=160000.000
```

**判讀**：只有 cycle=1 送達。第二次 `get_uptime` 回 `high=0`（第一次
`high=2`）→ **判定重置**。

### 檔 6：period_ms=500 cycles=3（sleep 15，對照組）

```
$ ssh arduino@192.168.40.120 '(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=500 cycles=3\n"; sleep 15; printf "get_uptime\n"; sleep 3) | timeout 40 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1'
[exit 124]
  ...(HELP 文字略)...
==================== attempting to connect ====================
INFO:root:Starting serial connect
Loaded 131 commands (v0.13.0-470-gb48410cc2 / gcc: (15:14.2.rel1-1) 14.2.1 20241119 binutils: (2.44-3+23+b2) 2.44)
MCU config: ADC_MAX=4095 CLOCK_FREQ=160000000 MCU=stm32u585xx RECEIVE_WINDOW=192 RESERVE_PINS_serial=PG8,PG7 SERIAL_BAUD=115200 STATS_SUMSQ_BASE=256 STEPPER_OPTIMIZED_EDGE=20 STEPPER_STEP_BOTH_EDGE=1
WARNING:root:got {'count': 144, 'sum': 312907, 'sumsq': 3542858, '#name': 'stats', '#sent_time': 5855.214118703333, '#receive_time': 5855.267373442667}
====================       connected       ====================
006.182: stats count=57 sum=42729 sumsq=185586
006.437: uptime high=2 clock=1050916793
009.953: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.950: stats count=63 sum=43021 sumsq=139661
019.933: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5874.057: freq=160527296 diff=2677536274 stddev=120038.614
INFO:root:Resetting prediction variance 5875.041: freq=257811718 diff=807304323 stddev=160000.000
INFO:root:Resetting prediction variance 5876.027: freq=276014992 diff=344471070 stddev=160000.000
INFO:root:Resetting prediction variance 5877.012: freq=281690674 diff=111090725 stddev=160000.000
INFO:root:Resetting prediction variance 5877.998: freq=283141006 diff=-40156870 stddev=160000.000
024.448: uptime high=0 clock=2324642195
INFO:root:Resetting prediction variance 5878.983: freq=282704992 diff=-150979099 stddev=160000.000
024.918: stats count=58 sum=43388 sumsq=187694
INFO:root:Resetting prediction variance 5879.968: freq=281297090 diff=-238106027 stddev=160000.000
INFO:root:Resetting prediction variance 5880.954: freq=279345888 diff=-309422011 stddev=160000.000
INFO:root:Resetting prediction variance 5881.937: freq=277079182 diff=-369122007 stddev=160000.000
INFO:root:Resetting prediction variance 5882.921: freq=274629751 diff=-420118883 stddev=160000.000
INFO:root:Resetting prediction variance 5883.905: freq=272077480 diff=-464117032 stddev=160000.000
029.900: stats count=55 sum=38495 sumsq=141297
INFO:root:Resetting prediction variance 5884.889: freq=269473750 diff=-502331404 stddev=160000.000
INFO:root:Resetting prediction variance 5885.873: freq=266852588 diff=-535638874 stddev=160000.000
INFO:root:Resetting prediction variance 5886.857: freq=264237156 diff=-564704065 stddev=160000.000
INFO:root:Resetting prediction variance 5887.841: freq=261643504 diff=-590062682 stddev=160000.000
INFO:root:Resetting prediction variance 5888.825: freq=259082821 diff=-612153215 stddev=160000.000
034.884: stats count=55 sum=38772 sumsq=144280
INFO:root:Resetting prediction variance 5889.809: freq=256562904 diff=-631331368 stddev=160000.000
INFO:root:Resetting prediction variance 5890.793: freq=254089176 diff=-647868215 stddev=160000.000
INFO:root:Resetting prediction variance 5891.776: freq=251665484 diff=-662047137 stddev=160000.000
INFO:root:Resetting prediction variance 5892.760: freq=249294295 diff=-674087249 stddev=160000.000
```

**判讀**：只有 cycle=1 送達。第二次 `get_uptime` 回 `high=0`（第一次
`high=2`）→ **判定重置**，與上一輪已知結果一致，對照組符合預期。

---

## 結果表

`完成輪數` 指實際收到的 `stop2_status` 筆數（相對於指令要求的 `cycles`）。
`sws`/`pll1rdy`/`restore_timeout`/`lptim_timeout`/`entry_fail` 取**最後一筆
成功送達**的 `stop2_status`。`mcu_sec`/`slept_sec` 在判定「重置」的列一律
標記為「N/A（計數器不連續）」——重置後 `high`/`clock` 從新開機狀態重新
計數，兩次讀數相減沒有物理意義；`wall_sec` 仍照公式取 console 兩行
`uptime` 回應的時間戳差，僅供參照重置前後總耗時，不代表睡眠時長。

| period_ms | cycles | 完成輪數 | mcu_sec | wall_sec | slept_sec | expect_sec | 重置? | sws | pll1rdy | restore_timeout | lptim_timeout | entry_fail |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 100 | 10 | 2/10 | N/A | 18.011 | N/A | 1.00 | 是 | 3 | 1 | 0 | 0 | 0 |
| 200 | 5  | 1/5  | N/A | 18.010 | N/A | 1.00 | 是 | 3 | 1 | 0 | 0 | 0 |
| 300 | 5  | 1/5  | N/A | 23.010 | N/A | 1.50 | 是 | 3 | 1 | 0 | 0 | 0 |
| 400 | 3  | 1/3  | 無法判定 | 無法判定（無第二筆 uptime） | 無法判定 | 1.20 | **無法判定** | 3 | 1 | 0 | 0 | 0 |
| 450 | 3  | 1/3  | N/A | 18.009 | N/A | 1.35 | 是 | 3 | 1 | 0 | 0 | 0 |
| 500 | 3  | 1/3  | N/A | 18.011 | N/A | 1.50 | 是 | 3 | 1 | 0 | 0 | 0 |

`wall_sec` 欄位在「是」的列偏大（17-23 秒），是因為它涵蓋了「重置 → 重
開機 → host 重新握手完成 → 送出第二個 get_uptime 的回應」整段時間，不是
單純的睡眠時長；不能拿來跟 `expect_sec` 比對（這正是本輪要強調的方法論
限制，見下一節）。

---

## IWDG 逾時邊界

**這組數據無法給出「period_ms 低於某值就不會重置」這種邊界**，因為
**六個檔全部重置**（第 4 檔不確定，其餘 5 檔確定），包括最小的
`period_ms=100`。原因可以直接從已確認的事實推出，不是新的推測：
`watchdog_reset()` 是 `DECL_TASK`，只有排程器的 task loop 跑到它才會餵狗；
而 `command_test_stop2()` 從收到指令到回傳之間，**整個多輪迴圈都在同一次
command handler 呼叫裡跑完**，中途完全不會把控制權交還給排程器——也就是
說，能不能餵到狗，取決於「這次 command 呼叫從開始到現在的累積耗時」，
跟單一 `period_ms` 有沒有安全都無關；`period_ms` 只影響「累積耗時衝到
IWDG 門檻之前，能塞進幾輪」。

檔 1（`period_ms=100`）完成了 2 輪才被重置，其餘檔只完成 1 輪，這與上述
推論一致：`period_ms` 愈小，單輪耗時愈短，累積到門檻前能跑的輪數愈多。

### 實際量到的逾時區間

檔 2（`period_ms=200`）是唯一同時捕到「最後一次成功 `stop2_status`」與
「重開機 `starting` 橫幅」的檔，兩者都在同一個連線階段內、用同一顆
console 時鐘量測，是這批數據裡最乾淨的邊界證據：

- `test_stop2` 指令送出時間（由 `get_uptime` 回應時間 `006.448` 推算，
  管線固定等 3 秒後送出）≈ `009.448`（console 時鐘）
- cycle=1 完成於 `009.658` → 指令送出後 `0.210s`——這只代表「重置發生在
  這之後」，是很鬆的下界，因為第 2 輪可能已經開始執行了一段時間才被咬。
- `starting` 橫幅出現於 `009.976` → 指令送出後 `0.528s`——重開機一定已
  經發生在這之前（開機到印出橫幅只需要極短時間，可視為上界）。

即：**這次重置發生在指令送出後 (0.21s, 0.53s] 的區間內**，下界很鬆、上
界較緊。其餘檔（1、3、5、6）都只提供更鬆散的邊界（沒有 `starting` 橫幅
可用），皆與這個區間不衝突，故整批數據能給出的**最緊的上界是 0.528s**，
下界則沒有一筆數據夠緊，只能說「大於單輪完成時間」，本身對推算 LSI 沒有
實用意義。

### 反推 LSI 頻率

套用公式 `f_LSI = 16384 / T_timeout`：

- 用最緊的上界 `T ≈ 0.528s` → `f_LSI ≈ 16384 / 0.528 ≈ 31.0 kHz`
- 下界因為數據不夠緊（見上段），代入會得到 `f_LSI` 的一個不合理的上界
  （例如用 `T=0.21s` 會反推出 `f_LSI ≈ 78 kHz`，遠超 STM32 LSI 規格範圍，
  代表這個下界本身不可信，不是 LSI 真的有這麼快）——**誠實地說，這組
  數據只能給出 `f_LSI` 的一個下界（≈31.0 kHz），給不出可信的上界**。

`31.0 kHz` 與韌體註解假設的「標稱 32kHz」相符（`16384/32000=0.512s`，
落在文件既有的「410-512ms」上緣附近），與這批數據互相印證，但仍只是
**單一事件、單一次量測**，不是統計意義上的頻率標定。

---

## Stop2 進出是否成立

**成立，但只在「單一 command 呼叫的前 1-2 輪」這個範圍內被證實**，且
純靠遙測（不靠 LED）：六個檔裡，每一檔至少有一筆 `stop2_status` 回傳
`sws=3`（SWS 讀回確實是 PLL1R，不是卡在 MSIS/HSI16）、`pll1rdy=1`、
`restore_timeout=0`、`lptim_timeout=0`、`entry_fail=0`。這代表對這些完成
的輪次而言：

1. `PWR_CR1`/`SCB_SCR` 的寫入讀回驗證通過（沒有進入「entry_fail」分支）；
2. `wfi` 確實返回過（否則不會執行到 `stm32u5_sysclk_restore()` 之後的
   `stop2_capture_status()`）；
3. `stm32u5_sysclk_bringup()` 的四段忙等都沒有逾時（`restore_timeout=0`）；
4. SYSCLK 讀回確實是 PLL1R，不是本輪修復問題 1 針對的那種「醒了但沒鎖
   回 160MHz」的靜默失敗；
5. LPTIM1 的 DIER/ARR 寫入時機修正（把 DIER 搬到 ENABLE 之後）沒有造成
   逾時（`lptim_timeout=0`）——這回答了上一輪 review 文件裡列為「無法
   在沒有硬體情況下確認」的第 1 項風險，這批數據**支持**該修正是對的。

但無法回答「slept_sec ≈ expect_sec 嗎」這個原本設計要驗證的問題，因為
**六檔全部在跑完全部 `cycles` 之前就被 IWDG 重置**，導致每一檔用來算
`slept_sec` 的第二筆 `get_uptime` 都落在計數器不連續（重置後）的一側，
公式定義下無法計算出有意義的睡眠秒數。換句話說：**這批數據證明了
「Stop2 進出的機制本身是對的」，但沒有辦法（也不是它的設計目的）證明
「連續多輪 Stop2 搭配目前的 command handler 寫法，可以在不被 IWDG 咬到
的情況下穩定運作」**——後者需要修改 command handler 讓它在多輪之間把
控制權還給排程器（例如拆成多次呼叫，或在迴圈中呼叫餵狗函式），但依照
本輪任務範圍「不要修改任何原始碼」，這裡不做也不建議在此文件之外自行
更動。

---

## 無法從這組數據判定的事

1. **IWDG 真正跳脫的精確時刻**：只有檔 2 給出區間 `(0.21s, 0.528s]`，
   其餘檔的區間更寬，不足以進一步收斂。
2. **`f_LSI` 的上界**：現有數據只能給下界（≈31.0 kHz），給不出上界；
   需要更多次重複量測、且需要一個比「cycle 完成」更接近重置瞬間的訊號
   （例如更密集的心跳訊息）才能收斂下界，讓上界也有意義。
3. **檔 4（`period_ms=400`）到底有沒有重置**：視窗內沒有任何後續證據
   （沒有 `starting`、沒有第二筆 `get_uptime`、沒有新的 `stats`），無法
   判定是「重置但我方視窗太短沒等到重開機」還是「MCU 真的完全卡死、
   IWDG 也沒有把它救回來」。兩者需要用更長的觀測視窗重新量測這一檔才能
   分辨，本輪没有做（因為規格書指定的 `sleep` 值就是 15 秒，我沒有自行
   加長）。
4. **是否存在真的不會被重置的 `period_ms`/`cycles` 組合**：六檔全部在
   規格指定的 `cycles` 下重置，本輪掃描範圍內沒有任何一組「乾淨完成
   全部 cycles」的對照數據，因此也無法回答「多小的 period_ms、多少的
   cycles 才能在不修改 command handler 的前提下安全跑完」。
5. **累積耗時裡各步驟各佔多少**：無法從現有 `sendf` 欄位拆解出「LPTIM
   設定/OK 輪詢」「Stop2 睡眠本身」「`stm32u5_sysclk_restore()`」
   「`sendf` 傳輸」「`busy_delay`」個別耗時多少，只能看到「輪與輪之間
   的總間隔」。
6. **LSI 頻率本身的穩定性**：只有一次事件可用於反推，無法評估溫度、
   多次重置之間 LSI 頻率是否漂移或有統計分佈。
7. **這個 IWDG 重置行為是否會影響其他仍在 board 上運作的功能**（例如
   config 是否需要重新下發、上層 Klipper 對 MCU restart 的處理是否正常）
   ——本輪只確認了 `systemctl start klipper` 之後 `state=ready`，沒有進一
   步驗證其餘子系統。

---

## 收尾

```
$ ssh arduino@192.168.40.120 'sudo systemctl start klipper && sleep 15 && curl -s http://localhost:7125/printer/info; echo'
{"result":{"state":"ready","state_message":"Printer is ready","hostname":"hunter","klipper_path":"/home/arduino/klipper","python_path":"/home/arduino/klippy-env/bin/python","process_id":4522,"user_id":1000,"group_id":1001,"log_file":"/home/arduino/printer_data/logs/klippy.log","config_file":"/home/arduino/printer_data/config/printer.cfg","software_version":"v0.13.0-470-gb48410cc2","cpu_info":"4 core ?"}}
```

`state: "ready"` 確認。

---
---

# cycles=1 邊界掃描（第二輪，修正版）

延續 `98c213cb`。上一節（以上）的六組測試 `cycles×period_ms` 全部
≥1000ms，必然超過 IWDG 預算，测不到「單次 Stop2 睡眠上限」。這一節全部
改用 `cycles=1`，把兩個限制分開量：

- **限制 A（迴圈累積預算）**：N 次睡眠總和受 IWDG 約束——上一節已經（意
  外地）證明過存在，這節用對照實驗更精確地重新驗證一次。
- **限制 B（單次睡眠上限）**：一次 Stop2 不能超過 IWDG 逾時——這節要量
  的重點。

方法與判讀規則完全沿用上一節：只看 `get_uptime` 的 `high`/`clock` 與
`stop2_status` 的 sendf 回報，不看 LED；重置判定一律用 `mcu_ticks` 正負，
不用 `pwr_cr1`/`scb_scr`。

## 前置

執行第一次掃描時忘記先停 `klipper.service`，10 組全部因為
`Could not exclusively lock port /dev/ttyHS1` 失敗（`console.py` 拿不到
獨占鎖）。這不是新的失敗模式，是漏做上一節已經寫明的前置步驟，補做後
即解決，不算需要停下來回報的情況：

```
$ ssh arduino@192.168.40.120 'sudo systemctl stop klipper; pgrep -x openocd || echo NO_OPENOCD'
NO_OPENOCD
```

## 主掃描原始輸出（逐字，`cycles=1`，每檔 `sleep 20`）

每檔指令：
```
(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 period_ms=<P> cycles=1\n"; sleep 20; printf "get_uptime\n"; sleep 3) | timeout 45 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 115200 /dev/ttyHS1
```

HELP 文字與 MCU config 橫幅每檔重複，以下只列連線後的內容。

### period_ms=100

```
====================       connected       ====================
005.043: stats count=144 sum=309644 sumsq=3476502
006.444: uptime high=230 clock=382304425
009.551: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
010.126: stats count=57 sum=30507969 sumsq=4294967295
INFO:root:Resetting prediction variance 12023.351: freq=160538747 diff=-15999264 stddev=120128.608
INFO:root:Resetting prediction variance 12024.337: freq=159994367 diff=-10675827 stddev=160000.000
INFO:root:Resetting prediction variance 12025.322: freq=159688217 diff=-6877967 stddev=160000.000
INFO:root:Resetting prediction variance 12026.308: freq=159519295 diff=-4085879 stddev=160000.000
INFO:root:Resetting prediction variance 12027.293: freq=159432182 diff=-1992834 stddev=160000.000
015.109: stats count=56 sum=40702 sumsq=169073
020.092: stats count=55 sum=38730 sumsq=143789
025.075: stats count=56 sum=39330 sumsq=145196
029.454: uptime high=230 clock=4060540020
030.058: stats count=56 sum=40813 sumsq=160738
035.041: stats count=55 sum=38770 sumsq=144405
040.023: stats count=56 sum=39350 sumsq=145428
```

`high` 兩次都是 230（沒有跳到 0）→ **未重置**。`010.126` 那行 `sumsq`
飽和成 `4294967295`（`0xFFFFFFFF`）——這是 Klipper 自身的排程抖動統計
出現了一次極端離群值，佐證這段時間排程器確實被 `stop2_once()` 卡住過
一次，跟「進了 Stop2」的結論一致，不是異常。

### period_ms=200

```
====================       connected       ====================
004.367: stats count=149 sum=323958 sumsq=3629924
006.427: uptime high=231 clock=4193214694
009.350: stats count=56 sum=40949 sumsq=162496
009.637: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.532: stats count=56 sum=30895874 sumsq=4294967295
019.514: stats count=55 sum=38697 sumsq=143413
024.497: stats count=55 sum=38772 sumsq=144280
029.438: uptime high=232 clock=3560817114
029.479: stats count=57 sum=41455 sumsq=162636
034.461: stats count=55 sum=38792 sumsq=144588
039.445: stats count=55 sum=38812 sumsq=144827
```

`high` 從 231 → 232，差 1——這是 32 位元 `clock` 正常溢位一次（`clock1`
已經很接近 2^32，`clock2` 是溢位後從頭數上來的值），**不是重置**。判定
仍然要用完整 64 位元 `mcu_ticks` 的正負，不能只看 `high` 有沒有變。

### period_ms=300

```
====================       connected       ====================
003.815: stats count=149 sum=322597 sumsq=3616587
006.439: uptime high=233 clock=3693912638
008.798: stats count=56 sum=40908 sumsq=161868
009.751: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.079: stats count=55 sum=31304363 sumsq=4294967295
019.061: stats count=57 sum=41280 sumsq=169986
024.044: stats count=55 sum=38730 sumsq=143789
029.027: stats count=55 sum=38730 sumsq=143789
029.450: uptime high=234 clock=3045541305
034.009: stats count=57 sum=41413 sumsq=162145
038.992: stats count=55 sum=38750 sumsq=144021
043.975: stats count=55 sum=38812 sumsq=144897
```

`high` 233→234，同樣是溢位一次，`mcu_ticks` 為正 → **未重置**——這是
主掃描裡**最後一個成功**的檔。

### period_ms=350（第一個重置）

```
====================       connected       ====================
003.343: stats count=147 sum=321539 sumsq=3614175
006.423: uptime high=235 clock=3177071449
008.325: stats count=57 sum=41508 sumsq=163275
009.785: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.933: stats count=56 sum=37692 sumsq=129398
019.916: stats count=55 sum=38772 sumsq=144280
024.899: stats count=56 sum=39372 sumsq=156176
029.433: uptime high=0 clock=3127943115
029.881: stats count=57 sum=42788 sumsq=186287
034.864: stats count=55 sum=38772 sumsq=144280
039.847: stats count=56 sum=39412 sumsq=146303
```

注意：`cycle=1` 的 `stop2_status` **有**正常送達（`sws=3 pll1rdy=1
restore_timeout=0 lptim_timeout=0`）——`stop2_once()` 本身完整跑完、
`wfi` 有醒來、SYSCLK 有鎖回 PLL1R。但第二次 `get_uptime` 的 `high` 掉回
0 → **後來還是重置了**。也就是說，`period_ms=350` 這組不是「Stop2 序列
本身失敗」，而是「Stop2 序列成功完成，但完成後 MCU 在某個時間點被 IWDG
咬掉」。這個現象在 400/425/450/475/500/550ms 全部重現，見下方結果表。

### period_ms=400

```
====================       connected       ====================
004.232: stats count=148 sum=321967 sumsq=3614696
006.444: uptime high=1 clock=3260487879
009.213: stats count=57 sum=42564 sumsq=181373
009.859: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.939: stats count=55 sum=38104 sumsq=135778
024.922: stats count=55 sum=38772 sumsq=154769
029.455: uptime high=0 clock=3127743055
029.905: stats count=58 sum=43388 sumsq=187694
034.887: stats count=55 sum=38772 sumsq=144280
039.870: stats count=55 sum=38812 sumsq=144896
```

`high` 從 1（上一檔重置後才剛重開機不久）掉到 0 → **重置**。

### period_ms=425

```
====================       connected       ====================
001.927: stats count=144 sum=316995 sumsq=3582543
006.442: uptime high=1 clock=3629851159
006.910: stats count=57 sum=41436 sumsq=162283
009.881: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
024.919: stats count=55 sum=38104 sumsq=141273
029.453: uptime high=0 clock=3127727085
029.902: stats count=57 sum=42788 sumsq=186287
034.885: stats count=56 sum=39372 sumsq=145687
039.868: stats count=55 sum=38812 sumsq=144896
```

**重置**（`high` 1→0）。

### period_ms=450

```
====================       connected       ====================
004.297: stats count=147 sum=322750 sumsq=3627025
006.438: uptime high=1 clock=3248766269
009.280: stats count=56 sum=40836 sumsq=160876
009.904: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
009.967: starting
014.950: stats count=56 sum=38380 sumsq=138208
019.933: stats count=55 sum=38772 sumsq=144280
024.916: stats count=56 sum=39372 sumsq=156176
029.449: uptime high=0 clock=3127751555
029.899: stats count=57 sum=42788 sumsq=186287
034.881: stats count=55 sum=38772 sumsq=144280
039.864: stats count=56 sum=39412 sumsq=146303
```

**重置**，且這次抓到 `009.967: starting` 開機橫幅——`cycle=1` 在
`009.904` 成功送達之後只過了 `0.063s` 就看到重開機證據，是本輪最緊的
「已經死透」時間點，用於下方 B 邊界計算。

### period_ms=475

```
====================       connected       ====================
004.311: stats count=148 sum=323331 sumsq=3628367
006.439: uptime high=1 clock=3246632239
009.294: stats count=56 sum=40836 sumsq=160876
009.930: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.951: stats count=72 sum=49340 sumsq=151626
019.933: stats count=55 sum=38772 sumsq=144280
024.916: stats count=55 sum=38772 sumsq=154769
029.450: uptime high=0 clock=3127807935
029.899: stats count=58 sum=43388 sumsq=187694
034.881: stats count=55 sum=38772 sumsq=144280
039.864: stats count=55 sum=38812 sumsq=144896
```

**重置**。

### period_ms=500

```
====================       connected       ====================
004.217: stats count=147 sum=321407 sumsq=3613469
006.444: uptime high=1 clock=3262638339
009.199: stats count=57 sum=42799 sumsq=186538
009.960: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.938: stats count=66 sum=47097 sumsq=154402
024.923: stats count=55 sum=38772 sumsq=154769
029.455: uptime high=0 clock=3127894975
029.904: stats count=58 sum=43388 sumsq=187694
034.886: stats count=55 sum=38772 sumsq=144280
039.869: stats count=55 sum=38812 sumsq=144896
```

**重置**。`cycle=1` 於 `009.960` 成功送達——是本輪所有檔案裡「確認活著」
時間點最晚的一筆，用於下方 B 邊界計算。

### period_ms=550

```
====================       connected       ====================
004.245: stats count=147 sum=322730 sumsq=3626935
006.444: uptime high=1 clock=3257960479
009.228: stats count=57 sum=41436 sumsq=162283
009.973: starting
010.231: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
014.956: stats count=62 sum=30102008 sumsq=4294967295
019.939: stats count=55 sum=38697 sumsq=143413
029.454: uptime high=0 clock=3127537048
029.905: stats count=57 sum=42746 sumsq=185336
034.888: stats count=56 sum=39330 sumsq=145196
039.871: stats count=55 sum=38770 sumsq=144405
```

**重置**。這一檔的兩行順序反常：`starting`（`009.973`）印在
`stop2_status cycle=1`（`010.231`）**之前**。`console.py` 印出的時間戳
是由 host 端的時鐘估計模型換算，緊接著這兩行後面就出現一串
`Resetting prediction variance`，代表估計模型當時正處於重置導致的
重新鎖定期間——這個順序異常很可能是模型在不穩定期間換算出的時間戳
不可靠，而不是真的「先開機後收到舊指令的回應」。老實說：**這個順序矛盾
我沒有辦法從現有數據解釋清楚**，只確認「重置有發生」這件事本身沒有疑義
（`high` 1→0）。

## 主掃描結果表

`handler_dur` = `stop2_status cycle=1` 時間戳 − 指令送出時間（用第一個
`get_uptime` 回應時間 + 3.000s 的管線固定延遲估算，跟第一輪掃描的估算
方式相同）。`slept_sec` 只在「未重置」列有意義；重置列的 `mcu_ticks` 為
負，公式不適用，標 N/A。

| period_ms | 完成輪數 | handler_dur (s) | mcu_ticks | wall_sec | slept_sec | 重置? |
|---|---|---|---|---|---|---|
| 100 | 1/1 | 0.107 | 3,678,235,595 | 23.010 | 0.021 | 否 |
| 200 | 1/1 | 0.210 | 3,662,569,716 | 23.011 | 0.120 | 否 |
| 300 | 1/1 | 0.312 | 3,646,595,963 | 23.011 | 0.220 | **否（最後一個成功）** |
| 350 | 1/1（送達後才重置） | 0.362 | 負值 | 23.010 | N/A | **是（第一個重置）** |
| 400 | 1/1（送達後才重置） | 0.415 | 負值 | 23.011 | N/A | 是 |
| 425 | 1/1（送達後才重置） | 0.439 | 負值 | 23.011 | N/A | 是 |
| 450 | 1/1（送達後才重置） | 0.466 | 負值 | 23.011 | N/A | 是（另有 starting @ +0.529s） |
| 475 | 1/1（送達後才重置） | 0.491 | 負值 | 23.011 | N/A | 是 |
| 500 | 1/1（送達後才重置） | 0.516 | 負值 | 23.011 | N/A | 是 |
| 550 | 1/1（順序異常，見上） | — | 負值 | 23.010 | N/A | 是 |

`period_ms=100/200/300` 的 `slept_sec` 明顯小於 `period_ms/1000`（例如
300ms 只量到 220ms），差額在三檔之間幾乎是同一個常數（約 79-80ms）。這
不是 Stop2 睡眠時間不準，而是換算 `mcu_sec` 時假設 SYSCLK 恰好等於
160,000,000 Hz——但 SYSCLK 是由 HSI16（內部 RC 振盪器，出廠只做粗調，
無晶振等級精度）乘頻 10 倍得到，HSI16 本身的絕對誤差會等比例乘上去。用
三檔數據反推 `實際頻率/160MHz` 的比值，三次算出來的結果幾乎一致
（1.003448 / 1.003511 / 1.003531，平均 ≈1.003497，換算實際 SYSCLK
≈160.56MHz，比標稱值快約 0.35%）；用這個修正後的頻率重算，三檔的
`slept_sec` 分別變成 101.1ms / 199.7ms / 299.2ms——跟各自的
`period_ms` 幾乎完全吻合（誤差 <1.1ms）。這**強烈支持**：只要 Stop2
沒被重置，睡眠時長本身跟下達的 `period_ms` 高度一致；同時也說明用「原始
公式（除以標稱 160MHz）」直接讀 `slept_sec` 在只有幾百毫秒訊號、卻疊在
23 秒 wall_sec 之上時，系統性偏差（HSI16 誤差）的量級跟訊號本身相當，
必須先扣掉這個偏差才能看出真正的睡眠時長。這個修正只適用於 SYSCLK/HSI16
（Step 0-4 用的振盪器），跟下面 IWDG/LSI 的推算是完全不同的振盪器，
兩者不要混為一談。

## 控制實驗：驗證限制 A 獨立存在（`P_max=300` → `period_ms=150`）

```
$ ssh ... test_stop2 period_ms=150 cycles=1   （sleep 20）
```
```
====================       connected       ====================
002.947: stats count=147 sum=320125 sumsq=3600029
006.446: uptime high=10 clock=2412012118
007.930: stats count=56 sum=40878 sumsq=161384
009.605: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
013.063: stats count=55 sum=30686777 sumsq=4294967295
018.046: stats count=56 sum=40680 sumsq=168441
023.029: stats count=55 sum=38793 sumsq=144600
028.013: stats count=56 sum=39393 sumsq=145938
029.457: uptime high=11 clock=1787113691
032.995: stats count=56 sum=40826 sumsq=160757
037.979: stats count=55 sum=38773 sumsq=144292
042.962: stats count=56 sum=39373 sumsq=145699
```
`high` 10→11，`clock1=2412012118` 到 `clock2=1787113691`——單次溢位，
**未重置**。（`cycles=1, 150ms` 成功，符合預期。）

```
$ ssh ... test_stop2 period_ms=150 cycles=2   （sleep 20）
```
```
====================       connected       ====================
002.422: stats count=146 sum=319461 sumsq=3598673
006.443: uptime high=12 clock=1905976495
007.405: stats count=56 sum=40878 sumsq=161384
009.602: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
009.942: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
019.938: stats count=65 sum=47165 sumsq=161497
024.921: stats count=55 sum=38772 sumsq=154769
029.454: uptime high=0 clock=3127648065
029.904: stats count=57 sum=42788 sumsq=186287
034.887: stats count=55 sum=38772 sumsq=144280
039.870: stats count=55 sum=38812 sumsq=144896
```
`high` 12→0 → **重置**。兩輪都成功送達（`cycle=1`@009.602,
`cycle=2`@009.942），但最終仍被重置。

```
$ ssh ... test_stop2 period_ms=150 cycles=4   （sleep 20）
```
```
====================       connected       ====================
004.235: stats count=146 sum=320971 sumsq=3612865
006.444: uptime high=1 clock=3259945259
009.216: stats count=58 sum=43399 sumsq=187945
009.603: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
009.943: stop2_status cycle=2 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
```
輸出到此為止，45 秒視窗內沒有 `cycle=3`、沒有 `starting`、也沒有第二個
`get_uptime` 回應——**這一檔本身無法直接判定是否重置**（跟第一輪
`period_ms=400` 那次一樣的「視窗不夠長」情況，這次視窗已經放到 20 秒
仍然沒等到）。基於 `cycles=2` 已經在幾乎相同的時間點（`009.602`/
`009.942`，跟這裡的 `009.603`/`009.943` 幾乎一致）重置，可以**合理推論**
`cycles=4` 大機率也會重置（甚至更早），但這是推論，不是這一檔自己的
直接證據，報告裡明確分開標註。

### A/B 兩個限制的分離結論

1. **限制 A（累積效應）獨立存在，且比原先預期更早出現**：單獨
   `period_ms=150, cycles=1` 成功，但 `cycles=2`（累積睡眠僅 300ms，
   而且是兩次「本身完全成功」的 150ms Stop2 睡眠疊加）就重置了——
   `cycle=1`、`cycle=2` 的 `sws`/`pll1rdy`/`restore_timeout`/
   `lptim_timeout` 全部正常，代表 Stop2 序列本身沒有出任何錯，純粹是
   「這次 command handler 佔用 task loop 的時間太久，餵狗餵太慢」。
2. **限制 A 和限制 B 不是同一把尺**：單次 300ms（限制 B 系列最後一個
   成功）之後緊接著回傳、下一輪立刻餵狗，安然無事；但
   `2×150ms=300ms`（同樣的睡眠總量，只是拆成兩段、中間多了一次
   LPTIM 重新武裝、一次 `busy_delay`、一次額外的 `sendf`）就重置了。
   這代表**每一輪之間的固定開銷（LPTIM 設定/OK 輪詢、`busy_delay`、
   `sendf` 傳輸）本身會實質消耗掉餵狗預算**，不是只有「睡多久」在算
   總帳。
3. 觀察到一個目前無法解釋的計時異常：`cycles=2` 這檔 `cycle=1`→`cycle=2`
   的間隔量到 `0.340s`，遠高於單看 `period_ms=150`+已知開銷（≈12ms）
   估算出的 `≈0.162s`。可能原因包括：`busy_delay` 在時鐘還沒完全穩定時
   的實際耗時比預期長、LPTIM OK 輪詢在連續兩次武裝之間有額外延遲、或
   `console.py` 的時鐘估計模型在此區間本身有系統性偏差。**這點誠實列
   在下方「無法判定」小節，沒有足夠證據支持任何一種解釋**。

## 限制 B（單次睡眠上限）與 LSI 反推

用主掃描裡最緊的一組「確認活著」與「確認已死」的時間點：

- **確認活著**：`period_ms=500` 的 `cycle=1` 於指令送出後 `0.516s`
  成功送達完整的 `stop2_status`（`sws=3, restore_timeout=0`）——這證明
  IWDG 在 `0.516s` 這個時間點**還沒有**跳脫，否則這行不可能送出來。
- **確認已死**：`period_ms=450` 的 `starting` 開機橫幅於指令送出後
  `0.529s` 出現——這證明 IWDG 在 `0.529s` 這個時間點**已經**跳脫並且
  重開機完成。

即真正的 IWDG 逾時 `T_timeout` 落在區間 **`(0.516s, 0.529s]`**，比第一
輪掃描給出的鬆散區間 `(0.21s, 0.53s]` 收斂了非常多。

按照公式 `f_LSI = 16384 / T`：

```
f_LSI(0.529s) = 16384 / 0.529 ≈ 30,972 Hz
f_LSI(0.516s) = 16384 / 0.516 ≈ 31,752 Hz
```

即 **`f_LSI ∈ [30.97, 31.75] kHz`**（上下界都給，不報單一數字）。

**系統性偏差說明（务必一起看，不要只看區間數字）**：這裡量到的
`handler_dur`（指令送出到 `cycle=1` 送達，或到 `starting` 出現）只涵蓋
「這次 command handler 自己跑了多久」，不包含「上一次 `watchdog_reset()`
真正執行，到這次 command 開始被處理」之間可能存在的空檔。也就是說：

```
真正的「距上次餵狗經過的時間」 = (上次餵狗到指令開始處理的空檔) + handler_dur
```

只要那個空檔 ≥0（一定成立，不可能是負的），我們量到的 `handler_dur`
**必然小於或等於**真正讓 IWDG 跳脫所需的時間，也就是這裡算出的
`T_timeout` 區間是被系統性低估的。反映到 `f_LSI = 16384/T` 上——`T` 被
低估 → `f_LSI` 被系統性**高估**。所以正確的講法是：「真正的 `f_LSI`
很可能等於或**低於** `[30.97, 31.75] kHz`」，不能反過來說「這個數字證明
LSI 比標稱 32kHz 慢」——我們連 LSI 是否比 32kHz 快或慢都無法從這個方向
的偏差本身下結論，只知道我們算出來的區間本身是一個偏高的估計，真值只會
更低，不會更高。

## 無法從這組數據判定的事（本輪新增）

1. **`cycles=2, period_ms=150` 裡 `cycle=1`→`cycle=2` 間隔異常拉長
   （0.340s 對比預期 ≈0.162s）的真正原因**：可能是 `busy_delay` 實耗
   時間、LPTIM 連續重新武裝的額外延遲、或 host 端時鐘估計模型的偏差，
   數據不足以判斷是哪一種，也不排除是三者疊加。
2. **`period_ms=550` 那檔 `starting` 印在 `cycle=1` 之前的順序矛盾**：
   懷疑是 host 端時鐘估計模型在重置後重新鎖定期間換算出的時間戳不可靠，
   但沒有直接證據，無法排除是別的原因（例如真正的位元組到達順序就是
   反的，若真是如此代表對「靠印出時間戳判斷事件先後」這個方法本身的
   可信度要打折扣，但這一點本身也無法從現有數據證實或證偽）。
3. **`cycles=4, period_ms=150` 本身是否真的重置**：45 秒視窗內沒有任何
   直接證據（沒有 `cycle=3`、沒有 `starting`、沒有第二個 `get_uptime`）。
   「大機率會重置」是根據 `cycles=2` 幾乎相同時間點已重置所做的推論，
   不是這一檔自己的觀測結果。
4. **`T_timeout` 的精確值**：只收斂到 `(0.516s, 0.529s]` 這個區間，
   且如上所述這個區間本身有系統性低估的偏差，無法給出比這更精確、且
   無偏的單一數字。
5. **`period_ms=350` 之後、`starting`（或下一筆 `get_uptime`）出現之前
   的確切重置時刻**：這一檔沒有捕到 `starting` 橫幅，只知道重置發生在
   `cycle=1` 送達（`+0.362s`）之後、下一個 `get_uptime` 回應
   （`+22.6s` 附近，含重開機與 host 重新握手）之前，區間比
   `period_ms=450` 的寬得多。
6. **限制 A 的確切「每輪固定開銷」數字**：從 `period_ms=300`（單輪
   `handler_dur=0.312s`，其中 `sleep=0.300s`，推得開銷≈12ms）推算的
   「單輪開銷」跟 `cycles=2` 觀察到的輪間間隔（0.340s，遠高於
   `0.150+0.012=0.162s`）對不上，代表「每輪固定開銷是常數」這個簡化
   假設本身可能不成立，但數據不足以建立更精確的模型。
7. **HSI16／SYSCLK 的 0.35% 誤差是否穩定**：只在同一次連續測試的三個
   資料點上觀察到高度一致的比值，沒有跨電源重啟、跨溫度的重複量測，
   不能排除這個誤差本身會隨時間或溫度漂移。

## 收尾（第二輪）

```
$ ssh arduino@192.168.40.120 'sudo systemctl start klipper && sleep 15 && curl -s http://localhost:7125/printer/info; echo'
{"result":{"state":"ready","state_message":"Printer is ready","hostname":"hunter","klipper_path":"/home/arduino/klipper","python_path":"/home/arduino/klippy-env/bin/python","process_id":6843,"user_id":1000,"group_id":1001,"log_file":"/home/arduino/printer_data/logs/klippy.log","config_file":"/home/arduino/printer_data/config/printer.cfg","software_version":"v0.13.0-470-gb48410cc2","cpu_info":"4 core ?"}}
```

`state: "ready"` 確認。
