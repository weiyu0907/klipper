#ifndef __STM32_INTERNAL_H
#define __STM32_INTERNAL_H
// Local definitions for STM32 code

#include "autoconf.h"

#include <stdint.h>
#if defined(CONFIG_MACH_STM32U585)
// --- [Uno Q 碩論: 全硬體模組暴力映射補丁] ---

// 1. 核心定義
#ifndef IRQn_Type
typedef enum { NonMaskableInt_IRQn=-14, HardFault_IRQn=-13, SVCall_IRQn=-5, PendSV_IRQn=-2, SysTick_IRQn=-1, WWDG_IRQn=0 } IRQn_Type;
#endif
#define __NVIC_PRIO_BITS 4U
#define __FPU_PRESENT 1U
#ifndef __CORTEX_M
#define __CORTEX_M 4U
#endif
#include "core_cm4.h"

// 2. ADC 暫存器 (對齊 stm32/adc.c)
typedef struct { volatile uint32_t SR, CR1, CR2, SMPR1, SMPR2, JOFR1, JOFR2, JOFR3, JOFR4, HTR, LTR, SQR1, SQR2, SQR3, JSQR, JDR1, JDR2, JDR3, JDR4, DR; } ADC_TypeDef;
typedef struct { volatile uint32_t CCR; } ADC_Common_TypeDef;
#define ADC1 ((ADC_TypeDef *)0x42028000)
#define ADC1_BASE 0x42028000
#define ADC123_COMMON ((ADC_Common_TypeDef *)(0x42028000 + 0x300))
#define ADC_SR_STRT 0x10
#define ADC_SR_EOC 0x02
#define ADC_CR2_ADON 0x01
#define ADC_CR2_SWSTART 0x400000
#define ADC_CCR_TSVREFE 0x800000

// 3. SPI 暫存器 (對齊 stm32/spi.c)
typedef struct { volatile uint32_t CR1, CR2, SR, DR, CRCPR, RXCRCR, TXCRCR, I2SCFGR, I2SPR; } SPI_TypeDef;
#define SPI1 ((SPI_TypeDef *)0x40013000)
#define SPI_CR1_CPHA_Pos 0
#define SPI_CR1_BR_Pos 3
#define SPI_CR1_SPE 0x40
#define SPI_CR1_MSTR 0x04
#define SPI_CR1_SSM 0x200
#define SPI_CR1_SSI 0x100
#define SPI_SR_RXNE 0x01
#define SPI_SR_TXE 0x02
#define SPI_SR_BSY 0x80

// 4. I2C 暫存器 (對齊 stm32/stm32f0_i2c.c)
typedef struct { volatile uint32_t CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, ISR, ICR, PECR, RXDR, TXDR; } I2C_TypeDef;
#define I2C1 ((I2C_TypeDef *)0x40005400)
#define I2C_TIMINGR_PRESC_Pos 28
#define I2C_TIMINGR_SCLL_Pos 0
#define I2C_TIMINGR_SCLH_Pos 8
#define I2C_TIMINGR_SDADEL_Pos 16
#define I2C_TIMINGR_SCLDEL_Pos 20
#define I2C_CR1_PE 0x01
#define I2C_CR2_START 0x2000
#define I2C_CR2_STOP 0x4000
#define I2C_CR2_NBYTES_Pos 16
#define I2C_CR2_AUTOEND 0x2000000
#define I2C_CR2_RD_WRN 0x400
#define I2C_ISR_TXIS 0x02
#define I2C_ISR_TXE 0x01
#define I2C_ISR_RXNE 0x04
#define I2C_ISR_TC 0x40
#define I2C_ISR_NACKF 0x10      // 新增：I2C NACK 狀態位元
#define I2C_ICR_NACKCF 0x10     // 新增：I2C NACK 清除位元
#define I2C_ISR_STOPF 0x20

// 5. LPUART1 (APB3 @ 0x46002400) for stm32f0_serial.c
// Register layout matches newer USART (ISR/RDR/TDR style)
typedef struct {
    volatile uint32_t CR1;    // 0x00
    volatile uint32_t CR2;    // 0x04
    volatile uint32_t CR3;    // 0x08
    volatile uint32_t BRR;    // 0x0C  BRR = 256*pclk/baud for LPUART
    volatile uint32_t PRESC;  // 0x10
    volatile uint32_t _res;   // 0x14
    volatile uint32_t RQR;    // 0x18
    volatile uint32_t ISR;    // 0x1C
    volatile uint32_t ICR;    // 0x20
    volatile uint32_t RDR;    // 0x24
    volatile uint32_t TDR;    // 0x28
} LPUART_TypeDef;
#define LPUART1 ((LPUART_TypeDef *)0x46002400UL)
#define LPUART1_IRQn 66
void lpuart1_handler(void);
#define LPUART1_IRQHandler lpuart1_handler

#define USART_ISR_RXNE   0x0020U   // FIFOEN=1 時同一個 bit 代表 RXFNE
#define USART_ISR_TXE    0x0080U
#define USART_ISR_FE     0x0002U   // ISR bit1，RM0456 行 189346
#define USART_ISR_NE     0x0004U   // ISR bit2，RM0456 行 189336
#define USART_ISR_ORE    0x0008U   // ISR bit3，RM0456 行 189323（OVRDIS=1 時永遠鎖 0，行 189329-189331）
#define USART_ISR_TC     0x0040U   // ISR bit6，RM0456 行 189273
#define USART_ISR_RXFT   0x04000000U // ISR bit26，RM0456 行 189140
#define USART_ICR_FECF   0x0002U   // ICR bit1，RM0456 行 189639
#define USART_ICR_NECF   0x0004U   // ICR bit2，RM0456 行 189636
#define USART_ICR_ORECF  0x0008U   // ICR bit3，RM0456 行 189623
#define USART_CR1_UE     0x0001U
#define USART_CR1_UESM   0x0002U   // CR1 bit1，RM0456 行 188386
#define USART_CR1_RE     0x0004U
#define USART_CR1_TE     0x0008U
#define USART_CR1_RXNEIE 0x0020U
#define USART_CR1_TXEIE  0x0080U
#define USART_CR1_FIFOEN 0x20000000U // CR1 bit29，RM0456 行 188234
#define USART_CR3_OVRDIS 0x1000U
#define USART_CR3_RXFTIE 0x10000000U // CR3 bit28，RM0456 行 188763
#define USART_CR3_RXFTCFG_000 0x00000000U // CR3 bits27:25=000（1/8深度），行 188781-188782
#define USART_BRR_DIV_MANTISSA_Pos 4
#define USART_BRR_DIV_FRACTION_Pos 0

// 2B c1：LPUART1 的 APB3SMENR/SRDAMR 位元致能（實作於 u5_main.c，
// 跟 LPTIM1 在 stm32u5_lowpower.c 的直寫模式一致，見
// docs/2b_step2_impl_spec.md 第 2 節）——已經在
// #if defined(CONFIG_MACH_STM32U585) 區塊內，不需要再包一層
void lpuart1_enable_stop_wake(void);

// 6. GPIO & Watchdog
typedef struct { volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2], BRR; } GPIO_TypeDef;
#define GPIOA ((GPIO_TypeDef *)0x42020000)
#define GPIOB ((GPIO_TypeDef *)0x42020400)
#define GPIOC ((GPIO_TypeDef *)0x42020800)
#define GPIOD ((GPIO_TypeDef *)0x42020C00)
#define GPIOE ((GPIO_TypeDef *)0x42021000)
#define GPIOF ((GPIO_TypeDef *)0x42021400)
#define GPIOG ((GPIO_TypeDef *)0x42021800)
#define GPIOH ((GPIO_TypeDef *)0x42021C00)
#define GPIOI ((GPIO_TypeDef *)0x42022000)
typedef struct { volatile uint32_t KR, PR, RLR, SR, WINR; } IWDG_TypeDef;
#define IWDG ((IWDG_TypeDef *)0x40003000)

// 7. USB OTG FS
#include "cmsis_u5/stm32u5_usb.h"
// 8. Unique Device ID
#define UID_BASE 0x0BFA0700UL

#endif
 // CONFIG_MACH_STM32F1

#if CONFIG_MACH_STM32F0
#include "stm32f0xx.h"
#elif CONFIG_MACH_STM32F1
#include "stm32f1xx.h"
#elif CONFIG_MACH_STM32F2
#include "stm32f2xx.h"
#elif CONFIG_MACH_STM32F4
#include "stm32f4xx.h"
#elif CONFIG_MACH_STM32F7
#include "stm32f7xx.h"
#elif CONFIG_MACH_STM32G0
#include "stm32g0xx.h"
#elif CONFIG_MACH_STM32G4
#include "stm32g4xx.h"
#elif CONFIG_MACH_STM32H7
#include "stm32h7xx.h"
#elif CONFIG_MACH_STM32L4
#include "stm32l4xx.h"
#endif

// gpio.c
GPIO_TypeDef *gpio_pin_to_regs(uint32_t pin);
#define GPIO(PORT, NUM) (((PORT)-'A') * 16 + (NUM))
#define GPIO2PORT(PIN) ((PIN) / 16)
#define GPIO2BIT(PIN) (1<<((PIN) % 16))

// gpioperiph.c
#define GPIO_INPUT 0
#define GPIO_OUTPUT 1
#define GPIO_OPEN_DRAIN 0x100
#define GPIO_HIGH_SPEED 0x200
#define GPIO_FUNCTION(fn) (2 | ((fn) << 4))
#define GPIO_ANALOG 3
void gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup);

// clockline.c
void enable_pclock(uint32_t periph_base);
int is_enabled_pclock(uint32_t periph_base);

// dfu_reboot.c
void dfu_reboot(void);
void dfu_reboot_check(void);

// stm32??.c
struct cline { volatile uint32_t *en, *rst; uint32_t bit; };
struct cline lookup_clock_line(uint32_t periph_base);
uint32_t get_pclock_frequency(uint32_t periph_base);
void gpio_clock_enable(GPIO_TypeDef *regs);

#endif // internal.h
