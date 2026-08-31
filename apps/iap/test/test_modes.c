/***************************************************************************
 * The accessory-mode matrix.
 *
 * A General lingo packet can arrive in four states that are NOT
 * interchangeable, and a handler has to be right in all of them:
 *
 *   legacy   identified with IdentifyDeviceLingoes. No transaction IDs,
 *            ever (MFi 2.6.1.4, p.112).
 *   window   after StartIDPS, before EndIDPS. Transaction IDs are
 *            already mandatory here -- p.95's IMPORTANT note, "all
 *            subsequent iAP command packets must include transaction
 *            IDs, regardless of lingo" -- but device.auth.idps is still
 *            false, because it is only set at EndIDPS.
 *   post     after EndIDPS. Mandatory, and device.auth.idps is true.
 *   short    a packet too small to carry the ID it owes.
 *
 * Every regression shipped in this series so far was the same mistake:
 * a handler made right in one mode and wrong in another, with a test
 * written only for the mode the author had in mind. GetiPodOptionsForLingo
 * derived its offset from device.auth.idps and so misread the lingo in
 * `window`; SetEventNotification hand-rolled an ack the same way and
 * emitted a transaction-ID teardown there.
 *
 * So rather than another handful of hand-written cases, this drives the
 * commands that echo one of their own parameters back and checks the
 * echo in every mode. An offset computed from the wrong thing shows up
 * immediately, because the echoed byte comes back wrong.
 ****************************************************************************/

#include "iap_test.h"
#include "appevents.h"
#include "accessory.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "iap.h"
#include "audio.h"
#include "iap-core.h"
#include "button.h"

enum mode { MODE_LEGACY, MODE_WINDOW, MODE_POST, MODE_SHORT };

static const char *mode_name(enum mode m)
{
    switch (m) {
        case MODE_LEGACY: return "legacy";
        case MODE_WINDOW: return "inside the IDPS window";
        case MODE_POST:   return "after EndIDPS";
        default:          return "truncated";
    }
}

/* Bring the link to the requested state. The model is detached because
 * these cases build their own packets and deliberately truncate some. */
static void enter_mode(enum mode m)
{
    /* MODE_SHORT deliberately sends packets too small to carry the
     * transaction ID they owe, which the model has no way to follow.
     * Every other mode is a conformant exchange and is judged. Keeping
     * the model attached here is the point: it was detached for all four
     * modes at first, and that hid live transaction-ID defects in the
     * authentication path. */
    if (m == MODE_SHORT)
        iapacc_detach();
    else
        iapacc_attach();

    switch (m) {
        case MODE_LEGACY:
            iaptest_identify_legacy(0x0000041D);
            break;
        case MODE_WINDOW:
        case MODE_SHORT:
            IAPTEST_RX(0x00, 0x38, 0x00, 0x01);     /* StartIDPS only */
            break;
        case MODE_POST:
            IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
            IAPTEST_RX(0x00, 0x3B, 0x00, 0x02, 0x00);
            break;
    }
    iaptest_force_authenticated();
    iaptest_tx_clear();
}

static bool mode_has_transid(enum mode m)
{
    return m == MODE_WINDOW || m == MODE_POST;
}

/* One command that echoes a parameter back in its reply. */
struct echo_case {
    const char   *name;
    unsigned char lingo;
    unsigned char cmd;          /* request */
    unsigned char reply;        /* expected reply command */
    unsigned char param;        /* the distinctive value we send */
    int           echo_at;      /* index of the echo within the reply
                                 * payload, past lingo, command and any
                                 * transaction ID */
    bool          in_window;    /* answered inside the IDPS window? */
};

/* Every one of these is a command whose reply repeats a request byte, so
 * a wrong offset anywhere in the chain is visible in the answer. */
static const struct echo_case echoes[] = {
    /* RequestLingoProtocolVersion -> ReturnLingoProtocolVersion echoes
     * the lingo asked about (MFi Table 3-16, p.128). */
    { "RequestLingoProtocolVersion", 0x00, 0x0F, 0x10, 0x03, 0, true },

    /* GetiPodOptionsForLingo -> RetiPodOptionsForLingo echoes the lingo
     * (Table 3-131, p.192: "LingoID: ID of lingo for which options were
     * requested"). Accepted inside the IDPS window (p.95). */
    { "GetiPodOptionsForLingo",      0x00, 0x4B, 0x4C, 0x03, 0, true },

    /* GetiPodPreferences -> RetiPodPreferences echoes the class. */
    { "GetiPodPreferences",          0x00, 0x29, 0x2A, 0x03, 0, true },

    /* GetiPodStateInfo -> RetiPodStateInfo echoes the info type
     * (Table 4-71, p.265). Not answered inside the IDPS window: p.95
     * says the device "accepts only" four General lingo commands during
     * IDPS, and device.lingoes is not populated until EndIDPS, so
     * Display Remote has not been negotiated yet and the rejection is
     * correct. It is here to pin the legacy and post-EndIDPS offsets. */
    { "GetiPodStateInfo",            0x03, 0x0C, 0x0D, 0x04, 0, false },
};

/* Send one command in one mode and check the echo. */
static void check_echo(const struct echo_case *e, enum mode m)
{
    unsigned char p[8];
    int n = 0;

    enter_mode(m);

    p[n++] = e->lingo;
    p[n++] = e->cmd;
    if (mode_has_transid(m)) {
        p[n++] = 0xA5;
        p[n++] = 0x5A;
    }
    if (m != MODE_SHORT)
        p[n++] = e->param;

    iaptest_rx(p, n);

    const struct iaptest_pkt *r = iaptest_tx(0);
    if (!r) {
        /* Silence is acceptable for a truncated packet. */
        if (m != MODE_SHORT)
            CHECK(false, "%s in %s: no reply at all",
                  e->name, mode_name(m));
        return;
    }

    CHECK(r->checksum_ok, "%s in %s: bad checksum",
          e->name, mode_name(m));

    /* A rejection is a legitimate answer to a truncated command, and to
     * anything the device declines. It is not an echo, so stop here --
     * but it must never be one of the two lengths that MFi 2.6.1.2
     * (p.111) defines as "stop using transaction IDs". */
    /* A command the device does not accept in this mode answers with a
     * rejection, which is correct and not an echo. */
    if (m == MODE_WINDOW && !e->in_window) {
        CHECK(r->paylen >= 2 && r->payload[1] == 0x00,
              "%s in %s: expected a rejection, got command 0x%02X",
              e->name, mode_name(m), r->paylen >= 2 ? r->payload[1] : 0);
        return;
    }

    if (r->paylen >= 2 && r->payload[0] == 0x00 && r->payload[1] == 0x02) {
        if (mode_has_transid(m))
            CHECK(r->paylen != 4 && r->paylen != 8,
                  "%s in %s: rejected with a %d byte General iPodAck, "
                  "which tells the accessory to stop using transaction "
                  "IDs", e->name, mode_name(m), r->paylen);
        return;
    }

    if (m == MODE_SHORT)
        return;

    int idb = 1;
    int tid = mode_has_transid(m) ? 2 : 0;

    CHECK_EQ_INT(r->payload[0], e->lingo, "reply lingo");
    CHECK_EQ_INT(r->payload[1], e->reply, "reply command");

    if (tid) {
        CHECK_EQ_INT(r->payload[2], 0xA5,
                     "transaction ID high byte must be echoed");
        CHECK_EQ_INT(r->payload[3], 0x5A,
                     "transaction ID low byte must be echoed");
    }

    int at = 1 + idb + tid + e->echo_at;
    if (r->paylen <= at) {
        CHECK(false, "%s in %s: reply is %d bytes, too short to hold the "
              "echoed parameter", e->name, mode_name(m), r->paylen);
        return;
    }

    if (r->payload[at] != e->param)
        CHECK(false,
              "%s in %s: echoed 0x%02X where 0x%02X was requested. The "
              "handler read its parameter from the wrong offset for this "
              "mode -- the classic symptom of deriving the offset from "
              "device.auth.idps, which is false until EndIDPS, rather "
              "than from whether the packet carries a transaction ID",
              e->name, mode_name(m), r->payload[at], e->param);
}

/* ------------------------------------------------------------------ */

void test_modes_echo_legacy(void)
{
    for (unsigned i = 0; i < sizeof(echoes)/sizeof(echoes[0]); i++)
        check_echo(&echoes[i], MODE_LEGACY);
}

void test_modes_echo_inside_idps_window(void)
{
    for (unsigned i = 0; i < sizeof(echoes)/sizeof(echoes[0]); i++)
        check_echo(&echoes[i], MODE_WINDOW);
}

void test_modes_echo_after_endidps(void)
{
    for (unsigned i = 0; i < sizeof(echoes)/sizeof(echoes[0]); i++)
        check_echo(&echoes[i], MODE_POST);
}

/* RetTransportMaxPayloadSize carries a 2-byte size and nothing else, so
 * its length is the whole assertion: 4 bytes legacy, 6 with an ID.
 * MFi Table 3-22 (p.132), which gives it
 * "maxPayload: The maximum allowable packet payload, in bytes" and
 * nothing else. A hardcoded transaction ID made the legacy
 * form four bytes of payload where two belong. */
void test_modes_transport_payload_size_length(void)
{
    static const struct { enum mode m; int paylen; } want[] = {
        { MODE_LEGACY, 4 },     /* lingo, command, 2 size bytes */
        { MODE_WINDOW, 6 },     /* plus the transaction ID */
        { MODE_POST,   6 },
    };

    for (unsigned i = 0; i < sizeof(want)/sizeof(want[0]); i++) {
        unsigned char p[4] = { 0x00, 0x11, 0xA5, 0x5A };
        int len = mode_has_transid(want[i].m) ? 4 : 2;

        enter_mode(want[i].m);
        iaptest_rx(p, len);

        const struct iaptest_pkt *r = iaptest_tx(0);
        if (!r) {
            CHECK(false, "RequestTransportMaxPayloadSize in %s: no reply",
                  mode_name(want[i].m));
            continue;
        }
        CHECK_EQ_INT(r->payload[1], 0x12,
                     "reply must be RetTransportMaxPayloadSize");
        CHECK_EQ_INT(r->paylen, want[i].paylen,
                     "RetTransportMaxPayloadSize payload length");
        if (mode_has_transid(want[i].m) && r->paylen >= 4) {
            CHECK_EQ_INT(r->payload[2], 0xA5, "transaction ID high byte");
            CHECK_EQ_INT(r->payload[3], 0x5A, "transaction ID low byte");
        }
    }
}

/* RequestLingoProtocolVersion truncated under IDPS must be refused, not
 * answered from where the transaction ID belongs. */
void test_modes_lingo_version_rejects_short_idps_packet(void)
{
    enter_mode(MODE_WINDOW);

    /* Three bytes: no room for the ID this packet owes. */
    static const unsigned char p[] = { 0x00, 0x0F, 0x03 };
    iaptest_rx(p, sizeof(p));

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no answer at all");
    if (!r || r->paylen < 2)
        return;
    CHECK(r->payload[1] == 0x02,
          "a three-byte RequestLingoProtocolVersion under IDPS must be "
          "refused, got command 0x%02X", r->payload[1]);
}

void test_modes_echo_truncated(void)
{
    for (unsigned i = 0; i < sizeof(echoes)/sizeof(echoes[0]); i++)
        check_echo(&echoes[i], MODE_SHORT);
}

/* ------------------------------------------------------------------ */
/* No command may emit a transaction-ID teardown in any IDPS mode      */
/* ------------------------------------------------------------------ */

/* MFi 2.6.1.2 (p.111): a General lingo iPodAck with a payload of 0x04 or
 * 0x08 tells the accessory to stop sending transaction IDs. Once IDPS
 * has begun, the device must never send one -- the accessory would obey
 * while our side kept adding the two-byte offset, and the link would be
 * two bytes out for good.
 *
 * This sweeps every General command in both IDPS modes at several
 * lengths, which is the shape of packet that provoked it twice. */
static void sweep_teardown(enum mode m)
{
    /* Counted so a sweep that silently stopped feeding, or that the
     * device stopped answering, fails instead of reporting ok. The
     * malformed sweeps got this via sweep_end(); these two did not, and
     * an audit found them contributing 0 of the suite's checks. */
    int fed = 0, answered = 0;

    /* 0x00, 0x01 and 0x13 are absent deliberately. MFi 2.6.1.2 (p.111)
     * has the accessory disable transaction IDs "upon receipt of a
     * RequestIdentify command" and "before sending an
     * IdentifyDeviceLingoes command", so by the time we answer either of
     * those the accessory is already back in legacy mode and a
     * four-byte ack is the correct reply, not a teardown. 2.6.1.4 lists
     * the same three as the only commands exempt from carrying an ID. */
    static const unsigned char cmds[] = {
        0x02, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11,
        0x14, 0x15, 0x17, 0x18, 0x19, 0x1D, 0x1F, 0x23, 0x24, 0x26,
        0x27, 0x28, 0x29, 0x2B, 0x35, 0x37, 0x38, 0x39, 0x3B, 0x3C,
        0x48, 0x49, 0x4B, 0x4D, 0x4F, 0x54, 0x77, 0xFE, 0xFF,
    };
    unsigned char p[10];

    for (unsigned i = 0; i < sizeof(cmds); i++) {
        for (int len = 2; len <= 8; len++) {
            enter_mode(m);

            p[0] = 0x00;
            p[1] = cmds[i];
            for (int b = 2; b < len; b++)
                p[b] = (unsigned char)(0xA5 ^ b);

            iaptest_rx(p, len);
            fed++;
            answered += iaptest_tx_count();

            for (int k = 0; k < iaptest_tx_count(); k++) {
                const struct iaptest_pkt *r = iaptest_tx(k);
                if (r->paylen >= 2 && r->payload[0] == 0x00
                    && r->payload[1] == 0x02
                    && (r->paylen == 4 || r->paylen == 8)) {
                    CHECK(false,
                          "General command 0x%02X at length %d in %s "
                          "produced a %d byte iPodAck, which MFi 2.6.1.2 "
                          "makes an instruction to stop using transaction "
                          "IDs", cmds[i], len, mode_name(m), r->paylen);
                    return;
                }
            }
            iaptest_button_sample(4);
        }
    }

    CHECK(fed >= 200, "only %d packets were fed in %s, expected at "
          "least 200", fed, mode_name(m));
    CHECK(answered > 0, "the device answered none of %d packets in %s, "
          "so nothing was validated", fed, mode_name(m));
}

void test_modes_no_teardown_in_idps_window(void)
{
    sweep_teardown(MODE_WINDOW);
}

void test_modes_no_teardown_after_endidps(void)
{
    sweep_teardown(MODE_POST);
}

/* ------------------------------------------------------------------ */
/* Simple Remote reads its buttons past the transaction ID             */
/* ------------------------------------------------------------------ */

/* ContextButtonStatus carries no echo, so the mode matrix above cannot
 * see it: the only observable is which button comes out. A mutation
 * setting lingo 2's doff to 0 -- making the remote read its buttons from
 * the transaction ID -- survived the whole suite.
 *
 * The transaction ID here is chosen so that misreading it produces no
 * button at all rather than accidentally the right one. */
void test_modes_simple_remote_button_offset(void)
{
    iaptest_detach_model_for_raw_probes();

    /* Legacy: state bytes immediately after the command. */
    enter_mode(MODE_LEGACY);
    IAPTEST_RX(0x02, 0x00, 0x01, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_PLAY,
                 "legacy ContextButtonStatus play/pause");
    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(4);

    /* Under IDPS the state bytes sit two later. Transaction ID 0x1200
     * puts 0x00 where a doff of 0 would look, so a wrong offset yields
     * BUTTON_NONE and cannot pass by coincidence. */
    enter_mode(MODE_POST);
    IAPTEST_RX(0x02, 0x00, 0x12, 0x00, 0x01, 0x00);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_PLAY,
                 "ContextButtonStatus play/pause after EndIDPS; "
                 "BUTTON_NONE here means the state byte was read from "
                 "the transaction ID");
    iaptest_button_sample(4);
    IAPTEST_RX(0x02, 0x00, 0x12, 0x01, 0x00, 0x00);
    iaptest_button_sample(4);

    /* The transport controls live in the third state byte, which the
     * handler reads at buf[4 + doff] and only when the two before it are
     * zero. Legacy that is buf[4]; under IDPS it is buf[6]. */
    enter_mode(MODE_LEGACY);
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00, 0x10);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_RIGHT,
                 "legacy ContextButtonStatus fast forward");
    iaptest_button_sample(4);

    enter_mode(MODE_POST);
    IAPTEST_RX(0x02, 0x00, 0x12, 0x00, 0x00, 0x00, 0x10);
    CHECK_EQ_INT(iap_remotebtn, BUTTON_RC_RIGHT,
                 "ContextButtonStatus fast forward after EndIDPS");
    iaptest_button_sample(4);
}

/* ------------------------------------------------------------------ */
/* Microphone lingo (0x01) and transaction IDs                          */
/* ------------------------------------------------------------------ */

/* MFi 2.6 (p.110): "Once an accessory has started the IDPS process,
 * beginning with and including the StartIDPS command, the accessory must
 * include 16-bit transaction IDs in every iAP packet."
 *
 * 2.6.1.4 (p.112) names the only three exemptions -- RequestIdentify,
 * Identify and IdentifyDeviceLingoes -- and adds: "If the description of
 * the command in this specification does not include transaction ID
 * fields, the developer must add them after the Command ID field in
 * every command packet, and every packet length byte must be increased
 * by 2."
 *
 * iap-lingo1.c had no doff, no captured id and no IAP_TX_PUT_IPOD_TRANSID
 * anywhere in it -- alone among the lingo handlers. So under IDPS it
 * read its payload two bytes early and sent three device-originated
 * commands with no id at all.
 *
 * The command origins are from the Appendix C.5 section headings:
 * 0x05, 0x06, 0x07, 0x09 and 0x0B are "Origin: Apple device"; 0x04,
 * 0x08 and 0x0A are "Origin: Accessory".
 */

static const struct iaptest_pkt *mic_tx(unsigned char cmd)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x01 && p->payload[1] == cmd)
            return p;
    }
    return NULL;
}

void test_modes_microphone_honours_transaction_ids(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.lingoes |= (1u << 0x01);

    /* RetAccessoryCaps (0x01/0x08). Table C-12 (p.534) gives it four
     * capability bytes, so eight with a transaction id. Bits 0 and 1 of
     * the last byte are the stereo line-in pair, and setting both is
     * what should provoke SetAccessoryCtrl.
     *
     * The id is 0x0300 so that a handler ignoring it reads 0x03 as the
     * top capability byte and finds the real ones two bytes late. */
    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x03, 0x00,
               0x00, 0x00, 0x00, 0x03);

    const struct iaptest_pkt *p = mic_tx(0x0B);
    CHECK(p != NULL,
          "no SetAccessoryCtrl after RetAccessoryCaps advertised stereo "
          "line-in: the capability byte was read from the transaction ID");
    if (p) {
        CHECK_EQ_INT(p->paylen, 6,
                     "SetAccessoryCtrl length with a transaction ID "
                     "(lingo, command, two id bytes, two data bytes)");
    }

    /* And a packet that does NOT advertise stereo line-in must produce
     * nothing, so the check above is not passing on any input. */
    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x03, 0x01,
               0x00, 0x00, 0x00, 0x00);
    CHECK(mic_tx(0x0B) == NULL,
          "SetAccessoryCtrl sent although the accessory advertised no "
          "stereo line-in");

    /* iPodModeChange (0x01/0x06) is Origin: Apple device, so it carries
     * the device's own counter. It was three bytes with no id, and an
     * accessory parsed its Mode byte as the id's high byte -- so it
     * never learned recording had ended, and C.5.4 (p.536) has it stay
     * out of low-power mode until it does. */
    iaptest_tx_clear();
    iap_record(true);
    /* iap_record() only flags it. audio_set_source() calls that from
     * the audio, UI and plugin threads, and the TX buffer belongs to
     * the iAP thread, so the packet goes out from the tick. */
    iap_periodic();
    p = mic_tx(0x06);
    CHECK(p != NULL, "iap_record() sent no iPodModeChange");
    if (p) {
        CHECK_EQ_INT(p->paylen, 5,
                     "iPodModeChange length with a transaction ID");
        if (p->paylen >= 5)
            CHECK_EQ_INT(p->payload[4], 0x00, "Mode 0x00, begin recording");
    }

    iaptest_tx_clear();
    iap_record(false);
    iap_periodic();
    p = mic_tx(0x06);
    if (p && p->paylen >= 5)
        CHECK_EQ_INT(p->payload[4], 0x01, "Mode 0x01, end recording");
}

/* Legacy accessories must still see the short form. */
void test_modes_microphone_legacy_has_no_transaction_id(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();

    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);

    const struct iaptest_pkt *p = mic_tx(0x0B);
    CHECK(p != NULL, "no SetAccessoryCtrl for a legacy accessory");
    if (p)
        CHECK_EQ_INT(p->paylen, 4,
                     "SetAccessoryCtrl length without a transaction ID");

    iaptest_tx_clear();
    iap_record(true);
    /* iap_record() only flags it. audio_set_source() calls that from
     * the audio, UI and plugin threads, and the TX buffer belongs to
     * the iAP thread, so the packet goes out from the tick. */
    iap_periodic();
    p = mic_tx(0x06);
    CHECK(p != NULL, "no iPodModeChange for a legacy accessory");
    if (p) {
        CHECK_EQ_INT(p->paylen, 3, "iPodModeChange length, legacy");
        if (p->paylen >= 3)
            CHECK_EQ_INT(p->payload[2], 0x00, "Mode 0x00, begin recording");
    }
}

/* ------------------------------------------------------------------ */
/* Digital Audio (0x0A) gates                                           */
/* ------------------------------------------------------------------ */

/* MFi 4.10.1 (p.345): "Every accessory that supports the Digital Audio
 * lingo must authenticate itself with a connected Apple device as soon
 * as the Apple device recognizes the accessory; deferred authentication
 * is not permitted."
 *
 * iap_handlepkt_mode10() had no authentication gate, no
 * DEVICE_LINGO_SUPPORTED check and no length check -- alone among the
 * handlers. Anything that reached the dispatcher got a reply. */
static bool saw_track_attrs(void)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x0A && p->payload[1] == 0x04)
            return true;
    }
    return false;
}

void test_modes_digital_audio_is_gated(void)
{
    /* Negotiated but NOT authenticated. */
    iaptest_enter_idps();
    device.auth.state = AUST_NONE;
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x10,
               0x00, 0x00, 0x7D, 0x00,      /* 32000 */
               0x00, 0x00, 0xAC, 0x44,      /* 44100 */
               0x00, 0x00, 0xBB, 0x80);     /* 48000 */
    CHECK(!saw_track_attrs(),
          "TrackNewAudioAttributes sent to an unauthenticated accessory, "
          "though MFi 4.10.1 (p.345) makes authentication mandatory for "
          "this lingo");

    /* Authenticated but the lingo was never negotiated. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.lingoes &= ~(1u << 0x0A);
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x11,
               0x00, 0x00, 0x7D, 0x00,      /* 32000 */
               0x00, 0x00, 0xAC, 0x44,      /* 44100 */
               0x00, 0x00, 0xBB, 0x80);     /* 48000 */
    CHECK(!saw_track_attrs(),
          "TrackNewAudioAttributes sent although lingo 0x0A was never "
          "negotiated");

    /* And it must not answer at all. The rate-list path above is
     * refused twice -- here and inside iap_send_audio_attrs() -- so
     * either check alone looks dead to the mutation sweep. An
     * unsupported command is refused only here, because the default arm
     * that answers one sits past this gate. A lingo the accessory never
     * negotiated should draw nothing whatever it sends. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x06, 0x00, 0x12);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "a Digital Audio command was answered for an accessory "
                 "that never negotiated the lingo");

    /* Both in order: it works. */
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x12,
               0x00, 0x00, 0x7D, 0x00,      /* 32000 */
               0x00, 0x00, 0xAC, 0x44,      /* 44100 */
               0x00, 0x00, 0xBB, 0x80);     /* 48000 */
    CHECK(saw_track_attrs(),
          "a negotiated, authenticated accessory got no "
          "TrackNewAudioAttributes");

    /* Table 4-237 (p.355) makes the payload "a list of n sample rates
     * (32-bit big-endian format)". A packet carrying none is not one. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x13);
    CHECK(!saw_track_attrs(),
          "TrackNewAudioAttributes sent for a RetAccessorySampleRateCaps "
          "carrying no sample rates at all");

    /* And a two-byte packet must not be read past either. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03);
    CHECK(!saw_track_attrs(),
          "TrackNewAudioAttributes sent for a bare lingo and command");
}

/* Digital Audio works inside the IDPS window, and its reply carries a
 * transaction ID there.
 *
 * The inbound offset in this handler was gated on device.auth.idps,
 * which is only set at EndIDPS -- inside the window the accessory is
 * already sending IDs while that flag is still false, so the offset was
 * two bytes short. It has been corrected to DEVICE_TRANSID_ACTIVE, but
 * this case does NOT test that: nothing in the handler reads the offset
 * today, so no assertion can distinguish the two. Reverting the offset
 * alone changes no observable byte, and a mutation confirms it. What
 * this case does pin is the reply: dropping its transaction ID fails
 * five checks. */
void test_modes_digital_audio_replies_inside_the_idps_window(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Re-enter the window: StartIDPS sets idps_started, and idps stays
     * false until EndIDPS. */
    device.auth.idps = false;
    device.auth.idps_started = true;
    CHECK(DEVICE_TRANSID_ACTIVE,
          "the harness did not reproduce the IDPS window");

    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x14,
               0x00, 0x00, 0x7D, 0x00,      /* 32000 */
               0x00, 0x00, 0xAC, 0x44,      /* 44100 */
               0x00, 0x00, 0xBB, 0x80);     /* 48000 */
    CHECK(saw_track_attrs(),
          "no TrackNewAudioAttributes inside the IDPS window");

    /* The reply carries a transaction ID of its own, so it is two bytes
     * longer than the legacy form: lingo, command, id, and three 32-bit
     * fields. */
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x0A
            && p->payload[1] == 0x04) {
            CHECK_EQ_INT(p->paylen, 16,
                         "TrackNewAudioAttributes length inside the "
                         "IDPS window");
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* IDPS commands outside the IDPS process                               */
/* ------------------------------------------------------------------ */

/* MFi 3.3.43 (p.173): "If the accessory sends this command while the
 * Apple device is not in the IDPS process, the Apple device responds
 * with an iPodAck command that passes a nonzero status." MFi 3.3.41
 * (p.160) says the same of SetFIDTokenValues: "The Apple device accepts
 * this command only if the accessory previously initiated the IDPS
 * process by sending a StartIDPS command."
 *
 * Neither checked. An EndIDPS with no StartIDPS before it set
 * device.auth.idps, which makes DEVICE_TRANSID_ACTIVE true for every
 * lingo -- so from then on every packet from an accessory that never
 * enabled transaction IDs was parsed two bytes off. */
void test_modes_idps_commands_need_a_started_idps(void)
{
    /* From a clean session. Without this the case inherited whatever
     * the case before it left in device, and what the probes below are
     * answered with depended on the order of cases.def -- which is how
     * adding an unrelated case to another file made this one fail. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();

    CHECK(!DEVICE_TRANSID_ACTIVE,
          "a legacy accessory should not have transaction IDs in force");

    /* EndIDPS out of the blue. Five bytes, not three: EndIDPS is
     * CHECKLEN(5) in iap-lingo0.c, so a three-byte version was refused
     * for its length and never reached the out-of-process guard this
     * case exists to test. Deleting that guard left the suite green. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x00, 0x00);
    CHECK(!device.auth.idps,
          "an EndIDPS with no StartIDPS before it completed IDPS");
    CHECK(!DEVICE_TRANSID_ACTIVE,
          "an EndIDPS with no StartIDPS switched transaction IDs on for "
          "an accessory that never enabled them");
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to an out-of-process EndIDPS");
        if (r && r->paylen >= 3) {
            CHECK_EQ_INT(r->payload[1], 0x02, "the reply is an iPodAck");
            CHECK(r->payload[2] != 0x00,
                  "an out-of-process EndIDPS was acknowledged Success");
        }
    }

    /* SetFIDTokenValues out of the blue. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x39, 0x01, 0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to an out-of-process SetFIDTokenValues");
        if (r && r->paylen >= 3) {
            CHECK_EQ_INT(r->payload[1], 0x02, "the reply is an iPodAck");
            CHECK(r->payload[2] != 0x00,
                  "an out-of-process SetFIDTokenValues was acknowledged "
                  "Success");
        }
    }

    /* And a real IDPS still works afterwards. */
    iaptest_enter_idps();
    CHECK(device.auth.idps, "IDPS stopped working after the refusals");
}

/* Table 3-97 (p.173), accEndIDPSStatus 3: "The accessory has finished
 * with IDPS on the current transport and is restarting IDPS on another
 * transport. The accessory must cease traffic on the old transport
 * immediately and restart IDPS on the new transport within 2 seconds."
 *
 * It fell into the Abandon arm and was answered with IDPSStatus 6,
 * which Table 3-99 (p.175) defines only for accEndIDPSStatus 2 and
 * which means "the accessory may send IdentifyDeviceLingoes but not
 * StartIDPS" -- forbidding the very command it must send within two
 * seconds. */
void test_modes_end_idps_transport_change(void)
{
    static const unsigned char lingoes[] = { 0x00, 0x02, 0x03 };

    iapacc_attach();
    iapacc_reset();
    IAPACC_SEND(0x00, 0x38);                     /* StartIDPS */
    iaptest_tx_clear();
    IAPACC_SEND(0x00, 0x3B, 0x03);               /* restarting elsewhere */

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 5 && p->payload[0] == 0x00
            && p->payload[1] == 0x3C) {
            CHECK(p->payload[4] != 0x06,
                  "a transport change was answered with IDPSStatus 6, "
                  "which forbids the StartIDPS it must now send");
        }
    }

    /* The device must also let go of the IDPS state, so the StartIDPS
     * on the new transport is a fresh start rather than a second one
     * inside a process that never ended. */
    CHECK(!device.auth.idps_started,
          "a transport change left the IDPS process open, so the "
          "accessory's StartIDPS on the new transport arrives inside "
          "the old one");
    CHECK(!device.auth.idps,
          "a transport change completed IDPS");

    /* And the new transport's StartIDPS is accepted. */
    iapacc_identify_idps(lingoes, sizeof(lingoes));
    CHECK(device.auth.idps,
          "IDPS on the new transport was refused after a transport "
          "change");
}

/* A reserved accEndIDPSStatus must leave the process alone. MFi 3.3.43
 * (p.173): "If the Apple device is in the IDPS process and the
 * accessory sends this command with an unsupported accEndIDPSStatus
 * value, the Apple device remains in the IDPS process." */
void test_modes_end_idps_reserved_status_changes_nothing(void)
{
    iapacc_attach();
    iapacc_reset();
    IAPACC_SEND(0x00, 0x38);                     /* StartIDPS */

    CHECK(device.auth.idps_started, "StartIDPS did not take");

    static const unsigned char reserved[] = { 0x04, 0x10, 0xFF };
    for (unsigned i = 0; i < sizeof(reserved); i++) {
        unsigned char c[3] = { 0x00, 0x3B, reserved[i] };
        iaptest_tx_clear();
        iapacc_send(c, sizeof(c));

        CHECK(device.auth.idps_started,
              "a reserved accEndIDPSStatus 0x%02X left the IDPS process",
              reserved[i]);
        CHECK(!device.auth.idps,
              "a reserved accEndIDPSStatus 0x%02X completed IDPS",
              reserved[i]);
    }

    /* A real EndIDPS still completes it. */
    IAPACC_SEND(0x00, 0x3B, 0x00);
    CHECK(device.auth.idps,
          "EndIDPS stopped working after a reserved status");
}

/* MFi 4.10.9 (p.356): "The sample rate sent to the accessory is taken
 * from the list of sample rates returned to the Apple device by the
 * RetAccessorySampleRateCaps command. If the accessory supports the
 * sample rate of the current audio track, then it is sent as the
 * current sample rate. If the accessory does not support the sample
 * rate, the Apple device resamples the audio data to a supported sample
 * rate in real time and sends this new supported sample rate as the
 * current sample rate."
 *
 * MFi 4.10.8 (p.355): "At a minimum, every accessory must support the
 * sample rates 32 KHz, 44.1 KHz, and 48 KHz. A RetAccessorySampleRateCaps
 * command with sample rates not listed in Table 4-238, or missing any of
 * the required sample rates, is invalid. If the Apple device receives
 * such a command, it sends the accessory an iPodAck command with a
 * negative acknowledgment as the command status."
 *
 * The list used to be discarded and the mixer's own rate reported
 * whatever it was. This hardware runs at anything from 8 to 48 kHz
 * (HW_SAMPR_CAPS, ipod6g.h:34) and an accessory need only support three
 * of those, so a 22.05 kHz track told the dock to expect a rate it may
 * not have. */
static uint32_t attrs_rate(void)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 8 && p->payload[0] == 0x0A && p->payload[1] == 0x04)
            return ((uint32_t)p->payload[4] << 24)
                 | ((uint32_t)p->payload[5] << 16)
                 | ((uint32_t)p->payload[6] << 8)
                 |  (uint32_t)p->payload[7];
    }
    return 0;
}

void test_modes_digital_audio_honours_the_rate_list(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* The rate selection itself only exists where USB audio does:
     * config.h:1392 defines USB_ENABLE_AUDIO for USB_HAS_ISOCHRONOUS on
     * the S5L8702, so on the PP502x iPod Video the handler reports a
     * fixed 44100 and there is no mixer to move. The validation below
     * is outside that guard and is checked on both targets. */
#ifdef USB_ENABLE_AUDIO
    /* A rate the accessory supports is reported unchanged, and the
     * mixer is left alone. */
    rbstub_set_mixer_frequency(44100);
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x70,
               0x00, 0x00, 0x7D, 0x00, 0x00, 0x00, 0xAC, 0x44,
               0x00, 0x00, 0xBB, 0x80);
    CHECK_EQ_INT(attrs_rate(), 44100, "a supported rate is reported as is");
    CHECK_EQ_INT(rbstub_calls.mixer_frequency, 0,
                 "the mixer must not move for a rate the accessory takes");

    /* A rate it does not support must not be reported. The device
     * resamples instead, to 44.1 kHz, which 4.10.8 obliges every
     * accessory to support. */
    rbstub_set_mixer_frequency(22050);
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x71,
               0x00, 0x00, 0x7D, 0x00, 0x00, 0x00, 0xAC, 0x44,
               0x00, 0x00, 0xBB, 0x80);
    CHECK_EQ_INT(attrs_rate(), 44100,
                 "22050 is not in the accessory's list, so it must not be "
                 "reported");
    CHECK_EQ_INT(rbstub_calls.mixer_frequency, 44100,
                 "and the mixer must move to what was reported, or the "
                 "stream and the announcement disagree");

#endif /* USB_ENABLE_AUDIO */

    /* A list missing a mandatory rate is invalid and gets a negative
     * acknowledgement, not a TrackNewAudioAttributes. This half holds
     * on every target. */
    rbstub_set_mixer_frequency(44100);
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0x72,
               0x00, 0x00, 0xAC, 0x44);        /* 44100 alone */
    CHECK_EQ_INT(attrs_rate(), 0,
                 "a list missing 32 kHz and 48 kHz must not be accepted");
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no reply to an invalid rate list");
        if (p && p->paylen >= 6) {
            /* Table 4-232 (p.346) gives this lingo its own iPodAck,
             * 0x01, Origin: Apple device. It has to come back here:
             * the General iPodAck carries a one-byte command id with
             * no lingo, so on the General lingo 0x03 reads as
             * RequestExtendedInterfaceMode. */
            CHECK_EQ_INT(p->payload[0], 0x0A, "the reply is on lingo 0x0A");
            CHECK_EQ_INT(p->payload[1], 0x01, "iPodAck");
            CHECK_EQ_INT(p->payload[2], 0x00, "it echoes the id, high");
            CHECK_EQ_INT(p->payload[3], 0x72, "it echoes the id, low");
            CHECK(p->payload[4] != 0x00,
                  "and carries a negative acknowledgement");
            CHECK_EQ_INT(p->payload[5], 0x03,
                         "naming RetAccessorySampleRateCaps");
        }
    }
}

/* The three General lingo commands that gate on Extended Interface
 * having been negotiated.
 *
 * MFi 3.3.39: the device enters Extended Interface mode only after "the
 * accessory identifies itself successfully for the Extended Interface
 * lingo (Lingo 0x04)". All three refuse with Bad Parameter otherwise,
 * and refusing is what lets an accessory fall back to the deprecated
 * EnterExtendedInterfaceMode rather than hang.
 *
 * None of the three checks was exercised: deleting any one left every
 * binary green, because every case that sends these declares lingo 0x04
 * first. */
void test_modes_extended_interface_commands_need_the_lingo(void)
{
    static const struct {
        unsigned char pkt[4];
        int           len;
        unsigned char cmd;
        const char   *name;
    } probes[] = {
        { { 0x00, 0x03 },       2, 0x03, "RequestExtendedInterfaceMode" },
        { { 0x00, 0x06 },       2, 0x06, "ExitExtendedInterfaceMode"    },
        { { 0x00, 0x37, 0x01 }, 3, 0x37, "SetUIMode(extended)"          },
    };

    for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        /* Everything except lingo 0x04. */
        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
        iaptest_force_authenticated();
        iaptest_tx_clear();

        iaptest_rx(probes[i].pkt, probes[i].len);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s went unanswered on a session that never "
                         "declared lingo 0x04", probes[i].name);
        /* Guard on two bytes, not four. An unrefused
         * RequestExtendedInterfaceMode answers RetExtendedInterfaceMode,
         * which is three bytes on a legacy session -- skipping short
         * replies meant the case walked straight past the very thing it
         * was written to catch, and the mutation survived. */
        if (!r || r->paylen < 2)
            continue;
        CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x02,
              "%s was answered with lingo 0x%02X command 0x%02X rather "
              "than a General iPodAck -- it was carried out, not refused",
              probes[i].name, r->payload[0], r->payload[1]);
        if (r->payload[0] != 0x00 || r->payload[1] != 0x02 || r->paylen < 4)
            continue;
        CHECK(r->payload[2] == IAP_ACK_BAD_PARAM,
              "%s answered status 0x%02X without lingo 0x04 negotiated; "
              "MFi 3.3.39 makes Extended Interface mode conditional on "
              "it, and Bad Parameter is what lets the accessory fall "
              "back", probes[i].name, r->payload[2]);
        CHECK_EQ_INT(r->payload[3], probes[i].cmd,
                     "the ack names the command that was sent");
    }

    /* With the lingo declared, none of them is refused for that reason
     * -- so the case is watching the gate, not a dead handler. */
    for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x04));
        iaptest_force_authenticated();
        iaptest_tx_clear();

        iaptest_rx(probes[i].pkt, probes[i].len);

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s went unanswered with lingo 0x04 declared",
              probes[i].name);
        if (r && r->paylen >= 4 && r->payload[0] == 0x00
            && r->payload[1] == 0x02)
            CHECK(r->payload[2] != IAP_ACK_BAD_PARAM,
                  "%s was still refused with lingo 0x04 declared",
                  probes[i].name);
    }
}

/* MFi p.96: "If the accessory sends StartIDPS again, while the Apple
 * device is in the IDPS process, the Apple device restarts the IDPS
 * process." And p.173, EndIDPS status 1: "The accessory asks to reset
 * all IDPS information it has sent to the Apple device."
 *
 * Neither discarded anything. StartIDPS only set a flag and acked, so a
 * second attempt inherited the first one's IdentifyToken: the lingoes
 * an accessory declared and then withdrew stayed granted. */
void test_modes_idps_restart_forgets_the_first_attempt(void)
{
    iaptest_init();
    iapacc_detach();

    /* First attempt: declare Extended Interface among others. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);

    /* Start again, and this time declare only General and Display
     * Remote. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x03);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x04, 0x01,
               0x0D, 0x00, 0x00,
               0x02, 0x00, 0x03,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x05, 0x00);

    CHECK((device.lingoes & (1u << 0x04)) == 0,
          "the Extended Interface lingo from the abandoned first attempt "
          "is still granted (lingoes = 0x%08X)", device.lingoes);
    CHECK((device.lingoes & (1u << 0x03)) != 0,
          "the Display Remote lingo the second attempt declared was not "
          "granted (lingoes = 0x%08X)", device.lingoes);

    /* And a restart that declares nothing at all must not inherit
     * either. With a second token the overwrite hides the question;
     * without one, whatever the first attempt said is all there is. */
    iaptest_init();
    iapacc_detach();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x38, 0x00, 0x03);         /* start again, no token */

    CHECK_EQ_INT(device.idps_lingoes, 0,
                 "StartIDPS carried the previous attempt's IdentifyToken "
                 "forward; MFi p.96 restarts the process");

    /* EndIDPS(Reset) discards too. */
    iaptest_init();
    iapacc_detach();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x01);   /* Reset */

    CHECK_EQ_INT(device.idps_lingoes, 0,
                 "EndIDPS(Reset) kept the lingoes the accessory asked it "
                 "to discard");
}

/* The two identify paths must agree about which lingoes exist.
 *
 * IdentifyDeviceLingoes masks out anything this firmware has no handler
 * for; the IdentifyToken path stored whatever the accessory listed. So
 * DEVICE_LINGO_SUPPORTED() answered yes for a lingo iap_handlepkt() has
 * no case for, and the accessory's commands on it vanished with no
 * reply at all -- worse than a refusal, because the accessory waits. */
void test_modes_identify_token_filters_unsupported_lingoes(void)
{
    iaptest_init();
    iapacc_detach();

    /* Declare General, USB Host (0x06) and Accessory Equalizer (0x08),
     * neither of the last two implemented here. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x0E, 0x00, 0x00,
               0x03, 0x00, 0x06, 0x08,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);

    CHECK((device.lingoes & (1u << 0x06)) == 0,
          "USB Host was granted through IdentifyToken; nothing handles "
          "it, so its commands disappear (lingoes = 0x%08X)",
          device.lingoes);
    CHECK((device.lingoes & (1u << 0x08)) == 0,
          "Accessory Equalizer was granted through IdentifyToken "
          "(lingoes = 0x%08X)", device.lingoes);
    CHECK((device.lingoes & (1u << 0x00)) != 0,
          "the General lingo was filtered out (lingoes = 0x%08X)",
          device.lingoes);

    /* The legacy path has always filtered; the two now agree. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x06) | (1u << 0x08));
    CHECK((device.lingoes & ((1u << 0x06) | (1u << 0x08))) == 0,
          "IdentifyDeviceLingoes granted an unimplemented lingo "
          "(lingoes = 0x%08X)", device.lingoes);
}

/* A command the device refuses must change nothing.
 *
 * IdentifyDeviceLingoes dropped Extended Interface mode before it had
 * decided whether to accept the command, and it did so through
 * iap_interface_state_change(), which raises BUTTON_RC_PLAY on an
 * EXTENDED to STANDARD transition while playing. So an accessory whose
 * identify was refused with CMD_FAILED had the music paused underneath
 * it and every later Extended Interface command rejected -- while
 * device.lingoes still had bit 4 set, so nothing told it to re-enter a
 * mode it had never asked to leave. */
void test_modes_a_refused_identify_changes_nothing(void)
{
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    IAPTEST_RX(0x00, 0x05);                 /* Extended Interface mode */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();
    iaptest_tx_clear();
    iap_remotebtn = BUTTON_NONE;

    /* A deviceID with no authentication requested: refused at the
     * second of the three rejection paths. */
    IAPTEST_RX(0x00, 0x13,
               0x00, 0x00, 0x00, 0x11,      /* lingoes: General + 0x04 */
               0x00, 0x00, 0x00, 0x00,      /* options: none */
               0x00, 0x00, 0x00, 0x04);     /* deviceID: non-zero */

    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to IdentifyDeviceLingoes");
        if (r && r->paylen >= 4)
            CHECK(r->payload[2] != 0x00,
                  "the identify was accepted; this one has a deviceID "
                  "and no authentication requested");
    }

    CHECK_EQ_INT(iap_remotebtn, BUTTON_NONE,
                 "a refused identify raised a remote button, which is "
                 "what pauses the music");

    /* And the mode the accessory asked for is still there. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x03);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no answer to RequestExtendedInterfaceMode");
        if (r && r->paylen >= 3)
            CHECK_EQ_INT(r->payload[2], 0x01,
                         "a refused identify dropped Extended Interface "
                         "mode");
    }
}

void test_modes_interface_switch_ignores_unrelated_audio_flags(void)
{
    iaptest_init();
    interface_state = IST_STANDARD;
    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_RECORD);
    rbstub_reset_calls();

    iap_interface_state_change(IST_EXTENDED);
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "an interface switch did not pause active playback");
    CHECK(iap_remote_ui_active(),
          "Extended Interface mode did not lock the local UI");
    CHECK_EQ_INT(rbstub_calls.last_button_event, SYS_IAP_UI_ENTER,
                 "Extended Interface mode did not open its screen");

    rbstub_set_audio_status(AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE |
                            AUDIO_STATUS_RECORD);
    iap_interface_state_change(IST_STANDARD);
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "an interface switch paused playback twice");
    CHECK(!iap_remote_ui_active(),
          "Standard UI mode left the local UI locked");
    CHECK_EQ_INT(rbstub_calls.last_button_event, SYS_IAP_UI_EXIT,
                 "Standard UI mode did not close the accessory screen");
}

void test_modes_detach_defers_the_extended_mode_pause(void)
{
    iaptest_init();
    iap_interface_state_change(IST_EXTENDED);
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    rbstub_reset_calls();

    iaptest_irq_context = true;
    iap_reset_device(&device);
    iaptest_irq_context = false;

    CHECK_EQ_INT(rbstub_calls.pause, 0,
                 "detach paused playback from interrupt context");
    CHECK(!iap_remote_ui_active(),
          "detach left Extended Interface mode active");
    CHECK_EQ_INT(rbstub_calls.last_button_event, SYS_IAP_UI_EXIT,
                 "detach did not close the accessory screen");

    iap_reset_device(&device);
    CHECK_EQ_INT(rbstub_calls.button_post, 1,
                 "repeated reset queued another accessory-screen exit");

    iap_periodic();
    CHECK_EQ_INT(rbstub_calls.pause, 1,
                 "the iAP thread did not pause after detach");
}

/* LINGO_SUPPORTED(), LINGO_MAJOR() and LINGO_MINOR() all index
 * lingo_versions[(x) & 0x1f], so a raw packet byte aliases onto a real
 * row. RequestLingoProtocolVersion passed the byte straight in, so
 * every ID from 0x20 up was answered "supported", at a version borrowed
 * from the lingo 32 below it -- and iap_handlepkt() has no case for any
 * of them, so the accessory's next command vanished with no reply. */
void test_modes_lingo_version_rejects_ids_above_the_table(void)
{
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));

    static const unsigned char alias[] = { 0x20, 0x23, 0x40, 0xFF };
    for (unsigned i = 0; i < sizeof(alias); i++) {
        iaptest_tx_clear();
        unsigned char q[3] = { 0x00, 0x0F, alias[i] };
        iaptest_rx(q, sizeof(q));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply to RequestLingoProtocolVersion(0x%02X)",
              alias[i]);
        if (!r || r->paylen < 2)
            continue;
        CHECK(!(r->payload[0] == 0x00 && r->payload[1] == 0x10),
              "lingo 0x%02X was answered with a version; it aliases onto "
              "lingo 0x%02X's row and nothing handles it",
              alias[i], alias[i] & 0x1f);
    }

    /* A real lingo still answers. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x0F, 0x03);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply for the Display Remote lingo");
        if (r && r->paylen >= 3)
            CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x10,
                  "the Display Remote lingo was refused a version");
    }
}

/* Digital audio is started for the accessory on the far side of a
 * completed authentication, and only if it asked for the lingo.
 *
 * MFi 2.2 (p.100) has an accessory granted only the lingoes it declares,
 * and 10.2 (p.556) scopes the Digital Audio lingo to accessories that
 * support USB audio. Setting the flag for one that never declared it
 * makes the periodic handler send a lingo 0x0A command to an accessory
 * with no receiver for it -- and unlike the button lingoes there is no
 * exemption here to soften it.
 */
static void auth_completes(uint32_t lingoes)
{
    iaptest_init();
    iaptest_identify_legacy(lingoes);

    /* The challenge response is the last step; the handler refuses it
     * from any other state. */
    device.auth.state = AUST_CHASENT;
    device.audio_init_pending = false;

    IAPTEST_RX(0x00, 0x18, 0x00);

    if (device.auth.state != AUST_AUTH)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: authentication did not complete "
                     "(state %d), so the flag below means nothing",
                     (int)device.auth.state);
}

void test_modes_digital_audio_starts_only_for_the_lingo(void)
{
    /* An accessory that declared Digital Audio: the flag is the whole
     * point of the path, so it has to be set. */
    auth_completes((1u << 0x00) | (1u << 0x04) | (1u << 0x0A));
    CHECK(device.audio_init_pending,
          "an accessory that declared the Digital Audio lingo was not "
          "queued for audio init, so the check below proves nothing");

    /* One that did not: same path, same authentication, no flag. */
    auth_completes((1u << 0x00) | (1u << 0x04));
    CHECK(!device.audio_init_pending,
          "digital audio was started for an accessory that never "
          "declared the Digital Audio lingo");
}

/* Sample-rate changes queue a digital-audio re-init, and that too is
 * only for an accessory that asked for the lingo.
 *
 * Separate gate from the one above and separate code path: this one is
 * in iap_track_changed(), on the audio thread, and fires whenever the
 * mixer frequency moves. An accessory that never declared Digital Audio
 * has no receiver for what follows.
 */
#ifdef USB_ENABLE_AUDIO
void test_modes_sample_rate_change_needs_the_audio_lingo(void)
{
    /* Declared: the queue has to happen, or the check below is vacuous. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04) | (1u << 0x0A));
    iaptest_force_authenticated();
    device.audio_init_pending = false;
    rbstub_set_mixer_frequency(44100);
    CHECK(rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL),
          "nothing is subscribed to PLAYBACK_EVENT_TRACK_CHANGE, so this "
          "case tests nothing");
    CHECK(device.audio_init_pending,
          "a sample-rate change did not queue digital audio init for an "
          "accessory that declared the lingo");

    /* Not declared: same event, same rate change, no queue. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    iaptest_force_authenticated();
    device.audio_init_pending = false;
    rbstub_set_mixer_frequency(48000);
    rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
    CHECK(!device.audio_init_pending,
          "a sample-rate change queued digital audio init for an "
          "accessory that never declared the Digital Audio lingo");
}
#endif /* USB_ENABLE_AUDIO */

/* Extended Interface mode must not outlive the lingo that granted it.
 *
 * The mode and the negotiated lingoes are separate state -- interface_
 * state is a static in iap-core.c, device.lingoes is a field -- so an
 * accessory re-identifying without the Extended Interface lingo must
 * lose the mode with it. If it did not, the mode gate in iap-lingo4.c
 * would let its commands through on a lingo it no longer holds, and the
 * negotiation check above the mode gate would be the only thing left
 * refusing them. */
void test_modes_extended_mode_does_not_outlive_the_lingo(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05);                 /* EnterRemoteUIMode */
    rbstub_set_playlist(20, 3);

    /* The mode works, or nothing below is being tested. Playing,
     * because PlayControl refuses a toggle with nothing to toggle
     * between and the probe would then fail for the wrong reason. */
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x01);     /* PlayControl: play/pause */
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 4 && r->payload[0] == 0x04
              && r->payload[1] == 0x00 && r->payload[2] == 0x01
              && r->payload[3] == 0x00,
              "PlayControl was not accepted in Extended Interface mode");
    }

    CHECK_EQ_INT(interface_state, IST_EXTENDED,
                 "the mode was not entered, so losing it below proves "
                 "nothing");

    /* Re-identify without the lingo. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    iaptest_force_authenticated();
    CHECK(!(device.lingoes & (1u << 0x04)),
          "the second identify did not drop the Extended Interface "
          "lingo");

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x01);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL && r->paylen >= 4 && r->payload[0] == 0x04
              && r->payload[1] == 0x00 && r->payload[2] == 0x01,
              "an Extended Interface command drew no acknowledgement "
              "after the lingo was dropped");
        if (r && r->paylen >= 4)
            CHECK(r->payload[3] != 0x00,
                  "an Extended Interface command succeeded after the "
                  "accessory re-identified without the lingo");
    }
    CHECK(rbstub_calls.skip == 0,
          "the refused PlayControl still moved playback");
}

/* iap_record() must not build a packet where it is called.
 *
 * audio_set_source() calls it from the audio thread
 * (apps/playback.c:3070), the UI thread (apps/radio/radio.c:395 and
 * :737) and any plugin that touches audio_set_input_source(). The TX
 * buffer belongs to the iAP thread; on the 6G that thread can be parked
 * inside iap_hid_tx(), which holds tx_frame_lock and waits on a
 * semaphore per fragment, reading each chunk out of the shared buffer
 * between waits. An IAP_TX_INIT from another thread rewrites a packet
 * that is halfway onto the wire.
 *
 * The suite is single-threaded, so what it can check is the property
 * that makes the race impossible: nothing goes out until the tick. */
void test_modes_record_notification_waits_for_the_tick(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();
    iaptest_tx_clear();

    CHECK(iap_record(true), "iap_record() refused a negotiated lingo 1");
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "iap_record() built a packet on its caller's thread");

    iap_periodic();
    bool sent = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01
            && p->payload[1] == 0x06)
            sent = true;
    }
    CHECK(sent, "the tick did not send the flagged iPodModeChange");

    /* And only once: a second tick with nothing pending is silent. */
    iaptest_tx_clear();
    iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(!(p && p->paylen >= 2 && p->payload[0] == 0x01
                && p->payload[1] == 0x06),
              "iPodModeChange was sent again with nothing pending");
    }

    /* An accessory without the lingo gets nothing at all. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    CHECK(!iap_record(true),
          "iap_record() accepted a lingo the accessory never declared");
    iap_periodic();
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(!(p && p->paylen >= 2 && p->payload[0] == 0x01),
              "a Microphone packet went to an accessory without the "
              "Microphone lingo");
    }
}

/* A second StartIDPS on a completed session starts it over.
 *
 * MFi p.96 verbatim: "If an accessory has already completed IDPS
 * successfully, inadvertently sending a StartIDPS or
 * IdentifyDeviceLingoes command resets its authentication and
 * identification states. The accessory must repeat the IDPS and
 * authentication processes."
 *
 * The handler cleared the IdentifyToken fields and nothing else, so
 * authentication, the negotiated lingoes, Extended Interface mode and
 * every notification mask survived. p.95 gives a power glitch as a
 * reason to restart, so this is reachable without an accessory
 * misbehaving -- and inside the three-second IDPS budget the device
 * went on serving gated commands and sending unsolicited notifications
 * for a session that no longer existed.
 */
void test_modes_restarting_idps_resets_the_session(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(20, 3);

    /* A live session: authenticated, in Extended Interface mode, with
     * notifications running. */
    IAPTEST_RX(0x04, 0x00, 0x26, 0x00, 0x90, 0x00, 0x00, 0x00, 0x0D);
    CHECK(DEVICE_AUTHENTICATED, "the session must start authenticated");
    CHECK_EQ_INT(interface_state, IST_EXTENDED,
                 "the session must start in Extended Interface mode");
    CHECK(device.lingoes & (1u << 0x04), "lingo 4 must be negotiated");
    CHECK(device.pb_notifications != 0, "notifications must be running");

    /* StartIDPS again. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x91);

    CHECK(!DEVICE_AUTHENTICATED,
          "a repeated StartIDPS left the accessory authenticated");
    CHECK_EQ_INT(interface_state, IST_STANDARD,
                 "a repeated StartIDPS left Extended Interface mode set");
    CHECK(device.lingoes == 0,
          "a repeated StartIDPS left the negotiated lingoes in place "
          "(0x%08X)", device.lingoes);
    CHECK_EQ_INT(device.pb_notifications, 0,
                 "a repeated StartIDPS left the play-status "
                 "subscriptions running");
    CHECK_EQ_INT(device.notifications, 0,
                 "a repeated StartIDPS left the remote-event "
                 "subscriptions running");

    /* Transaction IDs stay live across the restart -- they are in force
     * from this command, not from EndIDPS -- so the ack carries one. */
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "a repeated StartIDPS drew no acknowledgement");
    if (r && r->paylen >= 6)
        CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x02
              && r->payload[2] == 0x00 && r->payload[3] == 0x91
              && r->payload[4] == 0x00 && r->payload[5] == 0x38,
              "the ack must be Success for StartIDPS, quoting this "
              "packet's transaction ID");
    CHECK(DEVICE_TRANSID_ACTIVE,
          "transaction IDs must stay in force across the restart");
}

/* The Digital Audio lingo version, and the behaviour it promises.
 *
 * MFi 4.10 (p.345): "The Apple devices that contain version 1.00 of the
 * Digital Audio lingo do not correctly support digital audio. An
 * accessory should check the attached Apple device's version of the
 * Digital Audio lingo and use digital audio only if the version number
 * is greater than 1.00." This device said 1.00, so a conformant dock
 * asked, was told 1.00, and never streamed -- the feature was
 * unreachable for anything following the spec.
 *
 * Table 4-233 (p.346) is what a higher number has to mean. 1.01: "The
 * TrackNewAudioAttributes command is resent until it is acknowledged by
 * the USB host with an AccessoryAck command. Also corrected a bug where
 * TrackNewAudioAttributes was not being sent before every track."
 * 1.02: "The Digital Audio lingo no longer requires the Apple device to
 * be in Extended Interface mode." 1.03 adds SetVideoDelay, which is not
 * implemented, so 1.02 is the honest answer.
 */
static int count_audio_attrs(void)
{
    int n = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x0A
            && p->payload[1] == 0x04)
            n++;
    }
    return n;
}

void test_modes_digital_audio_version_matches_the_behaviour(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* RequestLingoProtocolVersion for lingo 0x0A. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x0F, 0x00, 0xA0, 0x0A);
    {
        const struct iaptest_pkt *p = iaptest_tx(0);
        CHECK(p != NULL, "no ReturnLingoProtocolVersion");
        if (p && p->paylen >= 7) {
            CHECK_EQ_INT(p->payload[4], 0x0A, "the lingo asked about");
            CHECK_EQ_INT(p->payload[5], 1, "major version");
            CHECK_EQ_INT(p->payload[6], 2,
                         "minor version -- 1.00 tells a conformant "
                         "accessory not to use digital audio at all");
        }
    }

    /* Bring digital audio up: the caps request, the accessory's list,
     * and the attributes that follow it. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0xA1,
               0x00, 0x00, 0x7D, 0x00,      /* 32000 */
               0x00, 0x00, 0xAC, 0x44,      /* 44100 */
               0x00, 0x00, 0xBB, 0x80);     /* 48000 */
    CHECK_EQ_INT(count_audio_attrs(), 1,
                 "RetAccessorySampleRateCaps did not draw exactly one "
                 "TrackNewAudioAttributes");

    /* 1.01, first clause: resent until acknowledged. */
    iaptest_tx_clear();
    iap_periodic();
    CHECK(count_audio_attrs() >= 1,
          "TrackNewAudioAttributes was not resent while unacknowledged");

    /* AccessoryAck for command 0x04 stops it. Table 4-234 (p.353):
     * command status, then the ID of the command acknowledged. */
    IAPTEST_RX(0x0A, 0x00, 0x00, 0xA2, 0x00, 0x04);
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK_EQ_INT(count_audio_attrs(), 0,
                 "TrackNewAudioAttributes kept being resent after the "
                 "accessory acknowledged it");

    /* An acknowledgement naming some other command must not stop it. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x03, 0x00, 0xA3,
               0x00, 0x00, 0x7D, 0x00,
               0x00, 0x00, 0xAC, 0x44,
               0x00, 0x00, 0xBB, 0x80);
    IAPTEST_RX(0x0A, 0x00, 0x00, 0xA4, 0x00, 0x02);   /* acks 0x02 */
    iaptest_tx_clear();
    iap_periodic();
    CHECK(count_audio_attrs() >= 1,
          "an AccessoryAck naming a different command stopped the "
          "resend of TrackNewAudioAttributes");

    /* And it stops eventually rather than for ever. */
    IAPTEST_RX(0x0A, 0x00, 0x00, 0xA5, 0x00, 0x04);
    iaptest_tx_clear();
    for (int t = 0; t < 20; t++)
        iap_periodic();
    CHECK_EQ_INT(count_audio_attrs(), 0, "the resend did not settle");
}

/* An unsupported Digital Audio command draws a rejection, not silence.
 *
 * MFi 4.10.6 (p.354): "The Apple device sends the iPodAck command when
 * it receives an invalid or unsupported command or a bad parameter."
 * Table 4-232 (p.346) reserves 0x06-0xFF, and 0x02, 0x04 and 0x05 are
 * Origin: Apple device, so nothing but 0x00 and 0x03 should arrive.
 *
 * This lingo was the only one that answered silence. Every other
 * rejects, and an accessory waiting on a reply that never comes has to
 * time out to find that out. */
void test_modes_digital_audio_rejects_unsupported_commands(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    static const unsigned char bad[] = { 0x06, 0x20, 0xFF, 0x05 };

    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        iaptest_tx_clear();
        {
            unsigned char p[4] = { 0x0A, bad[i], 0x00, (unsigned char)(0xB0 + i) };
            iaptest_rx(p, sizeof(p));
        }

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL,
              "lingo 0x0A command 0x%02X was answered with silence",
              bad[i]);
        if (r && r->paylen >= 6) {
            CHECK(r->payload[0] == 0x0A && r->payload[1] == 0x01,
                  "the rejection must be a Digital Audio iPodAck");
            CHECK(r->payload[2] == 0x00 && r->payload[3] == 0xB0 + i,
                  "the rejection must quote this packet's transaction "
                  "ID (got 0x%02X%02X)", r->payload[2], r->payload[3]);
            CHECK_EQ_INT(r->payload[4], IAP_ACK_BAD_PARAM, "status");
            CHECK_EQ_INT(r->payload[5], bad[i], "the command rejected");
        }
    }

    /* AccessoryAck (0x00) is an acknowledgement and must still draw
     * nothing -- acknowledging one is a packet the accessory is not
     * waiting for. */
    iaptest_tx_clear();
    IAPTEST_RX(0x0A, 0x00, 0x00, 0xBF, 0x00, 0x04);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "AccessoryAck drew a reply");
}

#ifndef USB_ENABLE_AUDIO
void test_modes_digital_audio_is_not_advertised_without_usb(void)
{
    iaptest_enter_idps();

    CHECK(!LINGO_SUPPORTED(0x0A),
          "Digital Audio is supported without a USB audio transport");
    CHECK(!(device.lingoes & BIT_N(0x0A)),
          "IDPS retained Digital Audio without a USB audio transport");

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x0F, 0x00, 0xA6, 0x0A);
    EXPECT_PAYLOAD(0, 0x00, 0x02, 0x00, 0xA6,
                   IAP_ACK_BAD_PARAM, 0x0F);

    iaptest_init();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x02, 0x00);
    CHECK(!(device.lingoes & BIT_N(0x0A)),
          "the no-token fallback enabled unavailable Digital Audio");
}
#endif

void test_modes_set_display_image_is_not_falsely_accepted(void)
{
    iaptest_session_extended();

    IAPTEST_RX(0x04, 0x00, 0x32, 0x00, 0xA7,
               0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
               0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x00, 0xA7,
                   IAP_ACK_CMD_FAILED, 0x00, 0x32);
}

/* GetUIMode answers, and answers what SetUIMode set.
 *
 * MFi 3.3.37 (p.158): "The accessory sends this command to the attached
 * Apple device to determine its current user interface mode. The Apple
 * device replies by sending a RetUIMode command." Table 3-62 (p.159):
 * "0x00 Standard Apple device operating mode, 0x01 Extended Interface
 * mode".
 *
 * SetUIMode was implemented and this was not, so an accessory could put
 * the device into a mode and not read it back. Both are protocol 1.09,
 * which this device advertises, and Table 3-132 defines no option bit
 * for either -- so there was no way to discover the asymmetry short of
 * getting a Bad Parameter for a command the version says is there. */
void test_modes_ui_mode_can_be_read_back(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    struct { unsigned char set; unsigned char want; const char *what; } tc[] = {
        { 0x01, 0x01, "Extended Interface mode" },
        { 0x00, 0x00, "Standard mode"           },
    };

    for (unsigned i = 0; i < sizeof(tc)/sizeof(tc[0]); i++) {
        iaptest_tx_clear();
        {
            unsigned char p[5] = { 0x00, 0x37, 0x00,
                                   (unsigned char)(0x50 + i), tc[i].set };
            iaptest_rx(p, sizeof(p));
        }
        /* Entering or leaving Extended Interface raises BUTTON_RC_PLAY
         * while playing, which defers the next packet. */
        iaptest_button_sample(4);

        iaptest_tx_clear();
        {
            unsigned char p[4] = { 0x00, 0x35, 0x00,
                                   (unsigned char)(0x60 + i) };
            iaptest_rx(p, sizeof(p));
        }

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "GetUIMode drew no reply after setting %s",
              tc[i].what);
        if (r && r->paylen >= 5) {
            CHECK(r->payload[0] == 0x00 && r->payload[1] == 0x36,
                  "the reply must be RetUIMode");
            CHECK(r->payload[2] == 0x00 && r->payload[3] == 0x60 + i,
                  "RetUIMode must quote this packet's transaction ID "
                  "(got 0x%02X%02X)", r->payload[2], r->payload[3]);
            CHECK_EQ_INT(r->payload[4], tc[i].want, tc[i].what);
        }
    }
}

/* An aliased lingo ID is not a lingo.
 *
 * LINGO_SUPPORTED() indexes lingo_versions[] with "& 0x1f", so 0x20
 * aliases the General lingo, 0x24 aliases Extended Interface, and 48 of
 * the 256 possible bytes answered as though they named a lingo this
 * device has. MFi 3.3.55 (p.191) wants a nonzero iPodAck for a lingo
 * "not listed in Table 3-132 (page 192) or ... not supported by the
 * Apple device on the port being used", and 0x20 is neither. */
void test_modes_aliased_lingo_ids_are_refused(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    static const unsigned char alias[] = { 0x20, 0x22, 0x24, 0xE3 };

    for (unsigned i = 0; i < sizeof(alias); i++) {
        iaptest_tx_clear();
        {
            unsigned char p[5] = { 0x00, 0x4B, 0x00,
                                   (unsigned char)(0xC0 + i), alias[i] };
            iaptest_rx(p, sizeof(p));
        }

        bool answered = false, refused = false;
        for (int j = 0; j < iaptest_tx_count(); j++) {
            const struct iaptest_pkt *p = iaptest_tx(j);
            if (!p || p->paylen < 2)
                continue;
            if (p->payload[0] == 0x00 && p->payload[1] == 0x4C)
                answered = true;
            if (p->payload[0] == 0x00 && p->payload[1] == 0x02
                && p->payload[p->paylen - 2] != 0x00)
                refused = true;
        }
        CHECK(!answered,
              "options were answered for lingo 0x%02X, which is only a "
              "lingo after the & 0x1f", alias[i]);
        CHECK(refused,
              "lingo 0x%02X drew no refusal", alias[i]);
    }
}
