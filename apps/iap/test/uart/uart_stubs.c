/* Stubs for the PP dock UART path. The driver is compiled unmodified;
 * everything below it is recorded so a case can assert on what it did.
 */
#include <string.h>
#include "uart_stubs.h"

volatile unsigned long uartstub_reg[32];

struct uartstub uartstub;

void uartstub_reset(void)
{
    memset((void *)uartstub_reg, 0, sizeof(uartstub_reg));
    memset(&uartstub, 0, sizeof(uartstub));
    uartstub.accessory_adc = 0;     /* attached; >= 10 means gone */
    iap_uart_test_reset();
}

/* The accessory-detect line. ipod_remote_tuner.c reads the same signal
 * and treats >= 10 as the accessory having gone, which is where the
 * threshold in the driver comes from. */
int adc_read(int channel)
{
    (void)channel;
    uartstub.adc_reads++;
    return uartstub.accessory_adc;
}

void iap_reset_state(void)
{
    uartstub.resets++;
    uartstub.framer_state = 0;
}

static void uartstub_load_current(void)
{
    if (uartstub.fed < uartstub.nfeed)
    {
        uartstub_reg[R_SER0_RBR] = uartstub.feed[uartstub.fed];
        uartstub_reg[R_SER0_LSR] = 0x01 | uartstub.errors[uartstub.fed];
    }
    else
    {
        uartstub_reg[R_SER0_LSR] = 0;
    }
}

void uartstub_feed_errors(const unsigned char *bytes,
                          const unsigned char *errors, int n)
{
    if (n < 0)
        n = 0;
    if (n > UARTSTUB_MAX_BYTES)
        n = UARTSTUB_MAX_BYTES;

    for (int i = 0; i < n; i++)
    {
        uartstub.feed[i] = bytes[i];
        uartstub.errors[i] = errors ? errors[i] : 0;
    }

    uartstub.nfeed = n;
    uartstub.fed = 0;
    uartstub_load_current();
}

void uartstub_feed(const unsigned char *bytes, int n)
{
    uartstub_feed_errors(bytes, NULL, n);
}

unsigned long uartstub_rx_read(void)
{
    unsigned long value = uartstub.fed < uartstub.nfeed
                        ? uartstub.feed[uartstub.fed]
                        : uartstub_reg[R_SER0_RBR];

    if (uartstub.fed < uartstub.nfeed)
        uartstub.fed++;
    uartstub_load_current();
    return value;
}

bool iap_getc(unsigned char c)
{
    if (uartstub.nbytes < UARTSTUB_MAX_BYTES)
        uartstub.bytes[uartstub.nbytes++] = c;

    if (uartstub.framer_state == 0)
    {
        if (c == 0xFF)
            uartstub.framer_state = 1;
        return true;
    }

    if (uartstub.framer_state == 1)
    {
        if (c == 0x55)
        {
            uartstub.framer_state = 2;
            return false;
        }

        uartstub.framer_state = c == 0xFF ? 1 : 0;
        return true;
    }

    return false;
}

void iap_rx_flush(void)
{
    uartstub.flushes++;
    uartstub.framer_state = 0;
}

void tick_add_task(void (*f)(void))
{
    uartstub.tick = f;
    uartstub.tick_adds++;
}

void sleep(int ticks) { (void)ticks; }

unsigned long inl(unsigned long addr) { (void)addr; return 0; }
void outl(unsigned long v, unsigned long addr) { (void)v; (void)addr; }
