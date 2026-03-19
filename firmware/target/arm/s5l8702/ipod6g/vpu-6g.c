/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * S5L8702 VPU-B IRQ 35 completion handler.
 *
 * IRQ 35 = VIC1 bit 3, edge-triggered. Fired by VPU-B (0x39800000)
 * on H.264 decode completion. Apple's ISR at SRAM 0x08035bb8 does
 * the same: read STATUS1, ack edge, signal semaphore.
 *
 * Pattern: ATA driver (storage_ata-6g.c lines 889-898, 1318-1335).
 * default_interrupt(INT_IRQ35) at system-s5l8702.c:77 is overridden
 * by our strong definition of INT_IRQ35() below.
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

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "s5l87xx.h"

#define VPU_B_STATUS1  (*(volatile uint32_t *)(0x39800000 + 0xF4))

static struct semaphore vpu_complete;
static volatile uint32_t vpu_last_status1;

void vpu_irq_init(void)
{
    semaphore_init(&vpu_complete, 1, 0);
    vpu_last_status1 = 0;
    VIC1INTENCLEAR = (1 << 3);  /* disable IRQ 35 until armed */
    VIC1EDGE0 |= (1 << 3);     /* configure as edge-triggered */
    VIC1EDGE1 = (1 << 3);      /* clear any pending edge */
}

void vpu_irq_arm(void)
{
    semaphore_wait(&vpu_complete, 0);  /* drain stale signal */
    VIC1EDGE1 = (1 << 3);             /* clear pending edge */
    VIC1INTENABLE = (1 << 3);         /* enable IRQ 35 */
}

int vpu_irq_wait(int timeout_ticks)
{
    int ret = semaphore_wait(&vpu_complete, timeout_ticks);
    VIC1INTENCLEAR = (1 << 3);  /* disable until next arm */
    return ret;
}

uint32_t vpu_irq_status1(void)
{
    return vpu_last_status1;
}

void ICODE_ATTR INT_IRQ35(void)
{
    vpu_last_status1 = VPU_B_STATUS1;
    VIC1EDGE1 = (1 << 3);      /* acknowledge edge */
    VIC1INTENCLEAR = (1 << 3); /* disable until re-armed */
    semaphore_release(&vpu_complete);
}
