# Milestone 2B step 2 — T4：receive_buf 刻意溢位測試

延續 [[2b_step2_stage2_result]] 第 4 節的 FIFO 層級 vs receive_buf
層級初步區分。本輪**沒有改韌體**（Phase 2 二進位檔不變），純粹用
更大流量把 `receive_buf` 刻意打滿，定位失效邊界、確認掉包發生在
哪一層、觀察 Klipper 重傳機制能不能完全復原。

過程中發現一個原本沒預期到的機制性問題（host 端流量封裝/流控），
以及一個對「`bytes_invalid` 到底量什麼」的重要澄清——這兩點都直接
影響怎麼解讀 T4-2 的數字，先寫在最前面。

---

## T4-0：事實查證（唯讀，附檔名行號）

### `receive_buf` 大小與溢位行為

`src/generic/serial_irq.c`：

```c
17: #define RX_BUFFER_SIZE 192
19: static uint8_t receive_buf[RX_BUFFER_SIZE], receive_pos;
...
26: void
27: serial_rx_byte(uint_fast8_t data)
28: {
29:     if (data == MESSAGE_SYNC)
30:         sched_wake_tasks();
31:     if (receive_pos >= sizeof(receive_buf))
32:         // Serial overflow - ignore it as crc error will force retransmit
33:         return;
34:     receive_buf[receive_pos++] = data;
35: }
```

**大小：192 bytes**（跟 MCU 常數 `RECEIVE_WINDOW=192` 一致，先前已
確認過）。**溢位行為：丟棄新資料，不覆蓋舊資料**（行 31-33，
`receive_pos >= sizeof(receive_buf)` 時直接 `return`，完全不寫入）
——設計上刻意讓溢位造成的後果是「這個 message block 的 CRC 一定會
對不上」，靠既有的 CRC+重傳機制復原（註解原文：「ignore it as crc
error will force retransmit」），不是額外設計一套溢位偵測/通知
機制。

`receive_buf` 只有在 `console_task()`（`DECL_TASK`，只有排程器主
迴圈跑到才會執行）呼叫 `command_find_block()`/`console_pop_input()`
時才會被清空——`test_stop2` 執行期間主迴圈整個被佔住，
`console_task()` 不會被排到，`receive_buf` 在整個測試期間只進不出。

### 一筆 `get_uptime` 上行封包的實際位元組數——比預期複雜

沿用先前算 `test_stop2`/`get_clock` 的方法（`out/klipper.dict` 查
command-id，VLQ 編碼＋5 bytes 框架），單獨一筆 `get_uptime`
（command-id=4，零參數）算出來是 6 bytes。**但這個數字只在訊息
「有間隔地個別送出」時成立**（先前用 `get_clock` 在 0.9839s 週期下
驗證過：120 bytes / 20 筆 = 精確 6 bytes/筆）。

本輪 T4-1 一開始沿用「N×6 bytes」估算，結果發現嚴重偏差——連續
無間隔送出時，host 端的 `serialqueue`（`build_and_send_command()`，
`klippy/chelper/serialqueue.c`）會把多筆指令**封裝進同一個 message
block** 共用同一份 5-byte 框架，`MESSAGE_MAX=64`
（`klippy/chelper/msgblock.h:8`，`MESSAGE_HEADER_SIZE=2`+
`MESSAGE_TRAILER_SIZE=3`，行 9-10）——一個 block 最多能塞下 59 bytes
的內容，`get_uptime` 內容只有 1 byte，理論上一個 block 最多可以塞
59 筆。

實測（見 T4-1）：64 筆送出後 `Δbytes_write=147`（2.30 bytes/筆），
280 筆送出後 `Δbytes_write=389`（1.39 bytes/筆，封裝效率隨筆數增加
而提升，符合「框架成本被更多筆數攤提」的預期）。**本輪之後全部改用
`STATS` 前後直接量測 `bytes_write` 差值，不再用「筆數×6」推算**，
這是本輪跟先前 T2/T3/T3b 一個方法學上的差異，如實記錄：
[[2b_step2_stage2_result]] 第 4 節 T3/T3b 用「筆數×6」訂的位元組
目標（90/12 bytes）實際送上線的位元組數可能比預估更少（因為同樣是
連續無間隔送出，會被封裝）——**不影響那兩組測試「沒觀察到遺失」的
結論本身**（送更少位元組更容易通過，不會製造假陽性的 PASS），但
目標位元組數本身沒有先前文件講的那麼精確，這裡一併更正記錄。

---

## T4-1：對照組（MCU 清醒，N=280，經校準達到約 2× `receive_buf`）

先用 N=64 試跑，量到 147 bytes，不夠；加大到 N=175 量到 274 bytes；
最終 **N=280** 量到 **389 bytes**（≈2.03× `receive_buf`），採用這個
當最終 N。

```
測試前 STATS：bytes_write=843  bytes_retransmit=0  bytes_invalid=0  send_seq=99
測試後 STATS：bytes_write=1232 bytes_retransmit=0  bytes_invalid=0  send_seq=120  receive_seq=120
```

`Δbytes_write=389`，`Δsend_seq=21`（block 數）。**`bytes_retransmit=0`、
`bytes_invalid=0`、`send_seq==receive_seq` 全程收斂**——MCU 醒著、
主迴圈持續運作時，即使瞬間送出約 2 倍 `receive_buf` 容量的資料，
`console_task()` 追得上（每收到一個完整 block 就會被排空一次），
**完全沒有遺失或錯誤**。這證明線路本身、host 端封裝/編碼邏輯都沒有
問題，T4-2 如果出現任何異常，可以排除「本來就有問題」這個可能性，
差異只會來自「主迴圈被阻塞」這件事本身。

---

## T4-2：溢位組（MCU 睡眠、主迴圈被阻塞，同樣 N=280）

`test_stop2 period_ms=400 cycles=10`（10 輪，實測全程約 7-8 秒），
在指令送出後立刻背靠背送出同樣 280 筆 `get_uptime`，期間多次查
`STATS` 追蹤收斂過程。

### 時間序列（各次 `STATS` 查詢結果）

| 時間點 | `bytes_write` | `bytes_retransmit` | `bytes_invalid` | `send_seq` | `receive_seq` | 是否收斂 | `rto` |
|---|---|---|---|---|---|---|---|
| 測試前（基準） | 851 | 0 | 0 | 107 | 107 | — | 0.025 |
| 送完 280 筆、test_stop2 仍在跑（~1s 後） | 950 | 10 | 0 | 120 | 108 | ✗（差 12） | 0.050 |
| test_stop2 剛結束（~8s 後，`stop2_wake`/`stop2_err` 剛印出） | 1142 | 556 | **0** | 123 | 120 | ✗（差 3） | 3.200 |
| +10 秒 | 1281 | 749 | **0** | 133 | 133 | **✓ 已收斂** | 0.025 |
| +15 秒（再確認） | 1371 | 749（不再增加） | **0** | 148 | 148 | ✓ 維持收斂 | 0.025 |

**MCU 端遙測**（`test_stop2` 結束時的 `sendf`）：

```
stop2_wake n=10 by_uart=5 by_lptim=5 both=0 tc_timeout=0
stop2_err  fe=0 ne=0 ore=0
```

### 收斂時間

從「test_stop2 結束、送出還沒收斂」（差 3 個 block）到「完全收斂」，
發生在中間那次查詢之間的某個時間點，落在**該次查詢後 10 秒的視窗
內**（沒有更細的取樣點，本輪的量測粒度就是這樣，如實記錄，沒有再
往下切更細）；比對 `rto` 的變化（3.200 → 0.025，代表期間至少發生過
一次成功的 ACK 讓 backoff 重置回 `MIN_RTO`），推算收斂應該發生在
test_stop2 結束後、下一次 retransmit timer 到期時（`rto=3.200` 那次
查詢當下的剩餘等待時間之內），也就是**大約 test_stop2 結束後的
3~4 秒內**，但這是推算不是直接量到的精確時刻。

### 關鍵澄清：`bytes_invalid` 量的是哪個方向

★ **本輪查 `serialqueue.c` 才確認：`bytes_invalid` 只統計
「host 收到的、來自 MCU 的回應」裡面 CRC 失敗或序號異常的部分
（行 263、367，`handle_message()`／輸入解析路徑），跟「MCU 有沒有
成功收到 host 送出的指令」完全是两回事。** `ready_bytes`/
`upcoming_bytes`（行 53、83、1043）也是 host 端**自己要送出**的
佇列記帳，跟 MCU 的 `receive_buf` 無關。

**這代表本輪測試沒有、也不可能直接量到「`receive_buf` 到底有沒有
真的溢位」這件事**——韌體本身沒有計數溢位發生次數的遙測（`serial_
rx_byte()` 溢位時只是單純 `return`，沒有累加任何欄位），要新增這種
遙測需要改韌體，超出本輪「Phase 2 韌體不動」的範圍。

### 那 `bytes_invalid=0` 代表什麼？

代表「host 端全程沒有收到任何一個 CRC 失敗或序號異常的**回應**」。
搭配 `serial_irq.c` 的溢位設計本身（行 32 的註解）：如果
`receive_buf` 真的溢位、把某個 block 的中段位元組丟了，
`command_find_block()` 會**整個 block CRC 失敗**（不是部分接受、
部分失敗）——這種情況下 MCU **不會**針對那個壞掉的 block 產生 ACK
（也就不會產生任何回應讓 host 判定 CRC 失敗，因為根本沒有回應可以
去驗證失敗與否），host 端看到的只是「這個序號遲遲沒被 ACK」→逾時
→重傳整個 block→重傳的版本這次沒有跟溢位窗口重疊、順利被完整收下
→ACK→收斂。**這條「溢位→整個 block 作廢→重傳→復原」的路徑，從
host 的角度看起來就是單純的「retransmit 次數變多、最終還是收斂」，
不會產生 `bytes_invalid`**——所以 `bytes_invalid=0` 不代表
`receive_buf` 沒有溢位，只代表「不管有沒有溢位，最終復原的方式沒有
產生 host 端偵測到的回應層級損壞」。

**能直接證實的是**：`send_seq==receive_seq` 最終收斂＋
`bytes_retransmit` 大幅上升（0→749，遠超過 T4-1 對照組的 0）——這組
數字本身就是「有東西被擋住、必須重試才能過關，但最終每一筆都真的
過關了」的完整證據鏈，不需要額外的 `bytes_invalid` 佐證。**溢位
發生與否的直接證據，本輪拿不到**（要拿到需要改韌體加計數器，超出
範圍），但溢位造成的**後果**（大量 retransmit、暫時性 block 級別
遺失後又復原）**有明確證據**。

### MCU 端 FIFO 層級的旗標：全程乾淨

`fe=0 ne=0 ore=0`——即使 host 端統計顯示大量重傳活動，**LPUART 硬體
層級的錯誤旗標全程沒有出現任何一次**。這跟 4.5 節（下方）的分層
論證直接相關：`receive_buf` 溢位是純軟體行為（`serial_rx_byte()`
自己判斷、自己 `return`），完全不會觸發任何 LPUART 硬體錯誤旗標
（`FE`/`NE`/`ORE` 都是實體收訊訊號層級的異常，根跟軟體緩衝區有沒有
滿沒有關係）——`fe=ne=ore=0` 搭配大量 `bytes_retransmit`，正是
「問題出在 `receive_buf` 這一層、不是 FIFO 這一層」最直接的旗標
證據。

---

## T4-3：邊界掃描——本輪略過，原因記錄

原計畫掃 0.5×/1×/2×/4× buffer 找 `bytes_invalid` 開始出現的門檻。
**T4-2 的發現讓這個計畫的前提站不住腳**：上一節已經論證
`bytes_invalid` 這個指標，依照目前的協定設計（block 級 CRC、失敗
整個作廢重傳），**可能永遠不會因為 `receive_buf` 溢位而變成非零**
——不管溢位多少倍，只要重傳機制還在正常運作，最終看到的都是
「retransmit 變多、但 `bytes_invalid` 維持 0」這個模式，掃描
「`bytes_invalid` 開始出現的門檻」很可能掃不到任何有意義的邊界
（除非把 host 端逼到 `MAX_RTO`／重傳次數上限而直接放棄連線，那是
另一種性質的失效模式，不是「找出溢位門檻」）。

要真正掃出「溢位開始造成實質影響」的門檻，需要換一個可觀測量——
例如改韌體加一個 `receive_buf` 溢位計數器（超出本輪「不改韌體」的
範圍），或者去量「完全收斂需要多久」隨溢位倍數增加的變化趨勢（用
收斂時間當代理指標，而不是找一個二元的「開始出現」門檻）。**本輪
判斷：與其在錯的指標上掃描出可能沒有意義的「門檻」，不如如實記錄
這個方法學限制，留給下一輪決定要不要改韌體加計數器再做**，不強行
湊出一個數字。

---

## 三層緩衝的區分與各自容量

| 層級 | 容量 | 誰在管 | 溢位行為 | 本輪相關證據 |
|---|---|---|---|---|
| **LPUART1 硬體 FIFO** | 8 bytes | 硬體（`RXFTCFG` 門檻式喚醒，`OVRDIS=1`） | 滿了之後新資料靜默覆寫（`OVRDIS=1` 讓 `ORE` 永遠不會被設，RM0456，[[2b_step2_impl_spec]] 已引用） | [[2b_step2_stage2_result]] T2/T3b：喚醒延遲（121.6~180.9µs）遠小於這一層的餘裕（本輪 T4 沒有針對這層加壓，維持先前結論） |
| **`receive_buf`（MCU 端，軟體）** | 192 bytes（`RX_BUFFER_SIZE`，`serial_irq.c:17`） | 軟體（`serial_rx_byte()`／`console_task()`） | 滿了之後新資料被**丟棄**（不覆寫，`serial_irq.c:31-33`），造成該 block CRC 失敗 | **本輪 T4-2 的主要對象**：`test_stop2` 封鎖主迴圈期間 `receive_buf` 沒有任何清空機會，`bytes_retransmit` 從 0 飆到 749，收斂前有明顯延遲 |
| **host 端可靠傳輸佇列**（`serialqueue`） | `MAX_PENDING_BLOCKS=12` 個未 ACK block（`serialqueue.c` 定義，[[2b_step2_stage2_result]] 提過） | host 端軟體 | 達到上限時暫停送出新 block，累積在 `upcoming_bytes`（host 本地佇列，不上線） | T4-2 觀察到 `upcoming_bytes=251`（送完 280 筆當下，host 還有 251 bytes 排隊沒送出，因為已經有 12 個 block 未 ACK）——**這一層的流控會自然限制某個時間點內真正「打到線路上」的位元組數，不是所有排隊的資料都會立刻變成 `receive_buf` 的壓力** |

---

## 失效邊界的實測值，以及它由哪一層決定

**沒有量到一個乾淨的「開始出現 `bytes_invalid` 的位元組數門檻」**
（見 T4-3 略過的原因）。能確定的邊界是：

- `receive_buf`（192 bytes）在 T4-2 的 ~8 秒封鎖窗口內，被送入的
  實際位元組數（`Δbytes_write` 從封鎖開始到 test_stop2 結束，
  851→1142＝**291 bytes**）**已經超過容量的 1.5 倍**，觸發了大量
  `bytes_retransmit`（0→556，在 test_stop2 結束當下）——這個時間
  尺度（幾秒鐘的封鎖 vs 192 bytes 容量）明顯超出這一層能無損承受
  的範圍。
- 對照 T4-1：同樣約 389 bytes（≈2× 容量）瞬間送出，但 MCU **醒著**
  （`console_task()` 持續在跑），完全沒有 `bytes_retransmit`——
  代表**問題不是「兩倍容量的資料量」本身，而是「這段時間內完全沒有
  排空的機會」**。
- **結論：失效邊界不是由 `receive_buf` 的絕對容量單獨決定，是由
  「封鎖時間內流量是否超過容量」共同決定**，這正是下一節要推導的
  設計約束。

---

## Step 3 設計約束推導

**在 250000 baud 下，主迴圈被阻塞的時間上限（最壞情況、連續滿載
流量）**：

```
byte_rate = baud / 10 = 250000 / 10 = 25000 bytes/s
            （每 byte 10 bit period：1 start + 8 data + 1 stop）

t_block_max = RX_BUFFER_SIZE / byte_rate
            = 192 / 25000
            = 0.00768 s
            = 7.68 ms
```

**這是「host 端持續用滿速 250000 baud 灌資料」這個最壞情況下的
上限**——只要主迴圈被阻塞超過約 7.68ms，`receive_buf` 理論上就會
被灌滿，之後的資料開始被丟棄（觸發 CRC 失敗+重傳，如前述，不會
永久遺失，但會付出重傳延遲的代價）。

**跟實際 idle 流量的差距，必須寫清楚**：[[2b_step2_stage2_result]]
第 2.5 節量到的背景流量（`clocksync` 的 `get_clock`）只有**約
6 bytes/秒**：

```
t_idle_fill = RX_BUFFER_SIZE / (6 bytes/s) = 192 / 6 = 32 秒
```

**`7.68ms` 跟 `32 秒`，差了超過 4000 倍。** 這代表：

- 如果 step 3 的閒置排程設計只需要應付「正常心跳/查詢等級的背景
  流量」（跟本輪 T1/T2 的量級相近），Stop2 睡眠期間主迴圈被阻塞
  數秒鐘（跟 `test_stop2 period_ms=400 cycles=10` 差不多量級）**不
  會有實質風險**——32 秒的容忍窗口遠遠大於任何合理的單次 Stop2
  睡眠時長。
- 但如果 step 3 需要處理「host 端短時間內連續灌大量指令」的情境
  （例如列印過程中大量 G-code 指令背靠背送達），**7.68ms 這個
  最壞情況上限才是真正的設計紅線**——本輪 T4-2 用
  `test_stop2` 封鎖了數秒，遠遠超過這個紅線，才會看到明顯的
  retransmit 堆積；真實情境下如果封鎖時間能壓到 7.68ms 等級以下，
  即使滿速流量也不會觸發 `receive_buf` 溢位。
- **兩種情境的落差達 4000 倍，代表 step 3 的閒置排程設計必須先
  搞清楚「目標情境的流量特性」再決定要不要在意這個上限**——用
  `test_stop2` 這種秒級封鎖去逼近真實 Stop2 idle-sleep 的行為模式
  本身就已經是壓力測試等級的情境，不是常態。

---

## 明確結論

1. **本測試的掉包（重傳堆積，非永久遺失）屬於 `receive_buf` 層級，
   與 LPUART FIFO 餘裕無關**——`fe=ne=ore=0` 全程乾淨（FIFO 硬體
   層級沒有任何錯誤旗標），`bytes_retransmit` 大幅上升且最終收斂
   （`receive_buf` 軟體層級的容量+排空時機問題，透過 CRC+重傳協定
   復原）。這跟 [[2b_step2_stage2_result]] 第 4 節已經證明的
   「FIFO 層級零遺失」（T2/T3b）是同一個結論的另一半證據，兩層
   合起來構成完整的分層驗證。
2. **`bytes_invalid` 這個指標量測的是「host 收到的 MCU 回應」是否
   損壞，不是「MCU 收到的 host 指令」是否遺失**——本輪查證後修正了
   這個先前隱含的誤用，往後解讀這個欄位時要記得它的方向性。
3. **Klipper 的 block 級 CRC+重傳協定設計能完全復原 `receive_buf`
   溢位造成的影響**——T4-2 在遠超容量（291 bytes 進 192 bytes
   buffer）、遠超正常時長（~8 秒封鎖）的極端情境下，最終依然完全
   收斂（`send_seq==receive_seq`），沒有任何訊息永久遺失，只是付出
   了明顯的重傳延遲代價。
4. **T4-3（邊界掃描）略過，原因是原計畫的判準（`bytes_invalid`
   門檻）在目前的協定設計下可能永遠掃不到有意義的邊界**，需要改
   韌體加計數器才能做更精確的邊界量測，這個限制如實記錄，不強行
   湊數字。

---

## 收尾確認

測試後已還原 klipper：

```
state: ready
bytes_retransmit: 0
bytes_invalid: 0
mcu_version: v0.13.0-474-ge9985ad22-dirty-20260923_225414-hunter（跟 Phase 2 燒錄版本一致，本輪韌體完全沒有變動）
```
