/* Host build of the PP dock UART path.
 *
 * firmware/target/arm/pp/uart-pp.c is the iAP transport on every PP
 * iPod. On the iPod Video it is the only one -- the USB HID transport
 * is not built for that target -- so the player whose entire iAP path
 * this is was the one never exercised by a test.
 *
 * config.h and cpu.h come in first, exactly as the driver would pull
 * them, and the fixed-address register macros from pp5020.h are then
 * redirected at a plain array. Their include guards make the driver's
 * own includes no-ops, so these definitions are the ones it compiles
 * against.
 */
#ifndef UART_PRELUDE_H
#define UART_PRELUDE_H

extern volatile unsigned long uartstub_reg[32];
unsigned long uartstub_rx_read(void);

#define IAP_UART_READ(port) uartstub_rx_read()

enum {
    R_SER0_RBR, R_SER0_LCR, R_SER0_LSR, R_SER0_DLM, R_SER0_FCR, R_SER0_IER,
    R_SER1_RBR, R_SER1_LCR, R_SER1_LSR, R_SER1_DLM, R_SER1_FCR, R_SER1_IER,
    R_DEV_EN, R_DEV_RS, R_CPU_INT_EN, R_CPU_HI_INT_EN, R_CPU_HI_INT_DIS,
    R_GPO32_ENABLE, R_GPIOD_ENABLE, R_GPIOD_OUTPUT_EN, R_GPIOD_INT_CLR,
    R_GPIOC_INT_CLR, R_GPIOC_INT_EN, R_GPIOC_INT_LEV,
    R_LAST
};


#define SER0_RBR   uartstub_reg[R_SER0_RBR]
#define SER0_LCR   uartstub_reg[R_SER0_LCR]
#define SER0_LSR   uartstub_reg[R_SER0_LSR]
#define SER0_DLM   uartstub_reg[R_SER0_DLM]
#define SER0_FCR   uartstub_reg[R_SER0_FCR]
#define SER0_IER   uartstub_reg[R_SER0_IER]
#define SER1_RBR   uartstub_reg[R_SER1_RBR]
#define SER1_LCR   uartstub_reg[R_SER1_LCR]
#define SER1_LSR   uartstub_reg[R_SER1_LSR]
#define SER1_DLM   uartstub_reg[R_SER1_DLM]
#define SER1_FCR   uartstub_reg[R_SER1_FCR]
#define SER1_IER   uartstub_reg[R_SER1_IER]
#define DEV_EN     uartstub_reg[R_DEV_EN]
#define DEV_RS     uartstub_reg[R_DEV_RS]
#define CPU_INT_EN uartstub_reg[R_CPU_INT_EN]
#define CPU_HI_INT_EN  uartstub_reg[R_CPU_HI_INT_EN]
#define CPU_HI_INT_DIS uartstub_reg[R_CPU_HI_INT_DIS]
#define GPO32_ENABLE   uartstub_reg[R_GPO32_ENABLE]
#define GPIOD_ENABLE     uartstub_reg[R_GPIOD_ENABLE]
#define GPIOD_OUTPUT_EN  uartstub_reg[R_GPIOD_OUTPUT_EN]
#define GPIOD_INT_CLR    uartstub_reg[R_GPIOD_INT_CLR]
#define GPIOC_INT_CLR    uartstub_reg[R_GPIOC_INT_CLR]
#define GPIOC_INT_EN     uartstub_reg[R_GPIOC_INT_EN]
#define GPIOC_INT_LEV    uartstub_reg[R_GPIOC_INT_LEV]

/* Bit masks the driver uses against those registers. Values are
 * arbitrary here -- what a case asserts is which bits the driver set,
 * not what the silicon does with them. */
#define DEV_SER0   0x01
#define DEV_SER1   0x02
#define SER0_MASK  0x10
#define SER1_MASK  0x20
#define HI_MASK    0x40

#define GPIO_CLEAR_BITWISE(reg, bits) \
    do { (reg) &= ~(unsigned long)(bits); } while (0)

#endif
