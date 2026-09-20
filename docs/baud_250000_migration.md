# LPUART1 鮑率遷移：115200 → 250000

延續 [[stop2_2a_watchdog_fix]] 分支（`e9985ad2`）。目的：把 `[mcu]` 序列埠鮑率從
115200 改到 250000，驗證論文公式 (1) `T = (N×10)/Baudrate` 的延遲主張
（N=10、250000 bps → 0.4 ms，「主從通訊延遲 < 0.5 ms」）能否在硬體上成立。

改前：`T = (10×10)/115200 = 0.868 ms` —— 論文主張**不成立**。
改後：`T = (10×10)/250000 = 0.400 ms` —— 論文主張**成立**。

---

## 1. 前置：確認 BRR 公式（gate，過不了就停）

`src/stm32/stm32f0_serial.c:99-104` 中 `CONFIG_STM32_SERIAL_LPUART1` 分支定義了
`LPUART_BRR`：

```c
#elif CONFIG_STM32_SERIAL_LPUART1
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PG8,PG7");
  #define GPIO_Rx GPIO('G', 8)
  #define GPIO_Tx GPIO('G', 7)
  #define USARTx_FUNCTION GPIO_FUNCTION(8)
  #define USARTx LPUART1
  #define USARTx_IRQn LPUART1_IRQn
  #define USARTx_IRQHandler LPUART1_IRQHandler
  #define LPUART_BRR 1
#endif
```

`serial_init()`（同檔 170-188 行）依此巨集選公式：

```c
#if defined(LPUART_BRR)
    USARTx->BRR = DIV_ROUND_CLOSEST((uint64_t)pclk * 256, CONFIG_SERIAL_BAUD) & 0xFFFFF;
#else
    uint32_t div = DIV_ROUND_CLOSEST(pclk, CONFIG_SERIAL_BAUD);
    USARTx->BRR = (((div / 16) << USART_BRR_DIV_MANTISSA_Pos)
                   | ((div % 16) << USART_BRR_DIV_FRACTION_Pos));
#endif
```

板子目前用的是 `LPUART1`（`.config` 中 `CONFIG_STM32_SERIAL_LPUART1=y`），走的是
**256 倍版本**（`pclk × 256 / baud`），不是 USART 的整除版本。Gate 通過。

理論值：`LPUARTDIV = 256 × 16,000,000 / 250,000 = 16,384 = 0x4000`（HSI16 16MHz，
整除、理論誤差 0%）。

---

## 2. `.config` 變更

```diff
--- configs/unoq_klipper_115200.config
+++ configs/unoq_klipper_250000.config
@@ -110 +110 @@
-CONFIG_SERIAL_BAUD=115200
+CONFIG_SERIAL_BAUD=250000
```

只有這一行差異。`CONFIG_SERIAL_BAUD` 原本就寫在檔案裡（非預設值缺項），直接
`sed` 替換，不需要 append。

---

## 3. 建置與燒錄

```
$ make clean && make
...
  Compiling out/src/stm32/stm32u5_lowpower.o
Version: v0.13.0-474-ge9985ad22
  Linking out/klipper.elf
  Creating bin file out/klipper.bin

$ size out/klipper.elf
   text    data     bss     dec     hex filename
  35987      52    1136   37175    9137 out/klipper.elf
```

`text=35987`，在 35000-50000 範圍內。`stm32u5_lowpower.o` 有編譯進去（確認不是
Stop2 milestone code 被意外排除的舊版 `.config`）。

```
$ sudo openocd -f /home/arduino/uno_q.cfg -c "program out/klipper.bin verify reset exit 0x08000000"
...
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **
```

---

## 4. BRR 暫存器讀回值

位址核算（板上 CMSIS header
`framework-arduinoststm32/.../stm32u585xx.h`）：

```
PERIPH_BASE_NS      = 0x40000000
APB3PERIPH_BASE_NS  = PERIPH_BASE_NS + 0x06000000 = 0x46000000
LPUART1_BASE_NS     = APB3PERIPH_BASE_NS + 0x2400 = 0x46002400
USART_TypeDef.BRR   = offset 0x0C
→ LPUART1->BRR 位址  = 0x4600240C
```

### 4.1 第一次嘗試：讀值失誤（記錄下來，因為原因本身有價值）

最初用 `openocd ... -c "init; mdw 0x4600240C; shutdown"`（任務原定命令，三個
指令塞在同一個 `-c` 字串裡）完全沒有輸出——不是暫存器的問題，是
**openocd 的 stdout 在同一個 `-c` 批次內不會逐行 flush，只有拆成獨立的
`-c` 參數（`-c "cmd"` `-c "shutdown"`）才會可靠印出**。拆開後才看到輸出，
但當時又疊加了另一個問題：先 `reset halt`，會在 reset vector 附近就把 CPU
停住，`serial_init()` 根本還沒執行到，讀回 `0x00000000` 不代表暫存器設定
錯誤，只代表讀取時機在初始化之前。

這個判斷一度被錯誤地歸因為「Stop2 週期性睡眠犧牲 LPUART1」，經使用者更正：
`stop2_lowpower_init()` 的 boot-time 呼叫在更早的版本就已移除（cold boot
不會碰 Stop2 機制），Stop2 只在 host 下 `test_stop2` command 時才會進入，
檔頭那段警告是「測試步驟注意事項」，不是主迴圈行為描述。

### 4.2 正確序列：先讓它開機跑一下，再 halt 讀

```
$ sudo openocd -f /home/arduino/uno_q.cfg \
    -c "init" -c "reset run" -c "sleep 2000" -c "halt" \
    -c "mdw 0x4600240C" -c "mdw 0x46020CA8" -c "mdw 0x46002400" \
    -c "resume" -c "shutdown"
...
0x4600240c: 00004000
0x46020ca8: 00000040
0x46002400: 0000002d
```

| 暫存器 | 位址 | 讀值 | 判讀 |
|---|---|---|---|
| `LPUART1->BRR` | `0x4600240C` | `0x00004000` | 與理論值 `0x4000` 完全一致 |
| `RCC->APB3ENR` | `0x46020CA8` | `0x00000040` | bit6 `LPUART1EN`=1，時脈已開 |
| `LPUART1->CR1` | `0x46002400` | `0x0000002D` | `UE\|RE\|TE\|RXNEIE` = `0x1+0x4+0x8+0x20=0x2D`，與 `stm32f0_serial.c` 的 `CR1_FLAGS` 定義完全吻合 |

---

## 5. 功能性驗證（端到端）

`printer.cfg` 的 `[mcu] baud:` 改為 `250000`，啟動 `klipper.service`：

```
$ curl -s http://localhost:7125/printer/info
{"result":{"state":"ready", ..., "software_version":"v0.13.0-474-ge9985ad22", ...}}

$ curl -s "http://localhost:7125/printer/objects/query?mcu" | python3 -m json.tool
"mcu_constants": { ..., "SERIAL_BAUD": 250000, ... },
"last_stats": {
    "bytes_retransmit": 0,
    "bytes_invalid": 0,
    "send_seq": 143,
    "receive_seq": 143,
    "srtt": 0.002,
    "rttvar": 0.0,
    "rto": 0.025,
    "freq": 160553050
}
```

### 改前 / 改後對照

| 項目 | 115200（改前） | 250000（改後） |
|---|---|---|
| `state` | ready | ready |
| `SERIAL_BAUD` | 115200 | 250000 |
| `srtt` | 0.003–0.004（[[stop2_2a_watchdog_fix]] 記錄的實測值為 `0.004`） | 0.002 |
| `bytes_retransmit` | 0 | 0 |
| `bytes_invalid` | 0 | 0 |
| 連續運行紀錄 | `e9985ad2` 在 115200 下曾連續運行 70.6 小時，`send_seq=receive_seq=258,440`，`retransmit=invalid=0` | 見下方 30 分鐘取樣 |

判準全數通過：`state=ready`、`SERIAL_BAUD=250000`、`bytes_retransmit=0`、
`bytes_invalid=0`、`srtt` 降低（0.004 → 0.002）。

---

## 6. 30 分鐘穩定性觀察（每 5 分鐘取樣，共 6 次）

啟動 `klipper.service` 後每 5 分鐘取樣一次 `printer/objects/query?mcu`，
6 次全部完成，過程中 `bytes_retransmit`/`bytes_invalid` 全數為 0：

| # | 時間 (UTC) | srtt | rttvar | bytes_retransmit | bytes_invalid | send_seq | receive_seq | freq |
|---|---|---|---|---|---|---|---|---|
| 1 | 2026-09-20T15:08:34Z | 0.002 | 0.0 | 0 | 0 | 464 | 464 | 160563877 |
| 2 | 2026-09-20T15:13:35Z | 0.002 | 0.0 | 0 | 0 | 769 | 769 | 160551131 |
| 3 | 2026-09-20T15:18:35Z | 0.002 | 0.0 | 0 | 0 | 1074 | 1074 | 160538489 |
| 4 | 2026-09-20T15:23:36Z | 0.002 | 0.0 | 0 | 0 | 1380 | 1380 | 160548876 |
| 5 | 2026-09-20T15:28:37Z | 0.002 | 0.0 | 0 | 0 | 1685 | 1685 | 160553866 |
| 6 | 2026-09-20T15:33:37Z | 0.002 | 0.0 | 0 | 0 | 1990 | 1990 | 160562403 |

30 分鐘窗口內 `srtt` 穩定維持在 `0.002`（無波動），`send_seq=receive_seq`
在每一筆都相等（無漏封包待確認），`bytes_retransmit`/`bytes_invalid`
全程為 0。`freq`（MCU 對 host 回報的 SYSCLK 估計值）在
160,538,489–160,563,877 Hz 之間微幅浮動（約 0.016%），屬 HSI16 正常抖動
範圍，與鮑率變更無關。

---

## 7. 延遲模型：實際數值

```
改前 T = (10×10)/115200 = 0.868 ms   ← 論文「< 0.5 ms」主張不成立
改後 T = (10×10)/250000 = 0.400 ms   ← 論文「< 0.5 ms」主張成立
```

`LPUARTDIV = 256 × 16,000,000 / 250,000 = 16,384 = 0x4000`，HSI16 16MHz 下整除，
理論量化誤差為 0%（`BRR` 實測值與理論值位元對位元相符，見第 4 節）。

---

## 8. 論文需要同步更正的地方

1. 摘要與公式 (1) 段落聲稱的「< 0.5 ms」延遲，在**原始韌體設定**
   （115200 bps）下不成立（實際 0.868 ms），必須註明所使用的鮑率設定，
   否則讀者會誤以為預設配置即滿足此延遲界限。
2. 若論文的延遲主張是基於 250000 bps 的假設配置，應在方法論章節明確寫出
   `CONFIG_SERIAL_BAUD=250000` 是**非預設值**、需要手動修改 `.config` 才能
   達成，並附上本文件第 2 節的 diff 作為可重現步驟。
3. 應補充「`.config` 預設是 gitignore 的」這件事對可重現性的影響（見下一節），
   並在論文的可重現性附錄中指向 `configs/unoq_klipper_250000.config`。
4. 公式 (1) 本身（`T=(N×10)/Baudrate`）未考慮 USB/序列層以外的排隊延遲、
   `srtt`/`rto` 動態調整、或韌體端任務排程延遲，論文若要以此公式代表
   「端到端主從通訊延遲」，應註明這只是**單一 UART 訊框的傳輸時間下界**，
   不是應用層往返延遲的全貌（`srtt` 實測 0.002s = 2ms，遠大於 0.4ms 的
   訊框傳輸時間，兩者是不同量級的東西，不應混用）。

---

## 9. 我無法判定的事

1. **30 分鐘窗口之外的長期穩定性**：本次只驗證了 30 分鐘（見第 6 節）。
   115200 曾有 70.6 小時連續運行且零錯誤的紀錄，250000 沒有等量的長期
   數據，不能斷言兩者在同等時長下表現相同。
2. **板載走線在 250000 下的實體裕度**：理論上 `LPUARTDIV` 整除、誤差 0%，
   但這只涵蓋時脈解析度誤差，不涵蓋走線長度、串擾、邊沿速率等實體層因素
   在更高鮑率下的餘裕，本次測試沒有做訊號完整性量測（示波器/邏輯分析儀），
   只能以 `bytes_retransmit`/`bytes_invalid` 這種軟體層指標間接推論。
3. **`srtt` 從 0.004 降到 0.002 這個比較不是同批次量測**：115200 的
   `0.004` 取自 [[stop2_2a_watchdog_fix]] 文件中另一輪測試的快照（同一份
   `e9985ad2` 韌體、同一塊板子），不是本次改動前後在完全相同條件下的
   A/B 對照。若要嚴謹對照，應在同一次 session 內先跑 115200 取樣、再切
   250000 取樣。
4. **openocd `mdw` 輸出 buffering 的确切機制**：觀察到「多個 `mdw` 塞進
   同一個 `-c` 字串時只有最後一個會印出來，拆成獨立 `-c` 才穩定」，但沒有
   深入 openocd 原始碼確認這是 Jim/Tcl 的 command batching 行為還是
   ssh pty buffering 造成，只記錄現象與繞過方法。
