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

## 3. B0/B1：現況讀值（已完成，B2 起需授權後補做）

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

B1 已完成，皆為純讀取，未寫入任何暫存器。

**B2 起（寫 `PWR_DBPR` 設 `DBP=1`，寫 `RCC_BDCR` 只設 `LSEON`）需要對
`openocd mww` 寫暫存器動作的明確授權** —— 這個動作被 Claude Code
auto-mode 分類器擋下（歸類為「Modify Shared Resources」），已詢問使用者，
使用者回覆「先寫 A 部分到文件裡，B2 等我放行」，故 B2-B5 在本次提交裡
**尚未執行**，待授權後補做並更新本文件。

**當前板子狀態**：`klipper.service` 已重新啟動並確認 `state=ready`
（B2 之前的操作只有讀取，沒有變動任何暫存器狀態，所以韌體行為未受
影響）。

---

## 4. 我無法判定的事

1. **A1 的 HSIKERON 疑問**：`LPUART1SEL` 的 RM 註解沒有像 `LPTIM1SEL`/
   `LPTIM34SEL` 那樣要求「HSI16 with HSIKERON=1」，本文件推論是因為
   LPUART1 走 autonomous-request 自動喚醒路徑、不需要手動常駐 HSI16，
   但 RM 沒有明文解釋兩者寫法不同的原因，這是推論不是直接證據。
2. **A2 的精確門檻**：RM 給的 88.36 kbaud 是「範例」數字，不是
   STM32U585 這顆晶片的 `tWULPUART`/HSI16 inaccuracy 實測值或 datasheet
   規格值。`refs/` 目錄沒有 datasheet，無法算出這顆晶片在 250000 baud
   下的真實 `DWU`/容錯裕度。第 2 節的方案建議是保守判斡，不是精確計算
   結果。
3. **A4/A5 的 LSEDRV 建議值與起振時間**：RM 沒有給數值化的建議檔位、
   也沒有給起振時間的典型值/最大值，這兩項都要嘛查 datasheet、要嘛
   等 B2-B5 的實測（如果之後被授權執行）才能有具體數字。
4. **B2-B5 尚未執行**，LSE 是否真的能在這塊板子上起振、要多久，目前
   完全沒有實測數據，第 2 節的方案建議完全建立在 RM 文字查證上，還沒有
   任何硬體驗證支持。
