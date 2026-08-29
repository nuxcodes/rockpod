/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Alan Korr & Nick Robinson
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "button.h"
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "serial.h"
#include "iap.h"
#include "adc.h"

#if defined(IPOD_ACCESSORY_PROTOCOL)
struct ppuart {
    volatile unsigned long *RBR_THR_DLL;
    volatile unsigned long *LCR;
    volatile unsigned long *LSR;
    int autobaud;
    bool auto_bitrate;
    int badbaud;
    bool newpkt;
};

static struct ppuart SER0 = {
    &SER0_RBR, &SER0_LCR, &SER0_LSR, 0, false, 0, true
};
#if defined(IPOD_COLOR) || defined(IPOD_4G) || defined(IPOD_MINI) || defined(IPOD_MINI2G)
static struct ppuart SER1 = {
    &SER1_RBR, &SER1_LCR, &SER1_LSR, 0, false, 0, true
};
static volatile struct ppuart *SERn = &SER1; // ie dock connector
#else
static volatile struct ppuart *SERn = &SER0;
#endif

#define UART_LSR_DATA_READY  0x01
#define UART_LSR_OVERRUN     0x02
#define UART_LSR_PARITY      0x04
#define UART_LSR_FRAMING     0x08
#define UART_LSR_BREAK       0x10
#define UART_LSR_BYTE_ERRORS \
    (UART_LSR_PARITY | UART_LSR_FRAMING | UART_LSR_BREAK)

#ifndef IAP_UART_READ
#define IAP_UART_READ(port) (*(port)->RBR_THR_DLL)
#endif

/* On these iPods iAP runs over the dock connector -- see the pin
 * routing in serial_setup() below -- and nothing told the protocol
 * layer when the accessory went away.
 *
 * iap_reset_state() has two other callers and neither one covers it:
 * firmware/usb.c on USB extract, and firmware/drivers/button.c on a
 * headphone unplug, which is compiled only for the 4G, mini and colour
 * iPods. Those have their remote on the headphone jack; here the jack
 * has nothing to do with the dock, so that hook would be both wrong and
 * absent.
 *
 * So the whole session survived a detach. The next accessory inherited
 * the previous one's authentication state, its negotiated lingoes, its
 * notification masks and its transaction-ID counter -- and MFi 4.3.11
 * (p.255) is explicit that "on accessory detach, event notification is
 * reset to the default disabled state". The 6G got this in serial-6g.c;
 * this is the same fix for the PP iPods whose iAP is on the dock.
 *
 * The detect is the one already used against this signal elsewhere in
 * the tree: ipod_remote_tuner.c treats adc_read(ADC_ACCESSORY) >= 10 as
 * the accessory having gone.
 *
 * iap_reset_state() reaches serial_bitrate(), which writes the divisor
 * latch. That is safe from the tick here because it only runs on the
 * detach edge, when the accessory is gone and the line is idle by
 * definition -- there is no in-flight byte for it to corrupt. */
#if defined(IPOD_NANO) || defined(IPOD_VIDEO)
static bool iap_acc_plugged = false;

/* Called from the iAP thread, never from a tick.
 *
 * This ran as a tick_add_task() when it was first written, and that
 * panics the device inside 400 ms of boot. Tick tasks run from
 * TIMER1() in the timer IRQ (firmware/target/arm/pp/kernel-pp.c), and
 * adc_read() here reaches _adc_read() in
 * firmware/target/arm/ipod/adc-ipod-pcf.c, which takes i2c_lock() ->
 * mutex_lock() whenever its 400 ms cache has expired --
 * ASSERT_CPU_MODE(CPU_MODE_THREAD_CONTEXT) in firmware/kernel/mutex.c
 * is a real panicf on ARM classic. The 6G version of this is safe only
 * because pmu_accessory_present() returns a cached int and touches no
 * bus. Nothing in a host test or a compiler can see that difference,
 * which is how it shipped. */
void iap_accessory_poll(void)
{
    bool plugged = (adc_read(ADC_ACCESSORY) < 10);

    if (iap_acc_plugged != plugged)
    {
        iap_acc_plugged = plugged;
        if (!plugged)
            iap_reset_state(IF_IAP_MP(0));
    }
}
#endif

static void set_bitrate(volatile struct ppuart *port, unsigned int rate)
{
    unsigned int divisor;
    int level;

    divisor = 24000000L / rate / 16;

    /* With interrupts off across the divisor latch window.
     *
     * pp5020.h aliases SER0_RBR, SER0_THR and SER0_DLL to the same
     * address, 0x70006000, so which one a read returns is decided by
     * the divisor-latch bit in LCR. Between the two LCR writes below
     * that bit is set, and the Rx ISR's drain loop is
     *
     *     while (*SERn->LSR & 0x1)
     *         temp = *SERn->RBR_THR_DLL & 0xFF;
     *
     * -- a read that with the latch enabled returns the divisor and
     * does not pop the Rx FIFO. Data Ready stays asserted, the loop
     * never exits, and nothing ever clears the latch again. The device
     * hangs.
     *
     * Reachable without anything misbehaving: an accessory plugged in
     * before boot is already transmitting (MFi 2.2.1, p.88), and both
     * iap_setup() at apps/main.c and iap_reset_state() on USB extract
     * reach serial_bitrate() from thread context.
     *
     * The 6G is immune for a structural reason rather than a careful
     * one: its UART has a separate UBRDIV register, so there is no
     * window to protect.
     *
     * Nested calls are fine -- the autobaud arms of the ISR itself call
     * this, and disable_irq_save() returns the level to restore. */
    level = disable_irq_save();
    *port->LCR = 0x80; /* Divisor latch enable */
    *port->RBR_THR_DLL = (divisor >> 0) & 0xFF;
    *port->LCR = 0x03; /* Divisor latch disable, 8-N-1 */
    restore_irq(level);
}

static void arm_autobaud(volatile struct ppuart *port)
{
    port->auto_bitrate = true;
    port->autobaud = 2;
    port->badbaud = 0;
    port->newpkt = true;
    set_bitrate(port, 115200);
}

void serial_setup (void)
{
    int tmp;

#if defined(IPOD_NANO) || defined(IPOD_VIDEO)
    /* Route the Tx/Rx pins. 5G Ipods. ser0, dock conncetor */
    (*(volatile unsigned long *)(0x7000008C)) &= ~0x0C;
    GPO32_ENABLE &= ~0x0C;

    DEV_EN = DEV_EN | DEV_SER0;
    CPU_HI_INT_DIS = SER0_MASK;

    DEV_RS |= DEV_SER0;
    sleep(1);
    DEV_RS &= ~DEV_SER0;

    SER0_LCR = 0x80; /* Divisor latch enable */
    SER0_DLM = 0x00;
    SER0_LCR = 0x03; /* Divisor latch disable, 8-N-1 */
    SER0_IER = 0x01;

    SER0_FCR = 0x07; /* Tx+Rx FIFO reset and FIFO enable */

    CPU_INT_EN = HI_MASK;
    CPU_HI_INT_EN = SER0_MASK;
    tmp = SER0_RBR;

    arm_autobaud(&SER0);

#elif defined(IPOD_COLOR) || defined(IPOD_4G) || defined(IPOD_MINI) || defined(IPOD_MINI2G)

    /* Route the Tx/Rx pins. 4G Ipods, MINI & MINI2G. ser1, dock connector */
    GPIO_CLEAR_BITWISE(GPIOD_ENABLE, 0x6);
    GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_EN, 0x6);
    GPIOD_INT_CLR = 0x6;

    outl(0x70000018, inl(0x70000018) & ~0xc00);

    DEV_EN |= DEV_SER1;
    CPU_HI_INT_DIS = SER1_MASK;

    DEV_RS |= DEV_SER1;
    sleep(1);
    DEV_RS &= ~DEV_SER1;

    SER1_LCR = 0x80; /* Divisor latch enable */
    SER1_DLM = 0x00;
    SER1_LCR = 0x03; /* Divisor latch disable, 8-N-1 */
    SER1_IER = 0x01;

    SER1_FCR = 0x07; /* Tx+Rx FIFO reset and FIFO enable */

    CPU_INT_EN = HI_MASK;
    CPU_HI_INT_EN = SER1_MASK;
    tmp = SER1_RBR;

    /* Route the Tx/Rx pins.  4G Ipod, ser0, top connector */
    GPIO_CLEAR_BITWISE(GPIOC_INT_EN, 0x8);
    GPIO_CLEAR_BITWISE(GPIOC_INT_LEV, 0x8);
    GPIOC_INT_CLR = 0x8;

    DEV_EN |= DEV_SER0;
    CPU_HI_INT_DIS = SER0_MASK;

    DEV_RS |= DEV_SER0;
    sleep(1);
    DEV_RS &= ~DEV_SER0;

    SER0_LCR = 0x80; /* Divisor latch enable */
    SER0_DLM = 0x00;
    SER0_LCR = 0x03; /* Divisor latch disable, 8-N-1 */
    SER0_IER = 0x01;

    SER0_FCR = 0x07; /* Tx+Rx FIFO reset and FIFO enable */

    CPU_INT_EN = HI_MASK;
    CPU_HI_INT_EN = SER0_MASK;
    tmp = SER0_RBR;

    arm_autobaud(&SER1);
    arm_autobaud(&SER0);

#endif

    (void)tmp;

}

void serial_bitrate(int rate)
{
    if(rate == 0)
    {
        arm_autobaud(&SER0);
#if defined(IPOD_COLOR) || defined(IPOD_4G) || defined(IPOD_MINI) || defined(IPOD_MINI2G)
        arm_autobaud(&SER1);
#endif
    }
    else
    {
        SER0.auto_bitrate = false;
        SER0.autobaud = 0;
        SER0.badbaud = 0;
        SER0.newpkt = true;
        set_bitrate(&SER0, rate);
#if defined(IPOD_COLOR) || defined(IPOD_4G) || defined(IPOD_MINI) || defined(IPOD_MINI2G)
        SER1.auto_bitrate = false;
        SER1.autobaud = 0;
        SER1.badbaud = 0;
        SER1.newpkt = true;
        set_bitrate(&SER1, rate);
#endif
    }
}

int tx_rdy(void)
{
    if((*SERn->LSR & 0x20))
        return 1;
    else
        return 0;
}

void tx_writec(unsigned char c)
{
    *SERn->RBR_THR_DLL = (int)c;
}

void SERIAL_ISR(int port)
{
    /* Unsigned: the autobaud switch below has cases 0xFF, 0xFC and
     * 0xE0, and on a signed char those are negative and can never
     * match. It works on the target because char is unsigned in the ARM
     * ABI -- autobaud would not function otherwise -- but the code
     * should not depend on that, and a host compiler warns about every
     * one of those cases. */
    unsigned char temp;
    unsigned long line_status;

#ifdef HAVE_IAP_MULTIPORT
    if (port && SERn != &SER1)
        SERn = &SER1;
    else if (!port && SERn != &SER0)
        SERn = &SER0;
    port = !port;  /* UART0 is headphone, ie IAP1 */
#else
    (void)port;
#endif

    while((line_status = *SERn->LSR) & UART_LSR_DATA_READY)
    {
        temp = IAP_UART_READ(SERn) & 0xFF;

        if (line_status & (UART_LSR_OVERRUN | UART_LSR_BYTE_ERRORS))
        {
            iap_rx_flush();
            SERn->badbaud = 0;
            SERn->newpkt = true;

            if ((line_status & UART_LSR_FRAMING) && SERn->auto_bitrate)
            {
                while (*SERn->LSR & UART_LSR_DATA_READY)
                    (void)IAP_UART_READ(SERn);
                arm_autobaud(SERn);
                break;
            }

            if (line_status & UART_LSR_BYTE_ERRORS)
                continue;
        }

        if (SERn->newpkt && SERn->autobaud > 0)
        {
            if (SERn->autobaud == 1)
            {
                switch (temp)
                {
                    case 0xFF:
                    case 0x55:
                        break;
                    case 0xFC:
                        set_bitrate(SERn, 19200);
                        temp = 0xFF;
                        break;
                    case 0xE0:
                        set_bitrate(SERn, 9600);
                        temp = 0xFF;
                        break;
                    default:
                        SERn->badbaud++;
                        if (SERn->badbaud >= 6) /* Switch baud detection mode */
                        {
                            SERn->autobaud = 2;
                            set_bitrate(SERn, 115200);
                            SERn->badbaud = 0;
                        } else {
                            set_bitrate(SERn, 57600);
                        }
                        continue;
                }
            } else {
                switch (temp)
                {
                    case 0xFF:
                    case 0x55:
                        break;
                    case 0xFE:
                        set_bitrate(SERn, 57600);
                        temp = 0xFF;
                        break;
                    case 0xFC:
                        set_bitrate(SERn, 38400);
                        temp = 0xFF;
                        break;
                    case 0xE0:
                        set_bitrate(SERn, 19200);
                        temp = 0xFF;
                        break;
                    default:
                        SERn->badbaud++;
                        if (SERn->badbaud >= 6) /* Switch baud detection */
                        {
                            SERn->autobaud = 1;
                            set_bitrate(SERn, 57600);
                            SERn->badbaud = 0;
                        } else {
                            set_bitrate(SERn, 115200);
                        }
                        continue;
                }
            }
        }
        bool pkt = iap_getc(IF_IAP_MP(port,) temp);
        if(SERn->newpkt && !pkt)
            SERn->autobaud = 0; /* Found good baud */
        SERn->newpkt = pkt;
    }
}

#ifdef IAP_UART_TEST
void iap_uart_test_reset(void)
{
    SERn = &SER0;
    SER0.autobaud = 0;
    SER0.auto_bitrate = false;
    SER0.badbaud = 0;
    SER0.newpkt = true;
    iap_acc_plugged = false;
}

void iap_uart_test_set_autobaud(int mode)
{
    SER0.autobaud = mode;
    SER0.auto_bitrate = mode != 0;
    SER0.badbaud = 0;
    SER0.newpkt = true;
}

void iap_uart_test_set_accessory_present(bool present)
{
    iap_acc_plugged = present;
}

int iap_uart_test_get_autobaud(void)
{
    return SER0.autobaud;
}

bool iap_uart_test_get_auto_bitrate(void)
{
    return SER0.auto_bitrate;
}
#endif
#endif
