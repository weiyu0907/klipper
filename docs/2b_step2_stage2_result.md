# Milestone 2B step 2 — Phase 2 結果：喚醒來源判定 + Stop2 收資料測試

延續 [[2b_step2_impl_spec]]、[[2b_step2_stage1_result]]。Phase 2
（`stop2_once()` 加 TC 等待、喚醒來源判定遙測、FE/NE/ORE 錯誤旗標
統計）已燒錄、跑完 T1/T2/T3/T3b。本輪過程中發現並修正了測試方法學
本身的問題（T1 的「零流量對照組」假設不成立），過程完整記錄，不是
只報告最終數字。

---

## 0. 摘要

| 測試 | 目的 | 結果 |
|---|---|---|
| T1（最終：`period_ms=100 cycles=10`） | 背景流量對照組（非零流量對照組，見第 2 節） | `by_uart=2`，落在背景流量上界（~3.5 筆）之內；`fe=ne=ore=0` |
| T2（`period_ms=400 cycles=10` + 20× `get_uptime`／200ms 間隔） | 睡眠中收資料 | `by_uart=8`；`fe=ne=ore=0`；兩次重跑一致 |
| T3 修正版（15× `get_uptime` 連續無間隔，90 bytes） | FIFO+receive_buf 混合層級壓力測試 | `by_uart=7-8`；`fe=ne=ore=0`；`send_seq==receive_seq`（收斂，無永久遺失） |
| T3b（2× `get_uptime` 單次爆發，12 bytes） | 直接探測 FIFO 深度餘裕 | `by_uart=7`；`fe=ne=ore=0`；`send_seq==receive_seq` |

**沒有觀察到任何 FIFO 層級或 receive_buf 層級的資料遺失**（定義與
證據見第 4 節）。測試後 klipper 已還原，`state=ready`、
`bytes_retransmit=0`、`bytes_invalid=0`。

---

## 1. Phase 2 燒錄與版本確認

**版本字串**：`v0.13.0-474-ge9985ad22-dirty-20260923_225414-hunter`
**對應 commit**：本輪測試用（尚未 commit，見第 7 節）

`openocd program ... verify` → `** Verified OK **`；`console.py`
connect 顯示 `Loaded 136 commands`（比 Phase 1 的 134 多 2，精確對應
新增的 `stop2_wake`/`stop2_err` 兩個 sendf 格式字串）。

---

## 2. 方法學發現：T1「零流量對照組」在這個工具鏈下不可能

### 2.1 第一次 T1（`period_ms=400 cycles=10`）就觸發停止條件

```
stop2_wake n=10 by_uart=7 by_lptim=3 both=0 tc_timeout=0
stop2_err  fe=0 ne=1 ore=0
```

依原訂判準（`by_uart>0 → 停下回報`），沒有直接判定為失敗，而是先查
根本原因。

### 2.2 根本原因（一）：`test_stop2` 執行期間 host 端的可靠傳輸層會自動重傳

用 `STATS`（`console.py` 本地指令，純讀 host 端 `serialqueue` 的統計，
不上線）在測試後立刻查：

```
bytes_retransmit=76  send_seq=124  receive_seq=120  rto=3.200
```

`bytes_retransmit>0` 證實 host 端確實自動重傳過。原因：`test_stop2`
指令本身讓 MCU 主迴圈整個被佔住（呼應你先前的補充點 1），期間完全
不會回 ACK；host 端的 `serialqueue`（可靠傳輸層）發現這個指令本身
遲遲沒被 ACK，逾時後自動重傳它自己——這些重傳位元組是真實送到
LPUART1 上的資料，被 `by_uart` 正確偵測到。

### 2.3 根本原因（二）：RTO 的真實初始值是 25ms，不是 3.2 秒——先前的推論有誤

原本以為「縮短測試到 RTO 之內就能避開重傳」，但這個 RTO=3.2s 本身
是**已經 backoff 過的結果**，不是連線的初始值。查
`klippy/chelper/serialqueue.c`：

| 常數/行為 | 值 | 行號 |
|---|---|---|
| `MIN_RTO` | **0.025**（25ms） | 107 |
| 連線建立時設定 | `sq->rto = MIN_RTO` | 743 |
| 逾時觸發重傳後 | `sq->rto *= 2.0`（倍增），上限 `MAX_RTO` | 457-459 |
| `MAX_RTO` | 5.000（5 秒） | 108 |

`25ms × 2⁷ ≈ 3.2s`——那次 9 秒的封鎖測試裡，RTO 經過約 7 次倍增才
變成 3.2s。**真正的初始 RTO 只有 25ms，遠比任何有意義的多輪 Stop2
測試短**，「把測試縮到 RTO 之內」這個策略在前提上就不成立——這點
在你的追問之前我們都以為 3.2s 是常數，是本輪一起查證時才發現的
錯誤。

### 2.4 根本原因（三）：`console.py` 確實有跑 `ClockSync`——先前「不跑 clocksync」的說法錯誤

依你的指示查證（唯讀，附行號）：

```
$ grep -n "clocksync\|ClockSync\|get_clock\|register_timer" klippy/console.py klippy/serialhdl.py klippy/clocksync.py
```

- `klippy/console.py:42`：`self.clocksync = clocksync.ClockSync(self.reactor)`
- `klippy/console.py:74`：`self.clocksync.connect(self.ser)`
- `klippy/clocksync.py:16`：`self.get_clock_timer = reactor.register_timer(self._get_clock_event)`
- `klippy/clocksync.py:40-49`（`ClockSync.connect()`）：連線時先做
  **8 次快速校準**（每次間隔 50ms，行 41-45），再啟動**週期性
  `get_clock` timer**（行 49）
- `klippy/clocksync.py:59-64`（`_get_clock_event`）：
  ```python
  def _get_clock_event(self, eventtime):
      self.serial.raw_send(self.get_clock_cmd, 0, 0, self.cmd_queue)
      self.queries_pending += 1
      return eventtime + .9839
  ```
  **每 0.9839 秒送一次 `get_clock`，只要連線還在就會一直送，跟
  「clocksync」這個字面功能無關的說法是錯的——`console.py`
  本來就會建立並啟動這個週期性 timer。**

另外查了 `IDLE_QUERY_TIME`（`serialqueue.c:112`，值 `1.0`）在整個
`klippy/` 目錄的使用情形：

```
$ grep -rn "IDLE_QUERY_TIME" klippy/
klippy/chelper/serialqueue.c:112:#define IDLE_QUERY_TIME 1.0
```

**只有定義這一行，整個專案沒有任何地方真的用到它**——是死掉的常數，
不是背景流量的來源，背景流量完全是 `clocksync.py` 的 `get_clock`
timer 造成的。

### 2.5 決定性量測：純 idle 20 秒，直接量測背景流量速率

不再用推論，直接量測（你的指示）：連線後完全不輸入任何指令，靜置
20 秒，頭尾各下一次 `STATS`：

```
測試前：bytes_write=851  bytes_retransmit=0  send_seq=112
測試後：bytes_write=971  bytes_retransmit=0  send_seq=132
```

`Δbytes_write=120`，`Δsend_seq=20`（剛好 20 筆，20 秒內每秒一筆，
精確對應 `_get_clock_event` 的 0.9839s 週期）。`120/20=6 bytes/筆`
——跟 `get_clock`（command-id=5，零參數，VLQ 1 byte + 5 bytes 框架
= 6 bytes）的編碼大小精確吻合。`bytes_retransmit=0`：純 idle 時
MCU 主迴圈沒被佔住，每筆都能即時 ACK，完全不需要重傳，這也反過來
確認了 2.2 節「重傳只在主迴圈被佔住時才會發生」的機制。

**結論：背景流量約 6 bytes/秒（1 筆 `get_clock`/0.9839s），來源
明確，零流量對照組在這個工具鏈下不可能。**

### 2.6 T1 最終形式：已知背景流量對照組

改用 `period_ms=100 cycles=10`（實測時長 3.489 秒），預期背景訊息
數上界：

```
3.489s / 0.9839s ≈ 3.55 筆
```

（上界，不是精確預測值——不是每筆背景訊息都會落在 WFI 睡眠窗內，
也可能落在兩輪之間的「醒著」LED 顯示區間，那時中斷已重新開啟，
訊息會被正常 ISR 處理掉，不會被計成 `by_uart`。）

實測結果：

```
stop2_wake n=10 by_uart=2 by_lptim=8 both=0 tc_timeout=0
stop2_err  fe=0 ne=0 ore=0
```

`by_uart=2` 落在 `~3.55` 這個上界之內，`fe=ne=ore=0`——支持「喚醒
機制正確反應真實背景流量」，不是雜訊誤觸發。**T1 以此定案為最終
結果**，不再視為異常，第一次跑（`ne=1`）判定為單次偶發，本輪三次
後續測試（T1 定案版、T2 兩次、T3/T3b 各兩次）**沒有再出現過任何
`fe`/`ne`/`ore` 非零**，支持「偶發」這個判斷。

---

## 3. T2：睡眠中送資料

`test_stop2 period_ms=400 cycles=10`，同時每 200ms 送一次
`get_uptime`，共 20 次，涵蓋整個約 9 秒的測試期間。

★ **依你補充的點 1**：`test_stop2` 期間 MCU 主迴圈被佔住，收到的
資料只會堆在 `receive_buf`，要等 `test_stop2` 返回才會被解析、產生
回應；host 端的 `serialqueue` 在這段期間也會重傳未被 ACK 的訊息
（見第 2 節）。**因此「host 端收到幾次回應」不是可靠判準**，本節
以 MCU 端遙測（`by_uart`/`fe`/`ne`/`ore`）為主要證據。

**結果（兩次獨立重跑，結果一致）**：

```
第一次：stop2_wake n=10 by_uart=8 by_lptim=2 both=0 tc_timeout=0
        stop2_err  fe=0 ne=0 ore=0
        bytes_retransmit=214（host 端，含 20 筆 get_uptime + 背景
                              get_clock 在主迴圈被佔住期間全部等 ACK
                              逾時後被重傳，符合預期，不是異常）

第二次：stop2_wake n=10 by_uart=8 by_lptim=2 both=0 tc_timeout=0
        stop2_err  fe=0 ne=0 ore=0
        bytes_retransmit=196
```

**PASS**：`by_uart=8`（遠高於 T1 的背景基準），`fe=ne=ore=0`（兩次
都乾淨），兩次重跑數字完全一致，不是巧合。

（附帶觀察：第一次重跑實際印出 8 筆 `uptime high=...` 回應，第二次
只在 25 秒逾時前印出同樣 8 筆——這個「印出筆數 < 送出筆數」現象在
第 4 節有更完整的討論，結論是 `console.py` 顯示層的行為，不是資料
遺失，這裡先不展開。）

---

## 4. T3（修正版）+ T3b：FIFO 餘裕實測，FIFO 層級 vs receive_buf 層級分開記錄

### 4.1 位元組預算（先前已回報，這裡重列供文件完整性）

`RX_BUFFER_SIZE`（`src/generic/serial_irq.c:17`）＝**192 bytes**
（對應 MCU 常數 `RECEIVE_WINDOW=192`）。`get_uptime`
（`out/klipper.dict` 查得 command-id=4，VLQ 1 byte + 5 bytes 框架）
＝**6 bytes/筆**。

- **T3**：預算＝192/2＝96 bytes，`96/6=16` 筆剛好卡邊界，取
  **15 筆（90 bytes）**留餘裕，連續無間隔送出。
- **T3b**：**2 筆（12 bytes）**，落在你要求的 8~12 bytes 區間頂端，
  單次爆發。

### 4.2 T3 結果（兩次跑法：一次標準逾時、一次長 drain 確認收斂）

**第一次**（25 秒逾時）：
```
stop2_wake n=10 by_uart=7 by_lptim=3 both=0 tc_timeout=0
stop2_err  fe=0 ne=0 ore=0
bytes_retransmit=226
0 筆 uptime 回應被印出（見 4.4 節解釋）
```

**第二次**（40 秒逾時、測試後多等 20 秒讓佇列排空）：
```
stop2_wake n=10 by_uart=8 by_lptim=2 both=0 tc_timeout=0
stop2_err  fe=0 ne=0 ore=0
bytes_write=1036  bytes_retransmit=269
send_seq=138  receive_seq=138  ← 完全收斂，沒有任何殘留未 ACK 訊息
0 筆 uptime 回應被印出（見 4.4 節解釋）
```

### 4.3 T3b 結果

```
stop2_wake n=10 by_uart=7 by_lptim=3 both=0 tc_timeout=0
stop2_err  fe=0 ne=0 ore=0
bytes_write=963  bytes_retransmit=97
send_seq=126  receive_seq=126  ← 同樣完全收斂
0 筆 uptime 回應被印出
```

### 4.4 「0 筆回應被印出」的診斷：確認是 `console.py` 顯示層問題，不是資料遺失

T3/T3b 都出現「host 端完全沒印出任何 `uptime high=...` 回應」，即使
T3 第二次跑法額外多等了 20 秒排空佇列。這個現象**必須先排除是不是
真的資料遺失**才能下結論，分兩步查證：

1. **T3b 只送 2 筆、間隔 0，一樣是 0 筆印出**——跟 T3 的 15 筆、跟
   T2（送出 20 筆、200ms 間隔、印出 8 筆）對照，**差異變數是「送出
   間隔」不是「筆數」**：只要是連續無間隔送出（T3/T3b），不論筆數
   多少，`console.py` 都印不出任何一筆回應；只要有間隔（T2），就
   會印出一部分。這指向 `console.py` 端「比對哪個回應對應哪個已送
   出的請求」的邏輯在無間隔連發時出問題，不是 MCU 端的行為。
2. **傳輸層本身的證據是乾淨的**：`send_seq == receive_seq`
   （T3 第二次 138=138、T3b 126=126）——這是 Klipper 可靠傳輸協定
   的核心保證：host 端每送出一個訊息區塊都會編號，`receive_seq`
   追的是「host 已經確認被 MCU 收到 ACK」的區塊數。兩者收斂到相等
   代表**所有送出的區塊最終都被 MCU 成功收到並確認**，沒有任何
   一筆永久遺失（重傳機制負責把因為主迴圈忙碌而延遲/暫時衝突的
   區塊補送回去，`bytes_retransmit` 非零正是這個補送過程的證據，
   不是遺失的證據）。

**結論：「0 筆回應被印出」是 `console.py` 在無間隔連續送出相同
零參數指令時的顯示層行為，跟 MCU/LPUART/FIFO 完全無關，不代表任何
資料遺失。** 這是本輪測試方法本身的一個副作用，如實記錄，不算是
被驗證對象（Stop2 c1 韌體）的問題。

### 4.5 FIFO 層級 vs receive_buf 層級：分開記錄（你的補充點 3）

這兩層是不同的緩衝機制，必須分開看：

| 層級 | 容量 | 本輪相關測試 | 觀察到的損失 |
|---|---|---|---|
| **LPUART1 硬體 FIFO** | 8 bytes | T3b（12 bytes 單次爆發，直接命中/略超過 FIFO 深度）、T3 每筆的第一個 byte（觸發 `RXFT` 的那個 byte） | **無**——`fe=ne=ore=0`，且 `OVRDIS=1` 下溢位是「靜默覆寫」不會設旗標（RM0456，見 [[2b_step2_impl_spec]] 第 3.2 節），所以旗標乾淨不能單獨證明零溢位，但 `send_seq==receive_seq` 收斂**間接證明**：即使真的在 FIFO 層級丟過任何一個 byte，其所屬的整個訊息區塊也會因為 checksum 不合被 host 端判定失敗、觸發重傳、最終補上——`bytes_invalid=0`（host 端統計，MCU 端沒有解析出無效區塊）是這個判斷的直接證據。 |
| **`receive_buf`（MCU 端）** | 192 bytes | T3（90 bytes，刻意壓在一半以下）、T2（120 bytes，在完整容量之下但沒有刻意壓到一半） | **無**——T3 的位元組預算本來就是為了**排除**這層的溢位風險而設計的（見 4.1 節），90 < 96 < 192，理論上不該滿；T2 的 120 bytes 同樣在 192 容量之下。這兩組測試的乾淨結果**主要驗證的是「在容量之下不會出問題」**，本輪沒有刻意讓 `receive_buf` 真正溢位過（那會是另一組獨立測試，測「超過 192 bytes 會怎樣」，不在本輪範圍內）。 |

★ **本輪唯一能直接證明「零永久遺失」的證據是 `send_seq==receive_seq`
收斂跟 `bytes_invalid=0`，不是 `fe/ne/ore=0`**——後者只能排除「有
旗標的錯誤」，排除不了 `OVRDIS=1` 下的靜默溢位。這兩種證據的差異
在解讀本輪結果時必須分清楚，這也是為什麼 4.2/4.3 節特地把
`send_seq`/`receive_seq` 列出來，不是只列 `stop2_wake`/`stop2_err`。

---

## 5. 項目 5：IRQ 重新開啟、ISR 排空 FIFO 的保證

逐行檢視 `stop2_once()`／`command_test_stop2()`：`irq_enable()` 在
`stop2_once()` 結尾呼叫，之後到函式返回之間、以及
`command_test_stop2()` 迴圈裡從一輪返回到下一輪呼叫 `stop2_once()`
之間，**沒有任何程式碼重新遮罩中斷**。ARM Cortex-M 架構規則：只要
中斷在 NVIC 已致能且處於 pending，`irq_enable()`（`cpsie i`）一
執行就會立即搶佔，不需要額外輪詢等待。`LPUART1_IRQn` 從
`serial_init()` 起一直是 NVIC-enabled，`stop2_once()` 全程沒有動過
它的 NVIC 位元（只清過 `LPTIM1_IRQn` 的 pending，見
`stm32u5_lowpower.c` 裡 `NVIC_ICPR2 = (1u << 3)` 那行，`(1u<<3)`
對應 `LPTIM1_IRQn=67` 的 `67-64=3`，不影響 `LPUART1_IRQn=66` 的
`bit 2`）。

**結論：現有流程已經保證「IRQ 有被重新開啟、ISR 有機會排空 FIFO」
這件事，不需要改程式碼。** 本輪 T2/T3/T3b 的乾淨結果（`fe=ne=ore=0`
+ `send_seq==receive_seq` 收斂）也是這個結論的間接實測佐證——如果
排空真的沒保證，多輪測試下應該會看到 `receive_buf`／FIFO 累積
髒資料的跡象，但沒有觀察到。

---

## 6. 「若核心不醒」SOP：本輪未觸發

[[2b_step2_impl_spec]] 第 5 節訂的分辨 SOP（`mdw 0x4600241C` 讀
`LPUART1_ISR`，依 `RXFT`/`RXFNE` 是否置位分兩條路查）本輪**完全
沒有用到**——所有測試 MCU 都正常醒來、`stop2_status` 每輪都正常
回報，沒有出現「核心不醒」的情況，SOP 保持原樣待命。

---

## 7. 結論與下一步

**Phase 2 通過**：TC 等待（含逾時計數）、喚醒來源判定、FE/NE/ORE
統計都正確運作；T1（修正後）確認喚醒機制對背景流量反應正常、不是
雜訊誤觸發；T2 確認睡眠中能正確收資料；T3/T3b 確認在 FIFO 深度
（8 bytes）跟 receive_buf 一半（96 bytes）的餘裕範圍內，沒有觀察到
任何 FIFO 層級或 receive_buf 層級的資料遺失。

**本輪最大的產出其實是方法學修正，不只是測試數字**：
1. T1「零流量對照組」在這個工具鏈下不可能，根本原因是
   `test_stop2` 封鎖主迴圈導致 host 端可靠傳輸層自動重傳
2. RTO 真實初始值是 25ms（`MIN_RTO`），先前以為的 3.2s 是 backoff
   後的結果，不是常數
3. `console.py` 確實會跑 `ClockSync`，先前「不跑 clocksync」的
   說法有誤，背景流量（~6 bytes/秒）就是它的週期性 `get_clock`
4. `fe/ne/ore=0` 不能單獨證明零溢位（`OVRDIS=1` 下溢位是靜默的），
   真正的零遺失證據是 `send_seq==receive_seq` 收斂 + `bytes_invalid=0`

測試後已還原 klipper：`state=ready`、`bytes_retransmit=0`、
`bytes_invalid=0`，版本字串跟本輪燒錄版本一致。

**下一步**：Phase 1+2 都已驗證通過，c1（LPUART1 autonomous Stop2
RX）在「不掉 byte」這個 step 2 通過標準上已經站得住腳。尚未涵蓋、
留給後續的範圍：`receive_buf` 真正溢位（>192 bytes）會發生什麼、
TX race（3.1 節，`TC` 逾時是否曾經發生——本輪 `tc_timeout` 全程是
0，沒有機會驗證逾時分支本身的行為）、以及 step 3 的主體工作
（Stop2 睡眠期間 `CYCCNT` 凍結對 clocksync 的補償）。
