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

/* Lingo 0x03: Display Remote Lingo
 *
 * A bit of a hodgepogde of odds and ends.
 *
 * Used to control the equalizer in version 1.00 of the Lingo, but later
 * grew functions to control album art transfer and check the player
 * status.
 *
 * TODO:
 * - Actually support multiple equalizer profiles, currently only the
 *   profile 0 (equalizer disabled) is supported
 */

#include "iap-core.h"
#include "iap-lingo.h"
#include "iap-artwork.h"
#include "system.h"
#include "audio.h"
#include "sound.h"
#include "powermgmt.h"
#include "settings.h"
#include "metadata.h"
#include "playback.h"
#include "misc.h"
#ifdef USB_ENABLE_AUDIO
bool usb_audio_get_active(void);
#endif
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif

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

/* Parameters and their length check, in the same units.
 *
 * Display Remote commands are lingo byte, command byte, then a
 * transaction ID when one is in force, so the first parameter is at
 * 2 + doff and CHECKLEN wants 2 + n + doff for n parameter bytes.
 * Written out, the two are in different units -- the reads count from
 * the start of the packet, the check counts the whole packet -- and
 * this file wrote the same offset both as 2 and as 0x02, so even
 * grepping for one form missed half of them.
 *
 * L3_NEED(n) and L3_PARAM(n) both count parameters. For
 * SetiPodStateInfo and GetiPodStateInfo the information type is
 * PARAM(0) and its payload begins at PARAM(1). */
#define L3_NEED(n)  CHECKLEN(2 + (n) + doff)
#define L3_PARAM(n) (buf[2 + doff + (n)])
/* Is parameter n-1 there? For trailing bytes a shorter accessory may
 * omit -- the same shape as iap-lingo4.c's L4_HAVE(). */
#define L3_HAVE(n)  (len >= (unsigned int)(2 + (n) + doff))

/* Check for authenticated state, and return an ACK Not
 * Authenticated on failure.
 */
#define CHECKAUTH do { \
        if (!DEVICE_AUTHENTICATED) { \
            cmd_ack(cmd, IAP_ACK_NO_AUTHEN); \
            return; \
        }} while(0)

/* File-scope transID for the current lingo 0x03 packet.
 * Parsed at the top of iap_handlepkt_mode3() and used by
 * cmd_ack() and L3_TX_TRANSID() in all handlers. */
static uint8_t l3_tid_hi, l3_tid_lo;

static void cmd_ack(const unsigned char cmd, const unsigned char status)
{
    IAP_TX_INIT(0x03, 0x00);
    if (DEVICE_TRANSID_ACTIVE) {
        IAP_TX_PUT(l3_tid_hi);
        IAP_TX_PUT(l3_tid_lo);
    }
    IAP_TX_PUT(status);
    IAP_TX_PUT(cmd);

    iap_send_tx();
}

#define cmd_ok(cmd) cmd_ack((cmd), IAP_ACK_OK)

/* Insert transID into a TX response packet after IAP_TX_INIT */
#define L3_TX_TRANSID() do { \
        if (DEVICE_TRANSID_ACTIVE) { \
            IAP_TX_PUT(l3_tid_hi); \
            IAP_TX_PUT(l3_tid_lo); \
        }} while(0)

void iap_handlepkt_mode3(const unsigned int len, const unsigned char *buf)
{
    unsigned int cmd = buf[1];

    /* Extra data offset for the IDPS transaction ID (0 or 2). Nothing
     * below adds it by hand: L3_PARAM(n) and L3_NEED(n) take parameter
     * numbers and fold it in, so the same code works for legacy and
     * IDPS packets and the check and the reads stay in one unit. */
    unsigned int doff = 0;

    l3_tid_hi = 0;
    l3_tid_lo = 0;

    /* We expect at least two bytes in the buffer, one for the
     * state bits.
     */
    /* Equivalent mutant: the framer never delivers a payload shorter
     * than this. MFi 2.5.2 (p.110) puts the smallest packet payload at
     * 0x02 and iap_getc() enforces it, so len is always at least two
     * and this can never fire. Kept because it states the handler's
     * precondition where a reader looks for it, and recorded so the
     * mutation sweep's survivor list stays fully accounted for. */
    CHECKLEN(2);

    /* After IDPS, all packets include a 2-byte transID after the
     * command byte.  Extract it and set doff so all data-reading
     * offsets and CHECKLENs are adjusted correctly.
     *
     * This has to happen before the negotiation check below, which
     * acks. Acking first left l3_tid at 0x0000, and MFi 2.6.1.1 (p.111)
     * has the accessory ignore any response whose transaction ID
     * matches no command it sent -- so an accessory that probed a
     * Display Remote command without having declared lingo 0x03 never
     * saw the rejection and retried indefinitely. Lingoes 0x02 and 0x04
     * already parse before their own checks. */
    if (DEVICE_TRANSID_ACTIVE) {
        CHECKLEN(4);    /* lingo + cmd + transID(2) minimum */
        l3_tid_hi = buf[2];
        l3_tid_lo = buf[3];
        doff = 2;
    }

    /* Lingo 0x03 must have been negotiated */
    if (!DEVICE_LINGO_SUPPORTED(0x03)) {
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }

    switch (cmd)
    {
        /* ACK (0x00)
         *
         * Sent from the iPod to the device
         */

        /* GetCurrentEQProfileIndex (0x01)
         *
         * Return the index of the current equalizer profile.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x01
         *
         * Returns:
         * RetCurrentEQProfileIndex
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x02
         * 0x02-0x05: Index as an unsigned 32bit integer
         */
        case 0x01:
        {
            IAP_TX_INIT(0x03, 0x02);
            L3_TX_TRANSID();
            IAP_TX_PUT_U32(0x00);

            iap_send_tx();
            break;
        }

        /* RetCurrentEQProfileIndex (0x02)
         *
         * Sent from the iPod to the device
         */

        /* SetCurrentEQProfileIndex (0x03)
         *
         * Set the active equalizer profile
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x03
         * 0x02-0x05: Profile index to activate
         * 0x06: Whether to restore the previous profile on detach
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_CMD_FAILED
         *
         * TODO: Figure out return code for invalid index
         */
        case 0x03:
        {
            uint32_t index;

            L3_NEED(5);

            index = get_u32(&L3_PARAM(0));

            if (index > 0) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            /* Currently, we just ignore the command and acknowledge it */
            cmd_ok(cmd);
            break;
        }

        /* GetNumEQProfiles (0x04)
         *
         * Get the number of available equalizer profiles
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x04
         *
         * Returns:
         * RetNumEQProfiles
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x05
         * 0x02-0x05: Number as an unsigned 32bit integer
         */
        case 0x04:
        {
            IAP_TX_INIT(0x03, 0x05);
            L3_TX_TRANSID();
            /* Return one profile (0, the disabled profile) */
            IAP_TX_PUT_U32(0x01);

            iap_send_tx();
            break;
        }

        /* RetNumEQProfiles (0x05)
         *
         * Sent from the iPod to the device
         */

        /* GetIndexedEQProfileName (0x06)
         *
         * Return the name of the indexed equalizer profile
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x06
         * 0x02-0x05: Profile index to get the name of
         *
         * Returns on success:
         * RetIndexedEQProfileName
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x06
         * 0x02-0xNN: Name as an UTF-8 null terminated string
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         *
         * TODO: Figure out return code for out of range index
         */
        case 0x06:
        {
            uint32_t index;

            L3_NEED(4);

            index = get_u32(&L3_PARAM(0));

            if (index > 0) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            IAP_TX_INIT(0x03, 0x07);
            L3_TX_TRANSID();
            IAP_TX_PUT_STRING("Default");

            iap_send_tx();
            break;
        }

        /* RetIndexedQUProfileName (0x07)
         *
         * Sent from the iPod to the device
         */

        /* SetRemoteEventNotification (0x08)
         *
         * Set events the device would like to be notified about
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x08
         * 0x02-0x05: Event bitmask
         *
         * Returns:
         * IAP_ACK_OK
         */
        case 0x08:
        {
            struct iap_chapter_info chapter;
            struct mp3entry *id3;
            struct tm* tm;

            L3_NEED(4);
            CHECKAUTH;

            /* Save the current state of the various attributes we track */
            device.trackpos_ms = iap_get_trackpos();
            device.track_index = iap_get_trackindex();
            id3 = audio_current_track();
            if ((audio_status() & AUDIO_STATUS_PLAY)
                && iap_current_chapter(id3, &chapter)) {
                device.chapter_index = chapter.index;
                device.chapter_track_index = device.track_index;
                device.chapter_count = chapter.count;
            } else {
                device.chapter_index = UINT32_MAX;
                device.chapter_track_index = UINT32_MAX;
                device.chapter_count = 0;
            }
            /* Same value the notification compares against, so a
             * subscription taken during a seek does not fire on its
             * first tick for a state that had not changed. */
            device.play_status = iap_play_state_reported();
            /* device.mute is deliberately not touched. Every other
             * line here records what the state is; asserting mute off
             * made the flag disagree with the codec, which is still at
             * sound_min() from wherever the mute was applied -- nothing
             * here lifts the attenuation. A following GetiPodStateInfo
             * then answered "not muted, UI volume 128" with the
             * hardware at -60 dB. */
            /* Saved in the transmitted 0..255 form, not dB: the field is
             * an unsigned char and the dB value is signed, so -25 dB
             * would land as 231 and defeat the change detection in
             * iap_periodic().
             *
             * volume_reported is cleared so the first tick after an
             * accessory enables volume notifications sends the current
             * level. Change detection alone would leave an accessory
             * that never sees the user touch the volume with no idea
             * what it is. */
            device.volume = iap_volume_to_byte(global_status.volume);
            device.volume_reported = false;
            device.power_state = charger_input_state;
            device.battery_level = battery_level();
            /* TODO: Fix this */
            device.equalizer_index = 0;
            device.shuffle = global_settings.playlist_shuffle;
            device.repeat = global_settings.repeat_mode;
            tm = get_time();
            memcpy(&(device.datetime), tm, sizeof(struct tm));
            device.alarm_state = 0;
            device.alarm_hour = 0;
            device.alarm_minute = 0;
            /* TODO: Fix this */
            device.backlight = 0;
            device.hold = button_hold();
            device.soundcheck = 0;
            device.audiobook = 0;
            device.trackpos_s = (device.trackpos_ms/1000) & 0xFFFF;

            /* Get the notification bits */
            device.do_notify = false;
            device.changed_notifications = 0;
            /* A fresh subscription is owed the current state, not just
             * the next change. The volume does this through
             * volume_reported; the power and battery state needs the
             * same, or an accessory that enables bit 5 hears nothing
             * until the battery moves. */
            device.volume_reported = false;
            device.power_reported = false;
            /* The mask is stored whole and acked Success even though
             * five of the bits Table 4-59 (p.255) defines are never
             * served: 06 Equalizer setting, 11 Backlight level, 13
             * Sound check state, 14 Audiobook speed and 17 Track
             * capabilities. Each is backed by a stub in this firmware
             * -- equalizer_index, backlight, soundcheck and audiobook
             * are assigned constants above and nothing ever moves them,
             * and every capability in event 0x11 is a constant zero --
             * so there is no change to report.
             *
             * This said six, and named four. Bit 18, Playback engine
             * contents, was in the list and is not a stub: its value is
             * playlist_amount(), and iap_periodic() was already sending
             * exactly it for the Extended Interface twin. It is served
             * now.
             *
             * That is the same shape as the Extended Interface bit 01
             * defect, and unlike that one it cannot be answered on the
             * wire. Lingo 3 has no GetSupportedRemoteEventNotification:
             * 4.3.11 (p.255) gives the accessory no way to ask which
             * bits work and defines no partial-support status, so a
             * non-Success ack would refuse the whole mask and break an
             * accessory that asked for everything and needed one bit.
             * Masking the unserved bits out would change nothing it can
             * observe.
             *
             * Recorded rather than fixed, so the next reader does not
             * take the silence for an oversight or the refusal for an
             * improvement. Serving them means implementing the four
             * features behind them. */
            device.notifications = get_u32(&L3_PARAM(0));
            if (device.notifications)
                device.do_notify = true;

            cmd_ok(cmd);
            break;
        }

        /* RemoteEventNotification (0x09)
         *
         * Sent from the iPod to the device
         */

        /* GetRemoteEventStatus (0x0A)
         *
         * Request the events changed since the last call to
         * GetREmoteEventStatus or SetRemoteEventNotification
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0A
         *
         * This command requires authentication
         *
         * Returns:
         * RetRemoteEventNotification
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0B
         * 0x02-0x05: Event status bits
         */
        case 0x0A:
        {
            CHECKAUTH;
            /* Detects changes itself, so polling works without any
             * subscription -- 4.3.13 (p.263) says it must. Reading
             * clears, as the same paragraph requires.
             *
             * Read before the buffer is opened, like every other call
             * in this file that grew. It samples the track position,
             * the battery and the hold switch now, and the next thing
             * added to it will be someone else's; a growing function
             * inside a TX window is the trap this layer has been fixed
             * for four times. */
            {
                uint32_t changed = iap_take_changed_events();

                IAP_TX_INIT(0x03, 0x0B);
                L3_TX_TRANSID();
                IAP_TX_PUT_U32(changed);
            }

            iap_send_tx();
            break;
        }

        /* RetRemoteEventStatus (0x0B)
         *
         * Sent from the iPod to the device
         */

        /* GetiPodStateInfo (0x0C)
         *
         * Request state information from the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0C
         * 0x02: Type information
         *
         * This command requires authentication
         *
         * Returns:
         * RetiPodStateInfo
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0D
         * 0x02: Type information
         * 0x03-0xNN: State information
         */
        case 0x0C:
        {
            struct mp3entry* id3;
            struct tm* tm;
            bool has_artwork = false;

            L3_NEED(1);
            CHECKAUTH;

            /* Before the buffer is opened; see the note at 0x13.
             *
             * get_time() belongs here for the same reason and was left
             * behind when the other two were moved. It blocks: once a
             * second rtc_dirty() is true and it reads the RTC over i2c
             * (rtc_pcf50605.c:44 on the 5G, rtc-6g.c on the 6G), which
             * takes i2c_mtx and yields inside pp_i2c_wait_not_busy().
             * Called from infoType 0x09 with the buffer already open,
             * that hands the CPU to the UI thread, and on the 5G any
             * tuner_set() from the radio screen reaches iap_send_pkt(),
             * which rewinds iap_txnext to the payload start. The date
             * bytes then land after the tuner command in one frame
             * under a single length and checksum, and the reply the
             * accessory asked for is gone. */
            id3 = audio_current_track();
            tm = get_time();
            if (L3_PARAM(0) == 0x11)
                has_artwork = iap_artwork_available(id3);

            IAP_TX_INIT(0x03, 0x0D);
            L3_TX_TRANSID();
            IAP_TX_PUT(L3_PARAM(0));

            switch (L3_PARAM(0))
            {
                /* 0x00: Track position
                 * Data length: 4
                 */
                case 0x00:
                {
                    IAP_TX_PUT_U32(id3->elapsed);

                    iap_send_tx();
                    break;
                }

                /* 0x01: Track index
                 * Data length: 4
                 */
                case 0x01:
                {
                    IAP_TX_PUT_U32(iap_get_trackindex());

                    iap_send_tx();
                    break;
                }

                /* 0x02: Chapter information
                 * Data length: 8
                 */
                case 0x02:
                {
                    struct iap_chapter_info chapter;

                    IAP_TX_PUT_U32(iap_get_trackindex());
                    if ((audio_status() & AUDIO_STATUS_PLAY)
                        && iap_current_chapter(id3, &chapter)) {
                        IAP_TX_PUT_U16(chapter.count);
                        IAP_TX_PUT_U16(chapter.index);
                    } else {
                        IAP_TX_PUT_U16(0);
                        IAP_TX_PUT_U16(0xFFFF);
                    }

                    iap_send_tx();
                    break;
                }

                /* 0x03: Play status
                 * Data length: 1
                 */
                case 0x03:
                {
                    /* Table 4-62 (p.262) has FF and REW here too; see
                     * iap_play_state_reported().
                     */
                    IAP_TX_PUT(iap_play_state_reported());

                    iap_send_tx();
                    break;
                }

                /* 0x04: Mute/UI/Volume
                 * Data length: 2
                 */
                case 0x04:
                {
                    if (device.mute == false) {
                        /* Mute status False*/
                        IAP_TX_PUT(0x00);
                        /* Volume.
                         *
                         * This reported a hardcoded 0xFF while USB
                         * audio was streaming, on the reasoning that
                         * the DAC owns volume and should apply full
                         * gain. That suits a dock with its own knob and
                         * nothing else: the digital stream carries no
                         * attenuation of its own, because SOUND_VOLUME
                         * is applied in the CS42L55 headphone amp
                         * (firmware/drivers/audio/cs42l55.c:44) and
                         * HAVE_SW_VOLUME_CONTROL is not defined for
                         * either target. An accessory without a volume
                         * control of its own -- a Bluetooth transmitter,
                         * say -- was therefore pinned at full scale with
                         * no way to follow the iPod.
                         *
                         * MFi Table 4-61 asks for the UI volume level,
                         * which p.261 says is "normalized to volume limit
                         * settings", so report that scale. */
                        IAP_TX_PUT(iap_volume_to_ui_byte(global_status.volume));

                    } else {
                        /* Mute status True*/
                        IAP_TX_PUT(0x01);
                        /* Volume should be 0 if muted */
                        IAP_TX_PUT(0x00);
                    }

                    iap_send_tx();
                    break;
                }

                /* 0x05: Power/Battery
                 * Data length: 2
                 */
                case 0x05:
                {
                    iap_fill_power_state();

                    iap_send_tx();
                    break;
                }

                /* 0x06: Equalizer state
                 * Data length: 4
                 */
                case 0x06:
                {
                    /* Currently only one equalizer setting supported, 0 */
                    IAP_TX_PUT_U32(0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x07: Shuffle
                 * Data length: 1
                 */
                case 0x07:
                {
                    IAP_TX_PUT(global_settings.playlist_shuffle?0x01:0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x08: Repeat
                 * Data length: 1
                 */
                case 0x08:
                {
                    switch (global_settings.repeat_mode)
                    {
                        case REPEAT_OFF:
                        {
                            IAP_TX_PUT(0x00);
                            break;
                        }

                        case REPEAT_ONE:
                        {
                            IAP_TX_PUT(0x01);
                            break;
                        }

                        case REPEAT_ALL:
                        {
                            IAP_TX_PUT(0x02);
                            break;
                        }

                        default:
                        {
                            /* Table 4-64 (p.262) has no encoding for
                             * REPEAT_SHUFFLE or REPEAT_AB, and without
                             * an arm here the reply went out with no
                             * data byte at all -- Table 4-71 (p.265)
                             * makes infoData follow Table 4-61, which
                             * gives repeat one byte. "All" is the
                             * closest of the three, and iap-lingo4.c
                             * already answers it. */
                            IAP_TX_PUT(0x02);
                            break;
                        }
                    }

                    iap_send_tx();
                    break;
                }

                /* 0x09: Data/Time
                 * Data length: 6
                 */
                case 0x09:
                {
                    /* Year */
                    /* MFi Table 4-61 (p.260) and Table 4-72 (p.264):
                    * "A value of 2005 represents the year 2005 A.D."
                    * Rockbox keeps tm_year as years since 1900, the
                    * POSIX convention -- rtc-6g.c:47 and
                    * rtc_pcf50605.c:54 both compute "buf[6] + 100",
                    * and valid_time() rejects anything outside
                    * 100..199 -- so it needs converting. Sent raw it
                    * put year 126 on the wire in 2026. */
                    IAP_TX_PUT_U16(tm->tm_year + 1900);

                    /* Month */
                    IAP_TX_PUT(tm->tm_mon+1);

                    /* Day */
                    IAP_TX_PUT(tm->tm_mday);

                    /* Hour */
                    IAP_TX_PUT(tm->tm_hour);

                    /* Minute */
                    IAP_TX_PUT(tm->tm_min);

                    iap_send_tx();
                    break;
                }

                /* 0x0A: Alarm
                 * Data length: 3
                 */
                case 0x0A:
                {
                    /* Alarm not supported, always off */
                    IAP_TX_PUT(0x00);
                    IAP_TX_PUT(0x00);
                    IAP_TX_PUT(0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x0B: Backlight
                 * Data length: 1
                 */
                case 0x0B:
                {
                    /* TOOD: Find out how to do this */
                    IAP_TX_PUT(0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x0C: Hold switch
                 * Data length: 1
                 */
                case 0x0C:
                {
                    IAP_TX_PUT(button_hold()?0x01:0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x0D: Sound check
                 * Data length: 1
                 */
                case 0x0D:
                {
                    /* TODO: Find out what the hell this is. Default to off */
                    IAP_TX_PUT(0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x0E: Audiobook
                 * Data length: 1
                 */
                case 0x0E:
                {
                    /* Default to normal */
                    IAP_TX_PUT(0x00);

                    iap_send_tx();
                    break;
                }

                /* 0x0F: Track position in seconds
                 * Data length: 2
                 */
                case 0x0F:
                {
                    /* id3 was read at the top of this handler, before
                     * the buffer was opened. Re-reading here would take
                     * id3_mutex with a half-built packet in the TX
                     * buffer, which is the window the hoist closes. */
                    unsigned int pos = id3->elapsed/1000;

                    IAP_TX_PUT_U16(pos);

                    iap_send_tx();
                    break;
                }

                /* 0x10: Mute/UI/Absolute volume
                 * Data length: 3
                 */
                case 0x10:
                {
                    if (device.mute == false) {
                        /* Mute status False*/
                        IAP_TX_PUT(0x00);
                        /* Table 4-61 (p.261) splits these two: byte 1 is
                         * the UI volume, "normalized to volume limit
                         * settings", byte 2 the absolute volume, "not
                         * normalized". They coincide until the user
                         * lowers the volume limit. See the note on info
                         * type 0x04 above for why neither reports a
                         * hardcoded maximum while USB audio streams. */
                        IAP_TX_PUT(iap_volume_to_ui_byte(global_status.volume));
                        IAP_TX_PUT(iap_volume_to_byte(global_status.volume));

                    } else {
                        /* Mute status True*/
                        IAP_TX_PUT(0x01);
                        /* Volume should be 0 if muted */
                        IAP_TX_PUT(0x00);
                        IAP_TX_PUT(0x00);
                    }

                    iap_send_tx();
                    break;
                }

                case 0x11:
                {
                    struct iap_chapter_info chapter;
                    uint32_t capabilities =
                        iap_current_chapter(id3, &chapter) ? BIT_N(1) : 0;

                    if (has_artwork)
                        capabilities |= BIT_N(2);
                    IAP_TX_PUT_U32(capabilities);
                    iap_send_tx();
                    break;
                }
                default:
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
            }
            break;
        }

        /* RetiPodStateInfo (0x0D)
         *
         * Sent from the iPod to the device
         */

        /* SetiPodStateInfo (0x0E)
         *
         * Set status information to new values
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0E
         * 0x02: Type of information to change
         * 0x03-0xNN: New information
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_CMD_FAILED
         * IAP_ACK_BAD_PARAM
         */
        case 0x0E:
        {
            L3_NEED(1);
            CHECKAUTH;
            switch (L3_PARAM(0))
            {
                /* Track position (ms)
                 * Data length: 4
                 */
                case 0x00:
                {
                    uint32_t pos;

                    L3_NEED(5);
                    pos = get_u32(&L3_PARAM(1));
                    /* audio_on_ff_rewind() (apps/playback.c:3374)
                     * returns on PLAY_STOPPED, so with nothing playing
                     * the seek does not happen -- and 4.3.17 (p.266)
                     * has the ack carry "the results of the operation".
                     * Status 0x02, matching the two precedents in this
                     * same switch. */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        break;
                    }
                    audio_ff_rewind(pos);

                    cmd_ok(cmd);
                    break;
                }

                /* Track index
                 * Data length: 4
                 */
                case 0x01:
                {
                    uint32_t index;

                    L3_NEED(5);
                    index = get_u32(&L3_PARAM(1));
                    /* Same guard as SetCurrentPlayingTrack below:
                     * audio_skip() walks an out-of-range offset back one
                     * track at a time holding id3_mutex, so a wild index
                     * hangs the device instead of being rejected. */
                    if (index >= (uint32_t)playlist_amount())
                    {
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                        break;
                    }
                    audio_skip(index-iap_get_trackindex());

                    cmd_ok(cmd);
                    break;
                }

                /* Chapter index
                 * Data length: 2
                 */
                case 0x02:
                {
                    uint16_t index;

                    L3_NEED(3);
                    index = get_u16(&L3_PARAM(1));
                    if (!(audio_status() & AUDIO_STATUS_PLAY))
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    else if (!iap_set_chapter(index))
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    else
                        cmd_ok(cmd);
                    break;
                }

                /* Play status
                 * Data length: 1
                 */
                case 0x03:
                {
                    L3_NEED(2);

                    /* Stop, Play and Pause are all exits from a seek,
                     * and Table 4-62 (p.262) lists them as peers of
                     * 0x03 FF and 0x04 REW -- nothing obliges an
                     * accessory to send 0x05 EndFFRew first.
                     *
                     * Ending it here rather than letting the ten-second
                     * safety release in iap_periodic() do it.
                     * iap_seek_start() leaves iap_remotebtn holding
                     * BUTTON_RC_RIGHT and iap_timeoutbtn at
                     * IAP_BTN_HELD, and none of these three arms
                     * touched either, so for those ten seconds
                     * iap_play_state_reported() answered Fast forward
                     * to a stopped player, button-clickwheel.c:479 ORed
                     * the phantom button into the user's own input and
                     * the browser scrolled by itself, and the accessory
                     * never got the seek-stop notifications it was
                     * owed. iap-lingo4.c:2687 does this before its own
                     * audio_stop(); this is the same line for the lingo
                     * that shares the seek helpers with it.
                     *
                     * No TX buffer is open here, so the packets
                     * iap_seek_stop() sends are safe. */
                    if (L3_PARAM(1) <= 0x02 && device.pb_seeking)
                        iap_seek_stop();

                    switch(L3_PARAM(1))
                    {
                        case 0x00:
                        {
                            audio_stop();
                            cmd_ok(cmd);
                            break;
                        }

                        case 0x01:
                        {
                            /* Table 4-62 (p.262) annotates this value
                             * "start or resume playback" for this
                             * command. Resuming alone is a no-op on a
                             * stopped engine, and this arm acked
                             * Success for it. */
                            if (!iap_play_or_resume())
                                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                            else
                                cmd_ok(cmd);
                            break;
                        }

                        case 0x02:
                        {
                            audio_pause();
                            cmd_ok(cmd);
                            break;
                        }

                        /* Table 4-62 (p.262): "0x03 Fast forward (FF),
                         * 0x04 Fast rewind (REW), 0x05 End fast forward
                         * or rewind mode", and Table 4-74 (p.267)
                         * describes this info type as "The play status
                         * of the Apple device (play, pause, stop, FF or
                         * REW)".
                         *
                         * All three were answered Command Failed, so a
                         * head unit speaking only Display Remote had no
                         * seek at all -- the Extended Interface lingo
                         * has had one since PlayControl was written.
                         * Same helpers, so the two cannot drift. */
                        case 0x03:
                        case 0x04:
                        {
                            /* Refused with nothing playing -- see the
                             * note on iap_seek_start(). */
                            if (!iap_seek_start(L3_PARAM(1) == 0x03))
                                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                            else
                                cmd_ok(cmd);
                            break;
                        }

                        case 0x05:
                        {
                            iap_seek_stop();
                            cmd_ok(cmd);
                            break;
                        }

                        default:
                        {
                            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                            break;
                        }
                    }
                    break;
                }

                case 0x04:
                {
                    /* Table 4-74 (p.267) gives this info type three
                     * data bytes -- mute state, UI volume level and
                     * bRestoreOnExit -- so the spec-exact check is
                     * L3_NEED(4). It stays at three, and the trailing
                     * byte is read only when it is there: the two bytes
                     * the command exists to carry are the two required
                     * ones, and refusing a dock that sends the shorter
                     * form would stop its volume control working
                     * outright. Same shape as iap-lingo4.c's SetShuffle
                     * and SetRepeat, whose RestoreOnExit byte the spec
                     * itself calls optional. */
                    L3_NEED(3);
#ifdef USB_ENABLE_AUDIO
                    /* In source mode, volume is controlled by the
                     * external DAC — don't modify Rockbox volume. */
                    if (usb_audio_get_active())
                    {
                        /* Volume belongs to the external DAC while the
                         * digital stream is live, so nothing here can
                         * apply it. MFi 4.3.17 (p.266) has this command
                         * answered by "an iPodAck command with the
                         * results of the operation", and the operation
                         * did not happen -- Success said it had, and a
                         * following GetiPodStateInfo then reported the
                         * unchanged level, so the two answers
                         * disagreed. The device also advertises UI
                         * Volume control in RetiPodOptionsForLingo
                         * (Table 3-132 p.194 bit 00), which is what
                         * invited the command. */
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        break;
                    }
#endif
                    /* bRestoreOnExit, Table 4-75 (p.270). Armed before
                     * the level moves, so what gets remembered is the
                     * user's own. Optional byte: absent means the
                     * accessory did not ask, which is IAP_RESTORE_NO. */
                    iap_volume_restore_arm(L3_HAVE(4) && L3_PARAM(3)
                                           ? IAP_RESTORE_YES
                                           : IAP_RESTORE_NO);

                    if (L3_PARAM(1)==0x00){
                        /* Not Muted */
                        {
                            int vol = iap_byte_to_volume(L3_PARAM(2));
                            /* Byte 1 is a UI volume level (MFi Table
                             * 4-73, p.267), so it maps onto the volume
                             * limit rather than sound_max. The clamp is
                             * belt and braces: this is written straight
                             * to global_status and re-applied on every
                             * cold start, so an out-of-range value would
                             * be a silent, permanent mute surviving a
                             * reboot. */
                            if (vol < sound_min(SOUND_VOLUME))
                                vol = sound_min(SOUND_VOLUME);
                            else if (vol > sound_max(SOUND_VOLUME))
                                vol = sound_max(SOUND_VOLUME);
                            global_status.volume = vol;

                            /* Storing the level is not applying it.
                             * setvol() (apps/misc.c:871) is what calls
                             * sound_set_volume(), and it is what every
                             * other volume control in Rockbox goes
                             * through -- the wheel, the sound menu, the
                             * radio screen. Without it an accessory
                             * changed the setting and nothing else.
                             * It also lifts a mute, because it re-applies
                             * the stored level the mute left untouched. */
                            iap_set_mute(false);
                        }
                    }
                    else {
                        /* Muting was recorded and reported and never
                         * applied, so an accessory that muted the iPod
                         * was told it had worked and heard no change.
                         *
                         * There is no mute in the app-level sound API --
                         * audiohw_mute() is static inside each codec
                         * driver -- so this attenuates to sound_min
                         * instead, which is -60 dB on the CS42L55 and
                         * -90 dB on the WM8758. global_status.volume is
                         * deliberately left alone: it is the level to
                         * come back to, and persisting a minimum there
                         * would be a silent mute surviving a reboot.
                         *
                         * The cost of that choice is that starting a
                         * track re-applies global_status.volume
                         * (apps/playback.c:3074) and so lifts the mute.
                         * Holding it across a track change needs a mute
                         * concept that Rockbox does not have.
                         *
                         * sound_set_volume() writes global_status.volume
                         * itself (firmware/sound.c:320), so the level to
                         * come back to has to be put back after it. */
                        iap_set_mute(true);
                    }
                    cmd_ok(cmd);
                    break;
                }

                /* Equalizer
                 * Data length: 5
                 */
                case 0x06:
                {
                    uint32_t index;

                    L3_NEED(6);
                    index = get_u32(&L3_PARAM(1));
                    if (index == 0) {
                        cmd_ok(cmd);
                    } else {
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    }
                    break;
                }

                /* Shuffle
                 * Data length: 2
                 */
                case 0x07:
                {
                    L3_NEED(3);

                    switch(L3_PARAM(1))
                    {
                        case 0x00:
                        {
                            iap_shuffle_state(false, L3_PARAM(2) ? IAP_RESTORE_YES : IAP_RESTORE_NO);
                            cmd_ok(cmd);
                            break;
                        }
                        case 0x01:
                        {
                            iap_shuffle_state(true, L3_PARAM(2) ? IAP_RESTORE_YES : IAP_RESTORE_NO);
                            cmd_ok(cmd);
                            break;
                        }

                        /* 0x02 is "Shuffle albums" in Table 4-63
                         * (p.262), and this device cannot do it -- it
                         * used to answer Success and shuffle tracks
                         * instead, which is a different thing done
                         * silently. The Extended Interface sibling
                         * refuses anything above 0x01, so the same
                         * device gave two answers to the same request.
                         * Falls through to the default below. */

                        default:
                        {
                            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                            break;
                        }
                    }
                    break;
                }

                /* Repeat
                 * Data length: 2
                 */
                case 0x08:
                {
                    L3_NEED(3);

                    switch(L3_PARAM(1))
                    {
                        case 0x00:
                        {
                            iap_repeat_state(REPEAT_OFF, L3_PARAM(2) ? IAP_RESTORE_YES : IAP_RESTORE_NO);
                            cmd_ok(cmd);
                            break;
                        }
                        case 0x01:
                        {
                            iap_repeat_state(REPEAT_ONE, L3_PARAM(2) ? IAP_RESTORE_YES : IAP_RESTORE_NO);
                            cmd_ok(cmd);
                            break;
                        }
                        case 0x02:
                        {
                            iap_repeat_state(REPEAT_ALL, L3_PARAM(2) ? IAP_RESTORE_YES : IAP_RESTORE_NO);
                            cmd_ok(cmd);
                            break;
                        }
                        default:
                        {
                            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                            break;
                        }
                    }
                    break;
                }

                /* Date/Time
                 * Data length: 6
                 */
                case 0x09:
                {
                    L3_NEED(7);

                    cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    break;
                }

                /* Alarm
                 * Data length: 4
                 */
                case 0x0A:
                {
                    L3_NEED(5);

                    cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    break;
                }

                /* Backlight
                 * Data length: 2
                 */
                case 0x0B:
                {
                    L3_NEED(3);

                    cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    break;
                }

                /* Sound check
                 * Data length: 2
                 */
                case 0x0D:
                {
                    L3_NEED(3);

                    cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    break;
                }

                /* Audio book speed
                 * Data length: 2
                 */
                case 0x0E:
                {
                    L3_NEED(3);

                    if (L3_PARAM(2) > 0x01)
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    else if (L3_PARAM(1) == 0x00)
                        cmd_ok(cmd);
                    else if (L3_PARAM(1) == 0xFF || L3_PARAM(1) == 0x01)
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                    else
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }

                /* Track position (s)
                 * Data length: 2
                 */
                case 0x0F:
                {
                    uint16_t pos;

                    L3_NEED(3);
                    pos = get_u16(&L3_PARAM(1));
                    /* audio_on_ff_rewind() (apps/playback.c:3374)
                     * returns on PLAY_STOPPED, so with nothing playing
                     * the seek does not happen -- and 4.3.17 (p.266)
                     * has the ack carry "the results of the operation".
                     * Status 0x02, matching the two precedents in this
                     * same switch. */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        break;
                    }
                    audio_ff_rewind(1000L * pos);

                    cmd_ok(cmd);
                    break;
                }

                /* Volume/Mute/Absolute
                 * Data length: 4
                 * TODO: Fix this
                 */
                case 0x10:
                {
                    /* Table 4-74 (p.268): mute state, UI volume,
                     * Absolute volume, bRestoreOnExit. */
                    L3_NEED(5);
#ifdef USB_ENABLE_AUDIO
                    if (usb_audio_get_active())
                    {
                        /* Volume belongs to the external DAC while the
                         * digital stream is live, so nothing here can
                         * apply it. MFi 4.3.17 (p.266) has this command
                         * answered by "an iPodAck command with the
                         * results of the operation", and the operation
                         * did not happen -- Success said it had, and a
                         * following GetiPodStateInfo then reported the
                         * unchanged level, so the two answers
                         * disagreed. The device also advertises UI
                         * Volume control in RetiPodOptionsForLingo
                         * (Table 3-132 p.194 bit 00), which is what
                         * invited the command. */
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        break;
                    }
#endif
                    /* bRestoreOnExit, Table 4-75 (p.270). Byte 3 of
                     * this info type's four, so it is required here
                     * and L3_NEED(5) above has already checked for it. */
                    iap_volume_restore_arm(L3_PARAM(4) ? IAP_RESTORE_YES
                                                       : IAP_RESTORE_NO);

                    if (L3_PARAM(1)==0x00){
                        /* Not Muted */
                        {
                            int vol;

                            /* MFi Table 4-74 (p.269): "Byte 1: UI volume
                             * level ... If the accessory sets this byte
                             * to 0, the Apple device uses the Absolute
                             * volume setting."
                             *
                             * A zero UI byte is therefore not a request
                             * for silence, it is a request to read byte
                             * 2 instead. Taking it literally meant an
                             * accessory that drives the absolute scale
                             * -- and so leaves byte 1 at zero, as the
                             * spec tells it to -- muted the iPod on
                             * every volume command it sent.
                             *
                             * The two scales differ: the UI byte is
                             * normalized to the volume limit, the
                             * absolute byte is not. */
                            if (L3_PARAM(2) == 0x00)
                                vol = iap_byte_to_abs_volume(L3_PARAM(3));
                            else
                                vol = iap_byte_to_volume(L3_PARAM(2));

                            /* The clamp is belt and braces: this is
                             * written straight to global_status and
                             * re-applied on every cold start, so an
                             * out-of-range value would be a silent,
                             * permanent mute surviving a reboot. */
                            if (vol < sound_min(SOUND_VOLUME))
                                vol = sound_min(SOUND_VOLUME);
                            else if (vol > sound_max(SOUND_VOLUME))
                                vol = sound_max(SOUND_VOLUME);
                            global_status.volume = vol;

                            /* Storing the level is not applying it.
                             * setvol() (apps/misc.c:871) is what calls
                             * sound_set_volume(), and it is what every
                             * other volume control in Rockbox goes
                             * through -- the wheel, the sound menu, the
                             * radio screen. Without it an accessory
                             * changed the setting and nothing else.
                             * It also lifts a mute, because it re-applies
                             * the stored level the mute left untouched. */
                            iap_set_mute(false);
                        }
                    }
                    else {
                        /* Muting was recorded and reported and never
                         * applied, so an accessory that muted the iPod
                         * was told it had worked and heard no change.
                         *
                         * There is no mute in the app-level sound API --
                         * audiohw_mute() is static inside each codec
                         * driver -- so this attenuates to sound_min
                         * instead, which is -60 dB on the CS42L55 and
                         * -90 dB on the WM8758. global_status.volume is
                         * deliberately left alone: it is the level to
                         * come back to, and persisting a minimum there
                         * would be a silent mute surviving a reboot.
                         *
                         * The cost of that choice is that starting a
                         * track re-applies global_status.volume
                         * (apps/playback.c:3074) and so lifts the mute.
                         * Holding it across a track change needs a mute
                         * concept that Rockbox does not have.
                         *
                         * sound_set_volume() writes global_status.volume
                         * itself (firmware/sound.c:320), so the level to
                         * come back to has to be put back after it. */
                        iap_set_mute(true);
                    }

                    cmd_ok(cmd);
                    break;
                }

                default:
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
            }

            break;
        }

        /* GetPlayStatus (0x0F)
         *
         * Request the current play status information
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x0F
         *
         * This command requires authentication
         *
         * Returns:
         * RetPlayStatus
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x10
         * 0x02: Play state
         * 0x03-0x06: Current track index
         * 0x07-0x0A: Current track length (ms)
         * 0x0B-0x0E: Current track position (ms)
         */
        case 0x0F:
        {
            int play_status;
            struct mp3entry* id3;

            CHECKAUTH;

            /* Both blocking reads before the buffer is opened; see the
             * note at 0x13. audio_current_track() takes id3_mutex. */
            id3 = audio_current_track();
            play_status = iap_play_state_reported();

            IAP_TX_INIT(0x03, 0x10);
            L3_TX_TRANSID();

            IAP_TX_PUT(play_status);

            if (play_status != 0x00) {
                IAP_TX_PUT_U32(iap_get_trackindex());
                IAP_TX_PUT_U32(id3->length);
                IAP_TX_PUT_U32(id3->elapsed);
            } else {
                /* Stopped, all values are 0x00 */
                IAP_TX_PUT_U32(0x00);
                IAP_TX_PUT_U32(0x00);
                IAP_TX_PUT_U32(0x00);
            }

            iap_send_tx();
            break;
        }

        /* RetPlayStatus (0x10)
         *
         * Sent from the iPod to the device
         */

        /* SetCurrentPlayingTrack (0x11)
         *
         * Set the current playing track
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x11
         * 0x02-0x05: Index of track to play
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x11:
        {
            uint32_t index;
            uint32_t trackcount;

            CHECKAUTH;
            L3_NEED(4);

            index = get_u32(&L3_PARAM(0));
            trackcount = playlist_amount();

            if (index >= trackcount)
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            audio_skip(index-iap_get_trackindex());
            cmd_ok(cmd);

            break;
        }

        /* GetIndexedPlayingTrackInfo (0x12)
         *
         * Request information about a given track
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x12
         * 0x02: Type of information to retrieve
         * 0x03-0x06: Track index
         * 0x07-0x08: Chapter index
         *
         * This command requires authentication.
         *
         * Returns:
         * RetIndexedPlayingTrackInfo
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x13
         * 0x02: Type of information returned
         * 0x03-0xNN: Information
         */
        case 0x12:
        {
            /* NOTE:
             *
             * Retrieving the track information from a track which is not
             * the currently playing track can take a seriously long time,
             * in the order of several seconds.
             *
             * This most certainly violates the IAP spec, but there's no way
             * around this for now.
             */
            uint32_t track_index;
            struct iap_chapter_info chapter;
            struct mp3entry id3;
            bool has_artwork = false;
            bool has_chapters = false;

            L3_NEED(7);
            CHECKAUTH;

            track_index = get_u32(&L3_PARAM(1));

            /* Read the metadata before opening the TX buffer, the way
             * iap-lingo4.c does at its equivalent site.
             *
             * iap_get_trackinfo() reaches get_metadata() and a file
             * read, and Rockbox's native scheduler is cooperative -- so
             * another thread runs precisely while this one is blocked.
             * The tuner driver transmits from the UI thread through
             * iap_send_pkt(), which begins by resetting iap_txnext to
             * the payload start, and it is reachable from
             * apps/radio/radio.c whenever the user opens the radio
             * screen. With the buffer opened first, its packet landed
             * on top of this half-built reply and the two went out as
             * one frame under a single length and checksum.
             *
             * Filling the buffer only after every blocking call has
             * returned closes the window without needing a lock: this
             * thread now owns the buffer for a stretch of code that
             * cannot yield. */
            /* The range check lives inside iap_get_trackinfo() now: it
             * is what converts the accessory's index into a playlist
             * one, so a check out here tested a different track than
             * the one that got read. */
            if (!iap_get_trackinfo(track_index, &id3)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            if (L3_PARAM(0) == 0x00 || L3_PARAM(0) == 0x08)
                has_artwork = iap_artwork_available(&id3);

            if (L3_PARAM(0) == 0x00)
                has_chapters = iap_chapter_at(&id3, 0, &chapter);

            if (L3_PARAM(0) == 0x01
                && !iap_chapter_at(&id3, get_u16(&L3_PARAM(5)), &chapter))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            IAP_TX_INIT(0x03, 0x13);
            L3_TX_TRANSID();
            IAP_TX_PUT(L3_PARAM(0));
            switch (L3_PARAM(0))
            {
                /* 0x00: Track caps/info
                 * Information length: 10 bytes
                 */
                case 0x00:
                {
                    uint32_t capabilities = has_artwork ? BIT_N(2) : 0;

                    if (has_chapters)
                        capabilities |= BIT_N(1);
                    IAP_TX_PUT_U32(capabilities);

                    /* Track length in ms */
                    IAP_TX_PUT_U32(id3.length);

                    IAP_TX_PUT_U16(has_chapters ? chapter.count : 0);

                    iap_send_tx();
                    break;
                }

                /* 0x01: Chapter time/name
                 * Information length: 4+variable
                 */
                case 0x01:
                {
                    IAP_TX_PUT_U32(chapter.offset_ms);
                    IAP_TX_PUT_STRLCPY(chapter.name);

                    iap_send_tx();
                    break;
                }

                /* 0x02, Artist name
                 * Information length: variable
                 */
                case 0x02:
                {
                    /* Artist name */
                    IAP_TX_PUT_STRLCPY(id3.artist);

                    iap_send_tx();
                    break;
                }

                /* 0x03, Album name
                 * Information length: variable
                 */
                case 0x03:
                {
                    /* Album name */
                    IAP_TX_PUT_STRLCPY(id3.album);

                    iap_send_tx();
                    break;
                }

                /* 0x04, Genre name
                 * Information length: variable
                 */
                case 0x04:
                {
                    /* Genre name */
                    IAP_TX_PUT_STRLCPY(id3.genre_string);

                    iap_send_tx();
                    break;
                }

                /* 0x05, Track title
                 * Information length: variable
                 */
                case 0x05:
                {
                    /* Track title */
                    IAP_TX_PUT_STRLCPY(id3.title);

                    iap_send_tx();
                    break;
                }

                /* 0x06, Composer name
                 * Information length: variable
                 */
                case 0x06:
                {
                    /* Track Composer */
                    IAP_TX_PUT_STRLCPY(id3.composer);

                    iap_send_tx();
                    break;
                }

                /* 0x07, Lyrics
                 * Information length: variable
                 */
                case 0x07:
                {
                    /* Packet information bits. All 0 (single packet) */
                    IAP_TX_PUT(0x00);

                    /* Packet index */
                    IAP_TX_PUT_U16(0x00);

                    /* Lyrics */
                    IAP_TX_PUT_STRING("");

                    iap_send_tx();
                    break;
                }

                /* 0x08, Artwork count
                 * Information length: variable
                 */
                case 0x08:
                {
                    if (has_artwork) {
                        IAP_TX_PUT_U16(IAP_ARTWORK_FORMAT_ID);
                        IAP_TX_PUT_U16(1);
                    }
                    iap_send_tx();
                    break;
                }

                default:
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
            }

            break;
        }

        /* RetIndexedPlayingTrackInfo (0x13)
         *
         * Sent from the iPod to the device
         */

        /* GetNumPlayingTracks (0x14)
         *
         * Request the number of tracks in the current playlist
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x14
         *
         * This command requires authentication.
         *
         * Returns:
         * RetNumPlayingTracks
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x15
         * 0x02-0xNN: Number of tracks
         */
        case 0x14:
        {
            CHECKAUTH;

            IAP_TX_INIT(0x03, 0x15);
            L3_TX_TRANSID();
            IAP_TX_PUT_U32(playlist_amount());

            iap_send_tx();
            break;
        }

        /* RetNumPlayingTracks (0x15)
         *
         * Sent from the iPod to the device
         */

        /* GetArtworkFormats (0x16)
         *
         * Request a list of supported artwork formats
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x16
         *
         * This command requires authentication.
         *
         * Returns:
         * RetArtworkFormats
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x17
         * 0x02-0xNN: list of 7 byte format descriptors
         */
        case 0x16:
        {
            CHECKAUTH;

            IAP_TX_INIT(0x03, 0x17);
            L3_TX_TRANSID();
            if (iap_artwork_supported()) {
                IAP_TX_PUT_U16(IAP_ARTWORK_FORMAT_ID);
                IAP_TX_PUT(IAP_ARTWORK_PIXEL_FORMAT);
                IAP_TX_PUT_U16(IAP_ARTWORK_WIDTH);
                IAP_TX_PUT_U16(IAP_ARTWORK_HEIGHT);
            }

            iap_send_tx();
            break;
        }

        /* RetArtworkFormats (0x17)
         *
         * Sent from the iPod to the device
         */

        /* GetTrackArtworkData (0x18)
         *
         * Request artwork for the given track
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x18
         * 0x02-0x05: Track index
         * 0x06-0x07: Format ID
         * 0x08-0x0B: Track offset in ms
         *
         * This command requires authentication.
         *
         * Returns:
         * RetTrackArtworkData
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x19
         * 0x02-0x03: Descriptor index
         * 0x04: Pixel format code
         * 0x05-0x06: Image width in pixels
         * 0x07-0x08: Image height in pixels
         * 0x09-0x0A: Inset rectangle, top left x
         * 0x0B-0x0C: Inset rectangle, top left y
         * 0x0D-0x0E: Inset rectangle, bottom right x
         * 0x0F-0x10: Inset rectangle, bottom right y
         * 0x11-0x14: Row size in bytes
         * 0x15-0xNN: Image data
         *
         * If the image data does not fit in a single packet, subsequent
         * packets omit bytes 0x04-0x14.
         */
        case 0x18:
        {
            struct mp3entry id3;
            enum iap_artwork_start_result result;
            uint32_t session_id;
            uint32_t track_index;

            CHECKAUTH;
            L3_NEED(10);

            session_id = iap_artwork_session_id();
            track_index = get_u32(&L3_PARAM(0));
            if (get_u16(&L3_PARAM(4)) != IAP_ARTWORK_FORMAT_ID
                || get_u32(&L3_PARAM(6)) != 0
                || !iap_get_trackinfo(track_index, &id3)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            result = iap_artwork_start_transfer(0x03, 0x18, 0x19,
                                                doff != 0,
                                                l3_tid_hi, l3_tid_lo,
                                                session_id, &id3);
            if (result == IAP_ARTWORK_START_FAILED)
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            else if (result == IAP_ARTWORK_START_BUSY)
                cmd_ack(cmd, IAP_ACK_NO_RESOURCE);
            break;
        }

        /* RetTrackArtworkFormat (0x19)
         *
         * Sent from the iPod to the device
         */

        /* GetPowerBatteryState (0x1A)
         *
         * Request the current power state
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1A
         *
         * This command requires authentication.
         *
         * Returns:
         * RetPowerBatteryState
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1B
         * 0x02: Power state
         * 0x03: Battery state
         */
        case 0x1A:
        {
            /* Table 2-7 (p.105) gives this lingo two answers:
             * "No (0x00-0x07, 0x1A-0x1E)" over serial and "Yes" over
             * USB. 0x1C and 0x1E are gated and 0x1A was not, which
             * matches neither column -- one range, two rules.
             *
             * Gated, for the USB reading. That is the stricter of the
             * two, it is the transport this firmware ships HID on, and
             * it costs nothing for an accessory that never asks for
             * authentication: iap-lingo0.c:717 puts one straight into
             * AUST_AUTH. Only an accessory part way through a handshake
             * it started itself is refused, and only until it
             * finishes. */
            CHECKAUTH;

            IAP_TX_INIT(0x03, 0x1B);
            L3_TX_TRANSID();

            iap_fill_power_state();
            iap_send_tx();
            break;
        }

        /* RetPowerBatteryState (0x1B)
         *
         * Sent from the iPod to the device
         */

        /* GetSoundCheckState (0x1C)
         *
         * Request the current sound check state
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1C
         *
         * This command requires authentication.
         *
         * Returns:
         * RetSoundCheckState
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1D
         * 0x02: Sound check state
         */
        case 0x1C:
        {
            CHECKAUTH;

            IAP_TX_INIT(0x03, 0x1D);
            L3_TX_TRANSID();
            IAP_TX_PUT(0x00);       /* Always off */

            iap_send_tx();
            break;
        }

        /* RetSoundCheckState (0x1D)
         *
         * Sent from the iPod to the device
         */

        /* SetSoundCheckState (0x1E)
         *
         * Set the sound check state
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1E
         * 0x02: Sound check state
         * 0x03: Restore on exit
         *
         * This command requires authentication.
         *
         * Returns on success
         * IAP_ACK_OK
         *
         * Returns on failure
         * IAP_ACK_CMD_FAILED
         */
        case 0x1E:
        {
            CHECKAUTH;
            L3_NEED(2);

            /* Sound check is not supported right now
             * TODO: Fix
             */

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* GetTrackArtworkTimes (0x1F)
         *
         * Request a list of timestamps at which artwork exists in a track
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x1F
         * 0x02-0x05: Track index
         * 0x06-0x07: Format ID
         * 0x08-0x09: Artwork Index
         * 0x0A-0x0B: Artwork count
         *
         * This command requires authentication.
         *
         * Returns:
         * RetTrackArtworkTimes
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: Display Remote Lingo, always 0x03
         * 0x01: Command, always 0x20
         * 0x02-0x05: Offset in ms
         *
         * Bytes 0x02-0x05 can be repeated multiple times
         */
        case 0x1F:
        {
            uint32_t index;
            uint16_t artwork_index;
            uint16_t artwork_count;
            struct mp3entry id3;
            bool has_artwork;

            CHECKAUTH;
            L3_NEED(10);

            index = get_u32(&L3_PARAM(0));
            artwork_index = get_u16(&L3_PARAM(6));
            artwork_count = get_u16(&L3_PARAM(8));

            if (get_u16(&L3_PARAM(4)) != IAP_ARTWORK_FORMAT_ID
                || !iap_get_trackinfo(index, &id3)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            has_artwork = iap_artwork_available(&id3);

            IAP_TX_INIT(0x03, 0x20);
            L3_TX_TRANSID();
            if (artwork_index == 0 && artwork_count != 0
                && has_artwork)
                IAP_TX_PUT_U32(0);

            iap_send_tx();
            break;
        }

        /* The default response is IAP_ACK_BAD_PARAM */
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode03 Command");
#endif
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
