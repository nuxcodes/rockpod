/***************************************************************************
 * End-to-end sessions.
 *
 * These drive whole conversations rather than single commands, with the
 * accessory model attached so every packet the device sends is judged
 * against MFi section 2.6 as it goes. A rule broken anywhere in the
 * session fails the case, without anyone having written an assertion for
 * that particular command.
 *
 * The sessions mirror what the reported hardware actually does: a
 * Bluetooth transmitter that identifies through IDPS and takes digital
 * audio, and a car head unit that browses the database over the Extended
 * Interface lingo.
 ****************************************************************************/

#include <stddef.h>
#include "iap_test.h"
#include "accessory.h"

#include <string.h>

#include "config.h"
#include "iap.h"
#include "settings.h"
#include "sound.h"
#include "iap-core.h"
#include "audio.h"
#include "appevents.h"
#include "cuesheet.h"

extern void iap_periodic(void);

/* Lingoes a full-featured accessory declares. */
static const unsigned char full_lingoes[] =
    { 0x00, 0x02, 0x03, 0x04, 0x0A };

static void expect_clean(const char *what)
{
    /* Liveness first. accessory.h documents iapacc_judged() for exactly
     * this: "the accessory model rejected nothing" is also what a model
     * that saw nothing says, and a detached one sees nothing. Forcing
     * it permanently detached left all five callers passing, including
     * test_session_car_head_unit, whose only check this is -- a case
     * reporting ok having verified an empty session. */
    CHECK(iapacc_judged() > 0,
          "%s: the accessory model judged no packets at all, so it "
          "rejecting none says nothing", what);

    CHECK(iapacc_violations() == 0,
          "%s: the accessory model rejected %d packet(s); first was: %s",
          what, iapacc_violations(), iapacc_first_violation());
}

/* ------------------------------------------------------------------ */
/* A Bluetooth transmitter                                             */
/* ------------------------------------------------------------------ */

void test_session_bluetooth_transmitter(void)
{
    iapacc_attach();

    /* Identify, authenticate, take digital audio, then use it. */
    iapacc_identify_idps(full_lingoes, sizeof(full_lingoes));
    CHECK(iapacc_transactions_enabled(),
          "transaction IDs were torn down during identification");
    iaptest_force_authenticated();

    /* Find out what the device can do before asking it to do anything. */
    IAPACC_SEND(0x00, 0x4B, 0x00);            /* options for General */
    IAPACC_SEND(0x00, 0x4B, 0x03);            /* options for Display Remote */
    IAPACC_SEND(0x00, 0x0F, 0x03);            /* lingo protocol version */
    IAPACC_SEND(0x00, 0x07);                  /* iPod name */

    /* Enable line out, then volume notifications, which the spec makes
     * the accessory's job in that order. */
    IAPACC_SEND(0x00, 0x2B, 0x03, 0x01, 0x00);
    IAPACC_SEND(0x03, 0x08, 0x00, 0x00, 0x00, 0x10);

    /* Digital audio brings up the sample rate exchange. */
    IAPACC_SEND(0x0A, 0x03, 0x00, 0x00, 0xAC, 0x44);

    /* Now run for a while: the user changes volume, tracks change. */
    for (int i = 0; i < 5; i++) {
        rbstub_set_volume(-40 + i * 8);
        iap_periodic();
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        /* The change is flagged on the audio thread and sent from
         * iap_periodic(); see iap_track_changed(). */
        iap_periodic();
    }

    /* And presses play on the headphones a few times. */
    for (int i = 0; i < 3; i++) {
        IAPACC_SEND(0x02, 0x00, 0x00, 0x01);   /* play/pause down */
        iaptest_button_sample(4);
        IAPACC_SEND(0x02, 0x00, 0x00, 0x00);   /* release */
        iaptest_button_sample(4);
    }

    expect_clean("bluetooth transmitter session");
}

/* ------------------------------------------------------------------ */
/* A car head unit                                                     */
/* ------------------------------------------------------------------ */

void test_session_car_head_unit(void)
{
    iapacc_attach();

    iapacc_identify_idps(full_lingoes, sizeof(full_lingoes));
    iaptest_force_authenticated();

    /* Enter Extended Interface and enable play status notifications. */
    IAPACC_SEND(0x00, 0x05);
    IAPACC_SEND(0x04, 0x00, 0x26, 0x01);

    /* Interrogate the state a head unit puts on its display. */
    IAPACC_SEND(0x04, 0x00, 0x12);            /* protocol version */
    IAPACC_SEND(0x04, 0x00, 0x14);            /* iPod name */
    IAPACC_SEND(0x04, 0x00, 0x1C);            /* play status */
    IAPACC_SEND(0x04, 0x00, 0x1E);            /* current track index */
    IAPACC_SEND(0x04, 0x00, 0x35);            /* number of tracks */
    IAPACC_SEND(0x04, 0x00, 0x2C);            /* shuffle */
    IAPACC_SEND(0x04, 0x00, 0x2F);            /* repeat */

    rbstub_set_playlist(25, 4);

    /* Track titles, the replies that actually carry text. */
    for (int i = 0; i < 4; i++) {
        unsigned char c[7] = { 0x04, 0x00, 0x20, 0x00, 0x00, 0x00,
                               (unsigned char)i };
        iapacc_send(c, sizeof(c));
    }

    /* Transport control from the dash. Every PlayControl arm refuses
     * from a stopped player, so without this the three commands below
     * were three Command Failed acks and the button samples were
     * no-ops. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    IAPACC_SEND(0x04, 0x00, 0x29, 0x03);      /* next */
    iaptest_button_sample(4);
    IAPACC_SEND(0x04, 0x00, 0x29, 0x04);      /* previous */
    iaptest_button_sample(4);
    IAPACC_SEND(0x04, 0x00, 0x29, 0x01);      /* play/pause */
    iaptest_button_sample(4);

    /* And a run of tracks changing underneath it. */
    for (int i = 0; i < 6; i++) {
        rbstub_set_playlist(25, 4 + i);
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        /* The change is flagged on the audio thread and sent from
         * iap_periodic(); see iap_track_changed(). */
        iap_periodic();
        iap_periodic();
    }

    expect_clean("car head unit session");
}

/* ------------------------------------------------------------------ */
/* A legacy accessory must still see no transaction IDs                */
/* ------------------------------------------------------------------ */

void test_session_legacy_remote(void)
{
    iapacc_attach();

    iapacc_identify_legacy(0x0000001D);
    CHECK(!iapacc_transactions_enabled(),
          "IdentifyDeviceLingoes must leave transaction IDs disabled");
    iaptest_force_authenticated();

    IAPACC_SEND(0x00, 0x07);
    IAPACC_SEND(0x00, 0x09);
    IAPACC_SEND(0x03, 0x04);
    IAPACC_SEND(0x03, 0x0C, 0x04);

    for (int i = 0; i < 3; i++) {
        IAPACC_SEND(0x02, 0x00, 0x00, 0x01);
        iaptest_button_sample(4);
        IAPACC_SEND(0x02, 0x00, 0x00, 0x00);
        iaptest_button_sample(4);
    }

    expect_clean("legacy remote session");
}

/* ------------------------------------------------------------------ */
/* Reconnection                                                        */
/* ------------------------------------------------------------------ */

/* An IDPS accessory, then a legacy one, without a reboot in between.
 * The device has to forget the first one's transaction-ID state or the
 * second is parsed two bytes off. */
void test_session_idps_then_legacy_reconnect(void)
{
    iapacc_attach();

    iapacc_identify_idps(full_lingoes, sizeof(full_lingoes));
    iaptest_force_authenticated();
    IAPACC_SEND(0x00, 0x07);
    expect_clean("first accessory");

    /* Second accessory attaches. On hardware this follows a detach; the
     * harness models the protocol half by re-identifying. */
    iapacc_reset();
    iapacc_attach();
    iapacc_identify_legacy(0x0000001D);
    iaptest_force_authenticated();

    IAPACC_SEND(0x00, 0x07);

    const struct iaptest_pkt *p = NULL;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *q = iaptest_tx(i);
        if (q->paylen >= 2 && q->payload[0] == 0x00 && q->payload[1] == 0x08)
            p = q;
    }
    CHECK(p != NULL, "no ReturniPodName for the second accessory");
    if (p)
        CHECK_EQ_INT(p->paylen, 10,
                     "ReturniPodName to a legacy accessory must be 10 bytes "
                     "(lingo, command, ROCKBOX and its NUL); a longer one "
                     "means the first accessory's transaction-ID state "
                     "survived the swap");

    expect_clean("second accessory");
}

/* ------------------------------------------------------------------ */
/* SetPlayStatusChangeNotification, both forms                         */
/* ------------------------------------------------------------------ */

/* MFi Table 5-43 (p.425) is a four-byte big-endian mask and Table 5-45
 * marks bits 31:13 Reserved, so a conformant mask starts with 0x00.
 * Reading only that byte disabled notifications for every legal
 * four-byte request while acking Success -- and p.424 makes the
 * four-byte form the one an accessory must try first, so it never falls
 * back to the one-byte form that does work. */
static bool notifications_on_after(const unsigned char *cmd, int len)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x10);
    iaptest_tx_clear();

    iaptest_rx(cmd, len);
    rbstub_set_playlist(10, 3);
    iaptest_tx_clear();
    rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    /* The change is flagged on the audio thread and sent from
     * iap_periodic(); see iap_track_changed(). */
    iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 3 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27)
            return true;
    }
    return false;
}

void test_playstatus_notify_four_byte_form(void)
{
    /* Every bit Table 5-45 defines. */
    static const unsigned char all[] =
        { 0x04, 0x00, 0x26, 0x00, 0x11, 0x00, 0x00, 0x1F, 0xFF };
    CHECK(notifications_on_after(all, sizeof(all)),
          "a four-byte mask with every defined bit set produced no "
          "PlayStatusChangeNotification");

    /* Bit 02 alone. A track change is reported as Table 5-47 (p.426)
     * type 0x01 "Track index", and Table 5-45 (p.425) bit 02 is the bit
     * that subscribes to it. */
    static const unsigned char idx[] =
        { 0x04, 0x00, 0x26, 0x00, 0x12, 0x00, 0x00, 0x00, 0x04 };
    CHECK(notifications_on_after(idx, sizeof(idx)),
          "a four-byte mask selecting Track index produced no "
          "PlayStatusChangeNotification");

    /* Bit 00 alone must NOT. Table 5-45 gives it as "Basic play state
     * changes (stop, FFW seek stop, or REW seek stop, using status
     * notification types 0x00, 0x02, or 0x03)" -- a track index change
     * is none of those.
     *
     * This case previously asserted the opposite, because the mask was
     * collapsed to a bool and any non-zero value enabled everything. */
    static const unsigned char basic[] =
        { 0x04, 0x00, 0x26, 0x00, 0x13, 0x00, 0x00, 0x00, 0x01 };
    CHECK(!notifications_on_after(basic, sizeof(basic)),
          "a mask selecting only basic play state produced a Track "
          "index notification, so the bits are still being ignored");

    /* All zero must disable. */
    static const unsigned char none[] =
        { 0x04, 0x00, 0x26, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00 };
    CHECK(!notifications_on_after(none, sizeof(none)),
          "an all-zero four-byte mask must disable notifications");
}

/* The two lingoes subscribe independently.
 *
 * MFi 4.3.11 (p.255) for the Display Remote lingo: "A remote event
 * bitmask of 0x0 disables all remote event status notifications. On
 * accessory detach, event notification is reset to the default disabled
 * state." Nothing else may switch them off -- and in particular an
 * Extended Interface command may not.
 *
 * Both were writing one shared bool, so SetPlayStatusChangeNotification(0)
 * silenced every Display Remote event, and SetRemoteEventNotification
 * switched on Extended Interface notifications for an accessory that
 * had never asked for them. */
void test_playstatus_notify_is_independent_of_display_remote(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;

    /* Every lingo 4 command is refused outside Extended Interface mode
     * (iap-lingo4.c:190), so without this the SetPlayStatusChange-
     * Notification below never lands and the case passes whatever the
     * code does. */
    IAPTEST_RX(0x00, 0x05, 0x00, 0x05);

    /* Subscribe to a Display Remote event: bit 4, Mute/UI volume. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10);
    CHECK(device.do_notify, "Display Remote notifications did not enable");

    /* Now quieten the Extended Interface. The Display Remote
     * subscription must survive. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00);
    CHECK(device.do_notify,
          "SetPlayStatusChangeNotification(0) switched off the Display "
          "Remote notifications too");

    rbstub_set_volume(-30);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    bool saw_volume = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x04)
            saw_volume = true;
    }
    CHECK(saw_volume,
          "the volume notification stopped after the Extended Interface "
          "was quietened, though only a zero remote event bitmask or a "
          "detach may do that (MFi 4.3.11, p.255)");

    /* And the reverse: a Display Remote subscription must not switch on
     * Extended Interface notifications the accessory never asked for. */
    CHECK_EQ_INT(device.pb_notifications, 0,
                 "a Display Remote subscription enabled Extended "
                 "Interface play-status notifications");
}

/* A subscribed position notification must still be change-detected.
 * Every Display Remote event is; this one fired on every tick for any
 * accessory in Extended Interface mode -- two packets a second at a
 * standstill. */
void test_playstatus_position_is_change_detected(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    interface_state = IST_EXTENDED;

    /* Bit 03, Track time offset (ms). */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x30, 0x00, 0x00, 0x00, 0x08);

    struct mp3entry *id3 = rbstub_id3();
    id3->elapsed = 5000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int first = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x04)
            first++;
    }
    CHECK(first > 0, "no position notification after subscribing to bit 03");

    /* Nothing moved: no further packets. */
    iaptest_tx_clear();
    for (int t = 0; t < 12; t++)
        iap_periodic();

    int again = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x04)
            again++;
    }
    CHECK_EQ_INT(again, 0,
                 "the position notification repeated at a standstill");

    /* Move, and it reports again. */
    id3->elapsed = 6000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int moved = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x04)
            moved++;
    }
    CHECK(moved > 0, "the position notification stopped after the "
          "position moved");
}

void test_playstatus_notify_one_byte_form_still_works(void)
{
    static const unsigned char on[]  = { 0x04, 0x00, 0x26, 0x00, 0x14, 0x01 };
    static const unsigned char off[] = { 0x04, 0x00, 0x26, 0x00, 0x15, 0x00 };

    CHECK(notifications_on_after(on, sizeof(on)),
          "the one-byte enable form stopped working");
    CHECK(!notifications_on_after(off, sizeof(off)),
          "the one-byte disable form stopped working");
}

/* MFi Table 4-61 (p.260), event 0x09 Date/time, and Table 4-72 (p.264)
 * for the GetiPodStateInfo reply, both say the same thing:
 *
 *   "Bytes 0-1 specify the current year. A value of 2005 represents the
 *    year 2005 A.D."
 *
 * Rockbox stores tm_year as years since 1900 -- rtc-6g.c:47 and
 * rtc_pcf50605.c:54 both compute "buf[6] + 100", and valid_time()
 * (timefuncs.c:92) rejects anything outside 100..199. Sending it raw
 * puts year 126 on the wire in 2026, so a dock that displays the clock
 * shows 126 and a head unit that syncs from the iPod sets itself to
 * 126 A.D. The month and day beside it are converted correctly, so the
 * packet looks well-formed. */
void test_session_date_is_the_ad_year(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    rbstub_set_date(2026, 8, 1, 14, 30);

    /* GetiPodStateInfo (lingo 3 / 0x0C), info type 0x09. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0x20, 0x09);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to GetiPodStateInfo Date/time");
    if (p && p->paylen >= 11) {
        int year = (p->payload[5] << 8) | p->payload[6];
        CHECK_EQ_INT(year, 2026, "the year on the wire must be A.D.");
        CHECK_EQ_INT(p->payload[7], 8,  "month");
        CHECK_EQ_INT(p->payload[8], 1,  "day");
        CHECK_EQ_INT(p->payload[9], 14, "hour");
        CHECK_EQ_INT(p->payload[10], 30, "minute");
    } else if (p) {
        CHECK(false, "date reply is %d bytes", p->paylen);
    }

    /* And the same field in the unsolicited notification. Event 0x09 is
     * bit 9 of the Display Remote notification mask.
     *
     * Settle the authentication machine first. iaptest_force_authenticated()
     * leaves it at AUST_CERTDONE, from which the tick below drives the
     * challenge and, with no accessory to answer it, eventually resets
     * the device -- taking the notification mask with it. */
    device.auth.state = AUST_AUTH;
    IAPTEST_RX(0x03, 0x08, 0x00, 0x21, 0x00, 0x00, 0x02, 0x00);
    CHECK_EQ_INT(device.notifications, 0x00000200,
                 "the notification mask after enabling event 0x09");
    iaptest_tx_clear();
    rbstub_set_date(2026, 12, 25, 9, 5);

    /* The notification block sits behind a five-tick gate
     * (iap-core.c:1284), and the counter is a function-static that no
     * case resets, so drive enough ticks to be sure of crossing it. */
    for (int t = 0; t < 6; t++)
        iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *q = iaptest_tx(i);
        if (q->paylen < 11 || q->payload[0] != 0x03 || q->payload[1] != 0x09
            || q->payload[4] != 0x09)
            continue;
        int year = (q->payload[5] << 8) | q->payload[6];
        CHECK_EQ_INT(year, 2026,
                     "the year in the date notification must be A.D.");
        return;
    }
    CHECK(false, "no date/time notification after enabling event 0x09 "
          "(%d packets sent, mask 0x%08X)",
          iaptest_tx_count(), device.notifications);
}

/* What a detach must clear.
 *
 * serial-6g.c's accessory tick now calls iap_reset_state() when the
 * dock connector goes away; before that it closed the UART and told the
 * protocol layer nothing, so a whole session survived a detach and the
 * next accessory inherited it.
 *
 * MFi 4.3.11 (p.255) names one of these explicitly: "On accessory
 * detach, event notification is reset to the default disabled state."
 * The rest follow from the session being over -- an accessory that has
 * been unplugged is not authenticated, and MFi 2.6.1.1 (p.111) has the
 * transaction counter re-initialised "every time it is connected".
 *
 * This pins the contract serial-6g.c depends on. The tick itself is
 * target code the harness cannot run. */
void test_session_detach_clears_the_session(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Subscribe to something, so there is state to lose. Lingo 4 needs
     * Extended Interface mode first (iap-lingo4.c:190). */
    device.auth.state = AUST_AUTH;
    IAPTEST_RX(0x00, 0x05, 0x00, 0x4F);
    IAPTEST_RX(0x03, 0x08, 0x00, 0x50, 0x00, 0x00, 0x00, 0x10);
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x51, 0x00, 0x00, 0x00, 0x04);

    CHECK(device.do_notify, "nothing to lose: no Display Remote mask set");
    CHECK(device.pb_notifications != 0,
          "nothing to lose: no play-status mask set");
    CHECK(device.lingoes != 0, "nothing to lose: no lingoes negotiated");
    CHECK(DEVICE_AUTHENTICATED, "nothing to lose: not authenticated");

    /* The accessory is unplugged. */
    iap_reset_state(IF_IAP_MP(0));

    CHECK(!DEVICE_AUTHENTICATED,
          "the next accessory inherits an authenticated session");
    CHECK_EQ_INT(device.lingoes, 0,
                 "the next accessory inherits the negotiated lingoes");
    CHECK(!device.do_notify,
          "the next accessory inherits the Display Remote notification "
          "subscription (MFi 4.3.11, p.255)");
    CHECK_EQ_INT(device.pb_notifications, 0,
                 "the next accessory inherits the play-status "
                 "notification subscription");
    CHECK_EQ_INT(device.notifications, 0,
                 "the next accessory inherits the event mask");
    CHECK(!DEVICE_TRANSID_ACTIVE,
          "the next accessory inherits transaction IDs it never enabled, "
          "so every packet it sends is parsed two bytes off");
    CHECK_EQ_INT(device.ipod_trans_id, 1,
                 "the transaction counter restarts (MFi 2.6.1.1, p.111)");
}

/* MFi Table 4-59 (p.255) lists bit 1 "Track playback index" and bit 2
 * "Chapter index" as separate events, and 4.3.11 says "Notification for
 * each event can be enabled by setting the associated bit in the remote
 * event bitmask". Table 4-61 (p.257) gives event 0x02 an 8-byte payload:
 * "the track index, the chapter count, and the chapter index".
 *
 * Both blocks in iap_periodic() compared against device.track_index and
 * both assigned to it. The bit 1 block runs first, so an accessory that
 * enabled both -- an ordinary audiobook or podcast mask -- had bit 1
 * update the field and bit 2's comparison was false every time. Event
 * 0x02 never fired at all. */
void test_session_chapter_and_track_index_are_separate_events(void)
{
    static struct cuesheet cue;

    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;

    memset(&cue, 0, sizeof(cue));
    cue.track_count = 2;
    cue.tracks[0].offset = 0;
    cue.tracks[1].offset = 60000;
    rbstub_id3()->cuesheet = &cue;
    rbstub_id3()->elapsed = 1000;
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Both bits: 0x00000006. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x06);
    CHECK_EQ_INT(device.notifications, 0x06, "both bits enabled");

    rbstub_set_playlist(20, 3);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int track = 0, chapter = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen < 5 || p->payload[0] != 0x03 || p->payload[1] != 0x09)
            continue;
        if (p->payload[4] == 0x01) track++;
        if (p->payload[4] == 0x02) {
            chapter++;
            CHECK_EQ_INT(p->paylen, 13,
                         "event 0x02 carries eight data bytes (Table 4-61, "
                         "p.257)");
        }
    }
    CHECK(track > 0, "no track index notification");
    CHECK(chapter > 0,
          "no chapter index notification, though bit 2 was enabled -- the "
          "two events share their change-detection state");

    /* Each fires once per change, not once and then never. */
    rbstub_set_playlist(20, 7);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    track = chapter = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen < 5 || p->payload[0] != 0x03 || p->payload[1] != 0x09)
            continue;
        if (p->payload[4] == 0x01) track++;
        if (p->payload[4] == 0x02) chapter++;
    }
    CHECK_EQ_INT(track, 1, "one track index notification per change");
    CHECK_EQ_INT(chapter, 1, "one chapter index notification per change");

    /* And neither repeats at a standstill. */
    iaptest_tx_clear();
    for (int t = 0; t < 12; t++)
        iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(!(p->paylen >= 5 && p->payload[0] == 0x03
                && p->payload[1] == 0x09
                && (p->payload[4] == 0x01 || p->payload[4] == 0x02)),
              "an index notification repeated with nothing changed");
    }
}

/* Two answers about line-out that used to disagree with each other.
 *
 * MFi Table 3-41 (p.144), GetiPodOptions: "Bit 1: the Apple device
 * supports using SetiPodPreferences to control line-out usage."
 * RetiPodOptionsForLingo already claims that for the General lingo,
 * and SetiPodPreferences really does accept class 0x03, so a zero here
 * was the odd one out -- and an accessory that only implements 0x24
 * concluded it must not register the line-out preference, which the
 * note to Table 4-59 (p.256) makes a precondition for volume-change
 * notifications.
 *
 * MFi Table 2-7 footnote 2 (p.105): "Preference commands (0x29-0x2B)
 * require authentication on all Apple devices except the 5G iPod;
 * however, getting or setting the line-out preference class (0x03) does
 * not require authentication." The gate was unconditional. */
void test_session_line_out_is_advertised_and_unauthenticated(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* GetiPodOptions must claim line-out control. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x24, 0x00, 0xE0);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to GetiPodOptions");
        if (p && p->paylen >= 12) {
            CHECK_EQ_INT(p->payload[1], 0x25, "RetiPodOptions");
            uint32_t lo = ((uint32_t)p->payload[8] << 24)
                        | ((uint32_t)p->payload[9] << 16)
                        | ((uint32_t)p->payload[10] << 8)
                        |  (uint32_t)p->payload[11];
            CHECK((lo & 0x02) != 0,
                  "GetiPodOptions must claim line-out control, to agree "
                  "with GetiPodOptionsForLingo and with "
                  "SetiPodPreferences accepting class 0x03");
            CHECK((lo & 0x01) == 0,
                  "video output must stay clear");
        }
    }

    /* And the line-out class works before authentication completes. */
    iaptest_enter_idps();
    device.auth.state = AUST_INIT;          /* identified, not yet authed */
    CHECK(!DEVICE_AUTHENTICATED, "the case needs an unauthenticated state");

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x2B, 0x00, 0xE1, 0x03, 0x01, 0x00);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to SetiPodPreferences(line-out)");
        if (p && p->paylen >= 5)
            CHECK_EQ_INT(p->payload[4], 0x00,
                         "the line-out class needs no authentication "
                         "(Table 2-7 footnote 2, p.105)");
    }

    /* Another class still does. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x2B, 0x00, 0xE2, 0x07, 0x01, 0x00);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to SetiPodPreferences(other class)");
        if (p && p->paylen >= 5)
            CHECK(p->payload[4] != 0x00,
                  "a class other than line-out must still require "
                  "authentication");
    }
}

/* Four paths nothing exercised, found by disabling each in turn and
 * watching the suite stay green rather than by reading it.
 *
 * MFi Table 4-59 (p.255) lists bit 3 "Play status (play, pause, stop,
 * FF, and RW)" and bit 5 "Power/battery". Table 4-61 (p.258) gives
 * event 0x03 a one-byte payload, "the current play status of the Apple
 * device", and event 0x05 two bytes, "Byte 0: Power State ... Byte 1:
 * Battery Level". */
void test_session_play_status_and_power_notifications(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;

    /* Bits 3 and 5 together. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x28);
    CHECK_EQ_INT(device.notifications, 0x28, "both bits enabled");

    /* A play-status change must produce event 0x03, one data byte. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int status = 0, power = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen < 5 || p->payload[0] != 0x03 || p->payload[1] != 0x09)
            continue;
        if (p->payload[4] == 0x03) {
            status++;
            CHECK_EQ_INT(p->paylen, 6,
                         "event 0x03 carries one data byte (Table 4-61, "
                         "p.258)");
        }
        if (p->payload[4] == 0x05) {
            power++;
            CHECK_EQ_INT(p->paylen, 7,
                         "event 0x05 carries two data bytes");
        }
    }
    CHECK(status > 0, "no play status notification, though bit 3 was set");
    CHECK(power > 0, "no power/battery notification, though bit 5 was set");

    /* Play status follows the player: stopping must report it again. */
    rbstub_set_audio_status(0);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    status = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 6 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x03)
            status++;
    }
    CHECK_EQ_INT(status, 1, "one play status notification per change");

    /* And neither repeats with nothing moving. */
    iaptest_tx_clear();
    for (int t = 0; t < 12; t++)
        iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(!(p->paylen >= 5 && p->payload[0] == 0x03
                && p->payload[1] == 0x09 && p->payload[4] == 0x03),
              "the play status notification repeated at a standstill");
    }

    /* Subscribing again is a fresh subscription and is owed the state
     * again, even though nothing has moved. Without that, an accessory
     * that re-registers its mask -- which any of them may do -- is left
     * with no idea of the battery until it next changes. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xF2, 0x00, 0x00, 0x00, 0x28);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    power = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 7 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x05)
            power++;
    }
    CHECK(power > 0,
          "re-subscribing to power/battery reported nothing, so an "
          "accessory that re-registers never learns the state");

    /* And a replacement accessory is owed it too: the fields the
     * change is measured against must not outlive the session. */
    iap_reset_state(IF_IAP_MP(0));
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    IAPTEST_RX(0x03, 0x08, 0x00, 0xF3, 0x00, 0x00, 0x00, 0x20);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    power = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 7 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x05)
            power++;
    }
    CHECK(power > 0,
          "a replacement accessory was told nothing about the power "
          "state, because the previous session's readings survived");
}

/* MFi 4.3.29 (p.278): GetPowerBatteryState takes no parameters and is
 * answered by RetPowerBatteryState (4.3.30, same page), whose payload
 * Table 4-91 gives as a power state byte and a battery level byte. */
void test_session_get_power_battery_state(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x1A, 0x00, 0xF1);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to GetPowerBatteryState");
    if (p && p->paylen >= 6) {
        CHECK_EQ_INT(p->payload[0], 0x03, "reply lingo");
        CHECK_EQ_INT(p->payload[1], 0x1B, "RetPowerBatteryState");
        CHECK_EQ_INT(p->payload[2], 0x00, "transaction ID high");
        CHECK_EQ_INT(p->payload[3], 0xF1, "transaction ID low");
        CHECK_EQ_INT(p->paylen, 6,
                     "a power state byte and a battery level byte "
                     "(Table 4-91, p.278)");
    } else if (p) {
        CHECK(false, "the reply is %d bytes", p->paylen);
    }
}

/* Shuffle, repeat and absolute volume are three of the events an
 * accessory can subscribe to, and all three were dark: deleting any of
 * their notification blocks left every binary green. Table 4-61
 * (pp.257-261) gives them events 0x07, 0x08 and 0x10.
 *
 * The volume one is the pair to event 0x04. 0x04 carries the mute state
 * and the UI level; 0x10 adds the absolute level, "not normalized" to
 * the volume limit where the UI one is. An accessory with its own
 * volume readout subscribes to 0x10 to get both. */
void test_session_shuffle_repeat_and_absolute_volume_events(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Bits 7, 8 and 16. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x90, 0x00, 0x01, 0x01, 0x80);

    /* Settle whatever the current state is, so what follows is a
     * change and not a first report. */
    for (int t = 0; t < 6; t++)
        iap_periodic();

    static const struct {
        unsigned char event;
        const char   *what;
    } want[] = {
        { 0x07, "shuffle" },
        { 0x08, "repeat" },
        { 0x10, "absolute volume" },
    };

    global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
    global_settings.repeat_mode =
        (global_settings.repeat_mode == REPEAT_OFF) ? REPEAT_ALL
                                                    : REPEAT_OFF;
    global_status.volume = (global_status.volume == sound_min(SOUND_VOLUME))
                           ? sound_max(SOUND_VOLUME)
                           : sound_min(SOUND_VOLUME);

    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    for (unsigned i = 0; i < sizeof(want)/sizeof(want[0]); i++) {
        bool saw = false;
        for (int k = 0; k < iaptest_tx_count(); k++) {
            const struct iaptest_pkt *p = iaptest_tx(k);
            if (p && p->paylen >= 5 && p->payload[0] == 0x03
                && p->payload[1] == 0x09 && p->payload[4] == want[i].event)
                saw = true;
        }
        CHECK(saw, "%s changed and the subscribed accessory was not told; "
                   "no RemoteEventNotification 0x%02X went out",
              want[i].what, want[i].event);
    }

    /* Event 0x10 carries three bytes: mute, UI level, absolute level
     * (Table 4-61, p.261). The last two differ whenever a volume limit
     * is in force, which is the reason the event exists. */
    for (int k = 0; k < iaptest_tx_count(); k++) {
        const struct iaptest_pkt *p = iaptest_tx(k);
        if (p && p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x10)
        {
            CHECK(p->paylen >= 8,
                  "the absolute volume event carried %d payload bytes; "
                  "Table 4-61 gives it mute, UI level and absolute level",
                  p->paylen);
            break;
        }
    }

    /* And none of the three repeats with nothing moving. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    for (int k = 0; k < iaptest_tx_count(); k++) {
        const struct iaptest_pkt *p = iaptest_tx(k);
        if (!p || p->paylen < 5 || p->payload[0] != 0x03
            || p->payload[1] != 0x09)
            continue;
        CHECK(p->payload[4] != 0x07 && p->payload[4] != 0x08
              && p->payload[4] != 0x10,
              "event 0x%02X repeated with nothing changed",
              p->payload[4]);
    }
}

/* MFi 4.3.13 (p.263): the device answers GetRemoteEventStatus with "a
 * bitmask of event states that changed since the last
 * GetRemoteEventStatus command and clears all the remote event status
 * bits. This command may be used to poll the Apple device for certain
 * event changes without enabling asynchronous remote event
 * notification."
 *
 * That last sentence is the whole case. changed_notifications was
 * written only by iap_periodic(), inside per-event subscription
 * checks, behind an early return taken when nothing is subscribed -- so
 * an accessory that never sent SetRemoteEventNotification and polled
 * instead read zero for the life of the session and concluded the
 * device never changed. GetRemoteEventStatus appears nowhere in the
 * suite, so nothing noticed. */
void test_session_event_status_can_be_polled_without_subscribing(void)
{
    /* Before the session starts, so the baseline taken when the
     * accessory arrives already has it. Setting it afterwards is a
     * genuine track change and the first poll rightly reports one. */
    rbstub_set_playlist(40, 3);
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Deliberately no SetRemoteEventNotification. */
    CHECK_EQ_INT(device.notifications, 0,
                 "the case is meant to poll with nothing subscribed");

    /* Nothing has moved since the session began, so the first answer
     * is empty. The baseline is taken when the accessory arrives, not
     * on the first poll -- taken on the first poll, an accessory that
     * connected, waited and then asked would always be told nothing
     * had happened. Zeroed rather than sampled, this reports every
     * non-zero field as a change that never occurred. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xA0);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetRemoteEventStatus");
        if (!r || r->paylen < 8)
            return;
        CHECK_EQ_INT(r->payload[0], 0x03, "reply lingo");
        CHECK_EQ_INT(r->payload[1], 0x0B, "RetRemoteEventStatus");
        uint32_t bits = ((uint32_t)r->payload[4] << 24)
                      | ((uint32_t)r->payload[5] << 16)
                      | ((uint32_t)r->payload[6] << 8)
                      |  (uint32_t)r->payload[7];
        CHECK_EQ_INT(bits, 0,
                     "a poll on an untouched session reported changes");
    }

    /* Move three things the spec has events for. */
    rbstub_set_playlist(40, 11);
    global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
    global_settings.repeat_mode =
        (global_settings.repeat_mode == REPEAT_OFF) ? REPEAT_ALL : REPEAT_OFF;

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xA1);
    uint32_t bits = 0;
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to the second GetRemoteEventStatus");
        if (!r || r->paylen < 8)
            return;
        bits = ((uint32_t)r->payload[4] << 24) | ((uint32_t)r->payload[5] << 16)
             | ((uint32_t)r->payload[6] << 8)  |  (uint32_t)r->payload[7];
    }

    CHECK(bits & (1u << 1),
          "the track index changed and polling reported 0x%08X; an "
          "accessory that never subscribes learns nothing", bits);
    CHECK(bits & (1u << 7),
          "shuffle changed and polling reported 0x%08X", bits);
    CHECK(bits & (1u << 8),
          "repeat changed and polling reported 0x%08X", bits);

    /* "and clears all the remote event status bits" -- a second read
     * with nothing moved comes back empty. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xA2);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 8) {
            uint32_t again = ((uint32_t)r->payload[4] << 24)
                           | ((uint32_t)r->payload[5] << 16)
                           | ((uint32_t)r->payload[6] << 8)
                           |  (uint32_t)r->payload[7];
            CHECK_EQ_INT(again, 0,
                         "the status bits were not cleared by the read");
        }
    }
}

#ifdef USB_ENABLE_AUDIO
/* MFi 4.3.12 (p.257): "The Apple device sends this command
 * asynchronously whenever an enabled event change has occurred."
 * Streaming does not suspend that.
 *
 * Notifications used to be suppressed outright while USB audio source
 * mode ran, so SetRemoteEventNotification was acked Success and then
 * nothing arrived for the whole session -- and for a digital audio dock
 * that is the entire session. What the suppression was for is the
 * continuous traffic: the same paragraph puts enabled events on a
 * 500 ms cadence, and elapsed track position moves on every one of
 * them. That part still stays off the wire. */
void test_session_discrete_events_survive_usb_audio(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Track index (bit 1), play status (bit 3) and elapsed position
     * (bit 0) all at once. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x0B);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* A dock that is streaming has both flags up: usb_audio.c sets
     * active and streaming fourteen lines apart, and the iAP layer
     * reads them at different sites -- the volume refusals ask
     * usb_audio_get_active(), the notification suppression asks
     * usb_audio_source_streaming(). This case is about the
     * suppression. */
    rbstub_set_usb_audio_active(true);
    rbstub_set_usb_audio_streaming(true);
    rbstub_set_playlist(40, 12);        /* a track change while streaming */
    rbstub_id3()->elapsed += 3000;      /* and time moving, as it does */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int track = 0, position = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 5 || p->payload[0] != 0x03
            || p->payload[1] != 0x09)
            continue;
        if (p->payload[4] == 0x01) track++;
        if (p->payload[4] == 0x00) position++;
    }

    CHECK(track > 0,
          "the track changed while USB audio was streaming and the "
          "subscribed accessory was told nothing; for a digital audio "
          "dock that is the whole session");
    CHECK_EQ_INT(position, 0,
                 "elapsed position notifications went out during "
                 "streaming; those are the continuous traffic the "
                 "suppression exists for");

    /* And position resumes once streaming stops, so it is suppressed
     * rather than broken. */
    rbstub_set_usb_audio_active(false);
    rbstub_set_usb_audio_streaming(false);
    rbstub_id3()->elapsed += 3000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    position = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x00)
            position++;
    }
    CHECK(position > 0,
          "elapsed position never resumed after streaming stopped");

    /* The Extended Interface has its own per-tick clock -- type 0x04,
     * "Track time offset (ms)", Table 5-47 (p.426) -- and it is the
     * same traffic on a different lingo. 8c63099d8e narrowed the
     * blanket guard for the Display Remote events and left this one
     * running. */
    IAPTEST_RX(0x00, 0x05, 0x00, 0xC1);
    /* The mode transition raises BUTTON_RC_PLAY while playing, and a
     * pending button defers the next packet. */
    for (int t = 0; t < 4; t++)
        iap_periodic();
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xC2, 0x00, 0x00, 0x00, 0x08);
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* A dock that is streaming has both flags up: usb_audio.c sets
     * active and streaming fourteen lines apart, and the iAP layer
     * reads them at different sites -- the volume refusals ask
     * usb_audio_get_active(), the notification suppression asks
     * usb_audio_source_streaming(). This case is about the
     * suppression. */
    rbstub_set_usb_audio_active(true);
    rbstub_set_usb_audio_streaming(true);
    rbstub_id3()->elapsed += 3000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 6 || p->payload[0] != 0x04
            || p->payload[1] != 0x00 || p->payload[2] != 0x27)
            continue;
        CHECK(p->payload[5] != 0x04,
              "an Extended Interface track-position notification went out "
              "during streaming; it is the same per-tick clock the "
              "Display Remote one is suppressed for");
    }

    /* And it comes back. */
    rbstub_set_usb_audio_active(false);
    rbstub_set_usb_audio_streaming(false);
    rbstub_id3()->elapsed += 3000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    bool resumed = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x04)
            resumed = true;
    }
    CHECK(resumed,
          "the Extended Interface position never resumed after streaming "
          "stopped");
}
#endif /* USB_ENABLE_AUDIO */

#ifdef HAVE_IAP_ACCESSORY_POLL
/* The accessory-detect poll has to run on every tick of the iAP thread,
 * and ahead of every early return in iap_periodic().
 *
 * A detached accessory has no subscriptions by definition, and the
 * early returns are all about having no subscriptions -- so putting the
 * poll after any of them means the one state it exists to notice is the
 * one state it never runs in.
 *
 * On the target this reads the accessory line over I2C, which is why it
 * cannot be a tick task: adc_read() takes a mutex and tick tasks run
 * from the timer IRQ. apps/iap/test/uart covers what the real
 * implementation does with the answer. */
void test_session_accessory_poll_runs_every_tick(void)
{
    iaptest_init();
    rbstub_accessory_polls = 0;

    /* Nothing identified, nothing subscribed: the state a detached
     * player is in, and the one every early return fires on. */
    for (int t = 0; t < 5; t++)
        iap_periodic();
    CHECK(rbstub_accessory_polls == 5,
          "the accessory poll ran %d times in 5 ticks with nothing "
          "subscribed; it sits behind an early return",
          rbstub_accessory_polls);

    /* And still with a live session. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x03, 0x08, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x02);
    rbstub_accessory_polls = 0;
    for (int t = 0; t < 5; t++)
        iap_periodic();
    CHECK(rbstub_accessory_polls == 5,
          "the accessory poll ran %d times in 5 ticks on a live session",
          rbstub_accessory_polls);
}
#endif

/* Polling and subscribing are independent, and an accessory may do
 * both.
 *
 * MFi 4.3.13 (p.263) has GetRemoteEventStatus answer "a bitmask of
 * event states that changed", and Table 4-68 (p.264) is that bitmask
 * and nothing else -- no values. So a poll tells the accessory that the
 * track moved, never where to; 4.3.12 (p.257) still owes it the
 * notification carrying the number.
 *
 * 1586845400 made the poll write the notification path's own tracked
 * fields, on the reasoning that both hold "what the accessory was last
 * told". They do not: one holds a value that was sent, the other holds
 * a change that was announced. A poll therefore consumed the change and
 * the notification never went out, which is a worse failure than the
 * one that commit fixed. */
void test_session_polling_does_not_swallow_the_notification(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(40, 3);

    /* Subscribe to shuffle (bit 7) and track index (bit 1). */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x82);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* Something changes, and the accessory polls before the tick that
     * would have reported it. */
    global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
    rbstub_set_playlist(40, 21);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xF1);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetRemoteEventStatus");
        if (r && r->paylen >= 8) {
            uint32_t bits = ((uint32_t)r->payload[4] << 24)
                          | ((uint32_t)r->payload[5] << 16)
                          | ((uint32_t)r->payload[6] << 8)
                          |  (uint32_t)r->payload[7];
            CHECK(bits & (1u << 7), "the poll missed the shuffle change");
            CHECK(bits & (1u << 1), "the poll missed the track change");
        }
    }

    /* The notifications are still owed: the poll said what changed, not
     * what it changed to. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int shuffle = 0, track = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 5 || p->payload[0] != 0x03
            || p->payload[1] != 0x09)
            continue;
        if (p->payload[4] == 0x07) shuffle++;
        if (p->payload[4] == 0x01) track++;
    }
    CHECK(shuffle > 0,
          "the shuffle notification never went out after the accessory "
          "polled; the poll consumed the change and the value was never "
          "sent");
    CHECK(track > 0,
          "the track index notification never went out after the "
          "accessory polled");
}

/* Rockbox has five repeat modes; Table 4-64 (p.262) defines three and
 * calls 0x03-0xFF reserved. REPEAT_SHUFFLE and REPEAT_AB are reachable
 * from Rockbox's own Playback Settings with no accessory involved.
 *
 * With no default arm, no data byte was appended at all -- event 0x08
 * has "Data length in bytes 0x01" (Table 4-61, p.259) and
 * GetiPodStateInfo's infoData follows the same table (Table 4-71,
 * p.265) -- so the packet went out a byte short and the accessory read
 * the checksum as the repeat state. */
void test_session_repeat_modes_outside_the_table(void)
{
    static const int modes[] = { REPEAT_SHUFFLE, REPEAT_AB };
    static const char *names[] = { "REPEAT_SHUFFLE", "REPEAT_AB" };

    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        iaptest_init();
        iaptest_enter_idps();
        iaptest_force_authenticated();
        global_settings.repeat_mode = modes[i];

        /* GetiPodStateInfo, repeat. */
        iaptest_tx_clear();
        IAPTEST_RX(0x03, 0x0C, 0x00, 0xC1, 0x08);
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetiPodStateInfo(repeat) in %s",
              names[i]);
        if (!r)
            continue;
        CHECK(r->paylen == 6,
              "the reply for %s carried %d payload bytes; the repeat "
              "state is one byte and it was missing",
              names[i], r->paylen);
        if (r->paylen >= 6)
            CHECK(r->payload[5] <= 0x02,
                  "%s was reported as 0x%02X; Table 4-64 defines only "
                  "0x00, 0x01 and 0x02", names[i], r->payload[5]);
    }

    /* And the Simple Remote Repeat button still cycles out of them
     * rather than sticking for ever. */
    iaptest_init();
    global_settings.repeat_mode = REPEAT_SHUFFLE;
    iap_repeat_next();
    CHECK(global_settings.repeat_mode != REPEAT_SHUFFLE,
          "the repeat button was a no-op in REPEAT_SHUFFLE, so an "
          "accessory could never cycle out of it");
}

/* Table 5-45 (p.425) bit 00: "Basic play state changes (stop, FFW seek
 * stop, or REW seek stop, using status notification types 0x00, 0x02,
 * or 0x03)". Table 5-47 (p.426) gives type 0x00 as "Playback stopped
 * {0x00}".
 *
 * Nothing ever sent it. The only senders of 0x0027 were types 0x01 and
 * 0x04, so an accessory that subscribed -- including through the
 * one-byte 0x01 form, which this firmware maps onto bits 0, 2, 3 and 5
 * -- was acked Success and then never told playback had stopped. */
void test_session_extended_interface_reports_playback_stopped(void)
{
    iaptest_init();
    rbstub_set_playlist(40, 3);
    iaptest_session_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Six ticks, not four. iap_periodic() sends the 500 ms events on
     * every fifth call and the counter is a function-static that no
     * per-case reset touches, so a loop shorter than the period passes
     * or fails on whatever phase the previous case left behind -- this
     * was green on ipod6g and red on ipodvideo purely because the two
     * run different numbers of cases before it. */
    /* The one-byte form, which the spec says covers basic play state. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xB1, 0x01);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* Stop. */
    rbstub_set_audio_status(0);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    bool saw = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        /* Exactly six: lingo, two command bytes, the transaction ID
         * and the type. A seventh byte would be a parameter type 0x00
         * does not have. */
        if (p && p->paylen == 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x00)
            saw = true;
    }
    CHECK(saw, "playback stopped and the subscribed accessory was never "
               "told; PlayStatusChangeNotification type 0x00 was not sent");

    /* And it does not repeat while it stays stopped. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 6 || p->payload[0] != 0x04
            || p->payload[1] != 0x00 || p->payload[2] != 0x27)
            continue;
        CHECK(p->payload[5] != 0x00,
              "the stopped notification repeated while it stayed stopped");
    }
}

/* State that belongs to one accessory must not reach the next.
 *
 * MFi 1.11.2 (p.57): "Multilingo accessories that support Extended
 * Interface mode do not automatically switch into the Extended
 * Interface mode after the identification process completes. These
 * accessories must use the General lingo mode switching commands to
 * explicitly switch into Extended Interface mode." iap_reset_device()
 * left interface_state alone, so a replacement accessory inherited the
 * mode and 00 03 answered "yes, Extended" to something that had never
 * asked.
 *
 * device.mute had no reset at all: SetRemoteEventNotification cleared
 * it as a side effect, which both hid the gap and made the flag
 * disagree with a codec still holding the attenuation. */
void test_session_mode_and_mute_do_not_outlive_the_accessory(void)
{
    /* An accessory that asks for Extended Interface mode and mutes. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0xD1);
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xD2, 0x04, 0x01, 0x80);   /* mute on */

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x03, 0x00, 0xD3);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to RequestExtendedInterfaceMode");
        if (r && r->paylen >= 5)
            CHECK_EQ_INT(r->payload[4], 0x01,
                         "the accessory that asked for the mode was not "
                         "in it");
    }

    /* It goes away; another arrives. */
    iap_reset_state(IF_IAP_MP(0));
    iaptest_enter_idps();
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x03, 0x00, 0xD4);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to RequestExtendedInterfaceMode");
        if (r && r->paylen >= 5)
            CHECK_EQ_INT(r->payload[4], 0x00,
                         "the replacement accessory inherited Extended "
                         "Interface mode it never asked for");
    }

    CHECK(device.mute == false,
          "the replacement accessory inherited the previous one's mute "
          "state");
}

/* The accessory-info sweep has to finish. iap_task() drops from the
 * 100 ms wakeup to the 1 s one only when accinfo is ACCST_NONE, and the
 * sweep left it at ACCST_DATA once every capability had been queried --
 * so the power saving was unreachable for the life of the connection.
 *
 * It could not be reached by accident either: Table 3-48 (p.148) makes
 * capability bits 1, 4, 5, 6 and 7 "Required; set to 1", so
 * device.capabilities is always non-zero and every RetAccessoryInfo
 * sets ACCST_DATA again. */
void test_session_accessory_info_sweep_finishes(void)
{
    iaptest_init();
    iaptest_enter_idps();
    /* AUST_AUTH, not iaptest_force_authenticated()'s AUST_CERTDONE:
     * DEVICE_AUTH_RUNNING is true for every state between NONE and
     * AUTH, and the sweep is gated on it not running. */
    device.auth.state = AUST_AUTH;

    /* RetAccessoryInfo type 0x00: the capability word. Two bits, so the
     * sweep has somewhere to go and an end to reach. */
    IAPTEST_RX(0x00, 0x28, 0x00, 0xE5, 0x00, 0x00, 0x00, 0x00, 0x0A);

    /* Let it walk. Each pass queries one capability and waits for a
     * reply that never comes, so the sweep is driven by the ticks. */
    for (int t = 0; t < 40; t++) {
        if (device.accinfo == ACCST_SENT)
            device.accinfo = ACCST_DATA;    /* stand in for the reply */
        iap_periodic();
    }
    if (device.accinfo == ACCST_SENT) {
        device.accinfo = ACCST_DATA;
        iap_periodic();
    }
    CHECK_EQ_INT(device.accinfo, ACCST_NONE,
                 "the sweep never returned to idle, so iap_task() stays "
                 "on the 100 ms wakeup for the whole connection");
}

/* Table 4-63 (p.262): 0x01 "Shuffle tracks and songs", 0x02 "Shuffle
 * albums". This device cannot shuffle albums. It answered Success and
 * shuffled tracks instead, while the Extended Interface sibling refused
 * anything above 0x01 -- two answers to the same request. */
void test_session_album_shuffle_is_refused_on_both_lingoes(void)
{
    iaptest_init();
    rbstub_set_playlist(40, 3);
    iaptest_session_extended();

    /* Display Remote: SetiPodStateInfo, shuffle, albums. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xE6, 0x07, 0x02, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to SetiPodStateInfo(shuffle, albums)");
        if (r && r->paylen >= 5)
            CHECK(r->payload[4] != 0x00,
                  "Display Remote accepted Shuffle albums and shuffled "
                  "tracks instead");
    }

    /* Extended Interface: SetShuffle, albums. */
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0xE7, 0x02);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to SetShuffle(albums)");
        if (r && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "Extended Interface accepted Shuffle albums");
    }

    /* And 0x01 still works on both. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xE8, 0x07, 0x01, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 5)
            CHECK_EQ_INT(r->payload[4], 0x00,
                         "Shuffle tracks was refused");
    }
}

/* Table 4-62 (p.262): 0x00 stopped, 0x01 playing, 0x02 paused. Four
 * sites encoded that separately and two disagreed.
 *
 * With recording paused -- AUDIO_STATUS_RECORD | AUDIO_STATUS_PAUSE,
 * which both targets can reach since HAVE_RECORDING is set for each --
 * Display Remote tested the PLAY bit and said stopped while Extended
 * Interface tested the PAUSE bit and said paused. Extended Interface
 * also had no else, so a stopped player left the byte at whatever the
 * reply buffer held. */
void test_session_play_state_is_one_answer(void)
{
    static const struct { int status; unsigned char want; const char *what; } t[] = {
        { AUDIO_STATUS_PLAY,                        0x01, "playing" },
        { AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE,   0x02, "paused" },
        { 0,                                        0x00, "stopped" },
        { AUDIO_STATUS_RECORD | AUDIO_STATUS_PAUSE, 0x00, "recording paused" },
        { AUDIO_STATUS_RECORD,                      0x00, "recording" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        rbstub_set_playlist(40, 3);
        iaptest_session_extended();
        rbstub_set_audio_status(t[i].status);

        /* Display Remote: GetiPodStateInfo, play status. */
        iaptest_tx_clear();
        IAPTEST_RX(0x03, 0x0C, 0x00, 0xF1, 0x03);
        unsigned char l3 = 0xFF;
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL, "no Display Remote answer while %s", t[i].what);
            if (r && r->paylen >= 6)
                l3 = r->payload[5];
        }

        /* Extended Interface: GetPlayStatus. */
        iaptest_tx_clear();
        IAPTEST_RX(0x04, 0x00, 0x1C, 0x00, 0xF2);
        unsigned char l4 = 0xFE;
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL, "no Extended Interface answer while %s",
                  t[i].what);
            /* 04 0x001D, transaction ID, total, elapsed, state:
             * 3 + 2 + 4 + 4 + 1, so the state is the last byte. */
            if (r && r->paylen >= 14)
                l4 = r->payload[13];
        }

        /* MFi 5.1.28 (p.421): "If there is no track currently playing
         * or paused, an index of -1 (0xFFFFFFFF) is returned." Testing
         * the PAUSE bit directly answers yes for RECORD|PAUSE, so a
         * paused recording handed out a stale playing index while
         * GetPlayStatus said Stopped for the same instant. */
        iaptest_tx_clear();
        IAPTEST_RX(0x04, 0x00, 0x1E, 0x00, 0xF3);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL, "no answer to GetCurrentPlayingTrackIndex "
                             "while %s", t[i].what);
            if (r && r->paylen >= 9) {
                uint32_t idx = ((uint32_t)r->payload[5] << 24)
                             | ((uint32_t)r->payload[6] << 16)
                             | ((uint32_t)r->payload[7] << 8)
                             |  (uint32_t)r->payload[8];
                if (t[i].want == 0x00)
                    CHECK(idx == 0xFFFFFFFF,
                          "while %s the track index was %u, not -1",
                          t[i].what, idx);
                else
                    CHECK(idx != 0xFFFFFFFF,
                          "while %s the track index was -1", t[i].what);
            }
        }

        CHECK(l3 == l4,
              "while %s the two lingoes disagree: Display Remote 0x%02X, "
              "Extended Interface 0x%02X", t[i].what, l3, l4);
        CHECK(l3 == t[i].want,
              "while %s the play state was reported as 0x%02X, not 0x%02X",
              t[i].what, l3, t[i].want);
    }
}

/* An accessory must not be told about something that happened before it
 * arrived.
 *
 * SetPlayStatusChangeNotification wrote only the mask.
 * device.pb_play_status kept the previous accessory's audio_status(),
 * so if that was Playing and audio had since stopped, the first tick
 * after a new accessory subscribed to bit 00 sent it a "Playback
 * stopped" for a transition it never saw. The Display Remote sibling
 * re-baselines every tracked value when an accessory subscribes; this
 * one did not. */
void test_session_play_status_subscription_baselines(void)
{
    iaptest_init();
    rbstub_set_playlist(40, 3);
    iaptest_session_extended();

    /* The first accessory is subscribed and playing. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xA1, 0x01);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* It goes away while playing, and playback stops with nobody
     * listening. */
    iap_reset_state(IF_IAP_MP(0));
    rbstub_set_audio_status(0);

    /* A replacement arrives and subscribes. */
    iaptest_session_extended();
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xA2, 0x01);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 6 || p->payload[0] != 0x04
            || p->payload[1] != 0x00 || p->payload[2] != 0x27)
            continue;
        CHECK(p->payload[5] != 0x00,
              "the replacement accessory was told playback stopped, for "
              "a transition that happened before it connected");
    }
}

/* A button the departed accessory was holding must not stay held.
 *
 * PlayControl(StartFF) sets iap_timeoutbtn to IAP_BTN_HELD, roughly ten
 * seconds at the 10 Hz countdown. iap_reset_device() cleared every
 * other piece of per-accessory state and not this, so an accessory
 * unplugged mid-seek left BUTTON_RC_RIGHT asserted --
 * button-clickwheel.c ORs remote_control_rx() into every physical
 * press, so for that whole window the clickwheel was unusable and the
 * WPS kept repeating SEEKFWD. MFi 4.2.4 (p.218) resets the button
 * status after 200 ms of silence; a detach is at least that. */
void test_session_a_held_button_does_not_outlive_the_accessory(void)
{
    iaptest_init();
    rbstub_set_playlist(40, 3);
    iaptest_session_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Start a seek and confirm the button is actually held. */
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0xB5, 0x05);
    CHECK(iap_remotebtn != BUTTON_NONE,
          "PlayControl(StartFF) raised no button, so the case proves "
          "nothing about releasing one");

    /* The accessory goes away mid-seek. */
    iap_reset_state(IF_IAP_MP(0));

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "the seek button is still held after the accessory "
                 "detached; every clickwheel press is OR'd with it");
    CHECK_EQ_INT(iap_timeoutbtn, 0,
                 "the button timeout survived the detach, so it will be "
                 "released seconds later instead of now");
}

/* Table 5-45 (p.425) bit 00: "Basic play state changes (stop, FFW seek
 * stop, or REW seek stop, using status notification types 0x00, 0x02,
 * or 0x03)". Table 5-47 (p.426) gives both seek-stop types no
 * parameter.
 *
 * Only type 0x00 was ever sent. An accessory subscribed to bit 00
 * started a seek, was acked Success, and was never told it ended -- so
 * its display stayed in seek mode with nothing to end it. */
void test_session_seek_stop_is_reported(void)
{
    static const struct { unsigned char start, want; const char *what; } t[] = {
        { 0x05, 0x02, "fast forward" },
        { 0x06, 0x03, "rewind" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        rbstub_set_playlist(40, 3);
        iaptest_session_extended();
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);

        IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xC5, 0x01);   /* bit 00 */

        unsigned char go[6] = { 0x04, 0x00, 0x29, 0x00, 0xC6, t[i].start };
        iaptest_rx(go, sizeof(go));
        iaptest_button_sample(4);

        iaptest_tx_clear();
        IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0xC7, 0x07);   /* end seek */

        bool saw = false;
        for (int k = 0; k < iaptest_tx_count(); k++) {
            const struct iaptest_pkt *p = iaptest_tx(k);
            if (p && p->paylen == 6 && p->payload[0] == 0x04
                && p->payload[1] == 0x00 && p->payload[2] == 0x27
                && p->payload[5] == t[i].want)
                saw = true;
        }
        CHECK(saw, "%s ended and the subscribed accessory was not told; "
                   "PlayStatusChangeNotification type 0x%02X was not sent",
              t[i].what, t[i].want);
    }

    /* Ending a seek that never started reports nothing. */
    iaptest_init();
    rbstub_set_playlist(40, 3);
    iaptest_session_extended();
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xC8, 0x01);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0xC9, 0x07);

    /* Count them, do not filter and then assert inside the loop. The
     * firmware sends no 04 00 27 here, so the loop ran zero times and
     * the only CHECK in this half never executed -- and it would have
     * passed anyway: a spurious notification for a seek that never
     * began carries device.pb_seeking, which is 0x00, and the
     * predicate excluded only 0x02 and 0x03. Dropping
     * "&& device.pb_seeking" from iap_seek_stop()'s bit-00 guard left
     * the suite green. */
    int spurious = 0;
    for (int k = 0; k < iaptest_tx_count(); k++) {
        const struct iaptest_pkt *p = iaptest_tx(k);
        if (p && p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27)
            spurious++;
    }
    CHECK_EQ_INT(spurious, 0,
                 "a play-status notification was sent for a seek that "
                 "never began");
}

/* Restore on Exit.
 *
 * MFi 5.1.42 (p.432): "This command has an optional field called
 * Restore on Exit... A nonzero value restores the original shuffle
 * setting of the Apple device when the accessory is detached. If this
 * field is zero, the shuffle setting set by the accessory overwrites
 * the original setting and persists after the accessory is detached
 * from the Apple device." Table 5-55 (p.433) puts the field in the
 * second parameter; 5.1.45 (p.434) and Table 5-60 say the same for
 * repeat. The Note on p.433: "Accessory developers are encouraged to
 * always use the Restore on Exit field with a nonzero value."
 *
 * Both fields were read as nothing and both settings went straight to
 * config.cfg, so a head unit asking for a temporary change rewrote the
 * user's shuffle and repeat permanently, on every connect.
 */
void test_session_restore_on_exit_puts_the_settings_back(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();

    /* Two-byte form: change it, and ask for it back on detach. */
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x70, 0x01, 0x01);
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x71, 0x02, 0x01);

    CHECK(global_settings.playlist_shuffle == 1,
          "SetShuffle did not take effect, so the restore below would "
          "prove nothing");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "SetRepeat did not take effect");
    CHECK_EQ_INT(rbstub_calls.settings_save, 0,
                 "a change the accessory asked to have undone on detach "
                 "was written to config.cfg");

    /* Detach, and let the tick do the work -- the restore cannot run
     * where the detach is noticed, which on the 6G is a tick. */
    iap_reset_device(&device);
    iap_periodic();

    CHECK(global_settings.playlist_shuffle == 0,
          "shuffle was not restored on detach");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_OFF,
                 "repeat was not restored on detach");
    CHECK_EQ_INT(rbstub_calls.settings_save, 0,
                 "putting the original setting back saved it again; it "
                 "was never overwritten on disk in the first place");
}

/* And the other half of the same sentence: with the field zero, or
 * absent, the change persists and is saved. */
void test_session_without_restore_on_exit_the_change_persists(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();

    /* A two-byte form asking for a restore first, so the byte past the
     * one-byte forms below is a 0x01 sitting in the receive buffer. An
     * unguarded L4_HAVE() read finds it and latches a restore the
     * accessory did not ask for -- at which point the change is not
     * saved and is undone on detach, which is the opposite of what the
     * one-byte form means. */
    /* Each one-byte form is preceded immediately by the two-byte form
     * of the same command asking for a restore, so the byte just past
     * the short packet is a known 0x01. The receive buffer's base moves
     * as packets are consumed, so priming further back does not put a
     * 1 where the missing parameter would be read. */
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x70, 0x00, 0x01);
    global_settings.playlist_shuffle = 0;
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x72, 0x01);
    CHECK(rbstub_calls.settings_save >= 1,
          "a one-byte SetShuffle did not persist; its Restore on Exit "
          "came from past the end of the packet");

    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x71, 0x00, 0x01);
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x73, 0x02);
    CHECK(rbstub_calls.settings_save >= 1,
          "a one-byte SetRepeat did not persist; its Restore on Exit "
          "came from past the end of the packet");

    iap_reset_device(&device);
    iap_periodic();

    CHECK(global_settings.playlist_shuffle == 1,
          "shuffle was restored on detach although the accessory did "
          "not ask for it");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "repeat was restored on detach although the accessory "
                 "did not ask for it");

    /* And the two-byte form with the field explicitly zero means the
     * same as the field being absent. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();

    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x74, 0x01, 0x00);
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x75, 0x02, 0x00);
    CHECK(rbstub_calls.settings_save >= 2,
          "an explicit Restore on Exit of zero must persist, but "
          "settings_save() ran %d times", rbstub_calls.settings_save);

    iap_reset_device(&device);
    iap_periodic();
    CHECK(global_settings.playlist_shuffle == 1,
          "shuffle was restored with the field explicitly zero");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "repeat was restored with the field explicitly zero");
}

/* Table 5-45 (p.425) bit 01, the extended play state.
 *
 * Verbatim: "01 Extended play state changes (playback stop, FFW seek
 * start, REW seek start, playback started, FFW/REW seek stop, or
 * playback pause using status notification type 0x06)." Table 5-47
 * (p.427) gives type 0x06 as {0x06, playState:1} with 0x02 Stopped,
 * 0x05 FFW seek started, 0x06 REW seek started, 0x07 FFW/REW seek
 * stopped, 0x0A Playing, 0x0B Paused.
 *
 * This is the only asynchronous route to play and pause. Bit 00 carries
 * stop and seek-stop only, and the one-byte subscription maps to bits
 * 0, 2, 3 and 5. So a head unit with lingoes 0 and 4 had no way to
 * learn the user had paused -- the mask was stored whole and acked
 * Success, and then nothing was ever sent for it.
 */
static const struct iaptest_pkt *pb_ext(unsigned char *state)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 7 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == 0x06) {
            *state = p->payload[6];
            return p;
        }
    }
    return NULL;
}

void test_session_extended_play_state_is_reported(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Four-byte subscription to bit 01 alone. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x80, 0x00, 0x00, 0x00, 0x02);

    unsigned char st = 0xAA;
    struct { int status; unsigned char want; const char *what; } steps[] = {
        { AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE, 0x0B, "Paused"   },
        { AUDIO_STATUS_PLAY,                      0x0A, "Playing"  },
        { 0,                                      0x02, "Stopped"  },
    };

    for (unsigned i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        rbstub_set_audio_status(steps[i].status);
        iaptest_tx_clear();
        for (int t = 0; t < 6; t++)
            iap_periodic();

        st = 0xAA;
        CHECK(pb_ext(&st) != NULL,
              "no extended play state notification for %s",
              steps[i].what);
        CHECK_EQ_INT(st, steps[i].want, steps[i].what);
    }

    /* And it is change-detected: a tick with nothing new is silent. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    st = 0xAA;
    CHECK(pb_ext(&st) == NULL,
          "the extended play state was resent with nothing changed "
          "(state 0x%02X)", st);

    /* Seek start and stop come from the PlayControl arms, where the
     * direction is known. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x81, 0x05);   /* FFW start */
    st = 0xAA;
    CHECK(pb_ext(&st) != NULL && st == 0x05,
          "FFW seek start was not reported (state 0x%02X)", st);

    iaptest_button_sample(4);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x82, 0x07);   /* seek stop */
    st = 0xAA;
    CHECK(pb_ext(&st) != NULL && st == 0x07,
          "seek stop was not reported (state 0x%02X)", st);
}

/* An accessory that did not subscribe to bit 01 must not receive it. */
void test_session_extended_play_state_needs_the_bit(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Every bit but 01. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x83, 0x00, 0x00, 0x1F, 0xFD);

    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    unsigned char st = 0xAA;
    CHECK(pb_ext(&st) == NULL,
          "type 0x06 was sent to an accessory that did not subscribe to "
          "Table 5-45 bit 01 (state 0x%02X)", st);

    /* The seek arms call the emitter directly rather than through the
     * change detector, so they are the ones that would leak if only the
     * emitter's own check held the line. Bit 00 is subscribed here, so
     * its seek-stop still goes out -- what must not appear is a type
     * 0x06 beside it. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x84, 0x05);   /* FFW start */
    st = 0xAA;
    CHECK(pb_ext(&st) == NULL,
          "an FFW seek start sent type 0x06 without bit 01 (state "
          "0x%02X)", st);

    iaptest_button_sample(4);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x85, 0x07);   /* seek stop */
    st = 0xAA;
    CHECK(pb_ext(&st) == NULL,
          "a seek stop sent type 0x06 without bit 01 (state 0x%02X)", st);
}

/* The once-per-press latches do not outlive the accessory that set
 * them.
 *
 * Shuffle, Repeat, Stop and Mute each latch so a held button acts once
 * -- the accessory repeats its status every 30 to 100 ms. iap_periodic()
 * clears them when the auto-release timer lapses, which is under a
 * second away, but iap_reset_device() did not. A replug inside that
 * window let the new accessory's first press of any of the four be
 * swallowed as a repeat of the old one's. */
void test_session_button_latches_do_not_survive_a_swap(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();

    /* Hold Stop: it acts once and stays latched. */
    IAPTEST_RX(0x02, 0x00, 0x80, 0x00);
    CHECK_EQ_INT(rbstub_calls.stop, 1, "the first Stop did not stop");
    CHECK(iap_btnstop, "Stop did not latch, so the swap below proves "
                       "nothing");

    /* The accessory goes, mid-press, without the tick that would have
     * cleared it. */
    iap_reset_device(&device);
    CHECK(!iap_btnstop,
          "the Stop latch survived the accessory that set it, so the "
          "next one's first Stop is swallowed");
    CHECK(!iap_btnshuffle, "the Shuffle latch survived the accessory");
    CHECK(!iap_btnrepeat, "the Repeat latch survived the accessory");

    /* And the next accessory's first press works. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x02, 0x00, 0x80, 0x00);
    CHECK_EQ_INT(rbstub_calls.stop, 2,
                 "the new accessory's first Stop was swallowed as a "
                 "repeat of the previous one's");
}

/* An accessory that polls instead of subscribing sees every event, not
 * the five that happened to share a shadow with the notification path.
 *
 * MFi 4.3.13 (p.263): the reply carries "a bitmask of event states that
 * changed since the last GetRemoteEventStatus command", and "This
 * command may be used to poll the Apple device for certain event
 * changes without enabling asynchronous remote event notification."
 *
 * Every other write to changed_notifications is inside a
 * "device.notifications & BIT_N(n)" block behind a subscription early
 * return, so without a subscription the elapsed-time bar and the
 * battery icon never moved. */
static uint32_t poll_events(unsigned char tid)
{
    iaptest_tx_clear();
    {
        unsigned char p[4] = { 0x03, 0x0A, 0x00, tid };
        iaptest_rx(p, sizeof(p));
    }
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 8 && p->payload[0] == 0x03
            && p->payload[1] == 0x0B)
            return ((uint32_t)p->payload[4] << 24)
                 | ((uint32_t)p->payload[5] << 16)
                 | ((uint32_t)p->payload[6] << 8)
                 |  (uint32_t)p->payload[7];
    }
    return 0xFFFFFFFFu;   /* no reply */
}

void test_session_polling_reports_every_tracked_event(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_id3()->elapsed = 0;

    /* Settle, and prove an untouched session reports nothing -- or the
     * assertions below would pass on noise. */
    poll_events(0x10);
    CHECK_EQ_INT(poll_events(0x11), 0,
                 "a poll on an untouched session reported changes");

    /* Track position: bit 00 in milliseconds, bit 15 in seconds. */
    rbstub_id3()->elapsed = 4200;
    {
        uint32_t bits = poll_events(0x12);
        CHECK(bits & (1u << 0),
              "the elapsed time moved and the poll did not say so "
              "(0x%08X)", bits);
        CHECK(bits & (1u << 15),
              "the elapsed time in seconds moved and the poll did not "
              "say so (0x%08X)", bits);
    }

    /* Battery: bit 05. */
    rbstub_set_battery(42);
    {
        uint32_t bits = poll_events(0x13);
        CHECK(bits & (1u << 5),
              "the battery level moved and the poll did not say so "
              "(0x%08X)", bits);
    }

    /* And reading clears, as the same paragraph requires. */
    CHECK_EQ_INT(poll_events(0x14), 0,
                 "the poll did not clear the bits it reported");
}

/* The whole of Table 4-65, not two of its six values.
 *
 * MFi Table 4-65 (p.263): "0x00 Internal battery power, low power
 * (< 30%), 0x01 Internal battery power, 0x02 External power, battery
 * pack, no charging, 0x03 External power, no charging, 0x04 External
 * power, battery charging, 0x05 External power, battery charged."
 *
 * Anything that was not NO_CHARGER reported 0x04, so a full battery on
 * a dock read as still charging for as long as it stayed there, and
 * CHARGER_UNPLUGGED -- which powermgmt.h calls a "Transitional state
 * during CHARGER=>NO_CHARGER" -- showed a charging icon for the instant
 * after the charger came out.
 *
 * The first attempt at that fix did not work, and this case is why it
 * looked as though it had: it drove charge_state directly, to pairs
 * the hardware cannot produce. charging_algorithm_step()
 * (firmware/powermgmt.c:593) derives charge_state from
 * charger_input_state alone -- CHARGER means CHARGING, always -- so
 * {CHARGER, DISCHARGING} is not a state, and neither is a battery
 * level of 100 while charging, which powermgmt.c:365 clamps to 99. The
 * two rows that exercised 0x03 and 0x05 were both impossible, and the
 * branch they covered was unreachable on the device.
 *
 * The signal that does answer the question is charging_state(), one
 * GPIO bit per target read straight from the charger IC. This case
 * drives that, and every row below is a state the hardware can be in.
 *
 * The battery byte is zero on external power. Table 4-61 (p.259) event
 * 0x05: "if an external power status is returned, the battery level is
 * invalid and is returned as 0", repeated at 4.3.30 (p.279) and in
 * Table 4-91 (p.279). It went out as a level on the strength of a
 * comment citing Table 4-64 -- which is Repeat state, on p.262. */
extern unsigned int charger_input_state;

static bool power_event(unsigned char *state, unsigned char *level)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 7 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x05) {
            *state = p->payload[5];
            *level = p->payload[6];
            return true;
        }
    }
    return false;
}

void test_session_power_state_covers_the_table(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Subscribe to Power/Battery, Table 4-60 (p.257) bit 05. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x20);

    struct { unsigned int charger; bool charging; int batt;
             unsigned char want; const char *what; } tc[] = {
        { 0 /*NO_CHARGER*/,        false, 20, 0x00, "battery, low"     },
        { 0 /*NO_CHARGER*/,        false, 80, 0x01, "battery"          },
        { 1 /*CHARGER_UNPLUGGED*/, false, 80, 0x01, "charger just out" },
        { 3 /*CHARGER*/,           true,  80, 0x04, "charging"         },
        /* 99 is the ceiling powermgmt.c:365 imposes while external
         * power is present, so it is what "as full as this reads"
         * means. */
        { 3 /*CHARGER*/,           false, 99, 0x05, "external, charged"},
        { 3 /*CHARGER*/,           false, 80, 0x03, "external, no charge"},
        { 2 /*CHARGER_PLUGGED*/,   true,  50, 0x04, "charger just in"  },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        charger_input_state = tc[i].charger;
        rbstub_set_charging(tc[i].charging);
        rbstub_set_battery(tc[i].batt);
        device.power_reported = false;

        iaptest_tx_clear();
        for (int t = 0; t < 6; t++)
            iap_periodic();

        unsigned char st = 0xAA, lvl = 0xAA;
        CHECK(power_event(&st, &lvl),
              "no Power/Battery event for %s", tc[i].what);
        CHECK_EQ_INT(st, tc[i].want, tc[i].what);
        /* Internal battery: the level. External power: zero, because
         * the spec says the field is invalid there. */
        bool internal = (tc[i].charger == 0 || tc[i].charger == 1);
        CHECK_EQ_INT(lvl, internal ? (tc[i].batt * 255) / 100 : 0,
                     "the battery level byte does not match Table 4-91");
    }
}


/* The 6G detach IRQ may post, but it must not block or take a mutex. */
void test_session_reset_is_safe_from_a_tick(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Give it something to unwind: a subscription, a mode, a latched
     * button, and a setting the accessory asked to have restored. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x20, 0x00, 0x00, 0x00, 0x0F);
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x21, 0x01, 0x01);
    /* Five bytes, not four. Under IDPS doff is 2, so the third and
     * fourth bytes are the transaction ID and lingo 2 then wants
     * CHECKLEN(5) -- and CHECKLEN suppresses the ack for command 0x00,
     * so the four-byte version vanished with no trace and the latched
     * button this case says it sets up never existed. Adding
     * "if (iap_btnstop) yield();" to iap_reset_device(), which is the
     * exact blocking call this case exists to catch, left the suite
     * green. */
    IAPTEST_RX(0x02, 0x00, 0x80, 0x00, 0x80);
    iaptest_button_sample(2);
    CHECK(iap_btnstop, "the Stop latch was not set, so the reset below "
                       "has one less thing to unwind than this case says");

    iaptest_irq_context = true;
    iap_reset_device(&device);
    iaptest_irq_context = false;

    /* The work it deferred still has to happen, on the thread. */
    iap_periodic();
    CHECK(!device.poll_baseline_pending,
          "the poll baseline was deferred out of the tick and then "
          "never taken");
}

/* A packet the accessory left behind does not re-establish its session.
 *
 * iap_reset_state() reset the device and left the receive buffer alone:
 * a packet that had arrived whole before the detach was still there
 * afterwards, and iap_handlepkt() drained it into the fresh session.
 * One buffered Identify put the negotiated lingoes, the authentication
 * state and the power-notify flag all back -- the session
 * re-established itself from an accessory that had gone, and the next
 * one inherited it.
 *
 * MFi 4.3.11 (p.255): "On accessory detach, event notification is reset
 * to the default disabled state." */
void test_session_a_buffered_packet_does_not_outlive_the_detach(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
    iaptest_force_authenticated();
    CHECK(device.lingoes != 0, "the session did not come up");

    /* Feed a whole Identify into the framer without handling it, the
     * way bytes sit in the buffer between the framer and the tick. */
    {
        static const unsigned char id[14] = { 0x00, 0x13,
                                              0x00, 0x00, 0x00, 0x0D,
                                              0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00 };
        unsigned char frame[32];
        int n = 0, sum = 0;
        frame[n++] = 0xFF; frame[n++] = 0x55;
        frame[n++] = (unsigned char)sizeof(id); sum = sizeof(id);
        for (unsigned i = 0; i < sizeof(id); i++) {
            frame[n++] = id[i];
            sum += id[i];
        }
        frame[n++] = (unsigned char)(0x100 - (sum & 0xFF));
        for (int i = 0; i < n; i++)
            iap_getc(IF_IAP_MP(0,) frame[i]);
    }

    /* The accessory goes. */
    iap_reset_state(IF_IAP_MP(0));
    CHECK_EQ_INT(device.lingoes, 0,
                 "the detach did not clear the negotiated lingoes");

    /* Whatever was buffered must be gone with it. */
    iap_handlepkt();
    CHECK(device.lingoes == 0,
          "a packet buffered before the detach re-established the "
          "session (lingoes = 0x%08X)", device.lingoes);
    CHECK(!DEVICE_AUTHENTICATED,
          "a packet buffered before the detach restored authentication");
    CHECK(!device.do_power_notify,
          "a packet buffered before the detach restored the "
          "power-notify subscription");
}

/* A capability the device has no question for does not wedge the sweep.
 *
 * iap_periodic() walks the bits an accessory advertised in
 * RetAccessoryInfo, asking about one per tick. The switch has arms for
 * bits 1 to 9; Table 3-48 (p.148) defines bits up to 18, and bit 18,
 * "Asynchronous playback state changes", is mandatory for Bluetooth
 * accessories. Any bit past 9 fell through it and then set accinfo to
 * ACCST_SENT anyway -- "waiting for a reply" to a question never
 * asked. No reply could arrive, the next bit was never queried, and
 * iap_task() never reached its 1 Hz idle drop: 10 Hz wakeups for the
 * rest of the connection, on a battery-powered player. */
void test_session_capability_sweep_does_not_stall(void)
{
    /* No iaptest_force_authenticated() here, deliberately.
     * IdentifyDeviceLingoes already leaves AUST_AUTH, and the helper
     * would put the state *back* to AUST_CERTDONE -- which
     * DEVICE_AUTH_RUNNING reads as a handshake in progress, and the
     * sweep only runs when one is not. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
    CHECK(!DEVICE_AUTH_RUNNING,
          "the sweep only runs outside a handshake");

    /* RetAccessoryInfo type 0x00: capabilities with a bit the device
     * knows (0x01, Name) and one it does not (0x12, bit 18). */
    IAPTEST_RX(0x00, 0x28, 0x00, 0x00, 0x04, 0x00, 0x02);
    CHECK(device.capabilities != 0,
          "the capability word was not recorded, so nothing below is "
          "being tested (0x%08X)", device.capabilities);

    /* Drive the sweep, answering each question as an accessory would.
     * Without a reply the sweep correctly waits, so a test that only
     * ticks proves nothing about the bits after the first. */
    for (int t = 0; t < 40; t++) {
        iaptest_tx_clear();
        iap_periodic();

        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *q = iaptest_tx(i);
            if (!q || q->paylen < 3 || q->payload[0] != 0x00
                || q->payload[1] != 0x27)
                continue;

            /* RetAccessoryInfo for whatever was asked: the type byte
             * and one byte of payload is enough for every arm. */
            unsigned char r[5] = { 0x00, 0x28,
                                   q->payload[q->paylen - 1], 0x00, 0x00 };
            iaptest_rx(r, sizeof(r));
        }
    }

    CHECK_EQ_INT(device.capabilities & ~device.capabilities_queried, 0,
                 "the sweep stopped with capabilities left unqueried");
    CHECK(device.accinfo != ACCST_SENT,
          "the sweep is still waiting for a reply to a question it "
          "never asked, which holds the tick at 10 Hz for the rest of "
          "the connection");
}

/* An accessory that muted the player does not get to leave it silent.
 *
 * SetiPodStateInfo mute drives the hardware down with
 * sound_set_volume(sound_min(...)) while keeping the user's level in
 * global_status.volume, and unmuting calls setvol() to put it back.
 * The detach path cleared device.mute and nothing else -- so unplugging
 * a muted accessory left the codec at minimum, the UI showing the old
 * dB, and device.mute reporting not-muted: a player that had gone
 * silent with nothing to say why and no way back but the volume keys.
 *
 * The lift cannot happen where the detach is noticed. setvol() reaches
 * the codec and iap_reset_device() runs from a tick on the 6G, so it is
 * flagged there and done from iap_periodic() -- and setvol()'s stub is
 * guarded, so doing it in the reset fails this case by name. */
void test_session_mute_does_not_outlive_the_accessory(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    iaptest_force_authenticated();

    global_status.volume = -25;
    rbstub_reset_calls();

    /* SetiPodStateInfo, info type 0x04, mute on. */
    IAPTEST_RX(0x03, 0x0E, 0x04, 0x01, 0x00);
    CHECK(device.mute, "the accessory's mute did not take");
    CHECK(rbstub_calls.sound_set_volume > 0,
          "muting did not drive the hardware volume");
    CHECK_EQ_INT(global_status.volume, -25,
                 "muting must keep the user's level, not overwrite it");

    /* It goes, muted. */
    rbstub_reset_calls();
    iaptest_irq_context = true;
    iap_reset_device(&device);
    iaptest_irq_context = false;

    CHECK(!device.mute, "the detach left device.mute set");
    CHECK_EQ_INT(rbstub_calls.sound_set_volume, 0,
                 "the codec was touched from the tick");

    iap_periodic();
    CHECK(rbstub_calls.sound_set_volume > 0,
          "the accessory's mute was never lifted, so the player is "
          "still silent with nothing muted");
    CHECK_EQ_INT(global_status.volume, -25,
                 "lifting the mute must restore the user's level");

    /* And a detach with nothing muted must not touch the volume. */
    rbstub_reset_calls();
    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(rbstub_calls.sound_set_volume, 0,
                 "a detach with nothing muted moved the volume");
}

/* Display Remote honours Restore on Exit too.
 *
 * MFi Table 4-74 (pp.268-269) gives SetiPodStateInfo info types 0x07
 * and 0x08 a second byte, bRestoreOnExit, and Table 4-75 (p.270) its
 * meaning: "0x00 Do not save the original state. 0x01 Save the original
 * state and restore it on exit."
 *
 * The byte was read nowhere. The Extended Interface pair was fixed
 * earlier and Display Remote was not, because the latch lived in
 * iap-lingo4.c -- so an accessory speaking the lingo it gets without
 * asking wrote the user's shuffle and repeat to config.cfg whatever it
 * requested. The latch is with the setting now and both lingoes reach
 * it through iap_shuffle_state() and iap_repeat_state(). */
void test_session_display_remote_restore_on_exit(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));

    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();

    /* Info type 0x07 shuffle on, and 0x08 repeat all, both asking for
     * the original back on detach. */
    IAPTEST_RX(0x03, 0x0E, 0x07, 0x01, 0x01);
    IAPTEST_RX(0x03, 0x0E, 0x08, 0x02, 0x01);

    CHECK(global_settings.playlist_shuffle == 1,
          "SetiPodStateInfo shuffle did not take");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "SetiPodStateInfo repeat did not take");
    CHECK_EQ_INT(rbstub_calls.settings_save, 0,
                 "a change the accessory asked to have undone was "
                 "written to config.cfg");

    iap_reset_device(&device);
    iap_periodic();

    CHECK(global_settings.playlist_shuffle == 0,
          "shuffle was not restored on detach");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_OFF,
                 "repeat was not restored on detach");

    /* And with the byte zero it persists, as Table 4-75 says. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();

    IAPTEST_RX(0x03, 0x0E, 0x07, 0x01, 0x00);
    IAPTEST_RX(0x03, 0x0E, 0x08, 0x02, 0x00);
    CHECK(rbstub_calls.settings_save >= 2,
          "a change with Restore on Exit clear must persist, but "
          "settings_save() ran %d times", rbstub_calls.settings_save);

    iap_reset_device(&device);
    iap_periodic();
    CHECK(global_settings.playlist_shuffle == 1,
          "shuffle was restored although the accessory did not ask");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "repeat was restored although the accessory did not ask");
}

/* Table 5-45 bits 04 and 12, which were accepted and never served.
 *
 * Bit 04 is "Track time offset (sec)" -- Table 5-47 (p.427) type 0x07,
 * {0x07, trackOffsetSec:4}. The seconds counterpart of bit 03, and the
 * one an accessory that only draws a clock wants. The Display Remote
 * lingo has served its equivalent, bit 15, all along.
 *
 * Bit 12 is "Playback engine contents changed" -- type 0x0E, {0x0E,
 * numTracks:4}. A head unit keeping its own list has no other way to
 * learn the queue was rebuilt, and MFi p.59 rules out the alternative:
 * "Continuous polling, using GetPlayStatus, is not an acceptable
 * alternative."
 *
 * Both were stored in the mask, acked Success and never sent -- the
 * same defect bit 01 had. */
static const struct iaptest_pkt *pb_type(unsigned char type)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == type)
            return p;
    }
    return NULL;
}

void test_session_track_seconds_and_queue_size_are_reported(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_id3()->elapsed = 1000;

    /* Subscribe to bits 04 and 12 only. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xB0, 0x00, 0x00, 0x10, 0x10);

    /* Settle, then move the clock past a second boundary. */
    for (int t = 0; t < 6; t++)
        iap_periodic();
    rbstub_id3()->elapsed = 5000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    {
        const struct iaptest_pkt *p = pb_type(0x07);
        CHECK(p != NULL,
              "the elapsed second moved and no type 0x07 was sent");
        if (p && p->paylen >= 10)
            CHECK_EQ_INT((p->payload[6] << 24) | (p->payload[7] << 16)
                         | (p->payload[8] << 8) | p->payload[9], 5,
                         "the reported second");
    }

    /* Rebuild the queue. */
    rbstub_set_playlist(31, 3);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    {
        const struct iaptest_pkt *p = pb_type(0x0E);
        CHECK(p != NULL,
              "the playback engine contents changed and no type 0x0E "
              "was sent");
        if (p && p->paylen >= 10)
            CHECK_EQ_INT((p->payload[6] << 24) | (p->payload[7] << 16)
                         | (p->payload[8] << 8) | p->payload[9], 31,
                         "the reported track count");
    }

    /* Both change-detected: a tick with nothing new is silent. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    CHECK(pb_type(0x07) == NULL && pb_type(0x0E) == NULL,
          "a notification was resent with nothing changed");

    /* And an accessory that did not subscribe hears neither. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0xB1, 0x00, 0x00, 0x00, 0x0D);
    rbstub_id3()->elapsed = 9000;
    rbstub_set_playlist(44, 3);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    CHECK(pb_type(0x07) == NULL,
          "type 0x07 went to an accessory that did not subscribe to "
          "bit 04");
    CHECK(pb_type(0x0E) == NULL,
          "type 0x0E went to an accessory that did not subscribe to "
          "bit 12");
}

/* Restoring one setting must not touch the other.
 *
 * iap_arm_settings_restore() collapsed the two latches into one pending
 * bit and then cleared both, destroying the only record of which had
 * been armed -- and iap_restore_settings() put *both* back from
 * statics that read false and REPEAT_OFF when unarmed. So an accessory
 * asking for its shuffle change to be undone also reset the user's
 * repeat mode, and one asking for repeat back wiped shuffle.
 *
 * A single SetShuffle that changes nothing, with RestoreOnExit set, was
 * enough to destroy a user's Repeat All. MFi 5.1.42 (p.432) says what
 * the field governs: "A nonzero value restores the original shuffle
 * setting" -- the shuffle setting.
 *
 * Every existing restore case armed both or neither, and every one
 * started from shuffle off and repeat off, which is exactly the
 * zero-init of those statics: the contaminant equalled the truth. */
void test_session_restore_is_per_setting(void)
{
    /* Shuffle armed, repeat left alone and set by the user. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_ALL;

    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x30, 0x01, 0x01);
    CHECK(global_settings.playlist_shuffle == 1, "shuffle did not take");

    iap_reset_device(&device);
    iap_periodic();
    CHECK(global_settings.playlist_shuffle == 0,
          "shuffle was not restored");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "restoring shuffle reset the user's repeat mode, which "
                 "no accessory had touched");

    /* Repeat armed, shuffle left alone and set by the user. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    global_settings.playlist_shuffle = 1;
    global_settings.repeat_mode = REPEAT_OFF;

    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x31, 0x02, 0x01);
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "repeat did not take");

    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_OFF,
                 "repeat was not restored");
    CHECK(global_settings.playlist_shuffle == 1,
          "restoring repeat wiped the user's shuffle setting, which no "
          "accessory had touched");

    /* One armed, the other explicitly asked to persist. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;

    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x32, 0x01, 0x01);
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x33, 0x02, 0x00);

    iap_reset_device(&device);
    iap_periodic();
    CHECK(global_settings.playlist_shuffle == 0,
          "the armed shuffle was not restored");
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "a change the accessory asked to persist was undone");
}

/* A physical button does not disarm an accessory's Restore on Exit.
 *
 * The Simple Remote shuffle and repeat buttons went through the shared
 * helpers passing "no restore", which cleared a latch the accessory had
 * armed -- so a user pressing Shuffle mid-session disarmed the restore
 * and wrote the accessory's temporary value to config.cfg. A button
 * press is not an accessory request and says nothing about Restore on
 * Exit either way. */
void test_session_a_button_does_not_disarm_the_restore(void)
{
    /* IDPS, so the lingo 4 packet below carries a transaction ID and
     * its parameters land where L4_PARAM() looks for them. */
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    global_settings.playlist_shuffle = 0;
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x34, 0x01, 0x01);
    CHECK(global_settings.playlist_shuffle == 1, "shuffle did not take");
    rbstub_reset_calls();

    /* The user presses Shuffle: Table 4-14 (p.227) byte 1 bit 7.
     * Under IDPS the transaction ID sits between the command and the
     * state bytes, so this needs six bytes -- a four-byte one leaves
     * L2_PARAM(0) past the end and the press never reaches the
     * decoder, which is how the first version of this case passed
     * against the bug. */
    IAPTEST_RX(0x02, 0x00, 0x00, 0x35, 0x00, 0x80);
    iaptest_button_sample(4);
    CHECK_EQ_INT(rbstub_calls.settings_save, 0,
                 "a button press wrote the accessory's temporary "
                 "setting to config.cfg");

    iap_reset_device(&device);
    iap_periodic();
    CHECK(global_settings.playlist_shuffle == 0,
          "a button press disarmed the accessory's Restore on Exit, so "
          "the user's setting was never put back");
}

/* Two Display Remote notifications the sweep could not see.
 *
 * Table 4-59 (p.255) bit 15 is "Track time position in seconds" and bit
 * 12 the hold switch; Table 4-61 (p.258) gives them event types 0x0F
 * and 0x0C. Both are served, and neither had a case -- mutate.py's rule
 * for the subscription test was anchored on "if (device.notifications
 * & BIT_N(n))" and these two are written differently, one as an "&&"
 * continuation and one with a hex bit number, so the sweep reported
 * them covered when nothing touched them. */
static const struct iaptest_pkt *remote_event(unsigned char type)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == type)
            return p;
    }
    return NULL;
}

void test_session_seconds_and_hold_notifications(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_id3()->elapsed = 1000;
    rbstub_set_hold(false);

    /* Subscribe to bits 12 and 15 only. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x20, 0x00, 0x00, 0x90, 0x00);

    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* The elapsed second moves. */
    rbstub_id3()->elapsed = 7000;
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    {
        const struct iaptest_pkt *p = remote_event(0x0F);
        CHECK(p != NULL,
              "the elapsed second moved and no event 0x0F was sent");
        if (p && p->paylen >= 7)
            CHECK_EQ_INT((p->payload[5] << 8) | p->payload[6], 7,
                         "the reported second");
    }

    /* The hold switch moves. */
    rbstub_set_hold(true);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    {
        const struct iaptest_pkt *p = remote_event(0x0C);
        CHECK(p != NULL,
              "the hold switch moved and no event 0x0C was sent");
        if (p && p->paylen >= 6)
            CHECK_EQ_INT(p->payload[5], 0x01, "hold on");
    }

    /* Both change-detected. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    CHECK(remote_event(0x0F) == NULL && remote_event(0x0C) == NULL,
          "an event was resent with nothing changed");

    /* And an accessory subscribing to neither hears neither. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x03, 0x08, 0x00, 0x21, 0x00, 0x00, 0x00, 0x0A);
    rbstub_id3()->elapsed = 12000;
    rbstub_set_hold(false);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    CHECK(remote_event(0x0F) == NULL,
          "event 0x0F went to an accessory that did not subscribe");
    CHECK(remote_event(0x0C) == NULL,
          "event 0x0C went to an accessory that did not subscribe");
}

/* iap_reset_device() cannot lift a departing accessory's mute itself --
 * setvol() reaches the codec and the reset runs from a tick on the 6G --
 * so it sets device.unmute_pending and iap_periodic() spends it.
 *
 * The flag therefore outlives the accessory that earned it by however
 * long the iAP thread takes to be scheduled, and in that window a new
 * accessory can mute. It used to survive that: iap_periodic() then
 * called setvol() while device.mute stayed true, so the device reported
 * itself muted (MFi Table 4-52 p.259, iPod state info type 0x04) and
 * played at the user's level. */
void test_session_pending_unmute_does_not_lift_a_new_mute(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xB1, 0x04, 0x01, 0x80);   /* mute on */
    CHECK(device.mute, "the first accessory's mute was not recorded");

    /* It goes away. The unmute is owed but not yet paid: no
     * iap_periodic() runs between the detach and the next attach. */
    iap_reset_state(IF_IAP_MP(0));
    CHECK(device.unmute_pending,
          "a muted accessory detached without arming the unmute");

    /* A replacement arrives and mutes before the iAP thread is
     * scheduled. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xB2, 0x04, 0x01, 0x80);
    CHECK(device.mute, "the second accessory's mute was not recorded");

    rbstub_reset_calls();
    for (int t = 0; t < 4; t++)
        iap_periodic();

    CHECK(rbstub_calls.setvol == 0,
          "iap_periodic() called setvol() %d time(s) behind a live mute -- "
          "the codec came back up while the device still reported muted",
          rbstub_calls.setvol);
    CHECK(device.mute,
          "the second accessory's mute was dropped");

    /* And the ordinary case still works: detach a muted accessory with
     * nothing replacing it, and the level comes back. */
    iap_reset_state(IF_IAP_MP(0));
    rbstub_reset_calls();
    iap_periodic();
    CHECK(rbstub_calls.setvol >= 1,
          "a muted accessory detached and the level was never restored");
    CHECK(device.mute == false, "the mute outlived the accessory");
}

/* Table 4-63 (p.262) gives play status 0x03 Fast forward and 0x04
 * Rewind alongside 0x00 Stopped, 0x01 Playing and 0x02 Paused, and
 * iap_play_state_reported() answers all five. Both the change
 * detection for notification bit 3 and the poll's snapshot compared
 * audio_status() instead, which is AUDIO_STATUS_PLAY throughout a seek
 * -- so the state a subscribed accessory was told went 0x01 -> 0x03 ->
 * 0x01 and it heard about neither edge. */
void test_session_seek_is_a_play_status_change(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Subscribe to play status, bit 3. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x08);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* SetiPodStateInfo, info type 0x03 Play status, value 0x03 Fast
     * forward. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xA1, 0x03, 0x03);
    /* The seek holds a button for IAP_BTN_HELD; drain it, or every
     * packet below is deferred rather than handled. */
    iaptest_button_sample(4);

    /* The poll has to see it. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xA2);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetRemoteEventStatus");
        if (r && r->paylen >= 8) {
            uint32_t bits = ((uint32_t)r->payload[4] << 24)
                          | ((uint32_t)r->payload[5] << 16)
                          | ((uint32_t)r->payload[6] << 8)
                          |  (uint32_t)r->payload[7];
            CHECK(bits & (1u << 3),
                  "the poll missed the change into fast forward");
        }
    }

    /* And the notification has to carry the new state. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int told_ff = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 6 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x03
            && p->payload[5] == 0x03)
            told_ff = 1;
    }
    CHECK(told_ff,
          "no RemoteEventNotification said the player had gone into fast "
          "forward");

    /* Ending the seek is the other edge, and it was just as silent. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xA3, 0x03, 0x05);   /* end FF/REW */
    iaptest_button_sample(4);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int told_playing = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 6 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x03
            && p->payload[5] == 0x01)
            told_playing = 1;
    }
    CHECK(told_playing,
          "no RemoteEventNotification said the seek had ended");
}

/* Table 4-74 (pp.267-268) gives SetiPodStateInfo info types 0x04 and
 * 0x10 a bRestoreOnExit byte, on the same terms as 0x07 Shuffle and
 * 0x08 Repeat: Table 4-75 (p.270), "0x01 Save the original state and
 * restore it on exit."
 *
 * Neither read it. A dock that turned the volume up for its own
 * speakers and asked for the level back left the user's iPod loud --
 * the one restore-on-exit setting a user notices the moment they put
 * the headphones on. */
void test_session_volume_restore_on_exit(void)
{
    /* Type 0x04, three data bytes: not muted, UI volume 255,
     * bRestoreOnExit 1. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);

    IAPTEST_RX(0x03, 0x0E, 0x00, 0x90, 0x04, 0x00, 0xFF, 0x01);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "the accessory's volume change did not take, so the "
                 "restore below would prove nothing");

    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_QTR,
                 "the volume was not put back on detach although the "
                 "accessory asked for it");

    /* With the byte zero the change is the user's now and stays. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);

    IAPTEST_RX(0x03, 0x0E, 0x00, 0x91, 0x04, 0x00, 0xFF, 0x00);
    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "a volume change the accessory did not ask to have "
                 "undone was undone anyway");

    /* And with the byte absent -- the short form this device still
     * accepts -- likewise. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);

    IAPTEST_RX(0x03, 0x0E, 0x00, 0x92, 0x04, 0x00, 0xFF);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "the short form of info type 0x04 was refused");
    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "a volume change with no bRestoreOnExit byte was "
                 "undone as though the byte had been 1");

    /* Type 0x10, four data bytes: not muted, UI volume 255, absolute
     * volume 0, bRestoreOnExit 1. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);

    IAPTEST_RX(0x03, 0x0E, 0x00, 0x93, 0x10, 0x00, 0xFF, 0x00, 0x01);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "the type 0x10 volume change did not take");

    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_QTR,
                 "the type 0x10 volume was not put back on detach");

    /* One accessory's restore must not undo another's setting, the way
     * a single pending flag once let SetShuffle destroy Repeat All. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);
    global_settings.repeat_mode = REPEAT_ALL;

    IAPTEST_RX(0x03, 0x0E, 0x00, 0x94, 0x04, 0x00, 0xFF, 0x01);
    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(global_settings.repeat_mode, REPEAT_ALL,
                 "restoring the volume also reset the user's repeat "
                 "mode, which no accessory had touched");
}

/* Table 4-59 (p.256) bit 18 "Playback engine contents", event 0x12 in
 * Table 4-61 (p.261): four bytes, "number of tracks in new playlist".
 *
 * It was subscribed, acked Success and never sent. The accounting
 * comment in iap-lingo3.c called it one of six stub-backed bits and
 * named four; this is not a stub, its value is playlist_amount(), and
 * iap_periodic() was already tracking and sending exactly it for the
 * Extended Interface twin (Table 5-45 bit 12, type 0x0E).
 *
 * MFi 1.11.2.1.2 (p.59): "an accessory that displays a list of tracks
 * currently being played must respond to every 'Playback engine
 * contents changed' notification... Continuous polling, using
 * GetPlayStatus, is not an acceptable alternative." A head unit that
 * never enters Extended Interface mode kept a stale list for the whole
 * session. */
void test_session_playback_engine_contents_is_reported(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);

    /* Subscribe to bit 18 alone. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0xB8, 0x00, 0x04, 0x00, 0x00);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* The queue is rebuilt. */
    iaptest_tx_clear();
    rbstub_set_playlist(41, 0);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    bool told = false;
    uint32_t n = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 9 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x12) {
            told = true;
            n = ((uint32_t)p->payload[5] << 24)
              | ((uint32_t)p->payload[6] << 16)
              | ((uint32_t)p->payload[7] << 8)
              |  (uint32_t)p->payload[8];
        }
    }
    CHECK(told, "the queue was rebuilt and no event 0x12 was sent, so a "
                "head unit that draws a track list keeps a stale one");
    CHECK_EQ_INT((int)n, 41, "event 0x12 carried the wrong track count");

    /* No change, no repeat: this is a notification, not a heartbeat. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == 0x12)
            CHECK(false, "event 0x12 was repeated with the count "
                         "unchanged");
    }

    /* And the poll route reports it too -- MFi 4.3.13 (p.263) has
     * GetRemoteEventStatus answer with everything that changed. */
    rbstub_set_playlist(7, 0);
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0A, 0x00, 0xB9);
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no answer to GetRemoteEventStatus");
    if (r && r->paylen >= 8) {
        uint32_t bits = ((uint32_t)r->payload[4] << 24)
                      | ((uint32_t)r->payload[5] << 16)
                      | ((uint32_t)r->payload[6] << 8)
                      |  (uint32_t)r->payload[7];
        CHECK(bits & (1u << 18),
              "the poll missed the change of playback engine contents");
    }
}

/* The third family the playback-effect sweep exposed: iap-core.c's own
 * shared helpers. Shuffle and Repeat reorder or reload the queue,
 * Restore-on-Exit puts all three settings back, and the mute helper
 * reaches the codec. Every one of those calls could be deleted with
 * the suite green -- the cases around them asked what was acked and
 * what global_settings said, never what the playback engine was told. */
void test_session_setting_changes_reach_the_engine(void)
{
    /* Shuffle on while playing re-randomises the queue. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.playlist_shuffle = 0;
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x50, 0x01);
    CHECK_EQ_INT(rbstub_calls.randomise, 1,
                 "SetShuffle(on) while playing did not shuffle the queue");

    /* And off sorts it back. */
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x51, 0x00);
    CHECK_EQ_INT(rbstub_calls.sort, 1,
                 "SetShuffle(off) while playing did not sort the queue");

    /* Repeat One has to reach the buffer: apps/playback.c only honours
     * a repeat-mode change after a reload. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.repeat_mode = REPEAT_OFF;
    rbstub_reset_calls();
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x52, 0x01);
    CHECK_EQ_INT(rbstub_calls.reload, 1,
                 "SetRepeat did not flush and reload, so the change does "
                 "not take until the track ends");

    /* Restore on Exit has to reach the engine too, not just the
     * settings struct. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.playlist_shuffle = 0;
    global_settings.repeat_mode = REPEAT_OFF;
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x53, 0x01, 0x01);
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x54, 0x02, 0x01);
    rbstub_reset_calls();

    iap_reset_device(&device);
    iap_periodic();

    CHECK_EQ_INT(rbstub_calls.sort, 1,
                 "restoring shuffle off did not sort the queue back");
    CHECK_EQ_INT(rbstub_calls.reload, 1,
                 "restoring the repeat mode did not reload the buffer");

    /* The other direction: the user had shuffle on, the accessory
     * turned it off, and the restore has to re-randomise. */
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.playlist_shuffle = 1;
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x57, 0x00, 0x01);
    rbstub_reset_calls();

    iap_reset_device(&device);
    iap_periodic();
    CHECK_EQ_INT(rbstub_calls.randomise, 1,
                 "restoring shuffle on did not re-randomise the queue");

    /* And the volume restore reaches the codec through setvol(). */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x55, 0x04, 0x00, 0xFF, 0x01);
    rbstub_reset_calls();

    iap_reset_device(&device);
    iap_periodic();
    CHECK(rbstub_calls.setvol >= 1,
          "the volume was put back in global_status and never applied");

    /* A mute drives the codec down and keeps the user's level. */
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    rbstub_set_volume(IAP_TEST_VOLUME_MID);
    rbstub_reset_calls();
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x56, 0x04, 0x01, 0x00);
    CHECK_EQ_INT(rbstub_calls.sound_set_volume, 1,
                 "the mute was recorded and never applied to the codec");
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MID,
                 "the mute overwrote the level it has to come back to");
}

/* A detach owes a clear for every piece of session state. The
 * reset-path mutation rule reports 27 of 48 clears deletable with the
 * suite green -- including device.pb_seeking, radio_present and the
 * button latches, which mutate.py's own comments record as bugs found
 * in the field. The fixes had no test.
 *
 * Rather than 27 assertions, one comparison: run a rich session, detach,
 * and require every field of device to match a struct that has only
 * ever been through iap_reset_device(). A clear that stops happening
 * makes the two differ, whatever field it was. */
#if CONFIG_TUNER
extern int radio_present;
#endif

void test_session_detach_leaves_nothing_behind(void)
{
    struct device_t fresh;

    memset(&fresh, 0, sizeof(fresh));
    iap_reset_device(&fresh);

    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Touch as much of the session as one accessory can. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x70, 0x00, 0x00, 0x1F, 0xFF);
    IAPTEST_RX(0x03, 0x08, 0x00, 0x71, 0x00, 0x04, 0xFF, 0xFF);
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x72, 0x04, 0x01, 0x80);   /* mute */
    IAPTEST_RX(0x04, 0x00, 0x2E, 0x00, 0x73, 0x01, 0x01);   /* shuffle */
    IAPTEST_RX(0x04, 0x00, 0x31, 0x00, 0x74, 0x02, 0x01);   /* repeat  */
    IAPTEST_RX(0x04, 0x00, 0x29, 0x00, 0x75, 0x05);         /* seek    */
    iaptest_button_sample(2);
    IAPTEST_RX(0x00, 0x28, 0x00, 0x76, 0x00, 0x00, 0x00, 0x00, 0x0A);
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* It has to have taken, or the comparison proves nothing. */
    CHECK(device.notifications != 0 && device.pb_notifications != 0,
          "no subscription was recorded");
    CHECK(device.mute, "the mute was not recorded");
    CHECK(device.pb_seeking != 0, "the seek was not recorded");
    CHECK(device.lingoes != 0, "no lingo was negotiated");

    /* One accessory cannot reach every shadow in this struct -- some
     * belong to notifications this device never raises, and some only
     * move when a value the suite cannot drive moves. Stamp the rest,
     * because what is under test is the reset, not how a field came to
     * be set: a clear that stops happening has to show up whatever
     * wrote the field. Sentinels, so a missed clear is unmistakable.
     *
     * The three *_pending flags are stamped rather than exempted:
     * iap_reset_device() clears all three, and only unmute_pending and
     * poll_baseline_pending are work it raises. */
    device.audio_init_pending = true;
    device.audio_attrs_pending = true;
    device.tuner_caps_pending = true;
    device.power_reported = true;
    device.pb_track_changed = true;
    device.pb_trackpos_ms = 0x11223344;
    device.pb_chapter_index = 0x22334455;
    device.pb_chapterpos_ms = 0x33445566;
    device.pb_chapterpos_s = 0x44556677;
    device.chapter_index = 0x55667788;
    device.chapter_track_index = 0x66778899;
    device.chapter_count = 0x7788;
    device.volume = 0x5A;
    device.shuffle = 0x5B;
    device.repeat = 0x5C;
    device.equalizer_index = 0x99AABBCC;
    device.backlight = 0x5D;
    device.soundcheck = 0x5E;
    device.audiobook = 0x5F;
    device.hold = true;
    device.alarm_state = 0x60;
    device.alarm_hour = 0x61;
    device.alarm_minute = 0x62;
    device.idps_options = 0x63646566;
    device.idps_deviceid = 0x6768696A;

    /* And the globals the reset owns that the struct does not hold. */
    iap_repeatbtn = 3;
    iap_btnshuffle = true;
    iap_btnrepeat = true;
    iap_btnchapter = true;
#if CONFIG_TUNER
    radio_present = 1;
#endif

    iap_reset_state(IF_IAP_MP(0));

    /* Two fields iap_reset_device() deliberately leaves alone: what
     * matters is that the next reading is reported, and
     * power_reported says so on its own. */
    fresh.power_state = device.power_state;
    fresh.battery_level = device.battery_level;

    /* And the deferred-work flags, which the reset raises rather than
     * clears: setvol(), a poll baseline and GetTunerCaps all block or
     * take a mutex, and on the 6G the detach is noticed from a tick.
     * They are work owed, not state kept, so they are asserted for
     * their own value instead of against a fresh struct. */
    CHECK(device.unmute_pending,
          "a muted accessory went away without arming the unmute");
    CHECK(device.poll_baseline_pending,
          "the detach did not arm a fresh poll baseline");
    fresh.unmute_pending = device.unmute_pending;
    fresh.poll_baseline_pending = device.poll_baseline_pending;

    if (memcmp(&device, &fresh, sizeof(device)) != 0) {
        const unsigned char *a = (const unsigned char *)&device;
        const unsigned char *b = (const unsigned char *)&fresh;
        for (unsigned i = 0; i < sizeof(device); i++)
            if (a[i] != b[i]) {
                CHECK(false,
                      "device state survived the detach: byte %u of "
                      "struct device_t is 0x%02X, a fresh reset leaves "
                      "0x%02X (auth@%u accinfo@%u lingoes@%u notif@%u "
                      "chgnotif@%u do_notify@%u pbnotif@%u pwrrep@%u "
                      "pbposms@%u pbposs@%u pbnum@%u num@%u pbstat@%u "
                      "pbext@%u pbtrkchg@%u pbseek@%u dopwr@%u "
                      "capq@%u caps@%u)",
                      i, a[i], b[i],
                      (unsigned)offsetof(struct device_t, auth),
                      (unsigned)offsetof(struct device_t, accinfo),
                      (unsigned)offsetof(struct device_t, lingoes),
                      (unsigned)offsetof(struct device_t, notifications),
                      (unsigned)offsetof(struct device_t, changed_notifications),
                      (unsigned)offsetof(struct device_t, do_notify),
                      (unsigned)offsetof(struct device_t, pb_notifications),
                      (unsigned)offsetof(struct device_t, power_reported),
                      (unsigned)offsetof(struct device_t, pb_trackpos_ms),
                      (unsigned)offsetof(struct device_t, pb_trackpos_s),
                      (unsigned)offsetof(struct device_t, pb_numtracks),
                      (unsigned)offsetof(struct device_t, numtracks),
                      (unsigned)offsetof(struct device_t, pb_play_status),
                      (unsigned)offsetof(struct device_t, pb_ext_state),
                      (unsigned)offsetof(struct device_t, pb_track_changed),
                      (unsigned)offsetof(struct device_t, pb_seeking),
                      (unsigned)offsetof(struct device_t, do_power_notify),
                      (unsigned)offsetof(struct device_t, capabilities_queried),
                      (unsigned)offsetof(struct device_t, capabilities));
                break;
            }
    } else {
        iaptest_checked(1);
    }

    /* And the globals the reset owns, which are not in the struct. */
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a button the accessory was holding outlived it");
    CHECK_EQ_INT(iap_repeatbtn, 0, "the repeat counter outlived it");
    CHECK(!iap_btnshuffle && !iap_btnrepeat && !iap_btnstop
          && !iap_btnchapter,
          "a button latch outlived the accessory");
    CHECK_EQ_INT(interface_state, IST_STANDARD,
                 "Extended Interface mode outlived the accessory");
#if CONFIG_TUNER
    CHECK_EQ_INT(radio_present, 0,
                 "radio_present outlived the accessory that declared "
                 "the RF Tuner lingo");
#endif
}

#ifdef HAVE_LINE_REC
/* iap_record() flags an iPodModeChange (MFi C.5.4, p.536) and
 * iap_periodic() sends it, because the TX buffer belongs to the iAP
 * thread and audio_set_source() is called from three others. The flag
 * therefore outlives the accessory by however long the thread takes to
 * be scheduled -- and iap_reset_lingo1() clears it for exactly that
 * reason.
 *
 * Without the clear, a recording that stopped just before a detach is
 * announced to whatever plugs in next: a fresh microphone accessory is
 * told the mode changed on a session it was not present for, and C.5.4
 * has it stay out of low power mode until it hears the end of a
 * recording it never saw begin. */
void test_session_mode_change_does_not_outlive_the_accessory(void)
{
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();

    /* Recording starts, then stops -- both flagged, neither sent yet. */
    iap_record(true);
    iap_record(false);

    /* It goes away before the tick that would have sent it. */
    iap_reset_state(IF_IAP_MP(0));

    /* A replacement arrives, with the same lingo. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* Counted, not asserted inside the loop: with the clear in place
     * the firmware sends nothing, so a CHECK in there would never run
     * and the case would pass by not executing. */
    int stale = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01
            && p->payload[1] == 0x06)
            stale++;
    }
    CHECK_EQ_INT(stale, 0,
                 "the replacement accessory was sent an iPodModeChange "
                 "for a recording that ended before it arrived");
}
#endif

/* Accessory Power lingo 0x05, Table C-37 (p.548): 0x02 BeginHighPower
 * and 0x03 EndHighPower, both Origin: Apple device.
 *
 * C.8 (p.547): the lingo "is intended for use in conjunction with audio
 * playback from the Apple device. The accessory must remain in low
 * power mode ... until it receives a BeginHighPower command ... The
 * EndHighPower command notifies the accessory that it must stop drawing
 * high power and return to low power mode within 1 second."
 *
 * BeginHighPower went out at identification with no reference to
 * playback, and EndHighPower was never sent at all -- so an FM
 * transmitter powered its RF stage the moment it was recognised and
 * held it for the whole attach, out of a battery that is the point of
 * the device. Table C-38 (p.548) names the version this firmware
 * advertises, 1.01, for exactly this case: "BugFix: BeginTransmission
 * command sent after accessory inserted while the Apple device is
 * playing." */
static int power_cmds(unsigned char cmd)
{
    int n = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x05
            && p->payload[1] == cmd)
            n++;
    }
    return n;
}

void test_session_high_power_follows_playback(void)
{
    iaptest_init();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(0);                 /* stopped */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x05));
    iaptest_force_authenticated();

    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK_EQ_INT(power_cmds(0x02), 0,
                 "high power was granted to a stopped player");

    /* Playback starts. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK_EQ_INT(power_cmds(0x02), 1,
                 "playback started and the accessory was not told it "
                 "may draw high power");

    /* And it is not repeated every tick. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    CHECK_EQ_INT(power_cmds(0x02), 0,
                 "BeginHighPower was re-sent with nothing changed");

    /* Playback stops. */
    rbstub_set_audio_status(0);
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK_EQ_INT(power_cmds(0x03), 1,
                 "playback stopped and the accessory was never told to "
                 "return to low power");

    /* An accessory that did not negotiate the lingo hears none of it. */
    iaptest_init();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK_EQ_INT(power_cmds(0x02) + power_cmds(0x03), 0,
                 "an accessory without the Accessory Power lingo was "
                 "sent one of its commands");

    iaptest_init();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    static const unsigned char lingoes[] = { 0x00, 0x05 };
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    iapacc_autorespond(true);
    iaptest_tx_clear();
    iap_periodic();
    CHECK_EQ_INT(power_cmds(0x02), 1,
                 "IDPS did not arm high power for the transmitter lingo");

    iaptest_tx_clear();
    for (int rounds = 0; rounds < 8 && device.auth.state != AUST_AUTH;
         rounds++) {
        iapacc_pump();
        iap_periodic();
    }
    CHECK_EQ_INT(device.auth.state, AUST_AUTH,
                 "the transmitter did not finish authentication");
    CHECK_EQ_INT(power_cmds(0x02), 1,
                 "high power was not reissued after authentication");
}

/* A reorder is a contents change even though the count did not move.
 *
 * Both events -- Table 5-45 bit 12 / type 0x0E and Table 4-59 bit 18 /
 * event 0x12 -- compared playlist_amount() and nothing else, and
 * apps/playlist.c permutes indices[] in place. MFi 5.1.42 (p.432):
 * "Shuffling tracks does not affect the track index, just the track at
 * that index"; 1.11.2.1.2 (p.59) has an accessory that draws a list
 * respond "to every 'Playback engine contents changed' notification",
 * with polling ruled out.
 *
 * The case that matters most is not an accessory command at all: a user
 * toggling Shuffle in Rockbox's own menu reorders the queue with the
 * iAP layer uninvolved, and from that moment every row of the head
 * unit's cached list names the wrong track. */
void test_session_a_reorder_is_a_contents_change(void)
{
    iaptest_init();
    iaptest_session_extended();
    rbstub_set_playlist(40, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    global_settings.playlist_shuffle = 0;

    /* Subscribe to both: Extended Interface bit 12 and Display Remote
     * bit 18. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x30, 0x00, 0x00, 0x10, 0x00);
    IAPTEST_RX(0x03, 0x08, 0x00, 0x31, 0x00, 0x04, 0x00, 0x00);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    /* The user shuffles from Rockbox's own menu. The track count does
     * not change and the playing track does not change. */
    iaptest_tx_clear();
    global_settings.playlist_shuffle = 1;
    playlist_randomise(NULL, 0x5EED, true);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    bool ei = false, dr = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 6)
            continue;
        if (p->payload[0] == 0x04 && p->payload[1] == 0x00
            && p->payload[2] == 0x27 && p->payload[5] == 0x0E)
            ei = true;
        if (p->payload[0] == 0x03 && p->payload[1] == 0x09
            && p->payload[4] == 0x12)
            dr = true;
    }
    CHECK(ei, "the queue was reordered and no Extended Interface "
              "contents-changed event was sent; the head unit's list "
              "still names the old order");
    CHECK(dr, "the queue was reordered and no Display Remote event 0x12 "
              "was sent");

    /* And it is not repeated once the order settles. */
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();
    int repeats = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[2] == 0x27 && p->payload[5] == 0x0E)
            repeats++;
    }
    CHECK_EQ_INT(repeats, 0,
                 "the contents-changed event repeated with the order "
                 "unchanged");
}
