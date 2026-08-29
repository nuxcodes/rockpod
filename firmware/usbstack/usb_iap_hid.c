/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025
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

/*
 * USB HID transport for iPod Accessory Protocol (iAP).
 *
 * MFi DACs (like the Oppo HA-2SE) authenticate via iAP messages
 * sent over USB HID reports before activating the UAC1 audio stream.
 *
 * Based on the ipod-gadget reference implementation by Andrew Onyshchuk.
 * Report descriptor and protocol format match Apple's iPod firmware.
 */

#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb_class_driver.h"
#include "usb_ch9.h"
#include "iap.h"

/* #define LOGF_ENABLE */
#include "logf.h"
#include "kernel.h"

/* HID class-specific descriptor types */
#define HID_DT_HID    0x21
#define HID_DT_REPORT 0x22

/* HID class-specific requests */
#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_GET_IDLE     0x02
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A

/*
 * HID Report Descriptor — vendor-specific Usage Page 0xFF00.
 * Defines variable-length reports with multiple Report IDs:
 *
 * IN reports (device -> host):
 *   ID 1: 12 bytes, ID 2: 14 bytes, ID 3: 20 bytes, ID 4: 63 bytes
 *
 * OUT reports (host -> device, via SET_REPORT on EP0):
 *   ID 5: 8 bytes, ID 6: 10 bytes, ID 7: 14 bytes, ID 8: 20 bytes, ID 9: 63 bytes
 *
 * From the ipod-gadget reference, where this 96-byte table is labelled
 * "hid descriptor for usb full speed" -- oandrew's own construction, not
 * Apple's. Apple's real descriptor is 208 bytes with IN report IDs 1-12
 * up to 767 bytes; ipod-gadget replaced it with this one deliberately,
 * to stop some devices hanging after GetDevAuthenticationInfo. The table
 * is self-consistent and the 63-byte maximum is what makes a report fit
 * one 64-byte full-speed packet.
 */
static const unsigned char iap_hid_report_desc[] = {
    /* Logical Maximum 0x00FF, not 0x0080: these are opaque 8-bit iAP
     * bytes and half the range was declared out of bounds. */
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x75, 0x08, 0x26, 0xff, 0x00,
    0x15, 0x00, 0x09, 0x01, 0x85, 0x01, 0x95, 0x0c, 0x82, 0x02, 0x01, 0x09,
    0x01, 0x85, 0x02, 0x95, 0x0e, 0x82, 0x02, 0x01, 0x09, 0x01, 0x85, 0x03,
    0x95, 0x14, 0x82, 0x02, 0x01, 0x09, 0x01, 0x85, 0x04, 0x95, 0x3f, 0x82,
    0x02, 0x01, 0x09, 0x01, 0x85, 0x05, 0x95, 0x08, 0x92, 0x02, 0x01, 0x09,
    0x01, 0x85, 0x06, 0x95, 0x0a, 0x92, 0x02, 0x01, 0x09, 0x01, 0x85, 0x07,
    0x95, 0x0e, 0x92, 0x02, 0x01, 0x09, 0x01, 0x85, 0x08, 0x95, 0x14, 0x92,
    0x02, 0x01, 0x09, 0x01, 0x85, 0x09, 0x95, 0x3f, 0x92, 0x02, 0x01, 0xc0
};

/* IN report ID -> payload size mapping */
static const struct {
    uint8_t id;
    uint8_t size;
} in_report_sizes[] = {
    { 1, 12 },
    { 2, 14 },
    { 3, 20 },
    { 4, 63 },
};
#define NUM_IN_REPORTS 4

/* OUT report ID -> payload size mapping */
static const struct {
    uint8_t id;
    uint8_t size;
} out_report_sizes[] = {
    { 5,  8 },
    { 6, 10 },
    { 7, 14 },
    { 8, 20 },
    { 9, 63 },
};
#define NUM_OUT_REPORTS 5

/* HID Interface Descriptor */
static struct usb_interface_descriptor iap_hid_intf_desc =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0, /* filled later */
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 1, /* 1 IN interrupt endpoint */
    .bInterfaceClass    = USB_CLASS_HID,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

/* HID Descriptor */
static struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bClassDescriptorType;
    uint16_t wDescriptorLength;
} __attribute__ ((packed)) iap_hid_desc =
{
    .bLength            = 9,
    .bDescriptorType    = HID_DT_HID,
    .bcdHID             = 0x0111,
    .bCountryCode       = 0,
    .bNumDescriptors    = 1,
    .bClassDescriptorType = HID_DT_REPORT,
    .wDescriptorLength  = sizeof(iap_hid_report_desc),
};

/* Interrupt IN endpoint */
static struct usb_endpoint_descriptor iap_hid_ep_in_desc =
{
    .bLength          = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN, /* filled later */
    .bmAttributes     = USB_ENDPOINT_XFER_INT,
    .wMaxPacketSize   = 64,
    .bInterval        = 1,
};

/* Descriptor list for packing into config descriptor */
static const struct usb_descriptor_header* const iap_hid_desc_list[] =
{
    (struct usb_descriptor_header *) &iap_hid_intf_desc,
    (struct usb_descriptor_header *) &iap_hid_desc,
    (struct usb_descriptor_header *) &iap_hid_ep_in_desc,
};

#define IAP_HID_DESC_LIST_SIZE \
    (sizeof(iap_hid_desc_list) / sizeof(iap_hid_desc_list[0]))

/* Endpoint allocation: 1 IN interrupt */
struct usb_class_driver_ep_allocation usb_iap_hid_ep_allocs[1] = {
    { .type = USB_ENDPOINT_XFER_INT, .dir = DIR_IN, .optional = false },
};

#define EP_IAP_HID_IN (usb_iap_hid_ep_allocs[0].ep)

/* State */
static int usb_interface;
static bool iap_hid_active = false;
static bool iap_hid_transport_active = false;

/* TX buffer for sending HID IN reports */
static unsigned char tx_buf[64] USB_DEVBSS_ATTR;
/* RX buffer for receiving SET_REPORT data */
static unsigned char rx_buf[64] USB_DEVBSS_ATTR;

/* Save/restore the original iAP transport */
static void (*saved_transport_send)(const unsigned char *buf, int len);

/* TX completion semaphore — serializes access to tx_buf so
 * back-to-back sends don't corrupt DMA-in-progress data. */
static struct semaphore tx_complete_sem;

/* Held for a whole frame, not a single transfer.
 *
 * iap_send_tx() is the iAP thread's, and the reason this lock is held
 * for a whole frame rather than a transfer is that it has not always
 * been. Three other threads have built packets here at one time or
 * another: the audio thread, because send_event()
 * (firmware/events.c:113) runs its handlers synchronously on the
 * caller's thread and iap_track_changed() used to end in a direct
 * iap_send_tx(); the same thread again through the periodic handler's
 * playlist_next(); and the UI, audio and plugin threads through
 * iap_record(). Each was moved behind a flag and the tick, and the host
 * suite now fails any case that calls a blocking function with a packet
 * half-built. The lock stays whole-frame because the next one will not
 * announce itself either.
 *
 * tx_complete_sem cannot serialise them across a fragmented frame: it
 * is released per transfer completion, so between two fragments the
 * token is free and semaphore_wait() is a yield point. The other thread
 * can take it, overwrite the one static tx_buf and emit a report in the
 * middle of the first thread's set.
 *
 * MFi Accessory Hardware Specification R9, Table 3-2 (p.56) makes that
 * fatal rather than merely untidy: a report whose link control byte has
 * bit 0 clear means "any incomplete iAP packets received prior to the
 * arrival of this report are flushed and lost". So the interrupted
 * frame is discarded and the rest of its fragments corrupt the frame
 * that displaced it.
 *
 * Before fragmentation each call emitted exactly one self-contained
 * report, so a race could scramble tx_buf but could not break a report
 * set. Splitting frames turned that into guaranteed frame loss, which
 * is why the lock arrives with it. */
static struct mutex tx_frame_lock;


/*
 * USB HID TX transport for iAP.
 *
 * Called by iap_send_tx() via the iap_transport_send function pointer.
 * Wraps the framed iAP packet into a HID IN report with the appropriate
 * Report ID.
 */
/* Link control byte values, shared with the reassembly side below.
 * Documented there against the ipod-gadget reference and confirmed on a
 * capture of a real accessory: of 854 reports, exactly ten sets opened
 * with 0x02 and exactly ten closed with 0x01. */
#define IAP_HID_LCB_SINGLE  0x00
#define IAP_HID_LCB_LAST    0x01
#define IAP_HID_LCB_FIRST   0x02
#define IAP_HID_LCB_MIDDLE  0x03

static void iap_hid_tx(const unsigned char *buf, int len)
{
    int i, off;
    uint8_t report_id;
    uint8_t report_size;
    int cap;

    if (!iap_hid_active || len <= 0)
        return;

    /* MFi 2.2.2.3 (p.90): "All iAP command packets transferred over the
     * USB IN and OUT pipes must follow the formats specified in Command
     * Packets, except that the sync byte (byte 0) of each packet is
     * unnecessary and should be omitted."
     *
     * The sync byte used to be overwritten in place with 0x00, which
     * happened to land a correct single-report link control byte in the
     * slot it vacated. Drop it explicitly instead, so the byte that
     * takes its place is chosen rather than inherited -- fragmentation
     * needs three other values there. */
    if (buf[0] == 0xFF)
    {
        buf++;
        len--;
        if (len <= 0)
            return;
    }

    /* One report ID for the whole frame, the smallest that fits it plus
     * its link control byte. A frame too big for any of them is
     * fragmented across repeats of the largest.
     *
     * The report ID is chosen once and kept for every fragment,
     * including the last: a capture of a real accessory shows it does
     * not down-shift for a short tail, so a receiver is entitled to
     * assume the size does not change mid-transaction. */
    report_id = in_report_sizes[NUM_IN_REPORTS - 1].id;
    report_size = in_report_sizes[NUM_IN_REPORTS - 1].size;
    for (i = 0; i < NUM_IN_REPORTS; i++)
    {
        if (len + 1 <= in_report_sizes[i].size)
        {
            report_id = in_report_sizes[i].id;
            report_size = in_report_sizes[i].size;
            break;
        }
    }
    cap = report_size - 1;      /* the link control byte takes one */

    /* Anything longer than one report used to be truncated to 63 bytes
     * and sent as a single report, so the accessory received a frame
     * whose length field claimed bytes that never arrived and whose
     * checksum was past the cut. IDPS, the authentication certificate
     * exchange and any track title over about fifty characters all
     * cross that line; a capture of the reference implementation shows
     * its largest single reply at 54 bytes, five short of the cliff. */
    mutex_lock(&tx_frame_lock);

    for (off = 0; off < len; off += cap)
    {
        int chunk = len - off;
        uint8_t lcb;

        if (!iap_hid_active)
            break;

        if (chunk > cap)
            chunk = cap;

        if (off == 0)
            lcb = (off + chunk >= len) ? IAP_HID_LCB_SINGLE
                                       : IAP_HID_LCB_FIRST;
        else
            lcb = (off + chunk >= len) ? IAP_HID_LCB_LAST
                                       : IAP_HID_LCB_MIDDLE;

        /* Inside the loop, not before it: tx_buf is reused for every
         * fragment, so each one has to wait for the last to leave.
         *
         * And the answer matters. On a timeout the previous fragment is
         * still the source of a live DMA read, so filling tx_buf again
         * would corrupt the report already on the wire -- the exact
         * thing the semaphore is here to prevent. The host having
         * stopped polling is enough to reach it. Abandoning the frame
         * is the lesser damage. */
        if (semaphore_wait(&tx_complete_sem, HZ/50) == OBJ_WAIT_TIMEDOUT)
        {
            logf("iap_hid: tx timeout at %d/%d, frame abandoned", off, len);
            break;
        }

        /* Re-checked after the wait, not only before it. The wait is
         * the window: it blocks for up to 20 ms, and
         * usb_iap_hid_disconnect() runs on the USB thread meanwhile --
         * its semaphore_release() is what wakes this thread. usb_core
         * tears the endpoints down straight after, so without this the
         * remaining fragments go to an endpoint that no longer exists.
         * The check above only catches a disconnect that lands between
         * fragments. */
        if (!iap_hid_active)
        {
            semaphore_release(&tx_complete_sem);
            break;
        }

        tx_buf[0] = report_id;
        tx_buf[1] = lcb;
        memcpy(tx_buf + 2, buf + off, chunk);

        /* Every report goes out full size and zero padded. The same
         * capture has all 854 reports at exactly their declared size. */
        if (chunk < cap)
            memset(tx_buf + 2 + chunk, 0, cap - chunk);

        logf("iap_hid: tx id=%d lcb=%02x off=%d/%d chunk=%d",
             report_id, lcb, off, len, chunk);

        /* A rejected fragment leaves a hole in the FIRST/../LAST
         * sequence, and its completion callback never fires, so the
         * semaphore is never released and every later fragment times
         * out anyway. Stop at the first one. */
        if (usb_drv_send_nonblocking(EP_IAP_HID_IN, tx_buf,
                                     1 + report_size) < 0)
        {
            logf("iap_hid: tx rejected at %d/%d, frame abandoned", off, len);
            /* The token taken before the memcpy is ours to give back:
             * the completion callback that would have returned it never
             * fires for a transfer the driver refused. Without this the
             * count reaches zero and every later frame blocks for ever.
             * Unreachable today -- usb-designware.c's
             * usb_drv_send_nonblocking() returns 0 unconditionally --
             * which is why it has never been observed. */
            semaphore_release(&tx_complete_sem);
            break;
        }
    }

    mutex_unlock(&tx_frame_lock);
}

/*
 * Process received iAP data from a SET_REPORT request.
 *
 * HID report wire format (per Apple iAP-over-HID / ipod-gadget reference):
 *   Byte 0: Report ID
 *   Byte 1: Link Control (fragmentation indicator)
 *   Byte 2+: iAP frame data
 *
 * Link Control values:
 *   0x00  Single complete report (no fragmentation)
 *   0x02  First fragment (more to follow)
 *   0x03  Middle fragment (continuation, more to follow)
 *   0x01  Last fragment (continuation, done)
 *
 * iAP packets larger than one HID report (e.g. 128-byte RSA signatures)
 * are fragmented across multiple SET_REPORT transfers. Only the first
 * report contains the 0x55 sync marker. Continuation reports must NOT
 * be scanned for 0x55 since signature data may contain that byte value.
 */
static bool iap_hid_rx_in_progress = false;

static void iap_hid_process_rx(const unsigned char *data, int len)
{
    int i;

    if (len < 3)
        return;

    /* Lazy transport activation: defer iAP transport override and
     * iap_setup() until actual HID data arrives.  This prevents
     * clobbering serial IAP on docks that use USB only for audio. */
    if (!iap_hid_transport_active)
    {
        saved_transport_send = iap_transport_send;
        iap_transport_send = iap_hid_tx;
        iap_hid_transport_active = true;
        /* Not iap_setup(0). That is "autobaud", and serial_bitrate()
         * (serial-6g.c:186) stores its argument before the
         * !acc_plugged early return -- so the first time a host
         * enumerated, the user's configured dock bit rate was gone for
         * the rest of the boot. This transport has no bit rate to set. */
        iap_setup(IAP_RATE_UNCHANGED);
        /* Allocate IAP buffers synchronously so the first packet is
         * not dropped.  iap_setup() sets iap_running=false; in the
         * serial path iap_getc() defers allocation via IAP_EV_MALLOC
         * and the accessory retransmits.  In the USB HID path we feed
         * an entire packet synchronously, so buffers must be ready
         * before the first byte hits iap_getc(). */
        iap_malloc();
    }

    uint8_t report_id = data[0];
    uint8_t link_ctrl = data[1];

    /* iAP data starts after report ID and link control byte.
     * Max payload per report = report_size - 1 (link control takes 1 byte).
     */
    int iap_len = len - 2;

    for (i = 0; i < NUM_OUT_REPORTS; i++)
    {
        if (out_report_sizes[i].id == report_id)
        {
            iap_len = out_report_sizes[i].size - 1;
            break;
        }
    }

    /* A report ID the table does not know carries no length of its own,
     * so iap_len is still len - 2 and the clamp below cannot narrow it
     * -- it is already that value. The caller's own clamp is what keeps
     * that inside rx_buf today, and relying on it means this function
     * is only safe for the lengths that caller happens to pass. Bound
     * it here too: nothing this driver declares is larger than the
     * biggest OUT report. */
    if (i == NUM_OUT_REPORTS)
    {
        int biggest = out_report_sizes[NUM_OUT_REPORTS - 1].size - 1;
        if (iap_len > biggest)
            iap_len = biggest;
    }

    /* clamp to what we actually received */
    if (iap_len > len - 2)
        iap_len = len - 2;

    logf("iap_hid: rx id=%d len=%d wLen=%d",
         report_id, iap_len, len);
    logf("iap_hid: [%02x %02x %02x %02x %02x %02x %02x %02x]",
         data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
         len > 3 ? data[3] : 0, len > 4 ? data[4] : 0,
         len > 5 ? data[5] : 0, len > 6 ? data[6] : 0,
         len > 7 ? data[7] : 0);

    const unsigned char *iap_data = data + 2;

    /* The link control byte is a two-bit field: bit 0 Continue, bit 1
     * MoreToFollow. Mask once and use the masked value throughout --
     * the dispatch below masked, and the two state assignments compared
     * the raw byte, so any accessory setting a bit above the low two
     * would be dispatched as a fragment and then have its continuation
     * silently dropped. */
    link_ctrl &= 0x03;

    switch (link_ctrl)
    {
        case IAP_HID_LCB_SINGLE:
        case IAP_HID_LCB_FIRST:
        {
            /* Look for 0x55 sync marker in iAP data */
            int sync_offset = -1;
            for (i = 0; i < iap_len; i++)
            {
                if (iap_data[i] == 0x55)
                {
                    sync_offset = i;
                    break;
                }
            }

            /* R9 Table 3-2 (p.56), link control bit 0: a report with
             * it clear "is the first in a set ... This also implies
             * that any previous sets are completed. Any incomplete iAP
             * packets received prior to the arrival of this report are
             * flushed and lost."
             *
             * Clearing the transport's own flag is half of that; the
             * protocol framer is the other half. It was left mid-packet
             * in ST_DATA, so the bytes of this report were consumed as
             * continuation data of the abandoned one and this command
             * was lost as well.
             *
             * The flush is unconditional, as the rule is. Gating it on
             * iap_hid_rx_in_progress covered only the set that never
             * closes, and the two states are independent: a set can end
             * properly -- LAST arrives, the flag clears -- while the
             * iAP packet inside it was short of its declared length,
             * leaving the framer in ST_DATA. On the next start report
             * the flag is already false, so nothing was flushed and the
             * framer ate the new command as continuation data. The
             * 25 ms IAP_PKT_TIMEOUT does not cover it either;
             * consecutive EP0 SET_REPORTs are milliseconds apart. */
            iap_rx_flush();
            iap_hid_rx_in_progress = false;

            if (sync_offset >= 0)
            {
                /* iap_getc() is an ISR callee. On this target
                 * serial-6g.c:256 and :266 call it from the dock UART's
                 * receive interrupt, and it mutates the framer state,
                 * iap_rxnext and iap_rxlen with no protection of its
                 * own. This loop runs on the USB thread, so the two
                 * interleave whenever a dock accessory is talking while
                 * the host is enumerated -- which is the ordinary state
                 * of a docked iPod. iap_rx_flush() two lines up brackets
                 * its four writes for exactly this reason. */
                int level = disable_irq_save();

                iap_hid_rx_in_progress = (link_ctrl == IAP_HID_LCB_FIRST);
                iap_getc(0xFF);
                for (i = sync_offset; i < iap_len; i++)
                    iap_getc(iap_data[i]);

                restore_irq(level);
            }
            break;
        }

        case IAP_HID_LCB_MIDDLE:
        case IAP_HID_LCB_LAST:
        {
            if (iap_hid_rx_in_progress)
            {
                /* Same window as the first-fragment loop above. */
                int level = disable_irq_save();

                for (i = 0; i < iap_len; i++)
                    iap_getc(iap_data[i]);

                if (link_ctrl == IAP_HID_LCB_LAST)
                    iap_hid_rx_in_progress = false;

                restore_irq(level);
            }
            break;
        }
    }
}

/* ===== USB Class Driver Interface ===== */

void usb_iap_hid_init(void)
{
    semaphore_init(&tx_complete_sem, 1, 1);
    mutex_init(&tx_frame_lock);
    logf("iap_hid: init");
}

int usb_iap_hid_set_first_interface(int interface)
{
    usb_interface = interface;
    logf("iap_hid: usb_interface=%d", usb_interface);
    return interface + 1; /* one HID interface */
}

int usb_iap_hid_get_config_descriptor(unsigned char *dest, int max_packet_size)
{
    (void) max_packet_size;
    unsigned int i;
    unsigned char *orig_dest = dest;

    /* fill in dynamic fields */
    iap_hid_intf_desc.bInterfaceNumber = usb_interface;
    iap_hid_ep_in_desc.bEndpointAddress = EP_IAP_HID_IN;

    /* pack descriptors */
    for (i = 0; i < IAP_HID_DESC_LIST_SIZE; i++)
    {
        memcpy(dest, iap_hid_desc_list[i], iap_hid_desc_list[i]->bLength);
        dest += iap_hid_desc_list[i]->bLength;
    }

    return dest - orig_dest;
}

void usb_iap_hid_init_connection(void)
{
    logf("iap_hid: init connection");
    iap_hid_active = true;
    /* Transport override and iap_setup() are deferred until
     * actual iAP data arrives via SET_REPORT.  This prevents
     * clobbering serial IAP on docks that use USB only for audio. */
}

void usb_iap_hid_disconnect(void)
{
    logf("iap_hid: disconnect");
    iap_hid_active = false;
    iap_hid_rx_in_progress = false;
    /* No iap_rx_flush() here. A disconnect can leave the framer in
     * ST_DATA -- iap_malloc() on reconnect resets the receive pointers
     * and leaves frame_state.len, .count and .state alone -- but every
     * command that follows arrives on a start report, and that path
     * flushes unconditionally. There is no route by which stale framer
     * state outlives the next command, and a mutation confirms it:
     * removing a flush from here changes nothing while the one in
     * iap_hid_process_rx() stands. */
    semaphore_release(&tx_complete_sem);

    if (iap_hid_transport_active)
    {
        iap_transport_send = saved_transport_send;
        iap_hid_transport_active = false;
    }
}

void usb_iap_hid_transfer_complete(int ep, int dir, int status, int length)
{
    (void) ep;
    (void) dir;
    (void) status;
    (void) length;
    semaphore_release(&tx_complete_sem);
}


bool usb_iap_hid_control_request(struct usb_ctrlrequest *req, void *reqdata,
                                  unsigned char *dest)
{
    switch (req->bRequest)
    {
        case USB_REQ_GET_DESCRIPTOR:
        {
            if (req->bRequestType != (USB_DIR_IN | USB_TYPE_STANDARD |
                                      USB_RECIP_INTERFACE)
                || (req->wIndex & 0xff) != usb_interface
                || (req->wValue & 0xff) != 0)
                return false;

            /* HID class GET_DESCRIPTOR for report descriptor.
             *
             * These used to stage the answer in rx_buf, which is 64
             * bytes. iap_hid_report_desc is 96, and wDescriptorLength
             * advertises exactly that, so every conformant host asked
             * for 96 and the memcpy wrote 32 bytes past the end of
             * rx_buf on every enumeration. In the linked image that
             * lands on usb_hid.o's .bss, over cur_buf_send, which
             * usb_hid.c then uses as an unguarded array index.
             *
             * dest is usb_core's 256-byte response_data and is what the
             * other drivers answer from (see usb_hid.c:674).
             */
            uint8_t desc_type = req->wValue >> 8;
            if (desc_type == HID_DT_REPORT)
            {
                int len = MIN(req->wLength, (int)sizeof(iap_hid_report_desc));
                memcpy(dest, iap_hid_report_desc, len);
                usb_drv_control_response(USB_CONTROL_ACK, dest, len);
                return true;
            }
            else if (desc_type == HID_DT_HID)
            {
                int len = MIN(req->wLength, (int)sizeof(iap_hid_desc));
                memcpy(dest, &iap_hid_desc, len);
                usb_drv_control_response(USB_CONTROL_ACK, dest, len);
                return true;
            }
            return false;
        }

        case HID_REQ_GET_REPORT:
            if (req->bRequestType != (USB_DIR_IN | USB_TYPE_CLASS |
                                      USB_RECIP_INTERFACE)
                || (req->wIndex & 0xff) != usb_interface)
                return false;
            {
                int len = MIN(req->wLength, (int)sizeof(rx_buf));
                memset(dest, 0, len);
                usb_drv_control_response(USB_CONTROL_ACK, dest, len);
            }
            return true;

        case HID_REQ_SET_REPORT:
            if (req->bRequestType != (USB_DIR_OUT | USB_TYPE_CLASS |
                                      USB_RECIP_INTERFACE)
                || (req->wIndex & 0xff) != usb_interface
                || req->wLength > sizeof(rx_buf))
                return false;
            if (reqdata)
            {
                /* Only as many bytes as the first pass agreed to
                 * receive are present. Passing the host's wLength
                 * straight through let a request claiming more than
                 * sizeof(rx_buf) feed whatever followed the buffer
                 * into the packet framer. */
                int len = MIN(req->wLength, (int)sizeof(rx_buf));
                iap_hid_process_rx(rx_buf, len);
                usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            }
            else
            {
                /* first pass: accept the data into rx_buf.
                 * Clear it first, so a data stage shorter than the
                 * report does not hand the framer stale bytes from the
                 * previous transfer. */
                /* Only a real host-to-device data stage may be
                 * received. usb-designware.c:936 enters
                 * EP0_REQ_CTRLWRITE for "wLength > 0 && !(bRequestType
                 * & USB_DIR_IN)" alone, and :974 answers a
                 * USB_CONTROL_RECEIVE outside that state with
                 * panicf("bad response"). So a SET_REPORT carrying no
                 * data stage, or one marked device-to-host, took the
                 * whole player down from a single malformed control
                 * packet -- reachable from the host for as long as the
                 * iPod is docked and enumerated.
                 *
                 * Returning false lets usb_core STALL it, which is what
                 * an unsupported control request deserves. usb_hid.c:617
                 * guards the same request the same way. */
                if (req->wLength == 0 || (req->bRequestType & USB_DIR_IN))
                    return false;

                int len = MIN(req->wLength, (int)sizeof(rx_buf));
                memset(rx_buf, 0, sizeof(rx_buf));
                usb_drv_control_response(USB_CONTROL_RECEIVE, rx_buf, len);
            }
            return true;

        case HID_REQ_SET_IDLE:
            if (req->bRequestType != (USB_DIR_OUT | USB_TYPE_CLASS |
                                      USB_RECIP_INTERFACE)
                || (req->wIndex & 0xff) != usb_interface
                || req->wLength != 0)
                return false;
            usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            return true;

        case 0x40:
            if ((req->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR)
                return false;
            /* Apple vendor-specific request — acknowledge it */
            logf("iap_hid: apple vendor req 0x40");
            usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            return true;

        default:
            logf("iap_hid: unhandled req 0x%x", req->bRequest);
            return false;
    }
}

int usb_iap_hid_set_interface(int intf, int alt)
{
    if (intf == usb_interface && alt == 0)
        return 0;
    return -1;
}

int usb_iap_hid_get_interface(int intf)
{
    if (intf == usb_interface)
        return 0;
    return -1;
}
