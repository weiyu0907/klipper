# Stop2 Milestone 2A — 硬體遙測掃描（IWDG 邊界）

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
