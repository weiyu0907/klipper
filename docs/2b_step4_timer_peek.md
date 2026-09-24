# Milestone 2B step 4 — `sched.c` peek-next-timer API

延續 [[2b_step3_clock_design]] A4/C2 指出的缺口：`sched.c` 沒有
「查詢下一個 timer 到期時間、不觸發 dispatch」的能力。本輪新增這個
API，**純新增，不改任何既有函式**，暫不接進 `stop2_once()`，只加一個
唯讀驗證指令。

---

## A. 唯讀查證（已在實作前貼給使用者確認）

### A1. `timer_list` 排序與 sentinel timer

`struct timer`（`src/sched.h:15-19`）：`{next, func, waketime}`，單向
鏈結串列。**`timer_list` 全程保持依 `waketime` 遞增排序，
`SchedStatus.timer_list` 永遠指向最早到期者**：

- `insert_timer()`（`sched.c:67-82`）：從指定起點往後找第一個
  `timer_is_before(waketime, pos->waketime)` 成立的位置插入，維持
  遞增排序。
- `sched_add_timer()`（`sched.c:86-107`）：新 timer 若比目前的頭
  更早（行91），用靜態哨兵 `deleted_timer` 當代理原子換頭（行
  99-101），避免 `waketime` 跟 `next` 不同步的中間狀態被 IRQ
  context 看到。
- `sched_del_timer()`（`sched.c:121-143`）：刪除目前的頭時，同樣
  借用 `deleted_timer`（行127-129）,但 `deleted_timer.waketime`
  被設成**被刪除者的舊 waketime**，不是新頭的——這個特性直接寫進了
  `sched_timer_peek()` 的程式碼註解（見 B 節）：因為串列排序保證
  舊值不晚於新頭真值，peek 在這個窄窗口只會低估、不會高估，方向
  安全。

**`sentinel_timer`**（`sched.c:61-64`）：`waketime` 恆為
`periodic_timer.waketime + 0x80000000`，純粹是走訪時的尾端哨兵，
真被 dispatch 到就直接 `shutdown`（行56-58）。正常運作下
`periodic_timer` 永遠排在它前面，不會被 peek 到。

**結論：不需要走訪找最小值，`SchedStatus.timer_list->waketime`
本身就是答案，O(1)。**

### A2. timer 新增/移除的 context，為何查詢必須在關中斷區段內

`sched_add_timer()`/`sched_del_timer()` 都用 `irq_save()`/
`irq_restore()`（行89,106,124,142）包住修改——代表兩者可以從**任意
context**（task 或 IRQ handler）呼叫，函式自己負責保護修改過程。
timer callback 常在 IRQ context（`SysTick_Handler`）裡透過
`SF_RESCHEDULE` 或直接呼叫 `sched_add_timer()` 排入新的後續 timer。
**`timer_list` 可能在任何「中斷仍開著」的瞬間被異動**——查完
`waketime` 後如果暫時開了中斷，剛好在那個縫隙插入的新 timer 可能
比查到的值更早，讓查到的預算變成過期資訊，若照這個過期預算進
`wfi` 可能睡過頭撞上新排入的 timer，觸發 `armcm_timer.c` 既有的
「Rescheduled timer in the past」自我關機。

### A3. 已知週期性 timer 與板子的 `printer.cfg`

**`periodic_timer`**（`sched.c:34-48`）：`periodic_event()` 每次
觸發把自己的 `waketime` 往後推 `timer_from_us(100000)`（100ms，
行40）——唯一保證「永遠存在、永遠在 100ms 內」的 timer。

板子 `printer.cfg`（`~/printer_data/config/printer.cfg`，唯讀查看）：

```
[include mainsail.cfg] [mcu] [printer] [stepper_x] [stepper_y]
[stepper_z] [virtual_sdcard] [pause_resume] [display_status]
[statistics] [force_move]
```

**沒有 `[heater_bed]`/`[extruder]`/`[fan]`/ADC 溫感等會掛週期性
PWM/取樣 timer 的區塊**——只有 stepper（只在真正走位時才有 pulse
timer，真正 idle 時不掛號）跟幾個純軟體模組。**代表這塊板子真正
idle 時，`timer_list` 裡除了必然存在的 `periodic_timer`（跟排在
它後面的 `sentinel_timer`）之外，沒有其他常駐的 timer**——是驗證
這個 API 的乾淨測試環境，不會被其他 100ms 以下週期的 timer 干擾。

---

## B. 實作

### `src/sched.c`：新增兩個函式 + 一個驗證指令

```c
uint32_t
sched_timer_peek(void)
{
    return SchedStatus.timer_list->waketime;
}

uint32_t
sched_timer_peek_budget(uint32_t now, uint32_t max_ticks)
{
    uint32_t waketime = SchedStatus.timer_list->waketime;
    if (timer_is_before(waketime, now))
        return 0;
    uint32_t avail = waketime - now;
    if (avail > max_ticks)
        return max_ticks;
    return avail;
}

void
command_get_timer_budget(uint32_t *args)
{
    uint32_t max_ticks = args[0];
    irqstatus_t flag = irq_save();
    uint32_t now = timer_read_time();
    uint32_t budget = sched_timer_peek_budget(now, max_ticks);
    irq_restore(flag);
    sendf("timer_budget now=%u budget=%u", now, budget);
}
DECL_COMMAND(command_get_timer_budget, "get_timer_budget max_ticks=%u");
```

完整版本（含安全論證跟使用契約的程式碼註解，見需求 1/2）已經寫進
`src/sched.c`，不在這裡重複貼——重點摘要：

- **安全方向論證**（寫在 `sched_timer_peek()` 上方）：`sched_del_
  timer()` 刪頭時 `deleted_timer.waketime` 帶的是被刪除者的舊值，
  排序保證這個舊值不晚於新頭真值，所以 peek 只會低估、不會高估；
  低估＝提早醒＝安全，高估＝睡過頭＝撞上「Rescheduled timer in
  the past」關機。
- **使用契約**（同一段註解）：回傳值只在跟呼叫端「查詢→決定睡多久
  →進 wfi」同一個 PRIMASK=1 區段內有效。
- **`sched_timer_peek_budget()` 的用途邊界**（函式上方註解）：這是
  **原始排程層預算**，不包含喚醒延遲——呼叫端必須自己扣掉
  `tWUSTOP2 + restore_us`（實測約 121~181µs，視 `STOPWUCK` 而定,
  [[2b_g_wake_latency]]）跟安全餘裕才是真正可睡的時間；`max_ticks`
  這個上限由呼叫端自己決定（IWDG 預算約 0.5s、或
  [[2b_step2_t4_overflow]] 講的 `receive_buf` 相關約束），`sched.c`
  不負責、也沒有能力知道這些平台層限制。
- `command_get_timer_budget()` 只是驗證用，**沒有接進任何低功耗
  路徑**，`now`／`budget` 的查詢動作包在同一個 `irq_save`/
  `irq_restore` 區段內，本身就是契約的一個實際示範。

`src/sched.h` 新增兩行宣告，其餘全部既有函式簽名/行為一字未動。

---

## C. 燒錄與驗證

**版本字串**：`v0.13.0-474-ge9985ad22-dirty-20260924_170438-hunter`
（`Loaded 138 commands`，比 Phase 2 的 136 多 2——精確對應新增的
`get_timer_budget` 指令 + `timer_budget` 回應）。

`openocd program ... verify` → `** Verified OK **`。

### 取樣方法可行性：`console.py` 單筆往返時間

本輪對話從 T1 到 T4 所有 `STATS` 查詢的 `srtt`（host 端量到的平滑
單趟往返時間）**全部一致顯示 `srtt=0.002`（2ms）**——遠低於 20ms
取樣間隔，代表可以用「個別間隔發送指令」的方式做細取樣，不需要
「單次指令回報多筆內部取樣」這個備案。

### C1：粗取樣（200ms × 20 筆）——判準：值域 0~100ms、無負值、非固定值

原始數據（`get_timer_budget max_ticks=4294967295`，19/20 筆被完整
擷取，換算 ms 用 `÷160560`,即 [[2b_step3_clock_design]] B4 查到的
實測頻率 ≈160.5587MHz）：

| # | `now`（ticks） | `budget`（ticks） | budget（ms） |
|---|---|---|---|
| 1 | 1125989910 | 3042794 | 18.95 |
| 2 | 1158849660 | 2183044 | 13.60 |
| 3 | 1191917810 | 1114894 | 6.94 |
| 4 | 1224842500 | 190204 | 1.18 |
| 5 | 1257681070 | 15351634 | 95.56 |
| 6 | 1290589700 | 14443004 | 89.96 |
| 7 | 1316320120 | 4712584 | 29.35 |
| 8 | 1372796310 | 12236394 | 76.19 |
| 9 | 1380829100 | 4203604 | 26.18 |
| 10 | 1422145410 | 10887294 | 67.80 |
| 11 | 1455079910 | 9952794 | 61.99 |
| 12 | 1487922800 | 9109904 | 56.72 |
| 13 | 1520829930 | 8202774 | 51.09 |
| 14 | 1553711470 | 7321234 | 45.61 |
| 15 | 1586585080 | 6447624 | 40.16 |
| 16 | 1619491030 | 5541674 | 34.51 |
| 17 | 1652368000 | 4664704 | 29.05 |
| 18 | 1685293650 | 3739054 | 23.29 |
| 19 | 1703499560 | 1533144 | 9.55 |
| 20 | 1751663490 | 1369214 | 8.53 |

**判讀**：值域 1.18~95.56ms，**無負值、無固定值、沒有明顯超過
100ms 的值**——跟「200ms 取樣對 100ms 週期會混疊、呈散亂分布（不是
乾淨遞減斜坡）」的預期完全吻合。**沒有觸發停止條件。**

### C2：細取樣（~20ms × 30 筆）——判準：鋸齒波，上限 ~100ms，線性遞減，每 100ms 重置

原始數據（30/30 筆全部擷取，實測取樣間隔落在 19~22ms,見
`now` 欄位差值）：

| # | `now`（ticks） | `budget`（ticks） | budget（ms） | 與前一筆差 |
|---|---|---|---|---|
| 1 | 3427689884 | 14375524 | 89.55 | — |
| 2 | 3430899684 | 11165724 | 69.53 | −20.02 |
| 3 | 3434047284 | 8018124 | 49.94 | −19.59 |
| 4 | 3437448564 | 4616844 | 28.75 | −21.19 |
| 5 | 3440770324 | 1295084 | 8.06 | −20.69 |
| 6 | 3444120484 | 13944924 | 86.82 | **+78.76（重置）** |
| 7 | 3447462124 | 10603284 | 66.01 | −20.81 |
| 8 | 3450781594 | 7283814 | 45.36 | −20.65 |
| 9 | 3454093084 | 3972324 | 24.74 | −20.62 |
| 10 | 3457471064 | 594344 | 3.70 | −21.04 |
| 11 | 3460749744 | 13315664 | 82.94 | **+79.24（重置）** |
| 12 | 3464047544 | 10017864 | 62.36 | −20.58 |
| 13 | 3467492344 | 6573064 | 40.94 | −21.42 |
| 14 | 3470728164 | 3337244 | 20.78 | −20.16 |
| 15 | 3473986184 | 79224 | 0.49 | −20.29 |
| 16 | 3477399484 | 12665924 | 78.90 | **+78.41（重置）** |
| 17 | 3480634214 | 9431194 | 58.72 | −20.18 |
| 18 | 3483941744 | 6123664 | 38.14 | −20.58 |
| 19 | 3487296904 | 2768504 | 17.24 | −20.90 |
| 20 | 3490527744 | 15537664 | 96.72 | **+79.48（重置）** |
| 21 | 3493847394 | 12218014 | 76.10 | −20.62 |
| 22 | 3497229024 | 8836384 | 55.02 | −21.08 |
| 23 | 3500492424 | 5572984 | 34.72 | −20.30 |
| 24 | 3503769724 | 2295684 | 14.30 | −20.42 |
| 25 | 3507264944 | 14800464 | 92.19 | **+77.89（重置）** |
| 26 | 3510462144 | 11603264 | 72.28 | −19.91 |
| 27 | 3513773154 | 8292254 | 51.62 | −20.66 |
| 28 | 3517106034 | 4959374 | 30.89 | −21.34 |
| 29 | 3520383084 | 1682324 | 10.47 | −20.42 |
| 30 | 3523641584 | 14423824 | 89.79 | **+79.32（重置）** |

**判讀**：**清楚的鋸齒波**——每組 4~5 筆連續遞減（每筆之間差約
−19.6~−21.4ms，跟實際取樣間隔 19~22ms 精確對應，代表 budget 就是
單純隨真實時間線性倒數），接著一次性跳回接近上限（重置後的值落在
78.90~96.72ms 之間，都在 100ms 以內，差異來自取樣網格跟
`periodic_timer` 相位沒有對齊——每個遞減區段的重置點落在 100ms
週期的哪個相位是隨機的，這是預期中的取樣假影，不是異常）。**6 次
重置（第 6/11/16/20/25/30 筆），平均間隔約 5 筆──跟「~20ms 取樣
間隔、100ms 週期」算出來的 100/20=5 筆完全吻合。**

**額外交叉驗證**（跟 [[2b_step3_clock_design]] B4 的關聯）：重置點
之間的 `now` 差值，例如第 6 筆到第 11 筆：
`3460749744-3444120484=16629260` ticks。`periodic_timer` 每次固定
推進 `timer_from_us(100000)=100000×160=16,000,000` ticks（用**編譯
期標稱** 160MHz 算的固定 tick 數，不是時間）。`16629260` 比
`16000000` 多出來的部分是「這個間隔內又多發生了一次取樣」的正常
現象,不是矛盾——但拿 `16000000` 這個固定 tick 數，用**實測頻率**
換算回真實時間：`16000000/160560≈99.65ms`，比名義上的 100ms 略短
——跟 [[2b_step3_clock_design]] B4/B5「`f_SYSCLK` 實際比標稱快
≈3492ppm，補償/換算不能用標稱值」的結論方向一致，這裡等於在完全
不同的測試場景下,又一次獨立印證了同一個發現。

**沒有觸發停止條件**（沒有固定值、沒有超過 100ms 太多的值——最大
觀測值 96.72ms，在合理誤差範圍內）。**兩組驗證都通過。**

---

## D. 補償用頻率基準的附記（延續 [[2b_step3_clock_design]] B4/B5）

**再次強調並量化**：任何未來把 `sched_timer_peek_budget()` 的結果
拿去反過來換算「LPTIM 該設多少 `ARR`」或「醒來後 `CYCCNT` 該補多少」
的邏輯，**不可以用 `CONFIG_CLOCK_FREQ`（編譯期常數，標稱
160MHz）做這個換算**——[[2b_step3_clock_design]] B4 已經查證
`PLL1SRC=HSI16`（`stm32u5.c:113`），`CYCCNT` 的真實走速繼承 HSI16
±1% 容差，本輪 C2 節的細取樣數據又獨立印證了一次同一個落差
（`periodic_timer` 的固定 tick 數換算回真實時間比名義 100ms 短
≈0.35ms）。

**具體量級**：用 B4 量到的 ≈3492ppm 系統性偏差，睡眠 0.5 秒的補償
若用標稱頻率換算：

```
誤差 = 0.5 s × 3492 × 1e-6 = 1.746 × 10⁻³ s ≈ 1.75 ms
```

**1.75ms 這個誤差量級，遠遠蓋過 LSE ±20ppm（≈10µs/0.5s）跟 LPTIM
量化誤差（±30.52µs/tick）——如果補償公式用錯了頻率基準，這一項
單獨就能讓整個補償機制失去意義。**

**列入 step 5 候選方案：執行期自我校準 CYCCNT/LPTIM tick 比值**——
不使用任何絕對頻率常數（不管是編譯期標稱值還是本輪這種事後查表
量到的近似值），而是在每次「LPTIM 到期喚醒」（B3，[[2b_step3_
clock_design]]，沒有量化誤差的那條路徑）時，同時記錄「這次睡眠對應
的 `ARR` tick 數」跟「醒來後量到的 `CYCCNT` 差值」，用這兩個現場量
到的數字直接算出當下的 tick 比值,不需要事先知道 `f_SYSCLK` 的絕對
值是多少。**優點**：(a) 不受 HSI16 出廠校準值跟實際溫度下真實頻率
落差的影響——本輪 B4 量到的 3492ppm 是「這次量測、這個溫度」下的
快照，不保證溫度變了以後還是同一個數字；自我校準是每次醒來都重新
量一次,天然跟著溫度漂移走，不會固定在一個過時的常數上。(b) 不需要
任何外部頻率標準或查表——本輪能查到 3492ppm 靠的是 host 端
`clocksync` 長時間收斂出來的估計值，MCU 韌體自己完全沒有這個資訊
管道，自我校準等於讓 MCU 不依賴 host 就能自己維護一個持續更新的
比值。**代價**：只在 LPTIM 到期（`ARR` match）這條路徑上才能校準
（UART 提前喚醒那條路徑本身需要用到還沒校準好的比值,存在先後
依賴），且第一次開機、還沒有任何一次成功的 Stop2 循環之前，沒有
校準數據可用,需要一個初始預設值（例如就先用標稱 160MHz 當
fallback，直到第一次真正睡過一輪為止）。本輪只列出這個方案的
概念跟取捨，沒有設計具體的校準演算法或程式碼，留給 step 5。

---

## E. 收尾

```
$ curl printer/info: state=ready
$ MCU last_stats: bytes_retransmit=0 bytes_invalid=0
$ mcu_version: v0.13.0-474-ge9985ad22-dirty-20260924_170438-hunter（跟本輪燒錄版本一致）
```

**本輪嚴格照範圍限制**：`sched_timer_peek()`/`sched_timer_peek_
budget()` 沒有接進 `stop2_once()` 或任何低功耗路徑，只被
`command_get_timer_budget()` 這個驗證指令呼叫；沒有做任何 Stop2
睡眠測試。下一步（把這個 API 接進實際的 Stop2 idle 判斷邏輯）待
使用者後續指示。
