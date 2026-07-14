/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * S5L8702 VPP — hardware compositor passthrough for YUV420 display.
 *
 ****************************************************************************/

#ifndef VPP_6G_H
#define VPP_6G_H

#include <stdbool.h>
#include <stdint.h>

void vpp_init(void);

void vpp_configure(int w, int h);

void vpp_enable(void);

void vpp_disable(void);

bool vpp_is_active(void);

void vpp_set_frame(const uint8_t *y, const uint8_t *cb, const uint8_t *cr);

void vpp_push_frame(void);

#endif /* VPP_6G_H */
