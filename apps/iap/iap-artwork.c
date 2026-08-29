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

#include "config.h"
#include "iap-artwork.h"
#include "iap-core.h"
#include "system.h"

static volatile uint32_t artwork_session;

uint32_t iap_artwork_session_id(void)
{
    int oldlevel = disable_irq_save();
    uint32_t session_id = artwork_session;

    restore_irq(oldlevel);
    return session_id;
}

#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR) \
 && LCD_DEPTH == 16 && LCD_PIXELFORMAT == RGB565

#include "albumart.h"
#include "bmp.h"
#include "file.h"
#include "jpeg_load.h"
#include "string-extra.h"

#define IAP_ARTWORK_BUFFER_SIZE \
    (IAP_ARTWORK_WIDTH * IAP_ARTWORK_HEIGHT * sizeof(fb_data) + 64 * 1024)

static unsigned char artwork_buffer[IAP_ARTWORK_BUFFER_SIZE] MEM_ALIGN_ATTR;

static struct {
    bool active;
    bool use_tid;
    unsigned char lingo;
    unsigned char tid_hi;
    unsigned char tid_lo;
    uint16_t request_command;
    uint16_t response_command;
    uint16_t packet_index;
    uint32_t session_id;
    uint32_t transfer_id;
    size_t payload_limit;
    size_t offset;
} artwork_tx;

static uint32_t next_transfer_id;

static bool artwork_is_jpeg(const char *path)
{
    const char *ext = strrchr(path, '.');

    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"));
}

static bool artwork_decode_file(const char *path, struct bitmap *bmp)
{
    int fd = open(path, O_RDONLY);
    int rc;

    if (fd < 0)
        return false;

    bmp->width = IAP_ARTWORK_WIDTH;
    bmp->height = IAP_ARTWORK_HEIGHT;
    bmp->data = artwork_buffer;
    rc = artwork_is_jpeg(path)
       ? read_jpeg_fd(fd, bmp, sizeof(artwork_buffer),
                      FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE, NULL)
       : read_bmp_fd(fd, bmp, sizeof(artwork_buffer),
                     FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE, NULL);
    close(fd);

    return rc > 0 && bmp->width == IAP_ARTWORK_WIDTH
                  && bmp->height == IAP_ARTWORK_HEIGHT;
}

#ifdef HAVE_JPEG
static bool artwork_decode_embedded(const struct mp3entry *id3,
                                    struct bitmap *bmp)
{
    const struct mp3_albumart *aa = &id3->albumart;
    int fd;
    int rc;

    if (!id3->path[0] || !id3->has_embedded_albumart
        || (aa->type & AA_CLEAR_FLAGS_MASK) != AA_TYPE_JPG)
        return false;

    fd = open(id3->path, O_RDONLY);
    if (fd < 0)
        return false;
    if (lseek(fd, aa->pos, SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    bmp->width = IAP_ARTWORK_WIDTH;
    bmp->height = IAP_ARTWORK_HEIGHT;
    bmp->data = artwork_buffer;
    rc = clip_jpeg_fd(fd, aa->type, aa->size, bmp,
                      sizeof(artwork_buffer),
                      FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE, NULL);
    close(fd);

    return rc > 0 && bmp->width == IAP_ARTWORK_WIDTH
                  && bmp->height == IAP_ARTWORK_HEIGHT;
}
#endif

static bool artwork_load(const struct mp3entry *id3, struct bitmap *bmp)
{
    char path[MAX_PATH];
    const struct dim dim = {
        .width = IAP_ARTWORK_WIDTH,
        .height = IAP_ARTWORK_HEIGHT,
    };

    memset(bmp, 0, sizeof(*bmp));

    if (id3 && id3->path[0]
        && find_albumart(id3, path, sizeof(path), &dim)
        && artwork_decode_file(path, bmp))
        return true;

#ifdef HAVE_JPEG
    if (id3 && artwork_decode_embedded(id3, bmp))
        return true;
#endif

    return false;
}

bool iap_artwork_supported(void)
{
    return true;
}

bool iap_artwork_available(const struct mp3entry *id3)
{
    char path[MAX_PATH];
    const struct dim dim = {
        .width = IAP_ARTWORK_WIDTH,
        .height = IAP_ARTWORK_HEIGHT,
    };

    if (!id3)
        return false;

#ifdef HAVE_JPEG
    if (id3->path[0] && id3->has_embedded_albumart
        && (id3->albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
        return true;
#endif

    return id3->path[0]
        && find_albumart(id3, path, sizeof(path), &dim);
}

static void artwork_tx_init(unsigned char lingo, uint16_t command,
                            bool use_tid, unsigned char tid_hi,
                            unsigned char tid_lo)
{
    if (lingo == 0x03)
        IAP_TX_INIT(0x03, command);
    else
        IAP_TX_INIT4(0x04, command);

    if (use_tid) {
        IAP_TX_PUT(tid_hi);
        IAP_TX_PUT(tid_lo);
    }
}

static unsigned char artwork_byte(size_t offset)
{
    uint16_t pixel = ((const fb_data *)artwork_buffer)[offset >> 1];

    return offset & 1 ? pixel >> 8 : pixel & 0xff;
}

enum iap_artwork_start_result
iap_artwork_start_transfer(unsigned char lingo, uint16_t request_command,
                           uint16_t response_command, bool use_tid,
                           unsigned char tid_hi, unsigned char tid_lo,
                           uint32_t session_id,
                           const struct mp3entry *id3)
{
    struct bitmap bmp;
    size_t payload_limit = TX_BUFLEN;
    uint32_t transfer_id;
    int oldlevel;

    oldlevel = disable_irq_save();
    if (session_id != artwork_session) {
        restore_irq(oldlevel);
        return IAP_ARTWORK_START_STALE;
    }
    if (artwork_tx.active) {
        restore_irq(oldlevel);
        return IAP_ARTWORK_START_BUSY;
    }
    restore_irq(oldlevel);

    if (!artwork_load(id3, &bmp))
        return session_id == iap_artwork_session_id()
             ? IAP_ARTWORK_START_FAILED : IAP_ARTWORK_START_STALE;

    if (device.acc_max_payload && device.acc_max_payload < payload_limit)
        payload_limit = device.acc_max_payload;

    oldlevel = disable_irq_save();
    if (session_id != artwork_session) {
        restore_irq(oldlevel);
        return IAP_ARTWORK_START_STALE;
    }
    if (artwork_tx.active) {
        restore_irq(oldlevel);
        return IAP_ARTWORK_START_BUSY;
    }

    transfer_id = ++next_transfer_id;
    if (transfer_id == 0)
        transfer_id = ++next_transfer_id;
    artwork_tx.active = true;
    artwork_tx.use_tid = use_tid;
    artwork_tx.lingo = lingo;
    artwork_tx.tid_hi = tid_hi;
    artwork_tx.tid_lo = tid_lo;
    artwork_tx.request_command = request_command;
    artwork_tx.response_command = response_command;
    artwork_tx.packet_index = 0;
    artwork_tx.session_id = session_id;
    artwork_tx.transfer_id = transfer_id;
    artwork_tx.payload_limit = payload_limit;
    artwork_tx.offset = 0;
    restore_irq(oldlevel);

    iap_schedule_artwork(transfer_id);
    return IAP_ARTWORK_START_OK;
}

void iap_artwork_send_next(uint32_t transfer_id)
{
    const size_t image_size = IAP_ARTWORK_WIDTH * IAP_ARTWORK_HEIGHT * 2;
    size_t payload_limit;
    size_t offset;
    size_t used;
    size_t chunk;
    uint32_t session_id;
    uint16_t packet_index;
    uint16_t response_command;
    unsigned char lingo;
    unsigned char tid_hi;
    unsigned char tid_lo;
    bool use_tid;
    bool more = false;
    int oldlevel;

    oldlevel = disable_irq_save();
    if (!artwork_tx.active || artwork_tx.transfer_id != transfer_id
        || artwork_tx.session_id != artwork_session) {
        restore_irq(oldlevel);
        return;
    }
    payload_limit = artwork_tx.payload_limit;
    offset = artwork_tx.offset;
    packet_index = artwork_tx.packet_index;
    session_id = artwork_tx.session_id;
    response_command = artwork_tx.response_command;
    lingo = artwork_tx.lingo;
    tid_hi = artwork_tx.tid_hi;
    tid_lo = artwork_tx.tid_lo;
    use_tid = artwork_tx.use_tid;
    restore_irq(oldlevel);

    artwork_tx_init(lingo, response_command, use_tid, tid_hi, tid_lo);
    IAP_TX_PUT_U16(packet_index);

    if (packet_index == 0) {
        IAP_TX_PUT(IAP_ARTWORK_PIXEL_FORMAT);
        IAP_TX_PUT_U16(IAP_ARTWORK_WIDTH);
        IAP_TX_PUT_U16(IAP_ARTWORK_HEIGHT);
        IAP_TX_PUT_U16(0);
        IAP_TX_PUT_U16(0);
        IAP_TX_PUT_U16(IAP_ARTWORK_WIDTH - 1);
        IAP_TX_PUT_U16(IAP_ARTWORK_HEIGHT - 1);
        IAP_TX_PUT_U32(IAP_ARTWORK_WIDTH * 2);
    }

    used = iap_txnext - iap_txpayload;
    if (used >= payload_limit) {
        iap_txnext = iap_txpayload;
        oldlevel = disable_irq_save();
        if (artwork_tx.transfer_id == transfer_id)
            artwork_tx.active = false;
        restore_irq(oldlevel);
        return;
    }

    chunk = image_size - offset;
    if (chunk > payload_limit - used)
        chunk = payload_limit - used;

    for (size_t i = 0; i < chunk; i++)
        IAP_TX_PUT(artwork_byte(offset + i));

    oldlevel = disable_irq_save();
    if (!artwork_tx.active || artwork_tx.transfer_id != transfer_id
        || artwork_tx.session_id != session_id
        || artwork_session != session_id) {
        iap_txnext = iap_txpayload;
        restore_irq(oldlevel);
        return;
    }
    restore_irq(oldlevel);

    iap_send_tx();

    oldlevel = disable_irq_save();
    if (artwork_tx.active && artwork_tx.transfer_id == transfer_id
        && artwork_tx.session_id == session_id
        && artwork_session == session_id) {
        artwork_tx.offset = offset + chunk;
        artwork_tx.packet_index = packet_index + 1;
        more = artwork_tx.offset < image_size;
        if (!more)
            artwork_tx.active = false;
    }
    restore_irq(oldlevel);

    if (more)
        iap_schedule_artwork(transfer_id);
}

bool iap_artwork_cancel(unsigned char lingo, uint16_t command,
                        unsigned char tid_hi, unsigned char tid_lo)
{
    bool cancelled;
    int oldlevel = disable_irq_save();

    cancelled = artwork_tx.active && artwork_tx.use_tid
             && artwork_tx.session_id == artwork_session
             && artwork_tx.lingo == lingo
             && artwork_tx.request_command == command
             && artwork_tx.tid_hi == tid_hi
             && artwork_tx.tid_lo == tid_lo;
    if (cancelled)
        artwork_tx.active = false;
    restore_irq(oldlevel);

    return cancelled;
}

void iap_artwork_reset(void)
{
    int oldlevel = disable_irq_save();

    artwork_tx.active = false;
    artwork_session++;
    restore_irq(oldlevel);
}

#else

bool iap_artwork_supported(void)
{
    return false;
}

bool iap_artwork_available(const struct mp3entry *id3)
{
    (void)id3;
    return false;
}

enum iap_artwork_start_result
iap_artwork_start_transfer(unsigned char lingo, uint16_t request_command,
                           uint16_t response_command, bool use_tid,
                           unsigned char tid_hi, unsigned char tid_lo,
                           uint32_t session_id,
                           const struct mp3entry *id3)
{
    (void)lingo;
    (void)request_command;
    (void)response_command;
    (void)use_tid;
    (void)tid_hi;
    (void)tid_lo;
    (void)session_id;
    (void)id3;
    return session_id == iap_artwork_session_id()
         ? IAP_ARTWORK_START_FAILED : IAP_ARTWORK_START_STALE;
}

void iap_artwork_send_next(uint32_t transfer_id)
{
    (void)transfer_id;
}

bool iap_artwork_cancel(unsigned char lingo, uint16_t command,
                        unsigned char tid_hi, unsigned char tid_lo)
{
    (void)lingo;
    (void)command;
    (void)tid_hi;
    (void)tid_lo;
    return false;
}

void iap_artwork_reset(void)
{
    int oldlevel = disable_irq_save();

    artwork_session++;
    restore_irq(oldlevel);
}

#endif
