// STM32F0 serial
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_SERIAL_BAUD
#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/serial_irq.h" // serial_rx_byte
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // enable_pclock
#include "sched.h" // DECL_INIT

// Select the configured serial port
#if CONFIG_STM32_SERIAL_USART1
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA10,PA9");
  #define GPIO_Rx GPIO('A', 10)
  #define GPIO_Tx GPIO('A', 9)
  #define USARTx_FUNCTION GPIO_FUNCTION( \
            (CONFIG_MACH_STM32H7 | CONFIG_MACH_STM32G4) ? 7 : 1)
  #define USARTx USART1
  #define USARTx_IRQn USART1_IRQn
#elif CONFIG_STM32_SERIAL_USART1_ALT_PB7_PB6
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PB7,PB6");
  #define GPIO_Rx GPIO('B', 7)
  #define GPIO_Tx GPIO('B', 6)
  #define USARTx_FUNCTION GPIO_FUNCTION( \
            (CONFIG_MACH_STM32H7 | CONFIG_MACH_STM32G4) ? 7 : 0)
  #define USARTx USART1
  #define USARTx_IRQn USART1_IRQn
#elif CONFIG_STM32_SERIAL_USART2
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA3,PG7");
  #define GPIO_Rx GPIO('A', 3)
  #define GPIO_Tx GPIO('G', 7)
  #define USARTx_FUNCTION GPIO_FUNCTION( \
            (CONFIG_MACH_STM32H7 | CONFIG_MACH_STM32G4) ? 7 : 1)
  #define USARTx USART2
  #define USARTx_IRQn USART2_IRQn
#elif CONFIG_STM32_SERIAL_USART2_ALT_PA15_PA14
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA15,PA14");
  #define GPIO_Rx GPIO('A', 15)
  #define GPIO_Tx GPIO('A', 14)
  #define USARTx_FUNCTION GPIO_FUNCTION(CONFIG_MACH_STM32G4 ? 7 : 1)
  #define USARTx USART2
  #define USARTx_IRQn USART2_IRQn
#elif CONFIG_STM32_SERIAL_USART2_ALT_PB4_PB3
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PB4,PB3");
  #define GPIO_Rx GPIO('B', 4)
  #define GPIO_Tx GPIO('B', 3)
  #define USARTx_FUNCTION GPIO_FUNCTION(7)
  #define USARTx USART2
  #define USARTx_IRQn USART2_IRQn
#elif CONFIG_STM32_SERIAL_USART2_ALT_PD6_PD5
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PD6,PD5");
  #define GPIO_Rx GPIO('D', 6)
  #define GPIO_Tx GPIO('D', 5)
  #define USARTx_FUNCTION GPIO_FUNCTION(7)
  #define USARTx USART2
  #define USARTx_IRQn USART2_IRQn
#elif CONFIG_STM32_SERIAL_USART3
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PB11,PB10");
  #define GPIO_Rx GPIO('B', 11)
  #define GPIO_Tx GPIO('B', 10)
  #define USARTx_FUNCTION GPIO_FUNCTION(7)
  #define USARTx USART3
  #define USARTx_IRQn USART3_IRQn
#elif CONFIG_STM32_SERIAL_USART3_ALT_PD9_PD8
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PD9,PD8");
  #define GPIO_Rx GPIO('D', 9)
  #define GPIO_Tx GPIO('D', 8)
  #define USARTx_FUNCTION GPIO_FUNCTION(CONFIG_MACH_STM32G0 ? 0 : 7)
  #define USARTx USART3
  #define USARTx_IRQn USART3_IRQn
#elif CONFIG_STM32_SERIAL_USART3_ALT_PC11_PC10
  //  Currently only supports STM32G474.
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PC11,PC10");
  #define GPIO_Rx GPIO('C', 11)
  #define GPIO_Tx GPIO('C', 10)
  #define USARTx_FUNCTION GPIO_FUNCTION(7)
  #define USARTx USART3
  #define USARTx_IRQn USART3_IRQn
#elif CONFIG_STM32_SERIAL_UART4
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PA1,PA0");
  #define GPIO_Rx GPIO('A', 1)
  #define GPIO_Tx GPIO('A', 0)
  #define USARTx_FUNCTION GPIO_FUNCTION(8)
  #define USARTx UART4
  #define USARTx_IRQn UART4_IRQn
#elif CONFIG_STM32_SERIAL_USART5
  DECL_CONSTANT_STR("RESERVE_PINS_serial", "PD2,PD3");
  #define GPIO_Rx GPIO('D', 2)
  #define GPIO_Tx GPIO('D', 3)
  #define USARTx_FUNCTION GPIO_FUNCTION(3)
  #define USARTx USART5
  #define USARTx_IRQn USART5_IRQn
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

#if CONFIG_MACH_STM32F031
  // The stm32f031 has same pins for USART2, but everything is routed to USART1
  #define USART2 USART1
  #define USART2_IRQn USART1_IRQn
#endif

#if CONFIG_MACH_STM32G0
  // Some of the stm32g0 MCUs have slightly different register names
  #if CONFIG_MACH_STM32G0B1
    #define USART2_IRQn USART2_LPUART2_IRQn
    #define USART3_IRQn USART3_4_5_6_LPUART1_IRQn
    #define USART4_IRQn USART3_4_5_6_LPUART1_IRQn
    #define USART5_IRQn USART3_4_5_6_LPUART1_IRQn
    #define USART6_IRQn USART3_4_5_6_LPUART1_IRQn
  #endif
  #if CONFIG_MACH_STM32G0B0
    #define USART2_IRQn USART2_IRQn
    #define USART3_IRQn USART3_4_5_6_IRQn
    #define USART4_IRQn USART3_4_5_6_IRQn
    #define USART5_IRQn USART3_4_5_6_IRQn
    #define USART6_IRQn USART3_4_5_6_IRQn
  #endif
  #define USART_CR1_RXNEIE USART_CR1_RXNEIE_RXFNEIE
  #define USART_CR1_TXEIE USART_CR1_TXEIE_TXFNFIE
  #define USART_ISR_RXNE USART_ISR_RXNE_RXFNE
  #define USART_ISR_TXE USART_ISR_TXE_TXFNF
  #define USART_BRR_DIV_MANTISSA_Pos 4
  #define USART_BRR_DIV_FRACTION_Pos 0
#elif CONFIG_MACH_STM32G4
  #define USART_BRR_DIV_MANTISSA_Pos 4
  #define USART_BRR_DIV_FRACTION_Pos 0
#elif CONFIG_MACH_STM32H7
  // The stm32h7 has slightly different register names
  #define USART_ISR_RXNE USART_ISR_RXNE_RXFNE
  #define USART_ISR_TXE USART_ISR_TXE_TXFNF
#endif

#if defined(LPUART_BRR) && CONFIG_MACH_STM32U585
  // 2B c1: Stop2 autonomous RX wake -- UESM/FIFOEN must stay set on
  // every CR1 write (including the ISR's "turn off TXEIE" write-back),
  // or a TX-complete interrupt would silently clear them.
  #define CR1_FLAGS (USART_CR1_UE | USART_CR1_RE | USART_CR1_TE     \
                     | USART_CR1_RXNEIE | USART_CR1_UESM            \
                     | USART_CR1_FIFOEN)
#else
  #define CR1_FLAGS (USART_CR1_UE | USART_CR1_RE | USART_CR1_TE   \
                     | USART_CR1_RXNEIE)
#endif

void
USARTx_IRQHandler(void)
{
    uint32_t sr = USARTx->ISR;
    // 2B c1: FIFOEN=1 時這個 bit 是 RXFNE（FIFO 非空），一次中斷可能
    // 已經累積多筆（RXFT 門檻式喚醒），必須排空到 FIFO 真的空了為止
    // ——非 FIFO 模式下這個迴圈頂多跑一次，行為不變。
    while (sr & USART_ISR_RXNE) {
        serial_rx_byte(USARTx->RDR);
        sr = USARTx->ISR;
    }
    if (sr & USART_ISR_TXE && USARTx->CR1 & USART_CR1_TXEIE) {
        uint8_t data;
        int ret = serial_get_tx_byte(&data);
        if (ret)
            USARTx->CR1 = CR1_FLAGS;
        else
            USARTx->TDR = data;
    }
}

void
serial_enable_tx_irq(void)
{
    USARTx->CR1 = CR1_FLAGS | USART_CR1_TXEIE;
}

void
serial_init(void)
{
    enable_pclock((uint32_t)USARTx);
#if defined(LPUART_BRR) && CONFIG_MACH_STM32U585
    // 2B c1: APB3SMENR/SRDAMR LPUART1SMEN/AMEN -- required for LPUART1
    // to wake the MCU from Stop modes (RM0456 lines 39384-39389 /
    // 39541-39546). See docs/2b_step2_impl_spec.md section 2.
    lpuart1_enable_stop_wake();
#endif

    uint32_t pclk = get_pclock_frequency((uint32_t)USARTx);
#if defined(LPUART_BRR)
    USARTx->BRR = DIV_ROUND_CLOSEST((uint64_t)pclk * 256, CONFIG_SERIAL_BAUD) & 0xFFFFF;
#else
    uint32_t div = DIV_ROUND_CLOSEST(pclk, CONFIG_SERIAL_BAUD);
    USARTx->BRR = (((div / 16) << USART_BRR_DIV_MANTISSA_Pos)
                   | ((div % 16) << USART_BRR_DIV_FRACTION_Pos));
#endif
#if defined(LPUART_BRR) && CONFIG_MACH_STM32U585
    // 2B c1: FIFO + RXFT 門檻式喚醒（RXFTCFG=000=1/8 深度=1 byte，
    // 理由見 docs/2b_step2_impl_spec.md 設計決策 2）。RXFTCFG/FIFOEN
    // 只能在 UE=0 時寫（此時 CR1 UE 還是 reset 值 0），OVRDIS 維持 1
    // 不變（設計決策 1）。
    USARTx->CR3 = USART_CR3_OVRDIS | USART_CR3_RXFTCFG_000
                  | USART_CR3_RXFTIE;
    USARTx->CR1 = USART_CR1_FIFOEN;   // UE 還是 0，先單獨開 FIFO
    USARTx->CR1 = CR1_FLAGS;          // 這裡已經含 UESM|FIFOEN
#else
    USARTx->CR3 = USART_CR3_OVRDIS; // disable the ORE ISR
    USARTx->CR1 = CR1_FLAGS;
#endif
    armcm_enable_irq(USARTx_IRQHandler, USARTx_IRQn, 0);

    gpio_peripheral(GPIO_Rx, USARTx_FUNCTION, 1);
    gpio_peripheral(GPIO_Tx, USARTx_FUNCTION, 0);
}
DECL_INIT(serial_init);
