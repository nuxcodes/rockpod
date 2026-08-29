/***************************************************************************
 * Extended Interface PlayControl button synthesis.
 *
 * A PlayControl command (0x04/0x0029) does not press a physical button;
 * it raises one in iap_remotebtn for the button driver to sample.
 * iap_periodic() clears that the instant iap_timeoutbtn reaches zero
 * (apps/iap/iap-core.c:1017-1024), and firmware/drivers/button.c:587
 * needs two consecutive 100 Hz samples to agree before it queues
 * anything. A press raised without a timeout therefore has no defined
 * lifetime at all.
 ****************************************************************************/

#include "iap_test.h"
#include "iap-core.h"

#include "config.h"
#include "iap.h"
#include "button.h"
#include "audio.h"
#include "settings.h"

extern void iap_periodic(void);

/* Put the link into Extended Interface mode, which PlayControl needs. */
static void enter_extended(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x40);   /* Request Extended Interface Mode */
    iaptest_tx_clear();
}

/* Send one PlayControl and report the button it raised. */
static unsigned long play_control(unsigned char code)
{
    unsigned char p[6] = { 0x04, 0x00, 0x29, 0x00, 0x50, code };

    /* Let the button driver sample whatever is outstanding first;
     * iap_handlepkt() refuses to process a new packet until it has. */
    iaptest_button_sample(4);
    iaptest_rx(p, sizeof(p));
    return iap_remotebtn;
}

/* The transport arms drive the Playback Engine, they do not press
 * buttons.
 *
 * They used to raise BUTTON_RC_PLAY, BUTTON_RC_RIGHT and
 * BUTTON_RC_LEFT, which remote_control_rx() ORs into the physical
 * button read -- and apps/keymaps/keymap-ipod.c dispatches by focused
 * context, binding all three bare in the standard one (:317 PREV, :318
 * NEXT, :320 OK). Nothing gives the WPS focus merely because audio is
 * playing, so on a docked iPod left in the file browser the head unit's
 * Next key moved the user's cursor, Previous moved it back, and
 * Play/Pause activated whatever was highlighted -- each answered
 * Success.
 *
 * MFi 5.1.37 (p.428): "This command is sent by the accessory to control
 * the media playback state of the Apple device", and the Stop arm was
 * already doing exactly that with audio_stop(). */
void test_playcontrol_transport_drives_the_engine(void)
{
    enter_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    rbstub_reset_calls();
    play_control(0x01);                 /* Toggle Play/Pause */
    CHECK_EQ_INT(rbstub_calls.pause, 1, "Toggle did not pause a playing "
                                        "device");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Toggle Play/Pause raised a button, which outside the "
                 "WPS is ACTION_STD_OK");

    /* And again, to resume. */
    rbstub_reset_calls();
    play_control(0x01);
    CHECK_EQ_INT(rbstub_calls.resume, 1,
                 "Toggle did not resume a paused device");

    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    play_control(0x03);                 /* Next Track */
    CHECK_EQ_INT(rbstub_calls.skip, 1, "Next Track moved nothing");
    CHECK_EQ_INT(rbstub_calls.last_skip, 1, "Next Track went the wrong way");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Next Track raised a button, which outside the WPS "
                 "moves the browser cursor");

    rbstub_reset_calls();
    play_control(0x04);                 /* Previous Track */
    CHECK_EQ_INT(rbstub_calls.skip, 1, "Previous Track moved nothing");
    CHECK_EQ_INT(rbstub_calls.last_skip, -1,
                 "Previous Track went the wrong way");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Previous Track raised a button");
}

/* The press must outlive at least one periodic tick, or the button
 * driver can never sample it twice. */
void test_playcontrol_press_survives_a_tick(void)
{
    /* The seek, which is the one transport command that still holds a
     * button: it has no Playback Engine equivalent, and iap_seek_start()
     * exists to hold BUTTON_RC_RIGHT until EndFFRew. Play/Pause and the
     * skips drive the engine now, so they raise nothing to survive. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    enter_extended();

    play_control(0x05);
    CHECK(iap_timeoutbtn > 0,
          "StartFF left iap_timeoutbtn at 0, so iap_periodic() clears "
          "the press before the button driver can debounce it");

    iap_periodic();
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_RIGHT,
                 "button still held after one tick");
}

/* ...and it must not be held forever. */
void test_playcontrol_press_is_released(void)
{
    enter_extended();
    /* Every PlayControl arm refuses from a stopped player, so without
     * this the command raised no button, the loop below ran zero times
     * and both assertions were trivially true. Proved by mutation:
     * IAP_BTN_TAP 3 -> 500, a "tap" held fifty seconds, kept the suite
     * green. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* The seek: the skips drive the engine now and hold nothing. */
    play_control(0x05);
    CHECK(iap_remotebtn != BUTTON_NONE,
          "the seek raised no button, so this case measures nothing");
    iap_seek_stop();

    int ticks = 0;
    while (iap_remotebtn != BUTTON_NONE && ticks < 1000) {
        iap_periodic();
        ticks++;
    }

    CHECK(iap_remotebtn == BUTTON_NONE,
          "the press was never released after %d ticks", ticks);
    CHECK(ticks <= 10,
          "a discrete press was held for %d ticks; a tap should last "
          "roughly the 300ms Simple Remote uses", ticks);
}

/* StartFF is held until EndFFRew rather than timing out like a tap. */
void test_playcontrol_seek_is_held_until_ended(void)
{
    /* Playing: MFi 5.1.37 (p.428) makes a seek that cannot start an
     * error, and iap_seek_start() refuses one from a stopped device --
     * this case is about the hold, not that. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    enter_extended();

    CHECK_EQ_INT(play_control(0x05), BUTTON_RC_RIGHT, "StartFF");

    for (int i = 0; i < 10; i++)
        iap_periodic();
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_RIGHT,
                 "StartFF must still be held a second later");

    play_control(0x07);                 /* EndFFRew */
    iap_periodic();
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "EndFFRew must release the seek");
}

/* A lost EndFFRew must not wedge the button indefinitely. */
void test_playcontrol_seek_has_a_safety_bound(void)
{
    enter_extended();
    /* Same: iap_seek_start() returns false with nothing playing
     * (iap-core.c), so the seek never began and the bound was measured
     * against a button that was never down. IAP_BTN_HELD 100 ->
     * 1000000, which is the safety release removed outright, kept this
     * case green. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    play_control(0x05);
    CHECK(iap_remotebtn != BUTTON_NONE,
          "the seek raised no button, so this case measures nothing");

    int ticks = 0;
    while (iap_remotebtn != BUTTON_NONE && ticks < 5000) {
        iap_periodic();
        ticks++;
    }

    CHECK(iap_remotebtn == BUTTON_NONE,
          "a seek with no EndFFRew never released");
}

/* MFi Table 5-49 (p.429) "Play control command codes":
 *
 *   Next Track  0x03  1.00  Deprecated; use Next (0x08) and
 *                           Previous (0x09) instead.
 *   Next        0x08  1.06
 *   Previous    0x09  1.06
 *   Play        0x0A  1.13  These commands may be used regardless of the
 *   Pause       0x0B  1.13  lingo protocol version if the Apple device
 *                           explicitly declares their support
 *   Reserved    0x0F - 0xFF
 *
 * This build advertises Extended Interface 1.12 (iap-core.c:209), so
 * 0x08 and 0x09 -- introduced at 1.06 -- are not optional. A head unit
 * that reads the deprecation note and uses them for its skip buttons
 * found nothing happened, and was told Success.
 *
 * Rockbox has no chapter concept on this path, and Table 5-49's own
 * wording covers that case: "If the track has no chapters, Previous
 * will back up to the beginning of the previous track." So without
 * chapters 0x08 and 0x09 are exactly 0x03 and 0x04. */
void test_playcontrol_next_and_previous(void)
{
    iaptest_session_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);   /* see p.429 */

    static const struct { unsigned char code; int step; const char *what; } tc[] = {
        { 0x03,  1, "Next Track (0x03, deprecated)" },
        { 0x04, -1, "Previous Track (0x04, deprecated)" },
        { 0x08,  1, "Next (0x08)" },
        { 0x09, -1, "Previous (0x09)" },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        iaptest_tx_clear();
        /* Clear first: an unhandled code does nothing, and 0x09 would
         * then pass on 0x04's leftovers. */
        rbstub_reset_calls();
        rbstub_calls.last_skip = 0;
        unsigned char p[7] = { 0x04, 0x00, 0x29, 0x00,
                               (unsigned char)(0x60 + i), tc[i].code };
        iaptest_rx(p, 6);

        CHECK_EQ_INT(rbstub_calls.last_skip, tc[i].step, tc[i].what);
        iaptest_button_sample(4);
    }
}

/* Table 5-49 reserves 0x00 and 0x0F-0xFF, and puts Play (0x0A), Pause
 * (0x0B), Next Chapter (0x0C), Previous Chapter (0x0D) and Resume iPod
 * (0x0E) at protocol 1.13 or 1.14 -- above the 1.12 this build claims.
 * The 1.13 pair may be used below that version only "if the Apple device
 * explicitly declares their support; see Table 3-132 (page 192)", and
 * this device leaves that option bit clear.
 *
 * MFi 5.1.37 (p.428): "If the Apple device does not enter the requested
 * state successfully, an error status is returned." An unhandled code
 * acked Success told the accessory its button had worked. */
void test_playcontrol_unsupported_codes_are_refused(void)
{
    /* In the mode, not merely authenticated. iap-lingo4.c refuses every
     * Extended Interface command outside IST_EXTENDED with
     * IAP_ACK_BAD_PARAM naming the same command -- which satisfies every
     * assertion below for the wrong reason. Deleting the default arm
     * this case exists to test left the suite green. */
    iaptest_session_extended();

    static const unsigned char bad[] = {
        0x00,                            /* Reserved */
        0x0A, 0x0B,                      /* Play, Pause -- 1.13, undeclared */
        0x0C, 0x0D, 0x0E,                /* chapter controls -- 1.14 */
        0x0F, 0x10, 0x7F, 0xFF           /* Reserved 0x0F-0xFF */
    };

    for (unsigned i = 0; i < sizeof(bad); i++) {
        iaptest_tx_clear();
        iap_remotebtn = BUTTON_NONE;

        unsigned char p[6] = { 0x04, 0x00, 0x29, 0x00,
                               (unsigned char)(0x70 + i), bad[i] };
        iaptest_rx(p, 6);

        CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                     "an unsupported play control code moved a button");

        /* The Extended Interface has its own acknowledgement, command
         * 0x0001, rather than the General lingo's iPodAck: lingo,
         * command (two bytes), transaction ID, status, then the
         * two-byte command being answered. */
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to play control code 0x%02X", bad[i]);
        if (r && r->paylen >= 8) {
            CHECK_EQ_INT(r->payload[0], 0x04, "ack lingo");
            CHECK_EQ_INT((r->payload[1] << 8) | r->payload[2], 0x0001,
                         "ack command");
            CHECK_EQ_INT((r->payload[6] << 8) | r->payload[7], 0x0029,
                         "the ack names PlayControl");
            CHECK(r->payload[5] != 0x00,
                  "play control code 0x%02X was acknowledged Success, "
                  "so the accessory believes its button worked",
                  bad[i]);
        } else if (r) {
            CHECK(false, "ack too short: %d bytes", r->paylen);
        }
        iaptest_button_sample(4);
    }
}

/* Three Extended Interface commands the Apple device ORIGINATES had a
 * case label and no body, so control fell into the next case.
 *
 * MFi 5.1.36 (p.426): "Command 0x0027: PlayStatusChangeNotification --
 * Lingo: 0x04 -- Origin: Apple device". 0x0021
 * (ReturnIndexedPlayingTrackTitle) and 0x0023
 * (ReturnIndexedPlayingTrackArtistName) are likewise replies the device
 * sends, not commands it accepts.
 *
 * Twenty-one other device-origin commands in this file are handled with
 * an explicit "We should NEVER receive this command so ERROR if we do"
 * and a Bad Parameter ack. These three were missed.
 *
 * 0x0027 is the dangerous one: it fell into 0x0028 PlayCurrentSelection,
 * which pauses, may randomise or sort the playlist, and skips. */
void test_playcontrol_device_origin_commands_are_refused(void)
{
    /* Same: outside IST_EXTENDED the negotiation gate answers first and
     * these assertions pass whatever the handlers do. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 5);

    /* 0x0027 with four payload bytes, which is exactly what
     * PlayCurrentSelection's CHECKLEN accepts as a selection index. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x27, 0x00, 0x80, 0x00, 0x00, 0x00, 0x03);

    CHECK_EQ_INT(rbstub_calls.pause, 0,
                 "PlayStatusChangeNotification paused playback: it fell "
                 "through into PlayCurrentSelection");
    CHECK_EQ_INT(rbstub_calls.skip, 0,
                 "PlayStatusChangeNotification skipped a track");
    CHECK_EQ_INT(rbstub_calls.randomise, 0,
                 "PlayStatusChangeNotification randomised the playlist");
    CHECK_EQ_INT(rbstub_calls.sort, 0,
                 "PlayStatusChangeNotification sorted the playlist");

    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to a device-origin command");
        if (r && r->paylen >= 8) {
            CHECK_EQ_INT((r->payload[1] << 8) | r->payload[2], 0x0001,
                         "the reply is an Extended Interface ack");
            CHECK(r->payload[5] != 0x00,
                  "a command the device originates was acknowledged "
                  "Success when sent to it");
            CHECK_EQ_INT((r->payload[6] << 8) | r->payload[7], 0x0027,
                         "the ack names the command it answers");
        }
    }

    /* 0x0021 and 0x0023 fell into the 0x0024 body, ran a metadata
     * lookup, and then hit a trailing switch with no matching case --
     * so nothing at all was transmitted and the accessory waited. */
    static const unsigned short devcmd[] = { 0x0021, 0x0023 };
    for (unsigned i = 0; i < 2; i++) {
        iaptest_tx_clear();
        unsigned char p[9] = { 0x04,
                               (unsigned char)(devcmd[i] >> 8),
                               (unsigned char)devcmd[i],
                               0x00, (unsigned char)(0x90 + i),
                               0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL,
              "command 0x%04X got no reply at all, so the accessory "
              "waits for one that never comes", devcmd[i]);
        if (r && r->paylen >= 8) {
            CHECK(r->payload[5] != 0x00,
                  "command 0x%04X was acknowledged Success", devcmd[i]);
            CHECK_EQ_INT((r->payload[6] << 8) | r->payload[7], devcmd[i],
                         "the ack names the command it answers");
        }
    }
}

/* MFi 5.1.20 (p.416):
 *
 *   "Sending a SelectDBRecord command with the Track or Audiobook
 *    category and a record index of -1 is invalid, because the previous
 *    database selection made with the Track category and a valid index
 *    passes the database selection to the Playback Engine. Sending a
 *    SelectDBRecord(Track, -1) command returns a parameter error."
 *
 * Track is category 0x05 and Audiobook 0x07 (Table 5-27, p.417).
 *
 * The range guard excluded 0xFFFFFFFF for every category, so Track and
 * -1 sailed past it into audio_skip(index - playlist_next(0)) with
 * index unsigned 0xFFFFFFFF. The comment above the guard asserted the
 * opposite -- "the spec makes it invalid only for Track (0x05), which
 * is rejected by the bound below" -- so the intent was stated and never
 * implemented. */
void test_playcontrol_select_track_minus_one_is_refused(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(40, 10);

    /* Initialise the category count, which 5.1.21 (p.417) requires
     * before a selection: GetNumberCategorizedDBRecords for Track. */
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0x70, 0x05);

    static const struct { unsigned char cat; const char *name; } bad[] = {
        { 0x05, "Track" },
        { 0x07, "Audiobook" },
    };

    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        iaptest_tx_clear();
        rbstub_reset_calls();

        unsigned char p[11] = { 0x04, 0x00, 0x17, 0x00,
                                (unsigned char)(0x71 + i), bad[i].cat,
                                0xFF, 0xFF, 0xFF, 0xFF };
        iaptest_rx(p, 10);

        CHECK_EQ_INT(rbstub_calls.skip, 0,
                     "SelectDBRecord with index -1 reached audio_skip");
        CHECK_EQ_INT(rbstub_calls.pause, 0,
                     "SelectDBRecord with index -1 paused playback");

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectDBRecord(%s, -1)", bad[i].name);
        if (r && r->paylen >= 8) {
            CHECK(r->payload[5] == 0x04,
                  "SelectDBRecord(%s, -1) should return a parameter "
                  "error, got status 0x%02X", bad[i].name, r->payload[5]);
            CHECK_EQ_INT((r->payload[6] << 8) | r->payload[7], 0x0017,
                         "the ack names SelectDBRecord");
        }
    }

    /* -1 is still how an accessory undoes a selection in a category
     * that is not passed to the Playback Engine, so the other
     * categories must keep accepting it. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x80, 0x03, 0xFF, 0xFF, 0xFF, 0xFF);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectDBRecord(Album, -1)");
        if (r && r->paylen >= 6)
            CHECK_EQ_INT(r->payload[5], 0x00,
                         "SelectDBRecord(Album, -1) must still succeed");
    }
    /* And must not move playback while doing it. MFi 5.1.20 (p.415)
     * makes the undo a Database Engine operation -- "moves the database
     * selection up to the next highest menu level" -- and p.416 has it
     * be a no-op when the category was never selected. Asserting only
     * the ack missed that -1 fell through to
     * audio_skip(index - playlist_next(0)) as an unsigned 0xFFFFFFFF,
     * which walked the queue back to the front. */
    CHECK_EQ_INT(rbstub_calls.skip, 0,
                 "SelectDBRecord(Album, -1) moved playback; the undo is a "
                 "database operation and must leave the queue alone");

    /* An invalid category is still a parameter error, -1 or not: the
     * no-op must not become a way to launder one past the check. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x82, 0x40, 0xFF, 0xFF, 0xFF, 0xFF);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectDBRecord(0x40, -1)");
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "SelectDBRecord(0x40, -1) was accepted; 0x40 is not a "
                  "category and -1 does not make it one");
    }
    CHECK_EQ_INT(rbstub_calls.skip, 0,
                 "an invalid category with -1 moved playback");

    /* And a valid Track index still works. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x81, 0x05, 0x00, 0x00, 0x00, 0x05);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to a valid SelectDBRecord(Track, 5)");
        if (r && r->paylen >= 6)
            CHECK_EQ_INT(r->payload[5], 0x00,
                         "a valid Track selection must succeed");
        /* From stopped it starts rather than skips: audio_skip()
         * early-returns when the Playback Engine is stopped, so this
         * used to assert a call that moved nothing. The session above
         * never set a play state, and the stub's audio_resume() used to
         * set PLAY unconditionally, which is what hid it. */
        CHECK(rbstub_calls.playlist_start > 0,
              "a valid Track selection from a stopped device did not "
              "start playback");
        CHECK_EQ_INT(rbstub_calls.last_start_index, 5,
                     "it started at the wrong track");
    }
}

/* GetIndexedPlayingTrackInfo (0x04/0x000C).
 *
 * MFi 5.1.13 (p.408): "Gets track information for the track at the
 * specified index ... If the information type is invalid or does not
 * apply to the selected track, the Apple device returns an iPodAck with
 * an error status."
 *
 * MFi 5.1.14 (p.409): "If the track has no release date, then the
 * returned release date has all bytes zeros."
 *
 * Table 5-18 reserves info types 0x08-0xFF. */
static void gitpi(unsigned char tid, unsigned char info, uint32_t index)
{
    unsigned char p[12] = { 0x04, 0x00, 0x0C, 0x00, tid, info,
                            (unsigned char)(index >> 24),
                            (unsigned char)(index >> 16),
                            (unsigned char)(index >> 8),
                            (unsigned char)index,
                            0x00, 0x00 };
    iaptest_tx_clear();
    iaptest_rx(p, 12);
}

void test_playcontrol_track_info_rejects_reserved_types(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x01);
    rbstub_set_playlist(20, 3);

    /* Table 5-18 reserves 0x08-0xFF. Each must earn an error ack, not a
     * ReturnIndexedPlayingTrackInfo with an empty payload -- which is a
     * well-formed packet the accessory cannot tell from a real answer,
     * so it parses past the end or blocks for the rest. */
    static const unsigned char reserved[] = { 0x08, 0x09, 0x40, 0xFF };
    for (unsigned i = 0; i < sizeof(reserved); i++) {
        gitpi((unsigned char)(0x10 + i), reserved[i], 0);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to info type 0x%02X", reserved[i]);
        if (!r || r->paylen < 3)
            continue;

        unsigned short c = (r->payload[1] << 8) | r->payload[2];
        CHECK(c != 0x000D,
              "reserved info type 0x%02X produced a "
              "ReturnIndexedPlayingTrackInfo instead of an error ack",
              reserved[i]);
        if (c == 0x0001 && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "reserved info type 0x%02X was acknowledged Success",
                  reserved[i]);
    }

    /* The three that are implemented still answer. */
    static const unsigned char good[] = { 0x00, 0x02, 0x07 };
    for (unsigned i = 0; i < sizeof(good); i++) {
        gitpi((unsigned char)(0x20 + i), good[i], 0);
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 3
              && ((r->payload[1] << 8) | r->payload[2]) == 0x000D,
              "info type 0x%02X got no ReturnIndexedPlayingTrackInfo",
              good[i]);
    }
}

/* Track capability bit 5 says whether the track has a release date, and
 * this device leaves it clear -- then handed out 1 Feb 2011 anyway. */
void test_playcontrol_track_release_date_is_zero(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x02);
    rbstub_set_playlist(20, 3);

    gitpi(0x30, 0x02, 0);
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to a release-date query");
    if (r && r->paylen >= 14) {
        /* lingo, command(2), transID(2), info type, then eight bytes. */
        int nonzero = 0;
        for (int i = 6; i < 14; i++)
            if (r->payload[i])
                nonzero++;
        CHECK_EQ_INT(nonzero, 0,
                     "the release date must be all zeros when the track "
                     "capabilities say the track has none");
    } else if (r) {
        CHECK(false, "release-date reply is %d bytes, want 14", r->paylen);
    }
}

/* 5.1.13 says "the track at the specified index". The index sat in
 * bytes 4..7 and was never read: every query answered about whatever
 * was playing, so a head unit building a track list saw the current
 * track's length and duration on every row. */
void test_playcontrol_track_info_uses_the_index(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x03);
    rbstub_set_playlist(20, 3);

    /* An index past the end of the playlist is not a valid track, so
     * 5.1.13's "does not apply to the selected track" applies: an error
     * ack, not the currently-playing track's data. */
    gitpi(0x40, 0x00, 9999);
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to an out-of-range track index");
    if (r && r->paylen >= 3) {
        unsigned short c = (r->payload[1] << 8) | r->payload[2];
        CHECK(c != 0x000D,
              "an out-of-range track index was answered with the "
              "currently playing track's information");
        if (c == 0x0001 && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "an out-of-range track index was acknowledged Success");
    }

    /* An in-range index must be answered ABOUT THAT TRACK.
     *
     * The first version of this case only checked that an out-of-range
     * index was refused, and passed against a handler that validated
     * the index and then called audio_current_track() anyway -- which
     * is exactly what cad88bc136 shipped. The stub now gives track n a
     * length of 10000 + n so the two are distinguishable. */
    rbstub_set_playlist(20, 3);          /* track 3 is playing */
    for (uint32_t want = 5; want <= 7; want++) {
        gitpi((unsigned char)(0x41 + want), 0x00, want);
        r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 3
              && ((r->payload[1] << 8) | r->payload[2]) == 0x000D,
              "a valid track index got no ReturnIndexedPlayingTrackInfo");
        if (!r || r->paylen < 14)
            continue;

        /* lingo, command(2), transID(2), info type, caps(4), length(4) */
        uint32_t len = ((uint32_t)r->payload[10] << 24)
                     | ((uint32_t)r->payload[11] << 16)
                     | ((uint32_t)r->payload[12] << 8)
                     |  (uint32_t)r->payload[13];
        CHECK_EQ_INT(len, 10000 + want,
                     "the reply must carry the requested track's length, "
                     "not the currently playing track's");
    }
}

/* The length check omitted doff, so under IDPS a packet two bytes short
 * of carrying its index was accepted and the index read past its end. */
void test_playcontrol_track_info_length_includes_the_transid(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x04);
    rbstub_set_playlist(20, 3);
    iaptest_detach_model_for_raw_probes();

    /* Ten bytes: enough for the legacy form, two short of the IDPS one. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0C, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to a short GetIndexedPlayingTrackInfo");
    if (r && r->paylen >= 3) {
        unsigned short c = (r->payload[1] << 8) | r->payload[2];
        CHECK(c != 0x000D,
              "a packet too short to carry its track index was answered "
              "with track information read from past its end");
    }
}

/* The artwork chain told an accessory that artwork existed and then
 * handed it nothing.
 *
 * GetArtworkFormats advertised two RGB565 formats, 100x100 and 200x200.
 * GetTrackArtworkTimes answered with one image at t=0.
 * GetTrackArtworkData then sent a descriptor whose pixel format was
 * 0x00 -- "Reserved" in Table 4-87 (p.276) -- and whose width, height
 * and row size were all zero, so the total image size (rowSize x
 * height) is zero. A head unit either renders nothing after a full
 * round trip or divides by zero computing the pixel-buffer stride.
 *
 * The same device answered the Display Remote lingo's GetArtworkFormats
 * with an empty list, so the two lingoes disagreed about whether
 * artwork existed at all, and the track capabilities in
 * GetIndexedPlayingTrackInfo reported bit 2 "track has album artwork"
 * clear -- which is the truth.
 *
 * MFi 5.1.16 (p.412): "The accessory may return zero records."
 * MFi 5.1.39 (p.430): "The Apple device returns zero or more 4-byte
 * times, one for each piece of artwork associated with the track." */
void test_playcontrol_artwork_is_consistently_absent(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x06);
    rbstub_set_playlist(20, 3);

    /* GetArtworkFormats describes the supported output format even when the
     * selected track has no image. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0E, 0x00, 0x60);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to GetArtworkFormats");
        if (r) {
            CHECK_EQ_INT(r->paylen, 12,
                         "GetArtworkFormats must return one format record");
            CHECK_EQ_INT((r->paylen - 5) % 7, 0,
                         "the format list is not a whole number of "
                         "7-byte records");
        }
    }

    /* The artwork count, asked of both lingoes. This is where the two
     * last disagreed: Extended Interface answered with one 4-byte
     * record naming format ID 0x0000 -- a format GetArtworkFormats
     * above does not advertise -- while Display Remote answered with
     * the type byte and no records. MFi Table 5-18 (p.410): the reply
     * "is a sequence of 4-byte records; each record consists of a
     * 2-byte format ID value followed by a 2-byte count of images in
     * that format for this track." No formats means no records. */
    iaptest_tx_clear();
    /* Table 5-14 (p.408): info type, track index (4), chapter index
     * (2), plus the transaction ID. */
    IAPTEST_RX(0x04, 0x00, 0x0C, 0x00, 0x63, 0x07,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to GetIndexedPlayingTrackInfo(0x07)");
        if (r && r->paylen >= 3
            && ((r->payload[1] << 8) | r->payload[2]) == 0x000D) {
            /* lingo, command(2), transaction ID(2), info type. */
            CHECK_EQ_INT(r->paylen, 6,
                         "the Extended Interface artwork count must "
                         "carry zero records while no format is "
                         "advertised");
            CHECK_EQ_INT((r->paylen - 6) % 4, 0,
                         "the artwork count is not a whole number of "
                         "4-byte records");
        }
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x12, 0x00, 0x64, 0x08,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to the Display Remote artwork count");
        if (r && r->paylen >= 2 && r->payload[0] == 0x03
            && r->payload[1] == 0x13)
            /* lingo, command, transaction ID(2), info type. */
            CHECK_EQ_INT(r->paylen, 5,
                         "the Display Remote artwork count must carry "
                         "zero records, and must agree with the "
                         "Extended Interface one above");
    }

    /* GetTrackArtworkTimes: zero times. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x2A, 0x00, 0x61,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x01);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to GetTrackArtworkTimes");
        if (r && r->paylen >= 3
            && ((r->payload[1] << 8) | r->payload[2]) == 0x002B)
            CHECK_EQ_INT(r->paylen, 5,
                         "GetTrackArtworkTimes must return zero times "
                         "while no artwork is supplied");
    }

    /* GetTrackArtworkData: there is nothing to send, so an error ack
     * rather than a descriptor promising a zero-sized image in a
     * reserved pixel format. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x10, 0x00, 0x62,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to GetTrackArtworkData");
        if (r && r->paylen >= 3) {
            unsigned short c = (r->payload[1] << 8) | r->payload[2];
            CHECK(c != 0x0011,
                  "GetTrackArtworkData returned a descriptor for artwork "
                  "that does not exist");
            if (c == 0x0001 && r->paylen >= 6)
                CHECK(r->payload[5] != 0x00,
                      "GetTrackArtworkData was acknowledged Success with "
                      "no artwork to send");
        }
    }

    /* And the track capabilities still say there is none, so all three
     * answers agree with each other and with the Display Remote lingo. */
    gitpi(0x63, 0x00, 3);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 10) {
            uint32_t caps = ((uint32_t)r->payload[6] << 24)
                          | ((uint32_t)r->payload[7] << 16)
                          | ((uint32_t)r->payload[8] << 8)
                          |  (uint32_t)r->payload[9];
            CHECK((caps & (1u << 2)) == 0,
                  "track capability bit 2 claims album artwork");
        }
    }
}

/* GetNumberCategorizedDBRecords returned a stale count for any category
 * the switch did not cover.
 *
 * dbrecordcount is a file-static. The switch handles 0x01-0x08 and has
 * no default, so Top-level (0x00), Nested playlist (0x09), Genius Mixes
 * (0x0A), iTunesU (0x0B) and the reserved 0x0C-0xFF all left it holding
 * whatever the previous query had put there -- and it was sent anyway.
 *
 * MFi 5.1.22 (p.418): "If no matching database records are found, a
 * record count of zero is returned."
 * Table 3-6 (p.125): status 0x01 is "ERROR: Unknown database category
 * or session ID".
 * MFi 5.1.19 (p.414): ResetDBSelection "invalidates the category entry
 * count (sets the count to 0) for all categories except the playlist
 * category". */
static uint32_t db_count(unsigned char tid, unsigned char cat, bool *acked)
{
    iaptest_tx_clear();
    unsigned char p[6] = { 0x04, 0x00, 0x18, 0x00, tid, cat };
    iaptest_rx(p, sizeof(p));

    *acked = false;
    const struct iaptest_pkt *r = iaptest_tx(0);
    if (!r || r->paylen < 3)
        return 0xFFFFFFFFu;

    unsigned short c = (r->payload[1] << 8) | r->payload[2];
    if (c == 0x0001) {          /* an ack, not a count */
        *acked = true;
        return (r->paylen >= 6) ? r->payload[5] : 0xFFFFFFFFu;
    }
    if (c != 0x0019 || r->paylen < 9)
        return 0xFFFFFFFFu;
    return ((uint32_t)r->payload[5] << 24) | ((uint32_t)r->payload[6] << 16)
         | ((uint32_t)r->payload[7] << 8)  |  (uint32_t)r->payload[8];
}

void test_playcontrol_db_record_count_is_never_stale(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x07);
    rbstub_set_playlist(500, 0);

    bool acked;

    /* Prime the static with a large, real count. */
    uint32_t tracks = db_count(0x90, 0x05, &acked);
    CHECK(!acked && tracks == 500,
          "Track count should be 500, got %u", tracks);

    /* Categories the switch never handled. None may report 500. */
    /* Top-level (0x00) is defined at every protocol version, so it
     * gets a count -- zero, because there is no hierarchy above the
     * playback engine here.
     *
     * Nested playlist (0x09) is protocol 1.13, Genius Mixes (0x0A) and
     * iTunesU (0x0B) are 1.14, all above the 1.12 this device
     * advertises, and 0x0C-0xFF are Reserved. None of them exists, so
     * each must be refused rather than given a count -- a count of zero
     * invites a RetrieveCategorizedDatabaseRecords the sibling handler
     * then refuses, and the two answers contradict each other. */
    static const struct {
        unsigned char cat; bool want_ack; const char *name;
    } uncovered[] = {
        { 0x00, false, "Top-level" },
        { 0x09, true,  "Nested playlist" },
        { 0x0A, true,  "Genius Mixes" },
        { 0x0B, true,  "iTunesU" },
        { 0x0C, true,  "Reserved 0x0C" },
        { 0xFF, true,  "Reserved 0xFF" },
    };
    for (unsigned i = 0; i < sizeof(uncovered)/sizeof(uncovered[0]); i++) {
        /* Re-prime before every probe. Without this the previous
         * iteration leaves the static at zero, so a category that
         * still falls through reads zero and looks correct -- three
         * mutations survived on exactly that. */
        uint32_t primed = db_count((unsigned char)(0xB0 + i), 0x05, &acked);
        CHECK(primed == 500 && !acked,
              "priming query before %s returned %u",
              uncovered[i].name, primed);

        uint32_t n = db_count((unsigned char)(0x91 + i), uncovered[i].cat,
                              &acked);
        CHECK_EQ_INT(acked, uncovered[i].want_ack,
                     uncovered[i].want_ack
                         ? "this category must be refused, not counted"
                         : "this category must be counted, not refused");
        if (acked) {
            CHECK_EQ_INT(n, 0x01,
                         "the refusal should be Unknown Database "
                         "Category (Table 3-6, p.125)");
        } else {
            CHECK(n == 0,
                  "%s reported %u records, which is the count left over "
                  "from the previous query", uncovered[i].name, n);
        }
    }

    /* The covered ones still answer. */
    CHECK(db_count(0xA0, 0x05, &acked) == 500 && !acked,
          "Track count stopped working");
    CHECK(db_count(0xA1, 0x07, &acked) == 0 && !acked,
          "Audiobook count should be zero");

    /* ResetDBSelection also zeroes the count, which MFi 5.1.19 (p.414)
     * requires -- "invalidates the category entry count (sets the count
     * to 0) for all categories except the playlist category".
     *
     * That is NOT asserted here, and cannot be: dbrecordcount is a
     * file-static, and once every category either computes its own
     * count or is refused outright, no reply can expose a stale one.
     * The reset is defence in depth against the next category added
     * without a count of its own. A mutation removing it changes no
     * observable byte.
     *
     * What is checked is that the reset does not break the counts. */
    IAPTEST_RX(0x04, 0x00, 0x16, 0x00, 0xA3);
    CHECK(db_count(0xA4, 0x05, &acked) == 500 && !acked,
          "the Track count stopped working after ResetDBSelection");
}

/* MFi Table 5-53 (p.432) gives shuffle 0x00 off, 0x01 tracks, 0x02
 * albums, "0x03 - 0xFF Reserved". Table 5-59 (p.434) gives repeat 0x00
 * off, 0x01 one track, 0x02 all, "0x03 - 0xFF Reserved".
 *
 * SetShuffle treated every non-zero byte as "shuffle tracks on",
 * reserved values included. SetRepeat left the mode untouched for
 * anything above 2. Both then acknowledged Success, so a following
 * GetShuffle or GetRepeat reported a state the accessory had not asked
 * for -- and the sibling Display Remote handlers answer Bad Parameter
 * for the same values on the same settings. */
void test_playcontrol_shuffle_and_repeat_reject_reserved_values(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0xB0);
    rbstub_set_playlist(20, 3);

    static const struct {
        unsigned short cmd; unsigned char val; bool ok; const char *what;
    } tc[] = {
        { 0x002E, 0x00, true,  "SetShuffle off" },
        { 0x002E, 0x01, true,  "SetShuffle tracks" },
        { 0x002E, 0x02, false, "SetShuffle albums (not implemented)" },
        { 0x002E, 0x03, false, "SetShuffle reserved 0x03" },
        { 0x002E, 0xFF, false, "SetShuffle reserved 0xFF" },
        { 0x0031, 0x00, true,  "SetRepeat off" },
        { 0x0031, 0x01, true,  "SetRepeat one" },
        { 0x0031, 0x02, true,  "SetRepeat all" },
        { 0x0031, 0x03, false, "SetRepeat reserved 0x03" },
        { 0x0031, 0xFF, false, "SetRepeat reserved 0xFF" },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        iaptest_tx_clear();
        unsigned char p[6] = { 0x04,
                               (unsigned char)(tc[i].cmd >> 8),
                               (unsigned char)tc[i].cmd,
                               0x00, (unsigned char)(0xB1 + i),
                               tc[i].val };
        iaptest_rx(p, sizeof(p));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s got no reply", tc[i].what);
        if (!r || r->paylen < 6)
            continue;

        CHECK_EQ_INT((r->payload[1] << 8) | r->payload[2], 0x0001,
                     "the reply is an Extended Interface ack");
        if (tc[i].ok) {
            CHECK_EQ_INT(r->payload[5], 0x00, tc[i].what);
        } else {
            CHECK(r->payload[5] != 0x00,
                  "%s was acknowledged Success though nothing changed",
                  tc[i].what);
        }
    }
}

/* MFi 5.1.20 (p.416): "The Apple device also returns a bad parameter
 * error iPodAck when accessories send the SelectDBRecord command with
 * an invalid category type."
 *
 * The category switch had an empty default that fell through to the
 * Success ack, which contradicted the sibling handler: 34012c491e made
 * GetNumberCategorizedDBRecords answer Unknown Database Category for
 * 0x09-0xFF, so the same device declared a category unknown and then
 * accepted a selection in it.
 *
 * And SelectSortDBRecord's length check was one byte short. MFi Table
 * C-35 (p.546) gives it a category type, a four-byte record index and a
 * sort type -- six payload bytes, nine with the lingo and the two-byte
 * command -- but the check said eight, carried over from
 * SelectDBRecord's five-byte payload. A command missing its sort type
 * was accepted, sorted the playlist and skipped. */
void test_playcontrol_selection_refuses_what_it_cannot_do(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    /* Categories the selection switch does nothing with. */
    static const unsigned char unknown[] = { 0x00, 0x09, 0x0A, 0x0B, 0xFF };
    for (unsigned i = 0; i < sizeof(unknown); i++) {
        iaptest_tx_clear();
        rbstub_reset_calls();
        unsigned char p[10] = { 0x04, 0x00, 0x17, 0x00,
                                (unsigned char)(0xC1 + i), unknown[i],
                                0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectDBRecord(0x%02X)", unknown[i]);
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "SelectDBRecord(0x%02X) was acknowledged Success though "
                  "the handler does nothing with that category",
                  unknown[i]);
    }

    /* A category it does handle still works. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0xD0, 0x03, 0x00, 0x00, 0x00, 0x02);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 6)
            CHECK_EQ_INT(r->payload[5], 0x00,
                         "a handled category must still succeed");
    }

    /* SelectSortDBRecord one byte short of its sort type. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x38, 0x00, 0xD1, 0x05, 0x00, 0x00, 0x00, 0x01);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to a short SelectSortDBRecord");
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "a SelectSortDBRecord missing its sort type was "
                  "acknowledged Success");
        CHECK_EQ_INT(rbstub_calls.sort + rbstub_calls.randomise, 0,
                     "a short SelectSortDBRecord reordered the playlist");
    }

    /* The full-length form still works. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x38, 0x00, 0xD2,
               0x05, 0x00, 0x00, 0x00, 0x01, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 6)
            CHECK_EQ_INT(r->payload[5], 0x00,
                         "a full-length SelectSortDBRecord must succeed");
    }
}

/* MFi 5.1.19 (p.414): ResetDBSelection "resets the current database
 * selection to an empty state, invalidates the category entry count
 * (sets the count to 0) for all categories except the playlist
 * category, and sets the database hierarchy to the audio hierarchy". It
 * is answered with an iPodAck.
 *
 * Nothing reached the handler: making its case label unreachable left
 * the whole suite green. */
void test_playcontrol_reset_db_selection(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0xE8);
    rbstub_set_playlist(40, 3);

    /* Make a selection, so there is something to reset. */
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0xE9, 0x05);
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0xEA, 0x05, 0x00, 0x00, 0x00, 0x02);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x16, 0x00, 0xEB);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to ResetDBSelection");
    if (r && r->paylen >= 8) {
        CHECK_EQ_INT(r->payload[0], 0x04, "reply lingo");
        CHECK_EQ_INT((r->payload[1] << 8) | r->payload[2], 0x0001,
                     "an Extended Interface ack");
        CHECK_EQ_INT(r->payload[3], 0x00, "transaction ID high");
        CHECK_EQ_INT(r->payload[4], 0xEB, "transaction ID low");
        CHECK_EQ_INT(r->payload[5], 0x00, "and it succeeds");
        CHECK_EQ_INT((r->payload[6] << 8) | r->payload[7], 0x0016,
                     "the ack names ResetDBSelection");
    }

    /* The selection really is empty afterwards: a category the handler
     * does nothing with is refused, which it would not be if a Track
     * selection were still current. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0xEC, 0x05);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to a count after the reset");
        if (p && p->paylen >= 9) {
            uint32_t n = ((uint32_t)p->payload[5] << 24)
                       | ((uint32_t)p->payload[6] << 16)
                       | ((uint32_t)p->payload[7] << 8)
                       |  (uint32_t)p->payload[8];
            CHECK_EQ_INT(n, 40,
                         "the Track count is the whole playlist again "
                         "after a reset");
        }
    }
}

/* MFi 5.1.29 (p.421), and the same note under 5.1.31 and 5.1.33: "If
 * the packet length or playing track index is invalid, the Apple device
 * responds with an iPodAck command including the specific error
 * status."
 *
 * All three indexed-metadata commands share one handler, and it used
 * the index unchecked. The sibling GetIndexedPlayingTrackInfo (0x000C)
 * refuses the identical index, so the same device answered two ways
 * about the same track. */
void test_playcontrol_indexed_metadata_refuses_bad_indices(void)
{
    static const unsigned char cmds[] = { 0x20, 0x22, 0x24 };
    static const struct { uint32_t idx; const char *what; } bad[] = {
        { 40,         "one past the end of a 40-track queue" },
        { 0xFFFFFFFF, "-1" },
        { 0x7FFFFFFF, "the top of a signed long" },
    };

    for (unsigned c = 0; c < sizeof(cmds); c++) {
        for (unsigned b = 0; b < sizeof(bad)/sizeof(bad[0]); b++) {
            iaptest_init();
            iaptest_session_extended();
            rbstub_set_playlist(40, 3);
            iaptest_tx_clear();

            unsigned char p[9] = {
                0x04, 0x00, cmds[c], 0x00, (unsigned char)(0x30 + b),
                (unsigned char)(bad[b].idx >> 24),
                (unsigned char)(bad[b].idx >> 16),
                (unsigned char)(bad[b].idx >> 8),
                (unsigned char)bad[b].idx
            };
            iaptest_rx(p, sizeof(p));

            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL, "no reply to command 0x00%02X with index %s",
                  cmds[c], bad[b].what);
            if (!r || r->paylen < 6)
                continue;
            CHECK(r->payload[2] == 0x01,
                  "command 0x00%02X with index %s was answered with "
                  "command 0x%02X%02X rather than an acknowledgement -- "
                  "the accessory was handed metadata for a track that "
                  "does not exist",
                  cmds[c], bad[b].what, r->payload[1], r->payload[2]);
            if (r->payload[2] == 0x01)
                CHECK(r->payload[5] != 0x00,
                      "command 0x00%02X with index %s answered Success",
                      cmds[c], bad[b].what);
        }
    }

    /* An in-range index whose track cannot be read. The range check
     * cannot see this coming -- on hardware it is a control-file read
     * failing -- and ignoring the return left info.filename holding
     * whatever was on the stack, which get_metadata() then opened. */
    for (unsigned c = 0; c < sizeof(cmds); c++) {
        iaptest_init();
        iaptest_session_extended();
        rbstub_set_playlist(40, 3);
        rbstub_fail_track_info(true);
        iaptest_tx_clear();

        unsigned char p[9] = { 0x04, 0x00, cmds[c], 0x00, 0x50,
                               0x00, 0x00, 0x00, 0x05 };
        iaptest_rx(p, sizeof(p));
        rbstub_fail_track_info(false);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to command 0x00%02X when the track "
                         "could not be read", cmds[c]);
        if (r && r->paylen >= 6)
            CHECK(r->payload[2] == 0x01 && r->payload[5] != 0x00,
                  "command 0x00%02X answered with metadata although the "
                  "track could not be read", cmds[c]);
    }

    /* A valid index still answers with the metadata, so the guard is
     * not simply refusing everything. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to a valid indexed title request");
        if (r && r->paylen >= 3)
            CHECK(r->payload[2] == 0x21,
                  "a valid index was answered with 0x%02X%02X rather than "
                  "ReturnIndexedPlayingTrackTitle",
                  r->payload[1], r->payload[2]);
    }
}

/* MFi 5.1.51 (p.442): "The index that is specified here is obtained by
 * sending ... GetCurrentPlayingTrackIndex", and "If this command is
 * sent with the current playing track index, the Apple device pauses
 * playback momentarily and then resumes."
 *
 * So the round trip has to be a no-op. It was not: 0x001E answers in
 * the accessory's index space by subtracting first_index, and 0x0037
 * used the raw value against playlist_next(), a Rockbox index. */
void test_playcontrol_set_current_track_round_trips(void)
{
    iaptest_session_extended();

    /* A queue that has been randomised, which is what makes
     * first_index non-zero (apps/playlist.c:1507). */
    rbstub_set_playlist(40, 17);
    rbstub_set_playlist_first_index(10);
    /* MFi 5.1.51 (p.442): "This command is usable only when the Apple
     * device is in a playing or paused state. If the Apple device is
     * stopped, this command fails." */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Ask where we are. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x1E, 0x00, 0x60);
    uint32_t reported = 0;
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetCurrentPlayingTrackIndex");
        if (!r || r->paylen < 9)
            return;
        reported = ((uint32_t)r->payload[5] << 24)
                 | ((uint32_t)r->payload[6] << 16)
                 | ((uint32_t)r->payload[7] << 8)
                 |  (uint32_t)r->payload[8];
    }

    /* Echo it straight back. */
    rbstub_reset_calls();
    iaptest_tx_clear();
    {
        unsigned char p[9] = {
            0x04, 0x00, 0x37, 0x00, 0x61,
            (unsigned char)(reported >> 24), (unsigned char)(reported >> 16),
            (unsigned char)(reported >> 8),  (unsigned char)reported
        };
        iaptest_rx(p, sizeof(p));
    }

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to SetCurrentPlayingTrack");
    if (r && r->paylen >= 6)
        CHECK_EQ_INT(r->payload[5], 0x00,
                     "SetCurrentPlayingTrack with the index the device "
                     "just reported was refused");

    CHECK(rbstub_calls.last_skip == 0,
          "echoing back GetCurrentPlayingTrackIndex moved playback by %d "
          "tracks; the two commands disagree about which index space "
          "they speak", rbstub_calls.last_skip);
}

/* MFi C.7.6 (p.546): "This command acts the same as Command 0x0017:
 * SelectDBRecord (page 415), but includes a sorting feature." So the
 * refusals in 5.1.20 (p.416) apply to it as well -- "an invalid
 * category type, or ... the Track category and an index greater than
 * the total number of tracks available".
 *
 * It answered Success for both. The bound was > where 0x0017 uses >=,
 * and there was no default arm, so reserved categories fell through to
 * the Success at the bottom. */
void test_playcontrol_sort_selection_refuses_what_the_sibling_does(void)
{
    /* Table C-36 (p.547) marks these Reserved, and
     * GetNumberCategorizedDBRecords already calls them Unknown Database
     * Category -- so accepting a selection in one contradicted the same
     * device's own answer. */
    static const unsigned char bad_cat[] = { 0x00, 0x09, 0x0A, 0xFF };

    for (unsigned i = 0; i < sizeof(bad_cat); i++) {
        iaptest_init();
        iaptest_session_extended();
        rbstub_set_playlist(40, 3);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        iaptest_tx_clear();
        rbstub_reset_calls();

        unsigned char p[12] = { 0x04, 0x00, 0x38, 0x00, (unsigned char)(0x80 + i),
                                bad_cat[i], 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(p, 11);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectSortDBRecord(0x%02X)", bad_cat[i]);
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "SelectSortDBRecord with category 0x%02X answered "
                  "Success; the same device calls that category unknown",
                  bad_cat[i]);
        CHECK(rbstub_calls.skip == 0,
              "SelectSortDBRecord with category 0x%02X moved playback",
              bad_cat[i]);
    }

    /* Track index equal to the count is one past the end. 0x0017
     * refuses it; this used to accept it. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    {
        unsigned char p[11] = { 0x04, 0x00, 0x38, 0x00, 0x90,
                                0x05, 0x00, 0x00, 0x00, 40, 0x00 };
        iaptest_rx(p, 11);
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to SelectSortDBRecord(Track, 40)");
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "SelectSortDBRecord(Track, 40) on a 40-track queue was "
                  "accepted; the sibling 0x0017 refuses the same index");
    }

    /* A valid one still works, so the guards are not refusing
     * everything. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    {
        unsigned char p[11] = { 0x04, 0x00, 0x38, 0x00, 0x91,
                                0x05, 0x00, 0x00, 0x00, 5, 0x00 };
        iaptest_rx(p, 11);
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to a valid SelectSortDBRecord");
        if (r && r->paylen >= 6)
            CHECK_EQ_INT(r->payload[5], 0x00,
                         "SelectSortDBRecord(Track, 5) was refused");
    }
}

/* GetNumberCategorizedDBRecords answers nbr_total_playlists() + 1 for
 * the Playlist category, so the last valid index is
 * nbr_total_playlists() -- index 0 being the On-The-Go playlist.
 * RetrieveCategorizedDatabaseRecords (0x001A) bounds it with >= against
 * that same count; SelectDBRecord and SelectSortDBRecord used > , which
 * let exactly one index past the end through.
 *
 * So the same device reported n+1 records, refused index n+1 from one
 * command and accepted it from another. */
void test_playcontrol_playlist_index_bound_matches_the_count(void)
{
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* What the device says it has. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0xD0, 0x01);
    uint32_t count = 0;
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetNumberCategorizedDBRecords");
        if (!r || r->paylen < 9)
            return;
        count = ((uint32_t)r->payload[5] << 24) | ((uint32_t)r->payload[6] << 16)
              | ((uint32_t)r->payload[7] << 8)  |  (uint32_t)r->payload[8];
    }

    /* One past the end, by its own count, through both commands. */
    static const struct { unsigned char cmd; const char *name; } sel[] = {
        { 0x17, "SelectDBRecord" },
        { 0x38, "SelectSortDBRecord" },
    };
    for (unsigned i = 0; i < sizeof(sel)/sizeof(sel[0]); i++) {
        iaptest_tx_clear();
        unsigned char p[12] = {
            0x04, 0x00, sel[i].cmd, 0x00, (unsigned char)(0xD1 + i),
            0x01,
            (unsigned char)(count >> 24), (unsigned char)(count >> 16),
            (unsigned char)(count >> 8),  (unsigned char)count,
            0x00, 0x00
        };
        iaptest_rx(p, (sel[i].cmd == 0x38) ? 11 : 10);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to %s(Playlist, %u)", sel[i].name, count);
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "%s accepted playlist index %u when the device reports "
                  "only %u records, so the last valid index is %u",
                  sel[i].name, count, count, count - 1);
    }
}

/* BUTTON_REPEAT is a flag firmware/drivers/button.c synthesises for a
 * held key; it is not a button code. Putting it in iap_remotebtn made
 * button.c:429 strip it from lastbtn, so "btn != lastbtn" held on every
 * tick and each one posted a fresh keypress. One PlayControl(Stop)
 * became a burst of play/pause toggles. */
void test_playcontrol_stop_raises_one_button_not_a_burst(void)
{
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0xE1, 0x02);   /* PlayControl, Stop */

    CHECK((iap_remotebtn & BUTTON_REPEAT) == 0,
          "iap_remotebtn carries BUTTON_REPEAT (0x%08X); the button "
          "driver owns that bit and strips it from lastbtn, so the "
          "press never compares equal and repeats every tick",
          iap_remotebtn);

    /* And it clears within the tap window rather than being re-raised
     * for ever. */
    for (int t = 0; t < 12; t++)
        iap_periodic();
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "the stop button was still held twelve ticks later");
}

/* MFi 4.3.20 (p.271): "The playing track index is zero based, so the
 * valid range is from 0x0 to numPlayTracks-1 (one less than the total
 * count)."
 *
 * index below first_index is ordinary -- apps/playlist.c wraps
 * backwards under REPEAT_ALL and sets index = first_index - 1 when a
 * queue is built. iap_get_trackindex() subtracted without wrapping and
 * returned the result as unsigned, so Display Remote answered
 * 4294967294 for a track the Extended Interface called 8, on the same
 * device, in the same session. */
void test_playcontrol_track_index_wraps_below_first_index(void)
{
    iaptest_init();
    rbstub_set_playlist(10, 3);
    rbstub_set_playlist_first_index(5);   /* index 3 is before it */
    iaptest_session_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Display Remote: GetiPodStateInfo, track index. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0xF5, 0x01);
    uint32_t l3 = 0xFFFFFFFF;
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetiPodStateInfo(track index)");
        if (r && r->paylen >= 9)
            l3 = ((uint32_t)r->payload[5] << 24) | ((uint32_t)r->payload[6] << 16)
               | ((uint32_t)r->payload[7] << 8)  |  (uint32_t)r->payload[8];
    }

    /* Extended Interface: GetCurrentPlayingTrackIndex. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x1E, 0x00, 0xF6);
    uint32_t l4 = 0xFFFFFFFE;
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetCurrentPlayingTrackIndex");
        if (r && r->paylen >= 9)
            l4 = ((uint32_t)r->payload[5] << 24) | ((uint32_t)r->payload[6] << 16)
               | ((uint32_t)r->payload[7] << 8)  |  (uint32_t)r->payload[8];
    }

    CHECK(l3 == l4,
          "the two lingoes report different indices for the same track: "
          "Display Remote %u, Extended Interface %u", l3, l4);
    CHECK(l3 < 10,
          "Display Remote reported index %u on a ten-track queue; the "
          "valid range is 0 to 9", l3);
}

/* A device must give one answer about a category.
 *
 * GetNumberCategorizedDBRecords reports 0 for Genre and Composer --
 * there is no genre or composer index here -- and then
 * RetrieveCategorizedDatabaseRecords enumerated playlist_amount() "Not
 * Supported" records for them, and SelectDBRecord accepted a selection
 * and moved playback. Three answers. Audiobook and Podcast were already
 * consistent across all three; these two were not, because they sat
 * inside accepting bands written as ranges. */
void test_playcontrol_unsupported_categories_answer_once(void)
{
    static const struct { unsigned char cat; const char *name; } t[] = {
        { 0x04, "Genre" },
        { 0x06, "Composer" },
        { 0x07, "Audiobook" },   /* already consistent; the control */
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        rbstub_set_playlist(40, 3);
        iaptest_session_extended();
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);

        /* How many does it say there are? */
        iaptest_tx_clear();
        unsigned char q[6] = { 0x04, 0x00, 0x18, 0x00, 0x70, t[i].cat };
        iaptest_rx(q, sizeof(q));
        uint32_t count = 0xFFFFFFFF;
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            if (r && r->paylen >= 9)
                count = ((uint32_t)r->payload[5] << 24)
                      | ((uint32_t)r->payload[6] << 16)
                      | ((uint32_t)r->payload[7] << 8)
                      |  (uint32_t)r->payload[8];
        }
        CHECK(count == 0, "%s: record count is %u, not 0",
              t[i].name, count);

        /* Then it must not enumerate any. */
        iaptest_tx_clear();
        unsigned char e[14] = { 0x04, 0x00, 0x1A, 0x00, 0x71, t[i].cat,
                                0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x01 };
        iaptest_rx(e, sizeof(e));
        for (int k = 0; k < iaptest_tx_count(); k++) {
            const struct iaptest_pkt *r = iaptest_tx(k);
            if (!r || r->paylen < 3)
                continue;
            CHECK(!(r->payload[0] == 0x04 && r->payload[1] == 0x00
                    && r->payload[2] == 0x1B),
                  "%s reports 0 records and then enumerated one",
                  t[i].name);
        }

        /* And it must not accept a selection, nor move playback. */
        iaptest_tx_clear();
        rbstub_reset_calls();
        unsigned char sel[10] = { 0x04, 0x00, 0x17, 0x00, 0x72, t[i].cat,
                                  0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(sel, sizeof(sel));
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL, "%s: no reply to SelectDBRecord", t[i].name);
            if (r && r->paylen >= 6)
                CHECK(r->payload[5] != 0x00,
                      "%s reports 0 records and then accepted a "
                      "selection in it", t[i].name);
        }
        CHECK(rbstub_calls.skip == 0,
              "%s: selecting a category with no records moved playback",
              t[i].name);
    }
}

/* PlayControl Stop stops.
 *
 * MFi Table 5-48 (p.429) gives code 0x02 as Stop, and 5.1.37 (p.428):
 * "If the Apple device does not enter the requested state successfully,
 * an error status is returned."
 *
 * It used to raise BUTTON_RC_PLAY, which keymap-ipod.c binds to
 * ACTION_WPS_PLAY -- a toggle. So Stop on a paused device started
 * playback and was acked Success. Neither half of that was visible
 * here: the reserved-code case deliberately skips 0x02, and no case
 * read the play state back after sending it.
 */
void test_playcontrol_stop_actually_stops(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    /* Paused is the state that made the old behaviour a resume. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    rbstub_reset_calls();
    iaptest_tx_clear();

    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x40, 0x02);

    CHECK(rbstub_calls.stop == 1,
          "PlayControl Stop called audio_stop() %d times",
          rbstub_calls.stop);
    CHECK(rbstub_calls.resume == 0,
          "PlayControl Stop resumed playback %d times",
          rbstub_calls.resume);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "PlayControl Stop raised a remote button; the Playback "
                 "Engine has no Stop button and every code it does have "
                 "means something else");

    /* Table 5-3 (p.403): the Extended Interface ack is command 0x0001,
     * then status, then the two-byte command being acknowledged. Under
     * IDPS the transaction ID sits between. */
    bool acked_ok = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 8 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x01
            && p->payload[5] == 0x00
            && p->payload[6] == 0x00 && p->payload[7] == 0x29)
            acked_ok = true;
    }
    CHECK(acked_ok,
          "PlayControl Stop was not acknowledged Success once the "
          "device was stopped");
}

/* Display Remote can seek too.
 *
 * MFi Table 4-74 (p.267) describes SetiPodStateInfo info type 0x03 as
 * "The play status of the Apple device (play, pause, stop, FF or REW)",
 * and Table 4-62 (p.262) gives the values: "0x03 Fast forward (FF),
 * 0x04 Fast rewind (REW), 0x05 End fast forward or rewind mode".
 *
 * All three were answered Command Failed, so a head unit speaking only
 * Display Remote had no seek at all -- while one speaking Extended
 * Interface has had PlayControl's since it was written. Both now go
 * through the same helpers, so they cannot drift. */
void test_playcontrol_display_remote_can_seek(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    struct { unsigned char val; unsigned long btn; const char *what; } fwd[] = {
        { 0x03, BUTTON_RC_RIGHT, "Fast forward" },
        { 0x04, BUTTON_RC_LEFT,  "Fast rewind"  },
    };

    for (unsigned i = 0; i < sizeof(fwd)/sizeof(fwd[0]); i++) {
        iap_remotebtn = BUTTON_NONE;
        iaptest_tx_clear();
        {
            unsigned char p[4] = { 0x03, 0x0E, 0x03, fwd[i].val };
            iaptest_rx(p, sizeof(p));
        }

        CHECK_EQ_INT(iap_remotebtn, fwd[i].btn, fwd[i].what);
        CHECK(iap_timeoutbtn > 3,
              "%s must be held, not tapped -- a seek runs until the "
              "accessory ends it (iap_timeoutbtn = %d)",
              fwd[i].what, iap_timeoutbtn);
        CHECK_EQ_INT(device.pb_seeking, i == 0 ? 0x02 : 0x03,
                     "the seek direction was not recorded");

        /* Table 4-49 (p.250): the Display Remote ack is 0x03 0x00,
         * then status and the command being acknowledged. */
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 4 && r->payload[0] == 0x03
              && r->payload[1] == 0x00
              && r->payload[r->paylen - 2] == 0x00
              && r->payload[r->paylen - 1] == 0x0E,
              "%s was not acknowledged Success", fwd[i].what);

        /* End it, so the next iteration starts clean. */
        iaptest_button_sample(4);
        iaptest_tx_clear();
        {
            unsigned char p[4] = { 0x03, 0x0E, 0x03, 0x05 };
            iaptest_rx(p, sizeof(p));
        }
        CHECK_EQ_INT(device.pb_seeking, 0,
                     "End fast forward or rewind did not clear the seek");
        CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                     "the seek button was not released");
        iaptest_button_sample(4);
    }
}

/* A skip with nothing playing is an error, not a Success.
 *
 * MFi p.429, on PlayControl: "The iPod models before the 2G nano
 * (09/2006) always return a successful status. Starting with the 2G
 * nano, Apple devices return the actual play control status. This means
 * that a next or previous track command will return an error if no
 * media is playing."
 *
 * This device identifies as a Classic, long after that. It returned
 * Success and raised a button that does nothing while stopped, so the
 * head unit was told a track change had happened and then saw no
 * notification for one.
 *
 * The suite pinned that: two cases sent skips without ever setting the
 * play state, and the stub leaves it stopped. */
void test_playcontrol_skip_while_stopped_is_refused(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    static const struct { unsigned char code; const char *what; } skips[] = {
        /* Toggle Play/Pause is here for a sharper reason than the
         * skips. It raises BUTTON_RC_PLAY, and keymap-ipod.c:320 binds
         * that bare to ACTION_STD_OK outside the WPS -- so a head unit
         * sending its most common command to a stopped device activated
         * whatever the user had highlighted in the browser. */
        { 0x01, "Toggle Play/Pause (0x01)" },
        { 0x03, "Next Track (0x03)"     },
        { 0x04, "Previous Track (0x04)" },
        { 0x08, "Next (0x08)"           },
        { 0x09, "Previous (0x09)"       },
    };

    for (unsigned i = 0; i < sizeof(skips)/sizeof(skips[0]); i++) {
        /* Playing first, so the refusal below is about the play state
         * and not about the code. */
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        iap_remotebtn = BUTTON_NONE;
        rbstub_reset_calls();
        iaptest_tx_clear();
        {
            unsigned char p[6] = { 0x04, 0x00, 0x29,
                                   0x00, (unsigned char)(0xC0 + i),
                                   skips[i].code };
            iaptest_rx(p, sizeof(p));
        }
        CHECK(rbstub_calls.skip + rbstub_calls.pause
              + rbstub_calls.resume > 0,
              "%s did nothing while playing", skips[i].what);
        iaptest_button_sample(4);

        /* Stopped. */
        rbstub_set_audio_status(0);
        iap_remotebtn = BUTTON_NONE;
        rbstub_reset_calls();
        iaptest_tx_clear();
        {
            unsigned char p[6] = { 0x04, 0x00, 0x29,
                                   0x00, (unsigned char)(0xD0 + i),
                                   skips[i].code };
            iaptest_rx(p, sizeof(p));
        }

        CHECK_EQ_INT(rbstub_calls.skip, 0,
                     "a skip with nothing playing moved the player");

        bool refused = false;
        for (int j = 0; j < iaptest_tx_count(); j++) {
            const struct iaptest_pkt *p = iaptest_tx(j);
            if (p && p->paylen >= 8 && p->payload[0] == 0x04
                && p->payload[1] == 0x00 && p->payload[2] == 0x01
                && p->payload[5] != 0x00
                && p->payload[6] == 0x00 && p->payload[7] == 0x29)
                refused = true;
        }
        CHECK(refused, "%s with nothing playing was not refused",
              skips[i].what);
        iaptest_button_sample(4);
    }
}

/* SetCurrentPlayingTrack while stopped fails.
 *
 * MFi 5.1.51 (p.442): "This command is usable only when the Apple
 * device is in a playing or paused state. If the Apple device is
 * stopped, this command fails."
 *
 * It was acked Success, and playback.c's skip entry points early-return
 * while stopped -- so the skip was a no-op and the head unit's display
 * moved to a track the device had not selected and would not report.
 * The existing case quotes that note in a comment and then sets a
 * playing state, so the sentence was documented and untested. */
void test_playcontrol_set_track_while_stopped_fails(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    /* Playing: it works, so the refusal below is about the state. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x37, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x05);
    CHECK(rbstub_calls.skip == 1,
          "SetCurrentPlayingTrack did not skip while playing (%d)",
          rbstub_calls.skip);

    /* Stopped. */
    rbstub_set_audio_status(0);
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x37, 0x00, 0xE1, 0x00, 0x00, 0x00, 0x07);

    CHECK_EQ_INT(rbstub_calls.skip, 0,
                 "SetCurrentPlayingTrack skipped while stopped");

    bool refused = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 8 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x01
            && p->payload[5] != 0x00
            && p->payload[6] == 0x00 && p->payload[7] == 0x37)
            refused = true;
    }
    CHECK(refused,
          "SetCurrentPlayingTrack while stopped was acked Success");
}

/* Playback can be started from a stopped device.
 *
 * SelectDBRecord and PlayCurrentSelection both did their work with
 * audio_pause(), audio_skip() and audio_resume(), and all three
 * early-return when the Playback Engine is stopped -- playback.c's
 * audio_on_pause() and audio_on_skip(). So from that state they moved
 * nothing and acked Success, and PlayCurrentSelection reshuffled the
 * queue on the way: the head unit picked a track, heard silence, and
 * had its play order destroyed.
 *
 * PlayControl Play (0x0A) is protocol 1.13, above the 1.12 this device
 * advertises, so its default arm refuses it -- these two are the only
 * ways an accessory can start playback at all.
 *
 * Unreachable until PlayControl Stop began really stopping, which is
 * this campaign's own doing. And invisible while the stub's
 * audio_resume() set PLAY unconditionally. */
void test_playcontrol_can_start_from_stopped(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(40, 10);
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0x90, 0x05);   /* category count */

    /* Stop, the way an accessory does. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x91, 0x02);
    CHECK_EQ_INT(iap_play_state_byte(), 0x00,
                 "PlayControl Stop did not stop, so nothing below is "
                 "being tested");
    iaptest_button_sample(4);

    /* SelectDBRecord(Track, 7) must start it. */
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x92, 0x05, 0x00, 0x00, 0x00, 0x07);
    CHECK(rbstub_calls.playlist_start > 0,
          "SelectDBRecord from a stopped device started nothing");
    CHECK_EQ_INT(rbstub_calls.last_start_index, 7,
                 "it started at the wrong track");
    CHECK_EQ_INT(iap_play_state_byte(), 0x01,
                 "the device is still not playing");

    /* And so must PlayCurrentSelection. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x93, 0x02);   /* stop again */
    iaptest_button_sample(4);
    CHECK_EQ_INT(iap_play_state_byte(), 0x00, "not stopped");

    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x28, 0x00, 0x94, 0x00, 0x00, 0x00, 0x03);
    CHECK(rbstub_calls.playlist_start > 0,
          "PlayCurrentSelection from a stopped device started nothing");
    CHECK_EQ_INT(rbstub_calls.last_start_index, 3,
                 "it started at the wrong track");
    CHECK_EQ_INT(iap_play_state_byte(), 0x01,
                 "the device is still not playing");
}

/* A seek with nothing playing is refused, on both lingoes.
 *
 * MFi 5.1.37 (p.428): "If the Apple device does not enter the requested
 * state successfully, an error status is returned." The skip arms
 * beside PlayControl's seek arms test the play state; the seek arms did
 * not.
 *
 * A seek is worse than a skip when it is wrong. It holds the button for
 * IAP_BTN_HELD -- about ten seconds -- and
 * button-clickwheel.c:479 returns "int_btn | remote_control_rx()", so
 * BUTTON_RC_RIGHT is ORed into every physical button read for that
 * whole time. keymap-ipod.c maps it to ACTION_STD_NEXT in the browser,
 * so a seek sent to a stopped device scrolls the user's file list on
 * its own until the timer lapses. */
void test_playcontrol_seek_while_stopped_is_refused(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    /* Extended Interface, PlayControl 0x05 Begin FF. */
    rbstub_set_audio_status(0);
    iap_remotebtn = BUTTON_NONE;
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0xA0, 0x05);

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a seek with nothing playing raised a button, which "
                 "the clickwheel driver ORs into every physical read "
                 "for the next ten seconds");
    CHECK_EQ_INT(device.pb_seeking, 0,
                 "a seek that could not start was recorded as running");

    bool refused = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 8 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x01
            && p->payload[5] != 0x00
            && p->payload[6] == 0x00 && p->payload[7] == 0x29)
            refused = true;
    }
    CHECK(refused, "PlayControl Begin FF while stopped was not refused");

    /* Display Remote, SetiPodStateInfo play status 0x03 Fast forward. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    rbstub_set_audio_status(0);
    iap_remotebtn = BUTTON_NONE;
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x03, 0x03);

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a Display Remote seek with nothing playing raised a "
                 "button");
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 4 && r->payload[0] == 0x03
              && r->payload[1] == 0x00
              && r->payload[r->paylen - 2] != 0x00,
              "the Display Remote seek while stopped was not refused");
    }

    /* And playing, it works -- so the refusals above are about the
     * state and not the command. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iap_remotebtn = BUTTON_NONE;
    IAPTEST_RX(0x03, 0x0E, 0x03, 0x03);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_RIGHT,
                 "a seek while playing must start");
}

/* Display Remote reports a seek as a seek.
 *
 * MFi Table 4-62 (p.262): "0x00 Playback stopped, 0x01 Playing, 0x02
 * Playback paused, 0x03 Fast forward (FF), 0x04 Fast rewind (REW), 0x05
 * End fast forward or rewind mode."
 *
 * All three places that report the play status used
 * iap_play_state_byte(), which stops at 0x02 -- one of them with a
 * "TODO: Handle FF/REW" on it. An accessory seeking saw plain Playing,
 * so its display never showed the seek it had just asked for. */
void test_playcontrol_display_remote_reports_a_seek(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    static const struct { unsigned char cmd; unsigned char want;
                          const char *what; } tc[] = {
        { 0x03, 0x03, "Fast forward" },
        { 0x04, 0x04, "Fast rewind"  },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        /* Start the seek. IAPTEST_RX() wants compile-time bytes. */
        {
            unsigned char p[4] = { 0x03, 0x0E, 0x03, tc[i].cmd };
            iaptest_rx(p, sizeof(p));
        }
        /* Let the raised button go out without ending the seek:
         * remote_control_rx() drains iap_repeatbtn and leaves
         * iap_timeoutbtn, which is IAP_BTN_HELD for a seek. Without
         * this the query below is re-queued rather than handled. */
        iaptest_button_sample(2);

        /* GetiPodStateInfo, info type 0x03 Play status. */
        iaptest_tx_clear();
        IAPTEST_RX(0x03, 0x0C, 0x03);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL && r->paylen >= 4 && r->payload[0] == 0x03
                  && r->payload[1] == 0x0D,
                  "%s: no RetiPodStateInfo", tc[i].what);
            if (r && r->paylen >= 4)
                CHECK_EQ_INT(r->payload[3], tc[i].want, tc[i].what);
        }

        /* End it, and the status goes back to Playing. */
        IAPTEST_RX(0x03, 0x0E, 0x03, 0x05);
        iaptest_button_sample(2);
        iaptest_tx_clear();
        IAPTEST_RX(0x03, 0x0C, 0x03);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            if (r && r->paylen >= 4)
                CHECK_EQ_INT(r->payload[3], 0x01,
                             "after the seek ends it is playing again");
        }
        iaptest_button_sample(4);
    }
}

/* A seek ends when the button it holds does.
 *
 * device.pb_seeking was written in three places and cleared in one --
 * iap_seek_stop() -- so a seek that ended any other way left it set for
 * ever, and iap_play_state_reported() went on answering Fast forward to
 * every GetPlayStatus. iap-lingo3.c also gates the track fields of
 * RetPlayStatus on the state not being Stopped, so the reply carried an
 * index, a length and a position for a device that was not playing.
 *
 * Three routes end a seek without an EndFFRew: the ten-second safety
 * release, a Simple Remote all-zero release (which MFi 4.2.7 (p.226)
 * requires the accessory to send), and PlayControl Stop. */
static unsigned char reported_play_state(void)
{
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x03);
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 4 && p->payload[0] == 0x03
            && p->payload[1] == 0x0D)
            return p->payload[3];
    }
    return 0xFF;
}

void test_playcontrol_seek_state_does_not_outlive_the_button(void)
{
    /* The safety release. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    IAPTEST_RX(0x03, 0x0E, 0x03, 0x03);         /* Fast forward */
    iaptest_button_sample(2);
    CHECK_EQ_INT(reported_play_state(), 0x03,
                 "the seek was not reported while it was running");

    for (int t = 0; t < 130; t++)               /* past IAP_BTN_HELD */
        iap_periodic();
    CHECK_EQ_INT(device.pb_seeking, 0,
                 "the ten-second safety release left the seek state set");
    CHECK_EQ_INT(reported_play_state(), 0x01,
                 "play status still reports Fast forward after the seek "
                 "timed out");

    /* A Simple Remote all-zero release. */
    IAPTEST_RX(0x03, 0x0E, 0x03, 0x03);
    iaptest_button_sample(2);
    CHECK_EQ_INT(device.pb_seeking, 0x02, "the seek did not start");
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(device.pb_seeking, 0,
                 "an all-zero button status left the seek state set");
    iaptest_button_sample(4);

    /* PlayControl Stop. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x40, 0x05);   /* Begin FF */
    iaptest_button_sample(2);
    CHECK_EQ_INT(device.pb_seeking, 0x02, "the seek did not start");

    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x41, 0x02);   /* Stop */
    CHECK_EQ_INT(device.pb_seeking, 0,
                 "PlayControl Stop left the seek state set");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "PlayControl Stop left the seek button asserted into "
                 "the stopped UI");
}

/* Every command that reads a track by index goes through
 * iap_get_trackinfo(), which converts the accessory's index into a
 * playlist one and then reads it. That conversion is the only place
 * that can range-check the index, and until this commit it was also
 * the one place that ignored playlist_get_track_info()'s return: three
 * callers probed with the unconverted index first, then called this,
 * which read a different track and reported whatever came back.
 *
 * MFi 5.1.13 (p.408) for 0x000C, 5.1.29 (p.421) for the sweep, and the
 * Display Remote sibling in C.4: an index that cannot be read earns an
 * acknowledgement with an error status, not a reply built from an
 * mp3entry nobody filled in. */
void test_playcontrol_unreadable_track_is_refused(void)
{
    /* Extended Interface, GetIndexedPlayingTrackInfo. Track 5 in a
     * 40-track queue playing track 3, so the current-track fast path
     * cannot answer it and the read has to reach the playlist. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_fail_track_info(true);
    iaptest_tx_clear();

    unsigned char p[12] = { 0x04, 0x00, 0x0C, 0x00, 0x60, 0x01,
                            0x00, 0x00, 0x00, 0x05, 0x00, 0x00 };
    iaptest_rx(p, sizeof(p));
    rbstub_fail_track_info(false);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to GetIndexedPlayingTrackInfo for a track "
                     "that could not be read");
    if (r && r->paylen >= 3)
        CHECK(r->payload[1] == 0x00 && r->payload[2] == 0x01,
              "GetIndexedPlayingTrackInfo answered 0x%02X%02X for a track "
              "that could not be read, not an error acknowledgement",
              r->payload[1], r->payload[2]);

    iaptest_tx_clear();
    rbstub_fail_metadata(true);
    unsigned char pf[9] = { 0x04, 0x00, 0x20, 0x00, 0x64,
                            0x00, 0x00, 0x00, 0x05 };
    iaptest_rx(pf, sizeof(pf));
    rbstub_fail_metadata(false);

    r = iaptest_tx(0);
    CHECK(r != NULL, "no reply when indexed metadata could not be parsed");
    if (r && r->paylen >= 3)
        CHECK(r->payload[1] == 0x00 && r->payload[2] == 0x01,
              "GetIndexedPlayingTrackTitle treated a metadata failure "
              "as a successful reply");

    /* Display Remote's own GetIndexedPlayingTrackInfo, same shape. It
     * had the same probe in the same wrong index space. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(40, 3);
    rbstub_fail_track_info(true);
    iaptest_tx_clear();

    unsigned char q[11] = { 0x03, 0x12, 0x00, 0x61, 0x00,
                            0x00, 0x00, 0x00, 0x05, 0x00, 0x00 };
    iaptest_rx(q, sizeof(q));
    rbstub_fail_track_info(false);

    r = iaptest_tx(0);
    CHECK(r != NULL, "no reply to lingo 3 GetIndexedPlayingTrackInfo for "
                     "a track that could not be read");
    if (r && r->paylen >= 2)
        CHECK(r->payload[0] == 0x03 && r->payload[1] == 0x00,
              "lingo 3 GetIndexedPlayingTrackInfo answered 0x%02X%02X for "
              "a track that could not be read, not an acknowledgement",
              r->payload[0], r->payload[1]);

    iaptest_tx_clear();
    rbstub_fail_metadata(true);
    unsigned char qf[11] = { 0x03, 0x12, 0x00, 0x65, 0x03,
                             0x00, 0x00, 0x00, 0x05, 0x00, 0x00 };
    iaptest_rx(qf, sizeof(qf));
    rbstub_fail_metadata(false);

    r = iaptest_tx(0);
    CHECK(r != NULL, "no Display Remote reply after a metadata failure");
    if (r && r->paylen >= 2)
        CHECK(r->payload[0] == 0x03 && r->payload[1] == 0x00,
              "Display Remote treated a metadata failure as success");

    /* RetrieveCategorizedDatabaseRecords over the Track category. This
     * one cannot refuse a single bad row -- it is answering a range --
     * so it owes the accessory an empty name rather than the previous
     * row's, or an mp3entry that was never written. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x18, 0x00, 0x62, 0x05,
               0xFF, 0xFF, 0xFF, 0xFF);
    iaptest_tx_clear();
    rbstub_fail_track_info(true);

    unsigned char s[14] = { 0x04, 0x00, 0x1A, 0x00, 0x63, 0x05,
                            0x00, 0x00, 0x00, 0x05,
                            0x00, 0x00, 0x00, 0x01 };
    iaptest_rx(s, sizeof(s));
    rbstub_fail_track_info(false);

    r = iaptest_tx(0);
    CHECK(r != NULL, "no ReturnCategorizedDatabaseRecord for an unreadable "
                     "track");
    if (r && r->paylen >= 8) {
        CHECK(r->payload[1] == 0x00 && r->payload[2] == 0x1B,
              "RetrieveCategorizedDatabaseRecords answered 0x%02X%02X",
              r->payload[1], r->payload[2]);
        CHECK(r->payload[r->paylen - 1] == 0x00,
              "the record name is not NUL-terminated");
        CHECK(r->payload[9] == 0x00,
              "an unreadable track was named \"%s\" -- that string came "
              "from an mp3entry nobody filled in",
              (const char *)&r->payload[9]);
    }
}

/* Table 4-62 (p.262), the play-status values, annotates one of them for
 * this command alone: "0x01 Playing (for Command 0x0E:
 * SetiPodStateInfo (page 266), start or resume playback)".
 *
 * The arm called audio_resume(), which apps/playback.c:3187 makes a
 * no-op on a stopped engine -- "if (play_status == PLAY_STOPPED || ...)
 * return;" -- and then acknowledged Success. That is the Play button of
 * every head unit speaking only General and Display Remote, in the
 * ordinary case of docking an idle iPod: press it, get Success, hear
 * nothing. iap-lingo4.c's SetCurrentPlayingTrack was fixed for the same
 * thing in 514204a8c6. */
void test_playcontrol_display_remote_play_starts_from_stopped(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(0);         /* stopped */
    rbstub_reset_calls();
    iaptest_tx_clear();

    IAPTEST_RX(0x03, 0x0E, 0x00, 0xC1, 0x03, 0x01);

    CHECK(rbstub_calls.playlist_start == 1,
          "SetiPodStateInfo play status 0x01 on a stopped player called "
          "playlist_start() %d times -- audio_resume() alone returns at "
          "apps/playback.c:3187 and nothing starts",
          rbstub_calls.playlist_start);
    CHECK_EQ_INT(rbstub_calls.last_start_index, 3,
                 "playback started from the wrong track");

    /* Under IDPS an iPodAck is lingo, command, two transaction bytes,
     * status, command -- so the status is payload[4]. Reading payload[2]
     * reads the transaction ID's high byte, which is zero here, and the
     * assertion passes whatever the device answered. That is how the
     * first draft of this case reported Success for a Command Failed. */
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no answer to SetiPodStateInfo");
    if (r && r->paylen >= 6)
        CHECK_EQ_INT(r->payload[4], 0x00,
                     "the command that did start playback did not answer "
                     "Success");

    /* Paused is a resume, not a restart: playlist_start() there would
     * throw away the position. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    rbstub_reset_calls();

    IAPTEST_RX(0x03, 0x0E, 0x00, 0xC2, 0x03, 0x01);
    CHECK_EQ_INT(rbstub_calls.playlist_start, 0,
                 "a paused player was restarted from the top rather than "
                 "resumed");
    CHECK(rbstub_calls.resume == 1,
          "a paused player was not resumed (%d calls)",
          rbstub_calls.resume);

    /* And with nothing loaded there is nothing to start. MFi 4.3.17
     * (p.266) has the ack carry "the results of the operation". */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(0, 0);
    rbstub_set_audio_status(0);
    rbstub_reset_calls();
    iaptest_tx_clear();

    IAPTEST_RX(0x03, 0x0E, 0x00, 0xC3, 0x03, 0x01);
    CHECK_EQ_INT(rbstub_calls.playlist_start, 0,
                 "an empty playlist was started");
    r = iaptest_tx(0);
    CHECK(r != NULL, "no answer to SetiPodStateInfo with nothing loaded");
    if (r && r->paylen >= 6)
        CHECK(r->payload[4] != 0x00,
              "Play with nothing loaded answered status 0x%02X -- Success "
              "for an operation that could not happen", r->payload[4]);
}

/* Table 4-62 (p.262) lists 0x00 Stop, 0x01 Playing and 0x02 Paused as
 * peers of 0x03 Fast forward and 0x04 Rewind. Nothing obliges an
 * accessory to send 0x05 EndFFRew before changing transport state, and
 * a head unit whose Stop button is a Stop button will not.
 *
 * None of the three arms ended the seek. iap_seek_start() leaves
 * iap_remotebtn holding BUTTON_RC_RIGHT and iap_timeoutbtn at
 * IAP_BTN_HELD -- ten seconds at the 10 Hz tick -- so until the safety
 * release in iap_periodic() fired, iap_play_state_reported() answered
 * Fast forward for a player that had stopped, the phantom button was
 * ORed into the user's own input at button-clickwheel.c:479 and their
 * browser scrolled by itself, and the accessory never received the
 * seek-stop it was owed. iap-lingo4.c:2687 already did this. */
void test_playcontrol_transport_change_ends_a_seek(void)
{
    static const struct { unsigned char v; const char *what; } t[] = {
        { 0x00, "Stop" }, { 0x01, "Play" }, { 0x02, "Pause" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        iaptest_enter_idps();
        iaptest_force_authenticated();
        rbstub_set_playlist(20, 3);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);

        /* Fast forward. */
        IAPTEST_RX(0x03, 0x0E, 0x00, 0xD0, 0x03, 0x03);
        iaptest_button_sample(2);
        CHECK(device.pb_seeking != 0,
              "the seek did not start, so the %s below would prove "
              "nothing", t[i].what);

        unsigned char p[6] = { 0x03, 0x0E, 0x00, 0xD1, 0x03, t[i].v };
        iaptest_rx(p, sizeof(p));

        CHECK(device.pb_seeking == 0,
              "%s left the seek latched -- the device goes on reporting "
              "Fast forward and holding a button the accessory has let "
              "go of", t[i].what);
        CHECK(iap_timeoutbtn == 0,
              "%s left the ten-second button timer armed at %d",
              t[i].what, iap_timeoutbtn);

        /* And the state it reports afterwards is a real one. */
        iaptest_button_sample(2);
        iaptest_tx_clear();
        IAPTEST_RX(0x03, 0x1D, 0x00, 0xD2);
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 5 && r->payload[1] == 0x1E)
            CHECK(r->payload[4] <= 0x02,
                  "after %s the device reported play status 0x%02X, "
                  "which is a seek", t[i].what, r->payload[4]);
    }
}

/* seek_to_playlist() used to call ft_play_playlist(), which is the file
 * browser's entry point and not a library call. Two of the three things
 * it does wait for a human:
 *
 *   warn_on_pl_erase() (apps/misc.c:189) runs gui_syncyesno_run(), and
 *   that is gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, ...)
 *   (apps/gui/yesno.c:367) -- a modal dialog with no timeout. Its gate
 *   includes global_settings.warnon_erase_dynplaylist, default true
 *   (apps/settings_list.c:1971). bookmark_autoload() is a second event
 *   loop on the same path.
 *
 * So a user who had queued anything, docked, and picked a playlist on
 * the head unit put the iAP thread inside a yes/no prompt on the iPod's
 * own screen: no iPodAck, no notifications, no iap_timeoutbtn
 * countdown, every queued packet stalled behind it.
 *
 * The suite cannot model a modal dialog. What it can hold is the
 * boundary: apps/iap must not reach the browser, and the command has to
 * answer for a load that failed -- MFi 5.1.20 (p.415) tells accessories
 * to "pay close attention to the iPodAck returned by the SelectDBRecord
 * command". */
void test_playcontrol_playlist_selection_stays_off_the_browser(void)
{
    static const char *cat[] = { "Road trip.m3u", "Focus.m3u8" };

    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_playlist_catalog(cat, 2);
    rbstub_reset_calls();
    iaptest_tx_clear();

    /* SelectDBRecord, category 0x01 Playlist, index 1. Index 0 is the
     * all-tracks entry the handler skips, so 1 is the first real one. */
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0xE0, 0x01,
               0x00, 0x00, 0x00, 0x01);

    CHECK_EQ_INT(rbstub_calls.play_playlist, 1,
                 "the playlist was not loaded");
    CHECK(rbstub_calls.playlist_start >= 1,
          "the loaded playlist was not started");

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no answer to SelectDBRecord");
    if (r && r->paylen >= 6)
        CHECK_EQ_INT(r->payload[5], 0x00,
                     "a playlist that loaded did not answer Success");

    /* A catalogue entry that has gone earns an error, not Success. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_playlist_catalog(cat, 2);
    rbstub_fail_playlist_create(true);
    rbstub_reset_calls();
    iaptest_tx_clear();

    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0xE1, 0x01,
               0x00, 0x00, 0x00, 0x01);
    rbstub_fail_playlist_create(false);

    CHECK_EQ_INT(rbstub_calls.playlist_start, 0,
                 "a catalogue entry that has gone was started anyway -- "
                 "playlist_create() returns 0 with an empty queue for "
                 "one, so the -1 test alone cannot see it");
    r = iaptest_tx(0);
    CHECK(r != NULL, "no answer to SelectDBRecord for a missing playlist");
    if (r && r->paylen >= 6)
        CHECK(r->payload[5] != 0x00,
              "a playlist that could not be loaded answered Success "
              "(status 0x%02X)", r->payload[5]);

    /* And nothing in apps/iap reaches the browser entry point. */
    CHECK_EQ_INT(rbstub_calls.ft_play_playlist, 0,
                 "ft_play_playlist() was called -- it blocks on a modal "
                 "dialog and cannot run on the iAP thread");
}

/* MFi 1.11.2.1.1 (p.58): "If shuffle mode is on, a SelectDBRecord
 * packet with an index of 0 or greater causes the playlist to be
 * shuffled randomly but with the first track set to the selected
 * index", with a worked example. The description quoted in
 * iap-lingo4.c above PlayCurrentSelection says the same thing in more
 * words: "If a track index of n is sent, the nth track of the selected
 * tracks is played first, regardless of where it is located in the Now
 * Playing playlist after the shuffle is performed."
 *
 * The handler shuffled first and indexed afterwards, so n addressed the
 * shuffled queue rather than the one the accessory had enumerated with
 * RetrieveCategorizedDatabaseRecords -- the user picked a row and got a
 * random track. The seed is current_tick, so it was a fresh random
 * order every time.
 *
 * With shuffle off it called playlist_sort(), which nothing asks for:
 * the accessory's index already refers to the order it enumerated, and
 * the sort throws away a queue the user shuffled from Rockbox's own UI. */
void test_playcontrol_play_selection_selects_before_shuffling(void)
{
    /* Shuffle on, stopped. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(0);
    global_settings.playlist_shuffle = 1;
    rbstub_reset_calls();

    IAPTEST_RX(0x04, 0x00, 0x28, 0x00, 0xF1, 0x00, 0x00, 0x00, 0x05);

    CHECK_EQ_INT(rbstub_calls.playlist_start, 1,
                 "PlayCurrentSelection did not start the selected track");
    CHECK_EQ_INT(rbstub_calls.last_start_index, 5,
                 "the wrong track was selected");
    CHECK_EQ_INT(rbstub_calls.randomise, 1,
                 "shuffle mode was on and the queue was not shuffled");
    CHECK(rbstub_calls.seq_start < rbstub_calls.seq_randomise,
          "the queue was shuffled before the track was selected, so the "
          "index addressed the shuffled order and not the one the "
          "accessory enumerated (start at step %d, shuffle at step %d)",
          rbstub_calls.seq_start, rbstub_calls.seq_randomise);
    CHECK_EQ_INT(rbstub_calls.sort, 0,
                 "the queue was sorted with shuffle mode on");

    /* Shuffle off: no reorder at all. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(0);
    global_settings.playlist_shuffle = 0;
    rbstub_reset_calls();

    IAPTEST_RX(0x04, 0x00, 0x28, 0x00, 0xF2, 0x00, 0x00, 0x00, 0x05);

    CHECK_EQ_INT(rbstub_calls.last_start_index, 5,
                 "the wrong track was selected with shuffle off");
    CHECK_EQ_INT(rbstub_calls.sort, 0,
                 "shuffle mode was off and the queue was re-sorted "
                 "anyway -- that discards an order the user may have "
                 "set from Rockbox's own UI");
    CHECK_EQ_INT(rbstub_calls.randomise, 0,
                 "shuffle mode was off and the queue was shuffled");

    /* Already playing: the same order, through the skip path. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.playlist_shuffle = 1;
    rbstub_reset_calls();

    IAPTEST_RX(0x04, 0x00, 0x28, 0x00, 0xF3, 0x00, 0x00, 0x00, 0x07);

    CHECK_EQ_INT(rbstub_calls.last_skip, 4,
                 "the skip did not land on the selected track");
    CHECK(rbstub_calls.seq_randomise > 0
          && rbstub_calls.seq_randomise > rbstub_calls.seq_start,
          "the shuffle did not follow the selection while playing");
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "PlayCurrentSelection did not bracket its skip");
    CHECK_EQ_INT(rbstub_calls.resume, 1,
                 "PlayCurrentSelection left the player paused");
}

/* The suite checked that commands are acknowledged far more than that
 * they do anything. A mutation sweep over the playback calls
 * themselves -- audio_pause/resume/stop/skip/ff_rewind, playlist_start,
 * playlist_sort/randomise, setvol -- found 36 of 45 could be deleted
 * with all seven binaries green. Display Remote's Stop could stop
 * calling audio_stop() and still answer Success.
 *
 * These four cases assert the effect, per family. This one is Display
 * Remote: SetiPodStateInfo (Table 4-74, pp.266-268) and
 * SetCurrentPlayingTrack (0x11). */
void test_playcontrol_display_remote_commands_have_effects(void)
{
    /* Play status Stop (Table 4-62, p.262). */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x31, 0x03, 0x00);
    CHECK_EQ_INT(rbstub_calls.stop, 1,
                 "SetiPodStateInfo play status Stop acked Success and did "
                 "not stop the player");

    /* Play status Pause. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x32, 0x03, 0x02);
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "SetiPodStateInfo play status Pause did not pause");

    /* Track index (info type 0x01), four bytes. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x33, 0x01, 0x00, 0x00, 0x00, 0x07);
    CHECK_EQ_INT(rbstub_calls.skip, 1,
                 "SetiPodStateInfo track index did not move the player");
    CHECK_EQ_INT(rbstub_calls.last_skip, 4,
                 "the track index skip went the wrong distance");

    /* Track position in milliseconds (0x00), four bytes. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x34, 0x00, 0x00, 0x00, 0x27, 0x10);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 1,
                 "SetiPodStateInfo track position (ms) did not seek");

    /* Track position in seconds (0x0F), two bytes. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x35, 0x0F, 0x00, 0x0A);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 1,
                 "SetiPodStateInfo track position (s) did not seek");

    /* SetCurrentPlayingTrack (0x11), four bytes. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x11, 0x00, 0x36, 0x00, 0x00, 0x00, 0x02);
    CHECK_EQ_INT(rbstub_calls.skip, 1,
                 "SetCurrentPlayingTrack did not move the player");
    CHECK_EQ_INT(rbstub_calls.last_skip, -1,
                 "SetCurrentPlayingTrack skipped the wrong distance");
}

/* Extended Interface, same question: does the command do anything.
 * SelectDBRecord (0x0017), PlayControl Stop (0x0029/0x02),
 * SetCurrentPlayingTrack (0x0037) and SelectSortDBRecord (0x0038). */
void test_playcontrol_extended_commands_have_effects(void)
{
    static const char *cat[] = { "Road trip.m3u", "Focus.m3u8" };

    /* SelectDBRecord, Track category, from stopped: a start. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(0);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x40, 0x05,
               0x00, 0x00, 0x00, 0x06);
    CHECK_EQ_INT(rbstub_calls.playlist_start, 1,
                 "SelectDBRecord(Track) from stopped started nothing");
    CHECK_EQ_INT(rbstub_calls.last_start_index, 6,
                 "SelectDBRecord(Track) started the wrong track");

    /* And from playing: a skip. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x41, 0x05,
               0x00, 0x00, 0x00, 0x06);
    CHECK_EQ_INT(rbstub_calls.skip, 1,
                 "SelectDBRecord(Track) while playing moved nothing");
    CHECK_EQ_INT(rbstub_calls.last_skip, 3,
                 "SelectDBRecord(Track) skipped the wrong distance");
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "the skip was not bracketed by a pause");
    /* And it comes back: the arm pauses, skips, then resumes unless the
     * accessory found the player already paused. Deleting either half
     * left the suite green. */
    CHECK_EQ_INT(rbstub_calls.resume, 1,
                 "the player was left paused after a skip it did not "
                 "start paused from");
    CHECK(audio_status() & AUDIO_STATUS_PLAY,
          "the player is not playing after SelectDBRecord(Track)");
    CHECK(!(audio_status() & AUDIO_STATUS_PAUSE),
          "the player was left paused");

    /* From paused, it stays paused. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x4A, 0x05,
               0x00, 0x00, 0x00, 0x06);
    CHECK_EQ_INT(rbstub_calls.resume, 0,
                 "a paused player was resumed by a track selection");
    CHECK(audio_status() & AUDIO_STATUS_PAUSE,
          "a paused player did not stay paused");

    /* SelectDBRecord, Playlist category. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_playlist_catalog(cat, 2);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x17, 0x00, 0x42, 0x01,
               0x00, 0x00, 0x00, 0x01);
    CHECK_EQ_INT(rbstub_calls.play_playlist, 1,
                 "SelectDBRecord(Playlist) loaded nothing");
    CHECK(rbstub_calls.playlist_start >= 1,
          "the loaded playlist was not started");

    /* PlayControl Stop. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x43, 0x02);
    CHECK_EQ_INT(rbstub_calls.stop, 1,
                 "PlayControl Stop acked and did not stop the player");

    /* SetCurrentPlayingTrack. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x37, 0x00, 0x44, 0x00, 0x00, 0x00, 0x09);
    CHECK_EQ_INT(rbstub_calls.skip, 1,
                 "SetCurrentPlayingTrack moved nothing");
    CHECK_EQ_INT(rbstub_calls.last_skip, 6,
                 "SetCurrentPlayingTrack skipped the wrong distance");
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "SetCurrentPlayingTrack did not bracket its skip");
    CHECK_EQ_INT(rbstub_calls.resume, 1,
                 "SetCurrentPlayingTrack left the player paused");

    /* SelectSortDBRecord, Track arm: Table C-35 (p.546) adds a sort
     * order byte after the category. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x38, 0x00, 0x45, 0x05,
               0x00, 0x00, 0x00, 0x08, 0x00);
    CHECK_EQ_INT(rbstub_calls.skip, 2,
                 "SelectSortDBRecord(Track) does two skips: back to the "
                 "front of the queue, then out to the chosen track");
    CHECK_EQ_INT(rbstub_calls.sort, 1,
                 "SelectSortDBRecord(Track) did not sort");
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "SelectSortDBRecord(Track) did not bracket its skips");
    CHECK_EQ_INT(rbstub_calls.resume, 1,
                 "SelectSortDBRecord(Track) left the player paused");

    /* SelectSortDBRecord, Playlist arm. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_playlist_catalog(cat, 2);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x38, 0x00, 0x46, 0x01,
               0x00, 0x00, 0x00, 0x01, 0x00);
    CHECK_EQ_INT(rbstub_calls.play_playlist, 1,
                 "SelectSortDBRecord(Playlist) loaded nothing");
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "SelectSortDBRecord(Playlist) did not pause first");
    CHECK_EQ_INT(rbstub_calls.skip, 1,
                 "SelectSortDBRecord(Playlist) did not rewind the queue");
    CHECK_EQ_INT(rbstub_calls.sort, 1,
                 "SelectSortDBRecord(Playlist) did not sort");
}

/* The default arm of both database-selection commands pauses on the way
 * in and has to resume on the way out. MFi 5.1.20 (p.416) has the
 * Apple device answer an unknown category with an error -- but the
 * player it paused is not part of the error, and leaving it paused is
 * a command that failed and changed something anyway. */
void test_playcontrol_refused_selection_leaves_playback_alone(void)
{
    static const struct { unsigned char hi, lo, cat, extra;
                          const char *what; } t[] = {
        { 0x00, 0x17, 0x09, 0, "SelectDBRecord"     },
        { 0x00, 0x38, 0x09, 1, "SelectSortDBRecord" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        iaptest_session_extended();
        rbstub_set_playlist(20, 3);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        rbstub_reset_calls();
        iaptest_tx_clear();

        unsigned char p[11] = { 0x04, t[i].hi, t[i].lo, 0x00,
                                (unsigned char)(0x60 + i), t[i].cat,
                                0x00, 0x00, 0x00, 0x01, 0x00 };
        iaptest_rx(p, (unsigned int)(10 + t[i].extra));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to %s with a reserved category",
              t[i].what);
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "%s accepted a reserved category", t[i].what);

        CHECK(audio_status() & AUDIO_STATUS_PLAY,
              "%s stopped the player on its way to refusing the "
              "category", t[i].what);
        CHECK(!(audio_status() & AUDIO_STATUS_PAUSE),
              "%s refused the category and left the player paused",
              t[i].what);
    }
}

/* Which index space does audio_skip() take?
 *
 * apps/playlist.c's get_next_index() answers it. Under REPEAT_OFF it
 * computes "rotate_index(playlist->index) + steps", and rotate_index()
 * is index - first_index (+ amount if negative) -- exactly
 * iap_track_to_mfi(). So steps is added to, and range-checked against,
 * the ACCESSORY's index space, and the result is converted back. Under
 * REPEAT_ALL it is "(playlist->index + steps) % amount", and the two
 * deltas are congruent modulo the playlist length.
 *
 * So an MFi-space delta is right in both modes and a Rockbox-space one
 * is right in only one. Two review rounds disagreed about this and one
 * of them had already converted four sites the wrong way; the deltas
 * only differ when the conversion wraps, which is why every existing
 * case missed it -- a round trip with target == current is zero in both
 * spaces.
 *
 * 40 tracks, first_index 10 (what a randomise leaves), playing Rockbox
 * 17 = MFi 7, accessory selects MFi 35. The MFi delta is +28 and lands
 * on Rockbox 5. The Rockbox delta is 5 - 17 = -12, which under
 * REPEAT_OFF gives next_index 7 - 12 = -5, out of range: the skip is
 * refused and playback stops. */
void test_playcontrol_skip_delta_is_in_the_accessory_index_space(void)
{
    static const struct { unsigned char p[11]; unsigned int len;
                          const char *what; } t[] = {
        /* Display Remote SetiPodStateInfo, track index. */
        { { 0x03, 0x0E, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x23 }, 9,
          "SetiPodStateInfo track index" },
        /* Display Remote SetCurrentPlayingTrack. */
        { { 0x03, 0x11, 0x00, 0x81, 0x00, 0x00, 0x00, 0x23 }, 8,
          "Display Remote SetCurrentPlayingTrack" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        iaptest_enter_idps();
        iaptest_force_authenticated();
        rbstub_set_playlist(40, 17);
        rbstub_set_playlist_first_index(10);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        rbstub_reset_calls();

        iaptest_rx(t[i].p, t[i].len);

        CHECK(rbstub_calls.skip == 1, "%s moved nothing", t[i].what);
        CHECK(rbstub_calls.last_skip == 28,
              "%s asked for a skip of %d; the accessory's index space "
              "wants +28 and Rockbox's would be -12",
              t[i].what, rbstub_calls.last_skip);
    }

    /* And the Extended Interface siblings, which had been converted to
     * the Rockbox delta. */
    static const struct { unsigned char p[12]; unsigned int len;
                          const char *what; } e[] = {
        { { 0x04, 0x00, 0x17, 0x00, 0x82, 0x05,
            0x00, 0x00, 0x00, 0x23 }, 10, "SelectDBRecord(Track)" },
        { { 0x04, 0x00, 0x28, 0x00, 0x83,
            0x00, 0x00, 0x00, 0x23 }, 9, "PlayCurrentSelection" },
        { { 0x04, 0x00, 0x37, 0x00, 0x84,
            0x00, 0x00, 0x00, 0x23 }, 9, "SetCurrentPlayingTrack" },
    };

    for (unsigned i = 0; i < sizeof(e)/sizeof(e[0]); i++) {
        iaptest_init();
        iaptest_session_extended();
        rbstub_set_playlist(40, 17);
        rbstub_set_playlist_first_index(10);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        global_settings.playlist_shuffle = 0;
        rbstub_reset_calls();

        iaptest_rx(e[i].p, e[i].len);

        CHECK(rbstub_calls.skip == 1, "%s moved nothing", e[i].what);
        CHECK(rbstub_calls.last_skip == 28,
              "%s asked for a skip of %d; the accessory's index space "
              "wants +28 and Rockbox's would be -12",
              e[i].what, rbstub_calls.last_skip);
    }
}

/* Display Remote's two track-position sets, and the offset they pass.
 *
 * The stub discarded audio_ff_rewind()'s argument and there was no
 * field to hold it, so audio_ff_rewind(0) at either site -- or seconds
 * passed where the call wants milliseconds -- changed nothing any case
 * could see. Table 4-74 (p.266) info type 0x00 is "Track position in
 * milliseconds", 4 bytes; type 0x0F (p.269) is "Track position in
 * seconds", 2 bytes. */
void test_playcontrol_track_position_uses_milliseconds(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();

    /* 0x00002710 ms = 10000. */
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x20, 0x00, 0x00, 0x00, 0x27, 0x10);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 1, "the ms seek did not happen");
    CHECK((int)rbstub_calls.last_ff_rewind == 10000,
          "the ms seek went to %ld, not the 10000 the accessory asked "
          "for", rbstub_calls.last_ff_rewind);

    /* 0x000A seconds = 10, which is 10000 ms on the wire to the engine. */
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x21, 0x0F, 0x00, 0x0A);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 1, "the seconds seek did not happen");
    CHECK((int)rbstub_calls.last_ff_rewind == 10000,
          "the seconds seek went to %ld; ten seconds is ten thousand "
          "milliseconds and audio_ff_rewind() takes milliseconds",
          rbstub_calls.last_ff_rewind);
}
