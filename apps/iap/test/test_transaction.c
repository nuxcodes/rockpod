/***************************************************************************
 * Transaction-ID conformance.
 *
 * MFi R46 section 2.6, p.110:
 *   "Once an accessory has started the IDPS process, beginning with and
 *    including the StartIDPS command, the accessory must include 16-bit
 *    transaction IDs in every iAP packet."
 *
 * MFi R46 section 2.6.1.4, p.112:
 *   "When transaction IDs are enabled, all iAP commands must use them. If
 *    the description of the command in this specification does not include
 *    transaction ID fields, the developer must add them after the Command
 *    ID field in every command packet, and every packet length byte must
 *    be increased by 2."
 *
 * The only exemptions are RequestIdentify (0x00/0x00), Identify (0x00/0x01)
 * and IdentifyDeviceLingoes (0x00/0x13).
 *
 * The oandrew/ipod Go reference agrees: cmd.go:99 writes a transaction on
 * every command once TrxEnabled, with no per-command exemption, and
 * cmd.go:69-80 toggles TrxEnabled on exactly those three command IDs.
 *
 * Responses echo the accessory's ID (cmd.go:183 Respond). Commands the
 * Apple device originates use its own counter starting at 0x0001
 * (cmd.go:192 TrxNext; spec Table 2-12 step 11 and Table 2-15 step 9).
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include <stdio.h>

#include "config.h"
#include "appevents.h"
#include "iap-core.h"

/* ------------------------------------------------------------------ */
/* Baseline: a legacy accessory must NOT receive transaction IDs        */
/* ------------------------------------------------------------------ */

void test_transid_absent_for_legacy_accessory(void)
{
    iaptest_identify_legacy(0x0000000D);   /* lingoes 0x00, 0x02, 0x03 */

    /* RequestiPodName (0x00/0x07), no transaction ID. */
    IAPTEST_RX(0x00, 0x07);

    /* ReturniPodName (0x00/0x08) carrying the name and its NUL. */
    EXPECT_PAYLOAD(0, 0x00, 0x08, 'R','O','C','K','B','O','X', 0x00);
}

/* ------------------------------------------------------------------ */
/* Responses must echo the accessory's transaction ID                   */
/* ------------------------------------------------------------------ */

void test_transid_echoed_on_returnipodname(void)
{
    iaptest_enter_idps();

    /* RequestiPodName with transaction 0xBEEF. */
    IAPTEST_RX(0x00, 0x07, 0xBE, 0xEF);

    EXPECT_PAYLOAD(0, 0x00, 0x08, 0xBE, 0xEF,
                   'R','O','C','K','B','O','X', 0x00);
}

void test_transid_echoed_on_returnipodsoftwareversion(void)
{
    iaptest_enter_idps();

    /* RequestiPodSoftwareVersion (0x00/0x09), transaction 0x0123. */
    IAPTEST_RX(0x00, 0x09, 0x01, 0x23);

    /* ReturniPodSoftwareVersion (0x00/0x0A): major, minor, revision.
     * IAP_IPOD_FIRMWARE_* in apps/iap/iap-core.h is 2.0.3. */
    EXPECT_PAYLOAD(0, 0x00, 0x0A, 0x01, 0x23, 2, 0, 3);
}

void test_transid_echoed_on_returnipodserialnum(void)
{
    iaptest_enter_idps();

    /* RequestiPodSerialNum (0x00/0x0B), transaction 0x0007. */
    IAPTEST_RX(0x00, 0x0B, 0x9C, 0x07);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to RequestiPodSerialNum");
    if (!p || p->paylen < 4)
        return;

    CHECK_EQ_INT(p->payload[0], 0x00, "reply lingo");
    CHECK_EQ_INT(p->payload[1], 0x0C, "reply command (ReturniPodSerialNum)");
    CHECK_EQ_INT(p->payload[2], 0x9C, "transaction ID high byte");
    CHECK_EQ_INT(p->payload[3], 0x07, "transaction ID low byte");
}

void test_transid_echoed_on_lingo3_reply(void)
{
    /* Display Remote is already correct; this guards against regression
     * while the other lingoes are being fixed. */
    iaptest_enter_idps();

    /* GetNumEQProfiles (0x03/0x04), transaction 0x0042. */
    IAPTEST_RX(0x03, 0x04, 0xD3, 0x42);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to GetNumEQProfiles");
    if (!p || p->paylen < 4)
        return;

    CHECK_EQ_INT(p->payload[0], 0x03, "reply lingo");
    CHECK_EQ_INT(p->payload[1], 0x05, "reply command (RetNumEQProfiles)");
    CHECK_EQ_INT(p->payload[2], 0xD3, "transaction ID high byte");
    CHECK_EQ_INT(p->payload[3], 0x42, "transaction ID low byte");
}

/* ------------------------------------------------------------------ */
/* Unsolicited notifications must carry the device's own counter        */
/* ------------------------------------------------------------------ */

void test_transid_on_track_change_notification(void)
{
    /* An Extended Interface accessory that enabled play-status
     * notifications. This is the path a car head unit uses to keep its
     * display in step with the iPod. */
    iaptest_enter_idps();

    /* Negotiate Extended Interface: EnterRemoteUIMode (0x00/0x05). */
    IAPTEST_RX(0x00, 0x05, 0x00, 0x10);
    /* SetPlayStatusChangeNotification (0x04/0x0026) enabled. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x11, 0x01);
    iaptest_tx_clear();

    rbstub_set_playlist(10, 3);
    /* The event handler flags the change and iap_periodic() sends it:
     * send_event() runs handlers on the caller's thread, which for a
     * track change is the audio thread, and building a packet there
     * corrupts whatever the iAP thread is assembling. So each change
     * needs a tick, and each tick has to follow the change it reports
     * -- two changes in one tick coalesce into one notification, which
     * is correct but not what this case is measuring. */
    /* Drain the housekeeping ticks first. The notification now leaves
     * on a tick, and so does the authentication traffic, so without
     * this the assertion below lands on a GetDevAuthenticationInfo. */
    for (int t = 0; t < 4; t++)
        iap_periodic();
    iaptest_tx_clear();

    bool fired = rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    CHECK(fired, "iAP layer did not subscribe to PLAYBACK_EVENT_TRACK_CHANGE");

    /* The handler must not transmit. firmware/events.c send_event()
     * runs handlers synchronously on the caller's thread, and a track
     * change is fired from the audio thread (apps/playback.c), so
     * anything built here writes iap_txnext and the TX payload out from
     * under whatever the iAP thread is assembling -- the accessory gets
     * one packet made of half a notification and the tail of the reply
     * it was waiting for.
     *
     * A single-threaded harness cannot observe the race itself. It can
     * observe the rule: this callback sends nothing, and the packet
     * appears on the next tick. */
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "the track-change event handler transmitted from the "
                 "caller's thread instead of flagging the change");
    /* More than one: iap_periodic() returns early on the ticks it uses
     * for authentication and accessory-info housekeeping. */
    for (int t = 0; t < 4; t++)
        iap_periodic();

    if (iaptest_tx_count() == 0) {
        /* Nothing sent at all is a separate defect from a malformed
         * packet; report it plainly rather than as a byte mismatch. */
        CHECK(false, "no PlayStatusChangeNotification sent on track change");
        return;
    }

    /* Extended Interface uses a 2-byte command ID, so the transaction
     * goes at payload[3..4] (MFi Table 2-10, and iap_send_reply()'s own
     * hdr==3 rule for lingo 0x04). */
    /* 3 command bytes + 2 transaction ID + 1 notification type + a
     * 4-byte track index. Asserting ">= 8" was useless: 8 is exactly the
     * length WITHOUT the transaction ID, so removing the ID entirely
     * still satisfied it. */
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x27,
                   0x00, 0x02,        /* second device-originated ID:
                                       * the drain above flushed a
                                       * GetDevAuthenticationInfo, which
                                       * took the first */
                   0x01,              /* notification type: track index */
                   0x00, 0x00, 0x00, 0x03);
}

/* ------------------------------------------------------------------ */
/* The device counter must not be reused or run backwards               */
/* ------------------------------------------------------------------ */

void test_device_transid_counter_advances(void)
{
    iaptest_enter_idps();

    /* Two device-originated commands must not share an ID.
     * Spec Table 2-15 steps 9-10: 0x0001 then 0x0002. */
    IAPTEST_RX(0x00, 0x05, 0x00, 0x10);
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x11, 0x01);
    iaptest_tx_clear();

    /* The event handler flags the change and iap_periodic() sends it:
     * send_event() runs handlers on the caller's thread, which for a
     * track change is the audio thread, and building a packet there
     * corrupts whatever the iAP thread is assembling. So each change
     * needs a tick, and each tick has to follow the change it reports
     * -- two changes in one tick coalesce into one notification, which
     * is correct but not what this case is measuring. */
    for (int t = 0; t < 4; t++)     /* drain the housekeeping ticks */
        iap_periodic();
    iaptest_tx_clear();

    rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    iap_periodic();
    rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    iap_periodic();

    if (iaptest_tx_count() < 2) {
        CHECK(false, "expected two notifications, got %d",
              iaptest_tx_count());
        return;
    }

    const struct iaptest_pkt *a = iaptest_tx(0);
    const struct iaptest_pkt *b = iaptest_tx(1);
    if (a->paylen < 5 || b->paylen < 5)
        return;

    int ta = (a->payload[3] << 8) | a->payload[4];
    int tb = (b->payload[3] << 8) | b->payload[4];
    CHECK(ta != tb,
          "two consecutive notifications reused transaction ID 0x%04X", ta);
}

/* ------------------------------------------------------------------ */
/* Rejections must be addressed to the command that caused them        */
/* ------------------------------------------------------------------ */

/* An accessory that did not declare a lingo may still probe it. The
 * rejection is an iPodAck like any other and must echo the transaction
 * ID, or MFi 2.6.1.1 obliges the accessory to ignore it and retry. */
void test_transid_echoed_on_lingo_rejection(void)
{
    /* Identify through IDPS declaring only General and Digital Audio,
     * so Display Remote has not been negotiated. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x0D, 0x00, 0x00,
               0x02, 0x00, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);
    iaptest_tx_clear();

    CHECK(!(device.lingoes & (1u << 0x03)),
          "precondition: lingo 0x03 must not be negotiated for this case");

    /* Probe a Display Remote command with transaction 0x0099. */
    IAPTEST_RX(0x03, 0x04, 0x6B, 0x99);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no rejection sent for an unnegotiated lingo");
    if (!p || p->paylen < 4)
        return;

    CHECK_EQ_INT(p->payload[0], 0x03, "rejection lingo");
    CHECK_EQ_INT(p->payload[1], 0x00, "rejection is an iPodAck");
    CHECK_EQ_INT(p->payload[2], 0x6B, "transaction ID high byte");
    CHECK_EQ_INT(p->payload[3], 0x99, "transaction ID low byte");
}

/* ------------------------------------------------------------------ */
/* Never emit the ack that turns transaction IDs off                   */
/* ------------------------------------------------------------------ */

/* MFi 2.6.1.2 (p.111): "Support for transaction IDs must be disabled
 * upon receipt of a General lingo iPodAck command without a transaction
 * ID. Such commands have a payload length value (byte 2) of either 0x04
 * or 0x08."
 *
 * So a four-byte General iPodAck is not merely malformed while IDPS is
 * running, it is an instruction to stop using transaction IDs. If we
 * send one by accident the accessory obeys while we keep parsing with
 * the two-byte offset, and the link desynchronises for good. */
void test_no_ack_tears_down_transaction_ids(void)
{
    iaptest_enter_idps();

    /* A truncated General packet: lingo, command, one stray byte, so
     * there is no room for a transaction ID. The command is not
     * implemented, so it falls through to the rejection ack. */
    IAPTEST_RX(0x00, 0x77, 0xAA);

    const struct iaptest_pkt *p = iaptest_tx(0);
    if (!p)
        return;         /* saying nothing at all is also safe */

    if (p->payload[0] == 0x00 && p->payload[1] == 0x02) {
        CHECK(p->paylen != 4 && p->paylen != 8,
              "General iPodAck went out with a %d byte payload while IDPS "
              "was active; MFi 2.6.1.2 makes that an instruction to the "
              "accessory to stop sending transaction IDs", p->paylen);
    }
}

/* The same must hold for a truncated packet on a command that does have
 * a handler. */
void test_no_teardown_ack_on_short_known_command(void)
{
    iaptest_enter_idps();

    /* SetUIMode (0x00/0x37) with no room for its transaction ID. */
    IAPTEST_RX(0x00, 0x37, 0x01);

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x00 && p->payload[1] == 0x02)
            CHECK(p->paylen != 4 && p->paylen != 8,
                  "packet %d is a %d byte General iPodAck, which disables "
                  "transaction IDs on the accessory", i, p->paylen);
    }
}

/* MFi 2.6 (p.110): transaction IDs are mandatory "beginning with and
 * including the StartIDPS command". device.auth.idps is only set at
 * EndIDPS, so the whole handshake window sat outside the guard against
 * emitting the four-byte ack that turns transaction IDs back off. An
 * accessory that trips a length check mid-handshake would disable them,
 * then EndIDPS would enable our side, and every later packet parses two
 * bytes off. */
void test_no_teardown_ack_during_the_idps_handshake(void)
{
    /* StartIDPS only: mid-handshake, before EndIDPS. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    iaptest_tx_clear();

    /* Anything that reaches an ack from here. */
    static const unsigned char probes[][3] = {
        { 0x00, 0x42, 0x00 },   /* unimplemented, truncated */
        { 0x00, 0x38, 0x00 },   /* truncated StartIDPS */
        { 0x00, 0x39, 0x00 },   /* truncated SetFIDTokenValues */
        { 0x00, 0x3B, 0x00 },   /* truncated EndIDPS */
    };

    for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        iaptest_tx_clear();
        iaptest_rx(probes[i], 3);

        for (int k = 0; k < iaptest_tx_count(); k++) {
            const struct iaptest_pkt *p = iaptest_tx(k);
            if (p->paylen >= 2 && p->payload[0] == 0x00
                && p->payload[1] == 0x02)
                CHECK(p->paylen != 4 && p->paylen != 8,
                      "probe %u produced a %d byte General iPodAck during "
                      "the IDPS handshake, which MFi 2.6.1.2 makes an "
                      "instruction to stop using transaction IDs",
                      i, p->paylen);
        }
    }
}

/* SetEventNotification hand-rolled its acknowledgement and gated the
 * transaction ID on device.auth.idps, which is not set until EndIDPS.
 * Inside the IDPS window that produced the four-byte General iPodAck
 * that MFi 2.6.1.2 defines as "stop using transaction IDs". */
void test_seteventnotification_ack_is_not_a_teardown(void)
{
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);     /* StartIDPS only */
    iaptest_tx_clear();

    IAPTEST_RX(0x00, 0x49, 0x00, 0x02,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no acknowledgement for SetEventNotification");
    if (!p)
        return;

    CHECK(p->paylen != 4 && p->paylen != 8,
          "a %d byte General iPodAck tells the accessory to stop using "
          "transaction IDs (MFi 2.6.1.2)", p->paylen);
    /* Status must be Bad Parameter, not Success. MFi 1.4.1 (p.48): a
     * successful ack "means that it supports Flow Control notifications
     * and the accessory should be prepared to handle them", and this
     * firmware never sends iPodNotification at all. */
    EXPECT_PAYLOAD(0, 0x00, 0x02, 0x00, 0x02, IAP_ACK_BAD_PARAM, 0x49);
}

/* ------------------------------------------------------------------ */
/* Lingoes with no Apple-device acknowledgement                        */
/* ------------------------------------------------------------------ */

/* Three lingoes define no acknowledgement the Apple device may send, so
 * the device must answer nothing at all rather than inventing one:
 *
 *   0x01 Microphone      C.5 (p.533): "the Apple device sends commands
 *                        to the accessory and the accessory responds
 *                        with data or AccessoryAck commands". Table
 *                        C-12 lists one ack, 0x04, Origin: Accessory.
 *   0x05 Accessory Power Table C-37 (p.548) has two commands, both
 *                        Origin: Apple device, and no ack either way.
 *   0x07 RF Tuner        4.7.5 (p.291): command 0x00 AccessoryAck is
 *                        "Lingo: 0x07 - Origin: Accessory".
 *
 * The Microphone helper used to emit a Display Remote iPodAck carrying a
 * Microphone command id, on a lingo the accessory had not negotiated.
 * The Accessory Power handler echoed the command straight back, so
 * receiving 0x05 0x02 made the device transmit BeginHighPower at the
 * accessory. */
static void expect_silence(const char *what,
                           const unsigned char *p, int len)
{
    iaptest_tx_clear();
    iaptest_rx(p, len);
    CHECK_EQ_INT(iaptest_tx_count(), 0, what);
}

void test_lingoes_without_an_ack_stay_silent(void)
{
    iaptest_detach_model_for_raw_probes();
    iaptest_identify_legacy(0x000000AF);   /* 0x00,0x01,0x02,0x03,0x05,0x07 */
    iaptest_force_authenticated();

    /* Accessory Power. Both commands are device-originated, so neither
     * should ever arrive, and neither may be answered. */
    { static const unsigned char p[] = { 0x05, 0x02 };
      expect_silence("Accessory Power 0x02 must not be echoed", p, sizeof(p)); }
    { static const unsigned char p[] = { 0x05, 0x03 };
      expect_silence("Accessory Power 0x03 must not be echoed", p, sizeof(p)); }
    { static const unsigned char p[] = { 0x05 };
      expect_silence("a one-byte lingo 5 frame must produce nothing",
                     p, sizeof(p)); }
    { static const unsigned char p[] = { 0x05, 0xFE, 0xAA };
      expect_silence("a reserved Accessory Power command must produce nothing",
                     p, sizeof(p)); }

    /* Microphone. Every path through the handler -- short packet,
     * unauthenticated, unimplemented command -- used to reach cmd_ack. */
    for (int len = 2; len <= 8; len++) {
        unsigned char p[8] = { 0x01, 0x07 };
        char what[72];
        for (int i = 2; i < len; i++)
            p[i] = (unsigned char)(0x5A ^ i);
        snprintf(what, sizeof(what),
                 "Microphone lingo must not acknowledge (%d bytes)", len);
        expect_silence(what, p, len);
    }
    { static const unsigned char p[] = { 0x01, 0xFD, 0x11 };
      expect_silence("an unimplemented Microphone command must produce nothing",
                     p, sizeof(p)); }
}

/* A rejection must be addressed to the command that caused it, in every
 * lingo. Lingo 3 was fixed for this; lingo 2 captured its id after the
 * length check, so a Simple Remote packet carrying an id but one byte
 * short of a full report was refused with 0x0000 -- matching no command
 * the accessory sent, which MFi 2.6.1.1 (p.111) obliges it to discard,
 * leaving it retrying for ever.
 *
 * Driven with AudioButtonStatus (0x04) rather than ContextButtonStatus
 * (0x00), because 0x00 is the one command in this lingo that must never
 * be answered at all: MFi 4.2.7 (p.226) says "the Apple device does not
 * return a packet to the accessory in response to this command", and
 * 4.2.8 (p.228) makes it the sole exception to the acknowledgement rule.
 * test_buttons_context_status_is_never_answered covers that side. */
void test_lingo2_rejection_echoes_the_transaction_id(void)
{
    iaptest_detach_model_for_raw_probes();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_tx_clear();

    /* Four bytes: carries the id, one short of a state byte. */
    IAPTEST_RX(0x02, 0x04, 0x7E, 0x11);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no rejection for a short AudioButtonStatus");
    if (!p || p->paylen < 4)
        return;

    CHECK_EQ_INT(p->payload[0], 0x02, "rejection lingo");
    CHECK_EQ_INT(p->payload[1], 0x01, "Simple Remote iPodAck");
    CHECK_EQ_INT(p->payload[2], 0x7E, "transaction ID high byte");
    CHECK_EQ_INT(p->payload[3], 0x11, "transaction ID low byte");
}

/* MFi 2.6.1.2 (p.111): "Support for transaction IDs must be disabled
 * before sending an IdentifyDeviceLingoes command." MFi 2.6.1.4
 * (p.112): "After sending one of these commands the accessory must
 * disable transaction ID support ... until it sends a StartIDPS
 * command."
 *
 * So the accessory has already stopped using them by the time one
 * arrives -- whether or not the command is well formed. The device
 * cleared its side in iap_reset_device(), at the end of the accepted
 * path only, so every rejection left it demanding IDs the accessory was
 * no longer sending. Every packet after that was parsed two bytes off,
 * in both directions, until the accessory was unplugged. */
void test_identify_rejection_still_disables_transaction_ids(void)
{
    /* A short Identify, inside an open IDPS process. */
    iaptest_detach_model_for_raw_probes();
    iaptest_enter_idps();
    CHECK(DEVICE_TRANSID_ACTIVE, "IDPS did not enable transaction IDs");
    IAPTEST_RX(0x00, 0x01);
    CHECK(!DEVICE_TRANSID_ACTIVE,
          "a truncated Identify left transaction IDs in force");

    /* A short IdentifyDeviceLingoes. */
    iaptest_enter_idps();
    IAPTEST_RX(0x00, 0x13, 0x00, 0x00);
    CHECK(!DEVICE_TRANSID_ACTIVE,
          "a truncated IdentifyDeviceLingoes left transaction IDs in "
          "force");

    /* And a well-formed one the parameter checks refuse: a device ID
     * with no authentication requested (iap-lingo0.c). */
    iaptest_enter_idps();
    IAPTEST_RX(0x00, 0x13,
               0x00, 0x00, 0x00, 0x1D,      /* lingoes */
               0x00, 0x00, 0x00, 0x00,      /* options: no auth */
               0x12, 0x34, 0x56, 0x78);     /* a device id anyway */
    CHECK(!DEVICE_TRANSID_ACTIVE,
          "a refused IdentifyDeviceLingoes left transaction IDs in "
          "force, so every later packet is parsed two bytes off");

    /* The accessory can still start again. */
    iaptest_enter_idps();
    CHECK(DEVICE_TRANSID_ACTIVE,
          "a fresh IDPS after a refusal did not re-enable transaction "
          "IDs");
}

/* One transaction ID for the whole authentication handshake.
 *
 * MFi p.111: "The Apple device does not increment transaction ID values
 * during authentication. The Accessory must continue to respond to
 * Apple device commands with the transaction ID included in the Apple
 * device's command."
 *
 * Both commands the device originates inside the handshake --
 * GetAccessoryAuthenticationInfo (0x00/0x14) and
 * GetAccessoryAuthenticationSignature (0x00/0x17) -- used the ordinary
 * advancing form, so the counter moved twice during a handshake it is
 * supposed to sit still through.
 *
 * Simply not advancing is the obvious reading and is wrong: the next
 * ordinary command would then reuse the handshake's ID, which the
 * accessory model rejects for the reason 2.6.1.1 gives. The ID is taken
 * from the counter once and held. */
void test_transid_authentication_holds_one_id(void)
{
    iaptest_enter_idps();
    iapacc_autorespond(true);

    device.auth.state = AUST_INIT;
    iaptest_tx_clear();
    iap_periodic();

    unsigned short auth_id = 0xFFFF;
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL && p->paylen >= 4 && p->payload[0] == 0x00
              && p->payload[1] == 0x14,
              "the handshake did not open with "
              "GetAccessoryAuthenticationInfo");
        if (p && p->paylen >= 4)
            auth_id = (p->payload[2] << 8) | p->payload[3];
    }

    /* Drive it to the signature command and check the ID is the same
     * one, not the next one. */
    int rounds = 0;
    unsigned short sig_id = 0xFFFF;
    bool saw_sig = false;
    while (rounds++ < 8 && !saw_sig) {
        iaptest_tx_clear();
        iapacc_pump();
        iap_periodic();
        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (p && p->paylen >= 4 && p->payload[0] == 0x00
                && p->payload[1] == 0x17) {
                sig_id = (p->payload[2] << 8) | p->payload[3];
                saw_sig = true;
            }
        }
    }
    CHECK(saw_sig,
          "the handshake never reached "
          "GetAccessoryAuthenticationSignature, so this case tests "
          "nothing");
    /* Absolute, not just equal to each other. Comparing the two reads
     * against one another passes if both lost their ID and both shifted
     * by the same two bytes, which is exactly the failure the check is
     * for. */
    CHECK(auth_id != 0,
          "the handshake's first command carried no transaction ID");
    if (saw_sig) {
        CHECK_EQ_INT(sig_id, auth_id,
                     "the transaction ID moved during authentication");
        CHECK(sig_id != 0,
              "the signature request carried no transaction ID");
    }

    /* And the first ordinary command after it does not reuse that ID --
     * which is what "hold" has to mean without breaking 2.6.1.1. */
    device.auth.state = AUST_CERTDONE;
    IAPTEST_RX(0x00, 0x05, 0x00, 0x40);            /* Extended Interface */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x41, 0x01);
    rbstub_set_playlist(10, 3);
    for (int t = 0; t < 4; t++)
        iap_periodic();

    iaptest_tx_clear();
    rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    for (int t = 0; t < 4; t++)
        iap_periodic();

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 5 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27) {
            unsigned short id = (p->payload[3] << 8) | p->payload[4];
            CHECK(id != auth_id,
                  "a notification after the handshake reused the "
                  "handshake's transaction ID (0x%04X)", id);
        }
    }
}

/* The certificate machine quotes transaction IDs on its error paths.
 *
 * DevAuthenticationInfo's version 2.00 branch answers a section index
 * that is not the one expected with a General iPodAck, and that ack has
 * to carry the transaction ID of the command it refuses -- MFi 2.6.1.1
 * (p.111) has the accessory discard a response whose ID matches no
 * command it sent, so a rejection it discards leaves it retrying the
 * same bad section for ever.
 *
 * The mutation sweep could not see any of this until it learned to
 * mutate DEVICE_TRANSID_ACTIVE, and then eight of these sites in
 * iap-lingo0.c turned out to be reachable only through error paths no
 * case drove. */
void test_transid_certificate_errors_quote_the_id(void)
{
    iaptest_enter_idps();

    /* Mid-handshake, expecting section 0. */
    device.auth.state = AUST_CERTREQ;
    device.auth.next_section = 0;
    device.auth.ipod_tid = 0x0055;

    iaptest_tx_clear();
    /* Version 2.00, section 5 of 1 -- not the section expected. */
    IAPTEST_RX(0x00, 0x15, 0x00, 0x55, 0x02, 0x00, 0x05, 0x01, 0xAA);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "an out-of-order certificate section drew no ack");
    if (r && r->paylen >= 6) {
        CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x02,
              "the refusal must be a General iPodAck");
        CHECK(r->payload[2] == 0x00 && r->payload[3] == 0x55,
              "the refusal must quote this packet's transaction ID "
              "(got 0x%02X%02X)", r->payload[2], r->payload[3]);
        CHECK_EQ_INT(r->payload[5], 0x15,
                     "the refusal must name DevAuthenticationInfo");
    }

    /* And the section in order is accepted, so the refusal above is
     * about the section and not about the packet. */
    device.auth.state = AUST_CERTREQ;
    device.auth.next_section = 0;
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x15, 0x00, 0x55, 0x02, 0x00, 0x00, 0x01, 0xAA);
    {
        const struct iaptest_pkt *q = iaptest_tx(0);
        CHECK(q != NULL && q->paylen >= 6,
              "an in-order certificate section drew no ack");
        if (q && q->paylen >= 6) {
            CHECK(q->payload[2] == 0x00 && q->payload[3] == 0x55,
                  "the acceptance must quote its own transaction ID");
            CHECK_EQ_INT(q->payload[4], 0x00,
                         "an in-order section must be accepted");
        }
    }
}

void test_transid_authentication_ignores_unmatched_responses(void)
{
    iaptest_enter_idps();
    current_tick = 100;
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();
    iap_periodic();

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen >= 4 && r->payload[1] == 0x14,
          "authentication did not start");
    if (!r || r->paylen < 4)
        return;

    uint16_t tid = ((uint16_t)r->payload[2] << 8) | r->payload[3];
    uint16_t wrong = tid + 1;
    long cert_deadline = device.auth.deadline;
    unsigned char cert[9] = { 0x00, 0x15, wrong >> 8, wrong,
                              0x02, 0x00, 0x00, 0x00, 0xAA };

    iaptest_tx_clear();
    iaptest_rx(cert, sizeof(cert));
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "an unmatched certificate response was answered");
    CHECK_EQ_INT(device.auth.state, AUST_CERTREQ,
                 "an unmatched certificate response advanced authentication");
    CHECK_EQ_INT(device.auth.deadline, cert_deadline,
                 "an unmatched certificate response changed the deadline");

    cert[2] = tid >> 8;
    cert[3] = tid;
    current_tick = cert_deadline;
    iaptest_rx(cert, sizeof(cert));
    CHECK_EQ_INT(device.auth.state, AUST_CERTDONE,
                 "a matched certificate response was not accepted");
    CHECK_EQ_INT(device.auth.deadline, 0,
                 "the accepted certificate left its deadline armed");

    iaptest_tx_clear();
    current_tick++;
    iap_periodic();
    r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen == 25 && r->payload[1] == 0x17,
          "the accepted certificate timed out before the challenge");
    if (!r || r->paylen < 4)
        return;
    CHECK_EQ_INT(((uint16_t)r->payload[2] << 8) | r->payload[3], tid,
                 "the challenge changed the authentication transaction ID");
    CHECK_EQ_INT(device.auth.deadline, current_tick + 75 * HZ,
                 "the Auth2 signature deadline");

    long signature_deadline = device.auth.deadline;
    unsigned char signature[5] = { 0x00, 0x18, wrong >> 8, wrong, 0xAA };
    iaptest_tx_clear();
    iaptest_rx(signature, sizeof(signature));
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "an unmatched signature response was answered");
    CHECK_EQ_INT(device.auth.state, AUST_CHASENT,
                 "an unmatched signature response authenticated");
    CHECK_EQ_INT(device.auth.deadline, signature_deadline,
                 "an unmatched signature response changed the deadline");

    signature[2] = tid >> 8;
    signature[3] = tid;
    iaptest_rx(signature, sizeof(signature));
    CHECK_EQ_INT(device.auth.state, AUST_AUTH,
                 "a matched signature response was not accepted");
    r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen == 5 && r->payload[1] == 0x19,
          "the matched signature response drew no status");
    if (r && r->paylen >= 5)
        CHECK(((uint16_t)r->payload[2] << 8 | r->payload[3]) == tid
              && r->payload[4] == 0x00,
              "the authentication status did not match the request");
}

void test_transid_authentication_timeout_keeps_the_id(void)
{
    iaptest_enter_idps();
    current_tick = 200;
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();
    iap_periodic();

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen >= 4 && r->payload[1] == 0x14,
          "authentication did not start");
    if (!r || r->paylen < 4)
        return;
    CHECK_EQ_INT(device.auth.deadline, current_tick + 2 * HZ,
                 "the unknown-version certificate deadline");

    uint16_t tid = ((uint16_t)r->payload[2] << 8) | r->payload[3];
    unsigned char cert[6] = { 0x00, 0x15, tid >> 8, tid, 0x01, 0x00 };
    iaptest_tx_clear();
    iaptest_rx(cert, sizeof(cert));
    iaptest_tx_clear();
    iap_periodic();

    r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen == 21 && r->payload[1] == 0x17,
          "the Auth1 challenge was not sent");
    CHECK_EQ_INT(device.auth.deadline, current_tick + 7 * HZ,
                 "the Auth1 signature deadline");
    CHECK(r && r->payload[20] == 0x01,
          "the first authentication retry counter was not one");

    uint16_t next_tid = device.ipod_trans_id;
    iaptest_detach_model_for_raw_probes();
    current_tick = device.auth.deadline + 1;
    iaptest_tx_clear();
    iap_periodic();
    r = iaptest_tx(0);
    CHECK(r != NULL && r->paylen == 5 && r->payload[1] == 0x19,
          "an authentication timeout drew no failure status");
    if (r && r->paylen >= 5)
        CHECK(((uint16_t)r->payload[2] << 8 | r->payload[3]) == tid
              && r->payload[4] != 0x00,
              "the timeout status did not retain the authentication ID");
    CHECK_EQ_INT(device.ipod_trans_id, next_tid,
                 "the timeout consumed another transaction ID");
    CHECK_EQ_INT(device.auth.state, AUST_NONE,
                 "the timed-out authentication remained active");
}

void test_transid_authentication_wrap_keeps_zero_id(void)
{
    iaptest_enter_idps();
    device.ipod_trans_id = 0;
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();
    iap_periodic();

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r && r->paylen >= 4 && r->payload[2] == 0
          && r->payload[3] == 0,
          "authentication did not allocate transaction ID zero");

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x15, 0x00, 0x00, 0x01, 0x00);
    iaptest_tx_clear();
    iap_periodic();
    r = iaptest_tx(0);
    CHECK(r && r->paylen >= 4 && r->payload[1] == 0x17
          && r->payload[2] == 0 && r->payload[3] == 0,
          "authentication changed transaction ID after counter wrap");
    CHECK_EQ_INT(device.ipod_trans_id, 1,
                 "authentication consumed two IDs after counter wrap");
}

void test_transid_authentication_rejects_info_in_signature_phase(void)
{
    iaptest_enter_idps();
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();
    iap_periodic();

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r && r->paylen >= 4, "authentication did not start");
    if (!r || r->paylen < 4)
        return;
    uint8_t tid_hi = r->payload[2];
    uint8_t tid_lo = r->payload[3];

    unsigned char cert[] = { 0x00, 0x15, tid_hi, tid_lo, 0x01, 0x00 };
    iaptest_tx_clear();
    iaptest_rx(cert, sizeof(cert));
    iaptest_tx_clear();
    iap_periodic();
    CHECK_EQ_INT(device.auth.state, AUST_CHASENT,
                 "authentication did not reach the signature phase");

    long deadline = device.auth.deadline;
    cert[3]++;
    iaptest_tx_clear();
    iaptest_rx(cert, sizeof(cert));
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "late authentication info with the wrong ID was answered");
    cert[3]--;

    iaptest_tx_clear();
    iaptest_rx(cert, sizeof(cert));
    r = iaptest_tx(0);
    CHECK(r && r->paylen == 6 && r->payload[0] == 0x00
          && r->payload[1] == 0x02 && r->payload[2] == tid_hi
          && r->payload[3] == tid_lo
          && r->payload[4] == IAP_ACK_BAD_PARAM
          && r->payload[5] == 0x15,
          "late authentication info did not receive Bad Parameter");
    CHECK_EQ_INT(device.auth.state, AUST_CHASENT,
                 "late authentication info rewound the state machine");
    CHECK_EQ_INT(device.auth.deadline, deadline,
                 "late authentication info changed the signature deadline");
}

/* RequestIdentify does not tear down our own transaction IDs.
 *
 * MFi p.111: "Support for transaction IDs must be disabled upon receipt
 * of a RequestIdentify command, but enabled again before the accessory
 * sends a subsequent StartIDPS command." That is an instruction to the
 * accessory. MFi 3.3.1 (p.124) gives RequestIdentify "Origin: Apple
 * device" -- this side sends it and never receives one.
 *
 * Applying the accessory's rule to ourselves meant a stray or malformed
 * 0x00 tore down our transaction-ID state mid-session, and every packet
 * after it was parsed two bytes off: an accessory would have had its
 * commands misread from then on with no way to tell.
 *
 * And the ack was suppressed by an "if (cmd != 0)" in cmd_ack(), so the
 * one command an accessory has no business sending drew silence. */
void test_transid_request_identify_does_not_disable_them(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    CHECK(DEVICE_TRANSID_ACTIVE,
          "the session must be using transaction IDs");

    /* The accessory sends the one command it should never send. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x00, 0x00, 0x70);

    CHECK(DEVICE_TRANSID_ACTIVE,
          "receiving RequestIdentify disabled our own transaction IDs, "
          "so every packet after it is parsed two bytes off");

    /* It is refused, not ignored. */
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL,
              "RequestIdentify from an accessory drew silence; it is "
              "Origin: Apple device and a direction violation");

        /* The length, before anything that depends on it. MFi 2.6.1.2
         * (p.111): the accessory disables transaction IDs on "receipt
         * of a General lingo iPodAck command without a transaction ID
         * ... payload length value of either 0x04 or 0x08". A bare
         * four-byte ack here is that signal, and the first version of
         * this case put every other assertion behind "paylen >= 6" --
         * so it skipped all of them and passed on r != NULL alone. */
        CHECK(r != NULL && r->paylen == 6,
              "the refusal must carry a transaction ID; a four-byte "
              "General iPodAck tells the accessory to stop using them "
              "(paylen %d)", r ? r->paylen : -1);

        if (r && r->paylen >= 6) {
            CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x02,
                  "the refusal must be a General iPodAck");
            CHECK(r->payload[2] == 0x00 && r->payload[3] == 0x70,
                  "the refusal must quote this packet's transaction ID "
                  "(got 0x%02X%02X)", r->payload[2], r->payload[3]);
            CHECK(r->payload[4] != 0x00,
                  "a direction violation must not be acked Success");
        }
    }

    /* And the session still parses: a following command is answered
     * with its own ID, which only works if the offset survived. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x0F, 0x00, 0x71, 0x03);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 7 && r->payload[0] == 0x00
              && r->payload[1] == 0x10,
              "the session stopped parsing after RequestIdentify");
        if (r && r->paylen >= 7)
            CHECK(r->payload[2] == 0x00 && r->payload[3] == 0x71,
                  "the reply after RequestIdentify lost the transaction "
                  "ID offset (got 0x%02X%02X)",
                  r->payload[2], r->payload[3]);
    }
}
