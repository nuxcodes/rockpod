/***************************************************************************
 * Integration: real accessories, following the specification's own flows.
 *
 * The other cases send a command and check the answer. These play the
 * part of an actual product from the moment it is plugged in, in the
 * order MFi R46 lays down, and refuse to go on when a step does not
 * produce what the spec says it must. If identification fails, the
 * accessory never reaches playback -- exactly as on a bench.
 *
 * The sequences come from the specification's own worked flows:
 *   2.3.2  Identifying with IDPS                     (p.95-96)
 *   2.3.3  Determining Apple Device Capabilities     (p.97)
 *   2.4    Authentication                            (p.103-104)
 *   Table 3-133  Sample GetiPodOptionsForLingo flow  (p.196-198)
 *   Table 2-15   Sample notification flow            (p.115)
 *   4.10   Digital Audio bring-up                    (p.345-357)
 *
 * Each accessory below is modelled on a real class of product, and each
 * asserts the things that product actually depends on. A car head unit
 * that gets no play-status notification has a blank display; a Bluetooth
 * transmitter that never learns the volume plays at the wrong level.
 * Those are the assertions.
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"    /* radio_present */
#endif
#include "appevents.h"
#include "button.h"
#include "audio.h"

extern void iap_periodic(void);

/* ------------------------------------------------------------------ */
/* Helpers that assert as they go                                      */
/* ------------------------------------------------------------------ */

/* Find the first reply with this lingo and command. */
static const struct iaptest_pkt *reply(unsigned char lingo,
                                       unsigned short command)
{
    int idb = (lingo == 0x04) ? 2 : 1;

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen < 1 + idb || p->payload[0] != lingo)
            continue;
        unsigned short c = (idb == 2)
            ? (unsigned short)((p->payload[1] << 8) | p->payload[2])
            : (unsigned short)p->payload[1];
        if (c == command)
            return p;
    }
    return NULL;
}

/* Send a command and require a particular reply. Returns false and
 * reports if it does not arrive, so a caller can stop the flow. */
static bool step(const char *what, const unsigned char *cmd, int len,
                 unsigned char lingo, unsigned short want)
{
    iaptest_tx_clear();
    iapacc_send(cmd, len);

    if (!reply(lingo, want)) {
        int idb = (lingo == 0x04) ? 2 : 1;
        CHECK(false, "%s: no lingo 0x%02X command 0x%0*X in reply",
              what, lingo, idb * 2, want);
        return false;
    }
    return true;
}

#define STEP(what, lingo, want, ...) ({                       \
        static const unsigned char _c[] = { __VA_ARGS__ };    \
        step((what), _c, (int)sizeof(_c), (lingo), (want));   \
    })

/* Nothing the device said may have broken a rule. */
static bool clean(const char *what)
{
    /* Liveness first, and a counted check either way.
     *
     * "No violations" is also what a model that saw nothing says, and a
     * detached one sees nothing -- accessory.h documents
     * iapacc_judged() for exactly that. And the old form only reached
     * CHECK on failure, so a passing session contributed no counted
     * check at all and the zero-assertion guard could not see a case
     * whose only verification this was. */
    CHECK(iapacc_judged() > 0,
          "%s: the accessory model judged no packets, so it rejecting "
          "none says nothing", what);

    CHECK(iapacc_violations() == 0,
          "%s: %d protocol violation(s); first was: %s",
          what, iapacc_violations(), iapacc_first_violation());

    return iapacc_violations() == 0;
}

/* ------------------------------------------------------------------ */
/* A Bluetooth transmitter on the dock connector                       */
/* ------------------------------------------------------------------ */

/* The reported hardware: identifies through IDPS, takes digital audio,
 * has no volume control of its own, so it must be told the level and
 * kept up to date. */
void test_integration_bluetooth_transmitter(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03, 0x0A };

    iapacc_attach();

    /* 2.3.2: StartIDPS, declare, EndIDPS. */
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    if (!clean("identification"))
        return;
    CHECK(iapacc_transactions_enabled(),
          "transaction IDs were torn down during identification");

    /* 2.4: authentication. The signature exchange needs a coprocessor we
     * do not have, so the state is forced; everything after it is real. */
    iaptest_force_authenticated();

    /* 2.3.3: ask what the device can do before using it. Volume sync
     * hangs off the Display Remote option bits. */
    if (!STEP("capability query", 0x00, 0x4C, 0x00, 0x4B, 0x03))
        return;
    {
        const struct iaptest_pkt *p = reply(0x00, 0x4C);
        CHECK(p->paylen == 13,
              "RetiPodOptionsForLingo should be 13 bytes, got %d",
              p->paylen);
        CHECK(p->payload[4] == 0x03,
              "the reply must echo the lingo asked about, got 0x%02X",
              p->payload[4]);
        CHECK((p->payload[12] & 0x01) != 0,
              "Display Remote option bit 00 (UI Volume control) is clear, "
              "so a conformant accessory will not attempt volume sync "
              "(MFi Table 3-132, p.194)");
    }

    /* Note to Table 4-59 (p.256): line out first. */
    if (!STEP("enable line out", 0x00, 0x02, 0x00, 0x2B, 0x03, 0x01, 0x00))
        return;

    /* Then volume notifications, bit 4. */
    if (!STEP("enable volume notifications", 0x03, 0x00,
              0x03, 0x08, 0x00, 0x00, 0x00, 0x10))
        return;

    /* The accessory must learn the current level without being told to
     * ask. It has no knob of its own. */
    iaptest_tx_clear();
    rbstub_set_volume(-30);
    iap_periodic();
    {
        const struct iaptest_pkt *p = reply(0x03, 0x09);
        CHECK(p != NULL,
              "no volume notification after enabling them, so the "
              "transmitter has no idea how loud to play");
        if (p) {
            CHECK(p->paylen == 7, "event 0x04 notification should be 7 "
                  "bytes with a transaction ID, got %d", p->paylen);
            CHECK(p->payload[4] == 0x04,
                  "notification event should be 0x04 Mute/UI Volume, "
                  "got 0x%02X", p->payload[4]);
        }
    }

    /* 4.10: digital audio. The accessory reports its rates, the device
     * answers with the one it will send. */
    iaptest_tx_clear();
    /* MFi 4.10.8 (p.355): "At a minimum, every accessory must support
     * the sample rates 32 KHz, 44.1 KHz, and 48 KHz", and a list
     * missing any of them "is invalid". */
    IAPACC_SEND(0x0A, 0x03,
                0x00, 0x00, 0x7D, 0x00,     /* 32000 */
                0x00, 0x00, 0xAC, 0x44,     /* 44100 */
                0x00, 0x00, 0xBB, 0x80);    /* 48000 */
    {
        const struct iaptest_pkt *p = reply(0x0A, 0x04);
        CHECK(p != NULL,
              "no TrackNewAudioAttributes after RetAccessorySampleRateCaps, "
              "so digital audio never starts");
    }

    /* Now use it: the user turns the volume up and down, tracks change.
     * Each distinct level must produce exactly one notification, and an
     * unchanged level none at all. */
    int notifications = 0;
    for (int i = 0; i < 6; i++) {
        iaptest_tx_clear();
        rbstub_set_volume(-50 + i * 10);
        iap_periodic();
        if (reply(0x03, 0x09))
            notifications++;

        iaptest_tx_clear();
        iap_periodic();                 /* nothing changed */
        CHECK(reply(0x03, 0x09) == NULL,
              "a second tick at an unchanged volume sent another "
              "notification; that is ten packets a second forever");
    }
    CHECK_EQ_INT(notifications, 6, "one notification per volume change");

    /* And the headphone buttons work. */
    for (int i = 0; i < 3; i++) {
        iaptest_tx_clear();
        IAPACC_SEND(0x02, 0x00, 0x00, 0x01);
        CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_PLAY,
                     "play/pause from the headphones");
        iaptest_button_sample(4);
        IAPACC_SEND(0x02, 0x00, 0x00, 0x00);
        iaptest_button_sample(4);
    }

    clean("the whole session");
}

/* ------------------------------------------------------------------ */
/* A car head unit                                                     */
/* ------------------------------------------------------------------ */

/* Browses the database over the Extended Interface lingo and keeps a
 * display in step. Its whole value is the metadata, so every reply that
 * carries text or an index is checked. */
void test_integration_car_head_unit(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03, 0x04 };

    iapacc_attach();

    iapacc_identify_idps(lingoes, sizeof(lingoes));
    if (!clean("identification"))
        return;
    iaptest_force_authenticated();

    /* Enter Extended Interface. */
    if (!STEP("enter Extended Interface", 0x00, 0x02, 0x00, 0x05))
        return;

    /* A conformant head unit registers notifications with the four-byte
     * mask (Table 5-43, p.425), which it must try before the one-byte
     * form. */
    if (!STEP("register play status notifications", 0x04, 0x0001,
              0x04, 0x00, 0x26, 0x00, 0x00, 0x1F, 0xFF))
        return;

    rbstub_set_playlist(30, 7);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* The state the dash shows. */
    if (!STEP("protocol version",   0x04, 0x0013, 0x04, 0x00, 0x12)) return;
    if (!STEP("device name",        0x04, 0x0015, 0x04, 0x00, 0x14)) return;
    if (!STEP("play status",        0x04, 0x001D, 0x04, 0x00, 0x1C)) return;
    if (!STEP("track count",        0x04, 0x0036, 0x04, 0x00, 0x35)) return;
    if (!STEP("shuffle",            0x04, 0x002D, 0x04, 0x00, 0x2C)) return;
    if (!STEP("repeat",             0x04, 0x0030, 0x04, 0x00, 0x2F)) return;

    /* The current track index has to be the one we set. */
    if (!STEP("current track index", 0x04, 0x001F, 0x04, 0x00, 0x1E))
        return;
    {
        const struct iaptest_pkt *p = reply(0x04, 0x001F);
        /* lingo, 2 command, 2 transaction, 4-byte index */
        CHECK(p->paylen == 9, "ReturnCurrentPlayingTrackIndex should be "
              "9 bytes, got %d", p->paylen);
        if (p->paylen == 9) {
            uint32_t idx = ((uint32_t)p->payload[5] << 24)
                         | ((uint32_t)p->payload[6] << 16)
                         | ((uint32_t)p->payload[7] << 8)
                         |  (uint32_t)p->payload[8];
            CHECK_EQ_INT(idx, 7, "reported track index");
        }
    }

    /* Titles for a screenful of tracks. */
    for (int i = 0; i < 5; i++) {
        unsigned char c[7] = { 0x04, 0x00, 0x20,
                               0x00, 0x00, 0x00, (unsigned char)i };
        iaptest_tx_clear();
        iapacc_send(c, sizeof(c));

        const struct iaptest_pkt *p = reply(0x04, 0x0021);
        CHECK(p != NULL, "no title for track %d", i);
        if (p)
            CHECK(p->paylen > 5,
                  "the title reply for track %d carries no string", i);
    }

    /* Tracks change underneath it; the dash must be told each time. */
    int notified = 0;
    for (int i = 0; i < 8; i++) {
        iaptest_tx_clear();
        rbstub_set_playlist(30, 7 + i);
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        /* The change is flagged on the audio thread and sent from
         * iap_periodic(); see iap_track_changed(). */
        iap_periodic();
        if (reply(0x04, 0x0027))
            notified++;
    }
    CHECK_EQ_INT(notified, 8,
                 "PlayStatusChangeNotification per track change; a head "
                 "unit that misses these shows a stale track forever");

    /* Transport control from the dash. The arms drive the Playback
     * Engine rather than raising a remote button, because outside the
     * WPS keymap-ipod.c turns BUTTON_RC_RIGHT into a browser cursor
     * move. */
    iaptest_tx_clear();
    rbstub_reset_calls();
    IAPACC_SEND(0x04, 0x00, 0x29, 0x03);
    CHECK_EQ_INT(rbstub_calls.skip, 1, "next track from the dash");
    CHECK_EQ_INT(rbstub_calls.last_skip, 1,
                 "next track from the dash went the wrong way");
    iaptest_button_sample(4);

    clean("the whole session");
}

/* ------------------------------------------------------------------ */
/* A legacy wired remote                                               */
/* ------------------------------------------------------------------ */

/* Identifies with IdentifyDeviceLingoes and never uses transaction IDs.
 * The point of this one is that everything the newer work changed must
 * be invisible to it. */
void test_integration_legacy_wired_remote(void)
{
    iapacc_attach();

    iapacc_identify_legacy(0x0000000D);      /* General, Simple, Display */
    CHECK(!iapacc_transactions_enabled(),
          "IdentifyDeviceLingoes must leave transaction IDs off");
    iaptest_force_authenticated();

    if (!STEP("device name",     0x00, 0x08, 0x00, 0x07)) return;
    if (!STEP("software version",0x00, 0x0A, 0x00, 0x09)) return;

    /* A legacy reply must carry no transaction ID at all. */
    {
        const struct iaptest_pkt *p = reply(0x00, 0x0A);
        EXPECT_PAYLOAD(0, 0x00, 0x0A, 2, 0, 3);
        (void)p;
    }

    /* Buttons, the only thing this class of accessory does. */
    /* Two state bytes. Play/pause and the menu cluster are in the
     * first; the transport controls are in the second, which the
     * handler reads at buf[4 + doff]. */
    static const struct { int byte; unsigned char bits; unsigned long btn;
                          const char *name; } presses[] = {
        { 0, 0x01, BUTTON_RC_PLAY,  "play/pause"   },
        { 1, 0x10, BUTTON_RC_RIGHT, "fast forward" },
        { 1, 0x20, BUTTON_RC_LEFT,  "rewind"       },
    };

    for (unsigned i = 0; i < sizeof(presses)/sizeof(presses[0]); i++) {
        unsigned char down[5] = { 0x02, 0x00, 0x00, 0x00, 0x00 };
        unsigned char up[5]   = { 0x02, 0x00, 0x00, 0x00, 0x00 };
        down[3 + presses[i].byte] = presses[i].bits;

        iaptest_tx_clear();
        iapacc_send(down, sizeof(down));
        CHECK_EQ_INT(iap_remotebtn, presses[i].btn, presses[i].name);
        CHECK_EQ_INT(iaptest_tx_count(), 0,
                     "ContextButtonStatus takes no reply (MFi 4.2.7, "
                     "p.226), but the device sent one");
        iaptest_button_sample(4);

        iapacc_send(up, sizeof(up));
        iaptest_button_sample(4);
    }

    clean("the whole session");
}

/* ------------------------------------------------------------------ */
/* Two accessories in a row                                            */
/* ------------------------------------------------------------------ */

/* MFi 2.6.1.2 (p.111) describes this explicitly: transaction IDs are
 * disabled before IdentifyDeviceLingoes and enabled again before a
 * later StartIDPS. Whichever order they arrive in, neither may inherit
 * the other's state. */
void test_integration_accessory_swap(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03, 0x04 };

    /* IDPS first. */
    iapacc_attach();
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    iaptest_force_authenticated();
    if (!STEP("first accessory name", 0x00, 0x08, 0x00, 0x07))
        return;
    if (!clean("the IDPS accessory"))
        return;

    /* Then a legacy one. */
    iapacc_reset();
    iapacc_attach();
    iapacc_identify_legacy(0x0000000D);
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPACC_SEND(0x00, 0x07);
    {
        const struct iaptest_pkt *p = reply(0x00, 0x08);
        CHECK(p != NULL, "no name for the legacy accessory");
        if (p)
            CHECK_EQ_INT(p->paylen, 10,
                         "the legacy reply must be 10 bytes; longer means "
                         "the first accessory's transaction-ID state "
                         "survived the swap");
    }
    if (!clean("the legacy accessory"))
        return;

    /* And back to IDPS, which 2.6.1.2 says re-enables them. */
    iapacc_reset();
    iapacc_attach();
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPACC_SEND(0x00, 0x07);
    {
        const struct iaptest_pkt *p = reply(0x00, 0x08);
        CHECK(p != NULL, "no name for the third accessory");
        if (p)
            CHECK_EQ_INT(p->paylen, 12,
                         "back in IDPS the reply must carry a transaction "
                         "ID again");
    }
    clean("the third accessory");
}

/* ------------------------------------------------------------------ */
/* Capability discovery in the order the spec prescribes               */
/* ------------------------------------------------------------------ */

/* 2.3.3 (p.97): "This is done by calling GetiPodOptionsForLingo after
 * StartIDPS." Table 3-133 runs the whole sweep before SetFIDTokenValues,
 * so every one of these arrives inside the IDPS window, where
 * device.auth.idps is still false. */
void test_integration_capability_sweep_in_idps_window(void)
{
    iapacc_attach();

    IAPACC_SEND(0x00, 0x38);                  /* StartIDPS, nothing after */

    static const unsigned char lingoes[] =
        { 0x00, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0A };

    for (unsigned i = 0; i < sizeof(lingoes); i++) {
        unsigned char c[3] = { 0x00, 0x4B, lingoes[i] };
        char what[48];

        iaptest_tx_clear();
        iapacc_send(c, sizeof(c));
        snprintf(what, sizeof(what), "options for lingo 0x%02X", lingoes[i]);

        const struct iaptest_pkt *p = reply(0x00, 0x4C);

        /* A lingo this build does not have must be refused instead.
         * MFi 3.3.55 (p.191): "If the accessory requests options for
         * any other lingo and the Apple device returns a nonzero
         * iPodAck, that lingo is not listed in Table 3-132 (page 192)
         * or is not supported by the Apple device on the port being
         * used; no RetiPodOptionsForLingo command will be returned."
         * The RF Tuner lingo is 5G-only, so this is the one entry that
         * differs between the two targets. */
        if (lingoes[i] >= 32 || !LINGO_SUPPORTED(lingoes[i])) {
            CHECK(p == NULL,
                  "%s: answered for a lingo this device does not have",
                  what);
            const struct iaptest_pkt *nak = reply(0x00, 0x02);
            CHECK(nak != NULL && nak->paylen >= 2
                  && nak->payload[nak->paylen - 2] != 0x00,
                  "%s: must draw a nonzero iPodAck", what);
            continue;
        }

        if (!p) {
            CHECK(false, "%s: no RetiPodOptionsForLingo", what);
            continue;
        }
        CHECK_EQ_INT(p->paylen, 13, "RetiPodOptionsForLingo length");
        if (p->paylen == 13)
            CHECK_EQ_INT(p->payload[4], lingoes[i],
                         "the reply must echo the lingo asked about");
    }

    /* Only then does it identify. */
    unsigned char tok[24];
    int n = 0;
    tok[n++] = 0x01;
    tok[n++] = (unsigned char)(3 + 4 + 8);
    tok[n++] = 0x00; tok[n++] = 0x00;
    tok[n++] = 0x04;
    tok[n++] = 0x00; tok[n++] = 0x02; tok[n++] = 0x03; tok[n++] = 0x04;
    memset(tok + n, 0, 8); n += 8;
    {
        unsigned char c[32];
        c[0] = 0x00; c[1] = 0x39;
        memcpy(c + 2, tok, n);
        iaptest_tx_clear();
        iapacc_send(c, 2 + n);
        CHECK(reply(0x00, 0x3A) != NULL,
              "no AckFIDTokenValues for a well-formed IdentifyToken");
    }

    IAPACC_SEND(0x00, 0x3B, 0x00);
    CHECK(device.auth.idps, "EndIDPS did not complete identification");

    clean("the capability sweep");
}

/* ------------------------------------------------------------------ */
/* The authentication handshake, driven end to end                     */
/* ------------------------------------------------------------------ */

/* Every other case forces the authenticated state, because the signature
 * needs a coprocessor this build does not have. That leaves the device's
 * own half of MFi 2.4 (p.103-106) untested: the certificate exchange, the
 * challenge, and the acknowledgements it owes at each step.
 *
 * Here the accessory model answers for itself, so the whole conversation
 * runs. What it cannot do is produce a signature the device would verify;
 * the device has no verifier either, so the flow completes on
 * well-formedness alone. The assertions are about the state machine and
 * the packets, not about cryptography.
 */
void test_integration_authentication_handshake(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03 };

    iapacc_attach();
    iapacc_autorespond(true);

    iapacc_identify_idps(lingoes, sizeof(lingoes));
    if (!clean("identification"))
        return;

    CHECK(device.auth.state != AUST_AUTH,
          "the device considered the accessory authenticated before the "
          "handshake began");

    /* The device opens the handshake itself, but only from the periodic
     * tick: iap_periodic() sends GetAccessoryAuthenticationInfo when the
     * state is AUST_INIT (iap-core.c:1039-1046). Nothing in the packet
     * path does it. An earlier version of this case never called the
     * tick, so the responder below was never asked anything and the
     * whole exchange it claims to drive did not happen -- a mutation
     * audit caught it by disabling the responder and seeing no failure.
     */
    iaptest_tx_clear();
    iap_periodic();

    {
        const struct iaptest_pkt *p = reply(0x00, 0x14);
        CHECK(p != NULL,
              "the device never asked for the accessory's authentication "
              "version (MFi 3.3.17, p.136)");
        if (!p)
            return;
    }

    /* Let the accessory answer, and keep answering until the exchange
     * settles. Each reply may prompt another question. */
    int rounds = 0;
    while (iapacc_pump() > 0 && ++rounds < 8)
        ;

    CHECK(iapacc_responses_sent() > 0,
          "the accessory model answered nothing, so this case is not "
          "driving a handshake at all");
    CHECK(rounds < 8, "the handshake did not settle in 8 rounds");

    if (!clean("authentication"))
        return;

    CHECK(DEVICE_AUTHENTICATED,
          "the handshake completed but the accessory is not "
          "authenticated, state is %d", (int)device.auth.state);

    /* And now it may use the lingoes it declared. Before authentication
     * a Display Remote command is refused; after it, it works. That is
     * the whole point of the handshake. */
    iaptest_tx_clear();
    IAPACC_SEND(0x03, 0x0C, 0x04);          /* GetiPodStateInfo, Mute/UI */
    {
        const struct iaptest_pkt *p = reply(0x03, 0x0D);
        CHECK(p != NULL,
              "GetiPodStateInfo refused after a completed handshake");
    }

    clean("the whole session");
}

/* An accessory declaring an authentication version the device does not
 * implement must be refused, and refused in a way it can act on. MFi
 * 3.3.18 (p.136) allows only 1.0 and 2.0. */
void test_integration_authentication_bad_version(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03 };

    iapacc_attach();
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    if (!clean("identification"))
        return;

    iaptest_tx_clear();
    iap_periodic();
    const struct iaptest_pkt *request = reply(0x00, 0x14);
    CHECK(request != NULL && request->paylen >= 4,
          "no authentication-info request before the bad version");
    if (!request || request->paylen < 4)
        return;

    unsigned char bad_version[6] = {
        0x00, 0x15, request->payload[2], request->payload[3], 0x03, 0x00
    };
    iaptest_tx_clear();
    iaptest_rx(bad_version, sizeof(bad_version));

    const struct iaptest_pkt *p = reply(0x00, 0x16);
    CHECK(p != NULL, "no AckAccessoryAuthenticationInfo for a bad version");
    if (p && p->paylen >= 5) {
        CHECK_EQ_INT(p->payload[4], 0x08,
                     "AckAccessoryAuthenticationInfo status for an "
                     "unsupported version");
    }
    CHECK(!DEVICE_AUTHENTICATED,
          "an unsupported authentication version left the accessory "
          "authenticated");

    /* The refusal must not have cost the session its transaction IDs:
     * the reply itself has to carry one, and so does everything after.
     * Losing them here shifted every later payload by two bytes. */
    if (p) {
        CHECK_EQ_INT(p->paylen, 5,
                     "the refusal should be lingo, command, transaction "
                     "ID and status");
    }
    CHECK(DEVICE_TRANSID_ACTIVE,
          "the refusal tore down transaction IDs mid-session");

    iaptest_tx_clear();
    IAPACC_SEND(0x00, 0x27, 0x00);          /* GetAccessoryInfo-ish probe */
    clean("after the refusal");
}

/* ------------------------------------------------------------------ */
/* Identifying with a device ID, which asks for authentication         */
/* ------------------------------------------------------------------ */

/* MFi Table 3-25 (p.135), IdentifyDeviceLingoes Options bits:
 *
 *   "1:0  Authentication control bits. These bits have the following
 *    meanings: 00 = no authentication is supported or required;
 *    01 = defer authentication until an authenticated command is used
 *    (Authentication 1.0 only); 10 = authenticate immediately after
 *    identification (required for Authentication 2.0)."
 *
 * and Table 3-23 (p.134) on the Device ID field: "If an accessory does
 * not require authentication, it can send a Device ID of 0x00000000 and
 * set the authentication option bits to 0x0."
 *
 * Every other case in this suite identifies with both fields zero,
 * which takes the branch that grants access on the spot. This is the
 * other branch. Until it was written nothing drove it, so nothing
 * checked that an accessory asking to be authenticated is in fact made
 * to authenticate before its commands are honoured.
 */
static void identify_with_auth(unsigned long mask, unsigned long options,
                               unsigned long deviceid)
{
    unsigned char c[14] = { 0x00, 0x13 };
    c[2] = (mask >> 24) & 0xFF;  c[3] = (mask >> 16) & 0xFF;
    c[4] = (mask >>  8) & 0xFF;  c[5] = mask & 0xFF;
    c[6] = (options >> 24) & 0xFF;  c[7] = (options >> 16) & 0xFF;
    c[8] = (options >>  8) & 0xFF;  c[9] = options & 0xFF;
    c[10] = (deviceid >> 24) & 0xFF; c[11] = (deviceid >> 16) & 0xFF;
    c[12] = (deviceid >>  8) & 0xFF; c[13] = deviceid & 0xFF;
    iapacc_send(c, sizeof(c));
}

void test_integration_identify_requesting_authentication(void)
{
    iapacc_attach();
    iapacc_autorespond(true);

    /* Options 0x02: authenticate immediately. Device ID non-zero, as
     * Table 3-23 requires when the option bits are not 0x0. */
    identify_with_auth(0x0000000D, 0x00000002, 0x00000003);
    if (!clean("identification")) return;

    CHECK(!DEVICE_AUTHENTICATED,
          "an accessory that asked to be authenticated was treated as "
          "authenticated the moment it identified");

    /* An authenticated command must be refused, and refused for that
     * reason -- not for its parameters. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00);
    {
        const struct iaptest_pkt *p = reply(0x03, 0x00);
        CHECK(p != NULL, "GetiPodStateInfo drew no Display Remote ack");
        if (p && p->paylen >= 4)
            CHECK_EQ_INT(p->payload[2], IAP_ACK_NO_AUTHEN,
                         "an authenticated command before the handshake "
                         "must be refused as Not Authenticated");
    }

    /* Now let the handshake run. The device opens it from the periodic
     * tick, not from the packet path. */
    iaptest_tx_clear();
    iap_periodic();
    {
        const struct iaptest_pkt *p = reply(0x00, 0x14);
        CHECK(p != NULL,
              "the device never asked for the accessory's authentication "
              "version, so the identify left the handshake unstarted");
    }
    int rounds = 0;
    while (iapacc_pump() > 0 && ++rounds < 8)
        ;
    CHECK(rounds < 8, "the handshake did not settle in 8 rounds");
    CHECK(DEVICE_AUTHENTICATED,
          "the handshake completed but the accessory is still not "
          "authenticated");

    /* And the same command now works. */
    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x00);
    {
        const struct iaptest_pkt *p = reply(0x03, 0x0D);
        CHECK(p != NULL,
              "GetiPodStateInfo was still not answered after the "
              "handshake completed");
    }

    clean("the whole session");
}

/* ------------------------------------------------------------------ */
/* The Apple Radio Remote                                              */
/* ------------------------------------------------------------------ */

#if CONFIG_TUNER
/* Byte for byte what the real accessory sends. iap-lingo0.c:797 records
 * it from the wire:
 *
 *   FF 55 0E 00 13 00 00 00 8D 00 00 00 0E 00 00 00 03
 *
 * -- lingoes 0x8D (General, Simple Remote, Display Remote, RF Tuner),
 * options 0x0000000E, device ID 0x00000003.
 *
 * The options word is worth a note: MFi Table 3-25 (p.135) gives bits
 * 1:0 as the authentication control bits and says of the rest "31:2
 * Reserved; set to 0". 0x0E has bits 3 and 2 set, so Apple's own
 * accessory does not follow that. Bits 1:0 are 10, "authenticate
 * immediately after identification", which is what this firmware acts
 * on -- it masks with 0x03 and ignores the rest, which is the right
 * thing to do with reserved bits it did not set.
 *
 * The tuner has fifteen cases of its own. What this adds is the
 * session: that this identify is accepted at all, that it turns the
 * radio on, that the RF Tuner lingo is refused until the handshake it
 * asked for has finished, and that the whole sequence leaves nothing
 * malformed behind.
 */
void test_integration_apple_radio_remote(void)
{
    iapacc_attach();
    iapacc_autorespond(true);

    radio_present = 0;
    identify_with_auth(0x0000008D, 0x0000000E, 0x00000003);
    if (!clean("identification")) return;

    CHECK(device.lingoes & (1u << 0x07),
          "the RF Tuner lingo was not negotiated (device.lingoes = "
          "0x%08X)", device.lingoes);
    CHECK(radio_present,
          "identifying with the RF Tuner lingo did not mark the radio "
          "present, so nothing downstream will offer it");

    /* It asked to be authenticated, so it is not yet. The RF Tuner
     * lingo requires authentication like every other. */
    CHECK(!DEVICE_AUTHENTICATED,
          "the remote asked to be authenticated and was granted access "
          "without it");
    iaptest_tx_clear();
    IAPTEST_RX(0x07, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00);
    CHECK(reply(0x07, 0x08) == NULL,
          "RetTunerCaps was acted on before the handshake the remote "
          "asked for had finished");

    /* Run the handshake the identify asked for. */
    iaptest_tx_clear();
    iap_periodic();
    {
        const struct iaptest_pkt *p = reply(0x00, 0x14);
        CHECK(p != NULL,
              "the device never opened the authentication handshake the "
              "remote asked for");
    }
    int rounds = 0;
    while (iapacc_pump() > 0 && ++rounds < 8)
        ;
    CHECK(rounds < 8, "the handshake did not settle in 8 rounds");
    CHECK(DEVICE_AUTHENTICATED, "the handshake did not authenticate");

    /* Now the tuner. Capabilities first: AM, both FM bands and wide. */
    iaptest_tx_clear();
    IAPTEST_RX(0x07, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00);
    {
        const struct iaptest_pkt *p = reply(0x07, 0x08);
        CHECK(p != NULL,
              "RetTunerCaps drew no SetTunerBand, so the device never "
              "chose a band for a remote that advertises four");
    }

    /* And the buttons this remote actually has, on the Simple Remote
     * lingo it also declared. Table 4-14 (p.227): byte 1 bit 0 is
     * Play/Resume, byte 0 bits 3 and 4 are next and previous track --
     * which on the radio are station up and down. */
    static const struct { int byte; unsigned char bits; unsigned long btn;
                          const char *name; } presses[] = {
        { 1, 0x01, BUTTON_RC_PLAY,  "play/pause"    },
        { 0, 0x08, BUTTON_RC_RIGHT, "station up"    },
        { 0, 0x10, BUTTON_RC_LEFT,  "station down"  },
        { 0, 0x02, BUTTON_RC_VOL_UP,   "volume up"   },
        { 0, 0x04, BUTTON_RC_VOL_DOWN, "volume down" },
    };
    for (unsigned i = 0; i < sizeof(presses)/sizeof(presses[0]); i++) {
        unsigned char down[5] = { 0x02, 0x00, 0x00, 0x00, 0x00 };
        unsigned char up[5]   = { 0x02, 0x00, 0x00, 0x00, 0x00 };
        down[2 + presses[i].byte] = presses[i].bits;

        iap_remotebtn = BUTTON_NONE;
        iapacc_send(down, sizeof(down));
        CHECK_EQ_INT(iap_remotebtn, presses[i].btn, presses[i].name);
        iaptest_button_sample(4);
        iapacc_send(up, sizeof(up));
        iaptest_button_sample(4);
    }

    clean("the whole session");
}
#endif /* CONFIG_TUNER */
