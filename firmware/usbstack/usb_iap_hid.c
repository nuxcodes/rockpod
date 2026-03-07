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
#ifdef USB_ENABLE_AUDIO
#include "usb_audio.h"
#endif

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
 * Exact bytes from Apple's iPod firmware (via ipod-gadget reference).
 */
static const unsigned char iap_hid_report_desc[] = {
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x75, 0x08, 0x26, 0x80, 0x00,
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

/*
 * USB HID TX transport for iAP.
 *
 * Called by iap_send_tx() via the iap_transport_send function pointer.
 * Wraps the framed iAP packet into a HID IN report with the appropriate
 * Report ID.
 */
static void iap_hid_tx(const unsigned char *buf, int len)
{
    int i;

    if (!iap_hid_active || len <= 0)
        return;

    /* Wait for previous HID TX to complete before touching tx_buf.
     * A 64-byte full-speed interrupt transfer completes in <2ms.
     * 20ms timeout avoids blocking audio if something goes wrong. */
    semaphore_wait(&tx_complete_sem, HZ/50);

    /* find smallest IN report ID that fits */
    uint8_t report_id = 0;
    uint8_t report_size = 0;
    for (i = 0; i < NUM_IN_REPORTS; i++)
    {
        if (len <= in_report_sizes[i].size)
        {
            report_id = in_report_sizes[i].id;
            report_size = in_report_sizes[i].size;
            break;
        }
    }

    /* if packet is too large, use the largest report */
    if (report_id == 0)
    {
        report_id = in_report_sizes[NUM_IN_REPORTS - 1].id;
        report_size = in_report_sizes[NUM_IN_REPORTS - 1].size;
        len = report_size; /* truncate */
    }

    /* build the HID report: [Report ID] [payload...] [zero padding] */
    tx_buf[0] = report_id;
    memcpy(tx_buf + 1, buf, len);

    /* iAP over USB HID: replace the 0xFF serial sync byte with 0x00
     * to match the HID transport format the accessory expects. */
    if (len > 0 && tx_buf[1] == 0xFF)
        tx_buf[1] = 0x00;

    /* zero-pad the rest */
    if (len < report_size)
        memset(tx_buf + 1 + len, 0, report_size - len);

    logf("iap_hid: tx id=%d len=%d [%02x %02x %02x %02x %02x %02x]",
         report_id, len,
         tx_buf[0], tx_buf[1], tx_buf[2],
         (len > 2) ? tx_buf[3] : 0, (len > 3) ? tx_buf[4] : 0,
         (len > 4) ? tx_buf[5] : 0);

    usb_drv_send_nonblocking(EP_IAP_HID_IN, tx_buf, 1 + report_size);

#ifdef USB_ENABLE_AUDIO
    /* Space out HID IN responses during source streaming for non-IDPS
     * docks.  The Onkyo ND-S1's USB host drops isochronous audio
     * frames when it has to schedule a burst of EP0 + HID IN + ISO IN
     * in the same few-ms window.  This sleep ensures each HID IN
     * response is isolated among ~100 ISO-only frames.
     * IDPS devices (like HA-2SE) don't poll during streaming. */
    if (usb_audio_source_streaming() && !iap_auth_is_idps())
        sleep(HZ/10); /* 100ms */
#endif
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
     * clobbering serial IAP on docks (like the Onkyo ND-S1) that
     * use USB only for audio, not for iAP control. */
    if (!iap_hid_transport_active)
    {
        saved_transport_send = iap_transport_send;
        iap_transport_send = iap_hid_tx;
        iap_hid_transport_active = true;
        iap_setup(0);
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

    switch (link_ctrl & 0x03)
    {
        case 0x00: /* single complete report */
        case 0x02: /* first fragment */
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

            if (sync_offset >= 0)
            {
                iap_hid_rx_in_progress = (link_ctrl == 0x02);
                iap_getc(0xFF);
                for (i = sync_offset; i < iap_len; i++)
                    iap_getc(iap_data[i]);
            }
            break;
        }

        case 0x03: /* middle fragment */
        case 0x01: /* last fragment */
        {
            if (iap_hid_rx_in_progress)
            {
                for (i = 0; i < iap_len; i++)
                    iap_getc(iap_data[i]);

                if (link_ctrl == 0x01)
                    iap_hid_rx_in_progress = false;
            }
            break;
        }
    }
}

/* ===== USB Class Driver Interface ===== */

void usb_iap_hid_init(void)
{
    semaphore_init(&tx_complete_sem, 1, 1);
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
     * clobbering serial IAP on docks (like the Onkyo ND-S1)
     * that use USB only for audio, not for iAP control. */
}

void usb_iap_hid_disconnect(void)
{
    logf("iap_hid: disconnect");
    iap_hid_active = false;
    iap_hid_rx_in_progress = false;
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
    (void) dest;

    switch (req->bRequest)
    {
        case USB_REQ_GET_DESCRIPTOR:
        {
            /* HID class GET_DESCRIPTOR for report descriptor */
            uint8_t desc_type = req->wValue >> 8;
            if (desc_type == HID_DT_REPORT)
            {
                int len = MIN(req->wLength, (int)sizeof(iap_hid_report_desc));
                memcpy(rx_buf, iap_hid_report_desc, len);
                usb_drv_control_response(USB_CONTROL_ACK, rx_buf, len);
                return true;
            }
            else if (desc_type == HID_DT_HID)
            {
                int len = MIN(req->wLength, (int)sizeof(iap_hid_desc));
                memcpy(rx_buf, &iap_hid_desc, len);
                usb_drv_control_response(USB_CONTROL_ACK, rx_buf, len);
                return true;
            }
            return false;
        }

        case HID_REQ_GET_REPORT:
            /* return zeros */
            {
                int len = MIN(req->wLength, (int)sizeof(rx_buf));
                memset(rx_buf, 0, len);
                usb_drv_control_response(USB_CONTROL_ACK, rx_buf, len);
            }
            return true;

        case HID_REQ_SET_REPORT:
            if (reqdata)
            {
                /* second pass: data received, process iAP payload */
                iap_hid_process_rx(rx_buf, req->wLength);
                usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            }
            else
            {
                /* first pass: accept the data into rx_buf */
                int len = MIN(req->wLength, (int)sizeof(rx_buf));
                usb_drv_control_response(USB_CONTROL_RECEIVE, rx_buf, len);
            }
            return true;

        case HID_REQ_SET_IDLE:
            usb_drv_control_response(USB_CONTROL_ACK, NULL, 0);
            return true;

        case 0x40:
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
