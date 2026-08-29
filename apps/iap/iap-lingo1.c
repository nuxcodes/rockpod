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
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Lingo 0x01: Microphone Lingo
 */

#include "iap-core.h"
#include "iap-lingo.h"

/*
 * This macro is meant to be used inside an IAP mode message handler.
 * It is passed the expected minimum length of the message buffer.
 * If the buffer does not have the required lenght an ACK
 * packet with a Bad Parameter error is generated.
 */
#define CHECKLEN(x) do { \
        if (len < (x)) { \
            cmd_ack(cmd, IAP_ACK_BAD_PARAM); \
            return; \
        }} while(0)

/* Parameters and their length check, in the same units -- see the
 * same pair in iap-lingo3.c. Microphone commands are lingo byte, command
 * byte, then a transaction ID when one is in force, so parameter n is
 * at 2 + doff + n and a check for n parameters wants 2 + n + doff. */
#define L1_NEED(n)  CHECKLEN(2 + (n) + doff)
#define L1_PARAM(n) (buf[2 + doff + (n)])

/* Check for authenticated state, and return an ACK Not
 * Authenticated on failure.
 */
#define CHECKAUTH do { \
        if (!DEVICE_AUTHENTICATED) { \
            cmd_ack(cmd, IAP_ACK_NO_AUTHEN); \
            return; \
        }} while(0)

/* The Microphone lingo has no Apple-device acknowledgement.
 *
 * MFi C.5 (p.533): "The Microphone lingo is defined such that the Apple
 * device initiates commands and the accessory responds to these
 * commands; that is, the Apple device sends commands to the accessory
 * and the accessory responds with data or AccessoryAck commands." Table
 * C-12 (p.534) lists one acknowledgement, 0x04 AccessoryAck, and C.5.2
 * gives it Origin: Accessory. There is nothing for the device to send.
 *
 * This used to transmit IAP_TX_INIT(0x03, 0x00) -- a Display Remote
 * iPodAck -- carrying a Microphone command id. Wrong lingo, and an
 * acknowledgement that does not exist in this lingo in either
 * direction. An accessory that identified with Identify(0x01) is not
 * even granted lingo 0x03 by iap-lingo0.c, so the packet arrives on a
 * lingo it never negotiated.
 *
 * Kept as a function so the CHECKLEN, CHECKAUTH and default paths read
 * the same as every other lingo; it simply says nothing.
 */
static void cmd_ack(const unsigned char cmd, const unsigned char status)
{
    (void)cmd;
    (void)status;
}

/* iPodModeChange, flagged here and sent from iap_periodic().
 *
 * audio_set_source() calls this from whatever thread changed the input:
 * the audio thread (apps/playback.c:3070), the UI thread
 * (apps/radio/radio.c:395 and :737) and any plugin that touches
 * audio_set_input_source(). Building the packet here put IAP_TX_INIT
 * into all of them, and the TX buffer belongs to the iAP thread. On the
 * 6G that thread can be parked inside iap_hid_tx(), which takes
 * tx_frame_lock and then waits on a semaphore per fragment, reading
 * each chunk out of the shared buffer between waits -- so an
 * IAP_TX_INIT from another thread rewrites a packet that is halfway
 * onto the wire.
 *
 * Same shape as the track-change notification, which was moved off the
 * audio thread for the same reason. The state is coalesced: an
 * accessory needs to know whether recording is on now, not how many
 * times it changed since the last tick.
 */
static bool  l1_mode_pending;
static bool  l1_mode_onoff;

/* returns record status */
bool iap_record(bool onoff)
{
    if (!DEVICE_LINGO_SUPPORTED(0x01))
        return false;

    l1_mode_onoff = onoff;
    l1_mode_pending = true;
    iap_wake();

    return onoff;
}

/* Called from iap_periodic(), in thread context, where this thread owns
 * the TX buffer. */
void iap_lingo1_send_pending(void)
{
    if (!l1_mode_pending)
        return;
    l1_mode_pending = false;

    /* Equivalent mutant: iap_record() refuses to flag anything for an
     * accessory without the lingo, and iap_reset_lingo1() clears the
     * flag on a detach, so nothing can be pending here for one. Kept as
     * defence in depth on the send side, where the check belongs. */
    if (!DEVICE_LINGO_SUPPORTED(0x01))
        return;

    /* iPodModeChange. Origin: Apple device (MFi C.5.4, p.536), so it
     * carries the device's own transaction ID counter rather than an
     * echo. Without one, an accessory under IDPS reads the Mode byte as
     * the ID's high byte and never learns recording ended -- C.5.4 has
     * it stay out of low power mode until it does. */
    IAP_TX_INIT(0x01, 0x06);
    IAP_TX_PUT_IPOD_TRANSID();
    IAP_TX_PUT(l1_mode_onoff ? 0x00 : 0x01);
    iap_send_tx();
}

void iap_reset_lingo1(void)
{
    l1_mode_pending = false;
    l1_mode_onoff = false;
}

void iap_handlepkt_mode1(const unsigned int len, const unsigned char *buf)
{
    unsigned int cmd = buf[1];
    unsigned int doff = 0;

    /* Microphone lingo commands are at least 4 bytes in length */
    /* Equivalent mutant: every command below re-checks its own length
     * in parameter units before reading anything, so removing this
     * changes no observable behaviour -- a three-byte packet is refused
     * by case 0x08's own check either way. Kept because it states the
     * lingo's floor where a reader looks for it.
     *
     * That claim was false when it was written: cases 0x04 and 0x0A had
     * no check of their own, and this one is absolute where their reads
     * are relative to doff. Under IDPS L1_PARAM(0) is buf[4] and four
     * bytes only guarantee buf[0..3], so a bare "01 0A TT TT" read the
     * control type from past the end of itself. Both have their own
     * check now, which is what makes the sentence above true. */
    CHECKLEN(4);

    /* MFi 2.6 (p.110): from StartIDPS onward the accessory "must
     * include 16-bit transaction IDs in every iAP packet", and 2.6.1.4
     * (p.112) exempts only RequestIdentify, Identify and
     * IdentifyDeviceLingoes. This handler had no offset at all -- alone
     * among the lingoes -- so under IDPS every payload below was read
     * two bytes early, and RetAccessoryCaps's capability word came out
     * of the transaction ID. */
    if (DEVICE_TRANSID_ACTIVE)
        doff = 2;

    /* Lingo 0x01 must have been negotiated */
    if (!DEVICE_LINGO_SUPPORTED(0x01)) {
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }

    /* Authentication required for all commands */
    CHECKAUTH;

    switch (cmd)
    {
        /* BeginRecord (0x00) Deprecated
         *
         * Sent from the iPod to the device
         */

        /* EndRecord (0x01) Deprecated
         *
         * Sent from the iPod to the device
         */

        /* BeginPlayback (0x02) Deprecated
         *
         * Sent from the iPod to the device
         */

        /* EndPlayback (0x03) Deprecated
         *
         * Sent from the iPod to the device
         */

        /* ACK (0x04)
         *
         * The device sends an ACK response when a command
         * that does not return any data has completed.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Microphone Lingo, always 0x01
         * 0x01: Command, always 0x04
         * 0x02: The command result status
         * 0x03: The ID of the command for which the
         *       response is being sent
         *
         * Returns: (none)
         */
        case 0x04:
            /* Table C-12 (p.534) gives AccessoryAck a status byte and
             * the command being acknowledged, so two parameters.
             *
             * Equivalent mutant: the only read is inside LOGF_ENABLE,
             * which is off, so removing this changes nothing
             * observable. It is still a fix rather than decoration --
             * the read was of bytes past the end of the packet, and a
             * discarded value is not a safe one. */
            L1_NEED(2);
#ifdef LOGF_ENABLE
            if (L1_PARAM(0) != 0x00)
                logf("iap: Mode1 Command ACK error: "
                            "0x%02x 0x%02x", L1_PARAM(0), L1_PARAM(1));
#endif
            break;

        /* GetDevAck (0x05)
         *
         * Sent from the iPod to the device
         */

        /* iPodModeChange (0x06)
         *
         * Sent from the iPod to the device
         */

        /* GetDevCaps (0x07)
         *
         * Sent from the iPod to the device
         */

        /* RetDevCaps (0x08)
         *
         * The microphone device returns the payload
         * indicating which capabilities it supports.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Microphone Lingo, always 0x01
         * 0x01: Command, always 0x08
         * 0x02: Device capabilities (bits 31:24)
         * 0x03: Device capabilities (bits 23:16)
         * 0x04: Device capabilities (bits 15:8)
         * 0x05: Device capabilities (bits 7:0)
         *
         * Returns:
         * SetDevCtrl, sets stereo line input when supported
         */
        case 0x08:
            /* Table C-12 (p.534) gives RetAccessoryCaps four capability
             * bytes, so six with the lingo and command. */
            L1_NEED(4);

            if ((L1_PARAM(3) & 3) == 3) {
                /* SetAccessoryCtrl, set stereo line-in. Origin: Apple
                 * device (MFi C.5.9, p.539). */
                IAP_TX_INIT(0x01, 0x0B);
                IAP_TX_PUT_IPOD_TRANSID();
                IAP_TX_PUT(0x01);
                IAP_TX_PUT(0x01);

                iap_send_tx();
            }

            /* TODO?: configure recording level/limiter controls
               when supported by the device */

            break;

        /* GetDevCtrl (0x09)
         *
         * Sent from the iPod to the device
         */

        /* RetDevCaps (0x0A)
         *
         * The device returns the current control state
         * for the specified control type.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Microphone Lingo, always 0x01
         * 0x01: Command, always 0x0A
         * 0x02: The control type
         * 0x03: The control data
         */
        case 0x0A:
            /* One parameter, the control type.
             *
             * Equivalent mutant for the same reason as case 0x04's:
             * every arm of the switch below is a bare break, so the
             * value read decides nothing. The read was still past the
             * end of a four-byte packet under IDPS. */
            L1_NEED(1);
            switch (L1_PARAM(0))
            {
                case 0x01:  /* stereo/mono line-in control */
                case 0x02:  /* recording level control */
                case 0x03:  /* recording level limiter control */
                    break;
            }
            break;

        /* SetDevCtrl (0x0B)
         *
         * Sent from the iPod to the device
         */

        /* The default response is IAP_ACK_BAD_PARAM */
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode1 Command");
#endif
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
