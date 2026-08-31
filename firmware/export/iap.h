/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Alan Korr
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __IAP_H__
#define __IAP_H__

#include <stdbool.h>

/* This is just the payload size, without sync, length and checksum */
#define RX_BUFLEN (64*1024)
/* This is the entire frame length, sync, length, payload and checksum */
/* Sized so an AckFIDTokenValues (0x3A) can never overflow. That ack is
 * never larger than the SetFIDTokenValues that provoked it, and that
 * packet is capped by RequestTransportMaxPayloadSize -- 255 as we
 * answer it, or the MFi 3.3.14 fallback default of 506. At 128 a
 * conformant accessory declaring all nine accessory-info types and all
 * twelve preference classes overflowed and panicked the player. */
#define TX_BUFLEN 512

#ifdef HAVE_IAP_MULTIPORT
#define IF_IAP_MP(x...) x
#define IF_IAP_MP_NONVOID(x...) x
#else
#define IF_IAP_MP(x...)
#define IF_IAP_MP_NONVOID(x...) void
#endif

extern bool iap_getc(IF_IAP_MP(int port,) unsigned char x);
/* Leave the serial bit rate as the user configured it. The USB HID
 * transport has no bit rate of its own, and iap_setup(0) would set the
 * serial one to autobaud: serial_bitrate() stores its argument before
 * the !acc_plugged early return, so the setting was lost for the rest
 * of the boot the first time a host enumerated. */
#define IAP_RATE_UNCHANGED (-1)

extern void iap_setup(int ratenum);
extern void iap_malloc(void);
extern void iap_bitrate_set(int ratenum);
extern void iap_periodic(void);
extern void iap_handlepkt(void);
extern bool iap_remote_ui_active(void);
extern bool iap_play_or_resume(void);
extern void iap_send_pkt(const unsigned char * data, int len);
extern void iap_send_reply(const unsigned char * data, int len,
                           unsigned char tid_hi, unsigned char tid_lo);
const unsigned char *iap_get_serbuf(void);

/* Codec volume to the protocol's 0..255 UI volume (MFi Table 4-61,
 * event 0x04). Declared here as well as in apps/iap/iap-core.h so the
 * remote tuner driver, which builds the same event 0x04 packet, uses
 * one conversion rather than a second one of its own. */
/* Two scales, and the difference matters on the wire. MFi Table 4-61
 * (p.261): the UI level is "normalized to volume limit settings", the
 * absolute one is not. Event 0x04 and GetiPodStateInfo(0x04) want the
 * UI byte; only the absolute-volume paths want the other. */
unsigned char iap_volume_to_byte(int volume);
unsigned char iap_volume_to_ui_byte(int volume);

/* Discard a partially received packet. The USB HID transport calls this
 * when it abandons a report set, which MFi Accessory Hardware
 * Specification R9 Table 3-2 (p.56) requires: "Any incomplete iAP
 * packets received prior to the arrival of this report are flushed and
 * lost." */
void iap_rx_flush(void);

/* Transport abstraction — USB HID driver overrides this for iAP-over-USB */
extern void (*iap_transport_send)(const unsigned char *buf, int len);

/* Button state — set by iAP Simple Remote and Extended Interface
 * lingo handlers, read by remote_control_rx() in the button driver. */
extern unsigned long iap_remotebtn;
extern unsigned int iap_timeoutbtn;
extern int iap_repeatbtn;
#ifdef HAVE_LINE_REC
extern bool iap_record(bool onoff);
#endif
void iap_reset_state(IF_IAP_MP_NONVOID(int port) ); /* 0 is dock, 1 is headphone */

#ifdef HAVE_IAP_ACCESSORY_POLL
/* Targets whose accessory-detect line can only be read from thread
 * context implement this; iap_periodic() calls it. */
void iap_accessory_poll(void);
#endif
bool dbg_iap(void);
#endif
