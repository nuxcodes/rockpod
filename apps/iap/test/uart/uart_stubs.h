#ifndef UART_STUBS_H
#define UART_STUBS_H

#include <stdbool.h>

#define UARTSTUB_MAX_BYTES 512
#define UARTSTUB_LSR_OVERRUN 0x02
#define UARTSTUB_LSR_PARITY  0x04
#define UARTSTUB_LSR_FRAMING 0x08
#define UARTSTUB_LSR_BREAK   0x10
#define UARTSTUB_LSR_FIFOERR 0x80

struct uartstub {
    int  accessory_adc;     /* what adc_read(ADC_ACCESSORY) returns */
    int  adc_reads;
    int  resets;            /* iap_reset_state() calls */
    int  flushes;
    int  tick_adds;
    void (*tick)(void);     /* whatever serial_setup() registered */
    unsigned char bytes[UARTSTUB_MAX_BYTES];
    int  nbytes;            /* bytes handed to iap_getc() */
    unsigned char feed[UARTSTUB_MAX_BYTES];
    unsigned char errors[UARTSTUB_MAX_BYTES];
    int  nfeed, fed;        /* what the line still has to deliver */
    int  framer_state;
};

extern struct uartstub uartstub;
extern volatile unsigned long uartstub_reg[32];

void uartstub_reset(void);

/* Queue bytes on the receive line for SERIAL_ISR() to drain. */
void uartstub_feed(const unsigned char *bytes, int n);
void uartstub_feed_errors(const unsigned char *bytes,
                          const unsigned char *errors, int n);
unsigned long uartstub_rx_read(void);

void serial_setup(void);
void serial_bitrate(int rate);
void SERIAL_ISR(int port);
void iap_accessory_poll(void);
void iap_uart_test_reset(void);
void iap_uart_test_set_autobaud(int mode);
void iap_uart_test_set_accessory_present(bool present);
int iap_uart_test_get_autobaud(void);
bool iap_uart_test_get_auto_bitrate(void);

#endif
