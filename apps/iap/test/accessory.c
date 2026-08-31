/***************************************************************************
 * A model of a conformant accessory. See accessory.h.
 *
 * Every rule below is cited to the MFi Accessory Firmware Specification
 * R46 (2012-09-12). Line numbers are into /tmp/mfi.txt, the layout text
 * extraction, alongside the spec's own section and page.
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "iap.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define ACC_MAX_OUTSTANDING 32

struct outstanding {
    unsigned char lingo;
    unsigned short command;
    unsigned short trans;
    bool           in_use;
};

static struct {
    bool attached;
    /* Replies the model actually judged. A harness change that stops it
     * judging cannot be seen by running the tests -- they all still
     * pass, because nothing is being checked. One commit in this series
     * detached the model for seven sweeps that should have been judged
     * and removed 1631 assertions without a single failure. */
    int  judged;
    bool trans_enabled;         /* our side is sending transaction IDs */
    unsigned short next_trans;  /* 2.6.1.1: counter starts at 0x0000 */
    struct outstanding out[ACC_MAX_OUTSTANDING];

    bool           saw_device_trans;
    unsigned short last_device_trans;

    /* Packets the test sent that were too short to carry the
     * transaction id they owed. Each earns one unmatched reply. */
    int            malformed_sends;

    int  violations;
    char first[320];

    /* Replies this accessory owes the device. A real accessory answers
     * the commands the device sends it; the model used only to judge
     * them, so every flow was half a conversation and the device's own
     * handling of a response was never exercised. Queued rather than
     * sent inline, because iapacc_observe() runs inside iap_send_tx(),
     * inside iap_handlepkt() -- answering there would re-enter the
     * protocol layer mid-transmission, which no transport does.
     * iapacc_pump() delivers them afterwards. */
    bool autorespond;
    int  responses;             /* how many the responder has queued */
    int  nq;
    struct { unsigned char buf[64]; int len; } q[ACC_MAX_OUTSTANDING];
} acc;

int  iapacc_judged(void)      { return acc.judged; }
bool iapacc_is_attached(void) { return acc.attached; }


void iapacc_attach(void) { acc.attached = true; }
void iapacc_autorespond(bool on) { acc.autorespond = on; }
void iapacc_detach(void) { acc.attached = false; }
int  iapacc_violations(void) { return acc.violations; }
bool iapacc_transactions_enabled(void) { return acc.trans_enabled; }

const char *iapacc_first_violation(void)
{
    return acc.violations ? acc.first : "";
}

void iapacc_reset(void)
{
    bool was = acc.attached;
    memset(&acc, 0, sizeof(acc));
    acc.attached = was;
}

static void violation(const char *fmt, ...)
{
    va_list ap;
    char msg[320];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (acc.violations == 0)
        snprintf(acc.first, sizeof(acc.first), "%s", msg);
    acc.violations++;

    iaptest_fail(__FILE__, __LINE__, "accessory model: %s", msg);
}

/* ------------------------------------------------------------------ */
/* What the accessory sends                                            */
/* ------------------------------------------------------------------ */

static void respond_to(unsigned char lingo, unsigned short command,
                       const unsigned char *payload, int paylen);

/* MFi Table 2-10 (p.109): the command id is one byte, or two for the
 * Extended Interface lingo, and the transaction id follows it. */
static int cmd_id_bytes(unsigned char lingo)
{
    return (lingo == 0x04) ? 2 : 1;
}

static unsigned short cmd_id_of(const unsigned char *c, unsigned char lingo)
{
    return (cmd_id_bytes(lingo) == 2)
         ? (unsigned short)((c[1] << 8) | c[2])
         : (unsigned short)c[1];
}

static void remember(unsigned char lingo, unsigned short command,
                     unsigned short trans)
{
    for (int i = 0; i < ACC_MAX_OUTSTANDING; i++) {
        if (!acc.out[i].in_use) {
            acc.out[i].lingo = lingo;
            acc.out[i].command = command;
            acc.out[i].trans = trans;
            acc.out[i].in_use = true;
            return;
        }
    }
    /* Ring is full; drop the oldest so a long test does not stop
     * matching. Not a protocol event. */
    memmove(&acc.out[0], &acc.out[1],
            sizeof(acc.out) - sizeof(acc.out[0]));
    acc.out[ACC_MAX_OUTSTANDING - 1].lingo = lingo;
    acc.out[ACC_MAX_OUTSTANDING - 1].command = command;
    acc.out[ACC_MAX_OUTSTANDING - 1].trans = trans;
    acc.out[ACC_MAX_OUTSTANDING - 1].in_use = true;
}

/* 2.6.1.4 (p.112): RequestIdentify (0x00/0x00), Identify (0x00/0x01) and
 * IdentifyDeviceLingoes (0x00/0x13) never carry a transaction id. */
/* Exempt from transaction IDs when the *device* sends it. MFi 2.6.1.4
 * (p.112) names three: RequestIdentify, "sent by the Apple device", and
 * the two accessory responses below. */
static bool exempt(unsigned char lingo, unsigned short command)
{
    return lingo == 0x00
        && (command == 0x00 || command == 0x01 || command == 0x13);
}

/* Exempt when the *accessory* sends it, which is only the two it can
 * actually send. RequestIdentify is Origin: Apple device (3.3.1,
 * p.124), so an accessory sending one is a direction violation -- and
 * if it carries an ID, the reply has to echo it like any other. */
static bool exempt_from_accessory(unsigned char lingo,
                                  unsigned short command)
{
    return lingo == 0x00 && (command == 0x01 || command == 0x13);
}

void iapacc_send(const unsigned char *cmd, int len)
{
    unsigned char frame[IAPTEST_MAX_TXLEN];
    unsigned char lingo = cmd[0];
    int idb = cmd_id_bytes(lingo);
    unsigned short command = cmd_id_of(cmd, lingo);
    int n = 0;

    /* 2.6.1.2 (p.111): support is disabled before sending
     * IdentifyDeviceLingoes, and enabled from StartIDPS. */
    if (lingo == 0x00 && (command == 0x01 || command == 0x13))
        acc.trans_enabled = false;

    memcpy(frame, cmd, 1 + idb);
    n = 1 + idb;

    bool with_trans = acc.trans_enabled && !exempt(lingo, command);
    unsigned short trans = 0;

    if (with_trans) {
        trans = acc.next_trans++;
        frame[n++] = (trans >> 8) & 0xFF;
        frame[n++] = trans & 0xFF;
        remember(lingo, command, trans);
    }

    if (len > 1 + idb) {
        memcpy(frame + n, cmd + 1 + idb, len - (1 + idb));
        n += len - (1 + idb);
    }

    /* 2.6 (p.110): "beginning with and including the StartIDPS command".
     * StartIDPS itself therefore carries one, so enable before sending. */
    if (lingo == 0x00 && command == 0x38 && !acc.trans_enabled) {
        acc.trans_enabled = true;
        /* Rebuild with the id in place. */
        n = 1 + idb;
        trans = acc.next_trans++;
        frame[n++] = (trans >> 8) & 0xFF;
        frame[n++] = trans & 0xFF;
        remember(lingo, command, trans);
        if (len > 1 + idb) {
            memcpy(frame + n, cmd + 1 + idb, len - (1 + idb));
            n += len - (1 + idb);
        }
    }

    iaptest_rx(frame, n);
}

/* Follow a packet a test built itself. iaptest_rx() sends bytes
 * verbatim, so without this the model would have no record of what was
 * asked and would reject every answer. The enable and disable triggers
 * are the ones in 2.6.1.2 (p.111), read off the command id. */
void iapacc_note_sent(const unsigned char *payload, int paylen)
{
    if (!acc.attached || paylen < 2)
        return;

    unsigned char lingo = payload[0];
    int idb = cmd_id_bytes(lingo);
    if (paylen < 1 + idb)
        return;
    unsigned short command = cmd_id_of(payload, lingo);

    /* Not 0x00. MFi p.111 disables transaction IDs "upon receipt of a
     * RequestIdentify command", and 3.3.1 (p.124) makes that command
     * Origin: Apple device -- so the rule fires when the accessory
     * *receives* one, and an accessory sending one is a direction
     * violation rather than a state change. The model had the same
     * confusion the firmware did, which is why it could not see the
     * firmware's. */
    if (lingo == 0x00 && (command == 0x01 || command == 0x13)) {
        acc.trans_enabled = false;
        return;
    }

    if (lingo == 0x00 && command == 0x38)
        acc.trans_enabled = true;

    if (!acc.trans_enabled || exempt_from_accessory(lingo, command))
        return;

    /* A packet too short to carry the id it owes is the accessory
     * misbehaving, not the device. It gets answered with a fabricated
     * 0x0000 (see L0_TX_TRANSID), which matches no real command, so
     * count it and let that many unmatched replies through rather than
     * blaming the device for the test's own malformed input. */
    if (paylen < 1 + idb + 2) {
        acc.malformed_sends++;
        return;
    }

    unsigned short trans = (unsigned short)((payload[1 + idb] << 8)
                                            | payload[1 + idb + 1]);
    remember(lingo, command, trans);
}

/* ------------------------------------------------------------------ */
/* What the accessory checks on everything the device sends            */
/* ------------------------------------------------------------------ */

/* 2.6.1.2 (p.111): "Support for transaction IDs must be disabled upon
 * receipt of a General lingo iPodAck command without a transaction ID.
 * Such commands have a payload length value (byte 2) of either 0x04 or
 * 0x08." */
static bool is_teardown_ack(const unsigned char *p, int paylen)
{
    return paylen >= 2 && p[0] == 0x00 && p[1] == 0x02
        && (paylen == 4 || paylen == 8);
}

/* Commands the device originates rather than answers. Kept deliberately
 * small: only entries taken straight from a spec heading of the form
 * "Origin: Apple device" for a command that answers nothing. */
static bool device_originated(unsigned char lingo, unsigned short command)
{
    if (lingo == 0x03 && command == 0x09) return true;  /* 4.3.12 RemoteEventNotification */
    if (lingo == 0x04 && command == 0x0027) return true;/* PlayStatusChangeNotification */
    if (lingo == 0x00 && command == 0x23) return true;  /* NotifyiPodStateChange */
    if (lingo == 0x00 && command == 0x27) return true;  /* GetAccessoryInfo */
    if (lingo == 0x00 && command == 0x14) return true;  /* GetAccessoryAuthenticationInfo */
    if (lingo == 0x00 && command == 0x17) return true;  /* GetAccessoryAuthenticationSignature */
    if (lingo == 0x0A && command == 0x02) return true;  /* GetAccessorySampleRateCaps */
    if (lingo == 0x0A && command == 0x04) return true;  /* 4.10.9 TrackNewAudioAttributes */

    /* Microphone. The section headings in MFi Appendix C.5 give each
     * command's origin; these are the ones marked "Origin: Apple
     * device". 0x04 AccessoryAck, 0x08 RetAccessoryCaps and 0x0A
     * RetAccessoryCtrl are "Origin: Accessory". */
    if (lingo == 0x01 && (command == 0x05 || command == 0x06
                          || command == 0x07 || command == 0x09
                          || command == 0x0B)) return true;
    if (lingo == 0x05) return true;                     /* C.8, Table C-37 p.548 */

    /* RF Tuner. The command summary at Table 4-111 (p.288) marks every
     * command's direction; these are the ones it gives as "Dev to Acc",
     * which is all the Get* and Set* commands. The rest -- 0x00, 0x02,
     * 0x04, 0x07, 0x0A, 0x0D, 0x10, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D
     * and 0x1F -- are "Acc to Dev" and would be wrong-way if the device
     * sent them. */
    if (lingo == 0x07) {
        switch (command) {
        case 0x01: case 0x03: case 0x05: case 0x06: case 0x08:
        case 0x09: case 0x0B: case 0x0C: case 0x0E: case 0x0F:
        case 0x11: case 0x12: case 0x14: case 0x16: case 0x18:
        case 0x1A: case 0x1C: case 0x1E: case 0x20:
            return true;
        /* Table 4-111 (p.289) marks 0x22-0x24 Reserved, but
         * ipod_remote_tuner.c sends 0x24 as a gain boost -- observed
         * from Apple's own firmware rather than read from R46. Listed
         * here because the model's job is to judge what this device
         * actually transmits: undocumented or not, it is a command we
         * originate and its transaction ID has to be ours. */
        case 0x24:
            return true;
        default:
            return false;
        }
    }
    return false;
}

/* Acknowledgements that do not exist in the direction the device would
 * be sending them. Table C-12 (Microphone) and Table 4-111 (RF Tuner)
 * define command 0x00 / 0x04 as Origin: Accessory, and neither lingo has
 * any Apple-device-originated acknowledgement at all. */
static const char *wrong_direction(unsigned char lingo, unsigned short command)
{
    /* Only the acknowledgement is wrong-way. MFi C.5 (p.533) has the
     * Apple device initiating, so 0x00-0x03, 0x05, 0x06, 0x07, 0x09 and
     * 0x0B are all legitimately device-originated (Table C-12, p.534).
     * Flagging the whole lingo would reject a correct SetAccessoryCtrl. */
    if (lingo == 0x01 && command == 0x04)
        return "Microphone 0x04 is AccessoryAck, Origin: Accessory "
               "(Table C-12, p.534; C.5.2, p.535)";
    if (lingo == 0x07 && command == 0x00)
        return "RF Tuner 0x00 is AccessoryAck, Origin: Accessory "
               "(Table 4-111, 4.7.5)";
    return NULL;
}

/* Match a reply to a command the accessory actually sent.
 *
 * This used to compare the id alone, discard the lingo and never retire
 * an entry, so the last 32 ids the accessory had used were permanently
 * valid answers to anything. A device that stamped every reply with
 * 0x0001 passed all six model-driven cases.
 *
 * Now the lingo has to match too, and the entry is retired once used.
 * The one exception is Table 2-13's pending ack: an iPodAck with status
 * 0x06 is followed by a final one carrying the same id, so a pending
 * reply leaves the entry in place.
 */
/* any_lingo is for the General lingo's iPodAck, which answers commands
 * from lingoes that have no device-originated acknowledgement of their
 * own. MFi 4.10.8 (p.355) is explicit for Digital Audio: an invalid
 * RetAccessorySampleRateCaps means "the Apple device ... sends the
 * accessory an iPodAck command with a negative acknowledgment as the
 * command status", and Table 4-232's only lingo 0x0A acknowledgement,
 * command 0x00, is Origin: Accessory. Requiring the ack's own lingo to
 * match the command's would reject a reply the spec requires. */
static bool match_outstanding(unsigned char lingo, unsigned short trans,
                              bool pending, bool any_lingo)
{
    for (int i = 0; i < ACC_MAX_OUTSTANDING; i++) {
        if (!acc.out[i].in_use)
            continue;
        if (acc.out[i].trans != trans)
            continue;
        if (!any_lingo && acc.out[i].lingo != lingo)
            continue;
        if (!pending)
            acc.out[i].in_use = false;
        return true;
    }
    return false;
}

void iapacc_observe(const unsigned char *payload, int paylen)
{
    if (!acc.attached || paylen < 2)
        return;

    acc.judged++;
    /* Counted for the runner as well. The model judges framing, length
     * form and transaction IDs and reports through violation(), which
     * bumps iaptest_failures and never iaptest_checks -- so a case
     * whose only verification is the model contributed nothing the
     * zero-assertion guard could see. */
    iaptest_checked(3);

    unsigned char lingo = payload[0];
    int idb = cmd_id_bytes(lingo);
    if (paylen < 1 + idb)
        return;
    unsigned short command = cmd_id_of(payload, lingo);

    const char *bad = wrong_direction(lingo, command);
    if (bad) {
        violation("device sent lingo 0x%02X command 0x%0*X: %s",
                  lingo, idb * 2, command, bad);
        return;
    }

    if (acc.autorespond)
        respond_to(lingo, command, payload + 1 + idb, paylen - (1 + idb));

    if (is_teardown_ack(payload, paylen)) {
        /* A real accessory obeys this. Model it, so any packet after
         * this point is judged against transaction IDs being off and the
         * resulting desync shows up rather than passing quietly. */
        if (acc.trans_enabled) {
            violation("device sent a %d byte General iPodAck while "
                      "transaction IDs were in force; MFi 2.6.1.2 makes "
                      "that an instruction to stop using them",
                      paylen);
            acc.trans_enabled = false;
        }
        return;
    }

    if (!acc.trans_enabled) {
        /* Legacy mode was returning here unchecked, which left the top
         * failure mode of this whole series -- the device sending
         * transaction IDs to an accessory that never enabled them --
         * invisible to the model.
         *
         * The General lingo iPodAck is the one reply whose length is
         * fixed regardless of the command: Table 2-11 (p.112) gives the
         * no-transaction form a payload of 0x04, status plus the
         * acknowledged command id. Six means an id was inserted.
         */
        if (paylen >= 2 && lingo == 0x00 && command == 0x02
            && paylen != 4 && paylen != 8)
            violation("legacy accessory received a %d byte General "
                      "iPodAck; MFi Table 2-11 (p.112) gives the form "
                      "without a transaction ID a payload of 4 or 8, so "
                      "the device is stamping IDs on an accessory that "
                      "never enabled them", paylen);
        return;
    }

    if (exempt(lingo, command))
        return;

    if (paylen < 1 + idb + 2) {
        violation("lingo 0x%02X command 0x%0*X came back %d bytes, too "
                  "short to carry the transaction ID that MFi 2.6.1.4 "
                  "requires once IDPS has started",
                  lingo, idb * 2, command, paylen);
        return;
    }

    unsigned short trans =
        (unsigned short)((payload[1 + idb] << 8) | payload[1 + idb + 1]);

    if (device_originated(lingo, command)) {
        /* 2.6.1.1 and Table 2-15: the device runs its own counter. Two
         * consecutive device-originated commands must not share an id.
         *
         * Except during authentication. MFi p.111: "The Apple device
         * does not increment transaction ID values during
         * authentication." The two commands it originates there are
         * GetAccessoryAuthenticationInfo (0x00/0x14) and
         * GetAccessoryAuthenticationSignature (0x00/0x17), and they are
         * meant to share one. The rule still holds either side of the
         * handshake, which is the half that matters -- an ordinary
         * command reusing the handshake's id is a real violation, and
         * this model caught exactly that on the first attempt at
         * implementing the sentence. */
        bool in_handshake = (lingo == 0x00
                             && (command == 0x14 || command == 0x17));

        if (!in_handshake
            && acc.saw_device_trans && trans == acc.last_device_trans)
            violation("device reused transaction ID 0x%04X on two "
                      "consecutive commands it originated "
                      "(lingo 0x%02X command 0x%0*X)",
                      trans, lingo, idb * 2, command);
        acc.saw_device_trans = true;
        acc.last_device_trans = trans;
        return;
    }

    /* Otherwise it is a response, and 2.6.1.1 (p.111) has the accessory
     * ignore any whose id matches no command it sent. */
    bool pending = (lingo == 0x00 && command == 0x02
                    && paylen >= 1 + idb + 2 + 1
                    && payload[1 + idb + 2] == 0x06);

    /* Every acknowledgement comes back on the lingo the command was
     * sent on. This briefly allowed a General iPodAck to answer any
     * lingo, to accommodate a Digital Audio rejection that was going
     * out on the wrong lingo -- which meant the model stopped being
     * able to see the very defect it exists to catch. */
    bool any_lingo = false;

    if (!match_outstanding(lingo, trans, pending, any_lingo)) {
        if (trans == 0x0000 && acc.malformed_sends > 0) {
            acc.malformed_sends--;
            return;
        }
        violation("lingo 0x%02X command 0x%0*X answered with transaction "
                  "ID 0x%04X, which matches no command the accessory "
                  "sent; MFi 2.6.1.1 obliges the accessory to discard it",
                  lingo, idb * 2, command, trans);
    }
}

/* ------------------------------------------------------------------ */
/* Answering the device                                                 */
/* ------------------------------------------------------------------ */

static void enqueue(const unsigned char *cmd, int len)
{
    if (acc.nq >= ACC_MAX_OUTSTANDING || len > (int)sizeof(acc.q[0].buf))
        return;
    memcpy(acc.q[acc.nq].buf, cmd, len);
    acc.q[acc.nq].len = len;
    acc.nq++;
    acc.responses++;
}

/* Autoresponses echo the device command's transaction ID. */
static void respond_to(unsigned char lingo, unsigned short command,
                       const unsigned char *payload, int paylen)
{
    unsigned char c[64];
    int n = 0;

    if (lingo == 0x00 && command == 0x14) {
        /* RetAccessoryAuthenticationInfo, Table 3-27 (p.136):
         * Authentication 1.0's form is a major and a minor version. */
        c[n++] = 0x00; c[n++] = 0x15;
        if (acc.trans_enabled && paylen >= 2) {
            c[n++] = payload[0]; c[n++] = payload[1];
        }
        c[n++] = 0x01; c[n++] = 0x00;
        enqueue(c, n);
        return;
    }
    if (lingo == 0x00 && command == 0x17) {
        /* RetAccessoryAuthenticationSignature, Table 3-31 (p.139): a
         * digital signature of variable length. The device cannot
         * verify it here -- there is no coprocessor -- so the bytes are
         * arbitrary; what is under test is that the device accepts a
         * well-formed response and moves the state machine on. */
        c[n++] = 0x00; c[n++] = 0x18;
        if (acc.trans_enabled && paylen >= 2) {
            c[n++] = payload[0]; c[n++] = payload[1];
        }
        for (int i = 0; i < 20; i++)
            c[n++] = (unsigned char)(0xA0 + i);
        enqueue(c, n);
        return;
    }
    if (lingo == 0x00 && command == 0x27) {
        /* RetAccessoryInfo, 3.3.33 (p.147). The info type the device
         * asked for is echoed back; payload[0] after the id is that
         * type. Answer type 0x00, the capabilities word. */
        c[n++] = 0x00; c[n++] = 0x28;
        if (acc.trans_enabled && paylen >= 2) {
            c[n++] = payload[0]; c[n++] = payload[1];
        }
        c[n++] = 0x00;
        c[n++] = 0x00; c[n++] = 0x00; c[n++] = 0x00; c[n++] = 0x00;
        enqueue(c, n);
        return;
    }
    if (lingo == 0x0A && command == 0x02) {
        /* RetAccessorySampleRateCaps, 4.10.8 (p.355): a count then that
         * many 32-bit rates. Offer the three an accessory of this class
         * really offers. */
        static const unsigned long rates[] = { 32000, 44100, 48000 };
        c[n++] = 0x0A; c[n++] = 0x03;
        if (acc.trans_enabled && paylen >= 2) {
            c[n++] = payload[0]; c[n++] = payload[1];
        }
        for (unsigned i = 0; i < sizeof(rates)/sizeof(rates[0]); i++) {
            c[n++] = (unsigned char)(rates[i] >> 24);
            c[n++] = (unsigned char)(rates[i] >> 16);
            c[n++] = (unsigned char)(rates[i] >> 8);
            c[n++] = (unsigned char)rates[i];
        }
        enqueue(c, n);
        return;
    }
}

int iapacc_responses_sent(void) { return acc.responses; }

int iapacc_pump(void)
{
    int sent = 0;

    /* Take a snapshot: delivering one reply can make the device ask
     * another question, which enqueues onto the same array. */
    while (acc.nq > 0) {
        unsigned char buf[64];
        int len = acc.q[0].len;
        memcpy(buf, acc.q[0].buf, len);
        memmove(&acc.q[0], &acc.q[1], sizeof(acc.q) - sizeof(acc.q[0]));
        acc.nq--;

        iaptest_rx(buf, len);
        sent++;

        if (sent > ACC_MAX_OUTSTANDING) {
            violation("the device and accessory are answering each other "
                      "without end; stopped after %d exchanges", sent);
            break;
        }
    }
    return sent;
}

/* ------------------------------------------------------------------ */
/* Identification helpers                                              */
/* ------------------------------------------------------------------ */

void iapacc_identify_idps(const unsigned char *lingoes, int n)
{
    unsigned char tok[40];
    int i = 0;

    IAPACC_SEND(0x00, 0x38);                    /* StartIDPS */

    /* One IdentifyToken: length, FIDType 0x00, FIDSubtype 0x00, the
     * lingo count, the lingoes, four option bytes, four device-id bytes
     * (Table 3-66, p.161). The length byte does not count itself. */
    tok[i++] = 0x01;                            /* numFIDTokenValues */
    tok[i++] = (unsigned char)(3 + n + 8);
    tok[i++] = 0x00;
    tok[i++] = 0x00;
    tok[i++] = (unsigned char)n;
    memcpy(tok + i, lingoes, n); i += n;
    memset(tok + i, 0, 8); i += 8;

    {
        unsigned char c[48];
        c[0] = 0x00; c[1] = 0x39;
        memcpy(c + 2, tok, i);
        iapacc_send(c, 2 + i);
    }

    IAPACC_SEND(0x00, 0x3B, 0x00);              /* EndIDPS, Continue */
}

void iapacc_identify_legacy(unsigned long mask)
{
    unsigned char c[14] = { 0x00, 0x13 };
    c[2] = (mask >> 24) & 0xFF;
    c[3] = (mask >> 16) & 0xFF;
    c[4] = (mask >>  8) & 0xFF;
    c[5] = mask & 0xFF;
    iapacc_send(c, sizeof(c));
}
