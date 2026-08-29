/***************************************************************************
 * Volume reporting.
 *
 * MFi R46 Table 4-61, p.258, event 0x04 "Mute/UI Volume":
 *   Byte 0: Mute State
 *   Byte 1: UI Volume Level, "a value between 0 and 255, with 0 indicating
 *           minimum volume and 255 indicating maximum volume."
 *
 * The scale therefore has to span the device's whole volume range. On
 * ipod6g that range comes from the CS42L55 and is -60..+12 dB
 * (firmware/export/cs42l55.h:29), not the -90..+6 dB of the iPod Video's
 * WM8758 (firmware/export/wm8758.h:29).
 *
 * Table 4-61 on p.261 adds that the UI volume is "normalized to volume
 * limit settings" while the absolute volume is "not normalized". Rockbox
 * defaults global_settings.volume_limit to sound_max, which makes the two
 * scales identical; the cases below that lower it check they diverge
 * correctly.
 ****************************************************************************/

#include "iap_test.h"

#include "config.h"
#include "sound.h"
#include "settings.h"
#include "iap-core.h"

/* The periodic handler carries the notification logic (apps/iap/iap-core.c
 * :909 onwards); driving it directly stands in for the 100 ms tick. */
extern void iap_periodic(void);

/* Ask the device for its current volume the way an accessory does:
 * GetiPodStateInfo (0x03/0x0C) with infoType 0x04. Returns the UI volume
 * byte from the reply, or -1 if no usable reply came back. */
static int query_volume_byte(void)
{
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0x01, 0x04);

    const struct iaptest_pkt *p = iaptest_tx(0);
    if (!p || !p->checksum_ok)
        return -1;
    /* 0x03 0x0D <tid_hi> <tid_lo> <infoType> <mute> <volume> */
    if (p->paylen != 7)
        return -1;
    if (p->payload[0] != 0x03 || p->payload[1] != 0x0D)
        return -1;
    if (p->payload[4] != 0x04)
        return -1;
    return p->payload[6];
}

void test_volume_range_matches_the_codec(void)
{
    /* If the codec header changes, the expectations below must change
     * with it. Fail loudly rather than silently testing the wrong range. */
    CHECK_EQ_INT(sound_min(SOUND_VOLUME), IAP_TEST_VOLUME_MIN,
                 "sound_min(SOUND_VOLUME) for " IAP_TEST_TARGET_NAME);
    CHECK_EQ_INT(sound_max(SOUND_VOLUME), IAP_TEST_VOLUME_MAX,
                 "sound_max(SOUND_VOLUME) for " IAP_TEST_TARGET_NAME);
}

void test_volume_scale_spans_full_byte(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    rbstub_set_volume(IAP_TEST_VOLUME_MIN);
    int at_min = query_volume_byte();
    CHECK(at_min >= 0, "no usable GetiPodStateInfo(0x04) reply at min volume");

    rbstub_set_volume(IAP_TEST_VOLUME_MAX);
    int at_max = query_volume_byte();
    CHECK(at_max >= 0, "no usable GetiPodStateInfo(0x04) reply at max volume");

    if (at_min < 0 || at_max < 0)
        return;

    CHECK_EQ_INT(at_min, 0,
                 "minimum volume must report 0 (MFi Table 4-61 event 0x04)");
    CHECK_EQ_INT(at_max, 255,
                 "maximum volume must report 255 (MFi Table 4-61 event 0x04)");
}

/* Monotonicity alone is satisfied by a constant, and the endpoints have
 * early returns, so pinning them proved nothing about the interior. This
 * checks the value at every dB step against the scale the spec asks for:
 * 0 at the codec minimum, 255 at its maximum, linear between, rounded to
 * nearest. */
void test_volume_scale_maps_every_step(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    const int lo = sound_min(SOUND_VOLUME);
    const int hi = sound_max(SOUND_VOLUME);
    const int span = hi - lo;
    int walked = 0;

    for (int vol = lo; vol <= hi; vol++) {
        rbstub_set_volume(vol);
        int got = query_volume_byte();
        int want = ((vol - lo) * 255 + span / 2) / span;

        if (got != want) {
            CHECK(false, "%d dB reported %d, expected %d "
                  "(0 at %d dB, 255 at %d dB, linear between)",
                  vol, got, want, lo, hi);
            return;
        }
        walked++;
    }

    /* Asserted positively, not only on the failure branch. Checking
     * nothing on the way through made this case report ok whether it
     * walked the scale or returned on the first step, and it counted
     * zero checks either way. */
    CHECK(walked == span + 1,
          "walked %d of the %d steps between %d and %d dB",
          walked, span + 1, lo, hi);
}

void test_volume_scale_is_monotonic_and_never_wraps(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    int prev = -1, walked = 0;
    for (int vol = IAP_TEST_VOLUME_MIN; vol <= IAP_TEST_VOLUME_MAX; vol++) {
        rbstub_set_volume(vol);
        int b = query_volume_byte();
        if (b < 0) {
            CHECK(false, "no usable reply at %d dB", vol);
            return;
        }
        if (b < prev) {
            CHECK(false,
                  "reported volume went backwards at %d dB: %d after %d. "
                  "A byte-wrapping scale makes loud settings read as near "
                  "silence on the accessory", vol, b, prev);
            return;
        }
        prev = b;
        walked++;
    }

    /* Positively, for the same reason as the case above: a run that
     * returned on the first step reported ok and counted no checks. */
    CHECK(walked == IAP_TEST_VOLUME_MAX - IAP_TEST_VOLUME_MIN + 1,
          "walked %d of the %d steps", walked,
          IAP_TEST_VOLUME_MAX - IAP_TEST_VOLUME_MIN + 1);
    CHECK(prev == 255,
          "the top of the scale reported %d, not 255", prev);
}

/* ------------------------------------------------------------------ */
/* Volume changes must reach the accessory while it stays connected     */
/* ------------------------------------------------------------------ */

void test_volume_change_notifies_accessory(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* SetRemoteEventNotification (0x03/0x08) enabling bit 4,
     * Mute/UI Volume (MFi Table 4-59, p.255). */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10);
    iaptest_tx_clear();

    /* The user turns the wheel. */
    rbstub_set_volume(-10);
    iap_periodic();

    bool saw_volume_notification = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 3 && p->payload[0] == 0x03 && p->payload[1] == 0x09)
            saw_volume_notification = true;
    }

    CHECK(saw_volume_notification,
          "no RemoteEventNotification (0x03/0x09) after a volume change; "
          "the accessory keeps playing at the level it saw at connect time");
}

/* ------------------------------------------------------------------ */
/* Capability advertisement                                            */
/* ------------------------------------------------------------------ */

/* An accessory checks RetiPodOptionsForLingo before it decides whether
 * to enable volume notifications. MFi Table 3-132, p.194: for Display
 * Remote, bit 00 is "UI Volume control". The worked example at step 8,
 * p.197, has a real device answer 0000000000000001 for that lingo. */
void test_volume_capability_is_advertised(void)
{
    iaptest_enter_idps();

    /* GetiPodOptionsForLingo (0x00/0x4B) for Display Remote. */
    IAPTEST_RX(0x00, 0x4B, 0x00, 0x21, 0x03);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x00, 0x21, 0x03,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x01);
}

/* Table 3-132 (p.194) gives Simple Remote bit 00 "Context-specific
 * controls" and bit 01 "Audio media controls". iap-lingo2.c implements
 * both -- ContextButtonStatus and AudioButtonStatus -- so both bits
 * have to be set.
 *
 * MFi 2.3.3 (p.97) is why: the accessory reads this "so that the
 * accessory does not try to declare and use features that the device
 * cannot handle". Answering zero told it the transport buttons were
 * unsupported, and a conformant accessory then never sent either
 * command. */
void test_simple_remote_capabilities_are_advertised(void)
{
    iaptest_enter_idps();

    IAPTEST_RX(0x00, 0x4B, 0x00, 0x24, 0x02);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x00, 0x24, 0x02,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x03);
}

/* Absolute Volume control is Display Remote bit 01 in the same table,
 * and it stays clear. Two of its three legs exist -- SetiPodStateInfo
 * has a 0x10 arm and iap_periodic() sends the 0x10 event -- but
 * GetiPodStateInfo answers information types 0x00 through 0x0E and has
 * no 0x10 arm, so an accessory that read the bit and asked would get
 * Bad Parameter. Claiming it would be the advertisement 2.3.3 exists to
 * prevent. */
void test_absolute_volume_capability_is_not_claimed(void)
{
    iaptest_enter_idps();

    /* Display Remote options: bit 00 set, bit 01 clear. */
    IAPTEST_RX(0x00, 0x4B, 0x00, 0x25, 0x03);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetiPodOptionsForLingo(0x03)");
        if (r && r->paylen >= 13)
            CHECK_EQ_INT(r->payload[12] & 0x02, 0,
                         "Absolute Volume control is advertised, but "
                         "GetiPodStateInfo has no 0x10 arm to answer it");
    }

    /* And the ask really is refused, which is what makes the bit a lie
     * if it were set. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0x26, 0x10);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to GetiPodStateInfo(0x10)");
        if (r && r->paylen >= 5 && r->payload[0] == 0x03
            && r->payload[1] == 0x00)
            CHECK(r->payload[4] != 0x00,
                  "GetiPodStateInfo(0x10) succeeded after all -- if it "
                  "answers, Table 3-132 bit 01 should be set");
    }
}

/* Lingoes we claim nothing for must still answer, with zero. */
void test_options_for_other_lingoes_stay_zero(void)
{
    iaptest_enter_idps();

    IAPTEST_RX(0x00, 0x4B, 0x00, 0x22, 0x04);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x00, 0x22, 0x04,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00);
}

/* Table 3-132 (p.192) gives the General lingo bit 00 as "Line out
 * usage". We answer GetiPodPreferences class 0x03 with line out enabled,
 * and Table 3-56's note (p.152) says that class "is available only if
 * the Apple device supports line-out usage", so the bit has to be set or
 * the two answers contradict each other. */
void test_general_lingo_advertises_line_out(void)
{
    iaptest_enter_idps();

    IAPTEST_RX(0x00, 0x4B, 0x00, 0x23, 0x00);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x00, 0x23, 0x00,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x01);
}

/* ------------------------------------------------------------------ */
/* Line-out preference                                                 */
/* ------------------------------------------------------------------ */

/* MFi note to Table 4-59, p.256: before an accessory that identified
 * through IDPS may enable volume notifications it must "register a
 * SetiPodPreferenceToken of Class 0x03 and value 0x01 to enable line
 * out". Both preference commands must therefore read the class from
 * past the transaction ID. */
void test_lineout_preference_query_under_idps(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* GetiPodPreferences (0x00/0x29), transaction 0x0031, class 0x03. */
    IAPTEST_RX(0x00, 0x29, 0x00, 0x31, 0x03);

    /* RetiPodPreferences: class 0x03, line-out enabled. */
    EXPECT_PAYLOAD(0, 0x00, 0x2A, 0x00, 0x31, 0x03, 0x01);
}

void test_lineout_preference_set_under_idps(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* SetiPodPreferences (0x00/0x2B), transaction 0x0032,
     * class 0x03, value 0x01 (enable line out), restore-on-exit 0x00. */
    IAPTEST_RX(0x00, 0x2B, 0x00, 0x32, 0x03, 0x01, 0x00);

    /* iPodAck: status OK for the command we just handled. */
    EXPECT_PAYLOAD(0, 0x00, 0x02, 0x00, 0x32, 0x00, 0x2B);
}

/* Disabling line out must be refused, and the refusal must name the
 * right command. Reading the class two bytes early turned this into an
 * unconditional accept. */
void test_lineout_disable_is_refused(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    IAPTEST_RX(0x00, 0x2B, 0x00, 0x33, 0x03, 0x00, 0x00);

    EXPECT_PAYLOAD(0, 0x00, 0x02, 0x00, 0x33, 0x02, 0x2B);
}

/* A digital-audio accessory must still be told the real volume. The old
 * behaviour reported a hardcoded maximum while USB audio streamed,
 * which left an accessory with no volume control of its own stuck at
 * full scale. */
void test_volume_is_real_while_usb_audio_streams(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_usb_audio_active(true);

    rbstub_set_volume(IAP_TEST_VOLUME_MIN);
    CHECK_EQ_INT(query_volume_byte(), 0,
                 "UI volume at minimum while USB audio is active");

    rbstub_set_volume(-24);
    int mid = query_volume_byte();
    CHECK(mid > 0 && mid < 255,
          "UI volume mid-scale while USB audio is active: got %d", mid);

    rbstub_set_usb_audio_active(false);
}

/* A legacy accessory sends GetiPodOptionsForLingo with no transaction
 * ID at all: MFi Table 3-130 (p.191) gives the payload as one LingoID
 * byte. Rejecting it costs the accessory the capability answer, and
 * 3.3.55 has it read a nonzero ack as "that lingo is not supported by
 * the Apple device on the port being used". */
void test_options_for_lingo_answers_a_legacy_accessory(void)
{
    iaptest_identify_legacy(0x0000001D);

    IAPTEST_RX(0x00, 0x4B, 0x03);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x03,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x01);
}

/* ------------------------------------------------------------------ */
/* Notification cadence                                                */
/* ------------------------------------------------------------------ */

/* Count RemoteEventNotification packets carrying a volume event. */
static int count_volume_notifications(void)
{
    int n = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        int off = (p->paylen >= 4 && p->payload[0] == 0x03
                   && p->payload[1] == 0x09) ? 4 : -1;
        if (off < 0 || p->paylen <= off)
            continue;
        if (p->payload[off] == 0x04 || p->payload[off] == 0x10)
            n++;
    }
    return n;
}

static void enable_volume_notifications(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    /* SetRemoteEventNotification enabling bit 4, Mute/UI Volume. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10);
    iaptest_tx_clear();
}

/* An accessory that has just enabled notifications does not know the
 * current level, so the first tick must tell it. */
void test_volume_reported_once_on_enable(void)
{
    enable_volume_notifications();

    iap_periodic();
    CHECK_EQ_INT(count_volume_notifications(), 1,
                 "volume notifications after the first tick");
}

/* MFi 4.3.12 (p.256): the notification is sent "whenever an enabled
 * event change has occurred". An unchanged level must not be resent, or
 * the accessory receives ten identical packets a second forever. */
void test_volume_not_resent_when_unchanged(void)
{
    enable_volume_notifications();

    iap_periodic();             /* the initial report */
    iaptest_tx_clear();

    for (int i = 0; i < 20; i++)
        iap_periodic();

    CHECK_EQ_INT(count_volume_notifications(), 0,
                 "volume notifications across 20 idle ticks");
}

void test_volume_resent_when_it_changes(void)
{
    enable_volume_notifications();

    iap_periodic();
    iaptest_tx_clear();

    rbstub_set_volume(-10);
    iap_periodic();
    CHECK_EQ_INT(count_volume_notifications(), 1,
                 "volume notifications after a change");

    iaptest_tx_clear();
    iap_periodic();
    CHECK_EQ_INT(count_volume_notifications(), 0,
                 "volume notifications on the tick after the change");
}

/* MFi p.95 lists GetiPodOptionsForLingo as one of only four General
 * lingo commands the device accepts DURING the IDPS process, and the
 * IMPORTANT note on the same page makes transaction IDs mandatory from
 * StartIDPS onward. So in that window the command carries one, and the
 * lingo byte sits two bytes later than in a legacy packet. Section
 * 2.3.3 (p.97) makes this the normal discovery path: "This is done by
 * calling GetiPodOptionsForLingo after StartIDPS." */
void test_options_for_lingo_inside_the_idps_window(void)
{
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);     /* StartIDPS, nothing after it */
    iaptest_tx_clear();

    /* GetiPodOptionsForLingo, transaction 0x0002, Display Remote. */
    IAPTEST_RX(0x00, 0x4B, 0x00, 0x02, 0x03);

    EXPECT_PAYLOAD(0, 0x00, 0x4C, 0x00, 0x02, 0x03,
                   0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x01);
}

/* MFi Table 4-61 (p.261) splits the two volume bytes of event 0x10: byte 1
 * is the UI volume, "normalized to volume limit settings", byte 2 is the
 * absolute volume, "not normalized". The paragraph on the same page makes
 * the consequence explicit: "setting the UI volume to 255 will result in
 * the Absolute volume being set to the Apple device's Volume Limit
 * setting."
 *
 * Rockbox's volume limit defaults to sound_max, where the two scales
 * coincide and nothing distinguishes them. Lower it and they must part. */
void test_volume_ui_scale_follows_volume_limit(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Halfway up whichever dB range this target has. */
    const int limit = IAP_TEST_VOLUME_MID;
    global_settings.volume_limit = limit;

    /* At the limit itself the UI volume is maximum, by the sentence
     * quoted above, while the absolute volume is only partway up. */
    rbstub_set_volume(limit);
    CHECK_EQ_INT(iap_volume_to_ui_byte(limit), 255,
                 "UI volume at the limit");
    CHECK(iap_volume_to_byte(limit) < 200,
          "absolute volume at the limit should be well short of 255, got %d",
          iap_volume_to_byte(limit));

    /* And the absolute byte is still the whole-range scale: a limit
     * halfway up the range is half of 255 in absolute terms. */
    CHECK_EQ_INT(iap_volume_to_byte(limit), 128,
                 "absolute volume at the midpoint of the dB range");

    /* Below the limit the UI scale is the steeper of the two: a quarter
     * of the way up the range is halfway to a midpoint limit. */
    rbstub_set_volume(IAP_TEST_VOLUME_QTR);
    CHECK_EQ_INT(iap_volume_to_ui_byte(IAP_TEST_VOLUME_QTR), 128,
                 "UI volume halfway to a midpoint limit");
    CHECK_EQ_INT(iap_volume_to_byte(IAP_TEST_VOLUME_QTR), 64,
                 "absolute volume a quarter of the way up the range");

    /* Now check the wire, which is what an accessory actually sees.
     * GetiPodStateInfo (lingo 3 / 0x0C) info type 0x10. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0x10, 0x10);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to GetiPodStateInfo 0x10");
    if (p && p->paylen >= 8) {
        CHECK_EQ_INT(p->payload[4], 0x10, "info type echoed");
        CHECK_EQ_INT(p->payload[5], 0x00, "not muted");
        CHECK_EQ_INT(p->payload[6], 128, "wire UI volume byte");
        CHECK_EQ_INT(p->payload[7], 64,  "wire absolute volume byte");
        CHECK(p->payload[6] != p->payload[7],
              "the two volume bytes must differ once a limit is set, "
              "got %d and %d", p->payload[6], p->payload[7]);
    } else {
        CHECK(false, "reply too short: %d bytes", p ? p->paylen : -1);
    }

    /* An accessory setting the UI volume aims at the limit, not at
     * sound_max: SetiPodStateInfo carries a UI volume, so 255 must land
     * on the limit rather than overshoot it. Otherwise the accessory
     * cannot reach its own maximum. */
    CHECK_EQ_INT(iap_byte_to_volume(255), limit,
                 "UI byte 255 maps back to the volume limit");
    CHECK_EQ_INT(iap_byte_to_volume(0), IAP_TEST_VOLUME_MIN,
                 "UI byte 0 maps back to minimum volume");
}

/* A limit at or below the minimum must not divide by zero or wrap. */
void test_volume_ui_scale_degenerate_limit(void)
{
    iaptest_enter_idps();

    global_settings.volume_limit = IAP_TEST_VOLUME_MIN;
    CHECK_EQ_INT(iap_volume_to_ui_byte(IAP_TEST_VOLUME_MIN), 0,
                 "UI volume with the limit at minimum");
    CHECK_EQ_INT(iap_volume_to_ui_byte(0), 0,
                 "UI volume above a minimum limit");
    CHECK_EQ_INT(iap_byte_to_volume(255), IAP_TEST_VOLUME_MIN,
                 "inverse with the limit at minimum");

    /* A limit stored above sound_max -- a stale config file from a
     * device with a wider range -- must clamp, not overflow. */
    global_settings.volume_limit = 1000;
    CHECK_EQ_INT(iap_volume_to_ui_byte(IAP_TEST_VOLUME_MAX), 255,
                 "UI volume at sound_max with an over-range limit");
    CHECK_EQ_INT(iap_byte_to_volume(255), IAP_TEST_VOLUME_MAX,
                 "inverse clamps to sound_max");
}

/* An accessory setting the volume has to reach the codec, not just the
 * settings struct.
 *
 * global_status.volume is where the level is stored; sound_set_volume()
 * is what tells the CS42L55. setvol() (apps/misc.c:871) is the only
 * thing that bridges the two, and it is what every other volume control
 * in Rockbox goes through -- the wheel (apps/gui/wps.c:888), the sound
 * menu (apps/menus/sound_menu.c:82), the radio screen, the touch skin.
 *
 * This is the bug a tester reported on an iPod 7.5 Gen with a Kokkia
 * Bluetooth transmitter and AirPods Pro 2: transport controls from the
 * headphones worked, volume did not, and the workaround was to unplug
 * the transmitter, set the volume on the iPod itself, and plug it back
 * in -- which works precisely because the iPod's own control does call
 * setvol(). */
void test_volume_accessory_change_reaches_the_codec(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    rbstub_set_volume(-40);
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(-40),
                 "the harness's own volume setter should reach the codec");

    rbstub_calls.setvol = 0;
    rbstub_calls.codec_volume = -9999;

    /* SetiPodStateInfo (lingo 3 / 0x0E), info type 0x04 Mute/UI Volume.
     * MFi Table 4-74 (p.267) gives it three data bytes: mute state, UI
     * volume, and bRestoreOnExit. Not muted, UI volume 255, which with
     * the default limit is the top of the range. */
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x30, 0x04, 0x00, 0xFF, 0x00);

    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "the stored volume after the accessory set UI 255");
    CHECK(rbstub_calls.setvol > 0,
          "the accessory set the volume and nothing applied it: "
          "global_status.volume changed but setvol() was never called, "
          "so the codec is still at the old level");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MAX),
                 "the level that reached the codec");

    /* And downwards, so this is not passing on a coincidence. */
    rbstub_calls.setvol = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x31, 0x04, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MIN,
                 "the stored volume after the accessory set UI 0");
    CHECK(rbstub_calls.setvol > 0, "setvol() on the way down");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MIN),
                 "the level that reached the codec on the way down");

    /* Info type 0x10 carries a UI volume in the same position and must
     * behave identically. Table 4-74 (p.269) gives it four data bytes:
     * mute, UI volume, absolute volume, bRestoreOnExit. */
    rbstub_calls.setvol = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x32, 0x10, 0x00, 0x80, 0x80, 0x00);
    CHECK(rbstub_calls.setvol > 0,
          "info type 0x10 stored the volume without applying it");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(global_status.volume),
                 "the codec and the stored level agree after type 0x10");
}

/* MFi Table 4-74 (p.269), SetiPodStateInfo info type 0x10, byte 1:
 *
 *   "UI volume level (bits 7:0). A value between 0 and 255, normalized
 *    to UI volume limit settings. 0 indicates minimum UI volume and 255
 *    indicates maximum UI volume. If the accessory sets this byte to 0,
 *    the Apple device uses the Absolute volume setting."
 *
 * So a zero UI byte is not a request for silence, it is a request to
 * read byte 2. An accessory driving the absolute scale leaves byte 1 at
 * zero exactly as instructed, and taking that literally muted the iPod
 * on every volume command it sent. */
void test_volume_zero_ui_byte_falls_back_to_absolute(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_volume(-30);

    /* Byte 1 zero, byte 2 at full scale: the result is maximum volume,
     * not silence. */
    rbstub_calls.setvol = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x40, 0x10, 0x00, 0x00, 0xFF, 0x00);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MAX,
                 "UI byte 0 with absolute byte 255 should be maximum "
                 "volume, not minimum");
    CHECK(rbstub_calls.setvol > 0, "and it must reach the codec");

    /* Halfway on the absolute scale is halfway up the whole dB range,
     * which is not the same point the UI scale would give. */
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x41, 0x10, 0x00, 0x00, 0x80, 0x00);
    CHECK_EQ_INT(global_status.volume, iap_byte_to_abs_volume(0x80),
                 "UI byte 0 with absolute byte 128");

    /* The fallback must read byte 2 on the ABSOLUTE scale, which
     * Table 4-61 (p.261) says is "not normalized" -- unlike byte 1. The
     * two scales only differ once a volume limit is set, so set one:
     * with the limit halfway up the range, an absolute 255 is still the
     * top of the range, where the UI scale would give the limit. Without
     * this the wrong scale passes unnoticed. */
    global_settings.volume_limit = IAP_TEST_VOLUME_MID;

    /* A quarter of the way up: the absolute scale puts that at a quarter
     * of the dB range, the UI scale at an eighth, and setvol()'s clamp
     * to the limit touches neither -- so the two are distinguishable.
     * Points at or above the limit are not: setvol() clamps them
     * together and sound_set_volume() writes the clamped value back to
     * global_status.volume (firmware/sound.c:320). */
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x45, 0x10, 0x00, 0x00, 0x40, 0x00);
    CHECK_EQ_INT(global_status.volume, iap_byte_to_abs_volume(0x40),
                 "the absolute byte is not normalized to the volume "
                 "limit, so it spans the whole range");
    CHECK(iap_byte_to_abs_volume(0x40) != iap_byte_to_volume(0x40),
          "the two scales coincide at this test point, so it cannot "
          "tell them apart -- pick another");
    CHECK(global_status.volume != iap_byte_to_volume(0x40),
          "the fallback used the UI scale for the absolute byte");
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;

    /* Both bytes zero is genuinely minimum: there is nothing else to
     * fall back to. */
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x42, 0x10, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MIN,
                 "both volume bytes zero is minimum volume");

    /* A non-zero UI byte still wins, and still uses the UI scale. Set a
     * limit so the two scales disagree and the choice is observable. */
    global_settings.volume_limit = IAP_TEST_VOLUME_MID;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x43, 0x10, 0x00, 0xFF, 0x40, 0x00);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MID,
                 "UI byte 255 maps to the volume limit, ignoring the "
                 "absolute byte beside it");

    /* And info type 0x04, which has no absolute byte, must NOT acquire
     * the fallback: there byte 1 zero really is minimum volume.
     *
     * The byte after the UI volume is bRestoreOnExit (Table 4-74,
     * p.267), and it is deliberately 0xFF here. A fallback wrongly
     * added to this case would read it as an absolute volume and give
     * maximum instead of minimum -- which is exactly the mistake made
     * while writing this, caught only because of this value. With a
     * zero there the wrong code and the right code agree. */
    global_settings.volume_limit = IAP_TEST_VOLUME_MAX;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x44, 0x04, 0x00, 0x00, 0xFF);
    CHECK_EQ_INT(global_status.volume, IAP_TEST_VOLUME_MIN,
                 "info type 0x04 has no absolute byte, so a zero UI "
                 "volume is minimum volume and the byte after it is "
                 "bRestoreOnExit, not a volume");
}

/* Muting was recorded in device.mute, reported back in every volume
 * notification, and never applied. An accessory that muted the iPod was
 * acknowledged, saw "muted" in the next status it asked for, and heard
 * the music carry on at the same level.
 *
 * MFi Table 4-74 (p.267): "A value of 0x00 turns mute off; a value of
 * 0x01 turns on mute." */
void test_volume_mute_reaches_the_codec(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    rbstub_set_volume(-20);
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(-20),
                 "starting level");

    /* Mute, via info type 0x04. */
    rbstub_calls.sound_set_volume = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x50, 0x04, 0x01, 0x00, 0x00);

    CHECK(device.mute, "the mute flag was not set");
    CHECK(rbstub_calls.sound_set_volume > 0,
          "mute was recorded but nothing reached the codec, so the "
          "music kept playing at the same level");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MIN),
                 "the level the codec was left at while muted");

    /* The stored level must survive, so unmuting has something to come
     * back to and a reboot does not find the user at minimum. */
    CHECK_EQ_INT(global_status.volume, -20,
                 "muting overwrote the stored volume, which would "
                 "persist as a silent mute across a reboot");

    /* Unmute restores it. */
    rbstub_calls.sound_set_volume = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x51, 0x04, 0x00, 0xFF, 0x00);
    CHECK(!device.mute, "the mute flag was not cleared");
    CHECK(rbstub_calls.sound_set_volume > 0, "unmute reached the codec");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MAX),
                 "unmute applied the new level");

    /* Info type 0x10 mutes the same way. */
    rbstub_set_volume(-20);
    rbstub_calls.sound_set_volume = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x52, 0x10, 0x01, 0x00, 0x00, 0x00);
    CHECK(device.mute, "info type 0x10 did not set the mute flag");
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MIN),
                 "info type 0x10 mute reached the codec");
    CHECK_EQ_INT(global_status.volume, -20,
                 "info type 0x10 mute overwrote the stored volume");

    /* A second mute must not re-apply: the level is already down, and
     * re-applying is how a remembered level gets lost. */
    rbstub_calls.sound_set_volume = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0x53, 0x10, 0x01, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(rbstub_calls.sound_set_volume, 0,
                 "muting an already-muted device touched the codec again");

    /* And what the accessory is told matches what it hears: MFi Table
     * 4-61 (p.261) has the absolute volume "returned as 0" while muted. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00, 0x54, 0x10);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to GetiPodStateInfo while muted");
        if (p && p->paylen >= 8) {
            CHECK_EQ_INT(p->payload[5], 0x01, "mute state reported");
            CHECK_EQ_INT(p->payload[7], 0x00,
                         "absolute volume while muted");
        }
    }
}

/* The volume path hands the codec centibels, and every check above that
 * names a level relies on the factor being ten. Pin it against the real
 * conversion rather than the comment. */
void test_volume_codec_units_are_centibels(void)
{
    iaptest_enter_idps();

    rbstub_set_volume(IAP_TEST_VOLUME_MAX);
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MAX),
                 "sound_max in centibels");
    rbstub_set_volume(IAP_TEST_VOLUME_MIN);
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(IAP_TEST_VOLUME_MIN),
                 "sound_min in centibels");
    rbstub_set_volume(-10);
    CHECK_EQ_INT(rbstub_calls.codec_volume, IAP_TEST_CB(-10),
                 "-10 dB in centibels");
}

/* Nothing may push state to an accessory that did not ask for it.
 *
 * iap_set_remote_volume() fired once after authentication for every
 * accessory, and was wrong four ways at once:
 *
 *  - It sent 0x03/0x0D RetiPodStateInfo. MFi 4.3.16 (p.265) gives that
 *    as "Origin: Apple device. The Apple device sends this command in
 *    response to Command 0x0C: GetiPodStateInfo" -- there is no
 *    unsolicited form. The unsolicited carrier is 0x03/0x09
 *    RemoteEventNotification, which iap_periodic() already uses.
 *  - It stamped the reply from the device's own counter, so MFi 2.6.1.1
 *    (p.111) obliges the accessory to discard it: "The accessory must
 *    ignore any response from the Apple device whose transaction ID
 *    does not match that of a previous command it has sent."
 *  - It had no DEVICE_LINGO_SUPPORTED(0x03) gate, so a digital-audio
 *    dock that negotiated only 0x00 and 0x0A received Display Remote
 *    traffic.
 *  - It used the absolute volume scale where event 0x04 wants the UI
 *    one, which the split in 4973fd4f26 missed.
 *
 * The subscription path covers the real need: device.volume_reported
 * makes iap_periodic() push the level once, on the correct command, as
 * soon as an accessory enables the event. */
void test_volume_is_not_pushed_to_accessories_that_did_not_ask(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    rbstub_set_volume(-30);

    /* No subscription: several ticks must produce no Display Remote
     * traffic at all. */
    iaptest_tx_clear();
    for (int t = 0; t < 12; t++)
        iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(p->paylen < 2 || p->payload[0] != 0x03,
              "a Display Remote packet (command 0x%02X) went to an "
              "accessory that subscribed to nothing",
              p->paylen > 1 ? p->payload[1] : 0);
    }

    /* Subscribe to event 0x04, and the level arrives once -- as a
     * RemoteEventNotification, on the UI scale. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x90, 0x00, 0x00, 0x00, 0x10);
    iaptest_tx_clear();
    for (int t = 0; t < 6; t++)
        iap_periodic();

    int seen = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen < 7 || p->payload[0] != 0x03)
            continue;
        CHECK_EQ_INT(p->payload[1], 0x09,
                     "the unsolicited carrier is RemoteEventNotification, "
                     "not a response command");
        if (p->payload[1] == 0x09 && p->payload[4] == 0x04) {
            seen++;
            CHECK_EQ_INT(p->payload[6],
                         iap_volume_to_ui_byte(global_status.volume),
                         "the pushed level uses the UI scale");
        }
    }
    CHECK_EQ_INT(seen, 1, "the level should be pushed exactly once on "
                          "subscription");

    /* And nothing more while it sits still. */
    iaptest_tx_clear();
    for (int t = 0; t < 12; t++)
        iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(!(p->paylen >= 5 && p->payload[0] == 0x03
                && p->payload[1] == 0x09 && p->payload[4] == 0x04),
              "the volume notification repeated at a standstill");
    }
}

/* While USB audio is streaming the external DAC owns the volume, so
 * nothing in the iPod can apply a level an accessory sets.
 *
 * MFi 4.3.17 (p.266): "In response, the Apple device sends an iPodAck
 * command with the results of the operation." Answering Success for an
 * operation that did not happen left the accessory believing the level
 * had taken -- and a following GetiPodStateInfo reported the unchanged
 * level, so the two answers disagreed on the same device. */
void test_volume_set_is_refused_while_usb_audio_streams(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_volume(-30);

    rbstub_set_usb_audio_active(true);

    static const struct { unsigned char info, extra; const char *what; } tc[] = {
        { 0x04, 0x00, "info type 0x04" },
        { 0x10, 0x80, "info type 0x10" },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        iaptest_tx_clear();
        rbstub_calls.setvol = 0;
        unsigned char p[9] = { 0x03, 0x0E, 0x00, (unsigned char)(0xF0 + i),
                               tc[i].info, 0x00, 0xFF, tc[i].extra, 0x00 };
        iaptest_rx(p, tc[i].info == 0x04 ? 8 : 9);

        CHECK_EQ_INT(rbstub_calls.setvol, 0,
                     "the volume must not be applied while the DAC owns it");
        CHECK_EQ_INT(global_status.volume, -30,
                     "the stored level must not move either");

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s got no reply", tc[i].what);
        if (r && r->paylen >= 5)
            CHECK(r->payload[4] != 0x00,
                  "%s was acknowledged Success though the volume was not "
                  "applied", tc[i].what);
    }

    /* And with the stream stopped it works again. */
    rbstub_set_usb_audio_active(false);
    iaptest_tx_clear();
    rbstub_calls.setvol = 0;
    IAPTEST_RX(0x03, 0x0E, 0x00, 0xF5, 0x04, 0x00, 0xFF, 0x00);
    CHECK(rbstub_calls.setvol > 0,
          "the volume stopped working once the stream ended");
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 5)
            CHECK_EQ_INT(r->payload[4], 0x00,
                         "and it is acknowledged Success then");
    }
}
