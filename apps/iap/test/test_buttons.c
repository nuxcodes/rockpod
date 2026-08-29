/***************************************************************************
 * Accessory button plumbing.
 *
 * The iAP lingo handlers synthesise BUTTON_RC_* codes. For those to reach
 * a keymap, apps/action.c:697 must add CONTEXT_REMOTE, which it only does
 * for codes covered by the BUTTON_REMOTE mask. Any injected code missing
 * from that mask is silently unroutable.
 ****************************************************************************/

#include "iap_test.h"

#include "config.h"
#include "button.h"
#include "audio.h"
#include "iap-core.h"

/* Every code the iAP layer can place in iap_remotebtn.
 * Sources: apps/iap/iap-lingo2.c:227-242 (Simple Remote ContextButtonStatus)
 *          apps/iap/iap-lingo4.c:2171-2196 (Extended Interface PlayControl)
 *          apps/iap/iap-core.c:1439 (headphone remote play)
 */
static const struct {
    const char   *name;
    unsigned long code;
} injected[] = {
    { "BUTTON_RC_PLAY",     BUTTON_RC_PLAY     },
    { "BUTTON_RC_STOP",     BUTTON_RC_STOP     },
    { "BUTTON_RC_LEFT",     BUTTON_RC_LEFT     },
    { "BUTTON_RC_RIGHT",    BUTTON_RC_RIGHT    },
    { "BUTTON_RC_MENU",     BUTTON_RC_MENU     },
    { "BUTTON_RC_SELECT",   BUTTON_RC_SELECT   },
    { "BUTTON_RC_UP",       BUTTON_RC_UP       },
    { "BUTTON_RC_DOWN",     BUTTON_RC_DOWN     },
    { "BUTTON_RC_VOL_UP",   BUTTON_RC_VOL_UP   },
    { "BUTTON_RC_VOL_DOWN", BUTTON_RC_VOL_DOWN },
};

void test_button_remote_mask_covers_every_injected_code(void)
{
    for (unsigned i = 0; i < sizeof(injected)/sizeof(injected[0]); i++) {
        CHECK((BUTTON_REMOTE & injected[i].code) == injected[i].code,
              "%s (0x%08lX) is not in BUTTON_REMOTE (0x%08lX); "
              "apps/action.c will never add CONTEXT_REMOTE for it, so every "
              "keymap entry using it is dead",
              injected[i].name, injected[i].code,
              (unsigned long)BUTTON_REMOTE);
    }
}

/* A code appearing twice in the mask is harmless at runtime but is the
 * signature of a copy-paste slip that dropped a different code. */
void test_button_remote_mask_has_no_redundant_terms(void)
{
    unsigned long all = 0;
    int bits_expected = 0;

    for (unsigned i = 0; i < sizeof(injected)/sizeof(injected[0]); i++) {
        all |= injected[i].code;
        bits_expected++;
    }

    int bits_in_mask = 0;
    for (int b = 0; b < 32; b++)
        if (BUTTON_REMOTE & (1UL << b))
            bits_in_mask++;

    CHECK_EQ_INT(bits_in_mask, bits_expected,
                 "number of distinct buttons in BUTTON_REMOTE");
    CHECK_EQ_INT((long)BUTTON_REMOTE, (long)all,
                 "BUTTON_REMOTE must equal the OR of every injected code");
}

/* ------------------------------------------------------------------ */
/* AudioButtonStatus (0x02/0x04) uses its OWN bitmap                    */
/* ------------------------------------------------------------------ */

/* iap-lingo2.c routes AudioButtonStatus through the ContextButtonStatus
 * decoder, on the reasoning that it is "basically the same command".
 * That holds for the first two state bytes and for nothing after them.
 *
 * MFi Table 4-14 "Button states" (p.227), byte index 2:
 *   0x01 Repeat Setting Advance   0x02 Power On    0x04 Power Off
 *   0x08 Backlight for 30 Seconds 0x10 Begin Fast Forward
 *   0x20 Begin Rewind             0x40 Menu        0x80 Select
 * byte index 3: 0x01 Up Arrow, 0x02 Down Arrow, 0x04 Backlight Off.
 *
 * MFi Table 4-19 "Audio-specific button values" (p.231), byte index 2:
 *   0x01 Repeat setting advance   0x02 Begin FF    0x04 Begin REW
 *   0x08 Record                   0xF0 Reserved (bits 20:23)
 * byte index 3: 0xFF Reserved (bits 24:31) -- the whole byte.
 *
 * Bytes 0 and 1 are identical in both tables, which is why this
 * survives casual use: volume, play/pause and track skip all work.
 */

/* Drive AudioButtonStatus with one byte-2 value and report the button. */
static unsigned long audio_button(unsigned char b2, unsigned char b3)
{
    iap_remotebtn = BUTTON_NONE;
    unsigned char p[8] = { 0x02, 0x04, 0x00, 0x30, 0x00, 0x00, b2, b3 };
    iaptest_rx(p, sizeof(p));
    unsigned long got = iap_remotebtn;

    /* Release, so the next call starts clean. */
    unsigned char rel[8] = { 0x02, 0x04, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00 };
    iaptest_button_sample(4);
    iaptest_rx(rel, sizeof(rel));
    iaptest_button_sample(4);
    return got;
}

void test_buttons_audio_status_uses_the_audio_bitmap(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Table 4-19 bit 18: Begin REW. Decoded with Table 4-14 this is
     * Power Off, which the handler answers by pausing if playing -- so
     * pressing Rewind stopped the music. */
    CHECK_EQ_INT(audio_button(0x04, 0x00), BUTTON_RC_LEFT,
                 "Begin REW (Table 4-19 byte 2 bit 0x04)");

    /* Table 4-19 bit 17: Begin FF. Decoded with Table 4-14 this is
     * Power On, which produces no button at all -- fast-forward was
     * simply dead. */
    CHECK_EQ_INT(audio_button(0x02, 0x00), BUTTON_RC_RIGHT,
                 "Begin FF (Table 4-19 byte 2 bit 0x02)");

    /* Bits 20:23 of byte 2 are Reserved in Table 4-19, but are Begin
     * Fast Forward, Begin Rewind, Menu and Select in Table 4-14. A
     * reserved bit must not produce a button. */
    static const unsigned char reserved[] = { 0x10, 0x20, 0x40, 0x80 };
    for (unsigned i = 0; i < sizeof(reserved); i++) {
        CHECK_EQ_INT(audio_button(reserved[i], 0x00), BUTTON_NONE,
                     "a reserved byte-2 bit produced a button");
    }

    /* Byte index 3 is reserved in its entirety (bits 24:31). Table 4-14
     * has Up Arrow and Down Arrow there. */
    CHECK_EQ_INT(audio_button(0x00, 0x01), BUTTON_NONE,
                 "byte 3 bit 0x01 is reserved for AudioButtonStatus");
    CHECK_EQ_INT(audio_button(0x00, 0x02), BUTTON_NONE,
                 "byte 3 bit 0x02 is reserved for AudioButtonStatus");
    CHECK_EQ_INT(audio_button(0x00, 0xFF), BUTTON_NONE,
                 "a whole reserved byte 3 produced a button");

    /* Record (Table 4-19 bit 19) has no Rockbox action, but it is still
     * a button being held. The decode keeps iap_timeoutbtn armed while
     * anything is down (iap-lingo2.c: "iap_timeoutbtn = any_bits ? 3 :
     * 0"), because letting it lapse under a held button is what made a
     * held Shuffle re-toggle itself ten times a second. A translation
     * that dropped the unmapped bits would lose that. */
    iap_remotebtn = BUTTON_NONE;
    iap_timeoutbtn = 0;
    IAPTEST_RX(0x02, 0x04, 0x00, 0x60, 0x00, 0x00, 0x08, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Record must produce no button");
    CHECK(iap_timeoutbtn != 0,
          "a held Record left the auto-release timer disarmed, so the "
          "decode no longer knows a button is down");

    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x04, 0x00, 0x61, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(iap_timeoutbtn, 0,
                 "releasing every button must disarm the timer");
}

/* Bytes 0 and 1 really are identical between the two tables, so the
 * shared decode is right for them and must stay working. */
void test_buttons_audio_status_shares_the_first_two_bytes(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    struct { unsigned char b0, b1; unsigned long btn; const char *what; } tc[] = {
        { 0x02, 0x00, BUTTON_RC_VOL_UP,   "Volume Up (byte 0 bit 1)" },
        { 0x04, 0x00, BUTTON_RC_VOL_DOWN, "Volume Down (byte 0 bit 2)" },
        { 0x08, 0x00, BUTTON_RC_RIGHT,    "Next Track (byte 0 bit 3)" },
        { 0x10, 0x00, BUTTON_RC_LEFT,     "Previous Track (byte 0 bit 4)" },
        { 0x00, 0x01, BUTTON_RC_PLAY,     "Play/Resume (byte 1 bit 0)" },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        iap_remotebtn = BUTTON_NONE;
        unsigned char p[8] = { 0x02, 0x04, 0x00, (unsigned char)(0x40 + i),
                               tc[i].b0, tc[i].b1, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));
        CHECK_EQ_INT(iap_remotebtn, tc[i].btn, tc[i].what);

        iaptest_button_sample(4);
        unsigned char rel[8] = { 0x02, 0x04, 0x00, (unsigned char)(0x50 + i),
                                 0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(rel, sizeof(rel));
        iaptest_button_sample(4);
    }
}

void test_buttons_transport_ignores_unrelated_audio_flags(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();

    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_RECORD);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x01);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Play retriggered while playback was already active");

    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x02);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_PLAY,
                 "Pause ignored playback with an unrelated status flag");

    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(4);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE |
                            AUDIO_STATUS_RECORD);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x02);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Pause toggled playback that was already paused");

    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(4);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_RECORD);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x04);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_PLAY,
                 "Power Off ignored playback with an unrelated status flag");
}

/* Power On press/release, and the short release form.
 *
 * MFi 4.2.3 (p.216): "It is not necessary to transmit any trailing bytes
 * in which no bits are set. If this option is exercised, the length of
 * the packet in the header must be adjusted accordingly."
 *
 * MFi 4.2.7 (p.226): "When all buttons are released, the accessory must
 * send a button status packet with a 0x00 payload to indicate that no
 * buttons are pressed."
 *
 * So the canonical release is a ONE-byte payload, and the release check
 * required the third state byte to be present before it would look at
 * it. A conformant accessory could therefore never clear the latch.
 *
 * What hangs off it is the Belkin TuneTalk workaround: GetDevCaps is
 * only sent on the power-on release edge, because the microphone
 * ignores it before then. No release, no GetDevCaps, no stereo line-in.
 */
static bool saw_getdevcaps(void)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x01 && p->payload[1] == 0x07)
            return true;
    }
    return false;
}

void test_buttons_power_on_release_may_omit_trailing_bytes(void)
{
    /* GetDevCaps is only sent if the accessory declared the Microphone
     * lingo, so declare it. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01) | (1u << 0x02));
    iaptest_force_authenticated();

    /* Power On: Table 4-14 byte index 2, bit 0x02. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x02);
    CHECK(!saw_getdevcaps(), "GetDevCaps sent on the press, not the release");
    iaptest_button_sample(4);

    /* Release the conformant short way: a one-byte zero payload. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00);
    CHECK(saw_getdevcaps(),
          "a one-byte release payload did not clear the power-on latch, "
          "so GetDevCaps was never sent (MFi 4.2.7, p.226)");
    iaptest_button_sample(4);

    /* And the long form must still work. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x02);
    iaptest_button_sample(4);
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x00);
    CHECK(saw_getdevcaps(),
          "the full-length release form stopped working");
    iaptest_button_sample(4);

    /* A release must fire GetDevCaps once, not on every later packet.
     * The latch is what makes it an edge. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00);
    CHECK(!saw_getdevcaps(),
          "GetDevCaps fired again with no intervening power-on press, "
          "so the latch is not acting as an edge");
}

/* The latch is a file-static that iap_reset_device() cannot reach, so a
 * press left pending when an accessory is unplugged is still pending
 * for the next one -- and the next accessory's first release fires a
 * GetDevCaps it never asked for. */
void test_buttons_power_on_latch_does_not_survive_a_reset(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01) | (1u << 0x02));
    iaptest_force_authenticated();

    /* Press, and never release: the accessory is unplugged mid-press. */
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x02);
    iaptest_button_sample(4);

    /* A new accessory arrives. */
    iap_reset_device(&device);
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01) | (1u << 0x02));
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00);
    CHECK(!saw_getdevcaps(),
          "the power-on latch survived the reset, so a new accessory's "
          "first button release fired a GetDevCaps for a press that "
          "belonged to the previous one");
}

/* MFi 4.2.7 (p.226): "The Apple device does not return a packet to the
 * accessory in response to this command." MFi 4.2.8 (p.228): the
 * acknowledgement goes out "in response to any command sent from the
 * accessory, except command 0x00."
 *
 * The three length checks at the top of iap_handlepkt_mode2() run for
 * every command, so a truncated ContextButtonStatus was answered with
 * an iPodAck -- and 4.2.3 (p.216) says truncation is expected on a
 * shared UART: "Multiple button status packets cannot be sent back to
 * back; otherwise, the repeated button status packets may be
 * misinterpreted as being part of a corrupted packet." An in-line
 * remote has no receiver for the reply. */
void test_buttons_context_status_is_never_answered(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();

    /* Every length below the IDPS minimum of five. */
    for (int n = 2; n <= 4; n++) {
        unsigned char p[5] = { 0x02, 0x00, 0x00, 0x60, 0x00 };
        iaptest_tx_clear();
        iaptest_rx(p, n);
        CHECK(iaptest_tx_count() == 0,
              "a %d-byte ContextButtonStatus was answered with %d "
              "packet(s)", n, iaptest_tx_count());
    }

    /* And legacy, below the minimum of three. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "a two-byte legacy ContextButtonStatus was answered");

    /* Other Simple Remote commands must still be acked when short --
     * 4.2.8's exception is command 0x00 alone. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x03);
    CHECK(iaptest_tx_count() > 0,
          "a short command other than ContextButtonStatus went "
          "unanswered; the exception is 0x00 alone");
}

/* The button auto-release is paced by the tick, and iap_task() drops
 * that tick from 10 Hz to 1 Hz once the link is idle.
 *
 * MFi 4.2.4 (p.218): "if a command has not been received within
 * approximately 200 ms after the last button status command, the button
 * status will be reset to all buttons up."
 *
 * iap_timeoutbtn is decremented only by iap_periodic(), which runs on
 * that tick. A button arriving between two 1 Hz ticks posts
 * IAP_EV_MSG_RCVD, not IAP_EV_TICK, so nothing decremented it until the
 * already-armed second had elapsed. firmware/drivers/button.c crosses
 * REPEAT_START at 300 ms, so a lost release turns a tap into a seek. */
void test_buttons_press_rearms_the_tick(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* A packet that presses nothing must not disturb the timer. */
    rbstub_reset_calls();
    IAPTEST_RX(0x00, 0x07);
    CHECK_EQ_INT(rbstub_calls.timeouts, 0,
                 "a non-button packet re-armed the tick");

    /* A button press must. */
    rbstub_reset_calls();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x70, 0x01);
    CHECK(iap_timeoutbtn != 0, "the press did not arm the release timer");
    CHECK(rbstub_calls.timeouts > 0,
          "a button press left the tick at whatever it was, so the "
          "auto-release waits for a timer that may be a second away");
    CHECK(rbstub_calls.timeout_ticks > 0
          && rbstub_calls.timeout_ticks <= (HZ / 5),
          "the re-armed tick is %d ticks; MFi 4.2.4 (p.218) wants the "
          "release inside about 200 ms, so it must be no slower than "
          "%d", rbstub_calls.timeout_ticks, HZ / 5);

    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x71, 0x00);
    iaptest_button_sample(4);
}

/* iap_task() drops the tick to 1 Hz once nothing needs it faster. Each
 * thing that does need it faster has to be in that condition, or its
 * cadence silently becomes one second.
 *
 * The Extended Interface notification mask was added to device_t when
 * the two subscriptions were separated, and the idle check still asked
 * only about the Display Remote one -- so an accessory subscribed to
 * play-status notifications alone had them delivered at 1 Hz. */
void test_buttons_idle_tick_accounts_for_every_subscription(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    device.accinfo = ACCST_NONE;

    /* Nothing outstanding: the slow tick is correct here. */
    int idle = rbstub_run_timeout();
    CHECK(idle == HZ, "an idle link should tick at 1 Hz, got %d ticks",
          idle);

    /* A Display Remote subscription must speed it up. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x80, 0x00, 0x00, 0x00, 0x10);
    CHECK(rbstub_run_timeout() < HZ,
          "the tick stayed slow with Display Remote notifications on");

    /* And so must an Extended Interface one, on its own. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    device.accinfo = ACCST_NONE;
    IAPTEST_RX(0x00, 0x05, 0x00, 0x81);
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x82, 0x00, 0x00, 0x00, 0x04);

    CHECK(device.pb_notifications != 0,
          "the play-status subscription did not take");
    CHECK(!device.do_notify,
          "this case needs the Display Remote mask clear to isolate the "
          "play-status one");
    CHECK(rbstub_run_timeout() < HZ,
          "the tick stayed at 1 Hz with only play-status notifications "
          "subscribed, so they are delivered once a second");

    /* And a held button, which is what drains iap_timeoutbtn. Re-arming
     * on the packet gets the first decrement in promptly; this is the
     * condition that keeps the following ones coming. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    device.accinfo = ACCST_NONE;
    CHECK(rbstub_run_timeout() == HZ, "the link is not idle to start");

    IAPTEST_RX(0x02, 0x00, 0x00, 0x83, 0x01);
    CHECK(iap_timeoutbtn != 0, "the press did not arm the release timer");
    CHECK(rbstub_run_timeout() < HZ,
          "the tick returned to 1 Hz with a button still down, so the "
          "auto-release drains a step a second");

    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x84, 0x00);
    iaptest_button_sample(4);
}

/* MFi 4.2.3 (p.216): the status bytes are "constructed by ORing the
 * masks of the buttons together", and "if a second button is pressed
 * while the first button is down, the button status packet sent by the
 * accessory must include status for both buttons".
 *
 * The decode was an if/else-if chain, so only the lowest non-zero byte
 * was ever read. While anything in byte 0 was held -- Volume Up, say --
 * Menu, Select, FF, REW, Up and Down were all unreachable. */
void test_buttons_chords_across_status_bytes(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();

    /* Volume Up (byte 0 bit 1) held together with Menu (byte 2 bit 6). */
    IAPTEST_RX(0x02, 0x00, 0x02, 0x00, 0x40, 0x00);

    CHECK(iap_remotebtn & BUTTON_RC_VOL_UP,
          "Volume Up was lost from a chord (iap_remotebtn = 0x%08X)",
          iap_remotebtn);
    CHECK(iap_remotebtn & BUTTON_RC_MENU,
          "Menu was dropped because Volume Up was held in an earlier "
          "status byte (iap_remotebtn = 0x%08X)", iap_remotebtn);

    /* Let the raised button go out. While iap_repeatbtn is set,
     * iap_handlepkt() re-queues the next packet instead of handling it,
     * so without this the sends below are simply dropped and the case
     * passes on the first chord's state. */
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* And a three-byte chord: Volume Down, Select, Up. */
    IAPTEST_RX(0x02, 0x00, 0x04, 0x00, 0x80, 0x01);

    CHECK(iap_remotebtn & BUTTON_RC_VOL_DOWN, "Volume Down lost");
    CHECK(iap_remotebtn & BUTTON_RC_SELECT,   "Select lost");
    CHECK(iap_remotebtn & BUTTON_RC_UP,       "Up lost");

    /* All released is still all released. */
    for (int t = 0; t < 4; t++)
        iap_periodic();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a full zero payload did not release every button");
}

/* The Simple Remote length checks, driven so they are load-bearing.
 *
 * Same technique as test_lengths.c: send a complete packet first, so a
 * handler that skips its length check reads the previous packet's bytes
 * off the end of the short one and acts on them. Left to itself the
 * receive buffer past the end is zero, and a zero button bitmap is
 * indistinguishable from a refusal -- which is why these four checks
 * could each be deleted with the suite staying green.
 */

/* The Simple Remote acknowledgement: 0x02 0x01, the transaction ID when
 * one is in force, then status and the command being acknowledged.
 *
 * Every case below asserts one of these went out. That is not decoration
 * -- iap_handlepkt() re-queues a packet while iap_repeatbtn is set, so a
 * short packet sent too soon after a button is silently dropped, and a
 * case that only asserted "no button was raised" would pass without the
 * handler ever seeing it. */
static const struct iaptest_pkt *l2_ack(void)
{
    for (int i = iaptest_tx_count() - 1; i >= 0; i--) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 4 && p->payload[0] == 0x02
            && p->payload[1] == 0x01)
            return p;
    }
    return NULL;
}

/* Did the handler refuse this command as Bad Parameter? */
static bool l2_refused(unsigned char cmd)
{
    const struct iaptest_pkt *p = l2_ack();
    return p && p->payload[p->paylen - 2] == IAP_ACK_BAD_PARAM
             && p->payload[p->paylen - 1] == cmd;
}

/* AudioButtonStatus (0x04) rather than ContextButtonStatus (0x00),
 * because 0x00 is the one command in this lingo that must never be
 * answered -- so a rejection of it is silence, and silence is also what
 * a packet deferred behind iap_repeatbtn produces. 0x04 shares 0x00's
 * decode and has no length check of its own, so it exercises exactly
 * the same three prologue guards while leaving an ack to assert on.
 */

/* Legacy framing has one length check standing between the packet and
 * the button decode, so a two-byte packet is the whole test. */
void test_buttons_legacy_short_packet_is_not_read_past(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();

    /* Complete, with Volume Up held: lingo, command, four state bytes. */
    iap_remotebtn = BUTTON_NONE;
    IAPTEST_RX(0x02, 0x04, 0x02, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_VOL_UP,
                 "the full-length packet must raise Volume Up, or the "
                 "short one below would prove nothing");

    /* Let the button go out without sending anything, so the 0x02 stays
     * in the receive buffer where an unguarded read would find it. */
    iaptest_button_sample(4);
    CHECK_EQ_INT(iap_repeatbtn, 0,
                 "the repeat deferral is still armed, so the packet "
                 "below would be re-queued rather than handled");
    iap_remotebtn = BUTTON_NONE;
    iaptest_tx_clear();

    /* Two bytes: lingo and command, no state at all. */
    {
        unsigned char p[2] = { 0x02, 0x04 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK(l2_refused(0x04),
          "a two-byte AudioButtonStatus was not refused as Bad "
          "Parameter, so its state bytes came from the packet before it");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a two-byte AudioButtonStatus raised a button");
}

/* Under IDPS the transaction ID sits between the command and the state
 * bytes, and two more checks guard it: one so the ID can be read before
 * a rejection has to quote it, one for the state byte after it. */
void test_buttons_idps_short_packets_are_not_read_past(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Complete: lingo, command, transaction 0x1234, Volume Up. */
    iap_remotebtn = BUTTON_NONE;
    IAPTEST_RX(0x02, 0x04, 0x12, 0x34, 0x02, 0x00, 0x00, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_VOL_UP,
                 "the full-length packet must raise Volume Up, or the "
                 "short ones below would prove nothing");

    iaptest_button_sample(4);
    CHECK_EQ_INT(iap_repeatbtn, 0, "the repeat deferral is still armed");
    iap_remotebtn = BUTTON_NONE;

    /* Three bytes: the transaction ID is cut in half. MFi 2.6.1.1
     * (p.111) has the accessory discard an acknowledgement whose ID
     * matches no command it sent, so quoting the low byte of the
     * previous packet's ID is worse than quoting nothing -- it names a
     * command that was already answered, and the accessory acts on the
     * rejection as though the earlier command had failed. */
    iaptest_tx_clear();
    {
        unsigned char p[3] = { 0x02, 0x04, 0x12 };
        iaptest_rx(p, sizeof(p));
    }
    const struct iaptest_pkt *a = l2_ack();
    CHECK(a != NULL && a->paylen >= 6,
          "the three-byte packet drew no Simple Remote acknowledgement, "
          "so nothing below is being tested");
    if (a && a->paylen >= 6)
        CHECK_EQ_INT(a->payload[3], 0x00,
                     "the rejection quoted a transaction ID low byte "
                     "that was not in the packet");

    /* Four bytes: a complete transaction ID, but no state byte. */
    iaptest_tx_clear();
    iap_remotebtn = BUTTON_NONE;
    {
        unsigned char p[4] = { 0x02, 0x04, 0x12, 0x35 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK(l2_refused(0x04),
          "a state-less AudioButtonStatus was not refused, so its state "
          "bytes came from the packet before it");
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a state-less AudioButtonStatus raised a button");
    a = l2_ack();
    if (a && a->paylen >= 6)
        CHECK(a->payload[2] == 0x12 && a->payload[3] == 0x35,
              "the rejection must quote this packet's own transaction "
              "ID (got 0x%02X%02X)", a->payload[2], a->payload[3]);
}
/* Every Simple Remote command but ContextButtonStatus needs the lingo
 * negotiated. ContextButtonStatus is exempt on purpose -- see the
 * comment on the gate -- so the gate is only observable through one of
 * the others. */
void test_buttons_unnegotiated_lingo_is_refused(void)
{
    /* General and Display Remote: this accessory never asked for the
     * Simple Remote lingo. General has to be in the mask or the
     * identify is refused whole and nothing is negotiated at all. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    iaptest_force_authenticated();

    iap_remotebtn = BUTTON_NONE;
    iaptest_tx_clear();

    /* AudioButtonStatus with Volume Up held. */
    IAPTEST_RX(0x02, 0x04, 0x00, 0x30, 0x02, 0x00, 0x00, 0x00);

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "an accessory that never negotiated the Simple Remote "
                 "lingo still drove its buttons");

    bool refused = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 4 && p->payload[0] == 0x02
            && p->payload[1] == 0x01
            && p->payload[p->paylen - 2] == IAP_ACK_BAD_PARAM
            && p->payload[p->paylen - 1] == 0x04)
            refused = true;
    }
    CHECK(refused, "the refusal must be an explicit Bad Parameter ack");

    /* And the exemption still holds: ContextButtonStatus works. */
    IAPTEST_RX(0x02, 0x00, 0x02, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_VOL_UP,
                 "ContextButtonStatus is exempt from the gate so a "
                 "remote powered before Rockbox still works");
}

/* GetAccessoryCaps goes out on a Power On release, and only to an
 * accessory that declared the Microphone lingo.
 *
 * MFi 2.2 (p.100) grants an accessory the lingoes it declares and no
 * others. This one has a sharper edge than most: C.5.5 (p.538) gives
 * GetAccessoryCaps a 200 ms timeout with no retry, so sending it to an
 * accessory with no Microphone receiver spends that window for nothing
 * and can have the accessory marked absent. */
void test_buttons_getdevcaps_needs_the_microphone_lingo(void)
{
    /* Declared: the send has to happen, or the check below is vacuous.
     * The existing power-on cases cover the latch itself; this is only
     * about who is allowed to receive it. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01) | (1u << 0x02));
    iaptest_force_authenticated();

    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x02);   /* Power On press */
    iaptest_button_sample(4);
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00);               /* release */
    CHECK(saw_getdevcaps(),
          "GetAccessoryCaps was not sent to an accessory that declared "
          "the Microphone lingo, so the check below proves nothing");

    /* Not declared: same press, same release, nothing sent. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();

    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x02);
    iaptest_button_sample(4);
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00);
    CHECK(!saw_getdevcaps(),
          "GetAccessoryCaps was sent to an accessory that never declared "
          "the Microphone lingo");
}

/* Stop, Table 4-14 (p.226) byte 0 bit 7.
 *
 * MFi 4.2.9 (p.228): "Apple products running iOS 3.2 support only the
 * Stop, Play/Resume, and Pause button values." The other two of those
 * three are decoded; this one was not decoded at all, so pressing Stop
 * on a remote did nothing.
 *
 * It is not a button. The Playback Engine has no Stop and every remote
 * code that exists means something else -- the same reason PlayControl
 * 0x02 calls audio_stop() rather than raising one. */
void test_buttons_stop_stops_once_per_press(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();

    /* One press, repeated as a real remote repeats it. Two samples per
     * repeat, or iap_handlepkt() defers every other packet. */
    for (int r = 0; r < 6; r++) {
        IAPTEST_RX(0x02, 0x00, 0x80, 0x00);
        iaptest_button_sample(2);
    }

    CHECK(rbstub_calls.stop == 1,
          "a held Stop called audio_stop() %d times; the accessory "
          "repeats its status every 30 to 100 ms", rbstub_calls.stop);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "Stop raised a remote button, and every code the "
                 "Playback Engine has means something else");

    /* Release, then press again: that is a second stop. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(2);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    IAPTEST_RX(0x02, 0x00, 0x80, 0x00);
    CHECK_EQ_INT(rbstub_calls.stop, 2,
                 "a second press after a release did not stop again");
}

/* The optional state bytes are optional.
 *
 * MFi Table 4-18 (p.229) makes bytes 1 to 3 of the button status
 * optional, so a conformant accessory may send fewer than the full
 * payload. Each is read behind an L2_HAVE() test, and all four of those
 * could be forced true with the suite green -- a short packet would
 * then decode whatever followed it in the receive buffer as held
 * buttons.
 *
 * The mandatory first byte is covered by
 * test_buttons_legacy_short_packet_is_not_read_past; this is the same
 * technique applied to the three after it. */
void test_buttons_optional_state_bytes_are_not_invented(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    rbstub_set_playlist(20, 3);
    /* Paused, not playing: byte 1 bit 0 is Play/Resume, and its arm
     * raises a button only when the device is not already playing. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);

    /* A full four-byte status with something set in every optional
     * byte: byte 1 bit 0 Play/Resume, byte 2 bit 6 Menu, byte 3 bit 0
     * Up Arrow. Byte 0 stays clear so the assertions below are about
     * the optional bytes alone. */
    iap_remotebtn = BUTTON_NONE;
    IAPTEST_RX(0x02, 0x00, 0x00, 0x01, 0x40, 0x01);
    CHECK(iap_remotebtn != BUTTON_NONE,
          "the full-length status raised nothing, so the short one "
          "below would prove nothing");

    /* Let it go without sending a release, so those bytes stay in the
     * receive buffer where an unguarded read would find them. */
    iaptest_button_sample(4);
    CHECK_EQ_INT(iap_repeatbtn, 0, "the repeat deferral is still armed");
    iap_remotebtn = BUTTON_NONE;
    iap_timeoutbtn = 0;

    /* Three bytes: lingo, command, and the one mandatory state byte,
     * which is zero. Nothing is pressed. */
    {
        unsigned char p[3] = { 0x02, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));
    }

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a status with only its mandatory byte raised a "
                 "button, so the optional ones came from the packet "
                 "before it");
    CHECK_EQ_INT(iap_timeoutbtn, 0,
                 "the auto-release timer was armed for a button that "
                 "was not in the packet");
}

/* iap_running is set from the first sync byte and cleared only by
 * iap_setup(), so it stays true for the life of the boot. The idle
 * check asked for device.auth.state == AUST_AUTH, and a detach puts it
 * back to AUST_NONE -- so from the moment any accessory was unplugged
 * until another one finished authenticating, the tick was pinned at
 * 10 Hz. That is the whole of the time there is nothing to do.
 *
 * The handshake states between the two do need 10 Hz, which is what
 * DEVICE_AUTH_RUNNING names. */
void test_buttons_idle_tick_survives_a_detach(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.auth.state = AUST_AUTH;
    device.accinfo = ACCST_NONE;
    CHECK(rbstub_run_timeout() == HZ, "the link is not idle to start");

    /* The accessory goes away. */
    iap_reset_state(IF_IAP_MP(0));
    CHECK_EQ_INT(device.auth.state, AUST_NONE,
                 "the detach did not clear the authentication state");

    int idle = rbstub_run_timeout();
    CHECK(idle == HZ,
          "the tick stayed at %d ticks after a detach -- 10 Hz for as "
          "long as the dock is empty", idle);

    /* A handshake in progress still gets 10 Hz. */
    device.auth.state = AUST_INIT;
    CHECK(rbstub_run_timeout() < HZ,
          "the tick went slow during the authentication handshake");

    device.auth.state = AUST_CERTREQ;
    CHECK(rbstub_run_timeout() < HZ,
          "the tick went slow waiting for the accessory's certificate");
}

/* iap_periodic() is what sends GetDevAuthenticationInfo, and
 * IAP_EV_MSG_RCVD runs iap_handlepkt() alone -- so the packet that asks
 * for authentication has to knock on the queue, or the handshake waits
 * for a tick that is now up to a second away. */
void test_buttons_identify_wakes_the_handshake(void)
{
    /* EndIDPS is the packet that sets AUST_INIT. Count the posts it
     * makes on its own, not the whole IDPS sequence's -- every packet
     * that arrives posts IAP_EV_MSG_RCVD, so a total says nothing. */
    iaptest_init();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);

    rbstub_reset_calls();
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);   /* EndIDPS, Continue */

    CHECK_EQ_INT(device.auth.state, AUST_INIT,
                 "EndIDPS did not ask for authentication");
    CHECK(rbstub_calls.queue_post >= 1
          && rbstub_calls.last_event == IAP_EV_TICK,
          "EndIDPS set AUST_INIT and did not knock on the queue, so "
          "GetDevAuthenticationInfo waits for the next tick -- a second "
          "away now that an idle link ticks at 1 Hz (%d post(s), last "
          "event %ld)", rbstub_calls.queue_post, rbstub_calls.last_event);

    /* The same for the legacy path: IdentifyDeviceLingoes with an
     * options word that asks for authentication. */
    iaptest_init();
    rbstub_reset_calls();
    /* Lingo bitmask (General, Simple Remote, Display Remote), then a
     * 4-byte options word with bits 0..1 asking for authentication and
     * a non-zero 4-byte device id -- iap-lingo0.c wants both before it
     * starts a handshake. */
    IAPTEST_RX(0x00, 0x13, 0x00, 0x00, 0x00, 0x0D,
               0x00, 0x00, 0x00, 0x03,
               0x00, 0x00, 0x00, 0x01);
    CHECK_EQ_INT(device.auth.state, AUST_INIT,
                 "IdentifyDeviceLingoes did not ask for authentication");
    CHECK(rbstub_calls.last_event == IAP_EV_TICK,
          "IdentifyDeviceLingoes set AUST_INIT and did not knock on the "
          "queue (last event %ld)", rbstub_calls.last_event);
}

/* Table 4-14 (p.227) buttons 5 and 6: Next Album at byte 0 mask 0x20,
 * Previous Album at 0x40. Table 4-19 (p.230) puts them at the same
 * byte and mask for AudioButtonStatus.
 *
 * Neither was decoded, on either command. Over ContextButtonStatus the
 * accessory gets no packet at all (4.2.7, p.226) and cannot tell; over
 * AudioButtonStatus it gets iPodAck Success for something that did not
 * happen. This device advertises Simple Remote 1.02 (iap-core.c) and
 * Table 3-132 lingo-0x02 bits 00 and 01 (iap-lingo0.c), so it claims
 * them both ways.
 *
 * p.217: "The Next and Previous Album commands have no effect if there
 * is no next or previous album to go to in the Now Playing list" --
 * which is what audio_next_dir()/audio_prev_dir() already do. */
void test_buttons_album_skip_is_decoded(void)
{
    static const struct { unsigned char lingo, cmd, mask;
                          int next, prev; const char *what; } t[] = {
        { 0x02, 0x00, 0x20, 1, 0, "ContextButtonStatus Next Album"     },
        { 0x02, 0x00, 0x40, 0, 1, "ContextButtonStatus Previous Album" },
        { 0x02, 0x04, 0x20, 1, 0, "AudioButtonStatus Next Album"       },
        { 0x02, 0x04, 0x40, 0, 1, "AudioButtonStatus Previous Album"   },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
        iaptest_force_authenticated();
        rbstub_set_playlist(40, 3);
        rbstub_set_audio_status(AUDIO_STATUS_PLAY);
        rbstub_reset_calls();

        unsigned char p[3] = { t[i].lingo, t[i].cmd, t[i].mask };
        iaptest_rx(p, sizeof(p));

        CHECK(rbstub_calls.next_dir == t[i].next,
              "%s: next_dir called %d times, wanted %d",
              t[i].what, rbstub_calls.next_dir, t[i].next);
        CHECK(rbstub_calls.prev_dir == t[i].prev,
              "%s: prev_dir called %d times, wanted %d",
              t[i].what, rbstub_calls.prev_dir, t[i].prev);

        /* Held, it moves once: the accessory repeats its status every
         * 30 to 100 ms. */
        for (int r = 0; r < 5; r++) {
            iaptest_rx(p, sizeof(p));
            iaptest_button_sample(1);
        }
        CHECK(rbstub_calls.next_dir + rbstub_calls.prev_dir == 1,
              "%s moved %d times while held", t[i].what,
              rbstub_calls.next_dir + rbstub_calls.prev_dir);

        /* Released and pressed again is a second move. */
        unsigned char up[3] = { t[i].lingo, t[i].cmd, 0x00 };
        iaptest_rx(up, sizeof(up));
        iaptest_button_sample(4);
        for (int k = 0; k < 6; k++)
            iap_periodic();
        iaptest_rx(p, sizeof(p));
        CHECK(rbstub_calls.next_dir + rbstub_calls.prev_dir == 2,
              "%s did not move again after the button came up (%d moves)",
              t[i].what, rbstub_calls.next_dir + rbstub_calls.prev_dir);
    }
}
