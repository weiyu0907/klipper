# Milestone 2B step 2 — Phase 1 結果：6 小時 soak test

延續 [[2b_step2_impl_spec]]。Phase 1（`internal.h` 位元巨集、
`serial_init()` 的 `UESM`/`FIFOEN`/`RXFTCFG=000`/`RXFTIE`、ISR 排空
迴圈、`u5_main.c` 的 `APB3SMENR`/`SRDAMR` `LPUART1` 位元）已燒錄、
唯讀驗證、跑完 6 小時 soak test。**本輪沒有動 `stop2_once()`、沒有跑
`test_stop2`**——Phase 2（`stop2_once()` 加 TC 等待）留給下一輪。

---

## 1. 燒錄與設定驗證

**版本字串**：`v0.13.0-474-ge9985ad22-dirty-20260923_151124-hunter`
**對應 commit**：`9af6bc1b`

燒錄前確認正常通訊一段時間、`bytes_write` 持續增長（確定已經發生過
TX），停 klipper、`openocd halt` 後**只用 `mdw`**（沒有任何 `mww`）
讀回四個目標暫存器：

| 暫存器 | 讀回值 | 判讀 |
|---|---|---|
| `LPUART1_CR1`（`0x46002400`） | `0x2000002F` | bit29 `FIFOEN`=1 ✓、bit1 `UESM`=1 ✓ |
| `LPUART1_CR3`（`0x46002408`） | `0x10001000` | bit28 `RXFTIE`=1 ✓、bits27:25 `RXFTCFG`=`000` ✓、bit12 `OVRDIS`=1 ✓（設計決策 1，維持不變） |
| `RCC_APB3SMENR`（`0x46020CD0`） | `0xFFFFFFFF` | bit6 `LPUART1SMEN`=1 ✓（這顆暫存器看起來像是很多 SMEN 位元的 reset 預設就是 1，本輪沒有進一步查證這個推測，只確認我們要的 bit6 確實是 1） |
| `RCC_SRDAMR`（`0x46020CD8`） | `0x00000040` | bit6 `LPUART1AMEN`=1 ✓，其餘全 0——乾淨、獨立的證據，證實是這次的程式碼寫進去的，不是重置預設值 |

四項全部符合規格，`resume` 後重啟 klipper，`state=ready`，進入 soak
test。

---

## 2. 6 小時 soak test

**方法**：`~/stage1_soak.log`（PC 端），每 5 分鐘（300 秒）取樣一次，
硬上限 72 筆（72×5min=360min=6 小時），透過 SSH 查
Moonraker `GET /printer/objects/query?mcu` 跟 `GET /printer/info`，
記錄 `bytes_retransmit`/`bytes_invalid`/`bytes_write`/`bytes_read`/
`state`。任一筆 `retransmit≠0` 或 `invalid≠0` 或 `state≠ready` 會
立即停止取樣並觸發自動 rollback（回燒 commit `14fe967b`，也就是 G6
最後驗證過的版本）；連續 3 筆（15 分鐘）連不上 host 會停止取樣但
**不**觸發 rollback（連線問題不等於韌體問題，這種情況本輪沒有遇到，
純屬設計上的保險）。腳本本身有固定的 72 筆終止條件，跑完就結束，
沒有留下無終止的背景程序。

### 結果：**PASSED，72/72 筆全部乾淨，零異常**

```
起始：2026-09-23T15:18:43Z
結束：2026-09-23T21:19:38Z
總時長：6 小時 0 分 55 秒
```

| 項目 | 結果 |
|---|---|
| 取樣筆數 | 72/72（達到硬上限，正常結束，不是提早停止） |
| `bytes_retransmit` | **72 筆全部是 0**，沒有任何一筆非零 |
| `bytes_invalid` | **72 筆全部是 0**，沒有任何一筆非零 |
| `state` | **72 筆全部是 `ready`**，沒有任何一筆不是 |
| 連線失敗 | **0 次**（SSH/Moonraker 查詢全部成功，沒有觸發 3 連敗的連線中斷判定） |
| 是否觸發 rollback | **否** |

流量規模（同一組 6 小時期間，`last_stats` 累積值）：

```
bytes_write（MCU→host）：1443 → 131547，成長 130104 bytes
bytes_read（host→MCU） ：6375 → 407675，成長 401300 bytes
```

六小時內持續有實際雙向流量（不是靜止不動的空轉測試），`bytes_write`
的持續成長也確認 TX 路徑全程都在正常運作，跟 §1 燒錄前特意確認過
TX 已發生過的前提一致。

Soak test 結束後**立即**（非隔了很久之後）再查一次即時狀態，確認跟
最後一筆取樣一致，且 klippy `process_id` 全程沒有變化（`140516`，
跟 Phase 1 燒錄驗證後重啟時的 PID 相同）——代表 klippy daemon 本身
六小時內**沒有重啟過**，不是靠「daemon 掛掉自動重連」這種方式維持
`state=ready`：

```
state: ready
mcu_version: v0.13.0-474-ge9985ad22-dirty-20260923_151124-hunter（跟燒錄版本一致，沒有被 rollback）
bytes_retransmit: 0
bytes_invalid: 0
```

---

## 3. 結論

**Phase 1 通過**：`UESM`/`FIFOEN`/`RXFTCFG=000`/`RXFTIE` 這組
autonomous/FIFO 設定，加上 `CR1_FLAGS` 覆寫問題的修正（見
[[2b_step2_impl_spec]] 第 7 節），在 6 小時、72 筆取樣、約 13 萬
bytes TX／40 萬 bytes RX 的真實流量下，**沒有破壞現有 250000 baud
正常通訊**——這正是 Phase 1 存在的目的：先確認 FIFO/UESM 改動本身
是安全的，再進 Phase 2 動 `stop2_once()`（Stop2 喚醒行為），兩者
分開驗證，出問題時範圍容易鎖定。

**本輪嚴格遵守使用者的範圍限制**：沒有做 Phase 2、沒有碰
`stop2_once()`、沒有跑 `test_stop2`。Stop2 期間 LPUART1 能不能正確
喚醒 MCU、`RXFT` 門檻式喚醒在真正睡眠情境下的行為、TC 等待邏輯——
這些都還沒有驗證，是 Phase 2 的範圍。

**下一步**：Phase 1 通過，可以進 Phase 2——`stop2_once()` 加 `wfi`
前的 TC 等待（含逾時計數，透過 `test_stop2` 的
`stop2_tc_timeout n=%u tc_timeout=%u` 回報），並加 Stop2 期間收資料
的測試（見 [[2b_step2_impl_spec]] 第 0、2 節）。這一步需要使用者
另外批准才會開始。
