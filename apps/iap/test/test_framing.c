/***************************************************************************
 * Packet framing.
 *
 * MFi R46 section 2.5.2, p.110:
 *   "A 1-byte field can express a payload length of 0x02 to 0xFC (2 to 252)
 *    ... A 3-byte field contains a 0x00 marker byte followed by a 2-byte
 *    payload length value from 0x00FD to 0xFFFA (253 to 65529)."
 *
 * MFi R46 section 2.5.3, p.110:
 *   "The checksum value is calculated by adding together the values of the
 *    length, lingoID, commandID, and transID fields, plus the values of all
 *    command parameters."
 ****************************************************************************/

#include "iap_test.h"

#include <string.h>

#include "config.h"
#include "iap.h"

void test_framing_checksum_is_valid(void)
{
    iaptest_identify_legacy(0x0000000D);

    IAPTEST_RX(0x00, 0x07);     /* RequestiPodName */

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to RequestiPodName");
    if (!p)
        return;
    CHECK(p->checksum_ok,
          "reply checksum does not sum to zero (MFi 2.5.3)");
}

/* The boundary between the two length forms. iap_send_tx() picks the form,
 * so drive it through the public iap_send_pkt() entry point at each length
 * either side of the 252-byte limit. */
static void check_length_form(int paylen)
{
    unsigned char buf[300];

    memset(buf, 0x5A, sizeof(buf));
    buf[0] = 0x00;      /* lingo   */
    buf[1] = 0x08;      /* command */

    iaptest_tx_clear();
    iap_send_pkt(buf, paylen);

    const struct iaptest_pkt *p = iaptest_tx(0);
    if (!p) {
        CHECK(false, "nothing transmitted for a %d byte payload", paylen);
        return;
    }

    CHECK(p->checksum_ok, "bad checksum on a %d byte payload", paylen);

    bool used_short_form = (p->rawlen >= 3 && p->raw[2] != 0x00);
    bool should_be_short = (paylen <= 0xFC);

    CHECK(used_short_form == should_be_short,
          "%d byte payload used the %s length form; MFi 2.5.2 allows the "
          "1-byte form only for 0x02..0xFC (2..252)",
          paylen, used_short_form ? "1-byte" : "3-byte");

    CHECK_EQ_INT(p->paylen, paylen, "decoded payload length");
}

void test_framing_length_form_boundary(void)
{
    iaptest_identify_legacy(0x0000000D);

    check_length_form(252);   /* last legal short-form length */
    check_length_form(253);   /* first that must use the long form */
    check_length_form(254);
    check_length_form(255);
    check_length_form(256);   /* already handled correctly today */
}

/* A reply must never claim a length that disagrees with what was sent. */
void test_framing_length_matches_payload(void)
{
    iaptest_enter_idps();

    IAPTEST_RX(0x00, 0x07, 0x00, 0x05);

    const struct iaptest_pkt *p = iaptest_tx(0);
    CHECK(p != NULL, "no reply to RequestiPodName");
    if (!p)
        return;

    int hdr = (p->raw[2] == 0x00) ? 5 : 3;
    CHECK_EQ_INT(p->rawlen, hdr + p->paylen + 1,
                 "framed size must be header + payload + checksum");
}

/* ------------------------------------------------------------------ */
/* Length-form bounds on the way IN                                     */
/* ------------------------------------------------------------------ */

/* MFi 2.5.2 (p.110): "The length field may contain 1 or 3 bytes,
 * depending on the payload length. A 1-byte field can express a payload
 * length of 0x02 to 0xFC (2 to 252) in a single byte. A 3-byte field
 * contains a 0x00 marker byte followed by a 2-byte payload length value
 * from 0x00FD to 0xFFFA (253 to 65529)."
 *
 * The transmit side has been checked against those bounds since the
 * suite existed (iap_test.c's length_form_ok). The receive framer only
 * ever rejected zero, so a one-byte payload was framed, buffered and
 * dispatched -- and iap_handlepkt_mode0() reads buf[1] for the command
 * before its own length check, so the command byte came from one past
 * the packet, out of the receive buffer's untouched tail.
 *
 * With the right predecessor that byte is non-zero and the handler
 * answers a 4-byte General iPodAck with no transaction ID, which MFi
 * 2.6.1.2 (p.111) makes the signal to stop using transaction IDs:
 * "Support for transaction IDs must be disabled upon receipt of a
 * General lingo iPodAck command without a transaction ID. Such commands
 * have a payload length value (byte 2) of either 0x04 or 0x08."
 *
 * The device keeps sending them; the accessory has stopped expecting
 * them. Every packet after that is parsed two bytes off, for good. */

extern void iap_handlepkt(void);

/* Feed raw bytes straight into the framer, checksum included, without
 * iaptest_rx()'s framing. */
static void feed_raw(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++)
        iap_getc(b[i]);
    iap_handlepkt();
}

#define FEED_RAW(...) do { \
        static const unsigned char _b[] = { __VA_ARGS__ }; \
        feed_raw(_b, (int)sizeof(_b)); \
    } while (0)

void test_framing_rejects_out_of_range_length_forms(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();

    /* Prime the buffer tail with a real packet, so the byte just past a
     * short frame is a plausible command id rather than zero -- which
     * is what makes the one-byte case reach cmd_ack() at all. */
    IAPTEST_RX(0x00, 0x07, 0x00, 0x10);
    iaptest_tx_clear();

    /* A one-byte payload. Checksum over the length and the one byte. */
    FEED_RAW(0xFF, 0x55, 0x01, 0x00, 0xFF);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "a payload length of 1 was dispatched; MFi 2.5.2 gives "
                 "the one-byte form a minimum of 2");

    /* 0xFD, 0xFE and 0xFF must use the three-byte form. */
    for (int n = 0xFD; n <= 0xFF; n++) {
        unsigned char b[4] = { 0xFF, 0x55, (unsigned char)n, 0x00 };
        int sum = n;
        b[3] = (unsigned char)(0x100 - (sum & 0xFF));
        iaptest_tx_clear();
        feed_raw(b, sizeof(b));
        CHECK_EQ_INT(iaptest_tx_count(), 0,
                     "a one-byte length field above 0xFC was accepted");
    }

    /* And the three-byte form below its own minimum of 0x00FD. */
    for (int n = 1; n <= 4; n++) {
        /* sync, start, the 0x00 marker, two length bytes, n payload
         * bytes and a checksum: 5 + n + 1, so 10 at n = 4. */
        unsigned char b[16] = { 0xFF, 0x55, 0x00, 0x00, (unsigned char)n };
        int sum = n, i, k = 5;
        for (i = 0; i < n; i++) { b[k] = 0x00; sum += b[k++]; }
        b[k++] = (unsigned char)(0x100 - (sum & 0xFF));
        iaptest_tx_clear();
        feed_raw(b, k);
        CHECK_EQ_INT(iaptest_tx_count(), 0,
                     "a three-byte length field below 0x00FD was accepted");
    }

    /* The link must still work afterwards: rejecting a bad length must
     * resynchronise, not wedge the framer. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x07, 0x00, 0x11);
    EXPECT_PAYLOAD(0, 0x00, 0x08, 0x00, 0x11,
                   'R','O','C','K','B','O','X', 0x00);

    /* And nothing above may ever have produced the teardown ack. */
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p->paylen >= 2 && p->payload[0] == 0x00 && p->payload[1] == 0x02)
            CHECK(p->paylen != 4 && p->paylen != 8,
                  "a General iPodAck of %d bytes went out while "
                  "transaction IDs were in force; MFi 2.6.1.2 makes that "
                  "an instruction to stop using them", p->paylen);
    }
}

/* The two-byte payload the spec does allow must still work. */
void test_framing_accepts_the_minimum_payload(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();
    iaptest_tx_clear();

    /* Lingo 0, command 0x07: two bytes, the shortest legal payload.
     * Under IDPS it is missing its transaction ID, so the reply is a
     * rejection -- but a rejection is a reply, which proves the frame
     * was accepted rather than dropped by the length check. */
    FEED_RAW(0xFF, 0x55, 0x02, 0x00, 0x07, 0xF7);
    CHECK(iaptest_tx_count() > 0,
          "a two-byte payload was rejected by the framer; MFi 2.5.2 "
          "makes 2 the minimum the one-byte form expresses");
}

/* MFi 2.5.3 (p.110): "The checksum is calculated by summing the packet
 * payload length, the payload bytes, and the checksum byte, and
 * verifying that the low-order byte of the result is zero."
 *
 * Nothing tested that the device does the verifying.
 * test_framing_checksum_is_valid checks the checksum of what the device
 * SENDS, and iaptest_rx() computes the one it feeds in, so no case
 * could present a bad one. Turning the gate in iap_getc() into
 * "if (1)" -- every corrupt frame dispatched -- left all seven binaries
 * green. */
void test_framing_a_bad_checksum_is_not_dispatched(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();

    /* RequestExtendedInterfaceMode, which answers with a packet, so a
     * frame that gets through is unmistakable. Payload 00 03 plus a
     * transaction ID; length 4, checksum = -(4+0+3+0+0) = 0xF9. */
    iaptest_tx_clear();
    FEED_RAW(0xFF, 0x55, 0x04, 0x00, 0x03, 0x00, 0x00, 0xF9);
    CHECK(iaptest_tx_count() > 0,
          "the well-formed frame was not answered, so the corrupt one "
          "below would prove nothing");

    /* The same frame with one payload byte flipped and the checksum
     * left alone. */
    iaptest_tx_clear();
    FEED_RAW(0xFF, 0x55, 0x04, 0x00, 0x03, 0x00, 0x01, 0xF9);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "a frame whose checksum does not close was handled");

    /* And a flipped checksum byte, which is the same rule from the
     * other side. */
    iaptest_tx_clear();
    FEED_RAW(0xFF, 0x55, 0x04, 0x00, 0x03, 0x00, 0x00, 0xF8);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "a frame with a corrupt checksum byte was handled");

    /* The framer has to resynchronise, not wedge. */
    iaptest_tx_clear();
    FEED_RAW(0xFF, 0x55, 0x04, 0x00, 0x03, 0x00, 0x00, 0xF9);
    CHECK(iaptest_tx_count() > 0,
          "the framer did not recover from a bad checksum");
}

/* The buffer bound in iap_getc()'s one-byte length arm. Its own comment
 * records that it replaced a signed comparison which wrapped once the
 * buffer filled to within one byte, and then accepted every frame --
 * writing past the end. Nothing tested it: deleting the bound leaves
 * all seven binaries green, and the mutant aborts on this case. */
void test_framing_a_full_buffer_stops_accepting(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    iaptest_detach_model_for_raw_probes();

    /* Fill the RX buffer without draining it: iap_getc() completes each
     * frame and posts, but nothing calls iap_handlepkt(). A 0x00/0x03
     * payload is two bytes plus the length and checksum. */
    int posted = 0;
    /* RX_BUFLEN is 64 KB and each of these frames consumes four bytes
     * of it, so the bound is reached after about sixteen thousand. */
    for (int i = 0; i < 20000; i++) {
        static const unsigned char f[] =
            { 0xFF, 0x55, 0x02, 0x00, 0x03, 0xFB };
        int before = rbstub_calls.queue_post;
        for (unsigned k = 0; k < sizeof(f); k++)
            iap_getc(f[k]);
        if (rbstub_calls.queue_post != before)
            posted++;
        else
            break;      /* the bound has started refusing */
    }

    CHECK(posted > 0, "no frame was accepted at all");
    CHECK(posted < 20000,
          "the framer accepted %d frames without draining one; the "
          "buffer bound never refused", posted);

    /* And once it refuses, it keeps refusing rather than writing past
     * the end. */
    int before = rbstub_calls.queue_post;
    for (int i = 0; i < 32; i++) {
        static const unsigned char f[] =
            { 0xFF, 0x55, 0x02, 0x00, 0x03, 0xFB };
        for (unsigned k = 0; k < sizeof(f); k++)
            iap_getc(f[k]);
    }
    CHECK_EQ_INT(rbstub_calls.queue_post, before,
                 "the framer accepted more frames after the buffer was "
                 "full");
}
