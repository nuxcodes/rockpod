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

/* Lingo 0x02, Simple Remote Lingo
 *
 * TODO:
 * - Fix cmd 0x00 handling, there has to be a more elegant way of doing
 *   this
 */

#include "iap-core.h"
#include "iap-lingo.h"
#include "system.h"
#include "button.h"
#include "audio.h"
#include "settings.h"
#include "tuner.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif

/*
 * This macro is meant to be used inside an IAP mode message handler.
 * It is passed the expected minimum length of the message buffer.
 * If the buffer does not have the required lenght an ACK
 * packet with a Bad Parameter error is generated.
 */
/* ContextButtonStatus is the one command in this lingo that must never
 * be answered. MFi 4.2.7 (p.226): "The Apple device does not return a
 * packet to the accessory in response to this command", and 4.2.8
 * (p.228) has the acknowledgement go out "in response to any command
 * sent from the accessory, except command 0x00."
 *
 * The three length checks at the top of the handler run for every
 * command, so a truncated or merged button frame -- which 4.2.3 (p.216)
 * warns is expected on a shared UART, "multiple button status packets
 * cannot be sent back to back; otherwise, the repeated button status
 * packets may be misinterpreted as being part of a corrupted packet" --
 * produced an unsolicited iPodAck to a remote that has no receiver. */
#define CHECKLEN(x) do { \
        if (len < (x)) { \
            if (cmd != 0x00) \
                cmd_ack(cmd, IAP_ACK_BAD_PARAM); \
            return; \
        }} while(0)

/* Parameters and their length check, in the same units -- see the same
 * pair in iap-lingo3.c. Simple Remote commands are lingo byte, command
 * byte, then a transaction ID when one is in force, so parameter n is
 * at 2 + doff + n and a check for n parameters wants 2 + n + doff.
 *
 * This lingo is where a mutation setting doff to 0 once survived the
 * whole suite -- the remote read its buttons out of the transaction
 * ID -- so having one place that knows the base matters here. */
/* No L2_NEED here: this lingo's length checks are absolutes in the
 * prologue, before doff is known, so a parameter-unit CHECKLEN has
 * nowhere to go -- it was defined anyway and went unused. What the
 * handlers do use is the soft form, testing whether an optional status
 * byte arrived at all, and that has to be in the same units as the read
 * beside it or the pair is back to needing arithmetic. */
#define L2_HAVE(n)       (len >= (unsigned int)(2 + (n) + doff))
#define L2_PARAM_OF(b,n) ((b)[2 + doff + (n)])
#define L2_PARAM(n)      L2_PARAM_OF(buf, (n))

/* Transaction ID of the packet being handled. After IDPS every packet
 * carries one between the command byte and the payload, and every reply
 * must echo it (MFi 2.3.2: "all subsequent iAP command packets must
 * include transaction IDs, regardless of lingo"). Parsed at the top of
 * iap_handlepkt_mode2(), mirroring what lingo 3 already does. */
static uint8_t l2_tid_hi, l2_tid_lo;

static void cmd_ack(const unsigned char cmd, const unsigned char status)
{
    IAP_TX_INIT(0x02, 0x01);
    if (DEVICE_TRANSID_ACTIVE) {
        IAP_TX_PUT(l2_tid_hi);
        IAP_TX_PUT(l2_tid_lo);
    }
    IAP_TX_PUT(status);
    IAP_TX_PUT(cmd);

    iap_send_tx();
}

#define cmd_ok(cmd) cmd_ack((cmd), IAP_ACK_OK)

/* Power On is an edge: GetDevCaps is sent on its release, not its press.
 * This lived inside iap_handlepkt_mode2() as a function-static, where
 * nothing could clear it -- so a press left pending when an accessory
 * was unplugged was still pending for the next one, whose first button
 * release then fired a GetDevCaps for a press that belonged to its
 * predecessor. At file scope iap_reset_lingo2() can reach it. */
static bool poweron_pressed = false;
#if CONFIG_TUNER
/* The radio's mute state as this lingo last set it. Out here for the
 * same reason poweron_pressed is: as a function-static it outlived the
 * accessory that set it, so accessory A's Play press left it true and
 * accessory B's first Play press muted the radio instead of unmuting
 * it. */
static bool remote_mute = false;
#endif

void iap_reset_lingo2(void)
{
    poweron_pressed = false;
#if CONFIG_TUNER
    remote_mute = false;
#endif
}

void iap_handlepkt_mode2(const unsigned int len, const unsigned char *buf)
{
    unsigned int cmd = buf[1];

    /* Extra offset for the IDPS transaction ID (0 or 2). Every buf[]
     * index and CHECKLEN below adds it, so one body serves both the
     * legacy and the IDPS framing. Without this the button bitmap was
     * read from the transaction ID: a counter below 0x0100 left byte 0
     * zero and the handler fell through to buf[3], decoding the low
     * byte as play/pause/shuffle, and above it the high byte was
     * decoded as a permanently held chord. */
    unsigned int doff = 0;

    l2_tid_hi = 0;
    l2_tid_lo = 0;

    /* We expect at least three bytes in the buffer, one for the
     * lingo, one for the command, and one for the first button
     * state bits.
     */
    CHECKLEN(3);

    if (DEVICE_TRANSID_ACTIVE) {
        /* Capture before the length check that follows, not after.
         * CHECKLEN rejects through cmd_ack(), and with the id still
         * zeroed that rejection went out stamped 0x0000 -- matching no
         * command the accessory sent, which MFi 2.6.1.1 (p.111)
         * obliges it to discard, so it retries for ever. Same shape as
         * the lingo 3 rejection fixed in 44d1c8243a. Four bytes is
         * enough to read the id itself. */
        CHECKLEN(4);
        l2_tid_hi = buf[2];
        l2_tid_lo = buf[3];
        doff = 2;

        CHECKLEN(5);    /* lingo + cmd + transID(2) + one state byte */
    }

    /* Lingo 0x02 must have been negotiated, except for
     * ContextButtonStatus (0x00): simple remotes like the Apple A1018
     * identify only once at power-up. If the remote was already
     * powered before Rockbox started (e.g. plugged in at boot) that
     * identification is never seen, and rejecting the button events
     * would leave the remote dead until it is replugged. Per MFi
     * spec Table 2-7, cmd 0x00 on UART does not require auth.
     */
    if ((cmd != 0x00) && !DEVICE_LINGO_SUPPORTED(0x02)) {
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }

    switch (cmd)
    {
        /* ContextButtonStatus (0x00)
         *
         * Transmit button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x00
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * Returns: (none)
         */
        case 0x00:
        {
            unsigned long btn = BUTTON_NONE;
            /* True while the accessory still reports any button down,
             * including ones that map to no BUTTON_* code. */
            bool any_bits = (L2_PARAM(0) != 0)
                         || (L2_HAVE(2) && L2_PARAM(1) != 0)
                         || (L2_HAVE(3) && L2_PARAM(2) != 0)
                         || (L2_HAVE(4) && L2_PARAM(3) != 0);

            /* Four independent tests, not a chain. MFi 4.2.3 (p.216):
             * "If a second button is pressed while the first button is
             * down, the button status packet sent by the accessory must
             * include status for both buttons" -- and the bytes are
             * "constructed by ORing the masks of the buttons together".
             *
             * Written as if/else-if, only the lowest non-zero byte was
             * ever decoded. Holding anything in byte 0 made Shuffle,
             * Repeat, Power, FF/REW, Menu, Select, Up and Down all
             * unreachable: Volume Up plus Menu arrived as Volume Up
             * alone. Every arm ORs into btn, so they compose. */
            if(L2_PARAM(0) != 0)
            {
                if(L2_PARAM(0) & 1)
                {
                    btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    /* Once per press, not once per packet. MFi 4.2.7
                     * (p.226) has the accessory repeat its button
                     * status every 30 to 100 ms while the button is
                     * held, so a one-second hold arrived as 10 to 33
                     * packets -- and each one flipped the radio mute
                     * and sent a tuner_set(), which
                     * ipod_remote_tuner.c answers with its own
                     * "03 09 03 01"/"03 09 03 02" play-status
                     * notification and an rmt_tuner_sleep(). The wire
                     * carried ten to thirty alternating Playing/Paused
                     * notifications a second and the net mute state was
                     * a coin flip on the packet count.
                     *
                     * Debounced exactly as Shuffle and Repeat are, by a
                     * flag iap_periodic() clears when the button goes
                     * up. */
                    if (radio_present == 1 && !iap_btnradiomute) {
                        iap_btnradiomute = true;
                        remote_mute = !remote_mute;
                        tuner_set(RADIO_MUTE, remote_mute ? 1 : 0);
                    }
#endif
                }
                if(L2_PARAM(0) & 2)
                    btn |= BUTTON_RC_VOL_UP;
                if(L2_PARAM(0) & 4)
                    btn |= BUTTON_RC_VOL_DOWN;
                /* These stay buttons, unlike PlayControl's arms in
                 * iap-lingo4.c, which were converted to audio_next()
                 * and audio_prev() because outside the WPS
                 * keymap-ipod.c turns BUTTON_RC_RIGHT and
                 * BUTTON_RC_LEFT into browser navigation.
                 *
                 * The Simple Remote is different, and the Apple Radio
                 * Remote is why. Its transport keys are meant to be
                 * read in context: the same BUTTON_RC_RIGHT is next
                 * track in the WPS and next station in the radio
                 * screen, and there is no Playback Engine call that
                 * means either-depending-on-what-the-user-is-looking-at.
                 * Extended Interface's PlayControl carries no such
                 * ambiguity -- MFi 5.1.37 (p.428) makes it a command to
                 * "control the media playback state" and nothing else.
                 *
                 * Converted here, station up and down on the Radio
                 * Remote would skip playlist tracks instead. Tried, and
                 * reverted for that reason. */
                if(L2_PARAM(0) & 8)
                    btn |= BUTTON_RC_RIGHT;
                if(L2_PARAM(0) & 16)
                    btn |= BUTTON_RC_LEFT;
                if(L2_PARAM(0) & (32 | 64)) /* Next/Previous Album */
                {
                    /* Table 4-14 (p.227) buttons 5 and 6, byte 0 masks
                     * 0x20 and 0x40. Neither was decoded, on either
                     * this command or AudioButtonStatus -- which copies
                     * byte 0 into the same switch and then acks Success,
                     * so a head unit's album keys did nothing and its
                     * display advanced as though they had.
                     *
                     * This device advertises Simple Remote 1.02 and
                     * Table 3-132 lingo-0x02 bits 00 and 01, so it
                     * claims them both ways.
                     *
                     * MFi p.217: "The Next and Previous Album commands
                     * have no effect if there is no next or previous
                     * album to go to in the Now Playing list", which is
                     * what audio_next_dir()/audio_prev_dir() already do
                     * -- both are a bare queue_post
                     * (apps/playback.c), so they are safe from here.
                     *
                     * Latched like Stop, Shuffle and Repeat: the
                     * accessory repeats its button status every 30 to
                     * 100 ms and a held album key must move once. */
                    if (!iap_btnalbum)
                    {
                        iap_btnalbum = true;
                        if (L2_PARAM(0) & 32)
                            audio_next_dir();
                        else
                            audio_prev_dir();
                    }
                }
                if(L2_PARAM(0) & 128) /* Stop */
                {
                    /* Table 4-14 (p.226) byte 0 bit 7, and 4.2.9
                     * (p.228): "Apple products running iOS 3.2 support
                     * only the Stop, Play/Resume, and Pause button
                     * values." The other two of those three are handled
                     * a screen below; this one was not decoded at all,
                     * so pressing Stop on a remote did nothing.
                     *
                     * Not a button: the Playback Engine has no Stop, and
                     * every remote code that exists means something
                     * else -- the same reason PlayControl 0x02 calls
                     * audio_stop() rather than raising one. Latched,
                     * because the accessory repeats its status every 30
                     * to 100 ms and a held Stop must stop once. */
                    if (!iap_btnstop)
                    {
                        iap_btnstop = true;
                        audio_stop();
                    }
                }
            }
            if(L2_HAVE(2) && L2_PARAM(1) != 0)
            {
                if(L2_PARAM(1) & 1) /* play */
                {
                    if ((audio_status() & (AUDIO_STATUS_PLAY |
                                           AUDIO_STATUS_PAUSE))
                        != AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    /* Latched, like the contextual Play/Pause above.
                     * Table 4-14 (p.227) puts Play/Resume and Pause at
                     * byte 1 bits 0 and 1, and 4.2.7 (p.226) has the
                     * accessory repeat its status every 30 to 100 ms
                     * while held -- so without the latch each of those
                     * 10 to 33 packets a second ran rmt_tuner_mute(),
                     * which has no state check of its own and calls
                     * rmt_tuner_sleep(): five more packets and a full
                     * reset of the tuner's cached state, on a 19200
                     * baud line.
                     *
                     * c45723627b fixed the arm one screen up and left
                     * these two. One function, two rules. */
                    if (radio_present == 1 && !iap_btnradiomute) {
                        iap_btnradiomute = true;
                        remote_mute = false;
                        tuner_set(RADIO_MUTE,0);
                    }
#endif
                }
                if(L2_PARAM(1) & 2) /* pause */
                {
                    if ((audio_status() & (AUDIO_STATUS_PLAY |
                                           AUDIO_STATUS_PAUSE))
                        == AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
#if CONFIG_TUNER
                    if (radio_present == 1 && !iap_btnradiomute) {
                        iap_btnradiomute = true;
                        remote_mute = true;
                        tuner_set(RADIO_MUTE,1);
                    }
#endif
                }
                if ((L2_PARAM(1) & (8 | 16)) && !iap_btnchapter)
                {
                    struct iap_chapter_info chapter;
                    struct mp3entry *id3 = audio_current_track();

                    iap_btnchapter = true;
                    if ((audio_status() & AUDIO_STATUS_PLAY)
                        && iap_current_chapter(id3, &chapter)
                        && chapter.count > 1)
                        iap_skip_chapter((L2_PARAM(1) & 8) ? 1 : -1);
                }
#if CONFIG_TUNER
                if(L2_PARAM(1) & 4) /* Mute toggle */
                {
                    /* Table 4-14 (p.227) byte 1 bit 2. Only the radio
                     * has a mute to toggle -- Rockbox has no mute for
                     * playback, and turning the volume down to nothing
                     * is not one, because there is nowhere to put the
                     * level it replaced. Latched with the same flag the
                     * Play/Resume and Pause arms use, so a held Mute
                     * toggles once. */
                    if (radio_present == 1 && !iap_btnradiomute) {
                        iap_btnradiomute = true;
                        remote_mute = !remote_mute;
                        tuner_set(RADIO_MUTE, remote_mute ? 1 : 0);
                    }
                }
#endif
                if(L2_PARAM(1) & 128) /* Shuffle */
                {
                    if (!iap_btnshuffle)
                    {
                        /* A physical button, not an accessory setting: no Restore
                         * on Exit to honour. */
                        iap_shuffle_state(!global_settings.playlist_shuffle,
                                          IAP_RESTORE_KEEP);
                        iap_btnshuffle = true;
                    }
                }
            }
            if(L2_HAVE(3) && L2_PARAM(2) != 0)
            {
                if(L2_PARAM(2) & 1) /* repeat */
                {
                    if (!iap_btnrepeat)
                    {
                        iap_repeat_next();
                        iap_btnrepeat = true;
                    }
                }

                if (L2_PARAM(2) & 2) /* power on */
                {
                    poweron_pressed = true;
                }

                /* Power off
                 * Not quite sure how to react to this, but stopping playback
                 * is a good start.
                 */
                if (L2_PARAM(2) & 0x04)
                {
                    if ((audio_status() & (AUDIO_STATUS_PLAY |
                                           AUDIO_STATUS_PAUSE))
                        == AUDIO_STATUS_PLAY)
                        btn |= BUTTON_RC_PLAY;
                }

                if(L2_PARAM(2) & 16) /* ffwd */
                    btn |= BUTTON_RC_RIGHT;
                if(L2_PARAM(2) & 32) /* frwd */
                    btn |= BUTTON_RC_LEFT;
                if(L2_PARAM(2) & 64) /* menu */
                    btn |= BUTTON_RC_MENU;
                if(L2_PARAM(2) & 128) /* select */
                    btn |= BUTTON_RC_SELECT;
            }
            if(L2_HAVE(4) && L2_PARAM(3) != 0)
            {
                if(L2_PARAM(3) & 1) /* up */
                    btn |= BUTTON_RC_UP;
                if (L2_PARAM(3) & 2) /* down */
                    btn |= BUTTON_RC_DOWN;
            }

            /* Commit the new button state in one go. The button driver
             * reads iap_remotebtn from interrupt context, a transient
             * BUTTON_NONE between two repeated button-down events would
             * register as a spurious release/press pair.
             */
            if (btn != BUTTON_NONE)
            {
                /* Only re-arm iap_repeatbtn when the state changes. A
                 * held button repeats its down event every 30-100ms,
                 * re-arming on every repeat makes iap_handlepkt()
                 * delay each following packet, so packets back up
                 * during the hold and stale events replay as phantom
                 * keypresses after the release.
                 */
                if (btn != iap_remotebtn)
                    iap_repeatbtn = 2;
                iap_remotebtn = btn;
                iap_timeoutbtn = 3;
            }
            else
            {
                /* Arm the delivery delay on the release edge too. Without
                 * it, a release and the next press can be drained back to
                 * back -- the drain loop no longer paces packets the way
                 * the old always-arm behaviour incidentally did -- so the
                 * 100Hz tick never observes BUTTON_NONE, no BUTTON_REL is
                 * posted, and two taps merge into a held press that turns
                 * into a seek at 300ms. A release is one-shot, unlike the
                 * 30-100ms repeat of a held button, so this cannot bring
                 * back the ghost-click stall.
                 */
                if (iap_remotebtn != BUTTON_NONE)
                    iap_repeatbtn = 2;
                /* An all-zero status is the release MFi 4.2.7 (p.226)
                 * requires, and it ends any seek that was running --
                 * see the note in iap_periodic(). */
                if (device.pb_seeking)
                    iap_seek_stop();
                iap_remotebtn = BUTTON_NONE;
                /* Shuffle and Repeat set iap_btnshuffle/iap_btnrepeat
                 * but map to no button, so btn stays NONE here. Zeroing
                 * the timeout unconditionally let iap_periodic() clear
                 * those debounce flags within 100ms, and the accessory's
                 * spec-mandated 30-100ms repeat then re-toggled shuffle
                 * -- calling settings_save(), and so writing to storage,
                 * around ten times a second for as long as the button
                 * was held. Keep the timeout alive while anything is
                 * still down.
                 */
                iap_timeoutbtn = any_bits ? 3 : 0;
            }

            /* Power on released.
             *
             * An absent byte is a zero byte: MFi 4.2.3 (p.216) says "it
             * is not necessary to transmit any trailing bytes in which
             * no bits are set", and 4.2.7 (p.226) makes the canonical
             * release "a button status packet with a 0x00 payload" --
             * one byte. Requiring the third state byte to be present
             * before looking at it meant a conformant accessory could
             * never clear this latch, so the GetDevCaps below was never
             * sent and stereo line-in was never configured. The
             * any_bits expression above already reads short packets
             * this way. */
            if (poweron_pressed
                && !(L2_HAVE(3) && (L2_PARAM(2) & 2)))
            {
                poweron_pressed = false;
#ifdef HAVE_LINE_REC
                /* Belkin TuneTalk microphone sends power-on press+release
                 * events once authentication sequence is finished,
                 * GetDevCaps command is ignored by the device when it is
                 * sent before power-on release event is received.
                 * XXX: It is unknown if other microphone devices are
                 * sending the power-on events.
                 */
                if (DEVICE_LINGO_SUPPORTED(0x01)) {
                    /* GetAccessoryCaps. Origin: Apple device (MFi
                     * C.5.5, p.538), so it carries the device's own
                     * transaction ID. C.5.5 gives it a 200 ms timeout
                     * with no retry, so a malformed one can have the
                     * accessory marked absent. */
                    IAP_TX_INIT(0x01, 0x07);
                    IAP_TX_PUT_IPOD_TRANSID();
                    iap_send_tx();
                }
#endif
            }

            break;
        }
        /* ACK (0x01)
         *
         * Sent from the iPod to the device
         */

        /* ImageButtonStatus (0x02)
         *
         * Transmit image button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x02
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x02:
        {
            if (!DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* VideoButtonStatus (0x03)
         *
         * Transmit video button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x03
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x03:
        {
            if (!DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* AudioButtonStatus (0x04)
         *
         * Transmit audio button events from the device to the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Simple Remote Lingo, always 0x02
         * 0x01: Command, always 0x04
         * 0x02: Button states 0:7
         * 0x03: Button states 8:15 (optional)
         * 0x04: Button states 16:23 (optional)
         * 0x05: Button states 24:31 (optional)
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_*
         */
        case 0x04:
        {
            /* 6 payload bytes, plus room for an IDPS transaction ID:
             * the recursive call below re-enters this handler, which
             * recomputes doff from device.auth.idps, so the copy has to
             * keep the transaction ID in place for it to skip. */
            unsigned char repeatbuf[8];

            if (!DEVICE_AUTHENTICATED) {
                cmd_ack(cmd, IAP_ACK_NO_AUTHEN);
                break;
            }

            /* This command shares ContextButtonStatus's (0x00) decode
             * for its first two state bytes and nothing after them, so
             * route it through that handler with byte index 2
             * translated and byte index 3 dropped.
             *
             * MFi Table 4-14 "Button states" (p.227) and Table 4-19
             * "Audio-specific button values" (p.231) agree exactly on
             * byte indices 0 and 1 -- volume, track skip, album skip,
             * stop, play, pause, mute, chapters, playlists, shuffle --
             * and agree on nothing beyond them:
             *
             *   byte 2   Table 4-14              Table 4-19
             *   0x01     Repeat Setting Advance  Repeat setting advance
             *   0x02     Power On                Begin FF
             *   0x04     Power Off               Begin REW
             *   0x08     Backlight 30 Seconds    Record
             *   0x10     Begin Fast Forward      Reserved
             *   0x20     Begin Rewind            Reserved
             *   0x40     Menu                    Reserved
             *   0x80     Select                  Reserved
             *   byte 3   Up/Down/Backlight Off   Reserved, all 8 bits
             *
             * Decoding one with the other made Begin REW read as Power
             * Off, which pauses when playing -- so Rewind stopped the
             * music -- and Begin FF read as Power On, which produces no
             * button, so fast-forward did nothing. Four reserved bits
             * produced ffwd, rewind, menu and select, and the wholly
             * reserved byte 3 produced Up and Down.
             *
             * Bytes 1-3 of the button status are optional (MFi Table
             * 4-18), so a conformant accessory may send fewer than the
             * full payload -- copy only what arrived. Under IDPS the
             * packet is 2 bytes longer, and the transaction ID is
             * copied along with it so the recursive call can skip it.
             */
            {
                unsigned int n = 6 + doff;
                if (len < n)
                    n = len;
                memcpy(repeatbuf, buf, n);
                repeatbuf[1] = 0x00;

                if (n > 4 + doff) {
                    unsigned char a2 = L2_PARAM(2);
                    unsigned char c2 = 0;

                    if (a2 & 0x01) c2 |= 0x01;  /* repeat advance */
                    if (a2 & 0x02) c2 |= 0x10;  /* Begin FF  -> ffwd */
                    if (a2 & 0x04) c2 |= 0x20;  /* Begin REW -> rewind */

                    /* Record and the reserved bits have no
                     * ContextButtonStatus equivalent and no Rockbox
                     * action. Map them onto Backlight for 30 Seconds,
                     * which the shared decode reads but does nothing
                     * with, so a held button still keeps the auto-
                     * release tracking alive without inventing one. */
                    if (a2 & 0xF8) c2 |= 0x08;

                    L2_PARAM_OF(repeatbuf, 2) = c2;
                }
                if (n > 5 + doff)
                    L2_PARAM_OF(repeatbuf, 3) = 0;  /* reserved in full */

                iap_handlepkt_mode2(n, repeatbuf);
            }

            cmd_ok(cmd);
            break;
        }

        /* The default response is IAP_ACK_BAD_PARAM */
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode02 Command");
#endif
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
