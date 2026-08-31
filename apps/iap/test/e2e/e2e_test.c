/***************************************************************************
 * End to end: a real accessory on the wire.
 *
 * The protocol suite drives iap_handlepkt() directly and captures what
 * the protocol layer hands its transport. The transport suite drives
 * the transport with synthetic frames. Neither joins the two, so
 * nothing had ever checked that a command arriving as HID reports comes
 * back out as HID reports the accessory can reassemble.
 *
 * This builds the same protocol layer and the same
 * firmware/usbstack/usb_iap_hid.c the firmware does, wires them
 * together, and speaks only in HID reports -- which is what an
 * accessory on a dock connector actually sees.
 *
 * The loop:
 *   accessory writes an OUT report  ->  iap_hid_process_rx()
 *      -> iap_getc() x N            ->  iap_handlepkt()
 *      -> a handler replies         ->  iap_send_tx()
 *      -> iap_transport_send        ->  iap_hid_tx()
 *      -> usb_drv_send_nonblocking  ->  recorded IN reports
 *   which the accessory then reassembles and checksums.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"
#include "appevents.h"
#include "iap_test.h"
#include "e2e.h"

int  failures;
int  checks;
const char *current = "?";

static void fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    const char *base = strrchr(file, '/');
    failures++;
    fprintf(stderr, "  FAIL [%s] %s:%d\n        ",
            current, base ? base + 1 : file, line);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

#define CHECK(cond, ...) do { \
        checks++; if (!(cond)) fail(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)
#define CHECK_EQ(got, want, what) do { \
        checks++; long _g = (long)(got), _w = (long)(want); \
        if (_g != _w) fail(__FILE__, __LINE__, \
            "%s: got %ld (0x%lX), want %ld (0x%lX)", \
            (what), _g, (unsigned long)_g, _w, (unsigned long)_w); \
    } while (0)

extern void iap_handlepkt(void);

/* ------------------------------------------------------------------ */
/* The accessory                                                       */
/* ------------------------------------------------------------------ */

/* OUT report 9 carries 63 bytes, of which the link control byte takes
 * one. Send an iAP command the way a real accessory does: frame it,
 * drop the sync byte, split it, and write each piece as a SET_REPORT. */
static void acc_send(const unsigned char *payload, int paylen)
{
    unsigned char frame[600];
    int n = 0, sum = 0, i, off;

    frame[n++] = 0x55;
    if (paylen <= 0xFC) {
        frame[n++] = (unsigned char)paylen;
        sum = paylen;
    } else {
        frame[n++] = 0x00;
        frame[n++] = (unsigned char)(paylen >> 8);
        frame[n++] = (unsigned char)paylen;
        sum = ((paylen >> 8) & 0xFF) + (paylen & 0xFF);
    }
    for (i = 0; i < paylen; i++) { frame[n] = payload[i]; sum += frame[n++]; }
    frame[n++] = (unsigned char)(0x100 - (sum & 0xFF));

    for (off = 0; off < n; off += 62) {
        unsigned char rpt[64];
        int chunk = n - off;
        unsigned char lcb;

        if (chunk > 62) chunk = 62;
        if (off == 0) lcb = (off + chunk >= n) ? 0x00 : 0x02;
        else          lcb = (off + chunk >= n) ? 0x01 : 0x03;

        memset(rpt, 0, sizeof(rpt));
        rpt[0] = 9;
        rpt[1] = lcb;
        memcpy(rpt + 2, frame + off, chunk);
        iap_hid_process_rx(rpt, 64);
    }
    iap_handlepkt();
}

#define ACC_SEND(...) do { \
        static const unsigned char _p[] = { __VA_ARGS__ }; \
        acc_send(_p, (int)sizeof(_p)); \
    } while (0)

/* Reassemble the IN reports the device sent, exactly as the accessory
 * would, and validate the frame. Returns the payload length, or -1. */
static int acc_recv(unsigned char *payload, int maxlen)
{
    unsigned char frame[600];
    int n = 0, i, k, paylen, hdr, sum;
    bool open = false;

    for (i = 0; i < e2e_report_count(); i++) {
        const struct e2e_report *r = e2e_report(i);
        unsigned char lcb = r->data[1];

        if (!open) {
            if (lcb != 0x00 && lcb != 0x02) return -1;
            open = (lcb == 0x02);
        } else {
            if (lcb != 0x03 && lcb != 0x01) return -1;
            open = (lcb == 0x03);
        }
        if (n + (r->len - 2) > (int)sizeof(frame)) return -1;
        memcpy(frame + n, r->data + 2, r->len - 2);
        n += r->len - 2;
    }
    if (open || n < 4) return -1;

    if (frame[0] != 0x55) return -1;
    if (frame[1] != 0x00) { paylen = frame[1]; hdr = 2; sum = frame[1]; }
    else { paylen = (frame[2] << 8) | frame[3]; hdr = 4;
           sum = frame[2] + frame[3]; }

    if (hdr + paylen + 1 > n) return -1;      /* truncated */
    for (k = 0; k < paylen; k++) sum += frame[hdr + k];
    if (((sum + frame[hdr + paylen]) & 0xFF) != 0) return -2;  /* bad sum */

    if (paylen > maxlen) return -1;
    memcpy(payload, frame + hdr, paylen);
    return paylen;
}

/* Bring the accessory up over the wire: IDPS, then authentication. */
static void acc_identify(void)
{
    e2e_usb_reset();
    ACC_SEND(0x00, 0x38, 0x00, 0x01);                      /* StartIDPS */
    ACC_SEND(0x00, 0x39, 0x00, 0x02, 0x01,
             0x10, 0x00, 0x00,
             0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
             0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00);                      /* tokens */
    ACC_SEND(0x00, 0x3B, 0x00, 0x03, 0x00);                /* EndIDPS */
    device.auth.state = AUST_AUTH;
    e2e_usb_reset();
}

/* ------------------------------------------------------------------ */

/* A short exchange must still be one report, byte for byte what the
 * protocol suite sees -- the transport must not disturb the common
 * case. */
static void test_short_command_round_trip(void)
{
    unsigned char got[600];

    acc_identify();
    ACC_SEND(0x00, 0x07, 0x00, 0x10);            /* GetiPodName */

    CHECK_EQ(e2e_report_count(), 1, "a short reply should be one report");
    int n = acc_recv(got, sizeof(got));
    CHECK(n > 0, "the accessory could not reassemble the reply (%d)", n);
    if (n < 4) return;

    CHECK_EQ(got[0], 0x00, "reply lingo");
    CHECK_EQ(got[1], 0x08, "RetiPodName");
    CHECK_EQ(got[2], 0x00, "transaction ID high");
    CHECK_EQ(got[3], 0x10, "transaction ID low");
    CHECK(memcmp(got + 4, "ROCKBOX", 7) == 0, "the name");
}

/* The case the transport used to break: a reply longer than one report.
 * A track title of 200 characters is well past the 59-byte cliff, and
 * the accessory must get every byte with the checksum intact. */
static void test_long_reply_survives_the_wire(void)
{
    unsigned char got[600];
    struct mp3entry *id3 = rbstub_id3();
    static char title[220];
    int i;

    for (i = 0; i < 200; i++) title[i] = 'A' + (i % 26);
    title[200] = '\0';
    id3->title = title;

    acc_identify();
    ACC_SEND(0x00, 0x05, 0x00, 0x11);            /* Extended Interface */
    e2e_usb_reset();
    ACC_SEND(0x04, 0x00, 0x20, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00);

    CHECK(e2e_report_count() >= 2,
          "a 200-character title should need more than one report, got %d",
          e2e_report_count());

    int n = acc_recv(got, sizeof(got));
    CHECK(n != -2, "the reassembled reply failed its checksum -- the "
                   "frame was truncated on the way out");
    CHECK(n > 0, "the accessory could not reassemble the reply (%d)", n);
    if (n < 5) return;

    CHECK_EQ(got[0], 0x04, "reply lingo");
    CHECK_EQ((got[1] << 8) | got[2], 0x0021,
             "ReturnIndexedPlayingTrackTitle");
    if (memcmp(got + 5, title, 200) != 0) {
        int d;
        for (d = 0; d < 200; d++)
            if (got[5 + d] != (unsigned char)title[d]) break;
        fprintf(stderr, "      n=%d first diff at %d: got %02X want %02X\n",
                n, d, got[5 + d], (unsigned char)title[d]);
    }
    CHECK(memcmp(got + 5, title, 200) == 0,
          "the title came back altered or short");
    CHECK_EQ(got[205], 0x00, "the string is still null terminated");
}

/* And in the other direction: a command too long for one OUT report
 * must be reassembled by the device and acted on. */
static void test_long_command_is_reassembled(void)
{
    unsigned char cmd[200];
    unsigned char got[600];
    int i;

    acc_identify();

    /* SetFIDTokenValues with enough tokens to need two reports. The
     * device must parse the whole thing, not the first 62 bytes. */
    e2e_usb_reset();
    ACC_SEND(0x00, 0x38, 0x00, 0x20);            /* StartIDPS again */
    e2e_usb_reset();

    i = 0;
    cmd[i++] = 0x00; cmd[i++] = 0x39;
    cmd[i++] = 0x00; cmd[i++] = 0x21;
    cmd[i++] = 0x04;                             /* four tokens */
    for (int t = 0; t < 4; t++) {
        cmd[i++] = 0x10;                         /* token length */
        cmd[i++] = 0x00; cmd[i++] = 0x00;        /* IdentifyToken */
        cmd[i++] = 0x05;
        cmd[i++] = 0x00; cmd[i++] = 0x02; cmd[i++] = 0x03;
        cmd[i++] = 0x04; cmd[i++] = 0x0A;
        memset(cmd + i, 0, 8); i += 8;
    }
    CHECK(i > 62, "the test command is only %d bytes, one report holds "
          "62 -- it would not exercise reassembly", i);
    acc_send(cmd, i);

    int n = acc_recv(got, sizeof(got));
    CHECK(n > 0, "no well-formed reply to a fragmented command (%d)", n);
    if (n < 4) return;
    CHECK_EQ(got[0], 0x00, "reply lingo");
    CHECK_EQ(got[1], 0x3A, "AckFIDTokenValues");
    CHECK_EQ(got[2], 0x00, "the reply echoes the transaction ID high");
    CHECK_EQ(got[3], 0x21, "the reply echoes the transaction ID low");
}

/* Nothing the device sends may ever be a frame the accessory cannot
 * parse, whatever it is asked. Sweep a range of commands and check
 * every reply reassembles and checksums. */
static void test_no_reply_is_ever_malformed(void)
{
    unsigned char got[600];
    static const unsigned char probes[][4] = {
        { 0x00, 0x07 }, { 0x00, 0x09 }, { 0x00, 0x0B },
        { 0x00, 0x27 }, { 0x00, 0x4B }, { 0x03, 0x04 },
        { 0x03, 0x0C }, { 0x03, 0x1A }, { 0x00, 0x11 },
    };
    unsigned i;

    acc_identify();

    for (i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        unsigned char c[6] = { probes[i][0], probes[i][1],
                               0x00, (unsigned char)(0x30 + i), 0x00, 0x00 };
        e2e_usb_reset();
        acc_send(c, 5);

        if (e2e_report_count() == 0)
            continue;                       /* silence is allowed */

        int n = acc_recv(got, sizeof(got));
        CHECK(n != -2,
              "the reply to %02X/%02X failed its checksum on the wire",
              probes[i][0], probes[i][1]);
        CHECK(n >= 0,
              "the reply to %02X/%02X could not be reassembled",
              probes[i][0], probes[i][1]);
    }
}

/* Every report the device sends must be full size and carry a valid
 * link control byte, across every exchange above. */
static void test_every_report_is_wellformed(void)
{
    unsigned char got[600];
    struct mp3entry *id3 = rbstub_id3();
    static char title[120];
    int i;

    for (i = 0; i < 100; i++) title[i] = 'x';
    title[100] = '\0';
    id3->title = title;

    acc_identify();
    ACC_SEND(0x00, 0x05, 0x00, 0x40);
    e2e_usb_reset();
    ACC_SEND(0x04, 0x00, 0x20, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00);

    CHECK(e2e_report_count() > 0, "nothing was sent");
    for (i = 0; i < e2e_report_count(); i++) {
        const struct e2e_report *r = e2e_report(i);
        unsigned char lcb = r->data[1];
        CHECK_EQ(r->len, e2e_report(0)->len,
                 "reports must not change size mid-transaction");
        CHECK_EQ(r->data[0], e2e_report(0)->data[0],
                 "the report id must not change mid-transaction");
        CHECK(lcb <= 0x03,
              "report %d has link control 0x%02X, outside the two-bit "
              "field", i, lcb);
    }
    CHECK(acc_recv(got, sizeof(got)) > 0, "the sweep's reply is unusable");

    /* One buffer wait per report: the transport reuses one tx_buf. */
    CHECK_EQ(e2e_semaphore_waits(), e2e_report_count(),
             "one buffer wait per report");
}


/* ------------------------------------------------------------------ */
/* Accessories that do not behave neatly                                */
/* ------------------------------------------------------------------ */

/* Send a command split into reports of a given size, rather than the
 * largest that fits. MFi Accessory Hardware Specification R9, Table 3-2
 * (p.56), leaves the choice to the sender: "The USB host should analyze
 * the HID report descriptor of the Apple device at runtime to determine
 * which Report ID corresponds to the most appropriate report type for
 * each transfer." A dock that picks a small report for a long command
 * is conformant, and the device has to reassemble it. */
static void acc_send_in_chunks(const unsigned char *payload, int paylen,
                               unsigned char rid, int cap)
{
    unsigned char frame[600];
    int n = 0, sum = 0, i, off;

    frame[n++] = 0x55;
    if (paylen <= 0xFC) { frame[n++] = (unsigned char)paylen; sum = paylen; }
    else {
        frame[n++] = 0x00;
        frame[n++] = (unsigned char)(paylen >> 8);
        frame[n++] = (unsigned char)paylen;
        sum = ((paylen >> 8) & 0xFF) + (paylen & 0xFF);
    }
    for (i = 0; i < paylen; i++) { frame[n] = payload[i]; sum += frame[n++]; }
    frame[n++] = (unsigned char)(0x100 - (sum & 0xFF));

    for (off = 0; off < n; off += cap) {
        unsigned char rpt[64];
        int chunk = n - off;
        unsigned char lcb;

        if (chunk > cap) chunk = cap;
        if (off == 0) lcb = (off + chunk >= n) ? 0x00 : 0x02;
        else          lcb = (off + chunk >= n) ? 0x01 : 0x03;

        memset(rpt, 0, sizeof(rpt));
        rpt[0] = rid;
        rpt[1] = lcb;
        memcpy(rpt + 2, frame + off, chunk);
        iap_hid_process_rx(rpt, 1 + cap + 1);
    }
    iap_handlepkt();
}

/* A dock that uses the smallest OUT report for everything, so even a
 * short command arrives in pieces. Report 5 carries 8 bytes, of which
 * the link control byte takes one: seven bytes of frame per report. */
static void test_awkward_fragmentation(void)
{
    unsigned char got[600];

    acc_identify();

    /* GetiPodName, 4 payload bytes, framed to 7 -- exactly one report's
     * worth, then the boundary cases either side of it. */
    static const struct { unsigned char rid; int cap; } sizes[] = {
        { 5, 7 }, { 6, 9 }, { 7, 13 }, { 8, 19 }, { 9, 62 },
    };

    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        unsigned char cmd[4] = { 0x00, 0x07, 0x00, (unsigned char)(0x20 + i) };
        e2e_usb_reset();
        acc_send_in_chunks(cmd, sizeof(cmd), sizes[i].rid, sizes[i].cap);

        int n = acc_recv(got, sizeof(got));
        CHECK(n > 0,
              "a command split across %d-byte reports (id %d) got no "
              "usable reply (%d)", sizes[i].cap, sizes[i].rid, n);
        if (n < 4)
            continue;
        CHECK_EQ(got[1], 0x08, "RetiPodName");
        CHECK_EQ(got[3], 0x20 + i, "the reply echoes the transaction ID");
    }
}

/* The same, for a command long enough that even the largest report has
 * to split it -- driven through the smallest, so it takes many. */
static void test_long_command_through_small_reports(void)
{
    unsigned char cmd[200];
    unsigned char got[600];
    int i = 0;

    acc_identify();
    e2e_usb_reset();
    /* StartIDPS carries a transaction ID here: the session already has
     * them in force from acc_identify(), and 2.6.1.4 (p.112) exempts
     * only RequestIdentify, Identify and IdentifyDeviceLingoes. */
    {
        static const unsigned char start[] = { 0x00, 0x38, 0x00, 0x2F };
        acc_send_in_chunks(start, sizeof(start), 5, 7);
    }
    e2e_usb_reset();

    /* SetFIDTokenValues with four tokens: 69 bytes, so ten reports of
     * seven. */
    cmd[i++] = 0x00; cmd[i++] = 0x39;
    cmd[i++] = 0x00; cmd[i++] = 0x30;
    cmd[i++] = 0x04;
    for (int t = 0; t < 4; t++) {
        cmd[i++] = 0x10;
        cmd[i++] = 0x00; cmd[i++] = 0x00;
        cmd[i++] = 0x05;
        cmd[i++] = 0x00; cmd[i++] = 0x02; cmd[i++] = 0x03;
        cmd[i++] = 0x04; cmd[i++] = 0x0A;
        memset(cmd + i, 0, 8); i += 8;
    }
    acc_send_in_chunks(cmd, i, 5, 7);

    int n = acc_recv(got, sizeof(got));
    CHECK(n > 0, "a 69-byte command in 7-byte pieces got no reply (%d)", n);
    if (n >= 4) {
        CHECK_EQ(got[1], 0x3A, "AckFIDTokenValues");
        CHECK_EQ(got[3], 0x30, "the reply echoes the transaction ID");
    }
}

/* An accessory that abandons a report set half way and starts another.
 * R9 Table 3-2 (p.56), link control bit 0: "0 indicates that this HID
 * report is the first in a set of one or more reports. This also
 * implies that any previous sets are completed. Any incomplete iAP
 * packets received prior to the arrival of this report are flushed and
 * lost."
 *
 * So the abandoned command must not be answered, and the one that
 * displaced it must be -- not merged with the wreckage of the first. */
/* A set that closes cleanly can still leave the framer mid-packet, and
 * that is a different state from a set that never closes.
 *
 * The link-control rule is unconditional -- R9 Table 3-2 (p.56): "Any
 * incomplete iAP packets received prior to the arrival of this report
 * are flushed and lost" -- but the flush was gated on the transport's
 * own in-progress flag, which a LAST report clears. So a truncated iAP
 * packet delivered inside a properly terminated set left the framer in
 * ST_DATA with the flag already false, and the next command was
 * swallowed whole as continuation data. */
static void test_short_packet_in_a_closed_set_is_flushed(void)
{
    unsigned char got[600];
    unsigned char rpt[64];

    acc_identify();
    e2e_usb_reset();

    /* FIRST then LAST, so the set terminates properly -- but the iAP
     * packet inside declares 64 payload bytes and only a handful
     * arrive. */
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 9;
    rpt[1] = 0x02;                      /* first, more to follow */
    /* 0xFC is the largest short-form payload (MFi 2.5.2, p.110). Both
     * reports together carry nowhere near it, so the frame is still
     * open when the set closes -- which is the whole point. A smaller
     * claim would be satisfied by the two reports and the framer would
     * return to ST_SYNC on its own. */
    rpt[2] = 0x55; rpt[3] = 0xFC;
    rpt[4] = 0x00; rpt[5] = 0x07;
    iap_hid_process_rx(rpt, 64);

    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 5;
    rpt[1] = 0x01;                      /* last */
    rpt[2] = 0x00;
    iap_hid_process_rx(rpt, 8);
    iap_handlepkt();

    CHECK_EQ(e2e_report_count(), 0,
             "a truncated command inside a closed set was answered");

    /* The next command must be handled, not eaten. */
    static const unsigned char tid[] = { 0x50, 0x51 };
    for (unsigned k = 0; k < sizeof(tid); k++) {
        unsigned char q[4] = { 0x00, 0x07, 0x00, tid[k] };
        e2e_usb_reset();
        acc_send(q, sizeof(q));

        int n = acc_recv(got, sizeof(got));
        CHECK(n > 0,
              "command %u after a short packet in a closed set got no "
              "usable reply (%d) -- the framer was still in ST_DATA and "
              "consumed it as continuation bytes", k, n);
        if (n < 4)
            continue;
        CHECK_EQ(got[0], 0x00, "reply lingo");
        CHECK_EQ(got[1], 0x08, "RetiPodName");
        CHECK_EQ(got[3], tid[k], "the reply belongs to the command that asked");
    }
}

/* A disconnect in the middle of a packet has to flush too.
 *
 * usb_iap_hid_disconnect() cleared its own in-progress flag and said
 * nothing to the framer. Reconnecting does not help: iap_malloc()
 * resets the receive pointers and leaves frame_state.len, .count and
 * .state where they were. The 25 ms IAP_PKT_TIMEOUT hides it on a
 * physical unplug, but usb_core reaches this function on a set-config
 * and on a bus reset, either of which can re-open the path in far less
 * than that. */
static void test_disconnect_mid_packet_is_flushed(void)
{
    unsigned char got[600];
    unsigned char rpt[64];

    acc_identify();
    e2e_usb_reset();

    /* Open a packet and leave it open. */
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 9;
    rpt[1] = 0x02;                      /* first, more to follow */
    rpt[2] = 0x55; rpt[3] = 0xFC;
    rpt[4] = 0x00; rpt[5] = 0x07;
    iap_hid_process_rx(rpt, 64);

    /* Yank it, then bring it straight back -- a bus reset, not an
     * unplug, so no timeout intervenes. */
    usb_iap_hid_disconnect();
    usb_iap_hid_init_connection();
    e2e_usb_reset();

    unsigned char q[4] = { 0x00, 0x07, 0x00, 0x70 };
    acc_send(q, sizeof(q));

    int n = acc_recv(got, sizeof(got));
    CHECK(n > 0,
          "the first command after a mid-packet disconnect got no usable "
          "reply (%d) -- the framer came back still in ST_DATA", n);
    if (n >= 2) {
        CHECK_EQ(got[0], 0x00, "reply lingo");
        CHECK_EQ(got[1], 0x08, "RetiPodName");
    }
    /* No transaction-ID check here: usb_iap_hid_init_connection() runs
     * iap_setup() again, so the session comes back without IDPS and the
     * reply carries no ID to echo. What this case is about is that the
     * command was handled at all rather than eaten as continuation
     * bytes. */
}

static void test_abandoned_set_is_flushed(void)
{
    unsigned char got[600];
    unsigned char rpt[64];

    acc_identify();
    e2e_usb_reset();

    /* Open a set and stop: a first fragment of a command that never
     * finishes. */
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 9;
    rpt[1] = 0x02;                      /* first, more to follow */
    rpt[2] = 0x55; rpt[3] = 0x40;       /* claims a 64-byte payload */
    rpt[4] = 0x00; rpt[5] = 0x07;
    iap_hid_process_rx(rpt, 64);
    iap_handlepkt();

    CHECK_EQ(e2e_report_count(), 0,
             "an unfinished command was answered");

    /* Now several complete ones, back to back. Each must be answered,
     * and answered with its own transaction ID.
     *
     * One command is not enough to prove the buffer recovered: the
     * abandoned fragment left a length prefix and partial data behind
     * it, and the damage shows when a later packet is drained past
     * them. */
    static const unsigned char tid[] = { 0x40, 0x41, 0x42 };
    for (unsigned k = 0; k < sizeof(tid); k++) {
        unsigned char q[4] = { 0x00, 0x07, 0x00, tid[k] };
        e2e_usb_reset();
        acc_send(q, sizeof(q));

        int n = acc_recv(got, sizeof(got));
        CHECK(n > 0,
              "command %u after an abandoned set got no usable reply (%d)",
              k, n);
        if (n < 4)
            continue;
        CHECK_EQ(got[0], 0x00, "reply lingo");
        CHECK_EQ(got[1], 0x08, "RetiPodName");
        CHECK_EQ(got[3], tid[k],
                 "the reply belongs to the command that asked, not to "
                 "the abandoned fragment or a neighbour");
    }
}

/* Notifications arriving while the accessory is mid-conversation. The
 * device transmits from two threads -- iap_handlepkt() on the iAP
 * thread and iap_track_changed() on the audio thread -- and every frame
 * either produces must still be a whole, checksummed frame. */
static void test_notifications_interleave_cleanly(void)
{
    unsigned char got[600];
    struct mp3entry *id3;
    static char title[120];
    int i;

    id3 = rbstub_id3();
    for (i = 0; i < 100; i++) title[i] = (char)('A' + (i % 26));
    title[100] = '\0';
    id3->title = title;

    acc_identify();
    ACC_SEND(0x00, 0x05, 0x00, 0x50);          /* Extended Interface */

    /* Subscribe to track-index notifications, then ask a question whose
     * answer needs several reports, with a track change in between. */
    ACC_SEND(0x04, 0x00, 0x26, 0x00, 0x51, 0x00, 0x00, 0x00, 0x04);
    rbstub_set_playlist(20, 3);

    for (i = 0; i < 6; i++) {
        e2e_usb_reset();
        rbstub_set_playlist(20, 3 + i);
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        {
            unsigned char q[9] = { 0x04, 0x00, 0x20,
                                   0x00, (unsigned char)(0x52 + i),
                                   0x00, 0x00, 0x00, 0x00 };
            acc_send(q, sizeof(q));
        }

        /* Whatever came out, every report must be full size and carry a
         * link control byte inside the two-bit field, and the set must
         * open and close. */
        int k, setlen = 0;
        bool open = false;
        for (k = 0; k < e2e_report_count(); k++) {
            const struct e2e_report *r = e2e_report(k);
            unsigned char lcb = r->data[1];
            CHECK(lcb <= 0x03,
                  "round %d report %d has link control 0x%02X", i, k, lcb);
            /* Size is constant within a frame, not across a batch: a
             * notification and a reply are two frames and each picks
             * the smallest report that fits it. Compare against the
             * report that opened the current set. */
            if (!open)
                setlen = r->len;
            else
                CHECK_EQ(r->len, setlen,
                         "reports must not change size within one frame");
            if (!open) open = (lcb == 0x02);
            else       open = (lcb == 0x03);
        }
        CHECK(!open, "round %d ended with an unclosed report set", i);

        /* And the frames reassemble. acc_recv() concatenates the lot,
         * so with a notification and a reply in the same batch it will
         * see the first frame; what matters is that it parses. */
        /* acc_recv() concatenates everything the batch produced, so
         * with a notification and a reply together it parses the first
         * frame; a bad checksum there means the wire was corrupted. */
        int n = acc_recv(got, sizeof(got));
        CHECK(n != -2, "round %d produced a frame with a bad checksum", i);
    }

    /* The lock is taken and released once per frame, never left held. */
    CHECK_EQ(e2e_frame_locks(), e2e_frame_unlocks(),
             "a frame was left holding the transmit lock");
}

/* ------------------------------------------------------------------ */

static const struct { const char *name; void (*fn)(void); } cases[] = {
    { "e2e_short_command_round_trip",     test_short_command_round_trip },
    { "e2e_long_reply_survives_the_wire", test_long_reply_survives_the_wire },
    { "e2e_long_command_is_reassembled",  test_long_command_is_reassembled },
    { "e2e_no_reply_is_ever_malformed",   test_no_reply_is_ever_malformed },
    { "e2e_every_report_is_wellformed",   test_every_report_is_wellformed },
    { "e2e_awkward_fragmentation",        test_awkward_fragmentation },
    { "e2e_long_command_small_reports",   test_long_command_through_small_reports },
    { "e2e_abandoned_set_is_flushed",     test_abandoned_set_is_flushed },
    { "e2e_short_packet_in_closed_set",   test_short_packet_in_a_closed_set_is_flushed },
    { "e2e_disconnect_mid_packet",        test_disconnect_mid_packet_is_flushed },
    { "e2e_notifications_interleave",     test_notifications_interleave_cleanly },
};

int main(void)
{
    unsigned i;
    int failed = 0;

    printf("iAP end to end: protocol layer through the USB HID transport\n");
    printf("  target ipod6g, MFi Accessory Firmware Specification R46\n\n");

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int before = failures;
        current = cases[i].name;

        rbstub_reset();
        iap_hid_active = true;
        e2e_usb_reset();
        cases[i].fn();

        if (failures > before) { failed++; printf("  \033[31mFAIL\033[0m  %s\n", cases[i].name); }
        else printf("  \033[32mok\033[0m    %s\n", cases[i].name);
    }

    printf("\n%u case%s run, %d check%s, %d failure%s\n",
           i, i == 1 ? "" : "s", checks, checks == 1 ? "" : "s",
           failures, failures == 1 ? "" : "s");
    return failed ? 1 : 0;
}
