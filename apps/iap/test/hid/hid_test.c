/***************************************************************************
 * iAP over USB HID: transmit-side fragmentation.
 *
 * firmware/usbstack/usb_iap_hid.c is compiled here for the host with the
 * USB stack stubbed out, so the bytes it hands usb_drv_send_nonblocking()
 * can be asserted on directly. It is a separate binary from iap_test
 * because the two need different stubs for the same names.
 *
 * MFi R46 2.2.2.3 (p.90): "All iAP command packets transferred over the
 * USB IN and OUT pipes must follow the formats specified in Command
 * Packets, except that the sync byte (byte 0) of each packet is
 * unnecessary and should be omitted."
 *
 * R46 glossary (p.581), the one sentence in the document describing
 * fragmentation: "iAP packets are broken into HID reports before being
 * sent across the USB port link and are reassembled on the receiving
 * side."
 *
 * The link control values are the ones usb_iap_hid.c's own reassembly
 * path documents, and which a capture of a real accessory confirms:
 * 0x00 single, 0x02 first, 0x03 middle, 0x01 last.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

#include "stubs/usb_ch9.h"
#include "hid_stubs.h"

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
        checks++; \
        if (!(cond)) fail(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define CHECK_EQ(got, want, what) do { \
        checks++; \
        long _g = (long)(got), _w = (long)(want); \
        if (_g != _w) fail(__FILE__, __LINE__, \
            "%s: got %ld (0x%lX), want %ld (0x%lX)", \
            (what), _g, (unsigned long)_g, _w, (unsigned long)_w); \
    } while (0)

/* ------------------------------------------------------------------ */

/* Build a framed iAP packet with a payload of n bytes, exactly as
 * iap_send_tx() does: sync, start, length, payload, checksum. */
static int build_frame(unsigned char *out, int n)
{
    int i, sum, k = 0;

    out[k++] = 0xFF;
    out[k++] = 0x55;

    /* MFi 2.5.2: a one-byte length field expresses 0x02 to 0xFC;
     * anything longer takes the three-byte form, a 0x00 marker then a
     * 16-bit length. iap_send_tx() switches at the same point. */
    if (n <= 0xFC) {
        out[k++] = (unsigned char)n;
        sum = n;
    } else {
        out[k++] = 0x00;
        out[k++] = (unsigned char)(n >> 8);
        out[k++] = (unsigned char)n;
        sum = ((n >> 8) & 0xFF) + (n & 0xFF);
    }

    for (i = 0; i < n; i++) {
        out[k] = (unsigned char)(0x10 + (i & 0x7F));
        sum += out[k++];
    }
    out[k++] = (unsigned char)(0x100 - (sum & 0xFF));
    return k;
}

/* Where the checksum starts: after the 0x55, over the length field and
 * the payload. */
static int checksum_start(int payload_len)
{
    return (payload_len <= 0xFC) ? 2 : 2;
}

/* Reassemble what the transport sent, the way the accessory would:
 * strip the report ID and link control byte from each report, and
 * concatenate. Returns the number of bytes recovered, or -1 if the
 * link control sequence is not a valid one. */
static int reassemble(unsigned char *out, int outmax)
{
    int i, n = 0;
    bool open = false;

    for (i = 0; i < hidstub_tx_count(); i++) {
        const struct hidstub_report *r = hidstub_tx(i);
        unsigned char lcb = r->data[1];
        int payload = r->len - 2;

        if (!open) {
            if (lcb != 0x00 && lcb != 0x02)
                return -1;                  /* a set must open */
            open = (lcb == 0x02);
        } else {
            if (lcb != 0x03 && lcb != 0x01)
                return -1;                  /* ...and continue */
            open = (lcb == 0x03);
        }

        if (n + payload > outmax)
            return -1;
        memcpy(out + n, r->data + 2, payload);
        n += payload;
    }
    return open ? -1 : n;                   /* a set must close */
}

/* Everything the accessory checks about a frame it has reassembled. */
static void check_frame_survives(int payload_len)
{
    unsigned char frame[1024];
    unsigned char got[1024];
    int framelen = build_frame(frame, payload_len);
    int i, sum, n;

    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() > 0,
          "payload %d: nothing was transmitted", payload_len);
    if (hidstub_tx_count() == 0)
        return;

    /* Every report is full size, and every report carries the same id. */
    unsigned char id = hidstub_tx(0)->data[0];
    for (i = 0; i < hidstub_tx_count(); i++) {
        const struct hidstub_report *r = hidstub_tx(i);
        CHECK(r->len == hidstub_tx(0)->len,
              "payload %d report %d: length %d, first was %d -- reports "
              "must not change size mid-transaction",
              payload_len, i, r->len, hidstub_tx(0)->len);
        CHECK_EQ(r->data[0], id, "report id changed mid-transaction");
    }

    /* Every report goes out at full size whatever it carries, so the
     * bytes past the payload are on the wire either way. They must be
     * zeroed, not left holding the previous frame's contents. */
    {
        int cap = hidstub_tx(0)->len - 2;   /* minus report id and LCB */
        int left = framelen - 1;            /* the frame without its sync */
        for (i = 0; i < hidstub_tx_count(); i++) {
            const struct hidstub_report *r = hidstub_tx(i);
            int carried = (left > cap) ? cap : left;
            int k;
            for (k = 2 + carried; k < r->len; k++)
                CHECK(r->data[k] == 0x00,
                      "payload %d report %d byte %d is padding but holds "
                      "0x%02X -- the previous frame is leaking onto the "
                      "wire", payload_len, i, k, r->data[k]);
            left -= carried;
        }
    }

    n = reassemble(got, sizeof(got));
    CHECK(n >= 0,
          "payload %d: the link control sequence over %d report(s) is "
          "not a valid single/first/middle/last set",
          payload_len, hidstub_tx_count());
    if (n < 0)
        return;

    /* The sync byte is omitted over USB, so what comes back is the
     * frame from 0x55 onwards, plus zero padding. */
    CHECK(n >= framelen - 1,
          "payload %d: recovered %d bytes, the frame without its sync "
          "byte is %d -- the tail was truncated",
          payload_len, n, framelen - 1);
    if (n < framelen - 1)
        return;

    CHECK_EQ(got[0], 0x55, "reassembled start byte");
    if (payload_len <= 0xFC) {
        CHECK_EQ(got[1], payload_len, "reassembled length field");
    } else {
        CHECK_EQ(got[1], 0x00, "long-form length marker");
        CHECK_EQ((got[2] << 8) | got[3], payload_len,
                 "reassembled long-form length field");
    }
    CHECK(memcmp(got, frame + 1, framelen - 1) == 0,
          "payload %d: the reassembled frame differs from the one sent",
          payload_len);

    /* And the checksum still closes over it, which is the property
     * truncation destroyed. iAP sums the length byte and the payload --
     * not the 0x55 start marker -- so start at frame[2]. */
    sum = 0;
    for (i = checksum_start(payload_len); i < framelen - 1; i++)
        sum += frame[i];
    CHECK_EQ((sum + got[framelen - 2]) & 0xFF, 0,
             "the reassembled frame's checksum");
}

/* ------------------------------------------------------------------ */

static void test_single_report_frames(void)
{
    /* Everything that fits one report must still go in one report, and
     * must carry the single-report link control byte. */
    static const int sizes[] = { 2, 5, 10, 11, 12, 13, 17, 18, 19, 58 };
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        check_frame_survives(sizes[i]);
        CHECK_EQ(hidstub_tx_count(), 1,
                 "a frame that fits one report was split");
        if (hidstub_tx_count() >= 1)
            CHECK_EQ(hidstub_tx(0)->data[1], 0x00,
                     "single report link control byte");
    }
}

static void test_the_old_truncation_boundary(void)
{
    /* A 63-byte report holds a link control byte and 62 frame bytes.
     * The frame is sync, start, length, payload, checksum, and the sync
     * is dropped -- so 62 frame bytes is a payload of 59.
     *
     * 59 was the last size that survived. 60 was silently truncated,
     * with a length field claiming bytes that never arrived. */
    check_frame_survives(59);
    CHECK_EQ(hidstub_tx_count(), 1, "a 59-byte payload should be one report");

    check_frame_survives(60);
    CHECK(hidstub_tx_count() == 2,
          "a 60-byte payload needs two reports, got %d",
          hidstub_tx_count());
}

static void test_fragmented_frames(void)
{
    /* Across the boundary, and well past it: 128-byte RSA signatures
     * and long track titles both live here. */
    /* 251-253 straddle the switch from the one-byte length field to the
     * three-byte one (MFi 2.5.2), and 255 is the maximum payload this
     * device advertises in ReturnTransportMaxPayloadSize
     * (iap-lingo0.c). That number was a 4.3x overclaim while the
     * transport truncated at 59; it is deliverable now, and this is
     * what keeps the two from drifting apart again. */
    static const int sizes[] = { 60, 61, 62, 63, 100, 123, 124, 125,
                                 128, 200, 251, 252, 253, 254, 255 };
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        check_frame_survives(sizes[i]);
        CHECK(hidstub_tx_count() >= 2,
              "payload %d should have been fragmented, got %d report(s)",
              sizes[i], hidstub_tx_count());
    }
}

static void test_link_control_sequence(void)
{
    unsigned char frame[1024];
    int framelen = build_frame(frame, 200);
    int i;

    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() >= 3,
          "a 200-byte payload should need at least three reports, got %d",
          hidstub_tx_count());
    if (hidstub_tx_count() < 3)
        return;

    CHECK_EQ(hidstub_tx(0)->data[1], 0x02, "first fragment");
    for (i = 1; i < hidstub_tx_count() - 1; i++)
        CHECK_EQ(hidstub_tx(i)->data[1], 0x03, "middle fragment");
    CHECK_EQ(hidstub_tx(hidstub_tx_count() - 1)->data[1], 0x01,
             "last fragment");
}

static void test_every_fragment_waits_for_the_buffer(void)
{
    unsigned char frame[1024];
    int framelen = build_frame(frame, 200);

    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);

    /* tx_buf is one buffer reused for every fragment, so each has to
     * wait for the previous transfer to leave before overwriting it.
     * The wait used to happen once, before the send. */
    CHECK_EQ(hidstub_semaphore_waits(), hidstub_tx_count(),
             "one buffer wait per report");
}

static void test_sync_byte_is_omitted(void)
{
    unsigned char frame[64];
    int framelen = build_frame(frame, 10);

    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() == 1, "expected one report");
    if (hidstub_tx_count() != 1)
        return;

    /* MFi 2.2.2.3 (p.90): the sync byte "is unnecessary and should be
     * omitted". Byte 1 is the link control byte, byte 2 the start of
     * the frame proper. 0xFF must appear in neither. */
    CHECK_EQ(hidstub_tx(0)->data[1], 0x00, "link control byte");
    CHECK_EQ(hidstub_tx(0)->data[2], 0x55, "frame starts at 0x55");
    CHECK(hidstub_tx(0)->data[1] != 0xFF && hidstub_tx(0)->data[2] != 0xFF,
          "the sync byte was left on the wire");

    /* A frame that arrives without a sync byte must not lose a real
     * byte to the same rule. */
    hidstub_reset();
    iap_hid_tx_for_test(frame + 1, framelen - 1);
    CHECK(hidstub_tx_count() == 1, "expected one report");
    if (hidstub_tx_count() == 1)
        CHECK_EQ(hidstub_tx(0)->data[2], 0x55,
                 "a frame sent without its sync byte lost its first byte");
}

static void test_inactive_transport_sends_nothing(void)
{
    unsigned char frame[64];
    int framelen = build_frame(frame, 10);

    hidstub_reset();
    hidstub_set_active(false);
    iap_hid_tx_for_test(frame, framelen);
    CHECK_EQ(hidstub_tx_count(), 0,
             "an inactive transport transmitted");
    hidstub_set_active(true);

    /* And a degenerate frame must not underflow the sync-byte strip. */
    hidstub_reset();
    unsigned char lone = 0xFF;
    iap_hid_tx_for_test(&lone, 1);
    CHECK_EQ(hidstub_tx_count(), 0,
             "a frame of nothing but a sync byte transmitted");
}

/* ------------------------------------------------------------------ */
/* Reassembly                                                           */
/* ------------------------------------------------------------------ */

/* Hand the receive path one OUT report. Report ID 9 carries 63 bytes,
 * of which the link control byte takes one. */
static void rx_report(unsigned char lcb, const unsigned char *body, int n)
{
    unsigned char r[64];
    memset(r, 0, sizeof(r));
    r[0] = 9;
    r[1] = lcb;
    if (n > 62) n = 62;
    memcpy(r + 2, body, n);
    iap_hid_process_rx_for_test(r, 64);
}

/* The link control byte is two bits wide. The dispatch masked it and
 * the two state assignments did not, so a report with any higher bit
 * set opened a frame that no continuation could then join. */
static void test_rx_link_control_is_masked_consistently(void)
{
    unsigned char first[8]  = { 0x55, 0x08, 0xAA, 0xBB };
    unsigned char rest[8]   = { 0xCC, 0xDD };

    hidstub_rx_clear();
    rx_report(0x02 | 0x04, first, 4);      /* first, with a stray bit */
    rx_report(0x01 | 0x04, rest, 2);       /* last, with a stray bit */

    /* 0xFF, then the frame bytes from the marker, then the tail. The
     * reports are zero padded, so the recovered length is the full
     * capacity each time; what matters is that the tail arrived. */
    CHECK(hidstub_rx_count() > 63,
          "a link control byte with a bit above the low two lost its "
          "continuation: only %d bytes reached the protocol layer",
          hidstub_rx_count());
    if (hidstub_rx_count() > 4) {
        const unsigned char *b = hidstub_rx();
        CHECK_EQ(b[0], 0xFF, "the sync byte is put back for the parser");
        CHECK_EQ(b[1], 0x55, "the frame starts at its marker");
    }
}

/* A report that opens a frame and carries no 0x55 is malformed. It used
 * to leave any reassembly already in progress untouched, so the next
 * continuation fragment was appended to a frame that had ended. */
static void test_rx_missing_sync_marker_ends_reassembly(void)
{
    unsigned char first[8] = { 0x55, 0x20, 0x11, 0x22 };
    unsigned char nosync[8] = { 0x01, 0x02, 0x03, 0x04 };
    unsigned char rest[8]  = { 0x33, 0x44 };

    hidstub_rx_clear();
    rx_report(0x02, first, 4);             /* opens a frame */
    int after_first = hidstub_rx_count();
    CHECK(after_first > 0, "the first fragment fed nothing");

    rx_report(0x00, nosync, 4);            /* single, but no marker */
    CHECK_EQ(hidstub_rx_count(), after_first,
             "a report with no start marker fed bytes to the parser");

    rx_report(0x03, rest, 2);              /* a continuation, now orphaned */
    CHECK_EQ(hidstub_rx_count(), after_first,
             "a continuation fragment was accepted after the frame it "
             "belonged to had already been abandoned");
}

/* A report ID the table does not know carries no length of its own, so
 * iap_hid_process_rx() starts with len - 2 and the clamp that follows
 * cannot narrow it -- it is already that value. Whatever length the
 * caller passes is then read out of a 64-byte buffer.
 *
 * usb_iap_hid_control_request() clamps to sizeof(rx_buf) before
 * calling, which is what keeps that safe in the driver today, and
 * test_control_set_report_is_bounded covers it there. This checks the
 * bound in process_rx itself, so the function is safe for any length a
 * future caller passes rather than only the ones this one does.
 *
 * The buffer here is deliberately larger than any report: the point is
 * to see what the framer is handed, not to commit the over-read the
 * bound exists to prevent. */
static void test_rx_unknown_report_id_is_bounded(void)
{
    static unsigned char big[4096];
    int i;

    for (i = 0; i < (int)sizeof(big); i++)
        big[i] = (unsigned char)(0x10 + (i & 0x3F));
    big[0] = 0x40;          /* a report ID out_report_sizes does not list */
    big[1] = 0x00;          /* single */
    big[2] = 0x55;          /* a start marker, so the bytes are consumed */
    big[3] = 0x02;
    big[4] = 0x00;
    big[5] = 0x07;

    hidstub_rx_clear();
    iap_hid_process_rx_for_test(big, (int)sizeof(big));

    /* The largest OUT report holds 63 bytes, of which the link control
     * byte takes one, so 62 is the most any report can carry -- plus
     * the 0xFF the reassembly injects ahead of a frame. */
    CHECK(hidstub_rx_count() <= 63,
          "an unknown report ID with a 4096-byte length fed the framer "
          "%d bytes; the largest report carries 62",
          hidstub_rx_count());
}

/* Every OUT report ID, and a transfer longer than the report declares.
 *
 * iap_hid_process_rx() reserves one byte of each report for the link
 * control byte -- "out_report_sizes[i].size - 1". A later line narrows
 * the result to len - 2, and for a transfer of exactly 1 + size those
 * two are equal, so the reservation is invisible: every case that sends
 * a full-size report passes with or without it.
 *
 * A transfer larger than the report declares separates them. Report 5
 * declares 8 bytes, so it carries 7 of frame; handed a 64-byte transfer
 * it must still take 7, not 8. */
static void test_rx_every_report_id_reserves_the_lcb(void)
{
    static const struct { unsigned char id; int size; } out[] = {
        { 5, 8 }, { 6, 10 }, { 7, 14 }, { 8, 20 }, { 9, 63 },
    };
    unsigned char rpt[64];
    unsigned i;

    for (i = 0; i < sizeof(out)/sizeof(out[0]); i++) {
        int j;

        /* A complete one-report frame, then filler out to 64 bytes so
         * the transfer is longer than the report. */
        memset(rpt, 0xA5, sizeof(rpt));
        rpt[0] = out[i].id;
        rpt[1] = 0x00;              /* single */
        rpt[2] = 0x55;
        rpt[3] = 0x02;
        rpt[4] = 0x00;
        rpt[5] = 0x07;
        rpt[6] = 0xF7;              /* checksum over 02 00 07 */
        for (j = 7; j < 64; j++)
            rpt[j] = 0xA5;

        hidstub_rx_clear();
        iap_hid_process_rx_for_test(rpt, 64);

        /* The injected 0xFF plus the report's own capacity, and not one
         * byte more. */
        CHECK_EQ(hidstub_rx_count(), 1 + (out[i].size - 1),
                 "the report's capacity is its declared size less the "
                 "link control byte");
    }
}

/* And the ordinary case still works. */
static void test_rx_reassembles_a_split_frame(void)
{
    unsigned char first[62], rest[62];
    int i;

    for (i = 0; i < 62; i++) { first[i] = (unsigned char)(0x60 + i); rest[i] = (unsigned char)(0xA0 + i); }
    first[0] = 0x55;

    hidstub_rx_clear();
    rx_report(0x02, first, 62);
    rx_report(0x01, rest, 62);

    CHECK_EQ(hidstub_rx_count(), 1 + 62 + 62,
             "a two-report frame should reassemble to its full length");
    if (hidstub_rx_count() >= 3) {
        const unsigned char *b = hidstub_rx();
        CHECK_EQ(b[0], 0xFF, "sync byte");
        CHECK_EQ(b[1], 0x55, "start marker");
        CHECK_EQ(b[63], 0xA0, "the second fragment follows the first");
    }
}

/* The counts alone cannot see whether the wait happens before the send
 * or after it, and after is exactly the mutation that removes the
 * protection: the write for fragment N would no longer wait for
 * fragment N-1's transfer to finish with the buffer.
 *
 * The trace is one character per event: 'L' lock, 'w' wait, 's' send,
 * 'U' unlock. A correct frame is L (ws)+ U.
 *
 * The lock matters because iap_send_tx() has two callers on two
 * threads: the iAP thread through iap_handlepkt(), and the audio thread
 * through send_event(), which firmware/events.c:113 runs synchronously
 * on the caller's thread. Without it a fragment from one thread can
 * land inside the other's report set -- and MFi Accessory Hardware
 * Specification R9 Table 3-2 (p.56) says a report with link control bit
 * 0 clear means "any incomplete iAP packets received prior to the
 * arrival of this report are flushed and lost". */
static void check_trace_is_wellformed(const char *what, int reports)
{
    const char *t = hidstub_trace();
    int i, n = (int)strlen(t);

    CHECK(n == 2 + 2 * reports,
          "%s: trace \"%s\" has %d events, want %d for %d report(s)",
          what, t, n, 2 + 2 * reports, reports);
    if (n != 2 + 2 * reports)
        return;

    CHECK(t[0] == 'L', "%s: the frame lock is not taken first (%s)",
          what, t);
    CHECK(t[n - 1] == 'U', "%s: the frame lock is not released last (%s)",
          what, t);

    for (i = 0; i < reports; i++) {
        CHECK(t[1 + 2 * i] == 'w',
              "%s: report %d was written without waiting for the buffer "
              "first (%s)", what, i, t);
        CHECK(t[2 + 2 * i] == 's',
              "%s: report %d's wait is not immediately followed by its "
              "send (%s)", what, i, t);
    }
}

static void test_buffer_is_held_for_the_whole_frame(void)
{
    unsigned char frame[600];
    int framelen;

    framelen = build_frame(frame, 20);
    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);
    check_trace_is_wellformed("a single-report frame", hidstub_tx_count());

    framelen = build_frame(frame, 80);
    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);
    CHECK(hidstub_tx_count() == 2, "expected two reports, got %d",
          hidstub_tx_count());
    check_trace_is_wellformed("a two-report frame", hidstub_tx_count());

    framelen = build_frame(frame, 250);
    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);
    CHECK(hidstub_tx_count() >= 4, "expected at least four reports, got %d",
          hidstub_tx_count());
    check_trace_is_wellformed("a long frame", hidstub_tx_count());
}

/* Every report must be exactly its declared size and carry the report
 * ID the table says fits. Deriving both from report 0, which the older
 * cases do, cannot tell "the smallest that fits" from "always the
 * largest", and cannot see a transfer length that ignores report_size. */
/* A disconnect can land on the USB thread while iap_hid_tx() is blocked
 * in semaphore_wait(), which is up to 20 ms per fragment.
 * usb_core tears the endpoints down straight after
 * usb_iap_hid_disconnect() returns, so the remaining fragments would be
 * handed to an endpoint that no longer exists -- and usb-designware's
 * maxpktsize() reads 0 from a deconfigured endpoint, which the packet
 * count divides by. Checking the gate once before the loop is not
 * enough. */
static void test_disconnect_mid_frame_stops_sending(void)
{
    unsigned char frame[600];
    int framelen = build_frame(frame, 250);

    hidstub_reset();
    hidstub_set_active(true);
    hidstub_deactivate_after(2);        /* gone after the second report */
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() == 2,
          "the transport kept sending after it was deactivated: %d "
          "reports went out where the frame was cut short at 2",
          hidstub_tx_count());

    /* The lock must still be released, or the next frame deadlocks. */
    const char *t = hidstub_trace();
    CHECK(t[strlen(t) - 1] == 'U',
          "the frame lock was not released when the frame was abandoned "
          "(%s)", t);

    /* And the transport recovers once it comes back. */
    hidstub_reset();
    hidstub_set_active(true);
    iap_hid_tx_for_test(frame, framelen);
    CHECK(hidstub_tx_count() >= 4,
          "the transport did not resume after reactivation");
}

/* The window the comment on that gate describes is the wait itself, and
 * the check sat above it. hidstub_deactivate_after() clears the flag
 * after a send returns, so it can only ever exercise the top of the
 * loop -- the case above passes against a sender that never re-checks.
 *
 * This one disconnects from inside the wait, which is what actually
 * happens: usb_iap_hid_disconnect() runs on the USB thread and its
 * semaphore_release() is what wakes this thread, straight into the
 * memcpy and the send. */
static void test_disconnect_during_the_wait_stops_sending(void)
{
    unsigned char frame[600];
    int framelen = build_frame(frame, 250);

    hidstub_reset();
    hidstub_set_active(true);
    hidstub_deactivate_in_wait_at(2);   /* gone while waiting for #3 */
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() == 2,
          "%d reports went out although the transport was torn down "
          "during the wait before the third; usb_core deconfigures the "
          "endpoint as soon as the disconnect returns",
          hidstub_tx_count());

    const char *t = hidstub_trace();
    CHECK(t[strlen(t) - 1] == 'U',
          "the frame lock was not released when the frame was abandoned "
          "(%s)", t);
}

/* A wait that times out means the previous fragment is still the source
 * of a live DMA read. Refilling tx_buf then corrupts the report already
 * on the wire -- precisely what the semaphore exists to prevent -- and
 * the return was discarded. The host having stopped polling for 20 ms
 * is enough to get here. */
static void test_tx_timeout_abandons_the_frame(void)
{
    unsigned char frame[600];
    int framelen = build_frame(frame, 250);

    hidstub_reset();
    hidstub_set_active(true);
    hidstub_timeout_wait_at(2);         /* the wait before the third */
    iap_hid_tx_for_test(frame, framelen);

    CHECK(hidstub_tx_count() == 2,
          "%d reports went out after a wait timed out; the third refilled "
          "the buffer the second was still being read from",
          hidstub_tx_count());

    const char *t = hidstub_trace();
    CHECK(t[strlen(t) - 1] == 'U',
          "the frame lock was not released after a timeout (%s)", t);

    /* And the transport still works afterwards. */
    hidstub_reset();
    hidstub_set_active(true);
    iap_hid_tx_for_test(frame, framelen);
    CHECK(hidstub_tx_count() >= 4,
          "the transport did not recover after an abandoned frame");
}

static void test_report_id_and_length_match_the_table(void)
{
    static const struct { int payload; int id; int size; } expect[] = {
        {  6, 1, 12 },
        { 10, 2, 14 },
        { 12, 3, 20 },
        { 16, 3, 20 },
        { 20, 4, 63 },
        { 59, 4, 63 },
    };
    unsigned char frame[600];
    unsigned i;
    int framelen, k;

    for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        framelen = build_frame(frame, expect[i].payload);
        hidstub_reset();
        iap_hid_tx_for_test(frame, framelen);

        CHECK(hidstub_tx_count() == 1,
              "payload %d should be one report, got %d",
              expect[i].payload, hidstub_tx_count());
        if (hidstub_tx_count() != 1)
            continue;

        CHECK_EQ(hidstub_tx(0)->data[0], expect[i].id,
                 "the smallest report ID that fits");
        CHECK_EQ(hidstub_tx(0)->len, 1 + expect[i].size,
                 "the transfer is the report ID plus the whole report");
    }

    framelen = build_frame(frame, 200);
    hidstub_reset();
    iap_hid_tx_for_test(frame, framelen);
    for (k = 0; k < hidstub_tx_count(); k++) {
        CHECK_EQ(hidstub_tx(k)->data[0], 4, "fragmented frames use ID 4");
        CHECK_EQ(hidstub_tx(k)->len, 64, "and its full 64-byte transfer");
    }
}


/* ------------------------------------------------------------------ */
/* Control requests                                                     */
/* ------------------------------------------------------------------ */

/* usb_iap_hid_control_request() had no coverage at all, including the
 * 32-byte out-of-bounds write dfb271a239 fixed: the report descriptor
 * is 96 bytes, wDescriptorLength advertises exactly that, and the
 * answer used to be staged in the 64-byte rx_buf.
 *
 * dest here is usb_core's 256-byte response_data, which is what the
 * other class drivers answer from. */
#define HID_DT_HID_    0x21
#define HID_DT_REPORT_ 0x22

extern bool usb_iap_hid_control_request(struct usb_ctrlrequest *req,
                                        void *reqdata, unsigned char *dest);
extern int usb_iap_hid_set_first_interface(int interface);
extern const unsigned char iap_hid_report_desc[];

static void test_control_get_report_descriptor(void)
{
    unsigned char dest[256];
    struct usb_ctrlrequest req;
    bool handled;

    /* A conformant host asks for exactly wDescriptorLength, 96. */
    memset(dest, 0xEE, sizeof(dest));
    hidstub_ctrl_reset();
    req.bRequestType = 0x81;
    req.bRequest = 0x06;                 /* USB_REQ_GET_DESCRIPTOR */
    req.wValue = HID_DT_REPORT_ << 8;
    req.wIndex = 0;
    req.wLength = 96;

    handled = usb_iap_hid_control_request(&req, NULL, dest);
    CHECK(handled, "the report descriptor request was not handled");
    CHECK_EQ(hidstub_ctrl_len(), 96, "the whole descriptor is returned");
    CHECK(memcmp(hidstub_ctrl(), iap_hid_report_desc, 96) == 0,
          "the bytes returned are the descriptor");

    /* Nothing may have been written past the 64-byte rx_buf, which is
     * where the answer used to be staged. */
    CHECK_EQ(hidstub_rx_canary_check(), 0,
             "the report descriptor was staged in rx_buf again and wrote "
             "past its end");

    /* A host asking for less gets less, and no more. */
    hidstub_ctrl_reset();
    req.wLength = 16;
    usb_iap_hid_control_request(&req, NULL, dest);
    CHECK_EQ(hidstub_ctrl_len(), 16, "a short request is truncated");
    CHECK_EQ(hidstub_rx_canary_check(), 0, "and still stays in bounds");

    /* A host asking for more than exists gets only what exists. */
    hidstub_ctrl_reset();
    req.wLength = 512;
    usb_iap_hid_control_request(&req, NULL, dest);
    CHECK_EQ(hidstub_ctrl_len(), 96,
             "an over-long request must not read past the descriptor");

    /* The HID descriptor itself. */
    hidstub_ctrl_reset();
    req.wValue = HID_DT_HID_ << 8;
    req.wLength = 64;
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "the HID descriptor request was not handled");
    CHECK_EQ(hidstub_ctrl_len(), 9, "the HID descriptor is nine bytes");

    /* An unknown descriptor type is declined, not answered. */
    hidstub_ctrl_reset();
    req.wValue = 0x33 << 8;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "an unknown descriptor type was answered");
}

/* A SET_REPORT with no data stage, or one marked device-to-host, must
 * be refused rather than answered with USB_CONTROL_RECEIVE.
 *
 * usb-designware.c:936 enters EP0_REQ_CTRLWRITE only for
 * "wLength > 0 && !(bRequestType & USB_DIR_IN)", and :974 answers a
 * USB_CONTROL_RECEIVE outside that state with panicf("bad response").
 * So one malformed control packet from the host took the whole player
 * down, for as long as the iPod was docked and enumerated.
 * usb_hid.c:617 guards the same request the same way. */
static void test_control_set_report_without_a_data_stage(void)
{
    unsigned char dest[256];
    struct usb_ctrlrequest req;

    req.bRequest = 0x09;                 /* HID_REQ_SET_REPORT */
    req.wValue = 0;
    req.wIndex = 0;

    /* No data stage. */
    hidstub_ctrl_reset();
    req.bRequestType = 0x21;             /* host to device */
    req.wLength = 0;
    CHECK_EQ(usb_iap_hid_control_request(&req, NULL, dest), false,
             "a SET_REPORT with wLength 0 was accepted; "
             "usb-designware.c panics on the USB_CONTROL_RECEIVE that "
             "follows");
    CHECK_EQ(hidstub_ctrl_len(), -1,
             "a SET_REPORT with no data stage still answered EP0");

    /* Marked device-to-host, which has no OUT stage either. */
    hidstub_ctrl_reset();
    req.bRequestType = 0xA1;             /* device to host */
    req.wLength = 8;
    CHECK_EQ(usb_iap_hid_control_request(&req, NULL, dest), false,
             "a device-to-host SET_REPORT was accepted");
    CHECK_EQ(hidstub_ctrl_len(), -1,
             "a device-to-host SET_REPORT still answered EP0");

    /* And a well-formed one is still taken. */
    hidstub_ctrl_reset();
    req.bRequestType = 0x21;
    req.wLength = 8;
    CHECK_EQ(usb_iap_hid_control_request(&req, NULL, dest), true,
             "a well-formed SET_REPORT was refused");
    /* 2 is USB_CONTROL_RECEIVE (firmware/export/usb_drv.h:59-63). */
    CHECK_EQ(hidstub_ctrl_resp(), 2,
             "a well-formed SET_REPORT was not answered with RECEIVE");
}

/* SET_REPORT's first pass accepts into rx_buf; the second feeds it to
 * the framer. Passing the host's wLength straight through let a request
 * claiming more than the buffer feed whatever followed it in. */
static void test_control_set_report_is_bounded(void)
{
    unsigned char dest[256];
    struct usb_ctrlrequest req;
    unsigned char body[8];

    req.bRequestType = 0x21;
    req.bRequest = 0x09;                 /* HID_REQ_SET_REPORT */
    req.wValue = 0;
    req.wIndex = 0;

    /* A wLength far past rx_buf must not make either pass run off the
     * end of it. Measured by how many bytes reach the framer, not by a
     * canary.
     *
     * hidstub_rx_canary_check() used to stand here and it was never a
     * canary. It ORed the 32 bytes after rx_buf and called any non-zero
     * a hit -- so an overflow writing zeroes was invisible, which is
     * what a memset of the unclamped length would have been. Planting a
     * pattern there instead does not work either, and the reason is
     * worth recording: nm on this binary puts _saved_transport_send at
     * exactly rx_buf + 64. That memory belongs to another static, the
     * firmware writes it legitimately in the lazy-activation block, and
     * a canary there reports its own corruption as an overflow. It also
     * clobbers a function pointer on the way.
     *
     * rx_buf is a static in another translation unit, so nothing the
     * harness can do will reserve the bytes after it. What is
     * observable, and what actually matters, is the number of bytes
     * handed to iap_getc(). */
    hidstub_ctrl_reset();
    hidstub_rx_clear();
    req.wLength = 4096;
    usb_iap_hid_control_request(&req, NULL, dest);        /* first pass */
    CHECK_EQ(hidstub_rx_count(), 0,
             "the first pass fed the framer, which is the second pass's "
             "job");

    hidstub_rx_clear();
    usb_iap_hid_control_request(&req, body, dest);        /* second pass */
    CHECK(hidstub_rx_count() <= 63,
          "a wLength of 4096 fed %d bytes to the framer out of a "
          "64-byte buffer", hidstub_rx_count());

    /* Honest about what this does not catch: removing the MIN() at the
     * call site is an equivalent mutant, because iap_hid_process_rx()
     * clamps again from the report-size table. That inner clamp is what
     * actually bounds the feed and the sub-case below is what covers
     * it. This one would catch a feed that escaped both. */

    /* And the clamp itself, which only shows with a report ID the
     * table does not know.
     *
     * iap_hid_process_rx() starts with iap_len = len - 2 and narrows it
     * to size - 1 only when it finds the ID; the trailing
     * "if (iap_len > len - 2)" cannot narrow it further because it is
     * already that value. So an unknown ID with an unclamped wLength of
     * 4096 feeds the framer 4094 bytes out of a 64-byte buffer.
     *
     * With report ID 9 both paths give 62, which is why an earlier
     * version of this case called the clamp an equivalent mutant. It
     * is not; that conclusion came from testing one report ID. */
    static const unsigned char unknown[8] = { 0x40, 0x00, 0x55, 0x02,
                                              0x00, 0x07, 0xF7, 0x00 };
    hidstub_ctrl_reset();
    req.wLength = 4096;
    usb_iap_hid_control_request(&req, NULL, dest);        /* first pass */
    hidstub_rx_seed(unknown, sizeof(unknown));
    hidstub_rx_clear();
    usb_iap_hid_control_request(&req, body, dest);        /* second pass */

    CHECK(hidstub_rx_count() <= 63,
          "an unknown report ID with wLength 4096 fed the framer %d "
          "bytes from a 64-byte buffer", hidstub_rx_count());
}

/* GET_REPORT answers zeros, bounded by the same buffer. */
static void test_control_get_report_is_bounded(void)
{
    unsigned char dest[256];
    struct usb_ctrlrequest req;

    req.bRequestType = 0xA1;
    req.bRequest = 0x01;                 /* HID_REQ_GET_REPORT */
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = 4096;

    memset(dest, 0xA5, sizeof(dest));
    hidstub_ctrl_reset();
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "GET_REPORT was not handled");
    CHECK(hidstub_ctrl_len() <= 64,
          "GET_REPORT returned %d bytes from a 64-byte buffer",
          hidstub_ctrl_len());
    for (int i = 0; i < hidstub_ctrl_len(); i++) {
        CHECK_EQ(dest[i], 0, "GET_REPORT did not initialize its destination");
        CHECK_EQ(hidstub_ctrl()[i], 0, "GET_REPORT did not return zeros");
    }
    CHECK_EQ(hidstub_rx_canary_check(), 0,
             "GET_REPORT zeroed past the end of rx_buf");
}

static void test_control_requests_validate_their_shape(void)
{
    unsigned char dest[256];
    unsigned char body[64] = { 0 };
    struct usb_ctrlrequest req = { 0 };

    usb_iap_hid_set_first_interface(3);

    req.bRequestType = 0x81;
    req.bRequest = 0x06;
    req.wValue = HID_DT_REPORT_ << 8;
    req.wIndex = 3;
    req.wLength = 16;
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "a valid descriptor request was refused");

    req.bRequestType = 0x01;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "an OUT descriptor request was accepted");
    req.bRequestType = 0x81;
    req.wIndex = 2;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a descriptor request for another interface was accepted");
    req.wIndex = 3;
    req.wValue |= 1;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a nonzero descriptor index was accepted");

    req.bRequestType = 0xA1;
    req.bRequest = 0x01;
    req.wValue = 0;
    req.wIndex = 3;
    req.wLength = 8;
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "a valid GET_REPORT was refused");
    req.bRequestType = 0x81;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a standard request collided with GET_REPORT");

    req.bRequestType = 0x21;
    req.bRequest = 0x09;
    req.wLength = sizeof(body);
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "a valid SET_REPORT was refused");
    req.wLength++;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "an oversized SET_REPORT was accepted");
    req.wLength = sizeof(body);
    req.wIndex = 2;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a SET_REPORT for another interface was accepted");

    req.bRequestType = 0x21;
    req.bRequest = 0x0A;
    req.wIndex = 3;
    req.wLength = 0;
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "a valid SET_IDLE was refused");
    req.wLength = 1;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a SET_IDLE with data was accepted");

    req.bRequestType = 0x40;
    req.bRequest = 0x40;
    CHECK(usb_iap_hid_control_request(&req, NULL, dest),
          "the Apple vendor request was refused");
    req.bRequestType = 0x00;
    CHECK(!usb_iap_hid_control_request(&req, NULL, dest),
          "a standard request collided with the Apple vendor request");

    usb_iap_hid_set_first_interface(0);
}

/* ------------------------------------------------------------------ */

/* Three bounds in this file that a mutation sweep found unswept once
 * mutate.py learned to reach the transport drivers at all. Each is a
 * length check between the host's claim and a fixed buffer, and each
 * could be disabled with every binary green.
 *
 * The sync-byte strip (usb_iap_hid.c:262). MFi 2.2.2.3 (p.90) says the
 * sync byte "is unnecessary and should be omitted" over HID, so the
 * send path drops a leading 0xFF -- and a frame that is nothing but
 * that byte leaves nothing to send.
 *
 * The receive floor (:395). A report shorter than a report ID plus a
 * link-control byte plus one payload byte carries no frame.
 *
 * The receive clamp (:448), which is the one that matters: iap_len
 * comes from the report-size table, and a short transfer must not make
 * the framer read past what arrived. */
static void test_transport_bounds_hold(void)
{
    /* A frame that is only a sync byte: nothing goes out. */
    hidstub_reset();
    unsigned char just_sync[1] = { 0xFF };
    iap_hid_tx_for_test(just_sync, 1);
    CHECK_EQ(hidstub_tx_count(), 0,
             "a frame containing only the sync byte was transmitted");

    /* And one sync byte plus a payload does go out, so the check above
     * is not passing because sending is broken. */
    hidstub_reset();
    unsigned char sync_and_body[3] = { 0xFF, 0x55, 0x02 };
    iap_hid_tx_for_test(sync_and_body, 3);
    CHECK(hidstub_tx_count() > 0,
          "a frame with a payload after the sync byte was dropped");

    /* Receive: a report too short to hold a frame is ignored.
     *
     * A two-byte report is the largest this can assert on from here.
     * The guard exists to stop data[1] being read when len is 1, and
     * that difference is a read one byte past the caller's buffer --
     * invisible without a canary or a sanitiser, so the mutation sweep
     * still lists usb_iap_hid.c:395 as a survivor and should. Recorded
     * rather than papered over. */
    hidstub_reset();
    hidstub_rx_clear();
    unsigned char runt[2] = { 0x04, 0x00 };
    iap_hid_process_rx_for_test(runt, 2);
    CHECK_EQ(hidstub_rx_count(), 0,
             "a report too short to carry a frame was fed to the framer");

    /* And the clamp: report ID 4 claims 63 bytes, but only eight
     * arrived. The framer must see six -- the eight minus the report ID
     * and the link-control byte -- not the sixty-one the table
     * promises. */
    hidstub_reset();
    hidstub_rx_clear();
    unsigned char shortfall[8] = { 0x04, 0x00, 0xFF, 0x55, 0x02,
                                   0x00, 0x03, 0xFB };
    iap_hid_process_rx_for_test(shortfall, 8);
    CHECK_EQ(hidstub_rx_count(), 6,
             "the framer was fed more bytes than the transfer carried");
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    { "hid_control_get_report_descriptor", test_control_get_report_descriptor },
    { "hid_control_set_report_bounded",    test_control_set_report_is_bounded },
    { "hid_control_set_report_no_data",    test_control_set_report_without_a_data_stage },
    { "hid_transport_bounds_hold",         test_transport_bounds_hold },
    { "hid_control_get_report_bounded",    test_control_get_report_is_bounded },
    { "hid_control_request_shape",
      test_control_requests_validate_their_shape },
    { "hid_rx_link_control_is_masked",       test_rx_link_control_is_masked_consistently },
    { "hid_rx_missing_sync_ends_reassembly", test_rx_missing_sync_marker_ends_reassembly },
    { "hid_rx_unknown_report_id_bounded",    test_rx_unknown_report_id_is_bounded },
    { "hid_rx_every_report_id_reserves_lcb", test_rx_every_report_id_reserves_the_lcb },
    { "hid_rx_reassembles_a_split_frame",    test_rx_reassembles_a_split_frame },
    { "hid_single_report_frames",            test_single_report_frames },
    { "hid_the_old_truncation_boundary",     test_the_old_truncation_boundary },
    { "hid_fragmented_frames",               test_fragmented_frames },
    { "hid_link_control_sequence",           test_link_control_sequence },
    { "hid_every_fragment_waits",            test_every_fragment_waits_for_the_buffer },
    { "hid_buffer_held_for_whole_frame",     test_buffer_is_held_for_the_whole_frame },
    { "hid_report_id_and_length",            test_report_id_and_length_match_the_table },
    { "hid_disconnect_mid_frame",            test_disconnect_mid_frame_stops_sending },
    { "hid_disconnect_during_wait",          test_disconnect_during_the_wait_stops_sending },
    { "hid_tx_timeout_abandons_frame",       test_tx_timeout_abandons_the_frame },
    { "hid_sync_byte_is_omitted",            test_sync_byte_is_omitted },
    { "hid_inactive_transport_sends_nothing", test_inactive_transport_sends_nothing },
};

int main(void)
{
    unsigned i;
    int failed_cases = 0;

    printf("iAP over USB HID: transmit fragmentation\n");
    printf("  MFi Accessory Firmware Specification R46\n\n");

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int before = failures;
        current = cases[i].name;
        hidstub_reset();
        hidstub_set_active(true);
        cases[i].fn();
        if (failures > before) {
            failed_cases++;
            printf("  \033[31mFAIL\033[0m  %s\n", cases[i].name);
        } else {
            printf("  \033[32mok\033[0m    %s\n", cases[i].name);
        }
    }

    printf("\n%u case%s run, %d check%s, %d failure%s\n",
           i, i == 1 ? "" : "s", checks, checks == 1 ? "" : "s",
           failures, failures == 1 ? "" : "s");
    return failed_cases ? 1 : 0;
}
