/***************************************************************************
 * Malformed and truncated packet sweep.
 *
 * Every lingo handler indexes buf[] through a `doff` offset that is 2
 * under IDPS and 0 otherwise, guarded by CHECKLEN values that have to be
 * adjusted to match. Getting one of those wrong reads past the packet.
 * Reading the code for it is slow and unreliable; this drives every
 * command with every short length instead and checks two invariants:
 *
 *   - the firmware does not panic (panicf() abort()s in this harness,
 *     so a violation kills the run rather than being reported)
 *   - whatever it does transmit is a well-formed packet
 *
 * MFi 2.1 (p.87) requires an accessory to tolerate a documented command
 * carrying reserved data, and section 2.5 fixes the framing. Neither
 * permits an implementation to walk off the end of a short packet.
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include <string.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"

/* Every command id worth probing, per lingo. Drawn from the switch
 * statements in apps/iap/iap-lingo*.c plus a few the spec defines that
 * we do not implement, so the default branches get exercised too. */
static const unsigned char lingo0_cmds[] = {
    0x00, 0x01, 0x02, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F,
    0x11, 0x13, 0x14, 0x15, 0x17, 0x18, 0x19, 0x1D, 0x1F, 0x23,
    0x24, 0x26, 0x27, 0x28, 0x29, 0x2B, 0x37, 0x38, 0x39, 0x3B,
    0x3C, 0x49, 0x4B, 0x77, 0xFF,
};
static const unsigned char lingo2_cmds[] = { 0x00, 0x01, 0x03, 0xFF };
static const unsigned char lingo3_cmds[] = {
    0x00, 0x01, 0x03, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x0F,
    0x11, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1C, 0x1E, 0x1F, 0x21,
    0x22, 0xFF,
};
static const unsigned char lingoA_cmds[] = { 0x00, 0x01, 0x02, 0x03, 0x05, 0xFF };

/* Extended Interface has 16-bit command ids. */
static const unsigned short lingo4_cmds[] = {
    0x0000, 0x0001, 0x0002, 0x0007, 0x000C, 0x000E, 0x0010, 0x0012,
    0x0014, 0x0016, 0x0018, 0x001A, 0x001C, 0x001E, 0x0020, 0x0023,
    0x0026, 0x0028, 0x0029, 0x002B, 0x002E, 0x0031, 0x0033, 0x0035,
    0x0037, 0x0039, 0x00FF,
};

/* Filler that is not obviously benign: alternating high bits, and values
 * that would be large counts or indices if read as parameters. */
static const unsigned char filler[] = {
    0xFF, 0x00, 0x80, 0x7F, 0xAA, 0x55, 0xFE, 0x01,
};

/* Packets fed in, and packets examined coming back, for this case. */
static int swept_in, swept_out;

/* If the device answered with an acknowledgement, it has to name the
 * command it is answering. MFi Table 3-4 (p.124) puts the acknowledged
 * command id last in the General iPodAck, and every per-lingo ack has
 * the same shape. Checking only the framing let a mutation that made
 * cmd_ack() answer a fixed command id pass every sweep. */
static void check_ack_names_the_command(const char *what, int cmd,
                                        int len, const struct iaptest_pkt *p)
{
    int lingo = p->paylen >= 1 ? p->payload[0] : -1;
    int idb   = (lingo == 0x04) ? 2 : 1;
    int ackcmd;

    /* Is this an acknowledgement at all? */
    if (lingo == 0x00 && p->paylen >= 2 && p->payload[1] == 0x02)
        ackcmd = 0x00;
    else if (lingo == 0x02 && p->paylen >= 2 && p->payload[1] == 0x01)
        ackcmd = 0x02;
    else if (lingo == 0x03 && p->paylen >= 2 && p->payload[1] == 0x00)
        ackcmd = 0x03;
    else if (lingo == 0x04 && p->paylen >= 3 && p->payload[1] == 0x00
             && p->payload[2] == 0x01)
        ackcmd = 0x04;
    else
        return;                 /* a data reply, not an ack */

    /* lingo + command id + optional transaction id + status, then the
     * command being acknowledged. */
    int tid = (p->paylen == 1 + idb + 1 + idb) ? 0 : 2;
    int at  = 1 + idb + tid + 1;

    if (p->paylen < at + idb)
        return;                 /* short forms are checked elsewhere */

    int named = (idb == 2)
        ? ((p->payload[at] << 8) | p->payload[at + 1])
        : p->payload[at];

    if (named != cmd)
        iaptest_fail(__FILE__, __LINE__,
                     "%s cmd 0x%02X len %d was acknowledged as command "
                     "0x%02X; an ack must name the command it answers",
                     what, cmd, len, named);
    (void)ackcmd;
}

static void check_output_wellformed(const char *what, int cmd, int len)
{
    swept_out += iaptest_tx_count();

    /* Four properties per packet: checksum, length form, minimum
     * payload, and that an ack names the command it answers. Counted,
     * because they are reported through iaptest_fail() rather than
     * CHECK and would otherwise be invisible to the zero-assertion
     * guard -- this helper is the only verification several cases in
     * this file do. */
    iaptest_checked(4 * iaptest_tx_count());

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);

        if (!p->checksum_ok) {
            iaptest_fail(__FILE__, __LINE__,
                         "%s cmd 0x%02X len %d produced a bad checksum",
                         what, cmd, len);
            iaptest_hexdump("raw", p->raw, p->rawlen);
            return;
        }
        if (!p->length_form_ok) {
            iaptest_fail(__FILE__, __LINE__,
                         "%s cmd 0x%02X len %d used an illegal length form "
                         "(payload %d)", what, cmd, len, p->paylen);
            return;
        }
        if (p->paylen < 2) {
            iaptest_fail(__FILE__, __LINE__,
                         "%s cmd 0x%02X len %d sent a %d byte payload; the "
                         "shortest legal packet carries a lingo and a command",
                         what, cmd, len, p->paylen);
            return;
        }

        check_ack_names_the_command(what, cmd, len, p);
    }
    iaptest_tx_clear();
}

/* Drive one command at every payload length from 2 up to max. */
static void sweep(const char *what, unsigned char lingo,
                  const unsigned char *cmd_bytes, int cmd_len,
                  int cmd_value, int max)
{
    unsigned char p[32];

    for (int len = cmd_len + 1; len <= max; len++) {
        p[0] = lingo;
        memcpy(p + 1, cmd_bytes, cmd_len);
        for (int i = cmd_len + 1; i < len; i++)
            p[i] = filler[i % (int)sizeof(filler)];

        iaptest_rx(p, len);
        swept_in++;
        check_output_wellformed(what, cmd_value, len);

        /* Some commands raise a button, which makes iap_handlepkt()
         * defer the next packet until the driver samples. */
        iaptest_button_sample(4);
    }
}

static void sweep_lingo(const char *what, unsigned char lingo,
                        const unsigned char *cmds, int n)
{
    /* Checked against the stimulus, not against what the caller said.
     * sweep_begin() takes the caller's word for whether this sweep
     * drives the model's own state machine, and a wrong word there is
     * exactly how 1631 assertions went missing once already -- so the
     * lingo actually being swept has to agree with it. Only lingo 0
     * carries the commands that desynchronise the model. */
    CHECK(iapacc_is_attached() == (lingo != 0x00),
          "%s sweeps lingo 0x%02X with the accessory model %s; only the "
          "lingo 0 sweeps may stand it down, and they must",
          what, lingo, iapacc_is_attached() ? "attached" : "detached");

    for (int i = 0; i < n; i++) {
        unsigned char c = cmds[i];
        sweep(what, lingo, &c, 1, c, 12);
    }
}

static void sweep_lingo4(const char *what)
{
    for (unsigned i = 0; i < sizeof(lingo4_cmds)/sizeof(lingo4_cmds[0]); i++) {
        unsigned char cb[2] = { (unsigned char)(lingo4_cmds[i] >> 8),
                                (unsigned char)(lingo4_cmds[i] & 0xFF) };
        sweep(what, 0x04, cb, 2, lingo4_cmds[i], 12);
    }
}

/* Record the sweep so a case that silently stopped feeding packets is
 * not mistaken for a clean run. */
static bool sweep_touches_lingo0;

static void sweep_begin(bool touches_lingo0)
{
    /* These cases send deliberately malformed packets, including
     * truncated StartIDPS and IdentifyDeviceLingoes. Those are the
     * commands that drive the accessory model's own transaction-ID
     * state machine, so after one of them the model and the firmware
     * legitimately disagree about whether transaction IDs are in force
     * and it rejects perfectly correct replies.
     *
     * Which is why the two General sweeps call iapacc_detach() at their
     * own call sites and the other seven do not: those never touch
     * lingo 0, so the model stays attached and judges every reply. The
     * framing and checksum checks apply to all nine either way.
     *
     * Which of the two a sweep is, is now an argument rather than a
     * convention: touches_lingo0 stands the model down, and anything
     * else is judged and must prove it in sweep_end(). One commit in
     * this series detached the model for all nine callers and removed
     * 1631 assertions with every suite still green -- the argument is
     * so that cannot happen quietly again. */
    sweep_touches_lingo0 = touches_lingo0;
    if (touches_lingo0)
        iapacc_detach();
    swept_in = 0;
    swept_out = 0;
}

static void sweep_end(int least)
{
    /* A sweep that did not stand the model down has to have been
     * judged by it. Running the tests cannot tell a sweep that passes
     * from one that checks nothing, so this asks directly. */
    if (!sweep_touches_lingo0) {
        CHECK(iapacc_is_attached(),
              "the accessory model was detached for a sweep that does "
              "not touch lingo 0, so its assertions did not run");
        CHECK(iapacc_judged() > 0,
              "the accessory model judged no replies in this sweep");
    }

    CHECK(swept_in >= least,
          "only %d malformed packets were fed in, expected at least %d",
          swept_in, least);
    CHECK(swept_out > 0,
          "the firmware answered none of %d malformed packets, so nothing "
          "was actually validated", swept_in);
}

/* ------------------------------------------------------------------ */
/* Legacy accessory                                                    */
/* ------------------------------------------------------------------ */

void test_malformed_legacy_general(void)
{
    sweep_begin(true);
    iaptest_identify_legacy(0x0000041D);
    sweep_lingo("legacy general", 0x00,
                lingo0_cmds, sizeof(lingo0_cmds));
    sweep_end(50);
}

void test_malformed_legacy_remotes(void)
{
    sweep_begin(false);
    iaptest_identify_legacy(0x0000041D);
    iaptest_force_authenticated();
    sweep_lingo("legacy simple remote", 0x02,
                lingo2_cmds, sizeof(lingo2_cmds));
    sweep_lingo("legacy display remote", 0x03,
                lingo3_cmds, sizeof(lingo3_cmds));
    sweep_end(50);
}

void test_malformed_legacy_extended(void)
{
    sweep_begin(false);
    iaptest_identify_legacy(0x0000041D);
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05);
    iaptest_tx_clear();
    sweep_lingo4("legacy extended interface");
    sweep_end(50);
}

void test_malformed_legacy_digital_audio(void)
{
    sweep_begin(false);
    iaptest_identify_legacy(0x0000041D);
    iaptest_force_authenticated();
    sweep_lingo("legacy digital audio", 0x0A,
                lingoA_cmds, sizeof(lingoA_cmds));
    sweep_end(50);
}

/* ------------------------------------------------------------------ */
/* IDPS accessory -- the offsets that matter                           */
/* ------------------------------------------------------------------ */

void test_malformed_idps_general(void)
{
    iapacc_detach();
    sweep_begin(true);
    iaptest_enter_idps();
    sweep_lingo("idps general", 0x00,
                lingo0_cmds, sizeof(lingo0_cmds));
    sweep_end(50);
}

void test_malformed_idps_remotes(void)
{
    sweep_begin(false);
    iaptest_enter_idps();
    iaptest_force_authenticated();
    sweep_lingo("idps simple remote", 0x02,
                lingo2_cmds, sizeof(lingo2_cmds));
    sweep_lingo("idps display remote", 0x03,
                lingo3_cmds, sizeof(lingo3_cmds));
    sweep_end(50);
}

void test_malformed_idps_extended(void)
{
    sweep_begin(false);
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x60);
    iaptest_tx_clear();
    sweep_lingo4("idps extended interface");
    sweep_end(50);
}

void test_malformed_idps_digital_audio(void)
{
    sweep_begin(false);
    iaptest_enter_idps();
    iaptest_force_authenticated();
    sweep_lingo("idps digital audio", 0x0A,
                lingoA_cmds, sizeof(lingoA_cmds));
    sweep_end(50);
}

/* An unnegotiated lingo must be rejected cleanly at every length. */
void test_malformed_unnegotiated_lingoes(void)
{
    sweep_begin(false);
    iaptest_identify_legacy(0x00000001);    /* General only */
    sweep_lingo("unnegotiated display remote", 0x03,
                lingo3_cmds, sizeof(lingo3_cmds));
    sweep_lingo4("unnegotiated extended interface");
    sweep_end(50);
}

/* ------------------------------------------------------------------ */
/* Bursts                                                             */
/* ------------------------------------------------------------------ */

/* The RX buffer holds several packets at once and is shifted down with
 * a memmove after each one is consumed (apps/iap/iap-core.c). Feeding
 * packets one at a time never exercises that. These push a burst in
 * before draining, so the shift runs with a non-empty tail.
 *
 * Both cases count the replies. Without that they could not see the
 * thing they exist for: a mutation audit shifted the buffer by the
 * wrong amount, 47 of 70 queued packets were silently discarded and the
 * corrupt-length resync fired 16 times, and both cases still reported
 * ok -- because a dropped packet produces no reply, and a loop that
 * only validates the replies it finds has nothing to complain about. */

extern void iap_handlepkt(void);

/* Frame a payload straight into the RX state machine without draining,
 * so several accumulate. iaptest_rx() would handle each immediately. */
static void queue_only(const unsigned char *payload, int len)
{
    unsigned char frame[64];
    int n = 0, sum = len, i;

    frame[n++] = 0xFF;
    frame[n++] = 0x55;
    frame[n++] = (unsigned char)len;
    for (i = 0; i < len; i++) {
        frame[n++] = payload[i];
        sum += payload[i];
    }
    frame[n++] = (unsigned char)(0x100 - (sum & 0xFF));

    iapacc_note_sent(payload, len);

    for (i = 0; i < n; i++)
        iap_getc(frame[i]);
}

void test_burst_of_queued_packets(void)
{

    static const unsigned char q1[] = { 0x00, 0x07 };                 /* name */
    static const unsigned char q2[] = { 0x00, 0x09 };                 /* version */
    static const unsigned char q3[] = { 0x00, 0x0B };                 /* serial */
    static const unsigned char q4[] = { 0x03, 0x04 };                 /* EQ count */
    static const unsigned char q5[] = { 0x00, 0x77, 0xAA, 0xBB };     /* unknown */

    iaptest_identify_legacy(0x0000001D);
    iaptest_force_authenticated();

    for (int round = 0; round < 8; round++) {
        queue_only(q1, sizeof(q1));
        queue_only(q2, sizeof(q2));
        queue_only(q3, sizeof(q3));
        queue_only(q4, sizeof(q4));
        queue_only(q5, sizeof(q5));

        iap_handlepkt();
        iaptest_button_sample(4);
        iap_handlepkt();

        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (!p->checksum_ok || !p->length_form_ok || p->paylen < 2) {
                iaptest_fail(__FILE__, __LINE__,
                             "burst round %d packet %d is malformed "
                             "(paylen %d, checksum %s)",
                             round, i, p->paylen,
                             p->checksum_ok ? "ok" : "bad");
                return;
            }
        }

        /* Every one of the five must have been answered: four queries
         * and the unknown command, which earns an iPodAck. */
        CHECK(iaptest_tx_count() == 5,
              "burst round %d: %d replies to five queued packets",
              round, iaptest_tx_count());

        static const unsigned char want[5][2] = {
            { 0x00, 0x08 },     /* RetiPodName */
            { 0x00, 0x0A },     /* RetiPodSoftwareVersion */
            { 0x00, 0x0C },     /* RetiPodSerialNumber */
            { 0x03, 0x05 },     /* RetNumEQProfiles */
            { 0x00, 0x02 },     /* iPodAck for the unknown command */
        };
        for (int i = 0; i < 5 && i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            CHECK(p->paylen >= 2 && p->payload[0] == want[i][0]
                  && p->payload[1] == want[i][1],
                  "burst round %d reply %d: got %02X %02X, want %02X %02X "
                  "-- the queue is out of step", round, i,
                  p->paylen > 0 ? p->payload[0] : 0,
                  p->paylen > 1 ? p->payload[1] : 0,
                  want[i][0], want[i][1]);
        }
        iaptest_tx_clear();
    }

    /* The link must still work normally afterwards. */
    IAPTEST_RX(0x00, 0x07);
    EXPECT_PAYLOAD(0, 0x00, 0x08, 'R','O','C','K','B','O','X', 0x00);
}

/* Same, but with the transaction IDs in play so the shift happens with
 * the two-byte offset active on every parse. */
void test_burst_of_queued_packets_idps(void)
{

    iaptest_enter_idps();
    iaptest_force_authenticated();

    for (int round = 0; round < 8; round++) {
        unsigned char a[] = { 0x00, 0x07, 0x00, (unsigned char)round };
        unsigned char b[] = { 0x00, 0x09, 0x01, (unsigned char)round };
        unsigned char c[] = { 0x03, 0x04, 0x02, (unsigned char)round };

        queue_only(a, sizeof(a));
        queue_only(b, sizeof(b));
        queue_only(c, sizeof(c));

        iap_handlepkt();
        iaptest_button_sample(4);
        iap_handlepkt();

        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (!p->checksum_ok || p->paylen < 4) {
                iaptest_fail(__FILE__, __LINE__,
                             "idps burst round %d packet %d is malformed",
                             round, i);
                return;
            }
        }
        /* Three queued, three answered, each echoing the id it was
         * given -- which the round number makes unique. */
        CHECK(iaptest_tx_count() == 3,
              "idps burst round %d: %d replies to three queued packets",
              round, iaptest_tx_count());
        for (int i = 0; i < 3 && i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (p->paylen >= 4) {
                CHECK(p->payload[2] == i && p->payload[3] == round,
                      "idps burst round %d reply %d echoed transaction "
                      "id %02X%02X, want %02X%02X", round, i,
                      p->payload[2], p->payload[3], i, round);
            }
        }
        iaptest_tx_clear();
    }

    IAPTEST_RX(0x00, 0x07, 0xAB, 0xCD);
    EXPECT_PAYLOAD(0, 0x00, 0x08, 0xAB, 0xCD,
                   'R','O','C','K','B','O','X', 0x00);
}

/* ------------------------------------------------------------------ */
/* SetFIDTokenValues                                                   */
/* ------------------------------------------------------------------ */

/* Each token can consume one input byte while producing four or five
 * output bytes, so a short command declaring many tokens used to build a
 * reply past TX_BUFLEN and hit panicf("IAP: TX buffer overflow"), which
 * halts the player. panicf() abort()s in this harness, so a regression
 * kills the run outright rather than reporting.
 *
 * MFi Table 3-65 (p.160): "If the number of token-value fields the Apple
 * device parses from the command doesn't match this value, the Apple
 * device returns a nonzero iPodAck and accepts no token-value fields." */
static void fid_tokens(int declared, const unsigned char *body, int bodylen)
{
    unsigned char p[400];
    int n = 0;

    p[n++] = 0x00;
    p[n++] = 0x39;
    p[n++] = 0x00;
    p[n++] = 0x05;
    p[n++] = (unsigned char)declared;
    if (bodylen > (int)sizeof(p) - n)
        bodylen = (int)sizeof(p) - n;
    memcpy(p + n, body, bodylen);
    n += bodylen;

    iaptest_rx(p, n);
}

void test_fid_tokens_cannot_overflow_the_tx_buffer(void)
{

    unsigned char body[380];

    iaptest_enter_idps();

    /* 255 zero-length tokens in 129 bytes: the original crash. */
    memset(body, 0x00, sizeof(body));
    iaptest_tx_clear();
    fid_tokens(255, body, 129);
    check_output_wellformed("255 zero-length tokens", 0x39, 134);

    /* And at every declared count across the range. */
    for (int declared = 1; declared <= 255; declared += 7) {
        memset(body, 0x00, sizeof(body));
        iaptest_tx_clear();
        fid_tokens(declared, body, 300);
        check_output_wellformed("zero-length tokens", 0x39, declared);
    }

    /* Tokens whose length runs past the end of the packet. */
    for (int tlen = 0; tlen < 40; tlen++) {
        body[0] = (unsigned char)tlen;
        body[1] = 0x00;
        body[2] = 0x00;
        iaptest_tx_clear();
        fid_tokens(1, body, 3);
        check_output_wellformed("token longer than the packet", 0x39, tlen);
    }

    CHECK(rbstub_calls.panics == 0, "the firmware panicked");
}

/* A malformed set must be refused wholesale, not partly accepted. */
void test_fid_tokens_malformed_set_is_refused(void)
{

    unsigned char body[8];

    iaptest_enter_idps();
    iaptest_tx_clear();

    /* Declares three tokens, supplies one valid then runs out. */
    body[0] = 0x02; body[1] = 0x00; body[2] = 0x01;
    fid_tokens(3, body, 3);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no answer to a malformed SetFIDTokenValues");
    if (!p || p->paylen < 6)
        return;

    CHECK_EQ_INT(p->payload[0], 0x00, "reply lingo");
    CHECK_EQ_INT(p->payload[1], 0x02, "must be an iPodAck, not AckFIDTokenValues");
    CHECK(p->payload[4] != 0x00,
          "MFi Table 3-65 requires a nonzero status on a count mismatch, "
          "got 0x%02X", p->payload[4]);

    iaptest_tx_clear();
    body[0] = 0x02; body[1] = 0x00; body[2] = 0x02;
    fid_tokens(1, body, 3);

    p = iaptest_tx(0);
    CHECK(p && p->paylen >= 6 && p->payload[1] == 0x02
          && p->payload[4] != 0x00 && p->payload[5] == 0x39,
          "an AccessoryInfoToken without accInfoType was accepted");
}

/* The valid path must be untouched: one IdentifyToken declaring five
 * lingoes, which is what iaptest_enter_idps() sends. */
void test_fid_tokens_valid_set_still_accepted(void)
{
    iaptest_enter_idps();
    CHECK(device.auth.idps,
          "a well-formed IDPS exchange no longer completes");
    CHECK((device.lingoes & (1u << 0x04)) != 0,
          "the IdentifyToken lingo list was not accepted");
}

/* ------------------------------------------------------------------ */
/* Truncated Extended Interface commands must be refused               */
/* ------------------------------------------------------------------ */

/* iap_handlepkt() never clears the RX tail after its memmove, so a read
 * past a short packet returns the previous packet's bytes -- content the
 * accessory chose. A truncated command that skipped its length check
 * therefore replayed the last one's parameters and still acked OK.
 *
 * Each entry is a command and the shortest payload MFi allows for it. */
void test_extended_interface_rejects_short_commands(void)
{
    static const struct { unsigned short cmd; int min; const char *name; } t[] = {
        { 0x0017,  8, "SelectDBRecord" },
        { 0x0018,  4, "GetNumberCategorizedDBRecords" },
        { 0x001A, 12, "RetrieveCategorizedDatabaseRecords" },
        { 0x0020,  7, "GetIndexedPlayingTrackTitle" },
        { 0x0026,  4, "SetPlayStatusChangeNotification" },
        { 0x0028,  7, "PlayCurrentSelection" },
        { 0x0029,  4, "PlayControl" },
        { 0x002E,  4, "SetShuffle" },
        { 0x0031,  4, "SetRepeat" },
        { 0x0037,  7, "SetCurrentPlayingTrack" },
        { 0x0038,  8, "SelectSortDBRecord" },
        { 0x003B,  4, "ResetDBSelectionHierarchy" },
    };

    iapacc_detach();

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        /* Prime the buffer with a long, valid-looking packet so the
         * residue a short one would read is distinctive. */
        iaptest_enter_idps();
        iaptest_force_authenticated();
        IAPTEST_RX(0x00, 0x05, 0x00, 0x10);
        /* Entering the mode while playing raises BUTTON_RC_PLAY
         * (iap_interface_state_change), and iap_handlepkt() defers the
         * next packet while a button is pending -- so without draining
         * it the primer below is re-queued and the reply read further
         * down belongs to something else. This became visible once
         * iap_reset_device() started clearing interface_state: the mode
         * entry used to be a no-op because a previous case had left the
         * mode set. */
        iaptest_button_sample(4);
        iaptest_tx_clear();

        unsigned char primer[16] = { 0x04,
                                     (unsigned char)(t[i].cmd >> 8),
                                     (unsigned char)(t[i].cmd & 0xFF),
                                     0x00, 0x20 };
        for (int b = 5; b < 16; b++)
            primer[b] = 0xA5;
        iaptest_rx(primer, sizeof(primer));
        iaptest_button_sample(4);
        iaptest_tx_clear();

        /* Now the same command one byte shorter than the spec allows.
         * doff is 2 here, so the minimum on the wire is min + 2. */
        int shortlen = t[i].min + 2 - 1;
        unsigned char p[16] = { 0x04,
                                (unsigned char)(t[i].cmd >> 8),
                                (unsigned char)(t[i].cmd & 0xFF),
                                0x00, 0x21 };
        for (int b = 5; b < shortlen; b++)
            p[b] = 0x00;
        iaptest_rx(p, shortlen);

        const struct iaptest_pkt *r = iaptest_tx(0);
        if (!r) {
            iaptest_button_sample(4);
            continue;               /* silence is a safe refusal */
        }

        /* An Extended Interface iPodAck is 0x04 0x0001. Anything else
         * means the command was carried out on truncated parameters. */
        bool is_ack = (r->paylen >= 3 && r->payload[0] == 0x04
                       && r->payload[1] == 0x00 && r->payload[2] == 0x01);
        CHECK(is_ack,
              "%s truncated to %d bytes was answered with command "
              "0x%02X%02X instead of being refused; it ran on whatever "
              "the previous packet left in the buffer",
              t[i].name, shortlen,
              r->paylen >= 2 ? r->payload[1] : 0,
              r->paylen >= 3 ? r->payload[2] : 0);

        if (is_ack && r->paylen >= 6)
            CHECK(r->payload[5] != 0x00,
                  "%s truncated to %d bytes was acked with Success",
                  t[i].name, shortlen);

        iaptest_button_sample(4);
    }
}
