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
 ******************************************************************************/

#include "iap-core.h"
#include "iap-lingo.h"
#include "iap-artwork.h"
#include "dir.h"
#include "settings.h"
#include "filetree.h"
#include "wps.h"
#include "playback.h"
#include "string-extra.h"

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
 * Every command here is lingo byte, two command-ID bytes, then a
 * transaction ID when one is in force -- so the first parameter is at
 * 3 + doff and CHECKLEN wants 3 + n + doff for n parameter bytes.
 * Written out, those two are in different units: the reads count from
 * the start of the packet and the check counts the whole packet, so
 * "does this check cover this read" needs arithmetic every time.
 * Getting it wrong is most of what the review rounds found -- offsets
 * that forgot doff, checks two bytes short of the reads under them,
 * and a base of 2 copied in from a one-byte-command lingo.
 *
 * L4_NEED(n) and L4_PARAM(n) both count parameters, so a handler that
 * says NEED(4) and reads PARAM(0..3) is visibly right. */
#define L4_NEED(n)  CHECKLEN(3 + (n) + doff)
#define L4_HAVE(n)  (len >= (unsigned int)(3 + (n) + doff))
#define L4_PARAM(n) (buf[3 + doff + (n)])

/* Check for authenticated state, and return an ACK Not
 * Authenticated on failure.
 */
#define CHECKAUTH do { \
        if (!DEVICE_AUTHENTICATED) { \
            cmd_ack(cmd, IAP_ACK_NO_AUTHEN); \
            return; \
        }} while(0)

/* Used to remember the last Type and Record requested */
static char cur_dbrecord[5] = {0};

/* Used to remember the total number of filtered database records */
/* The database selection an accessory built up. Session state: it
 * belongs to the accessory that made the selection, and the next one
 * starts from nothing. iap_reset_lingo4() below is what clears it. */
static unsigned long dbrecordcount = 0;

/* Used to remember the LAST playlist selected */
static unsigned long last_selected_playlist = 0;

void iap_reset_lingo4(void)
{
    /* Equivalent mutants, both of them, and recorded rather than
     * removed. Every arm of GetNumberCategorizedDBRecords assigns
     * dbrecordcount before the put_u32() that transmits it, and
     * last_selected_playlist is written on the line before each of the
     * two reads. Neither can carry a value out of the session it
     * belongs to. They are cleared anyway because a detach is where a
     * reader looks for the list of what a session owns, and finding
     * two of its variables missing from it invites the question a
     * second time. */
    dbrecordcount = 0;
    last_selected_playlist = 0;
}


/* Transaction ID of the packet being handled. Lingo 4's command ID is
 * two bytes (MFi Table 2-10), so under IDPS the transaction ID sits at
 * buf[3..4] and the first parameter is L4_PARAM(0) -- the macro folds
 * the ID in, which is the whole point of it. The old text said the
 * payload started at L4_PARAM(2), counting the ID's two bytes twice;
 * that was wrong before the units changed and is worse now, because the
 * notation's premise is that parameter 0 is the first parameter.
 * Parsed at the top of iap_handlepkt_mode4(). */
static uint8_t l4_tid_hi, l4_tid_lo;

/* Insert the transaction ID into a reply built with the IAP_TX_ macros. */
#define L4_TX_TRANSID() do { \
        if (DEVICE_TRANSID_ACTIVE) { \
            IAP_TX_PUT(l4_tid_hi); \
            IAP_TX_PUT(l4_tid_lo); \
        }} while(0)

/* Reply built in a prebuilt array rather than with the macros. */
#define L4_SEND_REPLY(d, n) iap_send_reply((d), (n), l4_tid_hi, l4_tid_lo)

static void cmd_ack(const unsigned int cmd, const unsigned char status)
{
    IAP_TX_INIT4(0x04, 0x0001);
    L4_TX_TRANSID();
    IAP_TX_PUT(status);
    IAP_TX_PUT_U16(cmd);
    iap_send_tx();
}

#define cmd_ok(cmd) cmd_ack((cmd), IAP_ACK_OK)

static void get_playlist_name(unsigned char *dest,
                              unsigned long item_offset,
                              size_t max_length)
{
    if (item_offset == 0) return;
    DIR* dp;
    struct dirent* playlist_file = NULL;

    dp = opendir(global_settings.playlist_catalog_dir);
    if (dp == NULL) return;

    char *extension;
    unsigned long nbr = 0;
    while ((nbr < item_offset) && ((playlist_file = readdir(dp)) != NULL))
    {
        /*Increment only if there is a playlist extension*/
        if ((extension=strrchr(playlist_file->d_name, '.')) != NULL){
            if ((strcmp(extension, ".m3u") == 0 ||
                 strcmp(extension, ".m3u8") == 0))
                nbr++;
        }
    }
    if (playlist_file != NULL) {
        strmemccpy(dest, playlist_file->d_name, max_length);
    }
    closedir(dp);
}

/* Load the catalogue playlist at this index and start it.
 *
 * This used to call ft_play_playlist(), which is the file browser's
 * entry point rather than a library call, and it is not safe from this
 * thread:
 *
 *   - warn_on_pl_erase() (apps/misc.c:189) runs gui_syncyesno_run(),
 *     and that is gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, ...)
 *     (apps/gui/yesno.c:367) -- a modal dialog with no timeout. Its
 *     gate includes global_settings.warnon_erase_dynplaylist, which
 *     defaults to true (apps/settings_list.c:1971). So a user who has
 *     queued anything, docks, and picks a playlist on the head unit
 *     put the iAP thread inside a yes/no prompt on the iPod's own
 *     screen: iap_handlepkt() never returned, so no iPodAck, no
 *     notifications, no iap_timeoutbtn countdown -- a synthetic remote
 *     button stayed asserted and was fed straight into the dialog --
 *     and every queued packet stalled behind it. The head unit sees a
 *     dead iPod.
 *   - bookmark_autoload() is a second event loop on the same path.
 *   - splash() writes the LCD unconditionally.
 *
 * The two steps this actually wants are playlist_create() and
 * playlist_start(), which is what ft_play_playlist() does once the
 * interactive part is out of the way. Party mode still refuses --
 * MFi 5.1.20 (p.415) tells accessories to "pay close attention to the
 * iPodAck returned by the SelectDBRecord command", and this now has an
 * answer to give it.
 */
static bool seek_to_playlist(unsigned long index)
{
    char selected_playlist[sizeof(global_settings.playlist_catalog_dir)
                           + 1 + MAX_PATH] = {0};
    char *file;

    if (global_settings.party_mode && audio_status())
        return false;

    strcpy(selected_playlist, global_settings.playlist_catalog_dir);
    int len = strlen(selected_playlist);
    selected_playlist[len] = '/';
    get_playlist_name(selected_playlist + len + 1, index, MAX_PATH);

    file = strrchr(selected_playlist, '/') + 1;
    if (*file == '\0')
        return false;

    if (playlist_create(global_settings.playlist_catalog_dir, file) == -1)
        return false;

    /* And an empty queue is a failed selection too.
     * playlist_create() (apps/playlist.c:2074) returns -1 only when the
     * buffer allocation fails; a catalogue entry that has been deleted
     * since the accessory enumerated it takes the ordinary path,
     * add_indices_to_playlist()'s failure is discarded at :2095, and
     * the call returns 0 with nothing in the queue. So the -1 test on
     * its own answered Success and started silence.
     *
     * An .m3u that really is empty lands here too, and belongs here:
     * MFi 5.1.20 (p.415) tells accessories to "pay close attention to
     * the iPodAck returned by the SelectDBRecord command", and a
     * selection that plays nothing is not one that succeeded --
     * whichever of the two reasons it was. */
    if (playlist_amount() <= 0)
        return false;

    if (global_settings.playlist_shuffle)
        playlist_shuffle(current_tick, -1);

    playlist_start(0, 0, 0);
    return true;
}

static unsigned long nbr_total_playlists(void)
{
    DIR* dp;
    unsigned long nbr_total_playlists = 0;
    struct dirent* playlist_file = NULL;
    char *extension;
    dp = opendir(global_settings.playlist_catalog_dir);
    if (dp == NULL) return 0;
    while ((playlist_file = readdir(dp)) != NULL)
    {
        /*Increment only if there is a playlist extension*/
        if ((extension=strrchr(playlist_file->d_name, '.')) != NULL)
        {
            if ((strcmp(extension, ".m3u") == 0 ||
                 strcmp(extension, ".m3u8") == 0))
            {
                nbr_total_playlists++;
            }
        }
    }
    closedir(dp);
    return nbr_total_playlists;
}

void iap_handlepkt_mode4(const unsigned int len, const unsigned char *buf)
{
    /* Zero until the length check below says buf[2] exists. CHECKLEN
     * names cmd in the ack it sends on failure, so it has to hold
     * something defined -- and 0x0000 is the honest answer for a packet
     * too short to say what it was. The accessory never sends 0x0000,
     * so it discards the ack, which is what MFi 2.6.1.1 (p.111)
     * prescribes for a response matching no command it sent. */
    unsigned int cmd = 0;

    /* Extra offset for the IDPS transaction ID (0 or 2). Every payload
     * index and length check below adds it. Without it, after IDPS
     * every parameter was read two bytes early -- PlayControl matched
     * no case and silently did nothing while still acking OK, and
     * SetPlayStatusChangeNotification could never be enabled. */
    unsigned int doff = 0;

    l4_tid_hi = 0;
    l4_tid_lo = 0;
    /* Lingo 0x04 commands are at least 3 bytes in length.
     *
     * Before the command ID is assembled, not after. The framer's
     * minimum payload of 0x02 covers the one-byte-command lingoes but
     * not this two-byte one, so "FF 55 02 04 00 FA" made buf[2] stale
     * buffer content -- and the iPodAck built from it named a command
     * the accessory never sent, which can collide with one it has
     * outstanding. */
    CHECKLEN(3);

    cmd = (buf[1] << 8) | buf[2];

    if (DEVICE_TRANSID_ACTIVE) {
        CHECKLEN(5);    /* lingo + cmd(2) + transID(2) */
        /* Raw indices, as every other lingo uses here. L4_PARAM(n) is
         * "parameter n, past the transaction ID", and these two bytes
         * are the transaction ID -- the macro gave the right address
         * only because doff is still 0 two lines above where it is set,
         * so reordering these three lines would have silently moved
         * both reads two bytes on. */
        l4_tid_hi = buf[3];
        l4_tid_lo = buf[4];
        doff = 2;
    }

    /* Lingo 0x04 must have been negotiated.
     *
     * Equivalent mutant: the mode check immediately below refuses with
     * the same Bad Parameter naming the same command, and no reachable
     * state has interface_state at IST_EXTENDED without this lingo --
     * the two paths into the mode both require it, and re-identifying
     * without it goes through iap_reset_device(), which drops both
     * together (test_modes_extended_mode_does_not_outlive_the_lingo).
     * So removing this changes nothing observable. Kept because it is
     * the check that states the rule, and recorded so the sweep's
     * survivor list stays fully accounted for. */
    if (!DEVICE_LINGO_SUPPORTED(0x04)) {
#ifdef LOGF_ENABLE
        logf("iap: Mode04 Not Negotiated");
#endif
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }

    /* All these commands require extended interface mode */
    if (interface_state != IST_EXTENDED) {
#ifdef LOGF_ENABLE
        logf("iap: Not in Mode04 Extended Mode");
#endif
        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
        return;
    }
    switch (cmd)
    {
        case 0x0001: /* CmdAck. See above cmd_ack() */
            /*
             * The following is the description for the Apple Firmware
             * The iPod sends this telegram to acknowledge the receipt of a
             * command and return the command status. The command ID field
             * indicates the device command for which the response is being
             * sent. The command status indicates the results of the command
             * (success or failure).
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x06  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x01  Command ID (bits 7:0)
             *  6   0xNN  Command result status. Possible values are:
             *            0x00 = Success (OK)
             *            0x01 = ERROR: Unknown database category
             *            0x02 = ERROR: Command failed
             *            0x03 = ERROR: Out of resources
             *            0x04 = ERROR: Bad parameter
             *            0x05 = ERROR: Unknown ID
             *            0x06 = Reserved
             *            0x07 = Accessory not authenticated
             *            0x08 - 0xFF = Reserved
             *  7  0xNN   The ID of the command being acknowledged (bits 15:8).
             *  8  0xNN   The ID of the command being acknowledged (bits 7:0).
             *  9  0xNN   Telegram payload checksum byte
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0002: /* GetCurrentPlayingTrackChapterInfo */
            /* The following is the description for the Apple Firmware
             * Requests the chapter information of the currently playing track.
             * In response, the iPod sends a
             * Command 0x0003: ReturnCurrentPlayingTrackChapterInfo
             * telegram to the device.
             * Note: The returned track index is valid only when there is a
             * currently playing or paused track.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x02  Command ID (bits 7:0)
             *  6   0xF7  Telegram payload checksum byte
             *
             */
        {
            struct iap_chapter_info chapter;
            struct mp3entry *id3 = audio_current_track();
            bool has_chapters = (audio_status() & AUDIO_STATUS_PLAY)
                             && iap_current_chapter(id3, &chapter);

            IAP_TX_INIT4(0x04, 0x0003);
            L4_TX_TRANSID();
            IAP_TX_PUT_U32(has_chapters ? chapter.index : UINT32_MAX);
            IAP_TX_PUT_U32(has_chapters ? chapter.count : 0);
            iap_send_tx();
            break;
        }
        case 0x0003: /* ReturnCurrentPlayingTrackChapterInfo. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the chapter information of the currently playing track.
             * The iPod sends this telegramin response to the
             * Command 0x0002: GetCurrentPlayingTrackChapterInfo
             * telegram from the device. The track chapter information includes
             * the currently playingtrack's chapter index,as well as the
             * total number of chapters in the track. The track chapter and the
             * total number of chapters are 32-bit signed integers. The chapter
             * index of the firstchapter is always 0x00000000. If the track does
             * not have chapter information, a chapter index of -1(0xFFFFFFFF)
             * and a chapter count of 0 (0x00000000) are returned.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0B  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x03  Command ID (bits 7:0)
             *  6   0xNN  Current chapter index (bits 31:24)
             *  7   0xNN  Current chapter index (bits 23:16)
             *  8   0xNN  Current chapter index (bits 15:8)
             *  9   0xNN  Current chapter index (bits 7:0)
             * 10   0xNN  Chapter count (bits 31:24)
             * 11   0xNN  Chapter count (bits 23:16)
             * 12   0xNN  Chapter count (bits 15:8)
             * 13   0xNN  Chapter count (bits 7:0)
             * 14   0xNN  Telegram payload checksum byte
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0004: /* SetCurrentPlayingTrackChapter */
            /* The following is the description for the Apple Firmware
             *
             * Sets the currently playing track chapter.You can send the Command
             * 0x0002: GetCurrentPlayingTrackChapterInfo telegram to get the
             * chapter count and the index of the currently playing chapter in
             * the current track. In response to the command
             * SetCurrentPlayingTrackChapter, the iPod sends an ACK telegram
             * with the command status.
             *
             * Note: This command should be used only when the iPod is in a
             * playing or paused state. The command fails if the iPod is stopped
             * or if the track does not contain chapter information.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x04  Command ID (bits 7:0)
             *  6   0xNN  Chapter index (bits 31:24)
             *  7   0xNN  Chapter index (bits 23:16)
             *  8   0xNN  Chapter index (bits 15:8)
             *  9   0xNN  Chapter index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            uint32_t index;

            L4_NEED(4);
            index = get_u32(&L4_PARAM(0));
            if (!(audio_status() & AUDIO_STATUS_PLAY))
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            else if (!iap_set_chapter(index))
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            else
                cmd_ok(cmd);
            break;
        }
        case 0x0005: /* GetCurrentPlayingTrackChapterPlayStatus */
            /* The following is the description for the Apple Firmware
             *
             * Requests the chapter playtime status of the currently playing
             * track. The status includes the chapter length and the time
             * elapsed within that chapter. In response to a valid telegram, the
             * iPod sends a Command 0x0006:
             * ReturnCurrentPlayingTrackChapterPlayStatus telegram to the
             * device.
             *
             * Note: If the telegram length or chapter index is invalid for
             * instance, if the track does not contain chapter information the
             * iPod responds with an ACK telegram including the specific error
             * status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x05  Command ID (bits 7:0)
             *  6   0xNN  Currently playingchapter index (bits31:24)
             *  7   0xNN  Currently playingchapter index (bits23:16)
             *  8   0xNN  Currently playing chapter index (bits 15:8)
             *  9   0xNN  Currently playing chapter index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            struct iap_chapter_info chapter;
            struct mp3entry *id3;
            uint32_t index;

            L4_NEED(4);
            index = get_u32(&L4_PARAM(0));
            id3 = audio_current_track();
            if (!(audio_status() & AUDIO_STATUS_PLAY)
                || !iap_current_chapter(id3, &chapter)
                || index != chapter.index)
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            IAP_TX_INIT4(0x04, 0x0006);
            L4_TX_TRANSID();
            IAP_TX_PUT_U32(chapter.length_ms);
            IAP_TX_PUT_U32(chapter.elapsed_ms);
            iap_send_tx();
            break;
        }
        case 0x0006: /* ReturnCurrentPlayingTrackChapterPlayStatus. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the play status of the currently playing track chapter.
             * The iPod sends this telegram in response to the Command 0x0005:
             * GetCurrentPlayingTrackChapterPlayStatus telegram from the device.
             * The returned information includes the chapter length and elapsed
             * time, in milliseconds. If there is no currently playing chapter,
             * the chapter length and elapsed time are zero.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0B  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x06  Command ID (bits 7:0)
             *  6   0xNN  Chapter length in milliseconds (bits 31:24)
             *  7   0xNN  Chapter length in milliseconds (bits 23:16)
             *  8   0xNN  Chapter length in milliseconds (bits 15:8)
             *  9   0xNN  Chapter length in milliseconds (bits 7:0)
             * 10   0xNN  Elapsed time in chapter, in milliseconds (bits 31:24)
             * 11   0xNN  Elapsed time in chapter, in milliseconds (bits 23:16)
             * 12   0xNN  Elapsed time in chapter, in milliseconds (bits 15:8)
             * 13   0xNN  Elapsed time in chapter, in milliseconds (bits 7:0)
             * 14   0xNN  Telegram payload checksum byte
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0007:  /* GetCurrentPlayingTrackChapterName */
            /* The following is the description for the Apple Firmware
             *
             * Requests a chapter name in the currently playing track. In
             * response to a valid telegram, the iPod sends a Command 0x0008:
             * ReturnCurrentPlayingTrackChapterName telegram to the device.
             *
             * Note: If the received telegram length or track index is invalid
             * for instance, if the track does not have chapter information or
             * is not a part of the Audiobook category, the iPod responds with
             * an ACK telegram including the specific error status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x07  Command ID (bits 7:0)
             *  6   0xNN  Chapter index (bits 31:24)
             *  7   0xNN  Chapter index (bits 23:16)
             *  8   0xNN  Chapter index (bits 15:8)
             *  9   0xNN  Chapter index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            struct iap_chapter_info chapter;
            struct mp3entry *id3;
            uint32_t index;

            L4_NEED(4);
            index = get_u32(&L4_PARAM(0));
            id3 = audio_current_track();
            if (!(audio_status() & AUDIO_STATUS_PLAY)
                || !iap_chapter_at(id3, index, &chapter))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            IAP_TX_INIT4(0x04, 0x0008);
            L4_TX_TRANSID();
            IAP_TX_PUT_STRLCPY(chapter.name);
            iap_send_tx();
            break;
        }
        case 0x0008: /* ReturnCurrentPlayingTrackChapterName. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns a chapter name in the currently playing track. The iPod
             * sends this telegram in response to a valid Command 0x0007:
             * GetCurrentPlayingTrackChapterName telegram from the
             * device. The chapter name is encoded as a null-terminated UTF-8
             * character array.
             *
             * Note: The chapter name string is not limited to 252 characters;
             * it may be sent in small or large telegram format depending on
             * the string length. The small telegram format is shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x08  Command ID (bits 7:0)
             *  6-N 0xNN  Chapter name as UTF-8 character array
             *(last byte) 0xNN Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0009: /* GetAudioBookSpeed */
            /* The following is the description for the Apple Firmware
             *
             * Requests the current iPod audiobook speed state. The iPod
             * responds with the “Command 0x000A: ReturnAudiobookSpeed”
             * telegram indicating the current audiobook speed.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x09  Command ID (bits 7:0)
             *  6   0xF0  Telegram payload checksum byte
             *
             * ReturnAudioBookSpeed
             * 0x00 = Normal, 0xFF = Slow, 0x01 = Fast
             * We always respond with Normal speed
             */
        {
            unsigned char data[] = {0x04, 0x00, 0x0A, 0x00};
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x000A: /* ReturnAudioBookSpeed. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the current audiobook speed setting. The iPod sends
             * this telegram in response to the Command 0x0009:
             * GetAudiobookSpeed command from the device.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0A  Command ID (bits 7:0)
             *  6   0xNN  Audiobook speed status code.
             *            0xFF Slow (-1)
             *            0x00 Normal
             *            0x01 Fast (+1)
             *  7   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x000B: /* SetAudioBookSpeed */
            /* The following is the description for the Apple Firmware
             * Sets the speed of audiobook playback. The iPod audiobook speed
             * states are listed above. This telegram has two modes: one to
             * set the speed of the currently playing audiobook and a second
             * to set the audiobook speed for all audiobooks. Byte number 7
             * is an optional byte; devices should not send this byte if they
             * want to set the speed only of the currently playing audiobook.
             * If devices want to set the global audiobook speed setting then
             * they must use byte number 7. This is the Restore on Exit byte;
             * a nonzero value restores the original audiobook speed setting
             * when the accessory is detached. If this byte is zero, the
             * audiobook speed setting set by the accessory is saved and
             * persists after the accessory is detached from the iPod. See below
             * for the telegram format used when including the Restore on Exit
             * byte.
             * In response to this command, the iPod sends an ACK telegram with
             * the command status.
             *
             * Note: Accessory developers are encouraged to always use the
             * Restore on Exit byte with a nonzero value, to restore any of the
             * user's iPod settings that were modified by the accessory.
             *
             * Byte Value Meaning (Cuurent audiobook only
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0B  Command ID (bits 7:0)
             *  6   0xNN  New audiobook speed code.
             *  7   0xNN Telegram payload checksum byte
             *
             *
             * SetAudiobookSpeed telegram to set the global audiobook speed
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x05  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0B  Command ID (bits 7:0)
             *  6   0xNN  Global audiobook speed code.
             *  7   0xNN  Restore on Exit byte.
             *            If 1, the original setting is restored on detach;
             *            if 0, the newsetting persists after accessory detach.
             *  8   0xNN  Telegram payload checksum byte
             *
             *
             */
        {
            L4_NEED(1);
            if (L4_HAVE(2) && L4_PARAM(1) > 0x01)
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            else if (L4_PARAM(0) == 0x00)
                cmd_ok(cmd);
            else if (L4_PARAM(0) == 0xFF || L4_PARAM(0) == 0x01)
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            else
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x000C: /* GetIndexedPlayingTrackInfo */
            /* The following is the description for the Apple Firmware
             *
             * Gets track information for the track at the specified index.
             * The track info type field specifies the type of information to be
             * returned, such as song lyrics, podcast name, episode date, and
             * episode description. In response, the iPod sends the Command
             * 0x000D: ReturnIndexedPlayingTrackInfo command with the requested
             * track information. If the information type is invalid or does not
             * apply to the selected track, the iPod returns an ACK with an
             * error status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0A  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0C  Command ID (bits 7:0)
             *  6   0xNN  Track info type. See Below
             *  7   0xNN  Track index (bits 31:24)
             *  8   0xNN  Track index (bits 23:16)
             *  9   0xNN  Track index (bits 15:8)
             * 10   0xNN  Track index (bits 7:0)
             * 11   0xNN  Chapter index (bits 15:8)
             * 12   0xNN  Chapter index (bits 7:0)
             * (last byte)0xNN Telegram payload checksum byte
             *
             * Track Info Types: Return Data
             * 0x00 Track Capabilities. 10byte data
             * 0x01 Podcast Name        UTF-8 String
             * 0x02 Track Release Date  7Byte Data All 0 if invalid
             * 0x03 Track Description   UTF-8 String
             * 0x04 Track Song Lyrics   UTF-8 String
             * 0x05 Track Genre         UTF-8 String
             * 0x06 Track Composer      UTF-8 String
             * 0x07 Track Artwork Count 2byte formatID and 2byte count of images
             * 0x08-0xff Reserved
             *
             * Track capabilities
             * 0x00-0x03
             *      Bit0 is Audiobook
             *      Bit1 has chapters
             *      Bit2 has album art
             *      Bit3 has lyrics
             *      Bit4 is podcast episode
             *      Bit5 has Release Date
             *      Bit6 has Decription
             *      Bit7 Contains Video
             *      Bit8 Play as Video
             *      Bit9+ Reserved
             * 0x04-0x07 Total Track Length in ms
             * 0x08-0x09 Chapter Count
             *
             * Track Release Date Encoding
             * 0x00 Seconds 0-59
             * 0x01 Minutes 0-59
             * 0x02 Hours 0-23
             * 0x03 Day Of Month 1-31
             * 0x04 Month 1-12
             * 0x05 Year Bits 15:8 2006 = 2006AD
             * 0x06 Year Bits 7:0
             * 0x07 Weekday 0-6 where 0=Sunday 6=Saturday
             *
             *
             */
        {
            /* Table 5-14 (p.408): info type (1), track index (4),
             * chapter index (2), so ten bytes with the lingo and the
             * two-byte command -- plus the transaction ID, which this
             * check was the only one in the file to omit. Under IDPS a
             * packet two bytes short of carrying its index was accepted
             * and the index read from past its end. */
            if (len < (unsigned int)(10 + doff))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            /* MFi 5.1.13 (p.408): "Gets track information for the track
             * at the specified index." The index sat in bytes 4..7 and
             * was never read, so every query was answered about
             * whatever happened to be playing and a head unit building
             * a track list saw the same length on every row. */
            uint32_t want = get_u32(&L4_PARAM(1));
            struct mp3entry track_id3;
            struct mp3entry *id3 = &track_id3;
            bool has_artwork = false;
            struct iap_chapter_info chapter;
            bool has_chapters = false;

            /* Load the track that was asked about. The first attempt at
             * this validated the index and then called
             * audio_current_track() anyway, so every in-range query
             * still answered about whatever was playing -- the defect
             * the validation was added to cure. iap_get_trackinfo()
             * is what lingo 3 uses for the same job.
             *
             * It converts the accessory's index into a playlist one,
             * so it is also the only thing that can range-check it:
             * "If the information type is invalid or does not apply to
             * the selected track, the Apple device returns an iPodAck
             * with an error status." The check used to sit here, done
             * against the unconverted index. */
            if (!iap_get_trackinfo(want, id3))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            if (L4_PARAM(0) == 0x00 || L4_PARAM(0) == 0x07)
                has_artwork = iap_artwork_available(id3);
            if (L4_PARAM(0) == 0x00)
                has_chapters = iap_chapter_at(id3, 0, &chapter);

            switch(L4_PARAM(0))
            {
                case 0x01: /* Podcast Not Supported */
                case 0x04: /* Lyrics Not Supported */
                case 0x03: /* Description */
                case 0x05: /* Genre */
                case 0x06: /* Composer */
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
                /* Table 5-18 (p.410) reserves 0x08-0xFF, and only
                 * three of the rest are implemented below. Anything
                 * else used to fall out of the inner switch and send a
                 * ReturnIndexedPlayingTrackInfo with a zero-length
                 * payload -- a well-formed packet with a valid checksum
                 * that the accessory cannot tell from a real answer, so
                 * it parses past the end or waits for the rest. 5.1.13
                 * (p.408) asks for an error ack instead. */
                default:
                {
                    if (L4_PARAM(0) != 0x00 && L4_PARAM(0) != 0x02
                        && L4_PARAM(0) != 0x07)
                    {
                        cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                        break;
                    }

                    IAP_TX_INIT4(0x04, 0x000D);
            L4_TX_TRANSID();
                    IAP_TX_PUT(L4_PARAM(0));
                    switch(L4_PARAM(0))
                    {
                        case 0x00:
                        {
                            uint32_t capabilities = has_artwork ? BIT_N(2) : 0;

                            if (has_chapters)
                                capabilities |= BIT_N(1);
                            IAP_TX_PUT_U32(capabilities);
                            IAP_TX_PUT_U32(id3->length); /* Track Length */
                            IAP_TX_PUT_U16(has_chapters ? chapter.count : 0);
                            break;
                        }
                        case 0x02:
                        {
                            /* Track Release Date, 8 bytes.
                             *
                             * MFi 5.1.14 (p.409): "If the track has no
                             * release date, then the returned release
                             * date has all bytes zeros." Rockbox keeps
                             * no release date, and the track
                             * capabilities above report bit 5 clear --
                             * "track has release date" -- so this used
                             * to deny having one and then hand out
                             * Sunday 1 Feb 2011, which a head unit
                             * sorting podcasts by date believed. */
                            IAP_TX_PUT(0);               /* Seconds */
                            IAP_TX_PUT(0);               /* Minutes */
                            IAP_TX_PUT(0);               /* Hours */
                            IAP_TX_PUT(0);               /* Day of month */
                            IAP_TX_PUT(0);               /* Month */
                            IAP_TX_PUT_U16(0);           /* Year */
                            IAP_TX_PUT(0);               /* Weekday */
                            break;
                        }
                        case 0x07:
                        {
                            if (has_artwork) {
                                IAP_TX_PUT_U16(IAP_ARTWORK_FORMAT_ID);
                                IAP_TX_PUT_U16(1);
                            }
                            break;
                        }
                    }
                    iap_send_tx();
                    break;
                }
            }
            break;
        }
        case 0x000D: /* ReturnIndexedPlayingTrackInfo. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the requested track information type and data. The iPod
             * sends this command in response to the Command 0x000C:
             * GetIndexedPlayingTrackInfo command.
             * Data returned as strings are encoded as null-terminated UTF-8
             * character arrays.
             * If the track information string does not exist, a null UTF-8
             * string is returned. If the track has no release date, then the
             * returned release date has all bytes zeros. Track song lyrics and
             * the track description are sent in a large or small telegram
             * format with an incrementing packet index field, spanning
             * multiple packets if needed.
             *
             * Below is the packet format for the
             * ReturnIndexedPlayingTrackInfo telegram sent in response to a
             * request for information types 0x00 to 0x02.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0D  Command ID (bits 7:0)
             *  6   0xNN  Track info type. See
             *  7-N 0xNN  Track information. The data format is specific
             *            to the track info type.
             *  NN  0xNN  Telegram payload checksum byte
             *
             * Below is the large packet format for the
             * ReturnIndexedPlayingTrackInfo telegram sent in response to a
             * request for information types 0x03 to 0x04. The small telegram
             * format is not shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte
             *  1   0x55  Start of telegram
             *  2   0x00  Telegram payload marker (large format)
             *  3   0xNN  Large telegram payload length (bits 15:8)
             *  4   0xNN  Large telegram payload length (bits 7:0)
             *  5   0x04  Lingo ID (Extended Interface lingo)
             *  6   0x00  Command ID (bits 15:8)
             *  7   0x0D  Command ID (bits 7:0)
             *  8   0xNN  Track info type.
             *  9   0xNN  Packet information bits. If set,
             *            these bits have the following meanings:
             *            Bit 0: Indicates that there are multiple packets.
             *            Bit 1: This is the last packet. Applicable only if
             *                   bit 0 is set.
             *            Bit 31:2 Reserved
             * 10   0xNN  Packet Index (bits 15:8)
             * 11   0xNN  Packet Index (bits 7:0)
             * 12-N 0xNN  Track information as a UTF-8 string.
             * NN   0xNN  Telegram payload checksum byte
             *
             * Track info types and return data
             * Info Type Code                          Data Format
             * 0x00 Track Capabilities and Information 10-byte data.
             * 0x01 PodcastName                        UTF-8 string
             * 0x02 Track Release Date                 7-byte data.
             * 0x03 Track Description                  UTF-8 string
             * 0x04 Track Song Lyrics                  UTF-8 string
             * 0x05 TrackGenre                         UTF-8 string
             * 0x06 Track Composer                     UTF-8 string
             * 0x07 Track Artwork Count                Artwork count data. The
             *      artwork count is a sequence of 4-byte records; each record
             *      consists of a 2-byte format ID value followed by a 2-byte
             *      count of images in that format for this track. For more
             *      information about formatID and chapter index values, see
             *      commands 0x000E-0x0011 and 0x002A-0x002B.
             * 0x08-0xFF Reserved
             *
             * Track Capabilities and Information encoding
             * Byte      Code
             * 0x00-0x03 Track Capability bits. If set, these bits have the
             *           following meanings:
             *           Bit 0: Track is audiobook
             *           Bit 1: Track has chapters
             *           Bit 2: Track has album artwork
             *           Bit 3: Track has song lyrics
             *           Bit 4: Track is a podcast episode
             *           Bit 5: Track has release date
             *           Bit 6: Track has description
             *           Bit 7: Track contains video (a video podcast, music
             *                  video, movie, or TV show)
             *           Bit 8: Track is currently queued to play as a video
             *           Bit 31:9: Reserved
             * 0x04      Total track length, in milliseconds (bits 31:24)
             * 0x05      Total track length, in milliseconds (bits 23:16)
             * 0x06      Total track length, in milliseconds (bits 15:8)
             * 0x07      Total track length, in milliseconds (bits 7:0)
             * 0x08      Chapter count (bits 15:8)
             * 0x09      Chapter count (bits 7:0)
             *
             * Track Release Date encoding
             * Byte Code
             * 0x00 Seconds (0-59)
             * 0x01 Minutes (0-59)
             * 0x02 Hours (0-23)
             * 0x03 Day of themonth(1-31)
             * 0x04 Month (1-12)
             * 0x05 Year (bits 15:8). For example, 2006 signifies the year 2006
             * 0x06 Year (bits 7:0)
             * 0x07 Weekday (0-6, where 0 = Sunday and 6 = Saturday)
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x000E: /* GetArtworkFormats */
            /* The following is the description for the Apple Firmware
             *
             * The device sends this command to obtain the list of supported
             * artwork formats on the iPod. No parameters are sent.
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0E  Command ID (bits 7:0)
             *  6   0xEB  Checksum/
             *
             * Returned Artwork Formats are a 7byte record.
             * formatID:2
             * pixelFormat:1
             * width:2
             * height:2
             */
        {
            IAP_TX_INIT4(0x04, 0x000F);
            L4_TX_TRANSID();
            if (iap_artwork_supported()) {
                IAP_TX_PUT_U16(IAP_ARTWORK_FORMAT_ID);
                IAP_TX_PUT(IAP_ARTWORK_PIXEL_FORMAT);
                IAP_TX_PUT_U16(IAP_ARTWORK_WIDTH);
                IAP_TX_PUT_U16(IAP_ARTWORK_HEIGHT);
            }
            iap_send_tx();
            break;
        }
        case 0x000F: /* RetArtworkFormats. See Above */
            /* The following is the description for the Apple Firmware
             *
             * The iPod sends this command to the device, giving it the list
             * of supported artwork formats. Each format is described in a
             * 7-byte record (formatID:2, pixelFormat:1, width:2, height:2).
             * The formatID is used when sending GetTrackArtworkTimes.
             * The device may return zero records.
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x0F  Command ID (bits 7:0)
             *  NN  0xNN  formatID (15:8) iPod-assigned value for this format
             *  NN  0xNN  formatID (7:0)
             *  NN  0xNN  pixelFormat. Same as from SetDisplayImage
             *  NN  0xNN  imageWidth(15:8).Number of pixels widefor eachimage.
             *  NN  0xNN  imageWidth (7:0)
             *  NN  0xNN  imageHeight (15:8). Number of pixels high for each
             *  NN  0xNN  imageHeight (7:0).  image
             *  Previous 7 bytes may be repeated NN times
             *  NN  0xNN  Checksum
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0010: /* GetTrackArtworkData */
            /* The following is the description for the Apple Firmware
             * The device sends this command to the iPod to request data for a
             * given trackIndex, formatID, and artworkIndex. The time offset
             * from track start is the value returned by GetTrackArtworkTimes
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0D  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x10  Command ID (bits 7:0)
             *  6   0xNN  trackIndex (31:24).
             *  7   0xNN  trackIndex(23:16)
             *  8   0xNN  trackIndex (15:8)
             *  9   0xNN  trackIndex (7:0)
             * 10   0xNN  formatID (15:8)
             * 11   0xNN  formatID (7:0)
             * 12   0xNN  time offset from track start, in ms (31:24)
             * 13   0xNN  time offset from track start, in ms (23:16)
             * 14   0xNN  time offset from track start, in ms (15:8)
             * 15   0xNN  time offset from track start, in ms (7:0)
             * 16   0xNN  Checksum
             *
             * Returned data is
             * DescriptorTelegramIndex:        2
             * pixelformatcode:                1
             * ImageWidthPixels:               2
             * ImageHeightPixels:              2
             * InsetRectangleTopLeftX:         2
             * InsetRectangleTopLeftY:         2
             * InsetRectangleBotRightX:        2
             * InsetRectangleBotRightY:        2
             * RowSizeInBytes:                 4
             * ImagePixelData:VariableLength
             * Subsequent packets omit bytes 8 - 24
             */
        {
            struct mp3entry id3;
            enum iap_artwork_start_result result;
            uint32_t session_id;
            uint32_t track_index;

            L4_NEED(10);
            session_id = iap_artwork_session_id();
            track_index = get_u32(&L4_PARAM(0));
            if (get_u16(&L4_PARAM(4)) != IAP_ARTWORK_FORMAT_ID
                || get_u32(&L4_PARAM(6)) != 0
                || !iap_get_trackinfo(track_index, &id3)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            result = iap_artwork_start_transfer(0x04, 0x0010, 0x0011,
                                                doff != 0,
                                                l4_tid_hi, l4_tid_lo,
                                                session_id, &id3);
            if (result == IAP_ARTWORK_START_FAILED)
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            else if (result == IAP_ARTWORK_START_BUSY)
                cmd_ack(cmd, IAP_ACK_NO_RESOURCE);
            break;
        }
        case 0x0011: /* RetTrackArtworkData. See Abobe */
            /* The following is the description for the Apple Firmware
             *
             * The iPod sends the requested artwork to the accessory. Multiple
             * RetTrackArtworkData commands may be necessary to transfer all
             * the data because it will be too much to fit into a single packet.
             * This command uses nearly the same format as the SetDisplayImage
             * command (command 0x0032). The only difference is the addition
             * of 2 coordinates; they define an inset rectangle that describes
             * any padding that may have been added to the image. The
             * coordinates consist of two x,y pairs. Each x or y value is 2
             * bytes, so the total size of the coordinate set is 8 bytes.
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x11  Command ID (bits 7:0)
             *  6   0x00  Descriptor telegram index (15:8).
             *            These fields uniquely identify each packet in the
             *            RetTrackArtworkData transaction. The first telegram
             *            is the descriptor telegram, which always starts with
             *            an index of 0x0000.
             *  7   0x00  Descriptor telegram index (7:0)
             *  8   0xNN  Display pixel format code.
             *  9   0xNN  Imagewidth in pixels (15:8)
             * 10   0xNN  Image width in pixels (7:0)
             * 11   0xNN  Image height in pixels (15:8)
             * 12   0xNN  Image height in pixels (7:0)
             * 13   0xNN  Inset rectangle, top-left point, x value (15:8)
             * 14   0xNN  Inset rectangle, top-left point, x value (7:0)
             * 15   0xNN  Inset rectangle, top-left point, y value (15:8)
             * 16   0xNN  Inset rectangle, top-left point, y value (7:0)
             * 17   0xNN  Inset rectangle,bottom-rightpoint,xvalue(15:8)
             * 18   0xNN  Inset rectangle,bottom-rightpoint, x value(7:0)
             * 19   0xNN  Inset rectangle,bottom-rightpoint,y value(15:8)
             * 20   0xNN  Inset rectangle, bottom-right point, y value(7:0)
             * 21   0xNN  Rowsize in bytes (31:24)
             * 22   0xNN  Rowsize in bytes (23:16)
             * 23   0xNN  Row size in bytes (15:8)
             * 24   0xNN  Row size in bytes (7:0)
             * 25-NN 0xNN Image pixel data (variable length)
             * NN   0xNN  Checksum
             *
             * In subsequent packets in the sequence, bytes 8 through 24 are
             * omitted.
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0012: /* RequestProtocolVersion */
            /* The following is the description for the Apple Firmware
             *
             * This command is deprecated.
             *
             * Requests the version of the running protocol from the iPod.
             * The iPod responds with a Command 0x0013:ReturnProtocolVersion
             * command.
             *
             * Note: This command requests the Extended Interface protocol
             * version, not the iPod software version.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x12  Command ID (bits 7:0)
             *  6   0xE7  Telegram payload checksum byte
             *
             */
        {
            IAP_TX_INIT4(0x04, 0x0013);
            L4_TX_TRANSID();
            IAP_TX_PUT(LINGO_MAJOR(0x04));
            IAP_TX_PUT(LINGO_MINOR(0x04));
            iap_send_tx();
            break;
        }
        case 0x0013: /* ReturnProtocolVersion. See Above */
            /* The following is the description for the Apple Firmware
             * This command is deprecated.
             * Sent from the iPod to the device
             * Returns the iPod Extended Interface protocol version number.
             * The iPod sends this command in response to the Command 0x0012:
             * RequestProtocolVersion command from the device. The major
             * version number specifies the protocol version digits to the left
             * of the decimal point; the minor version number specifies the
             * digits to the right of the decimal point. For example, a major
             * version number of 0x01 and a minor version number of 0x08
             * represents an Extended Interface protocol version of 1.08. This
             * protocol information is also available through the General lingo
             * (lingo 0x00) command RequestLingoProtocolVersion when passing
             * lingo 0x04 as the lingo parameter.
             *
             * Note: This command returns the Extended Interface protocol
             * version, not the iPod software version.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x05  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x13  Command ID (bits 7:0)
             *  6   0xNN  Protocol major version number
             *  7   0xNN  Protocol minor version number
             *  8   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0014: /* RequestiPodName */
            /* The following is the description for the Apple Firmware
             * This command is deprecated.
             * Retrieves the name of the iPod
             *
             * Returns the name of the user's iPod or 'iPod' if the iPod name
             * is undefined. This allows the iPod name to be shown in the
             * human-machineinterface(HMI) of the interfacingbody. The iPod
             * responds with the Command 0x0015: ReturniPodName command
             * containing the iPod name text string.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x14  Command ID (bits 7:0)
             *  6   0xE5  Telegram payload checksum byte
             *
             * We return ROCKBOX, should this be definable?
             * Should it be Volume Name?
             */
        {
            IAP_TX_INIT4(0x04, 0x0015);
            L4_TX_TRANSID();
            IAP_TX_PUT_STRING("ROCKBOX");
            iap_send_tx();
            break;
        }
        case 0x0015: /* ReturniPodName. See Above */
            /* The following is the description for the Apple Firmware
             * This command is deprecated.
             * Sent from the iPod to the device
             *
             * The iPod sends this command in response to the Command 0x0014:
             * RequestiPodName telegram from the device. The iPod name is
             * encoded as a null-terminated UTF-8 character array. The iPod
             * name string is not limited to 252 characters; it may be sent
             * in small or large telegram format. The small telegram format
             * is shown.
             *
             * Note: Starting with version 1.07 of the Extended Interface lingo,
             * the ReturniPodName command on Windows-formatted iPods returns the
             * iTunes name of the iPod instead of the Windows volume name.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x15  Command ID (bits 7:0)
             *  6-N 0xNN  iPod name as UTF-8 character array
             *  NN  0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0016: /* ResetDBSelection */
            /* The following is the description for the Apple Firmware
             *
             * Resets the current database selection to an empty state and
             * invalidates the category entry count that is, sets the count
             * to 0, for all categories except the playlist category. This is
             * analogous to pressing the Menu button repeatedly to get to the
             * topmost iPod HMI menu. Any previously selected database items
             * are deselected. The command has no effect on the playback engine
             * In response, the iPod sends an ACK telegram with the command
             * status. Once the accessory has reset the database selection,
             * it must initialize the category count before it can select
             * database records. Please refer to Command 0x0018:
             * GetNumberCategorizedDBRecords and Command 0x0017:
             * SelectDBRecord for details.
             *
             * Note: Starting with protocol version 1.07, the ResetDBSelection
             * command clears the sort order.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x16  Command ID (bits 7:0)
             *  6   0xE3  Telegram payload checksum byte
             *
             *
             * Reset the DB Record Type
             * Hierarchy is as follows
             * All        = 0
             * Playlist   = 1
             * Artist     = 2
             * Album      = 3
             * Genre      = 4
             * Tracks     = 5
             * Composers  = 6
             * Audiobooks = 7
             * Podcasts   = 8
             *
             */
        {
            cur_dbrecord[0] = 0;
            put_u32(&cur_dbrecord[1],0);
            /* MFi 5.1.19 (p.414): this "invalidates the category entry
             * count (sets the count to 0) for all categories except the
             * playlist category". The count survived, so a stale one
             * could outlive the selection it belonged to.
             * ResetDBSelectionHierarchy already did this. */
            dbrecordcount = 0;
            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0017: /* SelectDBRecord */
            /* The following is the description for the Apple Firmware
             * Selects one or more records in the Database Engine, based on a
             * category relative index. For example, selecting category two
             * (artist) and record index one results in a list of selected
             * tracks (or database records) from the second artist in the
             * artist list.
             * Selections are additive and limited by the category hierarchy;
             * Subsequent selections are made based on the subset of records
             * resulting from the previous selections and not from the entire
             * database.
             * Note that the selection of a single record automatically passes
             * it to the Playback Engine and starts its playback. Record indices
             * consist of a 32-bit signed integer. To select database records
             * with a specific sort order, use
             * Command 0x0038: SelectSortDBRecord
             * SelectDBRecord should be called only once a category count has
             * been initialized through a call to Command 0x0018:
             * GetNumberCategorizedDBRecords. Without a valid category count,
             * the SelectDBRecord call cannot select a database record and will
             * fail with a command failed ACK. Accessories that make use of
             * Command 0x0016: ResetDBSelection must always initialize the
             * category count before selecting a new database record using
             * SelectDBRecord.
             * Accessories should pay close attention to the ACK returned by the
             * SelectDBRecord command. Ignoring errors may cause unexpected
             * behavior.
             * To undo a database selection, send the SelectDBRecord telegram
             * with the current category selected in theDatabase Engine and a
             * record index of -1 (0xFFFFFFFF). This has the same effect as
             * pressing the iPod Menu button once and moves the database
             * selection up to the next highest menu level. For example, if a
             * device selected artist number three and then album number one,
             * it could use the SelectDBRecord(Album, -1) telegram to return
             * to the database selection of artist number three. If multiple
             * database selections have been made, devices can use any of the
             * previously used categories to return to the next highest database
             * selection. If the category used in one of these SelectDBRecord
             * telegrams has not been used in a previous database selection
             * then the command is treated as a no-op.
             * Sending a SelectDBRecord telegram with the Track category and a
             * record index of -1 is invalid, because the previous database
             * selection made with the Track category and a valid index passes
             * the database selection to the Playback Engine.
             * Sending a SelectDBRecord(Track, -1) telegram returns a parameter
             * error. The iPod also returns a bad parameter error ACK when
             * devices send the SelectDBRecord telegram with an invalid category
             * type, or with the Track category and an index greater than the
             * total number of tracks available on the iPod.
             *
             * Note: Selecting a podcast always selects from the main podcast
             * library regardless of the current category context of the iPod.
             *
             * To immediately go to the topmost iPod menu level and reset all
             * database selections, send the ResetDBSelection telegram to the
             * iPod.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x08  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x17  Command ID (bits 7:0)
             *  6   0xNN  Database category type. See
             *  7   0xNN  Database record index (bits 31:24)
             *  8   0xNN  Database record index (bits 23:16)
             *  9   0xNN  Database record index (bits 15:8)
             * 10   0xNN  Database record index (bits 7:0)
             * 11   0xNN  Telegram payload checksum byte
             *
             * The valid database categories are listed below
             *
             * Category Code Protocol version
             * Reserved 0x00 N/A
             * Playlist 0x01 1.00
             * Artist   0x02 1.00
             * Album    0x03 1.00
             * Genre    0x04 1.00
             * Track    0x05 1.00
             * Composer 0x06 1.00
             * Audiobook0x07 1.06
             * Podcast  0x08 1.08
             * Reserved 0x09+ N/A
             *
             * cur_dbrecord[0] is the record type
             * cur_dbrecord[1-4] is the u32 of the record number requested
             * which might be a playlist or a track number depending on
             * the value of cur_dbrecord[0]
             */
        {
            /* SelectDBRecord, Table 5-26 p.416. */
            L4_NEED(5);
            memcpy(cur_dbrecord, &L4_PARAM(0), 5);

            int paused = !!(audio_status() & AUDIO_STATUS_PAUSE);
            uint32_t index;
            uint32_t trackcount;
            index = get_u32(&cur_dbrecord[1]);
            trackcount = playlist_amount();
            /* Every record type from 0x02 to 0x06 reaches audio_skip()
             * below, not just 0x05, and audio_skip() locks the device
             * up on an out-of-range offset rather than rejecting it.
             * The bound is also >=, not >: a valid index is 0..count-1.
             */
            /* An index of -1 is how an accessory undoes a selection and
             * moves up a category level, so the range bound below lets
             * it through for every category. MFi 5.1.20 (p.416) carves
             * out two:
             *
             *   "Sending a SelectDBRecord command with the Track or
             *    Audiobook category and a record index of -1 is
             *    invalid ... Sending a SelectDBRecord(Track, -1)
             *    command returns a parameter error."
             *
             * Track is 0x05 and Audiobook 0x07 (Table 5-27, p.417).
             * The comment here used to claim the bound below rejected
             * them; it does not, because the bound excludes 0xFFFFFFFF
             * outright. So -1 reached audio_skip() as an unsigned
             * 0xFFFFFFFF, which walked playback back to the front of
             * the playlist, and the reply said Success. */
            if ((cur_dbrecord[0] == 0x05 || cur_dbrecord[0] == 0x07) &&
                (index == 0xFFFFFFFFu))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* Everything else with an index of -1 is the undo, and it
             * is a Database Engine operation only. MFi 5.1.20 (p.415):
             * "This has the same effect as pressing the iPod Menu
             * button once and moves the database selection up to the
             * next highest menu level", and p.416: "If the category
             * used in one of these SelectDBRecord commands has not been
             * used in a previous database selection then the command is
             * treated as a no-op."
             *
             * The range bound below deliberately lets -1 past, and the
             * Artist/Album/Genre/Composer arm then ran
             * audio_skip(index - playlist_next(0)) on an unsigned
             * 0xFFFFFFFF -- so an accessory backing out of an album
             * list restarted the queue and was told Success. Track and
             * Audiobook are refused above; the rest must not move
             * playback at all. */
            if (index == 0xFFFFFFFFu)
            {
                if ((cur_dbrecord[0] < 0x01) || (cur_dbrecord[0] > 0x06))
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
                cmd_ok(cmd);
                break;
            }
            /* The categories this device actually plays from, named
             * rather than spanned. Written as 2..6 it read as "the
             * browsable categories" and silently included Genre and
             * Composer, which have no index here -- the same range
             * assumption that let those two report zero records and
             * then accept a selection. No behaviour change: 0x04 and
             * 0x06 are refused by the default arm below either way. */
            if (((cur_dbrecord[0] == 0x02) || (cur_dbrecord[0] == 0x03)
                 || (cur_dbrecord[0] == 0x05)) &&
                (index >= trackcount))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* GetNumberCategorizedDBRecords answers
             * nbr_total_playlists() + 1 for this category, so the last
             * valid index is nbr_total_playlists(). The bound was > ,
             * which let one past the end through; RetrieveCategorized-
             * DatabaseRecords (0x001A) already uses >= against the same
             * count. */
            if ((cur_dbrecord[0] == 1) && (index >= (nbr_total_playlists() + 1)))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            audio_pause();
            switch (cur_dbrecord[0])
            {
                case 0x01: /* Playlist*/
                {
                    if (index != 0x00) /* 0x00 is the On-The-Go Playlist and
                                          we do nothing with it  */
                    {
                        /* No rewind first. seek_to_playlist() calls
                         * playlist_create(), which replaces the queue
                         * outright, and then starts it at 0 -- so
                         * walking the old queue back to its front
                         * changed nothing. The sibling at
                         * SelectSortDBRecord's Playlist arm keeps its
                         * rewind because the sort between the two acts
                         * on the queue that is still there. */
                        last_selected_playlist = index;
                        if (!seek_to_playlist(last_selected_playlist)) {
                            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                            return;
                        }
                    }
                    break;
                }
                case 0x02: /* Artist   */
                case 0x03: /* Album    */
                case 0x05: /* Track    */
                {
                    /* Genre and Composer are not here, and fall to the
                     * default below. The device reports zero records in
                     * both, so accepting a selection in one -- and
                     * moving playback for it -- was the third of three
                     * different answers about the same category. */
                    /* Started from stopped rather than skipped into,
                     * for the reason PlayCurrentSelection is. And
                     * through the accessory's index space, not past
                     * it. */
                    if (iap_play_state_byte() == 0x00)
                        playlist_start(
                            iap_track_from_mfi((uint32_t)index), 0, 0);
                    else
                        /* An MFi-space delta, not a Rockbox one.
                         * get_next_index() (apps/playlist.c) computes
                         * "rotate_index(playlist->index) + steps" under
                         * REPEAT_OFF, and rotate_index() is exactly
                         * iap_track_to_mfi() -- so steps is added to,
                         * and range-checked against, the accessory's
                         * own index space. Under REPEAT_ALL the two
                         * deltas are congruent modulo the playlist
                         * length, so this form is right in both modes
                         * and the converted one is right in only one.
                         */
                        audio_skip((long)index - iap_get_trackindex());
                    break;
                }
                default:
               {
                    /* MFi 5.1.20 (p.416): "The Apple device also
                     * returns a bad parameter error iPodAck when
                     * accessories send the SelectDBRecord command with
                     * an invalid category type."
                     *
                     * These fell through to the Success ack below,
                     * which contradicted the sibling handler: 34012c491e
                     * made GetNumberCategorizedDBRecords answer
                     * Unknown Database Category for 0x09-0xFF, so the
                     * same device declared a category unknown and then
                     * accepted a selection in it. A head unit that
                     * believed the Success sent PlayCurrentSelection
                     * and played whatever was already queued. */
                    if (!paused)
                        audio_resume();
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    return;
               }
            }
            if (!paused)
                audio_resume();
            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0018: /* GetNumberCategorizedDBRecords */
            /* The following is the description for the Apple Firmware
             *
             * Retrieves the number of records in a particular database
             * category.
             * For example, a device can get the number of artists or albums
             * present in the database. The category types are described above.
             * The iPod responds with a Command 0x0019:
             * ReturnNumberCategorizedDBRecords telegram indicating the number
             * of records present for this category.
             * GetNumberCategorizedDBRecords must be called to initialize the
             * category count before selecting a database record using Command
             * 0x0017: SelectDBRecord or Command 0x0038: SelectSortDBRecord
             * commands. A category’s record count can change based on the prior
             * categories selected and the database hierarchy. The accessory
             * is expected to call GetNumberCategorizedDBRecords in order to
             * get the valid range of category entries before selecting a
             * record in that category.
             *
             * Note: The record count returned by this command depends on the
             * database state before this command is sent. If the database has
             * been reset using Command 0x0016: ResetDBSelection this command
             * returns the total number of records for a given category.
             * However, if this command is sent after one or more categories
             * are selected, the record count is the subset of records that are
             * members of all the categories selected prior to this command.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x18  Command ID (bits 7:0)
             *  6   0xNN  Database category type. See above
             *  7   0xNN  Telegram payload checksum byte
             *
             * This is the actual number of available records for that category
             * some head units (Alpine CDE-103BT) use this command before
             * requesting records and then hang if not enough records are
             * returned.
             */
        {
            /* GetNumberCategorizedDBRecords, Table 5-28 p.417. */
            L4_NEED(1);
            unsigned char data[] = {0x04, 0x00, 0x19,
                                    0x00, 0x00, 0x00, 0x00};
            switch(L4_PARAM(0)) /* type number */
            {
                case 0x01: /* total number of playlists */
                    dbrecordcount = nbr_total_playlists() + 1;
                    break;
                case 0x05: /* total number of Tracks */
                case 0x02: /* total number of Artists */
                case 0x03: /* total number of Albums */
                           /* We don't sort on the above but some Head Units
                            * require at least one to exist so we just return
                            * the number of tracks in the playlist. */
                    dbrecordcount = playlist_amount();
                    break;
                case 0x00: /* Top-level: no hierarchy above the
                            * playback engine here, so zero */
                case 0x04: /* total number of Genres */
                case 0x06: /* total number of Composers */
                case 0x07: /* total number of AudioBooks */
                case 0x08: /* total number of Podcasts */
                           /* We don't support the above so just return that
                              there are none available. */
                    dbrecordcount = 0;
                    break;
                default:
                    /* Table 5-27 (p.417) reserves 0x0C-0xFF, and puts
                     * Nested playlist (0x09) at protocol 1.13 and
                     * Genius Mixes (0x0A) and iTunesU (0x0B) at 1.14 --
                     * all above the 1.12 this device advertises
                     * (iap-core.c:209). None of them exists here, so
                     * "unknown category" is the truthful answer rather
                     * than a count of zero, which would invite a
                     * RetrieveCategorizedDatabaseRecords for records
                     * that cannot be enumerated.
                     *
                     * Without a
                     * default the switch left dbrecordcount -- a
                     * file-static -- holding the previous query's
                     * answer, and it went out anyway: after asking for
                     * Tracks and being told 500, an accessory asking
                     * for Genius Mixes was told 500 of those too. Its
                     * follow-up RetrieveCategorizedDatabaseRecords is
                     * then refused by the sibling handler with
                     * IAP_ACK_UNKNOWN_DB, so the two answers
                     * contradicted each other and the browse list
                     * filled with empty rows.
                     *
                     * Table 3-6 (p.125) status 0x01 is "ERROR: Unknown
                     * database category or session ID". */
                    dbrecordcount = 0;
                    cmd_ack(cmd, IAP_ACK_UNKNOWN_DB);
                    return;
                    break;
            }
            put_u32(&data[3], dbrecordcount);
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x0019: /* ReturnNumberCategorizedDBRecords. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the number of database records matching the specified
             * database category. The iPod sends this telegram in response to
             * the Command 0x0018: GetNumberCategorizedDBRecords telegram from
             * the device. Individual records can then be extracted by sending
             * Command 0x001A: RetrieveCategorizedDatabaseRecords to the iPod.
             * If no matching database records are found, a record count of
             * zero is returned. Category types are described above.
             * After selecting the podcast category, the number of artist,
             * album, composer, genre, and audiobook records is always zero.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x19  Command ID (bits 7:0)
             *  6   0xNN  Database record count (bits 31:24)
             *  7   0xNN  Database record count (bits 23:16)
             *  8   0xNN  Database record count (bits 15:8)
             *  9   0xNN  Database record count (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x001A: /* RetrieveCategorizedDatabaseRecords */
            /* The following is the description for the Apple Firmware
             *
             * Retrieves one or more database records from the iPod,
             * typically based on the results from the Command 0x0018:
             * GetNumberCategorizedDBRecords query. The database
             * category types are described above. This telegram
             * specifies the starting record index and the number of
             * records to retrieve (the record count). This allows a device
             * to retrieve an individual record or the entire set of records
             * for a category. The record start index and record count consist
             * of 32-bit signed integers. To retrieve all records from a given
             * starting record index, set the record count to -1 (0xFFFFFFFF).
             * The iPod responds to this telegram with a separate Command
             * 0x001B: ReturnCategorizedDatabaseRecord telegram FOR EACH record
             * matching the specified criteria (category and record index
             * range).
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0C  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1A  Command ID (bits 7:0)
             *  6   0xNN  Database category type. See above
             *  7   0xNN  Database record start index (bits 31:24)
             *  8   0xNN  Database record start index (bits 23:16)
             *  9   0xNN  Database record start index (bits 15:8)
             * 10   0xNN  Database record start index (bits 7:0)
             * 11   0xNN  Database record read count (bits 31:24)
             * 12   0xNN  Database record read count (bits 23:16)
             * 13   0xNN  Database record read count (bits 15:8)
             * 14   0xNN  Database record read count (bits 7:0)
             * 15   0xNN  Telegram payload checksum byte

             * The returned data
             * contains information for a single database record. The iPod sends
             * one or more of these telegrams in response to the Command 0x001A:
             * RetrieveCategorizedDatabaseRecords telegram from the device. The
             * category record index is included to allow the device to
             * determine which record has been sent. The record data is sent as
             * a null-terminated UTF-8 encoded data array.
             */
        {
            /* RetrieveCategorizedDatabaseRecords, Table 5-30 p.418. */
            L4_NEED(9);
            unsigned char data[7 + MAX_PATH] =
                            {0x04, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00,
                             'O','n','-','T','h','e','-','G','o','\0'};
            struct mp3entry id3;

            unsigned long start_index = get_u32(&L4_PARAM(1));
            unsigned long read_count = get_u32(&L4_PARAM(5));
            unsigned long counter = 0;
            unsigned int number_of_playlists = nbr_total_playlists();
            uint32_t trackcount;
            trackcount = playlist_amount();

            /* Clamp read_count to the records actually available.
             * A count of -1 means "all records" and is covered by the
             * same test. Compare by subtraction rather than adding to
             * start_index, which would wrap for counts close to 2^32
             * and let an oversized count through.
             */
            if (L4_PARAM(0) == 0x01) {
                if (start_index >= (number_of_playlists + 1)) {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
                if (read_count > (number_of_playlists + 1) - start_index)
                    read_count = (number_of_playlists + 1) - start_index;
            }
            /* Genre (0x04) and Composer (0x06) are excluded, as
             * Audiobook and Podcast already are.
             * GetNumberCategorizedDBRecords answers 0 for all four --
             * this device has no genre or composer index -- and this
             * band then enumerated playlist_amount() "Not Supported"
             * records for two of them. One device, two answers about
             * how many records a category has. */
            else if ((L4_PARAM(0) == 0x02) || (L4_PARAM(0) == 0x03)
                     || (L4_PARAM(0) == 0x05)) {
                if (start_index >= trackcount) {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
                if (read_count > trackcount - start_index)
                    read_count = trackcount - start_index;
            }
            else {
                /* Categories we do not implement. Without this the
                 * count is left as received, and the spec's own way of
                 * asking for everything -- count = -1 (Table 5-30) --
                 * spins the loop below 4.3 billion times re-sending an
                 * unchanged buffer, wedging the iAP thread for the rest
                 * of the session. Refuse with the status the spec
                 * defines for exactly this (Table 5-2, 0x01).
                 */
                cmd_ack(cmd, IAP_ACK_UNKNOWN_DB);
                break;
            }
            /* Latched before the loop. L4_PARAM(0) is a live address in
             * the receive buffer and the loop yields at the bottom of
             * every iteration -- iap_handlepkt() documents that
             * iap_reset_buffers() can rewind those pointers from the
             * USB thread while a handler is yielding, after which the
             * next packet is written over the one being enumerated and
             * the record type changes mid-list. The accessory would
             * receive playlist names and track titles interleaved under
             * one continuous index sequence. */
            /* Both clamps above only ever lower read_count, so a zero
             * survives -- the loop body never runs and nothing is sent.
             * Every other terminating path in this handler replies, and
             * an accessory waiting on one has no way to tell silence
             * from a packet that was lost. */
            if (read_count == 0) {
                cmd_ack(cmd, IAP_ACK_OK);
                break;
            }

            unsigned char category = L4_PARAM(0);

            for (counter=0;counter<read_count;counter++)
            {
                switch(category) /* type number */
                {
                    case 0x01: /* Playlists */
                       get_playlist_name(data +7,start_index+counter, MAX_PATH);
                        /*Remove file extension*/
                       char *dot=NULL;
                       dot = (strrchr(data+7, '.'));
                       if (dot != NULL)
                           *dot = '\0';
                       break;
                    case 0x05: /* Tracks   */
                    case 0x02: /* Artists  */
                    case 0x03: /* Albums   */
                    case 0x04: /* Genre    */
                    case 0x06: /* Composer */
                        /* Same substitution as GetIndexedPlayingTrackInfo:
                         * the probe was in the wrong index space and its
                         * result unused, and an index off the end now
                         * leaves the row's name empty rather than
                         * copying out a stale id3. */
                        if (!iap_get_trackinfo(start_index + counter, &id3))
                            memset(&id3, 0, sizeof(id3));
                        switch(category)
                        {
                            case 0x05:
                            {
                                const char *title = id3.title ? id3.title : "";
                                strmemccpy((char *)&data[7], title, 64);
                                break;
                            }
                            case 0x02:
                            {
                                const char *artist = id3.artist ? id3.artist : "";
                                strmemccpy((char *)&data[7], artist, 64);
                                break;
                            }
                            case 0x03:
                            {
                                const char *album = id3.album ? id3.album : "";
                                strmemccpy((char *)&data[7], album, 64);
                                break;
                            }
                            case 0x04:
                            case 0x06:
                                strmemccpy((char *)&data[7], "Not Supported",14);
                                break;
                        }
                        break;
                }
                put_u32(&data[3], start_index+counter);
                {
                    /* get_playlist_name() may fill this up to MAX_PATH,
                     * but the packet has to fit TX_BUFLEN or
                     * iap_send_tx() panics. Truncate the name instead.
                     */
                    size_t nlen = strlen((char *)data + 7);
                    if (nlen > 63)
                    {
                        nlen = 63;
                        data[7 + nlen] = '\0';
                    }
                    L4_SEND_REPLY(data, 7 + nlen + 1);
                }
                yield();
            }
            break;
        }
        case 0x001B: /* ReturnCategorizedDatabaseRecord. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Contains information for a single database record. The iPod sends
             * ONE OR MORE of these telegrams in response to the Command 0x001A:
             * RetrieveCategorizedDatabaseRecords telegram from the device. The
             * category record index is included to allow the device to
             * determine which record has been sent. The record data is sent
             * as a null-terminated UTF-8 encoded data array.
             *
             * Note: The database record string is not limited to 252 characters
             * it may be sent in small or large telegram format, depending on
             * the record size. The small telegram format is shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1B  Command ID (bits 7:0)
             *  6   0xNN  Database record category index (bits 31:24)
             *  7   0xNN  Database record category index (bits 23:16)
             *  8   0xNN  Database record category index (bits 15:8)
             *  9   0xNN  Database record category index (bits 7:0)
             * 10-N 0xNN  Database record as a UTF-8 character array.
             * NN   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x001C: /* GetPlayStatus */
            /* The following is the description for the Apple Firmware
             *
             * Requests the current iPod playback status, allowing the
             * device to display feedback to the user. In response, the
             * iPod sends a Command 0x001D: ReturnPlayStatus telegram
             * with the current playback status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1C  Command ID (bits 7:0)
             *  6   0xDD  Telegram payload checksum byte
             *
             */
        {
            unsigned char data[] = {0x04, 0x00, 0x1D,
                                    0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00,
                                    0x00};
            struct mp3entry *id3 = audio_current_track();
            unsigned long time_total = id3->length;
            unsigned long time_elapsed = id3->elapsed;
            put_u32(&data[3], time_total);
            put_u32(&data[7], time_elapsed);
            data[11] = iap_play_state_byte();
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x001D: /* ReturnPlayStatus. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the current iPod playback status. The iPod sends this
             * telegram in response to the Command 0x001C: GetPlayStatus
             * telegram from the device. The information returned includes the
             * current track length, track position, and player state.
             *
             * Note: The track length and track position fields are valid only
             * if the player state is Playing or Paused. For other player
             * states, these fields should be ignored.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0C  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1D  Command ID (bits 7:0)
             *  6   0xNN  Track length in milliseconds (bits 31:24)
             *  7   0xNN  Track length in milliseconds (bits 23:16)
             *  8   0xNN  Track length in milliseconds (bits 15:8)
             *  9   0xNN  Track length in milliseconds (bits 7:0)
             * 10   0xNN  Track position in milliseconds (bits 31:24)
             * 11   0xNN  Track position in milliseconds (bits 23:16)
             * 12   0xNN  Track position in milliseconds (bits 15:8)
             * 13   0xNN  Track position in milliseconds (bits 7:0)
             * 14   0xNN  Player state. Possible values are:
             *            0x00 = Stopped
             *            0x01 = Playing
             *            0x02 = Paused
             *            0x03 - 0xFE = Reserved
             *            0xFF = Error
             * 15   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x001E: /* GetCurrentPlayingTrackIndex */
            /* The following is the description for the Apple Firmware
             *
             * Requests the playback engine index of the currently playing
             * track. In response, the iPod sends a Command 0x001F:
             * ReturnCurrentPlayingTrackIndex telegram to the device.
             *
             * Note: The track index returned is valid only if there is
             * currently a track playing or paused.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1E  Command ID (bits 7:0)
             *  6   0xDB  Telegram payload checksum byte
             *
             */
        {
            unsigned char data[] = {0x04, 0x00, 0x1F,
                                    0xFF, 0xFF, 0xFF, 0xFF};
            uint32_t playlist_pos = iap_get_trackindex();
            /* MFi 5.1.28 (p.421): "If there is no track currently
             * playing or paused, an index of -1 (0xFFFFFFFF) is
             * returned." Testing the PAUSE bit directly answers yes for
             * RECORD|PAUSE, so a paused recording handed out a stale
             * playing index while GetPlayStatus, two hundred lines up,
             * used iap_play_state_byte() and said Stopped for the same
             * instant. This was the fourth site 0a3563086e unified and
             * the one it missed. */
            if (iap_play_state_byte() != 0x00)
                put_u32(&data[3], playlist_pos);
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x001F: /* ReturnCurrentPlayingTrackIndex. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the playback engine index of the current playing track in
             * response to the Command 0x001E: GetCurrentPlayingTrackIndex
             * telegram from the device. The track index is a 32-bit signed
             * integer.
             * If there is no track currently playing or paused, an index of -1
             * (0xFFFFFFFF) is returned.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x1F  Command ID (bits 7:0)
             *  6   0xNN  Playback track index (bits 31:24)
             *  7   0xNN  Playback track index (bits 23:16)
             *  8   0xNN  Playback track index (bits 15:8)
             *  9   0xNN  Playback track index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        /* 0x0021 and 0x0023 are Origin: Apple device -- the replies to
         * 0x0020 and 0x0022, not commands the device accepts. Each had a
         * case label and no body, so control fell into the shared 0x0024
         * handler, ran a metadata lookup, and then found no matching arm
         * in that handler's trailing switch: nothing was transmitted at
         * all and the accessory waited for a reply that never came.
         *
         * They are refused here rather than in command order, because
         * the three Get* commands below them genuinely do share one body
         * and giving these two a body in the middle of that run would
         * divert 0x0020 and 0x0022 into it. 0x0025, the third Return*,
         * is already refused after the shared body for the same reason. */
        case 0x0021: /* ReturnIndexedPlayingTrackTitle */
        case 0x0023: /* ReturnIndexedPlayingTrackArtistName */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0020: /* GetIndexedPlayingTrackTitle. See 0x0024 below */
            /* The following is the description for the Apple Firmware
             *
             * Requests the title name of the indexed playing track from the
             * iPod. In response to a valid telegram, the iPod sends a
             * Command 0x0021: ReturnIndexedPlayingTrackTitle telegram to the
             * device.
             *
             * Note: If the telegram length or playing track index is invalid,
             * the iPod responds with an ACK telegram including the specific
             * error status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x20  Command ID (bits 7:0)
             *  6   0xNN  Playback track index (bits 31:24)
             *  7   0xNN  Playback track index (bits 23:16)
             *  8   0xNN  Playback track index (bits 15:8)
             *  9   0xNN  Playback track index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        /* 0x0021 ReturnIndexedPlayingTrackTitle is refused above; the
         * description below is for the Apple firmware. */
            /* The following is the description for the Apple Firmware
             *
             * Returns the title of the indexed playing track in response to
             * a valid Command 0x0020 GetIndexedPlayingTrackTitle telegram from
             * the device. The track title is encoded as a null-terminated UTF-8
             * character array.
             *
             * Note: The track title string is not limited to 252 characters;
             * it may be sent in small or large telegram format, depending on
             * the string length. The small telegram format is shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x21  Command ID (bits 7:0)
             *  6-N 0xNN  Track title as a UTF-8 character array
             * NN   0xNN  Telegram payload checksum byte
             *
             */
        case 0x0022: /* GetIndexedPlayingTrackArtistName. See 0x0024 Below */
            /* The following is the description for the Apple Firmware
             *
             * Requests the name of the artist of the indexed playing track
             * In response to a valid telegram, the iPod sends a
             * Command 0x0023: ReturnIndexedPlayingTrackArtistName telegram to
             * the device.
             *
             * Note: If the telegram length or playing track index is invalid,
             * the iPod responds with an ACK telegram including the specific
             * error status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x22  Command ID (bits 7:0)
             *  6   0xNN  Playback track index (bits 31:24)
             *  7   0xNN  Playback track index (bits 23:16)
             *  8   0xNN  Playback track index (bits 15:8)
             *  9   0xNN  Playback track index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        /* 0x0023 ReturnIndexedPlayingTrackArtistName is refused above;
         * the description below is for the Apple firmware. */
            /* The following is the description for the Apple Firmware
             *
             * Returns the artist name of the indexed playing track in response
             * to a valid Command 0x0022 GetIndexedPlayingTrackArtistName
             * telegram from the device. The track artist name is encoded as a
             * null-terminated UTF-8 character array.
             *
             * Note: The artist name string is not limited to 252 characters;
             * it may be sent in small or large telegram format, depending on
             * the string length. The small telegram format is shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x23  Command ID (bits 7:0)
             *  6-N 0xNN  Track Artist as a UTF-8 character array
             * NN   0xNN  Telegram payload checksum byte
             *
             */
        case 0x0024: /* GetIndexedPlayingTrackAlbumName AND
                      * GetIndexedPlayingTrackTitle AND
                      * AND GetIndexedPlayingTrackArtistName. */
            /* The following is the description for the Apple Firmware
             *
             * Requests the album name of the indexed playing track
             * In response to a valid telegram, the iPod sends a
             * Command 0x0025: ReturnIndexedPlayingTrackAlbumName telegram to
             * the device.
             *
             * Note: If the telegram length or playing track index is invalid,
             * the iPod responds with an ACK telegram including the specific
             * error status.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x24  Command ID (bits 7:0)
             *  6   0xNN  Playback track index (bits 31:24)
             *  7   0xNN  Playback track index (bits 23:16)
             *  8   0xNN  Playback track index (bits 15:8)
             *  9   0xNN  Playback track index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* GetIndexedPlayingTrack* block, Tables 5-36/38/40 p.421-424. */
            L4_NEED(4);
            struct mp3entry id3;
            uint32_t tracknum = get_u32(&L4_PARAM(0));

            /* MFi 5.1.29 (p.421), repeated under 5.1.31 and 5.1.33:
             * "If the packet length or playing track index is invalid,
             * the Apple device responds with an iPodAck command
             * including the specific error status."
             *
             * The index was wrapped and then used unchecked, so index
             * 40 on a 40-track queue was answered with track 0's
             * metadata and the accessory believed row 41 existed. On
             * target long is 32-bit, so -1 survived the wrap too. The
             * sibling 0x000C refuses the identical index -- the two
             * handlers contradicted each other. Reachable without a
             * misbehaving accessory: the queue can shrink between the
             * count query and the metadata query. */
            if (!iap_get_trackinfo(tracknum, &id3))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* Return the requested track data.
             *
             * MFi 5.1.30 (p.422), and the same note under 5.1.32 and
             * 5.1.34: "The track title string is not limited to 252
             * characters; it may be sent in small or large packet
             * format, depending on the string length."
             *
             * These used to build the reply in a 70-byte stack buffer
             * and clamp the tag to 63 characters plus a NUL, because
             * strlcpy() returns strlen(src) rather than the number of
             * bytes written -- so a longer tag sent more than had been
             * copied, past the end of data[70] beyond 66 characters and
             * past TX_BUFLEN into panicf() beyond about 124. The clamp
             * closed the overflow and left the truncation.
             *
             * Build it in the TX buffer instead, the way lingo 3 does
             * for the same strings: iap_tx_strlcpy() bounds itself
             * against TX_BUFLEN, keeps its own count, and maps a NULL
             * tag to an empty string. The HID transport fragments
             * whatever comes out.
             */
            {
                const char *tag;

                switch (cmd)
                {
                    case 0x20: tag = id3.title;  break;
                    case 0x22: tag = id3.artist; break;
                    default:   tag = id3.album;  break;
                }

                IAP_TX_INIT4(0x04, (cmd + 1) & 0xFF);
                L4_TX_TRANSID();
                iap_tx_strlcpy(tag);
                iap_send_tx();
            }
            break;
        }
        case 0x0025: /* ReturnIndexedPlayingTrackAlbumName. See 0x0024 Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the album name of the indexed playing track in response
             * to a valid Command 0x0024 GetIndexedPlayingTrackAlbumName
             * telegram from the device. The track artist name is encoded as a
             * null-terminated UTF-8 character array.
             *
             * Note: The album name string is not limited to 252 characters;
             * it may be sent in small or large telegram format, depending on
             * the string length. The small telegram format is shown.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x25  Command ID (bits 7:0)
             *  6-N 0xNN  Track Album as a UTF-8 character array
             * NN   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0026: /* SetPlayStatusChangeNotification */
            /* The following is the description for the Apple Firmware
             * Sets the state of play status change notifications from the iPod
             * to the device. Notification of play status changes can be
             * globally enabled or disabled. If notifications are enabled, the
             * iPod sends a Command 0x0027: PlayStatusChangeNotification
             * telegram to the device each time the play status changes, until
             * the device sends this telegram again with the disable
             * notification option. In response, the iPod sends an ACK telegram
             * indicating the status of the SetPlayStatusChangeNotification
             * command.
             *
             * Byte Value Meaning
             *  0     0xFF  Sync byte (required only for UART serial)
             *  1     0x55  Start of telegram
             *  2     0x04  Telegram payload length
             *  3     0x04  Lingo ID: Extended Interface lingo
             *  4     0x00  Command ID (bits 15:8)
             *  5     0x26  Command ID (bits 7:0)
             *  6     0xNN  The state of play status change notifications.
             *            Possible values are:
             *              0x00 = Disable all change notification
             *              0x01 = Enable all change notification
             *              0x02 - 0xFF = Reserved
             *  7     0xNN   Telegram payload checksum byte
             */
        {
            struct iap_chapter_info chapter;
            struct mp3entry *id3;

            /* SetPlayStatusChangeNotification, Table 5-42 p.425. */
            L4_NEED(1);
            /* Two forms, distinguished by length.
             *
             * MFi Table 5-42 (p.425) is the one-byte form. Table 5-43 on
             * the same page is a four-byte big-endian bitmask, and Table
             * 5-45 marks bits 31:13 Reserved, so a conformant mask
             * always has 0x00 as its first byte. Reading a single byte
             * therefore saw 0x00 for every legal four-byte request and
             * turned notifications OFF while acking Success.
             *
             * p.424: "The accessory must send the four-byte form first;
             * if the iPodAck ... passes a status other than 0x00
             * (Success), then the accessory may send the one-byte form."
             * We ack Success, so a spec-following accessory never falls
             * back and simply never receives a notification. That is a
             * car head unit with a permanently blank display.
             */
            if (L4_HAVE(4)) {
                /* Keep the bits. Collapsing the mask to a bool meant an
                 * accessory subscribing to Track index alone also
                 * received Track time offset twice a second, and one
                 * that quietened this lingo silenced the Display Remote
                 * lingo with it. */
                device.pb_notifications = get_u32(&L4_PARAM(0));
            } else {
                /* Table 5-44 (p.425): the one-byte form's 0x01 means
                 * "basic play state, track index, track time position,
                 * FFW/REW seek stop, and chapter index changes", which
                 * is bits 00, 02, 03 and 05 of Table 5-45. Track time
                 * position is bit 03, the millisecond form, because
                 * that is the one this device sends. 0x00 disables all;
                 * 0x02-0xFF are Reserved and are treated as disable. */
                device.pb_notifications =
                    (L4_PARAM(0) == 0x01)
                        ? (BIT_N(0) | BIT_N(2) | BIT_N(3) | BIT_N(5))
                        : 0;
            }

            /* Baseline the change detectors, as the Display Remote
             * sibling does when an accessory subscribes. pb_play_status
             * carried the previous accessory's audio_status(), so if
             * that was Playing and audio has since stopped, the first
             * tick after a new accessory subscribed sent it a
             * "Playback stopped" for something that happened before it
             * arrived. */
            device.pb_play_status = audio_status();
            device.pb_ext_state = iap_play_state_byte();
            device.pb_trackpos_s = iap_get_trackpos() / 1000;
            device.pb_numtracks = (uint32_t)playlist_amount();
            device.pb_trackpos_ms = 0;
            id3 = audio_current_track();
            if ((audio_status() & AUDIO_STATUS_PLAY)
                && iap_current_chapter(id3, &chapter)) {
                device.pb_chapter_index = chapter.index;
                device.pb_chapterpos_ms = chapter.elapsed_ms;
                device.pb_chapterpos_s = chapter.elapsed_ms / 1000;
            } else {
                device.pb_chapter_index = UINT32_MAX;
                device.pb_chapterpos_ms = 0;
                device.pb_chapterpos_s = 0;
            }
            device.pb_track_changed = false;
            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        /* MFi 5.1.36 (p.426): "Lingo: 0x04 -- Origin: Apple device".
         * This had a label followed only by comments, so control fell
         * into 0x0028 PlayCurrentSelection: a notification sent to the
         * device paused playback, sorted or randomised the playlist,
         * skipped to whatever its payload happened to decode as a
         * selection index, and was then acknowledged Success. */
        case 0x0027: /* PlayStatusChangeNotification */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        /* This response is handled by iap_track_changed() and iap_periodic()
         * within iap-core.c
         * The following is the description for the Apple Firmware
         *
         * The iPod sends this telegram to the device when the iPod play status
         * changes, if the device has previously enabled notifications using
         * Command 0x0026: SetPlayStatusChangeNotification . This telegram
         * contains details about the new play status. Notification telegrams
         * for changes in track position occur approximately every 500
         * milliseconds while the iPod is playing. Notification telegrams are
         * sent from the iPod until the device sends the
         * SetPlayStatusChangeNotification telegram with the option to disable
         * all notifications.
         * Some notifications include additional data about the new play status.
         *
         * PlayStatusChangeNotification telegram: notifications 0x00, 0x02, 0x03
         * Byte Value Meaning
         *  0   0xFF  Sync byte (required only for UART serial)
         *  1   0x55  Start of telegram
         *  2   0x04  Telegram payload length
         *  3   0x04  Lingo ID: Extended Interface lingo
         *  4   0x00  Command ID (bits 15:8)
         *  5   0x27  Command ID (bits 7:0)
         *  6   0xNN  New play status. See Below.
         *  7   0xNN  Telegram payload checksum byte
         *
         *             Play Status Change          Code Extended Status Data (if
         *                                              any)
         *             Playback stopped            0x00 None
         *             Playback track changed      0x01 New track record index
         *                                             (32 bits)
         *             Playback forward seek stop  0x02 None
         *             Playback backward seek stop 0x03 None
         *             Playback track position     0x04 New track position in
         *                                              milliseconds (32 bits)
         *             Playback chapter changed    0x05 New chapter index (32
         *                                              bits)
         *             Reserved                    0x06 - 0xFF N/A
         *
         * Playback track changed (code 0x01)
         * Byte Value Meaning
         *  0   0xFF  Sync byte (required only for UART serial)
         *  1   0x55  Start of telegram
         *  2   0x08  Telegram payload length
         *  3   0x04  Lingo ID: Extended Interface lingo
         *  4   0x00  Command ID (bits 15:8)
         *  5   0x27  Command ID (bits 7:0)
         *  6   0x01  New status: Playback track changed
         *  7   0xNN  New playback track index (bits 31:24)
         *  8   0xNN  New playback track index (bits 23:16)
         *  9   0xNN  New playback track index (bits 15:8)
         * 10   0xNN  New playback track index (bits 7:0)
         * 11   0xNN  Telegram payload checksum byte
         *
         * Playback track position changed (code 0x04)
         * Byte Value Meaning
         *  0   0xFF  Sync byte (required only for UART serial)
         *  1   0x55  Start of telegram
         *  2   0x08  Telegram payload length
         *  3   0x04  Lingo ID: Extended Interface lingo
         *  4   0x00  Command ID (bits 15:8)
         *  5   0x27  Command ID (bits 7:0)
         *  6   0x04  New status: Playback track position changed
         *  7   0xNN  New track position in milliseconds (bits 31:24)
         *  8   0xNN  New track position in milliseconds (bits 23:16)
         *  9   0xNN  New track position in milliseconds (bits 15:8)
         * 10   0xNN  New track position in milliseconds (bits 7:0)
         * 11   0xNN  Telegram payload checksum byte
         *
         * Playback chapter changed (code 0x05)
         * Byte Value Meaning
         *  0   0xFF  Sync byte (required only for UART serial)
         *  1   0x55  Start of telegram
         *  2   0x08  Telegram payload length
         *  3   0x04  Lingo ID: Extended Interface lingo
         *  4   0x00  Command ID (bits 15:8)
         *  5   0x27  Command ID (bits 7:0)
         *  6   0x05  New status: Playback chapter changed
         *  7   0xNN  New track chapter index (bits 31:24)
         *  8   0xNN  New track chapter index (bits 23:16)
         *  9   0xNN  New track chapter index (bits 15:8)
         * 10   0xNN  New track chapter index (bits 7:0)
         * 11   0xNN  Telegram payload checksum byte
         *
         */
        case 0x0028: /* PlayCurrentSelection */
            /* The following is the description for the Apple Firmware
             *
             * Requests playback of the currently selected track or list of
             * tracks. The currently selected tracks are placed in the Now
             * Playing playlist, where they are optionally shuffled (if the
             * shuffle feature is enabled). Finally, the specified track record
             * index is passed to the player to play. Note that if the track
             * index is -1(0xFFFFFFFF), the first track after the shuffle is
             * complete is played first. If a track index of n is sent, the nth
             * track of the selected tracks is played first, regardless of
             * where it is located in the Now Playing playlist after the shuffle
             * is performed (assuming that shuffle is on). In response, the iPod
             * sends an ACK telegram indicating the status of the command.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x28  Command ID (bits 7:0)
             *  6   0xNN  Selection track record index (bits 31:24)
             *  7   0xNN  Selection track record index (bits 23:16)
             *  8   0xNN  Selection track record index (bits 15:8)
             *  9   0xNN  Selection track record index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* PlayCurrentSelection, Table C-34 p.546. */
            L4_NEED(4);
            int paused = !!(audio_status() & AUDIO_STATUS_PAUSE);
            uint32_t index;
            uint32_t trackcount;
            index = get_u32(&L4_PARAM(0));
            trackcount = playlist_amount();
            if (index >= trackcount)
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* Starting from stopped is a start, not a resume.
             *
             * audio_pause(), audio_skip() and audio_resume() all
             * early-return when the Playback Engine is stopped
             * (playback.c's audio_on_pause and audio_on_skip), so from
             * that state this reshuffled the queue, moved nothing and
             * acked Success -- the head unit picked a track, heard
             * silence, and had its play order destroyed on the way.
             * Unreachable until PlayControl Stop began really stopping;
             * PlayControl Play (0x0A) is protocol 1.13, above the 1.12
             * advertised here, so this and SelectDBRecord are the only
             * ways an accessory can start playback at all.
             *
             * playlist_start() is what Rockbox's own browsers use: it
             * sets the index, calls audio_play() and resumes. It also
             * takes a Rockbox index, which is the other half of the
             * fix -- audio_skip(index - ...) mixed the accessory's
             * index space with Rockbox's, and after the shuffle above
             * first_index is guaranteed non-zero, so the two differ. */
            /* Select first, shuffle after. The description above is
             * explicit: "If a track index of n is sent, the nth track
             * of the selected tracks is played first, regardless of
             * where it is located in the Now Playing playlist after the
             * shuffle is performed", and MFi 1.11.2.1.1 (p.58) gives
             * the same rule with a worked example -- "the playlist to
             * be shuffled randomly but with the first track set to the
             * selected index".
             *
             * Reordering first meant n indexed the shuffled queue
             * rather than the one the accessory had enumerated with
             * RetrieveCategorizedDatabaseRecords, so the user picked a
             * row and got a random track -- and got a fresh random
             * order every time, since the seed is current_tick.
             * randomise_playlist_unlocked() takes start_current
             * (apps/playlist.c:1553), which re-finds the playing entry
             * after the swap loop, so selecting first is what keeps the
             * chosen track first.
             *
             * The shuffle-off arm reordered too, and nothing asked it
             * to. With shuffle off the accessory's index already refers
             * to the order it enumerated, and playlist_sort() would
             * throw away a queue the user had shuffled from Rockbox's
             * own UI. */
            long want = iap_track_from_mfi((uint32_t)index);

            if (iap_play_state_byte() == 0x00)
            {
                playlist_start(want, 0, 0);
            }
            else
            {
                audio_pause();
                /* MFi-space delta -- see the note in SelectDBRecord's
                 * Track arm. `want` is only used by the start branch,
                 * which takes a Rockbox index. */
                audio_skip((long)index - iap_get_trackindex());
                if (!paused)
                    audio_resume();
            }

            if (global_settings.playlist_shuffle)
                playlist_randomise(NULL, current_tick, true);
            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0029: /* PlayControl */
        {
            /* PlayControl, Table 5-48 p.429. */
            L4_NEED(1);
            /* The following is the description for the Apple Firmware
             *
             * Sets the new play state of the iPod. The play control command
             * codes are shown below. In response, the iPod sends an ACK
             * telegram indicating the status of the command.
             *
             * Note: If a remote device requests the Next or Previous Chapter
             * for a track that does not contain chapters, the iPod returns a
             * command failed ACK.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x29  Command ID (bits 7:0)
             *  6   0xNN  Play control command code.
             *  7   0xNN  Telegram payload checksum byte
             *
             * Play Control Command Code Protocol version
             * Reserved             0x00 N/A
             * Toggle Play/Pause    0x01 1.00
             * Stop                 0x02 1.00
             * Next Track           0x03 1.00
             * Previous Track       0x04 1.00
             * StartFF              0x05 1.00
             * StartRew             0x06 1.00
             * EndFFRew             0x07 1.00
             * NextChapter          0x08 1.06
             * Previous Chapter     0x09 1.06
             * Reserved             0x0A - 0xFF
             *
             */
            /* Every branch below has to set iap_timeoutbtn. It is the
             * only thing keeping the synthetic press alive:
             * iap_periodic() clears iap_remotebtn the moment it reaches
             * zero, so a press raised without it survives less than one
             * tick and the 2-sample debounce in button_tick() can miss
             * it entirely. Simple Remote already does this
             * (iap-lingo2.c:262); Extended Interface never did.
             *
             * IAP_BTN_TAP is the ~300ms Simple Remote uses for a
             * discrete press. StartFF and StartRew are different: they
             * are held until EndFFRew arrives and are not repeated, so
             * they get a long bound instead -- long enough to seek
             * through a track, short enough that a lost EndFFRew cannot
             * wedge the button.
             */
            /* IAP_BTN_TAP and IAP_BTN_HELD are in iap-core.h now:
             * Display Remote's SetiPodStateInfo starts the same seek
             * through the same helpers. */

            switch(L4_PARAM(0))
            {
                case 0x01: /* play/pause */
                    /* Refused with nothing playing, like every other
                     * arm here. MFi 5.1.37 (p.428): "If the Apple
                     * device does not enter the requested state
                     * successfully, an error status is returned" -- and
                     * it cannot, because there is nothing to toggle
                     * between. Play (0x0A) is the command for starting
                     * from stopped and Table 5-49 puts it at protocol
                     * 1.13, above the 1.12 advertised here, so the
                     * default arm refuses that too.
                     *
                     * Left alone it was worse than a no-op. It raises
                     * BUTTON_RC_PLAY, and keymap-ipod.c:320 binds that
                     * bare to ACTION_STD_OK outside the WPS -- so a
                     * head unit sending its most common command to a
                     * stopped device activated whatever the user had
                     * highlighted in the browser. */
                    /* And the guard was only half the answer. The
                     * button is wrong even when something IS playing:
                     * keymap-ipod.c dispatches by focused context, and
                     * nothing gives the WPS focus merely because audio
                     * is running -- so a head unit's Play/Pause key
                     * activated a browser row on a playing device just
                     * as readily as on a stopped one. Drive the engine,
                     * the way the Stop arm below already does. */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    if (iap_play_state_byte() == 0x02)
                        audio_resume();
                    else
                        audio_pause();
                    break;
                case 0x02: /* stop */
                    /* Table 5-48 (p.429) gives 0x02 as Stop, and MFi
                     * 5.1.37 (p.428): "If the Apple device does not
                     * enter the requested state successfully, an error
                     * status is returned."
                     *
                     * This raised BUTTON_RC_PLAY, and keymap-ipod.c
                     * binds BUTTON_RC_PLAY|BUTTON_REL to
                     * ACTION_WPS_PLAY -- a play/pause toggle. So Stop
                     * on a paused device started playback and was acked
                     * Success, and outside the WPS the same code is
                     * ACTION_STD_OK. Nor did any bit-00 subscriber
                     * hear about it: iap_periodic() reports the stop
                     * only once PLAY and PAUSE are both clear, which a
                     * toggle never makes true.
                     *
                     * It carried BUTTON_REPEAT before that, which the
                     * button driver strips from lastbtn -- so every
                     * tick took the fresh-keypress branch and one
                     * PlayControl(Stop) delivered about 29 toggles.
                     * Dropping the flag made it one toggle. One is
                     * still not a stop.
                     *
                     * audio_stop() is what the Playback Engine offers,
                     * and this file already calls audio_pause() and
                     * audio_resume() directly a thousand lines up. No
                     * TX buffer is open here -- the last one closed at
                     * the cmd_ok() before this case -- so its
                     * queue_send() to the audio thread is safe. */
                    /* End any seek first, so the accessory gets its
                     * seek-stop notification and the button it holds is
                     * released into the stopped UI rather than left
                     * asserted for the rest of IAP_BTN_HELD. */
                    if (device.pb_seeking)
                        iap_seek_stop();

                    audio_stop();
                    if (iap_play_state_byte() != 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    break;
                /* MFi p.429, on PlayControl: "The iPod models before
                 * the 2G nano (09/2006) always return a successful
                 * status. Starting with the 2G nano, Apple devices
                 * return the actual play control status. This means
                 * that a next or previous track command will return an
                 * error if no media is playing."
                 *
                 * This device identifies as a Classic (iap-core.h),
                 * long after that, and returned Success for a skip with
                 * nothing playing -- the button it raised does nothing
                 * in that state, so the head unit was told a track
                 * change had happened and then saw no notification for
                 * one. Table 5-49 groups Next/Previous Track with
                 * Next/Previous (0x08/0x09), so the same holds there.
                 */
                case 0x03: /* skip++ */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    /* audio_next(), not BUTTON_RC_RIGHT: that binds to
                     * ACTION_STD_NEXT outside the WPS
                     * (keymap-ipod.c:318), so the head unit's Next key
                     * moved the user's browser cursor and left the
                     * track alone -- and answered Success. */
                    audio_next();
                    break;
                case 0x04: /* skip-- */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    /* audio_prev(), for the same reason: keymap-ipod.c:317
                     * gives BUTTON_RC_LEFT to ACTION_STD_PREV outside
                     * the WPS. */
                    audio_prev();
                    break;
                case 0x08: /* next */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    if (!iap_skip_chapter(1))
                        audio_next();
                    break;
                case 0x09: /* previous */
                    if (iap_play_state_byte() == 0x00) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    if (!iap_skip_chapter(-1))
                        audio_prev();
                    break;
                case 0x05: /* ffwd */
                case 0x06: /* frwd */
                    /* Refused with nothing playing, like the skip arms
                     * above -- see the note on iap_seek_start(). */
                    if (!iap_seek_start(L4_PARAM(0) == 0x05)) {
                        cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                        return;
                    }
                    break;
                case 0x07: /* end ffwd/frwd */
                    iap_seek_stop();
                    break;
                default:
                    /* Table 5-49 reserves 0x00 and 0x0F-0xFF, and puts
                     * Play (0x0A), Pause (0x0B), the chapter controls
                     * (0x0C, 0x0D) and Resume iPod (0x0E) at protocol
                     * 1.13 or 1.14, above the 1.12 advertised here. The
                     * 1.13 pair may be used below that version only
                     * "if the Apple device explicitly declares their
                     * support; see Table 3-132 (page 192)", and the
                     * Extended Interface option bit that would declare
                     * them is left clear.
                     *
                     * MFi 5.1.37 (p.428): "If the Apple device does not
                     * enter the requested state successfully, an error
                     * status is returned." Falling out of the switch
                     * and acking Success said the opposite. */
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    return;
            }

            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x002A: /* GetTrackArtworkTimes */
            /* The following is the description for the Apple Firmware
             *
             * The device sends this command to the iPod to request the list of
             * artwork time locations for a track. A 4-byte track Index
             * specifies which track from the Playback Engine is to be selected.
             * A 2-byte formatID indicates which type of artwork is desired.
             * The 2-byte artworkIndex specifies at which index to begin
             * searching for artwork. A value of 0 indicates that the iPod
             * should start with the first available artwork.
             * The 2-byte artworkCount specifies the maximum number of times
             * (artwork locations) to be returned. A value of -1 (0xFFFF)
             * indicates that there is no preferred limit. Note that podcasts
             * may have a large number of associated images.
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x0D  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2A  Command ID (bits 7:0)
             *  6   0xNN  trackIndex(31:24)
             *  7   0xNN  trackIndex(23:16)
             *  8   0xNN  trackIndex (15:8)
             *  9   0xNN  trackIndex (7:0)
             * 10   0xNN  formatID (15:8)
             * 11   0xNN  formatID (7:0)
             * 12   0xNN  artworkIndex (15:8)
             * 13   0xNN  artworkIndex (7:0)
             * 14   0xNN  artworkCount (15:8)
             * 15   0xNN  artworkCount (7:0)
             * 16   0xNN  Checksum
             *
             */
        {
            struct mp3entry id3;
            uint32_t track_index;
            uint16_t artwork_index;
            uint16_t artwork_count;
            bool has_artwork;

            L4_NEED(10);
            track_index = get_u32(&L4_PARAM(0));
            artwork_index = get_u16(&L4_PARAM(6));
            artwork_count = get_u16(&L4_PARAM(8));

            if (get_u16(&L4_PARAM(4)) != IAP_ARTWORK_FORMAT_ID
                || !iap_get_trackinfo(track_index, &id3)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            has_artwork = iap_artwork_available(&id3);

            IAP_TX_INIT4(0x04, 0x002B);
            L4_TX_TRANSID();
            if (artwork_index == 0 && artwork_count != 0 && has_artwork)
                IAP_TX_PUT_U32(0);
            iap_send_tx();
            break;
        }
        case 0x002B: /* ReturnTrackArtworkTimes. See Above */
            /* The following is the description for the Apple Firmware
             *
             * The iPod sends this command to the device to return the list of
             * artwork times for a given track. The iPod returns zero or more
             * 4-byte times, one for each piece of artwork associated with the
             * track and format specified by GetTrackArtworkTimes.
             * The number of records returned will be no greater than the number
             * specified in the GetTrackArtworkTimes command. It may however, be
             * less than requested. This can happen if there are fewer pieces of
             * artwork available than were requested, or if the iPod is unable
             * to  place the full number in a single packet. Check the number of
             * records returned against the results of
             * RetIndexedPlayingTrackInfo with infoType 7 to ensure that all
             * artwork has been received.
             *
             * Byte Value Comment
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Length of packet
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2B  Command ID (bits 7:0)
             *  6   0xNN  time offset from track start in ms (31:24)
             *  7   0xNN  time offset from track start in ms (23:16)
             *  8   0xNN  time offset from track start in ms (15:8)
             *  9   0xNN  time offset from track start in ms (7:0)
             *  Preceding 4 bytes may be repeated NN times
             * NN   0xNN  Checksum
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x002C: /* GetShuffle */
        {
            /* The following is the description for the Apple Firmware
             *
             * Requests the current state of the iPod shuffle setting. The iPod
             * responds with the Command0x002D: ReturnShuffle telegram.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2C  Command ID (bits 7:0)
             *  6   0xCD  Telegram payload checksum byte
             *
             */
            unsigned char data[] = {0x04, 0x00, 0x2D,
                                    0x00};
            data[3] = global_settings.playlist_shuffle ? 1 : 0;
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x002D: /* ReturnShuffle. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the current state of the shuffle setting. The iPod sends
             * this telegram in response to the Command 0x002C: GetShuffle
             * telegram from the device.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2D  Command ID (bits 7:0)
             *  6   0xNN  Shuffle mode. See Below.
             *  7   0xNN  Telegram payload checksum byte
             *
             * Possible values of the shufflemode.
             * Value Meaning
             * 0x00  Shuffle off
             * 0x01  Shuffle tracks
             * 0x02  Shuffle albums
             * 0x03 – 0xFF Reserved
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x002E: /* SetShuffle */
        {
            /* SetShuffle, Table 5-55 p.433. */
            L4_NEED(1);
            /* The following is the description for the Apple Firmware
             *
             * Sets the iPod shuffle mode. The iPod shuffle modes are listed
             * below. In response, the iPod sends an ACK telegram with the
             * command status.
             * This telegram has an optional byte, byte 0x07, called the
             * RestoreonExit byte. This byte can be used to restore the
             * original shuffle setting in use when the accessory was attached
             * to the iPod. A non zero value restores the original shuffle
             * setting of the iPod when the accessory is detached. If this byte
             * is zero, the shuffle setting set by the accessory overwrites the
             * original setting and persists after the accessory is detached
             * from the iPod.
             * Accessory engineers should note that the shuffle mode affects
             * items only in the playback engine. The shuffle setting does not
             * affect the order of tracks in the database engine, so calling
             * Command 0x001A: RetrieveCategorizedDatabaseRecords on a database
             * selection with the shuffle mode set returns a list of unshuffled
             * tracks. To get the shuffled playlist, an accessory must query the
             * playback engine by calling Command 0x0020
             * GetIndexedPlayingTrackTitle.
             * Shuffling tracks does not affect the track index, just the track
             * at that index. If an unshuffled track at playback index 1 is
             * shuffled, a new track is placed into index 1. The playback
             * indexes themselves are not shuffled.
             * When shuffle mode is enabled, tracks that are marked 'skip when
             * shuffling' are filtered from the database selection. This affects
             * all audiobooks and all tracks that the user has marked in iTunes.
             * It also affects all podcasts unless their default 'skip when
             * shuffling' markings have been deliberately removed. To apply the
             * filter to the playback engine, the accessory should send
             * Command 0x0017: SelectDBRecord or
             * Command 0x0028: PlayCurrentSelection after enabling shuffle mode.
             *
             * Note: Accessory developers are encouraged to always use the
             * Restore on Exit byte with a nonzero value to restore any settings
             * modified by the accessory upon detach.
             *
             * SetShuffle telegram with Restore on Exit byte
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x05  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2E  Command ID (bits 7:0)
             *  6   0xNN  New shuffle mode. See above .
             *  7   0xNN  Restore on Exit byte. If 1, the orig setting is
             *            restored on detach; if 0, the newsetting persists
             *            after accessory detach.
             *  8   0xNN  Telegram payload checksum byte
             *
             * SetShuffle setting persistent after the accessory detach.
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2E  Command ID (bits 7:0)
             *  6   0xNN  New shuffle mode. See above.
             *  7   0xNN  Telegram payload checksum byte
             *
             */
            /* MFi Table 5-53 (p.432): 0x00 shuffle off, 0x01 shuffle
             * tracks, 0x02 shuffle albums, "0x03 - 0xFF Reserved".
             * Every non-zero byte used to mean "shuffle tracks on",
             * reserved values included, and was acknowledged Success.
             * Album shuffle is not implemented here either, so it is
             * refused rather than silently treated as track shuffle.
             * The sibling Display Remote handler already answers Bad
             * Parameter for the same values on the same setting. */
            if (L4_PARAM(0) > 0x01)
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* Table 5-55 (p.433) puts Restore on Exit in the second
             * parameter. The one-byte form (Table 5-54) has no such
             * field, which is the same as sending zero. The latch lives
             * with the setting in iap-core.c, so Display Remote's
             * SetiPodStateInfo gets it from the same place. */
            iap_shuffle_state(L4_PARAM(0) != 0,
                              (L4_HAVE(2) && L4_PARAM(1))
                              ? IAP_RESTORE_YES : IAP_RESTORE_NO);

            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x002F: /* GetRepeat */
        {
            /* The following is the description for the Apple Firmware
             *
             * Requests the track repeat state of the iPod. In response, the
             * iPod sends a Command 0x0030: ReturnRepeat  telegram
             * to the device.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x2F  Command ID (bits 7:0)
             *  6   0xCA  Telegram payload checksum byte
             *
             */
            unsigned char data[] = {0x04, 0x00, 0x30, 0x00};
            if(global_settings.repeat_mode == REPEAT_OFF)
                data[3] = 0;
            else if(global_settings.repeat_mode == REPEAT_ONE)
                data[3] = 1;
            else
                data[3] = 2;
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x0030: /* ReturnRepeat. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the current iPod track repeat state to the device.
             * The iPod sends this telegram in response to the Command
             * 0x002F:GetRepeat command.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x30  Command ID (bits 7:0)
             *  6   0xNN  Repeat state. See Below.
             *  7   0xNN  Telegram payload checksum byte
             *
             * Repeat state values
             * Value Meaning
             *  0x00 Repeat off
             *  0x01 Repeat one track
             *  0x02 Repeat all tracks
             *  0x03 - 0xFF Reserved
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0031: /* SetRepeat */
        {
            /* SetRepeat, Table 5-60 p.435. */
            L4_NEED(1);
            /* The following is the description for the Apple Firmware
             *
             * Sets the repeat state of the iPod. The iPod track repeat modes
             * are listed above. In response, the iPod sends an ACK telegram
             * with the command status.
             *
             * This telegram has an optional byte, byte 0x07, called the
             * RestoreonExitbyte. This byte can be used to restore the original
             * repeat setting in use when the accessory was attached to the
             * iPod. A nonzero value restores the original repeat setting of
             * the iPod when the accessory is detached. If this byte is zero,
             * the repeat setting set by the accessory overwrites the original
             * setting and persists after the accessory is detached from the
             * iPod.
             *
             * Note: Accessory developers are encouraged to always use the
             * Restore on Exit byte with a nonzero value to restore any
             * settings modified by the accessory upon detach.
             *
             * SetRepeat telegram with Restore on Exit byte
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x05  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x31  Command ID (bits 7:0)
             *  6   0xNN  New repeat state. See above.
             *  7   0xNN  Restore on Exit byte. If 1, the original setting is
             *            restored on detach; if 0, the newsetting persists
             *            after accessory detach.
             *  8   0xNN  Telegram payload checksum byte
             *
             * SetRepeat setting persistent after the accessory detach.
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x31  Command ID (bits 7:0)
             *  6   0xNN  New repeat state. See above.
             *  7   0xNN  Telegram payload checksum byte
             *
             */
            /* MFi Table 5-59 (p.434): 0x00 repeat off, 0x01 repeat one
             * track, 0x02 repeat all, "0x03 - 0xFF Reserved". A
             * reserved value left repeat_mode untouched and was then
             * acknowledged Success, so a following GetRepeat reported
             * the old state -- the same shape as the missing default in
             * PlayControl fixed by 39b9017164. */
            if (L4_PARAM(0) > 0x02)
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            /* Table 5-60 (p.434) puts Restore on Exit in the second
             * parameter, as SetShuffle does. */
            iap_repeat_state(L4_PARAM(0) == 0 ? REPEAT_OFF :
                             L4_PARAM(0) == 1 ? REPEAT_ONE : REPEAT_ALL,
                             (L4_HAVE(2) && L4_PARAM(1))
                              ? IAP_RESTORE_YES : IAP_RESTORE_NO);

            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0032: /* SetDisplayImage */
        {
            /* The following is the description for the Apple Firmware
             * This sets a bitmap image
             * that is displayed on the iPod display when connected to the
             * device. It replaces the default checkmark bitmap image that is
             * displayed when iPod is connected to an external device
             *
             * Sets a bitmap image that is shown on the iPod display when it is
             * connected to the device. The intent is to allow third party
             * branding when the iPod is communicating with an external device.
             * An image downloaded using this mechanism replaces the default
             * checkmark bitmap image that is displayed when iPod is connected
             * to an external device. The new bitmap is retained in RAM for as
             * long as the iPod remains powered. After a system reset or deep
             * sleep state, the new bitmap is lost and the checkmark image
             * restored as the default when the iPod next enters Extended
             * Interface mode after power-up.
             * Before setting a monochrome display image, the device can send
             * the Command 0x0033: GetMonoDisplayImageLimits telegram to obtain
             * the current iPod display width, height and pixel format.The
             * monochrome display information returned in the Command 0x0034:
             * ReturnMonoDisplayImageLimits telegram can be useful to the
             * device in deciding which type of display image format is suitable
             * for downloading to the iPod.
             * On iPods withcolor displays, devices can send the Command 0x0039:
             * GetColorDisplayImageLimits telegram to obtain the iPod color
             * display width,height,and pixel formats. The color display
             * information is returned in the Command 0x003A:
             * ReturnColorDisplayImageLimits” telegram.
             * To set a display image, the device must successfully send
             * SetDisplayImage descriptor and data telegrams to the iPod. The
             * SetDisplayImage descriptor telegram (telegram index 0x0000) must
             * be sent first, as it gives the iPod a description of the image
             * to be downloaded. This telegram is shown in below. The image
             * descriptor telegram includes image pixel format, image width and
             * height, and display row size (stride) in bytes. Optionally, the
             * descriptor telegram may also contain the beginning data of the
             * display image, as long as it remains within the maximum length
             * limits of the telegram.
             * Following the descriptor telegram, the SetDisplayImage data
             * telegrams (telegram index 0x0001 - 0xNNNN) should be sent using
             * sequential telegram indices until the entire image has been sent
             * to the iPod.
             *
             * Note: The SetDisplayImage telegram payload length is limited to
             * 500 bytes. This telegram length limit and the size and format of
             * the display image generally determine the minimum number of
             * telegrams that are required to set a display image.
             *
             * Note: Starting with the second generation iPod nano with version
             * 1.1.2 firmware, the use of the SetDisplayImage command is
             * limited to once every 15 seconds over USB transport. The iPod
             * classic and iPod 3G nano apply this restriction to both USB and
             * UART transports. Calls made to SetDisplayImage more frequently
             * than every 15 seconds will return a successful ACK command, but
             * the bitmap will not be displayed on the iPod’s screen. Hence use
             * of the SetDisplayImage command should be limited to drawing one
             * bitmap image per accessory connect. The iPod touch will accept
             * the SetDisplayImage command but will not draw it on the iPod’s
             * screen.
             *
             * Below shows the format of a descriptor telegram. This example
             * assumes the display image descriptor data exceeds the small
             * telegram payload capacity; a large telegram format is shown.
             *
             * SetDisplayImage descriptor telegram (telegram index = 0x0000)
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x00  Telegram payload marker (large format)
             *  3   0xNN  Large telegram payload length (bits 15:8)
             *  4   0xNN  Large telegram payload length (bits 7:0)
             *  5   0x04  Lingo ID: Extended Interface lingo
             *  6   0x00  Command ID (bits 15:8)
             *  7   0x32  Command ID (bits 7:0)
             *  8   0x00  Descriptor telegram index (bits 15:8). These fields
             *            uniquely identify each packet in the SetDisplayImage
             *            transaction. The first telegram is the Descriptor
             *            telegram and always starts with an index of 0x0000.
             *  9   0x00  Descriptor telegram index (bits 7:0)
             * 10   0xNN  Display pixel format code. See Below.
             * 11   0xNN  Imagewidth in pixels (bits 15:8). The number of
             *            pixels, from left to right, per row.
             * 12   0xNN  Image width in pixels (bits 7:0)
             * 13   0xNN  Image height in pixels (bits 15:8). The number of
             *            rows, from top to bottom, in the image.
             * 14   0xNN  Image height in pixels (bits 7:0)
             * 15   0xNN  Row size (stride) in bytes (bits 31:24). The number of
             *            bytes representing one row of pixels. Each row is
             *            zero-padded to end on a 32-bit boundary. The
             *            cumulative size, in bytes, of the image data,
             *            transferred across all telegrams in this transaction
             *            is effectively (Row Size * Image Height).
             * 16   0xNN  Row size (stride) in bytes (bits 23:16)
             * 17   0xNN  Row size (stride) in bytes (bits 15:8)
             * 18   0xNN  Row size (stride) in bytes (bits 7:0)
             * 19–N 0xNN  Display image pixel data
             * NN   0xNN  Telegram payload checksum byte
             *
             * SetDisplayImage data telegram (telegram index = 0x0001 - 0xNNNN)
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x00  Telegram payload marker (large format)
             *  3   0xNN  Large telegram payload length (bits 15:8)
             *  4   0xNN  Large telegram payload length (bits 7:0)
             *  5   0x04  Lingo ID: Extended Interface lingo
             *  6   0x00  Command ID (bits 15:8)
             *  7   0x32  Command ID (bits 7:0)
             *  8   0xNN  Descriptor telegram index (bits 15:8). These fields
             *            uniquely identify each packet in the SetDisplayImage
             *            transaction. The first telegram is the descriptor
             *            telegram, shown in Table 6-68 (page 97). The
             *            remaining n-1 telegrams are simply data telegrams,
             *            where n is determined by the size of the image.
             *  9   0xNN  Descriptor telegram index (bits 7:0)
             * 10–N 0xNN  Display image pixel data
             * NN   0xNN  Telegram payload checksum byte
             *
             * Note: A known issue causes SetDisplayImage data telegram
             * lengths less than 11 bytes to return a bad parameter error (0x04)
             * ACK on 3G iPods.
             *
             * The iPod display is oriented as a rectangular grid of pixels. In
             * the horizontal direction (x-coordinate), the pixel columns are
             * numbered, left to right, from 0 to Cmax. In the vertical
             * direction (y-coordinate), the pixel rows are numbered, top to
             * bottom, from 0 to Rmax. Therefore, an (x,y) coordinate of (0,0)
             * represents the upper-leftmost pixel on the display and
             * (Cmax,Rmax) represents the lower-rightmost pixel on the display.
             * A portion  of the iPod display pixel layout is shown below, where
             * x is the column number, y is the row number, and (Cmax,Rmax)
             * represents the maximum row and column numbers.
             *
             * Pixel layout
             * x
             * y 0,0 1,0 2,0 3,0 4,0 5,0 6,0 7,0 - Cmax,0
             *   0,1 1,1 2,1 3,1 4,1 5,1 6,1 7,1 - Cmax,1
             *   0,2 1,2 2,2 3,2 4,2 5,2 6,2 7,2 - Cmax,2
             *   0,3 1,3 2,3 3,3 4,3 5,3 6,3 7,3 - Cmax,3
             *   0,4 1,4 2,4 3,4 4,4 5,4 6,4 7,4 - Cmax,4
             *   0,5 1,5 2,5 3,5 4,5 5,5 6,5 7,5 - Cmax,5
             *   0,6 1,6 2,6 3,6 4,6 5,6 6,6 7,6 - Cmax,6
             *   0,7 1,7 2,7 3,7 4,7 5,7 6,7 7,7 - Cmax,7
             *    "   "   "   "   "   "   "   "     " "
             *   0,  1,  2,  3,  4,  5,  6,  7,  - Cmax,
             *   RmaxRmaxRmaxRmaxRmaxRmaxRmaxRmax  Rmax
             *
             * Display pixel format codes
             * Display pixel format                 Code Protocol version
             * Reserved                             0x00 N/A
             * Monochrome, 2 bits per pixel         0x01 1.01
             * RGB 565 color, little-endian, 16 bpp 0x02 1.09
             * RGB 565 color, big-endian, 16 bpp    0x03 1.09
             * Reserved                             0x04-0xFF N/A
             *
             * iPods with color screens support all three image formats. All
             * other iPods support only display pixel format 0x01 (monochrome,
             * 2 bpp).
             *
             * Display Pixel Format 0x01
             * Display pixel format 0x01 (monochrome, 2 bits per pixel) is the
             * pixel format supported by all iPods. Each pixel consists of 2
             * bits that control the pixel intensity. The pixel intensities and
             * associated binary codes are listed below.
             *
             * 2 bpp monochrome pixel intensities
             * Pixel Intensity           Binary Code
             * Pixel off (not visible)   00b
             * Pixel on 25% (light grey) 01b
             * Pixel on 50% (dark grey)  10b
             * Pixel on 100% (black)     11b
             *
             * Each byte of image data contains four packed pixels. The pixel
             * ordering within bytes and the byte ordering within 32 bits is
             * shown below.
             *
             * Image Data Byte 0x0000
             * Pixel0 Pixel1 Pixel2 Pixel3
             *  7 6    5 4    3 2    1 0
             *
             * Image Data Byte 0x0001
             * Pixel4 Pixel5 Pixel6 Pixel7
             *  7 6    5 4    3 2    1 0
             *
             * Image Data Byte 0x0002
             * Pixel8 Pixel9 Pixel10 Pixel11
             *  7 6    5 4    3 2    1 0
             *
             * Image Data Byte 0x0003
             * Pixel12 Pixel13 Pixel14 Pixel15
             *  7 6      5 4    3 2     1 0
             *
             * Image Data Byte 0xNNNN
             * PixelN PixelN+1 PixelN+2 PixelN+3
             *  7 6     5 4     3 2      1 0
             *
             * Display Pixel Formats 0x02 and 0x03
             * Display pixel format 0x02 (RGB 565, little-endian) and display
             * pixel format 0x03 (RGB 565, big-endian) are available for use
             * in all iPods with color screens. Each pixel consists of 16 bits
             * that control the pixel intensity for the colors red, green, and
             * blue.
             * It takes two bytes to represent a single pixel. Red is
             * represented by 5 bits, green is represented by 6 bits, and blue
             * by the final 5 bits. A 32-bit sequence represents 2 pixels. The
             * pixel ordering within bytes and the byte ordering within 32 bits
             * for display format 0x02 (RGB 565, little-endian) is shown below.
             *
             * Image Data Byte 0x0000
             * Pixel 0, lower 3 bits of green Pixel 0, all 5 bits of blue
             * 7 6 5                                   4 3 2 1 0
             * Image Data Byte 0x0001
             * Pixel 0, all 5 bits of red     Pixel 0,upper 3 bits of green
             * 7 6 5 4 3                               2 1 0
             *
             * Image Data Byte 0x0002
             * Pixel 1, lower 3 bits of green Pixel 1, all 5 bits of blue
             * 7 6 5                                   4 3 2 1 0
             *
             * Image Data Byte 0x0003
             * Pixel 1, all 5 bits of red     Pixel 1, upper 3 bits of green
             * 7 6 5 4 3                               2 1 0
             *
             * The format for display pixel format 0x03 (RGB 565, big-endian, 16
             * bpp) is almost identical, with the exception that bytes 0 and 1
             * are swapped and bytes 2 and 3 are swapped.
             *
             */
            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }
        case 0x0033: /* GetMonoDisplayImageLimits */

        {
            /* The following is the description for the Apple Firmware
             *
             * Requests the limiting characteristics of the monochrome image
             * that can be sent to the iPod for display while it is connected
             * to the device. It can be used to determine the display pixel
             * format and maximum width and height of a monochrome image to be
             * set using the Command 0x0032: SetDisplayImage telegram. In
             * response, the iPod sends a Command 0x0034:
             * ReturnMonoDisplayImageLimits telegram to the device with the
             * requested display information. The GetMonoDisplayImageLimits
             * command is supported by iPods with either monochrome or color
             * displays. To obtain color display image limits, use Command
             * 0x0039: GetColorDisplayImageLimits.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x33  Command ID (bits 7:0)
             *  6   0xC6  Telegram payload checksum byte
             *
             */
            unsigned char data[] = {0x04, 0x00, 0x34,
                                    LCD_WIDTH >> 8, LCD_WIDTH & 0xff,
                                    LCD_HEIGHT >> 8, LCD_HEIGHT & 0xff,
                                    0x01};
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x0034: /* ReturnMonoDisplayImageLimits. See Above*/
            /* The following is the description for the Apple Firmware
             *
             * Returns the limiting characteristics of the monochrome image that
             * can be sent to the iPod for display while it is connected to the
             * device. The iPod sends this telegram in response to the Command
             * 0x0033: GetMonoDisplayImageLimits telegram. Monochrome display
             * characteristics include maximum image width and height and the
             * display pixel format.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x34  Command ID (bits 7:0)
             *  6   0xNN  Maximum image width in pixels (bits 15:8)
             *  7   0xNN  Maximum image width in pixels (bits 7:0)
             *  8   0xNN  Maximum image height in pixels (bits 15:8)
             *  9   0xNN  Maximumimage height in pixels (bits 7:0)
             * 10   0xNN  Display pixel format (see Table 6-70 (page 99)).
             * 11   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0035: /* GetNumPlayingTracks */
        {
            /* The following is the description for the Apple Firmware
             *
             * Requests the number of tracks in the list of tracks queued to
             * play on the iPod. In response, the iPod sends a Command 0x0036:
             * ReturnNumPlayingTracks telegram with the count of tracks queued
             * to play.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x35  Command ID (bits 7:0)
             *  6   0xC4  Telegram payload checksum byte
             *
             */
            unsigned char data[] = {0x04, 0x00, 0x36,
                                    0x00, 0x00, 0x00, 0x00};
            unsigned long playlist_amt = playlist_amount();
            put_u32(&data[3], playlist_amt);
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x0036: /* ReturnNumPlayingTracks. See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the number of tracks in the actual list of tracks queued
             * to play, including the currently playing track (if any). The
             * iPod sends this telegram in response to the Command 0x0035:
             * GetNumPlayingTracks telegram.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x36  Command ID (bits 7:0)
             *  6   0xNN  Number of tracks playing(bits 31:24)
             *  7   0xNN  Number of tracks playing(bits 23:16)
             *  8   0xNN  Number of tracks playing (bits 15:8)
             *  9   0xNN  Number of tracks playing (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x0037: /* SetCurrentPlayingTrack */
            /* The following is the description for the Apple Firmware
             *
             * Sets the index of the track to play in the Now Playing playlist
             * on the iPod. The index that is specified here is obtained by
             * sending the Command 0x0035: GetNumPlayingTracks and Command
             * 0x001E: GetCurrentPlayingTrackIndex telegrams to obtain the
             * number of playing tracks and the current playing track index,
             * respectively. In response, the iPod sends an ACK telegram
             * indicating the status of the command.
             *
             * Note: The behavior of this command has changed. Before the
             * 2G nano, if this command was sent with the current playing track
             * index the iPod would pause playback momentarily and then resume.
             * Starting with the 2G nano, the iPod restarts playback of the
             * current track from the beginning. Older iPods will not be
             * updated to the new behavior.
             *
             * Note: This command is usable only when the iPod is in a playing
             * or paused state. If the iPod is stopped, this command fails.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x07  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x37  Command ID (bits 7:0)
             *  6   0xNN  New current playing track index (bits 31:24)
             *  7   0xNN  New current playing track index (bits 23:16)
             *  8   0xNN  New current playing track index (bits 15:8)
             *  9   0xNN  New current playing track index (bits 7:0)
             * 10   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* SetCurrentPlayingTrack, Table 5-70 p.442. */
            L4_NEED(4);

            /* MFi 5.1.51 (p.442): "This command is usable only when the
             * Apple device is in a playing or paused state. If the
             * Apple device is stopped, this command fails."
             *
             * It was acked Success while stopped, and playback.c's skip
             * entry points early-return in that state -- so the skip
             * was a no-op and the head unit's display moved to a track
             * the device had not selected and would not report. */
            if (iap_play_state_byte() == 0x00)
            {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }

            int paused = !!(audio_status() & AUDIO_STATUS_PAUSE);
            long tracknum = get_u32(&L4_PARAM(0));

            /* audio_skip() backs an out-of-range offset out one track at
             * a time, holding id3_mutex and never yielding, so a wild
             * index locks the whole device up rather than being
             * rejected -- an index of 0x40000000 is on the order of 1e9
             * iterations, i.e. minutes. Range-check before handing it
             * over.
             */
            if (tracknum < 0 || tracknum >= (long)playlist_amount())
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            /* The index arrives in the accessory's space, and MFi
             * 5.1.51 (p.442) ties that space to one command: "The index
             * that is specified here is obtained by sending ...
             * GetCurrentPlayingTrackIndex". 0x001E answers that by
             * subtracting playlist_get_first_index(), so this has to
             * add it back before comparing against playlist_next(0),
             * which is a Rockbox index. Every other command that takes
             * an index converts; this one did not.
             *
             * first_index is non-zero in ordinary use --
             * apps/playlist.c:1507 sets index = first_index = i when
             * the queue is randomised or sorted -- so with shuffle on
             * and playback started from track 17, the accessory read
             * index 0 for the current track, echoed it straight back,
             * and playback jumped seventeen tracks. p.442 says that
             * case should "pause playback momentarily and then
             * resume". */
            /* An MFi-space delta -- see the note in SelectDBRecord's
             * Track arm. iap_get_trackindex(), not playlist_next(0):
             * that one blocks on a queue_send() to the playlist thread
             * and mutates playlist->index on the way. */
            audio_pause();
            audio_skip((long)tracknum - iap_get_trackindex());
            if (!paused)
                audio_resume();

            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0038: /* SelectSortDBRecord */
            /* The following is the description for the Apple Firmware
             *
             * Selects one or more records in the iPod database, based on a
             * category-relative index. For example, selecting category 2
             * (Artist), record index 1, and sort order 3 (Album) results in a
             * list of selected tracks (records) from the second artist in the
             * artist list, sorted by album name. Selections are additive and
             * limited by the category hierarchy. Subsequent selections are
             * made based on the subset of records resulting from previous
             * selections and not from the entire database. The database
             * category types are shown above. The sort order options and codes
             * are shown below.
             *
             * SelectSortDBRecord telegram
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x09  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x38  Command ID (bits 7:0)
             *  6   0xNN  Database category type.
             *  7   0xNN  Category record index (bits 31:24)
             *  8   0xNN  Category record index (bits 23:16)
             *  9   0xNN  Category record index (bits 15:8)
             * 10   0xNN  Category record index (bits 7:0)
             * 11   0xNN  Database sort type.
             * 12   0xNN  Telegram payload checksum byte
             *
             * Database sort order options
             * Sort Order Code Protocol version
             * Sort by genre         0x00 1.00
             * Sort by artist        0x01 1.00
             * Sort by composer      0x02 1.00
             * Sort by album         0x03 1.00
             * Sort by name          0x04 1.00
             * Sort by playlist      0x05 1.00
             * Sort by release date  0x06 1.08
             * Reserved              0x07 - 0xFE N/A
             * Use default sort type 0xFF 1.00
             *
             * The default order of song and audiobook tracks on the iPod is
             * alphabetical by artist, then alphabetical by that artist's
             * albums, then ordered according to the order of the tracks on the
             * album.
             * For podcasts, the default order of episode tracks is reverse
             * chronological. That is, the newest ones are first,then
             * alphabetical by podcast name.
             * The SelectSortDBRecord command can be used to sort all the song
             * and audiobook tracks on the iPod alphabetically as follows:
             * 1. Command 0x0016: ResetDBSelection
             * 2. Command 0x0018: GetNumberCategorizedDBRecords for the Playlist
             *                    category.
             * 3. SelectSortDBRecord based on the Playlist category, using a
             *                    record index of 0 to select the All Tracks
             *                    playlist and the sort by name (0x04) sort
             *                    order.
             * 4. GetNumberCategorizedDBRecords for the Track category.
             * 5. Command 0x001A :RetrieveCategorizedDatabaseRecords based on
             *                    the Track category, using a start index of 0
             *                    and an end index of the number of records
             *                    returned by the call to
             *                    GetNumberCategorizedDBRecords in step 4.
             *
             * The sort order of artist names ignores certain articles such
             * that the artist “The Doors” is sorted under the letter ‘D’ and
             * not ‘T’; this matches the behavior of iTunes. The sort order is
             * different depending on the language setting used in the iPod.
             * The list of ignored articles may change in the future without
             * notice.
             * The SelectDBRecord command may also be used to select a database
             * record with the default sort order.
             *
             * Note: The sort order field is ignored for the Audiobook category.
             * Audiobooks are automatically sorted by track title. The sort
             * order for podcast tracks defaults to release date, with the
             * newest track coming first.
             *
             * Selects one or more records in the iPod database, based on a
             * category-relative index. This appears to be hardcoded hierarchy
             * decided by Apple that the external devices follow.
             * This is as follows for all except podcasts,
             *
             * All (highest level),
             * Playlist,
             * Genre or Media Kind,
             * Artist or Composer,
             * Album,
             * Track or Audiobook (lowest)
             *
             * for Podcasts, the order is
             *
             * All (highest),
             * Podcast,
             * Episode
             * Track (lowest)
             *
             * Categories are
             *
             * 0x00 Reserved
             * 0x01 Playlist
             * 0x02 Artist
             * 0x03 Album
             * 0x04 Genre
             * 0x05 Track
             * 0x06 Composer
             * 0x07 Audiobook
             * 0x08 Podcast
             * 0x09 - 0xff Reserved
             *
             * Sort Order optiona and codes are
             *
             * 0x00 Sort by Genre
             * 0x01 Sort by Artist
             * 0x02 Sort by Composer
             * 0x03 Sort by Album
             * 0x04 Sort by Name (Song Title)
             * 0x05 Sort by Playlist
             * 0x06 Sort by Release Date
             * 0x07 - 0xfe Reserved
             * 0xff Use default Sort Type
             *
             * Packet format (offset in data[]: Description)
             * 0x00: Lingo ID: Extended Interface Protocol Lingo, always 0x04
             * 0x01-0x02: Command, always 0x0038
             * 0x03: Database Category Type
             * 0x04-0x07:  Category Record Index
             * 0x08 Database Sort Type
             *
             * On Rockbox, if the recordtype is playlist, we load the selected
             * playlist and start playing from the first track.
             * If the recordtype is track, we play that track from the current
             * playlist.
             * On anything else we just play the current track from the current
             * playlist.
             * cur_dbrecord[0] is the recordtype
             * cur_dbrecord[1-4] is the u32 of the record number requested
             * which might be a playlist or a track number depending on
             * the value of cur_dbrecord[0]
             */
        {
            /* SelectSortDBRecord, Table C-35 p.546. */
            /* MFi Table C-35 (p.546): category type (1), record index
             * (4), sort type (1) -- six payload bytes, so nine with the
             * lingo and the two-byte command. The check said eight,
             * carried over from 0x0017's five-byte payload, so a
             * command one byte short of its sort type was accepted,
             * sorted the playlist, skipped, and was acknowledged
             * Success. */
            L4_NEED(6);
            memcpy(cur_dbrecord, &L4_PARAM(0), 5);

            int paused = !!(audio_status() & AUDIO_STATUS_PAUSE);
            unsigned int number_of_playlists = nbr_total_playlists();
            uint32_t index;
            uint32_t trackcount;
            index = get_u32(&cur_dbrecord[1]);
            trackcount = playlist_amount();
            /* MFi C.7.6 (p.546): "This command acts the same as
             * Command 0x0017: SelectDBRecord (page 415), but includes a
             * sorting feature." So the same refusals apply, and 5.1.20
             * (p.416) spells them out: "The Apple device also returns a
             * bad parameter error iPodAck when accessories send the
             * SelectDBRecord command with an invalid category type, or
             * with the Track category and an index greater than the
             * total number of tracks available on the Apple device."
             *
             * The bound was > where the sibling 0x0017 uses >=, so
             * index 40 on a 40-track queue was accepted here and
             * refused there -- the same device answering two ways about
             * the same track. */
            if ((cur_dbrecord[0] == 0x05) && (index >= trackcount))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            if ((cur_dbrecord[0] == 0x01) && (index >= (number_of_playlists + 1)))
            {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }
            switch (cur_dbrecord[0])
            {
                case 0x01: /* Playlist*/
                {
                    if (index != 0x00) /* 0x00 is the On-The-Go Playlist and
                                          we do nothing with it  */
                    {
                        audio_pause();
                        audio_skip(-iap_get_trackindex());
                        playlist_sort(NULL, true);
                        last_selected_playlist = index;
                        if (!seek_to_playlist(last_selected_playlist)) {
                            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                            return;
                        }
                    }
                    break;
                }
                case 0x02: /* Artist   Do Nothing  */
                case 0x03: /* Album    Do Nothing  */
                case 0x04: /* Genre    Do Nothing  */
                case 0x06: /* Composer Do Nothing  */
                    break;
                case 0x05: /* Track*/
                {
                    /* MFi-space deltas, both of them -- see the note in
                     * SelectDBRecord's Track arm. The rewind to the
                     * front is -iap_get_trackindex() for the same
                     * reason: it is the accessory's distance back to
                     * its own index 0. */
                    audio_pause();
                    audio_skip(-iap_get_trackindex());
                    playlist_sort(NULL, true);
                    audio_skip((long)index - iap_get_trackindex());
                    break;
                }
                default:
                {
                    /* Reserved and unsupported categories fell through
                     * to the Success below, which contradicted the
                     * sibling: 0x0017 refuses them, and
                     * GetNumberCategorizedDBRecords calls them Unknown
                     * Database Category. A head unit that believed the
                     * Success sent PlayCurrentSelection and played
                     * whatever was already queued.
                     *
                     * No resume here, unlike 0x0017's default arm.
                     * That command pauses once before its switch, so
                     * its refusal path owes a resume; this one pauses
                     * inside each arm and never reaches one, so the
                     * resume it used to carry could only ever be
                     * handed to an engine that was already playing.
                     * `paused` is still read -- the arms above use
                     * it. */
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    return;
                }
            }
            if (!paused)
                audio_resume();
            /* respond with cmd ok packet */
            cmd_ok(cmd);
            break;
        }
        case 0x0039: /* GetColorDisplayImageLimits */
            /* The following is the description for the Apple Firmware
             *
             * Requests the limiting characteristics of the color image that
             * can be sent to the iPod for display while it is connected to
             * the device. It can be used to determine the display pixel format
             * and maximum width and height of a color image to be set using
             * the Command 0x0032: SetDisplayImage telegram. In response, the
             * iPod sends a Command 0x003A: ReturnColorDisplayImageLimits
             * telegram to the device with the requested display information.
             * This command is supported only by iPods with color displays.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x03  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x39  Command ID (bits 7:0)
             *  6   0xC0  Telegram payload checksum byte
             *
             */
        {
            /* Set the same as the ReturnMonoDisplayImageLimits */
            unsigned char data[] = {0x04, 0x00, 0x3A,
                                    LCD_WIDTH >> 8, LCD_WIDTH & 0xff,
                                    LCD_HEIGHT >> 8, LCD_HEIGHT & 0xff,
                                    0x01};
            L4_SEND_REPLY(data, sizeof(data));
            break;
        }
        case 0x003A: /* ReturnColorDisplayImageLimits See Above */
            /* The following is the description for the Apple Firmware
             *
             * Returns the limiting characteristics of the color image that can
             * be sent to the iPod for display while it is connected to the
             * device. The iPod sends this telegram in response to the Command
             * 0x0039: GetColorDisplayImageLimits telegram. Display
             * characteristics include maximum image width and height and the
             * display pixel format.
             *
             * Note: If the iPod supports multiple display image formats, a five
             * byte block of additional image width, height, and pixel format
             * information is appended to the payload for each supported display
             * format. The list of supported color display image formats
             * returned by the iPod may change in future software versions.
             * Devices must be able to parse a variable length list of supported
             * color display formats to search for compatible formats.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0xNN  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x3A  Command ID (bits 7:0)
             *  6   0xNN  Maximum image width in pixels (bits 15:8)
             *  7   0xNN  Maximum image width in pixels (bits 7:0)
             *  8   0xNN  Maximum image height in pixels (bits 15:8)
             *  9   0xNN  Maximum image height in pixels (bits 7:0)
             * 10   0xNN  Display pixel format (see Table 6-70 (page 99)).
             * 11-N 0xNN  Optional display image width, height, and pixel format
             *            for the second to nth supported display formats,
             *            if present.
             * NN   0xNN  Telegram payload checksum byte
             *
             */
        {
            /* We should NEVER receive this command so ERROR if we do */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
        case 0x003B: /* ResetDBSelectionHierarchy */
        {
            /* Table 5-1 (p.401) marks this one Yes in the
             * authentication column:
             *
             *   0x003B  ResetDBSelectionHierarchy  Database Engine
             *           1.11  Yes
             *
             * and it is the only command in the 0x3B-0x49 range this
             * file implements -- Table 2-7 (p.105) draws the line in
             * the same place for UART, "No (0x00-0x3A) / Yes (0x3B-0x45,
             * 0x47-0x49)". CHECKAUTH was defined at the top of this
             * file and invoked nowhere in it.
             *
             * Table 2-7's USB column reads Yes for the whole lingo, so
             * on the 6G's HID transport every command here should be
             * gated. That is not done: nothing in apps/iap knows which
             * transport it is on, and gating the lot unconditionally
             * would lock out UART accessories the spec allows. The
             * transport is knowable -- iap_transport_send is swapped in
             * usb_iap_hid.c -- so it is implementable, but it is a
             * wider change than this one and needs hardware to trust. */
            CHECKAUTH;

            /* ResetDBSelectionHierarchy, Table 5-73 p.444. */
            L4_NEED(1);
            /* The following is the description for the Apple Firmware
             *
             * This command carries a single byte in its payload (byte 6). A
             * hierarchy selection value of 0x01 means that the accessory wants
             * to navigate the music hierarchy; a hierarchy selection value of
             * 0x02 means that the accessory wants to navigate the video
             * hierarchy.
             *
             * Byte Value Meaning
             *  0   0xFF  Sync byte (required only for UART serial)
             *  1   0x55  Start of telegram
             *  2   0x04  Telegram payload length
             *  3   0x04  Lingo ID: Extended Interface lingo
             *  4   0x00  Command ID (bits 15:8)
             *  5   0x3B  Command ID (bits 7:0)
             *  6   0x01 or 0x02 Hierarchy selection
             *  7   0xNN Telegram payload checksum byte
             *
             * Note: The iPod will return an error if a device attempts to
             * enable an unsupported hierarchy, such as a video hierarchy on an
             * iPod  model that does not support video.
             */
            dbrecordcount = 0;
            cur_dbrecord[0] = 0;
            put_u32(&cur_dbrecord[1],0);
            switch (L4_PARAM(0))
            {
                case 0x01: /* Music */
                {
                    cmd_ok(cmd);
                    break;
                }
                default: /* Anything else */
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
            }
            break;
        }
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode04 Command");
#endif
            /* The default response is IAP_ACK_BAD_PARAM */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
