/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__\/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Rockbox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 ****************************************************************************/

#ifndef IAP_ARTWORK_H
#define IAP_ARTWORK_H

#include <stdbool.h>
#include <stdint.h>
#include "metadata.h"

#define IAP_ARTWORK_FORMAT_ID     0x0404
#define IAP_ARTWORK_PIXEL_FORMAT  0x02
#define IAP_ARTWORK_WIDTH         56
#define IAP_ARTWORK_HEIGHT        56

enum iap_artwork_start_result {
    IAP_ARTWORK_START_FAILED,
    IAP_ARTWORK_START_OK,
    IAP_ARTWORK_START_STALE,
    IAP_ARTWORK_START_BUSY,
};

bool iap_artwork_supported(void);
bool iap_artwork_available(const struct mp3entry *id3);
uint32_t iap_artwork_session_id(void);
enum iap_artwork_start_result
iap_artwork_start_transfer(unsigned char lingo, uint16_t request_command,
                           uint16_t response_command, bool use_tid,
                           unsigned char tid_hi, unsigned char tid_lo,
                           uint32_t session_id,
                           const struct mp3entry *id3);
void iap_artwork_send_next(uint32_t transfer_id);
bool iap_artwork_cancel(unsigned char lingo, uint16_t command,
                        unsigned char tid_hi, unsigned char tid_lo);
void iap_artwork_reset(void);

#endif
