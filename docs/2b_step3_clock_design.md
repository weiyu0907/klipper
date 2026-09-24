# Milestone 2B step 3 — 設計盤點：Stop2 睡眠期間的 MCU 時鐘補償

唯讀查證＋計算，**沒有改任何程式碼、沒有碰硬體**。目的：把
「Stop2 睡眠期間 `DWT->CYCCNT` 凍結，醒來後要不要/怎麼補償」這個
[[2b_g_wake_latency]] 開頭就點出、[[2b_step2_stage2_result]] 第 6
節「明確排除在 step 2 範圍外」的問題,盤點清楚,產出設計依據。

---

## A. 現況盤點

### A1. `src/generic/armcm_timer.c`：`timer_read_time()`/`timer_set()`/`timer_kick()`

- **`timer_read_time()`**（行 43-47）：直接回傳 `DWT->CYCCNT`——這是
  整個 Klipper 排程系統唯一的「絕對時間」來源,不是 SysTick。
- **排程機制**：SysTick 只是「倒數計時器」，`timer_set_diff()`
  （行 34-40）每次都寫入**相對值**（`SysTick->LOAD = value`，
  `value` 是「還要多少 tick 才到期」，不是絕對時刻）。`SysTick_
  Handler()`（行 155-163）每次觸發後呼叫 `timer_dispatch_many()`
  算出下一個 timer 的 `diff`（絕對 waketime 減去當下 `timer_read_
  time()`），再用 `timer_set_diff(diff)` 重新武裝 SysTick。
- **`timer_kick()`**（行 50-56）：立刻強制觸發一次 SysTick（把
  `LOAD`/`VAL` 清零＋直接寫 `SCB->ICSR` 的 `PENDSTSET`），不是
  「排程」，是「馬上執行」。
- `timer_init()`（行 93-111）：`DWT->CYCCNT = 0`、致能
  `CYCCNTENA`，`SysTick->CTRL` 設 `CLKSOURCE|TICKINT|ENABLE`。
- `timer_reset()`/`wrap_timer`（行 72-91）：因為 SysTick 的
  `LOAD` 只有 24 bit，若 CPU 太快讓 100ms 換算成的 tick 數超過
  `0xffffff`，就加一個假的週期性 timer 把 SysTick 重新武裝切成
  小段——**這個機制跟 `DWT->CYCCNT` 本身的 32-bit 回捲無關**，只是
  SysTick 24-bit 暫存器本身裝不下太大數字的因應。

**結論：所有時間決策都繞著 `DWT->CYCCNT` 轉，SysTick 只是「鬧鐘」，
Stop2 讓 `CYCCNT` 凍結,等於讓整個排程系統的時間基準停擺,不只是
「少算一段時間」這麼單純——後面的 timer 到期判斷、`get_uptime` 的
高位字都建立在「`CYCCNT` 持續、單調遞增」這個假設上。**

### A2. `DWT->CYCCNT` 是否可寫

CMSIS 定義（`lib/cmsis-core/core_cm4.h:907`）：

```c
__IOM uint32_t CYCCNT;   /*!< Offset: 0x004 (R/W)  Cycle Count Register */
```

`__IOM`＝read-write，註解直接寫「(R/W)」——這是 ARMv7-M/ARMv8-M
架構本身定義的能力，不是 hack。軟體可以直接
`DWT->CYCCNT = new_value;`。

**寫入後對 SysTick 排程的影響**：SysTick 的硬體倒數暫存器
（`LOAD`/`VAL`）**不會**因為寫 `CYCCNT` 而自動改變——兩者是獨立的
硬體計數器。但**下一次** `SysTick_Handler()`／`timer_dispatch_
many()` 呼叫 `timer_read_time()` 時，讀到的就是新寫入的值，`diff =
next_waketime - now` 這個計算會立刻反映這個跳躍。**如果跳躍的量
夠大，會讓原本「還沒到期」的 timer 瞬間變成「早就該執行、而且已經
逾期很久」**——這正是第 C2 節要展開的風險，跟 A4 的缺口直接相關。

### A3. `get_uptime` 高位字維護與 32-bit 回捲偵測

`src/basecmd.c`：

```c
291: static uint32_t stats_send_time, stats_send_time_high;
...
294: command_get_uptime(uint32_t *args)
296:     uint32_t cur = timer_read_time();
297:     uint32_t high = stats_send_time_high + (cur < stats_send_time);
298:     sendf("uptime high=%u clock=%u", high, cur);
...
312: stats_update(uint32_t start, uint32_t cur)
...
332:     if (!timer_has_elapsed(stats_send_time, cur, timer_from_us(5000000)))
333:         return;
334:     sendf("stats count=%u sum=%u sumsq=%u", count, sum, sumsq);
335:     if (cur < stats_send_time)
336:         stats_send_time_high++;
337:     stats_send_time = cur;
```

**偵測方式**：`stats_update()`（由 `src/sched.c:266` 的
`run_tasks()` 主迴圈每輪呼叫）每次檢查「距離上次 `stats_send_time`
是否已經過了 5,000,000 µs（5 秒）」（`timer_has_elapsed()`，
`basecmd.c:304-307`，用減法＋無號比較處理 32-bit 回捲比較，跟
`timer_is_before()` 同一套手法），一旦超過就送一次 `stats` 回應，
同時檢查 `cur < stats_send_time`——如果目前的 `cur` 比上次記錄的
時刻還小，代表**中間發生過一次 32-bit 回捲**，`stats_send_time_
high++`。

**偵測週期：5 秒一次**（`stats_update` 被每輪主迴圈呼叫，但只有
每 5 秒才真正做回捲檢查跟送出 `stats`）。32-bit 回捲週期本身是
`2^32 / f_SYSCLK`——用標稱 160MHz 算是 `2^32/160e6 ≈ 26.84 秒`；
用 2A 實測頻率（見 B4）算差異微乎其微（26.7~26.8 秒量級）。
**5 秒遠小於 26.8 秒，兩次檢查之間正常情況下最多發生一次回捲**，
這個機制才能正確運作（見風險 C1 的延伸討論）。

### A4. `sched.c`：有沒有「查詢下一個 timer 到期時間」的現成 API

`SchedStatus.timer_list`（`sched.c:18-24`）是**檔案內 `static`
變數**，排序成一個鏈結串列（最早到期的在最前面），目前只有
`sched_timer_dispatch()`（行 145-179）能取得「下一個」——但這個
函式**同時會執行**目前排在最前面的 timer 的 callback（`t->func(t)`,
行 157），不是單純的「偷看一下」。

★ **沒有找到任何現成的「唯讀查詢下一個 waketime，不觸發 callback」
的 API。** `SchedStatus` 整個 struct 是檔案私有的，外部（例如未來
的 Stop2 idle 排程邏輯）目前完全無法在不觸發 dispatch 的前提下知道
「下一個 timer 還要多久才到期」。**這是 D1（醒來後補償）方案能不能
安全實作的關鍵缺口**——沒有這個查詢能力，就無法在進 Stop2 前決定
「這次可以睡多久才不會跨過下一個已排程 timer」（見 C2）。

補充：`periodic_timer`/`sentinel_timer`（`sched.c:18,45-59`）的
機制值得記錄——`periodic_timer` 保證 timer_list 永遠非空、且永遠有
一個「不太遠的將來」的 timer；`sentinel_timer.waketime = periodic_
timer.waketime + 0x80000000`，用固定偏移量（2^32 的一半）確保
`timer_is_before()` 的有號比較邏輯在鏈結串列走訪時不會因為回捲而
出錯——這個既有設計本身沒有問題，只是進一步說明這套系統對「絕對
時間單調遞增」這個假設的依賴有多深。

### A5. 既有的 idle 判定機制／其他 MCU port 的參考

`src/sched.c:238-269`（`run_tasks()`，Klipper 的主迴圈本身）：

```c
239: run_tasks(void)
...
244:     irq_poll();
245:     if (SchedStatus.tasks_status != TS_REQUESTED) {
...
250:         SchedStatus.tasks_status = SchedStatus.tasks_busy = TS_IDLE;
251:         do {
252:             irq_wait();
253:         } while (SchedStatus.tasks_status != TS_REQUESTED);
...
```

**這就是現成的 idle 判定機制**：`SchedStatus.tasks_status`／
`tasks_busy` 追蹤「有沒有任務要跑」，沒有任務時進入 `irq_wait()`
迴圈——`irq_wait()`（`src/generic/armcm_irq.c:38-46`）在非
Cortex-M7 上就是 `cpsie i; wfi; cpsid i`：**單純的 Sleep 模式
WFI**，不是 Stop2，SysTick/`CYCCNT` 全程不受影響。**這正是任何
Stop2 idle-sleep 設計理論上該接進去的既有掛鉤點**——不是要另外
發明一套 idle 偵測，是要把這裡的 WFI 換成/擴充成 Stop2 進入序列，
問題只在「換了之後 `CYCCNT` 會停」這個副作用要怎麼處理，這正是本輪
剩下所有章節在討論的。

`armcm_irq.c:41-43` 的 Cortex-M7 特殊處理很值得注意：

```c
if (__CORTEX_M == 7)
    // Cortex-m7 may disable cpu counter on wfi, so use nop
    asm volatile("cpsie i\n    nop\n    cpsid i\n" ::: "memory");
```

**這是本專案已有的先例：M7 上因為 WFI 可能停掉 CPU counter,就直接
放棄用 WFI，改用 `nop` 空轉**——代表「睡眠模式會不會停掉計時
硬體」這個問題類別，本專案不是第一次遇到，M7 的解法是「乾脆不睡」
（犧牲省電，換取計時器不中斷），跟 Stop2 這裡「要睡、但要處理計時
中斷」的取捨方向不同，但至少證明這類問題有被認知過。

**查了整個 `src/` 目錄，沒有找到任何其他 MCU port 實作過「睡眠模式
停用計時器、醒來後補償」這件事**——`rp2040`/`atsamd`/`lpc176x`/
`avr` 都只是透過共用的 `irq_wait()`（或各自更簡單的等價物）做
Sleep-mode-only 的 WFI，沒有更深的低功耗模式，也就沒有這個補償
問題。**STM32U5 的 Stop2 是這個 codebase 裡第一個需要處理「計時器
會被睡眠模式凍結」這件事的案例，沒有既有模式可以照抄。**

---

## B. 誤差預算（獨立計算）

### B1. LSE ±20ppm 在單次睡眠 0.5s 下的誤差

假設 LSE 晶體本身標稱 ±20ppm（一般規格等級的 32.768kHz 手表晶體
常見值，DS13086 沒有針對特定晶體給這個數字——這是外部 BOM 決定的
參數，不是 ST 矽片本身的規格，本輪當作既有前提使用，不重新查證
這個數字本身）：

```
Δt = t × ppm × 1e-6 = 0.5 s × 20 × 1e-6 = 1.0 × 10⁻⁵ s = 10 µs
```

**單次 0.5 秒睡眠，LSE 頻率誤差造成的絕對時間誤差量級是 ±10 µs。**

### B2. LPTIM 以 LSE 為源、prescaler=1 時的量化誤差

LSE 標稱 32.768 kHz，prescaler=1（不分頻）：

```
tick period = 1 / 32768 Hz = 30.517578125 µs ≈ 30.52 µs/tick
```

**每個 tick 的解析度是 30.52 µs**——ARR/CNT 都是整數 tick，任何
「真實經過的時間」都會被量化成最接近的整數 tick 邊界，造成最多
±1 tick（≈30.52 µs）的量化不確定性（讀到某個 CNT 值時，真實時間
落在「這個 tick 剛開始」到「下一個 tick 開始前」之間的任何一點，
不確定範圍就是一整個 tick 寬度）。

### B3. LPTIM 到期喚醒 vs UART 提前喚醒——量化誤差只在其中一種情況下存在

- **LPTIM 到期（`ARR` match）**：睡眠時長＝**軟體自己設定的 `ARR`
  值**（整數 tick，本身沒有「要去猜」的問題——這是進睡眠前主動
  決定的目標值，不是事後量測）。這種情況下**沒有量化誤差**，只有
  B1 的 LSE 頻率誤差（`ARR` 個 tick 的「名義時長」跟「真實時長」
  之間的落差，由 LSE 的頻率準確度決定）。
- **UART 提前喚醒（LPTIM 還沒 match，被 `RXFT` 中斷提前打斷）**：
  這種情況下才需要知道「到底真正睡了多久」才能算出正確的補償量，
  必須讀 `LPTIM1->CNT`（目前計數值）——這個讀數本身就是量化過的整
  數 tick，**帶有 B2 講的 ±1 tick（≈30.52µs）量化誤差**，疊加上
  這段（比完整 `ARR`更短的）睡眠時長對應的 LSE 頻率誤差（通常比
  B1 算的 10µs 更小，因為實際睡眠時間比 0.5s 短）。

**結論：兩種喚醒路徑的誤差結構不同——`ARR` 到期只有頻率誤差，UART
提前喚醒疊加了量化誤差，量化誤差（±30.52µs）比 0.5s 睡眠下的頻率
誤差（±10µs）還大，是 UART 提前喚醒這條路徑的主要誤差來源。**

### B4. `f_SYSCLK` 該用標稱 160MHz 還是 2A 實測值——差異的量級分析

**先確認 PLL1 的參考時脈來源**（不是假設，查證程式碼）：

```
src/stm32/stm32u5.c:113: U5_RCC_PLL1CFGR = (2u << 0)  /* PLL1SRC = HSI16 */
```

**PLL1（進而 `SYSCLK`／`CYCCNT` 的計數速率）的參考時脈是 HSI16，
不是任何晶體**——HSI16 是 DS13086 Table 82 已經查過的內部 RC
振盪器，出廠校準後 ±0.5%（`VDD=3.0V,TJ=30°C`），延伸溫度範圍下
±1%（`TJ=-10~100°C`，[[2b_g_wake_latency]] 第 8 節已引用）。

**獨立驗證**：從本輪（2B step 2 T1-T4）留存在 scratchpad 的
`console.py` session log 裡搜尋全部 `STATS` 輸出的 `freq=` 欄位
（host 端 `clocksync` 用線性回歸估計出來的實際頻率，不是 MCU 回報
的名義值），排除連線剛建立、估計還沒收斂的過渡值
（118987268/135782749/146814451/148012318 這幾筆——同一組 log 裡
可以看到 `freq=` 從這種偏低的值逐步爬升到穩定值，是收斂過程，
不是真實頻率），取穩定後的樣本（單位 Hz）：

```
160554411, 160556905, 160557027, 160557305, 160557400, 160560362, 160567753
平均 ≈ 160,558,738 Hz ≈ 160.5587 MHz
```

跟標稱 `CONFIG_CLOCK_FREQ=160000000`（160MHz）比：

```
(160558738 - 160000000) / 160000000 ≈ 0.3492% ≈ 3492 ppm
```

**這個 0.35%（3492ppm）的落差完全落在 HSI16 的 ±1% 容差帶內**
（甚至沒有頂到上限），跟「PLL1 參考時脈是 HSI16」這個查證結果
完全吻合，不是異常，是 HSI16 本身未校準（相對晶體而言）的正常
表現。

**對補償量的影響**：如果補償公式用標稱 160MHz 換算「LPTIM 睡了 N
個 tick，對應要補多少 `CYCCNT`」，這個換算本身就帶有約 3500ppm
（0.35%）的系統性誤差——**比 B1 的 LSE ±20ppm 大了約 175 倍**，比
B2 的量化誤差（30.52µs/tick，跟頻率無關的獨立誤差源）更是完全
不同量級的問題（3500ppm 是「每次補償都固定歪一個比例」，不是
「隨機噪聲」）。改用 host 端 `clocksync` 已經估計出來的實際頻率
（`freq=` 欄位，這個值本身已經在持續更新/收斂）換算，可以把這個
系統性誤差壓到跟 HSI16 瞬時穩定度（遠小於 ±1% 的長期溫飄範圍）
同等量級，但**這個值只存在於 host 端**，MCU 韌體本身目前沒有
任何機制知道「host 覺得我現在的實際頻率是多少」——要用這個數字
來補償，需要新的機制把它同步回 MCU（或者 MCU 自己用某種方式量測
校準,例如拿 LSE 當基準去校 HSI16，這是另一個更大的題目，本輪不
展開）。

### B5. 結論：主導誤差是哪一項

**排序（由大到小）**：

1. **`f_SYSCLK` 標稱值 vs 實際值的落差（≈3500ppm，源自 HSI16 未經
   晶體校準，±1% 容差帶內）——主導誤差項**，比其他所有項目大兩到
   三個量級。
2. LPTIM 量化誤差（±1 tick ≈ 30.52µs，只在 UART 提前喚醒時出現）
   ——固定量級，跟睡多久無關。
3. LSE 頻率誤差（±20ppm，0.5s 睡眠下 ≈±10µs）——隨睡眠時長線性
   增加，但係數（20ppm）遠小於 HSI16 的 3500ppm。

**如果 D1 方案要做，補償公式必須用某種方式取得接近真實的
`f_SYSCLK`（而不是編譯期常數 `CONFIG_CLOCK_FREQ`），否則 B4 這項
系統性誤差會完全蓋掉其他所有努力。**

---

## C. 風險清單

### C1. `CYCCNT` 跳躍跨過 32-bit 回捲點時，高位字偵測會不會漏算

**機制**（見 A3）：`stats_update()` 每 5 秒檢查一次「`cur` 有沒有
比上次記錄的 `stats_send_time` 更小」，一次回捲只加一次
`stats_send_time_high`。**如果一次補償寫入讓 `CYCCNT` 跳過了
「超過一個完整回捲週期」的距離（≈26.8 秒等級,見 A3），中間發生的
回捲次數會被少算**——例如跳過了 2 個回捲週期，`stats_send_time_
high` 卻只會被偵測到「有跳過」這一件事（`cur < stats_send_time`
是二元判斷，不是次數），高位字會少算 1，`get_uptime` 回報的
`high` 欄位就會錯，量級正是 26.8 秒×2^32 ticks 那種級別的巨大誤差
（症狀：`get_uptime` 回報的 64-bit uptime 突然「少了一大段」或跟
host 端换算出來的印表時間對不上）。

**目前的風險等級：低，但不是零**。現有 LPTIM1 的 `ARR` 是 16-bit
@ 32kHz，單次最長睡眠約 2047ms（`LPTIM1_ARR_MAX/LSI_HZ_NOMINAL`,
[[2b_g_wake_latency]] 已經確認過這個上限），遠小於 26.8 秒，**單次
睡眠的補償跳躍不可能自己跨過一個完整回捲週期**。但如果 step 3
之後允許「連續多次睡眠、中間不喚醒完全恢復排程」的設計（例如
背靠背睡好幾輪都不回主迴圈），累積的補償量有可能跨過回捲點——這個
情境本輪沒有設計出來，只是先標記這個邊界條件存在。

**偵測方式**：比對 `get_uptime` 的 `high` 欄位增長速率是否跟牆鐘
時間吻合（正常情況下 `high` 每 26.8 秒左右應該增加 1），如果補償
邏輯有這個 bug，會看到 `high` 該增加時沒增加，或者增加的時間點
跟預期不符。

### C2. 睡眠期間若有已排程 timer 到期，醒來後的行為

**這是本輪查證出來最嚴重的風險，直接關聯 A4 的缺口。**

`armcm_timer.c:135-138`（`timer_dispatch_many()`）：

```c
if (unlikely(timer_is_before(tru, now))) {
    if (diff < (int32_t)(-timer_from_us(1000)))
        try_shutdown("Rescheduled timer in the past");
```

如果 Stop2 睡眠期間有任何一個已排程的 timer（不管是不是 stepper，
見 C4）本來要在睡眠窗口內到期，醒來後如果直接把補償量整段寫進
`CYCCNT`，那個 timer 的 `waketime` 相對「補償後的 `now`」會變成
**逾期了整段睡眠時長**（例如睡了 400ms，逾期就是 400,000µs 等級）
——這**遠遠超過** `try_shutdown` 的 1000µs 容忍門檻，**會讓韌體
立刻自我關機（`try_shutdown`）**。

**症狀**：韌體在 Stop2 醒來後幾乎立刻進入 shutdown 狀態，host 端
會看到 `is_shutdown`／連線中斷，訊息可能包含
「Rescheduled timer in the past」這個字串（`try_shutdown` 呼叫時
帶的訊息）。

**偵測方式**：這個症狀非常明確、幾乎不會被誤判——直接搜尋
`klippy.log` 或 MCU 回應裡有沒有 `shutdown` 訊息、`static_string_
id` 對應到這個字串。

**這代表 D1 方案「醒來後直接補 `CYCCNT`」若要安全，前提是進 Stop2
之前就必須確認「這次要睡的時長，不會跨過任何已排程 timer 的到期
時刻」——而 A4 已經確認目前完全沒有這個查詢能力。這是本輪盤點中
D1 方案能不能成立的關鍵前提缺口，不是次要細節。**

### C3. host `clocksync` 對突發時脈跳躍的容忍度

`klippy/clocksync.py`：

```python
86:  clock_diff2 = (clock - exp_clock)**2
87:  if (clock_diff2 > 25. * self.prediction_variance
88:      and clock_diff2 > (.000500 * self.mcu_freq)**2):
89:      if clock > exp_clock and sent_time < self.last_prediction_time+10.:
90:          logging.debug("Ignoring clock sample ...")
91:          return
95:      logging.info("Resetting prediction variance ...")
99:      self.prediction_variance = (.001 * self.mcu_freq)**2
```

**機制**：收到的 `clock` 樣本跟線性回歸預測值（`exp_clock`）的
差距，只要同時滿足「超過目前變異數估計的 25 倍」**且**「超過
0.5ms（`.000500`）對應的 tick 數」，就判定為異常樣本：

- 如果是**正向**跳躍（`clock > exp_clock`，時鐘看起來變快/往前跳）
  且**10 秒內已經忽略過**類似樣本——直接忽略這個樣本，不更新回歸
  模型（行 89-94）。
- 否則（包含第一次遇到的正向跳躍，或任何負向跳躍）——**重設
  `prediction_variance` 成一個很寬鬆的值**（`(0.001×freq)²`，
  行 99），讓後續樣本更容易被模型接受，不會連續判定成異常。

★ **這個機制本身不會讓連線中斷或崩潰**——`clocksync` 有為「突發
的大幅時鐘落差」設計容忍/復原路徑，這是好消息。**但每一次 Stop2
醒來的補償跳躍，幾乎必然會先觸發一次「Resetting prediction
variance」**（本輪整個對話過程中已經多次觀察到這行 log，雖然是在
「MCU 剛連線、host 端估計還沒收斂」的情境下觸發，但機制完全相同）
——**如果 step 3 讓 Stop2 頻繁發生（例如閒置時每隔幾百毫秒就睡一
次），`clocksync` 的頻率估計會持續處在「剛被重設、還在重新收斂」
的狀態，長期精度可能因此下降**，本輪沒有實測量化這個長期精度
影響有多大，只確認了機制的存在跟觸發條件，列進「我無法判定的事」。

### C4. 步進馬達運作中睡眠的風險，以及「只在 idle 睡」能不能由韌體自行判定

**風險本身**：如果 Stop2 睡眠發生在 stepper 正在走位期間，
stepper 的下一個脈波 timer 就是 C2 講的「已排程 timer」，睡過去
一定會撞上 C2 的 shutdown 風險（stepper 的 pulse timer 通常是
微秒到毫秒級間距，遠比任何值得進 Stop2 的睡眠時長更密集）。

**能不能由韌體自行判定**：查 `src/stepper.c`——每個 `struct
stepper`（行 40 起）把自己的下一次脈波排程包裝成一個普通
`struct timer`（`stepper_event_edge()`/`stepper_event_full()`
等，透過 `container_of(t, struct stepper, time)` 從通用 timer
反查回 stepper 自己），跟系統裡其他任何 timer（加熱器 PWM、LED
閃爍等）**共用同一個 `SchedStatus.timer_list`，沒有專屬的「有
stepper 在動」全域旗標**。

★ **結論：韌體沒有辦法直接問「現在有沒有 stepper 在動」，但這個
問題其實不需要單獨回答**——只要 C2 的前提成立（進 Stop2 前必須
確認不會跨過下一個已排程 timer），active stepper 的高頻脈波 timer
本身就會讓「下一個 timer 到期時間」變得極短（微秒到毫秒級），短到
不足以覆蓋 Stop2 本身的喚醒延遲（[[2b_g_wake_latency]] 量到的
121.6~180.9µs），自然就不會嘗試進入 Stop2——**「只在 idle 睡」這個
前提，是「不跨過下一個 timer」這個更基本的安全規則的自然結果，
不需要另外設計一套「偵測 stepper」的機制**。前提是 A4 的查詢 API
真的被實作出來且正確使用。

---

## D. 方案比較

### D1. 醒來後直接補 `CYCCNT`

**做法**：LPTIM 到期或 UART 提前喚醒後，讀 `ARR`（到期）或 `CNT`
（提前喚醒），換算成對應的 `CYCCNT` tick 數，直接
`DWT->CYCCNT += compensation;`。

**需要改的檔案**：
- `src/stm32/stm32u5_lowpower.c`（`stop2_once()`，加補償計算跟
  寫入）
- `src/sched.c`（新增一個「查詢下一個 timer waketime，不觸發
  dispatch」的 API，供進 Stop2 前判斷安全睡眠上限——這是 A4/C2
  指出的必要前提，不是可選項）
- 可能需要某種機制讓 MCU 取得比 `CONFIG_CLOCK_FREQ` 更準的
  `f_SYSCLK`（B4/B5），否則補償量本身就帶系統性偏差

**誤差量級**：如果用實際頻率換算，主導誤差降到 B2/B3 等級
（±30µs 量化，UART 提前喚醒時）；如果偷懶用標稱頻率，誤差量級被
B4 的 ~3500ppm 系統性偏差主導，隨睡眠時長線性放大（睡 1 秒錯
3.5ms，睡 10 秒錯 35ms）。

**失效模式**：C1（回捲漏算，目前風險低但非零）、**C2（撞上已排程
timer 直接 shutdown，是這個方案最大的風險，前提沒做對就是硬傷）**、
C3（`clocksync` 每次都要重新收斂，長期精度影響未知）。

### D2. 不補 `CYCCNT`，改在 host 端處理

**做法**：MCU 完全不管 `CYCCNT` 停過，`clocksync.py`／
`serialhdl.py` 自己想辦法知道「MCU 剛才睡了多久」，在 host 端的
時間換算裡扣掉/修正這段落差。

★ **這會違反「不改 klippy」的限制**——`clocksync.py`/
`serialhdl.py` 都是 `klippy/` 底下的 host 端程式碼，不是 MCU
韌體，改這些檔案就是改 klippy host，超出這個專案目前的授權範圍
（本輪只是列出來對照，不是本輪的候選方案）。

**需要改的檔案（如果真的要做）**：`klippy/clocksync.py`（改寫
`_handle_clock`/回歸模型，讓它能接受「MCU 回報的 clock 出現一段
凍結」這種模式，而不是把它當異常樣本處理）、`klippy/serialhdl.py`
（可能需要額外的旁路通道讓 MCU 告知「我剛睡了多久」，這本身還是
要在 MCU 端新增 sendf，等於韌體端也要改，不是純 host 端方案）。

**誤差量級/失效模式**：如果完全不補償、host 端也不知道 MCU 睡過
——host 端所有依賴 `clock` 換算印表時間的邏輯，在這段睡眠期間會
整段失真（不是 µs 級的小誤差，是整個睡眠時長量級的系統性位移）。
要讓 host 端正確處理，勢必需要某種「MCU 睡了多久」的資訊來源，
而這個資訊只能來自 MCU 韌體自己——**這代表 D2 沒辦法真正做到
「完全不改韌體」，本質上還是要靠韌體提供資訊，只是把「怎麼用這個
資訊」的邏輯放在 host 端，改動範圍反而更大（兩邊都要改），且直接
違反限制**，列在這裡純粹是比較用，不是可行選項。

### D3. 限制單次睡眠長度到誤差可忽略的範圍，不做補償

**做法**：完全不寫 `CYCCNT`，讓它就這樣凍結整段睡眠時間，靠「睡得
夠短，凍結造成的落差小到 `clocksync` 的 outlier 判定門檻偵測不到
或不在乎」這個前提來迴避問題。

**需要改的檔案**：理論上不需要新增補償邏輯，只要在既有的
`lptim1_wakeup_init()`（`stm32u5_lowpower.c`）把 `ARR` 的允許上限
鎖在一個遠小於某個安全值的常數——**改動範圍最小**。

**誤差量級**：關鍵在於「多短算安全」——C3 查到 `clocksync` 的
outlier 門檻是 `0.5ms`（`.000500 * mcu_freq`，`clocksync.py:88`）
**worth of ticks**，這給了一個明確的參考上限：如果 Stop2 凍結
`CYCCNT` 的時長換算成的「clock 落差」遠小於 0.5ms，`clocksync`
大概不會觸發 outlier 判定路徑。

★ **這個方案有一個本輪發現的核心矛盾，必須寫清楚**：Stop2 本身的
喚醒延遲已經量到 121.6~180.9µs（[[2b_g_wake_latency]] 第 5、7
節）——如果為了避開 `clocksync` 的偵測門檻，把允許的睡眠時長也
壓到遠低於 0.5ms（比如幾百 µs）等級，**扣掉喚醒延遲之後，Stop2
真正省到電的淨睡眠時間會被壓縮到接近零，甚至可能因為 Stop2 進入/
退出本身的額外開銷而變成負收益**——換句話說，**「睡得夠短才不用
補償」跟「睡得夠久才有省電意義」這兩個要求直接衝突**，D3 方案要
成立，必須先回答「有沒有一個時間窗，同時滿足『夠短躲過 clocksync
偵測』跟『夠長讓 Stop2 值得進』」，本輪沒有做實測去回答這個問題
（見「我無法判定的事」）。

**失效模式**：如果選的睡眠上限沒有真的躲過 `clocksync` 的偵測
門檻，退化成跟 D1 不補償版本一樣的問題（host 端時間換算失真，
只是失真量級較小）；如果選得太保守（睡眠上限壓得太低），Stop2
機制本身的存在意義被削弱到接近無意義。

---

## E. 我無法判定的事

1. **「值得進 Stop2 的最短睡眠時長」實際數字**：D3 方案的核心矛盾
   （睡短躲避偵測 vs 睡長才省電）需要一個實測數字才能判斷這個
   矛盾是否真的無法兩全，本輪只有理論分析（喚醒延遲 121.6~
   180.9µs、`clocksync` 門檻 0.5ms），沒有做「Stop2 淨省電量隨
   睡眠時長變化」的實測，無法給出確切的可行區間（如果存在）。
2. **`clocksync` 的 variance-reset 機制長期、連續多次觸發下的精度
   影響**：C3 確認了機制存在、單次觸發不會導致連線問題，但如果
   Stop2 在真實閒置情境下頻繁發生（例如每秒好幾次），`clocksync`
   反覆處在「剛重設、還在收斂」的狀態對長期印表時間精度的實際
   影響，本輪沒有實測量化，只能確認「機制上不會崩潰」，不能確認
   「精度足夠好」。
3. **`sched.c` 新增「查詢下一個 timer waketime」API 的具體設計**：
   A4/C2 已經確認這個 API 目前不存在、且是 D1 方案的必要前提，但
   這個 API 該回傳什麼型態、要不要特別處理 `periodic_timer`/
   `sentinel_timer` 這兩個永遠存在的內部 timer（避免把它們當成
   「真正的下一個任務」而誤判安全睡眠時長），本輪只指出缺口，沒有
   設計介面本身，需要下一輪決定。
4. **LPTIM1 `ARR` 現有的 16-bit@32kHz 上限（約 2047ms）是否足夠
   支撐真實列印閒置情境的睡眠需求**：本輪沒有真實列印任務的閒置
   時間分布數據可以比對，無法判斷這個上限在實務上夠不夠用，或者
   需要多次背靠背睡眠（進而讓 C1 的回捲風險從「理論上不可能」變成
   「需要認真考慮」）。
5. **HSI16 的 ±1% 容差在不同溫度/板卡下的實際分布穩定性**：B4 的
   0.35% offset 只是這一顆板子、這一次量測的結果，沒有跨板卡、跨
   溫度的重複測量可以確認這個數字在其他條件下是否穩定、是否需要
   對每顆板子分別校準，還是可以當作一個近似通用常數使用。
