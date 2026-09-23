# Build 環境筆記

## PC（`~/klipper-fw`）不能 build

這個 repo（`weiyu0907/klipper.git`，分支 `stop2-milestone-2a`）**只用來編輯與
git 操作，沒有完整的 vendor 原始碼樹，不能在這台 PC 上 `make`**。

原因：`src/stm32/internal.h`（`#if defined(CONFIG_MACH_STM32U585)` 區塊）
無條件 `#include "cmsis_u5/stm32u5_usb.h"`；`src/stm32/cmsis_u5/` 被
`.gitignore` 第 8 行排除（`src/stm32/cmsis_u5/`），跟 `~/refs/` 一樣是
「vendor 檔案，使用者自行放置、不進 repo」的性質——**但這個目錄在 PC
上從來沒有放過**，只要 `.config` 選了 `CONFIG_MACH_STM32U585=y`（例如
套用 `configs/unoq_klipper_250000.config`）就會在編譯第一顆用到
`internal.h` 的 `.c` 檔卡住：

```
out/board/internal.h:116:10: fatal error: cmsis_u5/stm32u5_usb.h: No such file or directory
```

跟當下改了什麼程式碼完全無關——即使 `git stash` 掉所有改動，只要選這個
target 一樣編不過。

## 真正的 build 環境：板子（`unoq:~/klipper`）

**所有 build 都必須在板子上執行**（SSH alias `unoq`，見
`~/.ssh/config`），板子上的 `~/klipper` 才是完整原始碼樹：

```
$ ssh unoq "ls -la ~/klipper/src/stm32/cmsis_u5/"
-rw-rw-r-- 1 arduino arduino 143378  5月 30 20:20 stm32u5_usb.h
```

只有一個檔案，`stm32u5_usb.h`（USB OTG FS 暫存器定義），檔頭註解：

```
/* USB OTG FS definitions extracted from ST STM32U585 CMSIS header */
/* Synopsys OTG core - same IP as STM32F4/F7/H7                    */
```

沒有附 README 或版權/版本資訊，來源是「從 ST 官方 STM32U585 CMSIS
header 節錄出來的 USB OTG FS 定義」——這份筆記本身就是目前唯一的來源
紀錄，因為原始檔案裡沒有留來源連結或版本號。`.config` 裡
`CONFIG_USBSERIAL`/`CONFIG_HAVE_STM32_USBOTG` 目前都沒開（走 LPUART，
不是 USB CDC），這個檔案在執行期沒有真的被用到，但 `internal.h` 對這個
target 無條件 `#include` 它，缺了就是連編譯都過不了，屬於編譯期硬依賴。

## 板子上的工作流程

```
$ scp <改動的檔案> unoq:~/klipper/src/stm32/     # 只同步改動的檔案，
                                                    # 不要整棵樹覆蓋，
                                                    # 板子上的 vendor
                                                    # 檔案與 .config
                                                    # 不可動
$ ssh unoq "cd ~/klipper && make clean && make"
$ ssh unoq "cd ~/klipper && size out/klipper.elf"
```

板子上的 `~/klipper` 是 `git@github.com:weiyu0907/klipper.git`（remote
`mine`）的另一個 checkout，**跟 PC 的 `~/klipper-fw` 是兩份獨立的工作
目錄，git commit 只在 PC 端做**——板子端目前落在較舊的 commit
（`e9985ad22`），後續在 PC 端提交的改動（例如 G1b 的
`stm32u5_bringup_mark`）是以**未 commit 的 working tree 改動**形式存在
於板子上，每次要 build 新版本，都要先確認板子上的檔案內容跟 PC 這邊
「要送去編譯的版本」一致（例如用 `diff` 比對），再决定要不要用 `scp`
同步過去，避免蓋掉板子上還沒同步回 PC 的東西。

## 燒錄

沿用既有流程（見 `docs/2b_g_wake_latency.md` §4）：`openocd` 透過 SWD
對板子燒錄 `out/klipper.bin`，燒錄動作本身仍然需要在停下等使用者過目
`git diff`/`size`/版本字串、經批准後才執行，這條規則不因為 build 搬到
板子上而改變。
