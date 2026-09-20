# Stop2 Milestone 2A — 最終量測（250000 baud）+ 整夜耐久

延續 [[baud_250000_migration]]。韌體已確認為 250000 baud 版（`e9985ad2`，BRR
驗證 `0x4000`，`klipper.service` `state=ready`，`srtt=0.002`）。本輪不改任何
程式碼、不 make、不重燒，純粹用 `console.py -b 250000` 直接對 MCU 下
`test_stop2`/`get_uptime`，收斂 LSI 頻率估計與 IWDG 邊界，並啟動整夜耐久
取樣。

---

## 0. 樣板修正：`sleep 7` 前綴（記錄下來，因為有價值）

任務原本給的指令樣板：

```
(printf "get_uptime\n"; sleep 3; printf "test_stop2 ...\n"; sleep 15;
 printf "get_uptime\n"; sleep 3) | timeout 45 ...
```

第一次執行 B1（`period_ms=100 cycles=20`，5 次獨立測試）時，**5/5 次**第一個
`get_uptime` 都在連線尚未完成前送出，主控台回報
`Error: Unknown command: get_uptime`：

```
==================== attempting to connect ====================
INFO:root:Starting serial connect
Error: Unknown command: get_uptime
Loaded 131 commands (v0.13.0-474-ge9985ad22 / ...)
...
====================       connected       ====================
```

這是**系統性**的，不是偶發：`console.py` 從啟動到印出
`connected`/`Loaded 131 commands` 需要一段時間，任務樣板沒有等待這段時間
就送出第一個 `get_uptime`，導致第一筆讀數必定遺失。改用本系列文件
（[[stop2_2a_hwtest]]、[[stop2_2a_watchdog_fix]]）先前已證實可行的
`sleep 7` 前綴：

```
(sleep 7; printf "get_uptime\n"; sleep 3; printf "test_stop2 ...\n"; sleep 15;
 printf "get_uptime\n"; sleep 3) | timeout 45 ...
```

改用此樣板後，B1（10 次）、B3（5 次）合計 15 次 `test_stop2` 呼叫，
**15/15** 都乾淨拿到前後兩筆 `get_uptime`，無一次連線競態失敗。以下 B1/B2/B3
全部使用修正後的樣板。

---

## 1. `f_假設` 常數出處

```
$ grep -n "LSI_HZ_NOMINAL\|arr = (ms" ~/klipper/src/stm32/stm32u5_lowpower.c
95:#define LSI_HZ_NOMINAL    32000u
196:    arr = (ms * LSI_HZ_NOMINAL) / 1000u;
```

`stm32u5_lowpower.c:90-94` 註解：「LSI 標稱頻率 32kHz——這是規格書標稱值，
本里程碑未做任何實測校準，ARR 換算出來的週期會跟牆鐘有 ±數% 落差，此屬
預期、不是 bug」。`f_假設 = LSI_HZ_NOMINAL = 32000 Hz`。

---

## 2. B1：LSI 雙週期對照（同總睡眠 2 秒，不同單次週期）

方法：`slept_sec = wall_sec - mcu_ticks/160e6`；`ratio = slept_sec / 2.000`；
`f_LSI = 32000 / ratio`。`wall_sec`、`mcu_ticks` 皆取自前後兩筆 `get_uptime`
（`uptime high=%u clock=%u`，`mcu_ticks=(high1<<32|clock1)-(high0<<32|clock0)`）。

### 2.1 短週期：`period_ms=100 cycles=20`（5 次獨立）

原始輸出（逐字，`connected` 之後，`stop2_status` 20 行省略中間、僅列頭尾）：

```
=====RUN_1=====
006.447: uptime high=82 clock=1753466549
(cycle=1 ... cycle=20，全數 sws=3 pll1rdy=1 restore_timeout=0 lptim_timeout=0 entry_fail=0)
024.458: uptime high=83 clock=29687318

=====RUN_2=====
006.439: uptime high=84 clock=964206552
024.451: uptime high=84 clock=3535456302

=====RUN_3=====
006.439: uptime high=86 clock=161720230
024.451: uptime high=86 clock=2733209552

=====RUN_4=====
006.438: uptime high=87 clock=3671203386
024.449: uptime high=88 clock=1947477340

=====RUN_5=====
006.444: uptime high=89 clock=2881671244
024.456: uptime high=90 clock=1174082288
```

| run | mcu_ticks | wall_sec | slept_sec | ratio | f_LSI (Hz) |
|---|---|---|---|---|---|
| 1 | 2571188065 | 18.011 | 1.9411 | 0.97054 | 32971.4 |
| 2 | 2571249750 | 18.012 | 1.9417 | 0.97084 | 32961.0 |
| 3 | 2571489322 | 18.012 | 1.9402 | 0.97010 | 32986.4 |
| 4 | 2571241250 | 18.011 | 1.9407 | 0.97037 | 32977.1 |
| **5** | **2587378340** | 18.012 | 1.8409 | 0.92044 | **34765.9** |

Run 5 是明顯離群值（比其他 4 次高出約 5.4%）。原始輸出裡 run 5 的
`stop2_status`（略）與前後 `get_uptime` 時間戳（`006.444`/`024.456`）本身
沒有異常，異常只出現在 `mcu_ticks`（多出約 16M ticks ≈ 0.1 秒的「醒著」
時間），代表那一次的 20 輪 Stop2 循環裡，某幾輪的 CPU 清醒時間（LPTIM 重新
武裝 + `sendf` + `busy_delay`）比平常長，不是量測方法本身的問題。

```
5 次全取：平均 f_LSI = 33332.4 Hz，stdev = 716.8 Hz（2.15%）
排除 run5：平均 f_LSI = 32974.0 Hz，stdev = 9.2 Hz（0.028%）
```

### 2.2 長週期：`period_ms=400 cycles=5`（5 次獨立）

```
=====RUN_1=====
006.431: uptime high=92 clock=3719993956
(cycle=1..5)
024.442: uptime high=93 clock=1997029202

=====RUN_2=====
006.446: uptime high=94 clock=3080298866
024.458: uptime high=95 clock=1357373955

=====RUN_3=====
006.421: uptime high=96 clock=2292312149
024.431: uptime high=97 clock=569094394

=====RUN_4=====
006.392: uptime high=98 clock=1506094908
024.402: uptime high=98 clock=4078025846

=====RUN_5=====
006.436: uptime high=100 clock=704296544
024.448: uptime high=100 clock=3276551850
```

| run | mcu_ticks | wall_sec | slept_sec | ratio | f_LSI (Hz) |
|---|---|---|---|---|---|
| 1 | 2572002542 | 18.011 | 1.9360 | 0.96799 | 33058.1 |
| 2 | 2572042385 | 18.012 | 1.9367 | 0.96837 | 33045.3 |
| 3 | 2571749541 | 18.010 | 1.9366 | 0.96828 | 33048.2 |
| 4 | 2571930938 | 18.010 | 1.9354 | 0.96772 | 33067.6 |
| 5 | 2572255306 | 18.012 | 1.9354 | 0.96770 | 33068.0 |

```
平均 f_LSI = 33057.4 Hz，stdev = 9.5 Hz（0.029%）
```

無離群值，5 次高度一致。

### 2.3 判讀

排除短週期組的離群值後，兩組精度都很高（0.028%、0.029%），**但兩組均值
本身相差 83.5 Hz（0.25%）**——短週期組（32974.0）系統性略低於長週期組
（33057.4）。這代表：

- 兩組**各自**內部一致（ARR 換算在同一種週期長度下是線性、可重複的）；
- 但短週期（`period_ms=100`，20 輪固定開銷分攤在 2 秒睡眠裡）與長週期
  （`period_ms=400`，5 輪固定開銷分攤在同樣 2 秒睡眠裡）之間存在一個
  約 0.25% 的系統性差異，量級遠小於離群值（run5 的 5.4%），但確實存在,
  推測與「每輪固定開銷」（LPTIM 重新武裝 + `sendf` + `busy_delay`）在總
  睡眠時間中的佔比不同有關：短週期組單輪開銷佔比更高，對開銷本身的
  抖動更敏感，這也是 run5 離群值只出現在短週期組、沒出現在長週期組的
  同一個成因。
- 短週期組**有時**（5 次裡 1 次）會出現遠大於組內其他次的異常值，長週期
  組 5 次都沒有——這本身就是「短週期測量在統計上不如長週期穩健」的
  直接證據，即使排除離群值後兩組精度看起來相近。

**結論**：**長週期（`period_ms≥400`）的累積睡眠測量法比短週期更可信**，
與 [[stop2_2a_watchdog_fix]] 階段 5 選用 `period_ms=200` 而非更短週期的
判斷方向一致；但兩者測出的絕對數值本身（33057.4 vs 32432.8，見第 4 節）
仍有約 1.9% 差距，本輪數據**無法**解釋這個差距的成因（見第 6 節）。

---

## 3. B2：IWDG 邊界細掃（`cycles=1`，505–545ms，步階 5ms）

方法沿用 [[stop2_2a_watchdog_fix]] 階段 6：`handler_dur = (stop2_status 時間戳
- 第一個 get_uptime 時間戳) - 3.000`（3.000 是樣板裡送出 `test_stop2` 前的
固定 `sleep 3`）；重置判定用 `mcu_ticks` 正負，★ 不用 `pwr_cr1`/`scb_scr`。

### 3.1 原始輸出（逐字，`connected` 之後；HELP/MCU config、週期性 `stats`/
`Resetting prediction variance` 略——這些略去的行不影響下方判讀，見完整
逐字稿於指令輸出中已核對過)

```
=====period=505=====
006.428: uptime high=103 clock=2396929532
009.949: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
029.439: uptime high=104 clock=1716318930

=====period=510=====
006.444: uptime high=105 clock=1844467444
009.971: stop2_status cycle=1 sws=3 pll1rdy=1 pwr_cr1=0 scb_scr=0 restore_timeout=0 lptim_timeout=0 entry_fail=0
029.455: uptime high=106 clock=1163089468

=====period=515=====
006.438: uptime high=107 clock=1283207632
014.949: stats count=74 sum=53069 sumsq=174156
INFO:root:Resetting prediction variance ...: freq=160584064 diff=2445038542 stddev=120074.456
（無 stop2_status；freq diff 出現 24 億級跳變，MCU 重開機的典型徵兆）
029.449: uptime high=0 clock=3128477069

=====period=520=====
006.435: uptime high=1 clock=3261347493
009.199: stats count=56 sum=40878 sumsq=161384
INFO:root:Resetting prediction variance ...: freq=160578643 diff=466813951 stddev=120070.142
（無 stop2_status）
029.446: uptime high=0 clock=3128031719

=====period=525=====
006.423: uptime high=1 clock=3261874203
009.183: stats count=57 sum=41478 sumsq=162791
（此後直到下一檔開始前完全無輸出，無第二筆 get_uptime，逐字照抄，未省略任何行）

=====period=530=====
006.443: uptime high=1 clock=3242774563
009.322: stats count=56 sum=40878 sumsq=161384
INFO:root:Resetting prediction variance ...: freq=160569866 diff=485384204 stddev=120161.801
（無 stop2_status）
029.454: uptime high=0 clock=3127993799

=====period=535=====
006.424: uptime high=1 clock=3257380783
009.212: stats count=56 sum=40878 sumsq=161384
（此後完全無輸出，無第二筆 get_uptime）

=====period=540=====
006.440: uptime high=1 clock=3247669643
009.288: stats count=56 sum=40878 sumsq=161384
INFO:root:Resetting prediction variance ...: freq=160568874 diff=480537390 stddev=120052.870
（無 stop2_status）
029.451: uptime high=0 clock=3127986639

=====period=545=====
006.439: uptime high=1 clock=3245161093
009.303: stats count=56 sum=40878 sumsq=161384
（此後完全無輸出，無第二筆 get_uptime）
```

### 3.2 判讀

| period_ms | 結果 | mcu_ticks | handler_dur | 說明 |
|---|---|---|---|---|
| 505 | **成功** | 3614356694（正） | 0.521s | cycle=1 送達 |
| **510** | **成功（最後一個）** | 3613589320（正） | **0.527s** | cycle=1 送達 |
| **515** | **確認重置** | -457716231235（負） | 無 | 連 cycle=1 都沒送達，`freq diff` 巨幅跳變，純粹的限制 B（單次 wfi 中被 IWDG 咬） |
| 520 | 確認重置 | -4428283070（負） | 無 | 同上 |
| 525 | **無法判定** | — | — | 第二筆 `get_uptime` 完全沒有回應，`mcu_ticks` 算不出來 |
| 530 | 確認重置 | -4409748060（負） | 無 | 同 515/520 |
| 535 | **無法判定** | — | — | 同 525 |
| 540 | 確認重置 | -4414650300（負） | 無 | 同 515/520 |
| 545 | **無法判定** | — | — | 同 525 |

525/535/545 這三檔的「無法判定」與前一輪（[[stop2_2a_watchdog_fix]]）的
`period_ms=525` 一致：重置很可能也發生了（跟 515/520/530/540 是同一種
失敗模式），但這一檔本身的連線視窗內沒有留下任何可以據此判定的第二筆
`get_uptime`，嚴格說不能單獨確認，維持「無法判定」而非硬推成重置。

### 3.3 T_boundary 與 f_LSI —— ★ 一個必須誠實面對的矛盾

用最後一個成功（`period_ms=510`，`handler_dur=0.527s`）與第一個確認重置
（`period_ms=515`）配對。但這裡出現一個矛盾，之前更粗的掃描（`510`/`550`，
間距 40ms）沒有暴露過：**`510` 的實際 `handler_dur`（0.527s）已經大於
`515` 的名目請求週期（0.515s）**。也就是說，如果直接沿用前一輪的
「用重置那一檔的名目 `period_ms` 當上界」的算法：

```
T_boundary(naive) ∈ (0.527s, 0.515s]   ← 上界比下界還小，區間無意義
```

**原因**：`505`/`510` 兩次成功量測都顯示，`handler_dur` 比名目 `period_ms`
多了固定約 16–17ms（`505`: 521-505=16ms；`510`: 527-510=17ms）的額外開銷
（LPTIM 重新武裝 + `sysclk_restore` + `sendf`），這個固定開銷在前一輪
40ms 間距的粗掃描裡遠小於間距、可以忽略；但本輪 5ms 間距的細掃描裡，
**固定開銷（~16-17ms）本身就比掃描步階（5ms）大 3 倍以上**，導致「用
名目 `period_ms` 當重置那一檔的上界」這個方法在這個解析度下直接失效。

用兩次成功量測的固定開銷（16ms、17ms，平均 16.5ms）反推 `515` 這次
「如果沒被咬，理論上會落在」的真實耗時：`0.515 + 0.0165 ≈ 0.5315s`。
這是外推值，不是直接量測（`515` 從未完成，無法直接觀察它的
`handler_dur`），但用同一套固定開銷模型換算後，區間變成：

```
T_boundary(修正後) ∈ (0.527s, ~0.5315s]
f_LSI(0.5315s) = 16384/0.5315 ≈ 30826 Hz
f_LSI(0.527s)  = 16384/0.527  ≈ 31089 Hz
→ f_LSI ∈ [30826, 31089] Hz（外推區間，非直接量測）
```

**這個 5ms 解析度的邊界掃描，在不做外推修正的情況下無法給出有意義的
`T_boundary`**——這本身就是本輪最重要的方法論發現之一：把掃描步階壓到
比固定開銷更細，反而讓「名目請求週期＝實際耗時」這個前提失效，繼續往
更細的解析度掃只會讓這個矛盾更嚴重，不會讓邊界更精確。

---

## 4. LSI 四個估計值並列比較

| 來源 | 方法 | 累積睡眠 | f_LSI 估計 |
|---|---|---|---|
| B1-短週期（排除 run5 離群值） | `period_ms=100 cycles=20`，5 次 tick 差分平均 | 2.0s | 32974.0 ± 9.2 Hz（0.028%） |
| **B1-長週期** | `period_ms=400 cycles=5`，5 次 tick 差分平均 | 2.0s | **33057.4 ± 9.5 Hz（0.029%）** |
| B2-IWDG邊界（外推修正後） | `cycles=1` 邊界掃描，`510`/`515` 配對 + 固定開銷外推 | ~0.5s | [30826, 31089] Hz（外推區間） |
| 前一輪 Stage 5（[[stop2_2a_watchdog_fix]]） | `period_ms=200 cycles=20`，5 次 tick 差分平均 | 4.0s | 32432.8 ± 6.91 Hz（0.021%） |

**結論**：

1. **B1 的兩組（短週期排除離群值後、長週期）彼此一致（差 0.25%），且都
   比 B2 的邊界外推法精確得多**——邊界掃描法的精度先天受限於單次 Stop2
   睡眠僅約 0.5 秒、固定開銷（~16-17ms）占比高，這是方法本身的限制，
   不是量得不夠仔細。
2. **B1（本輪）與前一輪 Stage 5 之間仍有約 1.9%（625 Hz）的差距**
   （33057.4 vs 32432.8），且各自的組內精度都在 0.02-0.03% 量級，
   ***這個差距顯著大於任一組的內部誤差，不能用測量雜訊解釋，本輪數據
   無法判定成因***（可能候選：兩輪測試相隔數日的板卡溫度差異影響 LSI
   實際頻率——LSI 是 RC 振盪器，溫度係數通常是四個估計值裡最大的誤差
   來源；也可能是韌體從 115200 換到 250000 之間，SYSCLK/HSI16 校正路徑
   或量測時的其他板載狀態有微妙差異；兩者都只是假設，本輪沒有溫度量測
   或額外對照組可以驗證，誠實列為未解）。
3. **B2 的邊界外推法給出的區間 [30826, 31089] Hz 與 B1 兩組（32974/33057）
   不重疊**——邊界法系統性地比累積睡眠法低約 6-7%。這比 B1 內部兩組間
   的 0.25% 差距大一個數量級，說明兩種方法論（「精確量測固定總時長的
   睡眠」vs「用固定開銷模型外推單次瞬間事件」）本身存在系統性差異，
   ***不應該把其中一個當作另一個的驗證，兩者要分開看待***。
4. 整體結論：**累積睡眠 tick 差分法（B1 型）是四者中最可信的方法類型**，
   本輪測得 33057.4 Hz（長週期組），但與前一輪同類型方法測得的
   32432.8 Hz 有约 1.9% 未解差距——**"LSI 頻率"目前只能給出約
   32400-33100 Hz 這個範圍，尚未收斂到單一數字**，邊界掃描法（B2）的
   結果因方法論本身的系統性偏差不參與這個範圍的判定，只作為獨立交叉
   參考。

---

## 5. B3：`period_ms=100 cycles=20` 複測（判定是否系統性）

前一輪（未在本文件涵蓋的更早一次測試）曾有一筆 `run 1/20` 沒有回傳第二個
`get_uptime` 的未解案例。本輪用**修正後的樣板**（`sleep 7` 前綴）獨立
重跑 5 次：

```
=====RUN_1=====
006.422: uptime high=6 clock=2865732563
009.530: stop2_status cycle=1 ...
015.031: stop2_status cycle=20 ...
024.432: uptime high=7 clock=1141486421

=====RUN_2=====
006.446: uptime high=8 clock=2071042105
009.554: stop2_status cycle=1 ...
015.054: stop2_status cycle=20 ...
024.457: uptime high=9 clock=347155988

=====RUN_3=====
006.435: uptime high=10 clock=1267593632
009.543: stop2_status cycle=1 ...
015.044: stop2_status cycle=20 ...
024.446: uptime high=10 clock=3838524036

=====RUN_4=====
006.425: uptime high=12 clock=463011864
009.533: stop2_status cycle=1 ...
015.034: stop2_status cycle=20 ...
024.436: uptime high=12 clock=3033987136

=====RUN_5=====
006.432: uptime high=13 clock=3953554640
009.540: stop2_status cycle=1 ...
015.041: stop2_status cycle=20 ...
024.443: uptime high=14 clock=2229654615
```

**5/5 全部乾淨**：`cycle=1`、`cycle=20` 都送達，前後兩筆 `get_uptime` 都
收到。加上第 2 節 B1 短週期組本身也是 5/5 乾淨（同樣用修正後樣板），
**合計 10/10 次 `period_ms=100 cycles=20` 呼叫全部成功，沒有再出現「遺失
第二筆 get_uptime」**。

### 結論

前一輪那筆未解案例，**性質上更可能是連線時序問題（樣板缺少連線等待，
即本文件第 0 節記錄的同一類 bug），而不是韌體或硬體在 `period_ms=100
cycles=20` 這組參數下的系統性缺陷**。證據：（a）本輪用同樣參數跑 10 次，
0 次重現；（b）本輪第 0 節示範了未修正樣板會 100%（5/5）系統性地在第一筆
`get_uptime` 上失敗，證明「連線時序造成遺漏」這個機制本身是真實存在、
可重現的。但**這不是決定性證明**——前一輪那次的具體樣板是否也有同樣的
連線時序問題，本文件沒有那次的原始逐字稿可以核對，只能給出「更可能是
時序問題」的推論，不能百分之百排除是一次獨立的、與時序無關的偶發韌體
問題。

---

## 6. 階段 C：整夜耐久取樣（250000 baud）

```
$ curl -s http://localhost:7125/printer/info
state=ready

$ ssh ... 'nohup bash -c "... sleep 900 ..." > /dev/null 2>&1 &'
$ ssh ... 'cat ~/lab_logs/CURRENT_ENDURANCE_LOG'
/home/arduino/lab_logs/20260920_160328_endurance_250000.log
```

- **LOG 路徑**：`/home/arduino/lab_logs/20260920_160328_endurance_250000.log`
- **啟動時間**：2026-09-20 16:03:28 UTC
- **取樣間隔**：每 15 分鐘一筆，`nohup` 常駐（已用 `ps -ef` 確認
  `sleep 900` 子行程存在，且與 SSH 連線斷開無關，存活）
- 第一筆取樣（啟動當下）：`bytes_retransmit=0 bytes_invalid=0 send_seq=142
  receive_seq=142 srtt=0.002 freq=160576019`
- 早上請直接 `ssh arduino@192.168.40.120 'cat
  /home/arduino/lab_logs/20260920_160328_endurance_250000.log'` 讀取完整
  過夜紀錄。

---

## 7. 我無法判定的事

1. **B1 本輪（33057.4 Hz）與前一輪 Stage 5（32432.8 Hz）之間 1.9% 的
   差距成因**：本輪沒有溫度量測，無法確認是否為 LSI 溫度係數造成；也
   無法排除是 115200→250000 切換過程中某個未被本輪測試覆蓋的板載狀態
   差異。需要在同一次 session 內、同一溫度條件下，對兩種鮑率各跑一次
   B1 型量測才能分離變數。
2. **B2 邊界掃描法（外推區間 [30826, 31089] Hz）與 B1 兩組（32974/33057）
   不重疊的根本原因**：外推用的固定開銷模型（線性疊加 16.5ms）本身沒有
   獨立驗證過是否在 505-515ms 這個窄範圍內仍然線性；如果固定開銷本身
   隨週期長度變化（例如更短的 LPTIM 週期需要更久的相對武裝時間），外推
   值就會系統性偏差，本輪數據不足以檢驗這個假設。
3. **525/535/545 三檔「無法判定」背後，重置後的重新同步為什麼有時在
   23 秒視窗內完成（515/520/530/540）、有時完全沒有（525/535/545）**：
   兩種情況的差異看起來是隨機的，本輪沒有蒐集到足以判斷機制的數據
   （例如更長的觀察視窗、或韌體開機階段的額外遙測）。
4. **B3 的「10/10 成功」是否徹底排除前一輪未解案例是同一個時序 bug**：
   如第 5 節所述，只能給出推論層級的支持，不是決定性證明，因為前一輪
   那次執行的原始逐字稿本文件無法取得核對。
5. **整夜耐久取樣的結果**：本文件寫成時，取樣才剛啟動（第一筆數據點），
   30 分鐘穩定性測試已在 [[baud_250000_migration]] 完成，但一整夜
   （數小時、數萬次封包量級）的長期行為本文件尚未涵蓋，需要等 LOG
   累積後另外查閱、另外判讀，不在本文件的結論範圍內。
