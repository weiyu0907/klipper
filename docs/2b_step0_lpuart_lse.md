# Milestone 2B 第 0 步 —— LPUART Stop2 喚醒時脈查證 + LSE 起振測試

延續 [[stop2_2a_final_measurements]]。目的：查清楚 LPUART1 在 Stop2 模式下
用什麼機制喚醒 MCU、決定 2B 該走「LSE + 9600 動態切換」(b) 還是「HSI16
按需喚醒維持 250000」(c)，並實測 LSE 起振是否可行。不改任何程式碼、不
make、不燒錄。

參考文件：`~/klipper/refs/rm0456.txt`（PC 本機，RM0456 Rev 7 純文字版）、
`~/klipper/refs/stm32u585xx.h`（CMSIS header）。所有位址已用 CMSIS header
核對過，與 openocd 實際讀值一致（見第 3 節）。

---

## 1. A 部分：RM0456 查表結果

### A1 ★（本任務最重要的一題）：Stop2 下 LPUART kernel clock=HSI16 時，
start bit 能否自動開啟 HSI16？

**結論：能，但有條件，且有另一個關鍵限制（見 A2）。**

RM0456 §11.4「RCC low-power modes」，「Peripherals clock gating and
autonomous mode in Stop 0/1/2 modes」小節（行 33705-33762）：

> 33711-33713: "Some peripherals support autonomous mode (refer to Table
> 116). These peripherals are able to generate a kernel clock request and
> a AHB/APB bus clock request when they need, in order to operate and
> update their status register even in Stop mode."
>
> 33746-33749: "**If an autonomous peripheral requests its kernel clock in
> Stop 0, Stop 1, or Stop 2 mode, the internal oscillator (HSI16 or MSI)
> is woken up if it was off**, and the kernel clock is propagated only to
> the peripheral requesting it. When the peripheral releases its kernel
> clock request, the HSI16 or MSI is switched off if no other peripheral
> requests it."

Table 116「Autonomous peripherals」（行 33742 起）把 **LPUART1** 列在
SmartRun domain（SRD），「Autonomous in Stop 2 mode」欄位是
**Yes(3)**，註 3：「Enabled if all xxEN, xxSMEN, and xxAMEN bits of the
peripheral are set」。

對應的暫存器位元：
- `RCC_APB3SMENR` 的 `LPUART1SMEN`（行 39384-39387）
- `RCC_SRDAMR`（11.8.45，行 39413-39414）的 `LPUART1AMEN`，bit 6（行
  39541-39544）：「LPUART1 autonomous mode enable in Stop 0/1/2 mode」
- `RCC_CCIPR3` 的 `LPUART1SEL[2:0]`（11.8.48，offset 0x0E8，行
  40085-40095）：`010` = HSI16 selected；行 40093-40095 明文：「**The
  LPUART1 is functional in Stop 0, Stop 1, and Stop 2 modes only when the
  kernel clock is HSI16, LSE, or MSIK.**」

★ 值得注意：同一頁裡 `LPTIM1SEL`/`LPTIM34SEL`（行 40047-40056）的對應
註解都額外要求「HSI16 with **HSIKERON = 1**」，但 `LPUART1SEL` 這條
（行 40093-40095）**沒有**這個 HSIKERON 前提子句——推測是因為 LPUART1
走的是上面 Table 116 的「autonomous 請求觸發自動喚醒」路徑，不需要
事先手動把 HSIKERON 設起來養著 HSI16；但 RM 沒有明講這個差異的原因，
只能從兩處寫法不同推論，不算直接證據，見第 4 節。

**結論**：只要把 `LPUART1SEL=010`（HSI16）、`LPUART1EN`/`LPUART1SMEN`/
`LPUART1AMEN` 三個 EN 位元都設起來、`UESM=1`，RM0456 明確支持
「start bit 觸發 kernel clock request → RCC 自動把 HSI16 從關閉狀態叫醒
→ kernel clock 只送給 LPUART1」這條路徑，**不需要**事先讓 HSI16 常駐
運轉。

### A2 ★：喚醒延遲會不會讓第一個 byte 遺失？

**RM 有公式跟範例數字，範例數字本身標明僅供參考，不是這顆晶片的精確值。**

§67.4.15「LPUART autonomous mode」之後的「Determining the maximum LPUART
baud rate that enables to correctly wake up the MCU from low-power mode」
（行 188020-188063）：

> 188030-188032: "The maximum baud rate that enables to correctly wake up
> the MCU from low-power mode depends on the wake-up time parameter
> (refer to the device datasheet) and on the LPUART receiver tolerance..."
>
> 188039-188042（公式）:
> ```
> DTRA + DQUANT + DREC + DTCL + DWU < LPUART receiver tolerance
> DWUmax = tWULPUART / (11 x Tbit Min)
> Tbit Min = tWULPUART / (11 x DWUmax)
> ```
> where tWULPUART is the wake-up time from low-power mode.
>
> 188050-188058（範例，非本晶片實測值）:
> "For example, if HSI is used as lpuart_ker_ck, and the HSI inaccuracy is
> of 1%, then we obtain: tWULPUART = 3 µs (values provided only as
> examples; for correct values, refer to the device datasheet).
> DWUmax = 3.41% – 1% = 2.41%
> Tbit min = 3 µs/ (11 x 2.41%) = 11.32 µs.
> As a result, the maximum baud rate that enables to wake up correctly
> from low-power mode is: 1/11.32 µs = 88.36 kbauds."

**這個範例數字（~88.36 kbaud）遠低於 250000 baud**（差約 2.8 倍）。RM
自己註明「values provided only as examples; for correct values, refer to
the device datasheet」——`refs/` 目錄裡沒有 datasheet（只有 RM 跟 CMSIS
header），所以**無法確認 STM32U585 實際的 tWULPUART 與 HSI16 inaccuracy
數值**，不能把 88.36 kbaud 當成這顆晶片的精確門檻。但這個公式本身
（喚醒時間 vs bit period 的競爭關係）是通用機制，方向上足以構成對
「方案 (c) 在 250000 baud 下直接靠 HSI16 從關閉狀態喚醒」的重大疑慮
——即使實際 tWULPUART 比範例的 3µs 更短，250000 baud 的 bit period 只有
4µs，容錯空間非常窄。

### A3：RCC_CCIPR3 的 LPUART1SEL[2:0] 編碼

行 40085-40092（11.8.48，`RCC_CCIPR3`，offset **0x0E8**，即
`0x46020C00+0x0E8=0x46020CE8`）：

```
Bits 2:0 LPUART1SEL[2:0]: LPUART1 kernel clock source selection
   000: PCLK3 selected
   001: SYSCLK selected
   010: HSI16 selected
   011: LSE selected
   100: MSIK selected
   others: reserved
```

### A4：RCC_BDCR 的 LSEDRV[1:0]、LSEON、LSERDY、LSESYSEN

11.8.49「RCC backup domain control register (RCC_BDCR)」，offset
**0x0F0**（= `0x46020CF0`，與任務給的位址一致）：

- 行 40257-40264：**Bits 4:3 LSEDRV[1:0]**：「LSE oscillator drive
  capability... It can be written only when the external 32 kHz
  oscillator is disabled (LSEON = 0 and LSERDY = 0).」
  `00`=Xtal 低驅動力、`01`=中低、`10`=中高、`11`=高驅動力。RM 沒有給
  「建議值」，只說要「according to crystal specification, to obtain the
  best compromise between robustness and short startup time... and
  low-power-consumption」（行 33284-33288）——沒有數值化的建議，找不到
  具體推薦檔位。
- 行 40272-40277：**Bit 1 LSERDY**：硬體設定/清除，表示外部 32kHz
  振盪器是否穩定；`LSEON` 清除後 `LSERDY` 要等 6 個外部低速振盪器時脈
  週期才會變低。
- 行 40280-40283：**Bit 0 LSEON**：軟體設定/清除，開關 LSE 振盪器。
- 行 40235-40242：**Bit 7 LSESYSEN**：「This bit is set by software to
  enable always the LSE system clock generated by RCC, which can be used
  by any peripheral when its source clock is the LSE...」：0=LSE 只能給
  RTC/TAMP/CSS 用；1=LSE 可以給其他周邊/功能用。

★ 重要補充（行 33298-33320，「LSE when used by peripherals other than
RTC/TAMP, and RCC functions」）：**LPUART1 要用 LSE 當 kernel clock，
光設 `LSEON`+`LSERDY` 不夠**，還要照這個順序：
```
1. Set LSEON in RCC_BDCR, and wait for LSERDY = 1 in RCC_BDCR.
2. Set LSESYSEN = 1 in RCC_BDCR.
3. Wait for LSESYSRDY = 1 in RCC_BDCR.
```
`LSESYSRDY` 是 bit 11（行 40203-40210）：「When LSESYSEN is set, this
LSESYSRDY flag is set after two LSE clock cycles.」——這是方案 (b) 未來
真的要接 LSE 到 LPUART1 時，必須多做的一步，本輪只測到 `LSEON`→
`LSERDY`，還沒做 `LSESYSEN`（見第 4 節「我無法判定的事」）。

### A5：LSE 起振時間的典型值/最大值

**找不到。** RM0456 只在 §11.4.7「LSE clock」（行 33276-33297）質化地
描述 `LSEDRV` 對「startup time」與「power consumption」的權衡，以及
`LSERDY` 是硬體用來指示穩定與否的旗標，**沒有給出任何秒數/毫秒數的典型
值或最大值**。這類數值屬於 datasheet 的 electrical characteristics 章節
（RM 行 33305-33306 談 LSI 時也明講「refer to the electrical
characteristics section of the datasheet for more details」），
`refs/` 目錄裡沒有 datasheet，查不到。

### A6：LPUART 最大鮑率與 kernel clock 的關係（LSE 32768Hz 上限）

兩處明文確認：

- §67.1「LPUART introduction」（行 186327-186328）：「Only a 32.768 kHz
  LSE clock is required to enable UART communications **up to 9600
  bauds**. Higher baud rates can be reached when the LPUART is clocked by
  clock sources different from the LSE clock.」
- §67.2「LPUART main features」（行 186348）：「From 300 bauds to
  **9600 bauds** using a 32.768 kHz clock source.」
- §67.4.11 附近（行 187290-187291）再次確認：「The maximum baud rate
  that can be reached when the LPUART clock source is the LSE, is
  **9600 bauds**.」

三處數字完全一致，`9600` 不是任意選的參數，是 RM 明文的硬上限。

---

## 2. ★ 結論：2B 該走方案 (b) 還是 (c)

**傾向方案 (b)「LSE + 9600 動態切換」，理由：**

1. A6 確認：LSE 32768Hz kernel clock 下，LPUART 鮑率硬上限就是 9600
   baud，這不是本輪測試選的參數，是矽片規格限制——**方案 (b) 的 9600
   這個數字本身沒有討論空間，是 RM 訂死的**。
2. A1 確認方案 (c) 的機制在 RM 層級是存在的（LPUART1 的 autonomous
   kernel-clock-request 可以把 HSI16 從關閉狀態叫醒），但 A2 的公式與
   範例數字顯示，**在 250000 baud 這個量級，喚醒延遲吃掉的容錯空間非常
   薄**（範例算出的 88.36 kbaud 上限只有 250000 的 35%）。雖然範例數字
   不是這顆晶片的精確值，但機制本身（`tWULPUART` vs `Tbit`）是通用的物理
   限制，不會因為查不到精確 datasheet 數字就消失——**250000 baud 下
   Stop2 喚醒的第一個 byte 有實質遺失/誤判風險，且目前沒有數據可以
   排除這個風險**。
3. 方案 (b) 的「動態切換」本身有額外成本（LSE 起振時間未知、切換鮑率
   需要重新設定 BRR 並確保雙邊同步），但這是**已知、可測量、可控**的
   成本；方案 (c) 的風險是**每次 Stop2 喚醒时第一個 byte 是否正確**，
   這種問題不容易在測試中穩定重現，一旦上線後在特定時序下觸發會很難
   除錯。
4. ★ 這個結論建立在 A2 的**通用公式**上，不是這顆晶片的**實測 tWULPUART
   數字**——如果日後找得到 STM32U585 的 datasheet，用真實數值重算一次
   公式，有可能推翻這個結論（例如如果真實 tWULPUART 遠小於 3µs 範例值，
   250000 baud 也許可行）。本輪的建議是**在拿到真實數據前，先假設風險
   存在，走比較保守的方案 (b)**。

---

## 3. B0-B5：LSE 起振硬體測試（已完成）

```
$ ssh arduino@192.168.40.120 'sudo systemctl stop klipper; pgrep -x openocd || echo NO_OPENOCD'
NO_OPENOCD

$ sudo openocd -f /home/arduino/uno_q.cfg \
    -c "init" -c "reset run" -c "sleep 2000" -c "halt" \
    -c "mdw 0x46020C94" -c "mdw 0x46020828" -c "mdw 0x46020CF0" \
    -c "resume" -c "shutdown"
...
0x46020c94: 80000004
0x46020828: 00000000
0x46020cf0: 0c000000
```

| 暫存器 | 位址 | 原始讀值 | 解讀 |
|---|---|---|---|
| `RCC_AHB3ENR` | `0x46020C94` | `0x80000004` | bit31 `SRAM4EN`=1（重置預設值）、bit2 `PWREN`=1——**PWR 時脈已經是開的**（韌體既有的 Stop2 低功耗程式碼已經啟用），B2 不需要再設這個位元 |
| `PWR_DBPR` | `0x46020828` | `0x00000000` | bit0 `DBP`=0，backup domain 目前是寫保護狀態，B2 需要設為 1 |
| `RCC_BDCR` | `0x46020CF0` | `0x0C000000` | bit27 `LSIRDY`=1、bit26 `LSION`=1（IWDG 用的 LSI 已經開著且穩定，符合既有 Stop2 watchdog 韌體的行為）；`LSEDRV[4:3]`=`00`（重置預設，最低驅動力）；`LSEON`(bit0)/`LSERDY`(bit1)/`LSESYSEN`(bit7) 全為 0——**LSE 目前完全沒開** |

B1 為純讀取，未寫入任何暫存器。這個讀值階段一度因為 `openocd mww`
寫暫存器動作被 Claude Code auto-mode 分類器擋下（歸類為「Modify Shared
Resources」）而暫停，使用者確認「B2 放行，繼續照 RMW 流程做」後，
以下 B2-B5 才執行。

### B2：嚴格 RMW（先讀、算、寫、讀回，不整顆覆寫）

```
$ sudo openocd -f /home/arduino/uno_q.cfg \
    -c "init" -c "halt" \
    -c "mdw 0x46020828" \
    -c "mww 0x46020828 0x00000001" \
    -c "mdw 0x46020828" \
    -c "mdw 0x46020CF0" \
    -c "mww 0x46020CF0 0x0C000001" \
    -c "mdw 0x46020CF0" \
    -c "resume" -c "shutdown"
...
0x46020828: 00000000
0x46020828: 00000001
0x46020cf0: 0c000000
0x46020cf0: 0c000001
```

（`Warn: target was in unknown state when halt was requested` 是
openocd 內部狀態追蹤的良性警告，不影響讀寫正確性，讀回值已證實。）

| 暫存器 | 寫前 | 寫後 | 動作 |
|---|---|---|---|
| `PWR_DBPR`（`0x46020828`） | `0x00000000` | `0x00000001` | 只設 bit0 `DBP`=1 |
| `RCC_BDCR`（`0x46020CF0`） | `0x0C000000` | `0x0C000001` | 只設 bit0 `LSEON`=1，`LSEDRV[4:3]`（`00`）、`LSION`/`LSIRDY`（bit26/27）等其餘位元原封不動 |

RMW 正確性確認：`0x0C000001` = `0x0C000000 | 0x00000001`，僅新增
`LSEON` 這一個 bit，沒有動到其他任何位元（尤其 `LSEDRV` 仍是原本的
`00`，符合任務「LSEDRV 保持原值不動」的要求）。

### B3：LSERDY 起振時間量測（0.5/1/2/3/5 秒各讀一次，皆為 halt→mdw→
resume 完整序列）

```
t=0.5s: 0x46020cf0: 0c000003
t=1s:   0x46020cf0: 0c000003
t=2s:   0x46020cf0: 0c000003
t=3s:   0x46020cf0: 0c000003
t=5s:   0x46020cf0: 0c000003
```

`0x0C000003` = `0x0C000001 | 0x00000002`，即 bit1 `LSERDY` 已變成 1
（`LSEON`/`LSION`/`LSIRDY` 維持不變）。**五個時間點裡最早的一筆
（t=0.5s，且這個 0.5s 還包含每次 openocd 重新連線 SWD 的額外開銷，
所以實際起振時間可能比 0.5s 更短）就已經是 ready，後面 1/2/3/5s 全部
穩定維持 `LSERDY=1`，沒有再變回 0 或跳動**。LSE 在這塊板子上起振非常
快，不需要進入 B4（5 秒未起振的失敗處理）。

★ 這比 A5 查到「RM 沒有給起振時間數值」的狀況多了一筆本板實測：至少
在 `LSEDRV=00`（最低驅動力，重置預設值）這個設定下，這顆 32.768kHz
振盪器（不確定是石英晶體還是陶瓷諧振器，板子規格書不在 `refs/`
目錄裡）在 1 秒內（很可能遠少於 1 秒）就穩定起振，沒有觀察到「久久
不起振」或「起振後又掉回未 ready」的情況。

### B4：跳過

5 秒內已確認起振並穩定，不觸發 B4 的失敗處理流程。

### B5：重啟 klipper 驗證

```
$ sudo systemctl start klipper && sleep 20 && curl -s http://localhost:7125/printer/info
{"result":{"state":"ready", ..., "software_version":"v0.13.0-474-ge9985ad22", ...}}

$ curl -s "http://localhost:7125/printer/objects/query?mcu" | ...
{'bytes_retransmit': 0, 'bytes_invalid': 0, 'send_seq': 129, 'receive_seq': 129,
 'srtt': 0.002, 'rttvar': 0.0, 'freq': 160570172, ...}
```

`state=ready`、`bytes_retransmit=0`、`bytes_invalid=0` 全部確認，B2 的
`DBP`/`LSEON` 寫入沒有影響 klipper 的正常連線（本輪 LPUART1 kernel
clock 選擇`RCC_CCIPR3`仍是重置預設 `000`=PCLK3，沒有改動，實際序列埠
運作跟這次的 LSE 起振測試無關，這正是預期行為，見下方 backup domain
說明）。

### 注意：backup domain 不會被 MCU reset 清除

`RCC_BDCR` 屬於 backup domain，只受 backup domain reset（`BDRST` 位元）
或 VBAT 完全斷電影響，一般的 MCU 系統重置（`systemctl restart
klipper`、SWD `reset run`/`reset halt`、看門狗重置）都不會清掉
`LSEON`。也就是說，**本輪測試結束後，`LSEON=1`、`DBP=1` 會持續保持**，
下次開機或下次 SWD 連線再讀 `RCC_BDCR`，`LSEON`/`LSERDY` 應該還是 1
——這是預期行為，不是殘留的異常狀態。

---

## 4. Part D：LPUART1 改掛 LSE、實測 9600 端到端（FAIL，已還原）

延續 part B 的 LSE 起振（`LSEON=1`/`LSERDY=1`），這裡把 LPUART1 的 kernel
clock 實際切到 LSE、BRR 改成 9600 baud 對應值，測試端到端連線。**結果：
FAIL——9600 baud 連不上，250000 也連不上（因為此時 MCU 仍是 9600/LSE
設定），但確認不是 IWDG 重置造成，已用 `reset run` 乾淨還原。**

### D0：唯讀盤點

```
$ sudo openocd -f /home/arduino/uno_q.cfg \
    -c "init" -c "reset run" -c "sleep 2000" -c "halt" \
    -c "mdw 0x46020CE8" -c "mdw 0x46002400" -c "mdw 0x4600240C" \
    -c "mdw 0x4600242C" -c "mdw 0x46020CF0" -c "mdw 0x46020818" \
    -c "mdw 0xE0044008" \
    -c "resume" -c "shutdown"
...
0x46020ce8: 00000002
0x46002400: 0000002d
0x4600240c: 00004000
0x4600242c: 00000000
0x46020cf0: 0c000003
0x46020818: 00000000
0xe0044008: 00001800
```

| 暫存器 | 位址 | 讀值 | 解讀 |
|---|---|---|---|
| `RCC_CCIPR3` | `0x46020CE8` | `0x00000002` | `LPUART1SEL[2:0]=010`=**HSI16**（★ 更正 part B 的推論，見上方第 5 節第 6 點） |
| `LPUART1_CR1` | `0x46002400` | `0x0000002D` | `UE\|RE\|TE\|RXNEIE`，`FIFOEN`（bit29，RM0456 行 188234）= 0，**FIFO 目前未啟用** |
| `LPUART1_BRR` | `0x4600240C` | `0x00004000` | 符合預期 |
| `LPUART1_PRESC` | `0x4600242C` | `0x00000000` | 符合預期（不分頻） |
| `RCC_BDCR` | `0x46020CF0` | `0x0C000003` | 延續 part B：`LSION/LSIRDY=1`、`LSEON/LSERDY=1` |
| ~~`PWR_DBPR`~~ | `0x46020818` | `0x00000000` | ★★ **這個位址不是 `PWR_DBPR`**，見下方 |
| `DBGMCU_APB1LFZR` | `0xE0044008` | `0x00001800` | bit12 `DBG_IWDG_STOP=1`、bit11 `DBG_WWDG_STOP=1`（RM0456 行 237790 起）——halt 期間 IWDG/WWDG 凍結，不會被咬 |

**BRR 與 LPUART1SEL 自洽性**：`LPUART1SEL=010`=HSI16=16MHz（標稱），
`LPUARTDIV=256×16,000,000/250,000=16,384=0x4000`，與讀到的 `BRR` 完全
吻合——若 kernel clock 不是 16MHz，同一個 `BRR` 換算出來的實際鮑率會
差很多，這組讀值自洽，證實目前確實是 HSI16 在跑 250000。

**★ 位址落差**：任務原給的 `0x46020818` 讀回 `0x00000000`，一開始容易
誤判成「DBP=0」，但查 RM0456 §10.10.7（行 30402-30404）確認
`0x46020818`（PWR 基底`0x46020800`+offset `0x18`）是 **`PWR_WUCR2`**，
不是 `PWR_DBPR`；`PWR_DBPR` 真正位址是 `0x46020828`（§10.10.11，行
30701-30703），也是 part B 驗證過的位址。發現後立刻停下回報，經使用者
確認「`0x46020818` 是錯的，那是 `PWR_WUCR2`，不准碰」、改用
`0x46020828`後才繼續 D1。

### D1：準備（`DBP`、`LSESYSEN`）

```
$ sudo systemctl stop klipper; pgrep -x openocd || echo NO_OPENOCD
NO_OPENOCD

$ sudo openocd ... -c "mdw 0x46020828"
0x46020828: 00000000
```

`DBP=0`——代表 part B 的 B2 之後，MCU 確實被重置過（D0 這次的
`reset run` 就是一次）。`PWR_DBPR` 不屬於 backup domain，一般重置就會
把它清回 0，這跟 `RCC_BDCR`（backup domain，讀回仍是 `0x0C000003`，
`LSEON`/`LSERDY` 完全沒掉）行為不同，符合預期，不是異常。

RMW 設 `DBP`：

```
0x46020828: 00000000   (讀)
mww 0x46020828 0x00000001
0x46020828: 00000001   (讀回，符合)
```

RMW `RCC_BDCR` 設 `LSESYSEN`（bit7），保留其餘位元：

```
0x46020cf0: 0c000003   (讀，= part B 結束時的狀態)
mww 0x46020CF0 0x0C000083
0x46020cf0: 0c000883   (讀回)
```

`0x0C000883` = `0x0C000003 | 0x00000080`（`LSESYSEN`）再加上硬體自動
設的 bit11 `LSESYSRDY`——**第一次讀回就已經是 `0x0C000883`**，
`LSESYSRDY` 在 32.768kHz 下只需要 2 個 LSE 時脈週期（約 61µs），遠快於
SWD 往返一次的時間，5 秒的 poll 視窗完全用不到。

### D2：切換（單一 openocd 呼叫，halt 狀態內完成）

```
$ sudo openocd -f /home/arduino/uno_q.cfg \
    -c "init" -c "halt" \
    -c "mdw 0x46002400" -c "mww 0x46002400 0x0000002C" -c "mdw 0x46002400" \
    -c "mdw 0x46020CE8" -c "mww 0x46020CE8 0x00000003" -c "mdw 0x46020CE8" \
    -c "mww 0x4600240C 0x0000036A" -c "mdw 0x4600240C" \
    -c "mww 0x46002400 0x0000002D" -c "mdw 0x46002400" \
    -c "resume" -c "shutdown"
...
0x46002400: 0000002d   ← 寫前（原值）
0x46002400: 0000002c   ← 清 UE 後讀回，符合
0x46020ce8: 00000002   ← 寫前（HSI16）
0x46020ce8: 00000003   ← 改成 LSE 後讀回，符合
0x4600240c: 0000036a   ← BRR 寫入後讀回，符合
0x46002400: 0000002d   ← UE 設回後讀回，與 D0 原值一致，符合
```

五個讀回全部符合預期，`resume` 正常執行。`BRR=0x36A=874`：
`256×32,768/9,600=873.8`，`DIV_ROUND_CLOSEST` 後正是 `874=0x36A`，跟
韌體 `serial_init()` 用的公式一致。

### D3：端到端驗證 —— ★ FAIL

```
$ (sleep 30; echo get_uptime; sleep 2; echo get_uptime; sleep 2; echo get_clock; sleep 3) \
    | timeout 45 ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 9600 /dev/ttyHS1
==================== attempting to connect ====================
INFO:root:Starting serial connect
INFO:root:Timeout on connect
ERROR:root:Wait for identify_response
...
serialhdl.error: Serial connection closed
Error: Unknown command: get_uptime
Error: Unknown command: get_uptime
Error: Unknown command: get_clock
（反覆 Timeout on connect / identify_response 逾時，45 秒視窗內從未
connected，identify 永遠沒有完成，因此沒有 identify 耗時可記錄）
```

**PASS 判準（identify 完成 + get_uptime 有回應）沒有達成，判定 FAIL。**

FAIL 後判準：改用 `-b 250000` 再跑一次 `get_uptime`：

```
$ (sleep 7; printf "get_uptime\n"; sleep 3) | timeout 20 \
    ~/klippy-env/bin/python3 ~/klipper/klippy/console.py -b 250000 /dev/ttyHS1
...
INFO:root:Timeout on connect
ERROR:root:Wait for identify_response
...
（同樣連不上，45秒/20秒視窗內沒有連上）
```

**`-b 250000` 也連不上。** 在直接做 `reset run` 還原之前，先用純讀取
（不寫入任何暫存器）確認 D2 寫入的值是否還在：

```
$ sudo openocd ... -c "mdw 0x46002400" -c "mdw 0x46020CE8" -c "mdw 0x4600240C" ...
0x46002400: 0000002d
0x46020ce8: 00000003
0x4600240c: 0000036a
```

**D2 的三個值都還在，跟寫入時完全一樣——MCU 沒有被重置（`DBG_IWDG_STOP=1`
本來就會讓 halt 期間 IWDG 凍結，這期間也沒有進 Stop2，IWDG 本來就不會
咬）。** 這代表：
- `-b 250000` 連不上是理所當然的——此時 MCU 實際的 `BRR`/`CCIPR3` 仍是
  9600/LSE 設定，不是韌體預設的 250000/HSI16，用錯誤的鮑率去連當然連不上，
  這個檢查本身是設計來偵測「IWDG 重置、設定已被韌體重新初始化」這個
  情境，而這次沒有發生。
- **真正的問題是 9600/LSE 這個設定本身的端到端連線就是連不上**，不是
  「連上但鮑率不對」，也不是「MCU 掛了」。根本原因本輪沒有查（見下方
  第 5 節「我無法判定的事」），比較合理的懷疑方向包括：host 端
  `/dev/ttyHS1`（名稱暗示是某種 High-Speed UART 介面）的驅動或硬體本身
  對 9600 這種低鮑率是否有支援限制、或 LSE 的 kernel clock 雖然
  `LSESYSRDY=1` 但實際上還沒有真正把 clock 送到 LPUART1（例如還需要
  RM 沒提到的額外步驟）——都只是假設，沒有進一步驗證。

### D4：還原

```
$ sudo openocd -f /home/arduino/uno_q.cfg -c "init" -c "reset run" -c "sleep 2000" -c "halt" \
    -c "mdw 0x4600240C" -c "mdw 0x46020CE8" \
    -c "resume" -c "shutdown"
...
0x4600240c: 00004000   ← 符合預期
0x46020ce8: 00000002   ← 符合預期（HSI16）

$ sudo systemctl start klipper && sleep 20 && curl -s http://localhost:7125/printer/info
{"result":{"state":"ready", ...}}

$ curl -s "http://localhost:7125/printer/objects/query?mcu" | ...
{'bytes_retransmit': 0, 'bytes_invalid': 0, 'send_seq': 143, 'receive_seq': 143, 'srtt': 0.002, ...}
```

`BRR`/`CCIPR3` 都回到 D0 原值，`state=ready`、`bytes_retransmit=0`、
`bytes_invalid=0` 全部確認。`RCC_BDCR` 的 `LSEON`/`LSESYSEN` 沒有還原
（backup domain 不受一般重置影響），這是預期行為，跟 part B 的說明一致。

---

## 5. 我無法判定的事

1. **A1 的 HSIKERON 疑問**：`LPUART1SEL` 的 RM 註解沒有像 `LPTIM1SEL`/
   `LPTIM34SEL` 那樣要求「HSI16 with HSIKERON=1」，本文件推論是因為
   LPUART1 走 autonomous-request 自動喚醒路徑、不需要手動常駐 HSI16，
   但 RM 沒有明文解釋兩者寫法不同的原因，這是推論不是直接證據。
2. **A2 的精確門檻**：RM 給的 88.36 kbaud 是「範例」數字，不是
   STM32U585 這顆晶片的 `tWULPUART`/HSI16 inaccuracy 實測值或 datasheet
   規格值。`refs/` 目錄沒有 datasheet，無法算出這顆晶片在 250000 baud
   下的真實 `DWU`/容錯裕度。第 2 節的方案建議是保守判斡，不是精確計算
   結果。
3. **A4/A5 的 LSEDRV 建議值**：RM 沒有給數值化的建議檔位，本輪只在
   `LSEDRV=00`（重置預設、最低驅動力）測過一組設定，且已經很快起振，
   沒有理由再去試更高驅動力檔位（任務也明確要求「LSEDRV 保持原值不動」）
   ——但如果換一顆板子/換一顆晶體，`00` 檔位是否還能穩定起振沒有測過，
   不能直接類推到其他硬體。
4. **精確起振時間**：B3 只能確認「≤0.5 秒（含 SWD 重連開銷）」就已經
   ready，不能給出比這更精確的數字，因為每次讀值都要重新建立 SWD 連線，
   量測解析度被連線開銷限制住，量不到「起振實際花了幾毫秒」這種精細度。
5. **這是石英晶體還是陶瓷諧振器、廠牌/型號、走線寄生電容**：這些都會
   影響起振時間與長期穩定度，本輪沒有board schematic/datasheet 可查，
   無法判定這次量到的「快速起振」對其他同型板子是否有代表性。
6. ~~`LPUART1SEL` 仍是重置預設 `000`(PCLK3)，本輪完全沒有把 LPUART1
   實際接上 LSE~~ ★ **這句話是錯的，已在 part D（第 4 節）用 D0 實測更正**：
   `RCC_CCIPR3` 讀回是 `0x00000002`（`LPUART1SEL=010`=HSI16），不是重置
   預設的 `000`（PCLK3）——韌體開機流程本身就已經把 LPUART1 的 kernel
   clock 主動設成 HSI16，不是留在預設值。B2-B5 當時確實只驗證了「LSE
   本身能不能起振」，沒有把 LPUART1 接上 LSE；這件事後來在 part D
   （第 4 節）做了，也做了 `LSESYSEN`→`LSESYSRDY` 那一步，結果是
   **9600 baud 端到端連線失敗**，細節見第 4 節。
7. ★ **D3 為什麼 9600/LSE 連不上，根本原因本輪沒有查**。已知的事實只有：
   SWD 讀回證實 `CR1`/`CCIPR3`/`BRR` 三個暫存器都精確落在計算值、
   `LSESYSRDY=1`、MCU 沒有重置（暫存器值全程沒變）——照 RM0456 字面
   的設定步驟看起來都做對了，但實際收發就是建立不起來。沒有做的診斷
   包括：（a）沒有用示波器/邏輯分析儀看 `/dev/ttyHS1` 對應接腳在切換
   後有沒有真的輸出 9600 baud 的訊號；（b）沒有查 host 端（Qualcomm
   side）`/dev/ttyHS1` 這個 High-Speed UART 介面的驅動是否對超低鮑率
   有限制；（c）沒有嘗試過中間鮑率（例如先切到 38400 這種 HSI16 也能跑
   但比較低的鮑率）來縮小「是 LSE 本身的問題」還是「是鮑率太低本身的
   問題」這兩個假設的範圍。這些都留給下一輪，本輪只如實記錄「照 RM
   設定但連不上」這個事實，不猜測根因。
