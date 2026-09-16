# Stop2 Milestone 2A — 外部審查修復報告

延續 commit `e701a40a`（"stm32u5: add Stop2 feasibility test (Milestone 2A)"）。
本輪修復外部審查提出的四個問題，並補上 LPTIM1 寫入時機稽核。範圍仍嚴格
限制在「Stop2 進出可行性驗證」：不做時鐘飄移補償、不碰 DWT CYCCNT、不動
`irq_wait()`、不把 Stop2 序列放進 `stm32_clock_init()`。

涉及檔案：`src/stm32/stm32u5.c`、`src/stm32/stm32u5_lowpower.c`。

---

## 問題 1：漏了「醒了但 PLL 沒重鎖」這種靜默失敗

### 問題本質

LPTIM1 掛在 LSI 上，跟 SYSCLK 是否成功切回 PLL1R 完全獨立。如果
`stm32u5_sysclk_restore()` 內部某一步沒有真的鎖上（例如 PLL1RDY 一直不
來），LED3_R 仍會照正確週期閃——因為觸發閃爍的是 LPTIM1 中斷，跟 CPU
實際跑在 4MHz(MSIS) 還是 160MHz(PLL1R) 無關。外觀上跟成功一模一樣，但
整顆 MCU 慢了 40 倍，這是最危險的一種失敗：它不會被舊版的三態判讀
（rapid blink / 全暗 / 週期不對）任何一種抓到。

### 修法

改成三色狀態燈，並在 `stop2_once()` 醒來後直接讀 `RCC_CFGR1.SWS` 驗證
「真的切回 PLL1R 了沒」，不再只信任閃爍節奏。

修改前（`stop2_once()` 醒來段落，commit e701a40a）：

```c
    SCB_SCR &= ~(1u << 2);
    U5_PWR_CR1 &= ~7u;
    stm32u5_sysclk_restore();
    ...
    led3_red(1);
}
```

修改後：

```c
    SCB_SCR &= ~(1u << 2);
    U5_PWR_CR1 &= ~7u;

    restore_timeout = stm32u5_sysclk_restore();   /* 現在有回傳值 */
    ...
    stop2_capture_status(0, restore_timeout, lptim_timeout);

    if (restore_timeout) {
        led3_blue(1);
    } else if (s_status.sws != 3u) {      /* 直接讀 SWS，不信任閃爍節奏 */
        led3_green(1);
    } else {
        led3_red(1);
    }
}
```

新增 `led3_green()` / `led3_blue()`（PH11/PH12，接線與致能方式比照既有
`led3_red()`），`led3_init()` 一併把 PH11/PH12 的 MODER 設成輸出。

### 為什麼是「終止態」

G 或 B 點亮後，`command_test_stop2()` 會 `break` 掉後續 `cycles`，讓燈
維持在錯誤狀態給人看，不會被下一輪的 `led3_red(0)` 蓋掉。這點刻意跟舊版
「PWR_CR1/SCB_SCR 寫入驗證失敗」的處理方式不同——那個分支仍然保留continue
迴圈以維持原本「快速閃爍」的診斷訊號（見下方 LED 對照表的說明）。

---

## 問題 2：忙等沒有 timeout

### 問題本質

`stm32u5_sysclk_bringup()`（原名 `stm32u5_sysclk_restore()` 內聯的那段）
四段忙等（HSI16RDY / VOSRDY / BOOSTRDY / PLL1RDY，以及兩段 SWS 確認）
原本全部是不設上限的 `while()`。如果 PLL 真的鎖不上，喚醒路徑會卡死在
這裡出不來，LED 永久暗——跟「wfi 醒不來」外觀完全一樣，會讓人往錯的
方向查（去懷疑 LPTIM1/NVIC，而不是 PLL/VOSR）。

### 修法

`stm32u5.c` 內所有忙等一律改成「有上限的計數迴圈」，逾時只放棄那一段
等待、不提早 return，讓整個序列跑到底。

修改前：

```c
static void stm32u5_pll1_init(void)
{
    U5_RCC_CR &= ~(1u << 24);
    while (U5_RCC_CR & (1u << 25));
    ...
    U5_RCC_CR |= (1u << 24);
    while (!(U5_RCC_CR & (1u << 25)));
}
```

修改後：

```c
static uint32_t stm32u5_pll1_init(void)
{
    uint32_t i, timeout = 0;

    U5_RCC_CR &= ~(1u << 24);
    for (i = 0; i < U5_WAIT_LOOPS; i++) {
        if (!(U5_RCC_CR & (1u << 25)))
            break;
    }
    if (i >= U5_WAIT_LOOPS)
        timeout = 1;
    ...
    U5_RCC_CR |= (1u << 24);
    for (i = 0; i < U5_WAIT_LOOPS; i++) {
        if (U5_RCC_CR & (1u << 25))
            break;
    }
    if (i >= U5_WAIT_LOOPS)
        timeout = 1;

    return timeout;
}
```

`stm32u5_voltage_scale1()`、`stm32u5_flash_latency_set()`、
`stm32u5_pll1_init()`、`stm32u5_sysclk_bringup()` 全部比照改成回傳
`uint32_t`（0=順利，非0=至少一段逾時），`stm32u5_sysclk_bringup()` 用
`timeout |= sub_func()` 聚合。

**「不要 return 到一半留下半設定的時鐘樹」怎麼做到**：逾時只是不再等、
放棄那一段迴圈，但後面該做的暫存器寫入照樣全部執行。若某個來源真的沒
ready，硬體本來就會拒絕把 SW/SWS 切換到一個沒 ready 的來源——實際
SYSCLK 就會停在「切換前最後一個真的生效」的來源（開機重置預設或 Stop2
喚醒後的預設都是 MSIS，或已經成功切過去的 HSI16），不會變成切一半的不
可用狀態。這個安全網是 ARM/ST 時鐘控制器本身的行為，不需要額外程式碼
保證。

冷開機路徑 `stm32_clock_init()` 呼叫 `stm32u5_sysclk_bringup()` 時故意
用 `(void)` 丟棄回傳值——這個時間點 LPUART1 的時脈閘都還沒開（Step 8
才開），沒有任何管道能把逾時回報出去，加了也沒用；Stop2 喚醒路徑
（`stm32u5_lowpower.c`）才會真的檢查它並透過 LED3_B + `sendf()` 回報。

---

## 問題 3：回報實際的 SYSCLK 來源（`sendf`）

新增 `struct stop2_status`（`stm32u5_lowpower.c`），`stop2_once()` 在
每個結束路徑（正常 / PWR_CR1 或 SCB_SCR 寫入驗證失敗）都呼叫
`stop2_capture_status()` 填好，`command_test_stop2()` 每一輪都送一次：

```c
sendf("stop2_status cycle=%u sws=%u pll1rdy=%u pwr_cr1=%u"
      " scb_scr=%u restore_timeout=%u lptim_timeout=%u"
      " entry_fail=%u"
      , s_status.cycle, s_status.sws, s_status.pll1rdy
      , s_status.pwr_cr1, s_status.scb_scr
      , s_status.restore_timeout, s_status.lptim_timeout
      , s_status.entry_fail);
```

欄位對應：

| 欄位 | 來源 | 意義 |
|---|---|---|
| `sws` | `RCC_CFGR1[3:2]` | 3=PLL1R 才算真的鎖上；0=MSIS、1=HSI16、2=HSI48 |
| `pll1rdy` | `RCC_CR` bit25 | PLL1 是否鎖相 |
| `pwr_cr1` | `PWR_CR1` 收尾後讀回值 | 應為清空 LPMS 後的狀態 |
| `scb_scr` | `SCB_SCR` 收尾後讀回值 | 應為清空 SLEEPDEEP 後的狀態 |
| `restore_timeout` | `stm32u5_sysclk_restore()` 回傳值 | 非0=還原過程中至少一段忙等逾時 |
| `lptim_timeout` | 本輪 DIER/ARR 的 OK 輪詢 | 非0=LPTIM1 武裝過程逾時（未達 LED 對照表定義的獨立顏色，僅供記錄） |
| `entry_fail` | 進入前 PWR_CR1/SCB_SCR 讀回驗證 | 非0=根本沒發出 wfi 就放棄了這輪 |

這組數字附一份 log（host console 的輸出，或另存 serial capture）即可
當論文的原始數據，不需要額外量測工具。

---

## 問題 4：檔頭補上測試前提

`stm32u5_lowpower.c` 檔頭新增顯眼區塊，說明：

- Stop2 期間核心時脈停止，`DWT->CYCCNT` 之類的計時全部凍結（本檔案本
  來就沒用它，但這是背景知識，寫給要接手擴充的人看）。
- **關鍵**：LPUART1 在 Stop2 下也是死的——本里程碑刻意只設 LPTIM1 的
  `APB3SMENR`/`SRDAMR`，沒有連帶設 LPUART1 那兩顆。若 `klipper.service`
  還在跑，host 端週期性 `get_clock` 收不到回應，MCU 睡過 klipper 的逾時
  門檻就會觸發 "Lost communication with MCU" 並重啟韌體，測試在中途被
  砍掉，且症狀會被誤判成 Stop2 本身壞掉（其實是 host 端逾時重啟）。
- 正確測法明列在檔頭：先 `sudo systemctl stop klipper`，改用
  `~/klippy-env/bin/python3 ~/klipper/klippy/console.py /dev/ttyHS1`
  手動下 `test_stop2 period_ms=... cycles=...`。

---

## LPTIM1 寫入時機稽核

位元編號查證自使用者提供的 `stm32u585xx.h` 對照表。逐條結論：

| 項目 | 結論 | 說明 |
|---|---|---|
| (a) CFGR 只能在 ENABLE=0 時寫 | **原本符合，維持不變** | 序列一開始 `CR=0` 確保 ENABLE=0，再寫 `CFGR=0`，最後才 `CR=ENABLE` |
| (b) ARR 須在 ENABLE=1 之後寫，寫完輪詢 ARROK | **原本符合，維持不變** | 舊版就有 `for` 迴圈輪詢 `ISR` bit4 再 `ICR` 清 bit4；本輪只是把它從「先」改成「DIER 之後」執行，相對順序沒變 |
| (c) 寫 DIER 之後要輪詢 ISR.DIEROK | **原本違反，已修正** | e701a40a 版本寫完 `DIER` 直接往下走，沒有輪詢 bit24 DIEROK 也沒清 ICR bit24。本輪加上輪詢（`U5_WAIT_LOOPS` 逾時保護）與 `ICR=(1u<<24)` 清除，並把 DIER 的寫入時機從 ENABLE 之前挪到 ENABLE 之後（見下方追加說明） |
| (d) 啟動順序 ENABLE=1 → 等 → CNTSTRT/SNGSTRT=1 | **原本符合，維持不變** | 舊版已是 `CR=ENABLE` 在前、`CR|=SNGSTRT` 在後；本輪額外把「等」明確對應到 DIER/ARR 的 OK 輪詢，不需要再疊加一段無意義的延遲迴圈 |
| (e) 每個輪詢都要有 timeout | **原本部分違反，已修正** | 舊版只有 ARROK 輪詢有上限（`200000` 次），`stm32u5_sysclk_restore()`（現 `stm32u5_sysclk_bringup()`）裡的四段時鐘忙等完全沒有上限（見問題 2）。本輪全部補上 `U5_WAIT_LOOPS` 上限 |
| (f) LSI 是標稱值不是實測值，註解要寫明 | **原本符合，維持不變** | e701a40a 版本已有「LSI 標稱頻率 32kHz（未校準）」註解；本輪在 `LSI_HZ_NOMINAL` 定義處與 `lptim1_wakeup_init()` 內都重申一次，並在 review 文件內再次強調 |

### 追加說明：DIER 寫入時機為什麼從 ENABLE 之前挪到之後

e701a40a 版本的順序是 `CR=0 → CFGR=0 → DIER=ARRMIE → CR=ENABLE → ARR=...`
——DIER 寫在 ENABLE 之前。稽核給出的暫存器表把 DIER 的 OK 確認位放在
`ISR`/`ICR` 的 bit24（跟 ARR 的 bit4 是同一類「需要 kernel clock domain
同步」的機制），這意味著 DIER 的寫入跟 ARR 一樣，必須等 LPTIM 已經
`ENABLE=1`、同步電路開始運作之後才能真正落地——寫在 ENABLE 之前，硬體
可能根本不會產生 DIEROK（同步電路還沒通電），若照舊等它會直接逾時。
本輪把順序改成 `CR=0 → CFGR=0 → CR=ENABLE → DIER=ARRMIE(等DIEROK) →
ARR=...(等ARROK) → CR|=SNGSTRT`，讓 DIER 也在 ENABLE 之後寫、比照 ARR
的模式輪詢確認。

---

## 進入 Stop2 的完整暫存器寫入順序

| # | 暫存器 | 位址 | 值 | 為什麼在這個位置 |
|---|---|---|---|---|
| 1 | `LPTIM1_CR` | 0x46004410 | 0 | 確保 ENABLE=0，後面才能合法寫 CFGR |
| 2 | `LPTIM1_CFGR` | 0x4600440C | 0 | (a) CFGR 只能在 ENABLE=0 時寫 |
| 3 | `LPTIM1_CR` | 0x46004410 | bit0=1 (ENABLE) | DIER/ARR 的同步電路要 ENABLE=1 才會動作 |
| 4 | `LPTIM1_DIER` | 0x46004408 | bit1=1 (ARRMIE) | 開自動重載中斷；寫完輪詢 `ISR` bit24 DIEROK（有 timeout），再寫 `ICR` bit24 清旗標 |
| 5 | `LPTIM1_ARR` | 0x46004418 | 換算後的 tick 數 | (b) 必須在 ENABLE=1 之後寫；寫完輪詢 `ISR` bit4 ARROK（有 timeout），再寫 `ICR` bit4 清旗標 |
| 6 | `LPTIM1_CR` | 0x46004410 | bit1=1 (SNGSTRT，疊加在 ENABLE 上) | (d) 啟動順序的最後一步，此時 DIER/ARR 都已確認生效 |
| 7 | `SYSTICK_CTRL` | 0xE000E010 | 存舊值後寫 0 | 關閉 SysTick，避免它的 pending 讓 wfi 直接變 no-op |
| 8 | `SCB_ICSR` | 0xE000ED04 | bit25=1 (PENDSTCLR) | 清掉 SysTick 已經 pending 的旗標 |
| 9 | `PWR_CR1` | 0x46020800 | LPMS[2:0]=0b010 | 選 Stop2；讀回驗證（寫了看不出來，見「兩顆特殊暫存器」） |
| 10 | `SCB_SCR` | 0xE000ED10 | bit2=1 (SLEEPDEEP) | 配合 PWR_CR1 決定 wfi 進哪一種睡眠；讀回驗證 |
| 11 | `dsb` | — | — | 確保 9/10 這兩顆經過 bus fabric 的寫入真正落地，否則 wfi 可能退化成普通 Sleep |
| 12 | `wfi` | — | — | 實際休眠點 |
| 13 | `isb` | — | — | 醒來沖刷管線，後續讀取反映真實喚醒後狀態 |
| 14 | `SCB_SCR` | 0xE000ED10 | 清 bit2 | 醒來後**立刻**拆線，避免之後任何一次 wfi 意外重新掉進 Stop2 |
| 15 | `PWR_CR1` | 0x46020800 | 清 LPMS | 同上 |
| 16 | `stm32u5_sysclk_restore()`（= Step 0-4 bringup） | 見 stm32u5.c | — | 重建 SYSCLK；回傳值即 `restore_timeout` |
| 17 | `SCB_ICSR` / `SYSTICK_CTRL` | 同 7/8 | 還原成呼叫前的值 | 讓 Klipper 的排程繼續運作 |
| 18 | `LPTIM1_ICR` / `LPTIM1_CR` / `NVIC_ICPR2` | — | 清乾淨 | 避免殘留旗標讓下一輪或其他中斷誤判 |
| 19 | `irq_enable()` | — | — | 全部清乾淨、狀態一致之後才重新開中斷，pending 的 LPTIM1 IRQ 這時才真正跑 |

---

## LED3 狀態對照表

| 現象 | 狀態 | 代表意義 |
|---|---|---|
| LED3_R 穩定按 `period_ms` 閃爍 | 正常 | 每輪都真的進出 Stop2，且醒來後 SYSCLK 確實鎖回 PLL1R |
| LED3_R 幾乎瞬間反覆亮滅（肉眼看接近快閃/常亮交替） | 根本沒進 Stop2 | `PWR_CR1`/`SCB_SCR` 讀回驗證失敗，`stop2_once()` 提早 return；因為不中止外層迴圈，會一輪一輪快速重跑 |
| 全暗，之後永遠不再變化 | 進去了但醒不來 | 卡在 `wfi`；此時程式根本沒有執行到之後任何一行，因此連 `sendf` 都不會送出——這是唯一「連診斷數據都拿不到」的情況 |
| LED3_G 恆亮（不再閃） | 醒來了但 SWS ≠ PLL1R | **問題1**：SYSCLK 卡在 MSIS/HSI16，PLL1 沒重鎖成功，但因為喚醒源獨立於 SYSCLK，若沒有這顆燈會誤判成完全正常 |
| LED3_B 恆亮（不再閃） | restore 某段忙等逾時 | **問題2**：`stm32u5_sysclk_bringup()` 內至少一段等待超過 `U5_WAIT_LOOPS`；此狀態下 `sws` 多半也會不等於 3（兩者常同時出現，此時以 B 優先顯示，因為它指出的是更早的根因） |
| LED3_R 穩定閃爍但週期跟 `period_ms` 對不上 | 進出正常但週期不對 | LSI 未校準（審計項目 (f)），預期內的落差 |

判讀順序建議：先看有沒有任何顏色，再看是不是 R 以外的顏色恆亮，最後才
去比對閃爍週期。

---

## 板子上的完整測試步驟

```sh
# 1. 停用 klipper.service，避免 host 端 get_clock 逾時把韌體重啟掉
sudo systemctl stop klipper

# 2. 用裸 console 連上 MCU（依實際序列埠調整路徑）
~/klippy-env/bin/python3 ~/klipper/klippy/console.py /dev/ttyHS1

# 3. 在 console 提示字元下手動觸發測試
test_stop2 period_ms=2000 cycles=5
```

觀察與記錄：

1. 用肉眼/手機錄影盯著 LED3，比對「LED3 狀態對照表」判斷落在哪一種情況。
2. console 視窗會即時印出每一輪的 `stop2_status ...` 回應（來自本輪新增
   的 `sendf`），把這些行存下來即為論文可用的原始數據。
3. 測試完成或中止後，若要恢復正常列印功能，重新啟動 klipper.service：
   `sudo systemctl start klipper`。

---

## ★ 我無法在沒有硬體的情況下確認的事

以下判斷全部只靠 RM0456 文件推論與程式碼邏輯推導，**沒有任何實機驗證**，
列出來是誠實的風險清單，不是掩飾：

1. **DIER 寫入時機的修正方向**：我依「DIER 有 OK 確認位，跟 ARR 同類」
   推論它必須在 ENABLE=1 之後才寫，並據此把它從 ENABLE 之前搬到之後。
   這是合理但未經硬體驗證的推論——如果實際上 DIER 反而跟 CFGR 同類、
   必須在 ENABLE=0 時寫，這個修改會讓 DIEROK 永遠等不到，變成新的
   `lptim_timeout=1`。這點務必在拿到硬體後第一件事就用 `sendf` 的
   `lptim_timeout` 欄位驗證。

2. **LED3_G / LED3_B 的 GPIO 位置與極性**：PH11/PH12 的 MODER 位置、
   BSRR 的 active-low 慣例是照搬使用者提供的 PH10 資訊等比例推算，我沒
   有這兩顆 LED 實際存在於板子上、接線正確的獨立確認依據。如果板子上
   LED3_G/B 沒接在 PH11/PH12，這兩個「終止態」燈號會完全無反應，容易被
   誤判成「沒有問題」。

3. **`U5_WAIT_LOOPS = 200000` 這個逾時上限是否足夠寬鬆**：這個數字沿用
   舊版 ARROK 輪詢已經在用的值，沒有實測過在 160MHz/HSI16/PLL 各種時脈
   下對應到多少真實時間。如果 PLL 鎖相在正常情況下就需要比這個迴圈次數
   更久，會出現「假性 timeout」——LED3_B 亮起但其實只是等得不夠久，不是
   真的硬體故障。這個數字之後需要拿示波器或至少用 host 端的牆鐘時間
   （`sendf` 送出的時間戳）反推校準。

4. **PWR_CR1/SCB_SCR 讀回驗證抓不到「部分正確」的情況**：目前的驗證只
   檢查「值有沒有變成我要的」，如果硬體接受了 LPMS 的寫入但其他相關的
   前提條件（例如某個電源域還沒穩定）沒有滿足，讀回可能仍顯示「正確」
   但實際上不會真的進 Stop2——這種情況會被誤判成正常，而不是任何一種
   已定義的失敗態。目前沒有辦法純靠暫存器讀回排除這個可能性。

5. **`entry_fail` 分支「不中止迴圈」是否真的會表現成穩定的快速閃爍**：
   這是邏輯推導（每輪執行時間在微秒級，遠快於 `busy_delay` 的可見亮燈
   時間），但沒有實機測過人眼/手機相機在這個閃爍速度下實際看起來是什麼
   樣子——有可能因為亮燈時間（`busy_delay(2000000)`，微秒級）和熄燈時間
   （幾乎為0）差距懸殊，肉眼看起來反而接近「常亮」而非「快閃」，屆時
   需要調整 `busy_delay` 的參數或改用更明顯的視覺區分方式。

6. **`sendf` 的 8 個欄位在單一 command 觸發的多輪回應下，host 端
   console.py 是否都能正確逐行解析並顯示**：欄位數與型別是仿照既有
   `basecmd.c` 的用法推斷合法，但沒有實際跑過 `buildcommands.py` 產生
   對應的 dictionary、也沒有實際在 console 裡看過這行輸出長什麼樣子。

7. **Step 0-4 的 timeout 聚合邏輯（`timeout |= sub_func()`）在冷開機路徑
   完全沒有出過問題的前提下是否會改變原本開機時序的時間**：新增的迴圈
   計數相較原本的 `while()` 在邏輯上等價（成功時循環次數相同），但沒有
   實測過在真正冷開機、一路正常鎖相的路徑上，這個改寫有沒有引入任何
   額外的時間開銷或編譯器優化差異。
