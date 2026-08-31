/***************************************************************************
 * iAP protocol test harness.
 *
 * Captures everything the protocol layer transmits, and lets a test feed
 * synthetic accessory packets in. Framing here mirrors iap_send_tx()
 * (apps/iap/iap-core.c) and MFi R46 section 2.5 "Command Packets".
 ****************************************************************************/

#include "iap_test.h"
#include "sound.h"
#include "accessory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"

extern void iap_handlepkt(void);
extern int  remote_control_rx(void);

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

static struct iaptest_pkt tx_log[IAPTEST_MAX_TX];
static int tx_n;

int iaptest_tx_count(void) { return tx_n; }
void iaptest_tx_clear(void) { tx_n = 0; }

const struct iaptest_pkt *iaptest_tx(int index)
{
    if (index < 0 || index >= tx_n)
        return NULL;
    return &tx_log[index];
}

/* Decode one framed packet into the log entry, validating the length
 * form and the checksum the way a conformant accessory would. */
static void capture(const unsigned char *buf, int len)
{
    if (tx_n >= IAPTEST_MAX_TX)
        return;

    struct iaptest_pkt *p = &tx_log[tx_n++];
    memset(p, 0, sizeof(*p));

    if (len > IAPTEST_MAX_TXLEN)
        len = IAPTEST_MAX_TXLEN;
    memcpy(p->raw, buf, len);
    p->rawlen = len;

    if (len < 5 || buf[0] != 0xFF || buf[1] != 0x55)
        return;

    int paylen, hdr, sum;

    if (buf[2] != 0x00) {
        /* Short form. MFi 2.5.2: a 1-byte length field expresses 0x02
         * to 0xFC. 0xFD..0xFF must use the 3-byte form. */
        paylen = buf[2];
        hdr = 3;
        p->length_form_ok = (paylen >= 0x02 && paylen <= 0xFC);
        sum = paylen;
    } else {
        /* Long form: 0x00 marker then a 16-bit length, 0x00FD..0xFFFA. */
        paylen = (buf[3] << 8) | buf[4];
        hdr = 5;
        p->length_form_ok = (paylen >= 0x00FD && paylen <= 0xFFFA);
        sum = buf[3] + buf[4];
    }

    if (hdr + paylen + 1 > len)
        return;

    p->payload = p->raw + hdr;
    p->paylen  = paylen;

    for (int i = 0; i < paylen; i++)
        sum += p->payload[i];
    p->checksum_ok = (((sum + p->payload[paylen]) & 0xFF) == 0);
}

static void capture_send(const unsigned char *buf, int len)
{
    capture(buf, len);

    /* Let the accessory model judge it. Attached, this validates the
     * transaction-ID rules on every packet of every test, not just the
     * ones somebody wrote an assertion for. */
    if (tx_n > 0) {
        const struct iaptest_pkt *p = &tx_log[tx_n - 1];

        /* Every frame the device puts on the wire, checked here.
         *
         * The accessory model is handed the payload and the length and
         * never sees the frame, so it structurally could not verify a
         * checksum -- corrupting every one of them left most of the
         * suite green. check_output_wellformed() does check, but only
         * the malformed sweeps call it. This is the one place every
         * transmission passes through. */
        iaptest_checked(2);

        if (!p->checksum_ok) {
            iaptest_fail(__FILE__, __LINE__,
                         "the device transmitted a frame with a bad "
                         "checksum");
            iaptest_hexdump("raw", p->raw, p->rawlen);
        }
        if (!p->length_form_ok)
            iaptest_fail(__FILE__, __LINE__,
                         "the device transmitted a %d byte payload in "
                         "an out-of-range length form; MFi 2.5.2 "
                         "(p.110) allows 0x02-0xFC short and "
                         "0x00FD-0xFFFA long", p->paylen);

        if (p->payload && p->paylen >= 2)
            iapacc_observe(p->payload, p->paylen);
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up and RX injection                                           */
/* ------------------------------------------------------------------ */

static bool iap_is_up;

void iaptest_init(void)
{
    rbstub_reset();
    tx_n = 0;

    if (iap_is_up) {
        /* Start each case from a clean protocol state, as if the
         * accessory had just been unplugged and plugged back in. */
        iap_reset_device(&device);
    }
    iap_is_up = true;

    /* iap_setup() clears iap_remotebtn but not the two counters that
     * pace button delivery. iap_handlepkt() refuses to process anything
     * while iap_repeatbtn is set, so a case that ends mid-press would
     * otherwise stall every packet in the case after it. */
    iap_repeatbtn = 0;
    iap_timeoutbtn = 0;

    iap_setup(0);

    /* On hardware the first sync byte makes iap_getc() post IAP_EV_MALLOC,
     * and the iAP thread answers it by calling iap_malloc(), which
     * allocates the buffers and sets iap_running. There is no thread here,
     * so stand in for it. */
    iap_malloc();

    iap_transport_send = capture_send;
    iapacc_reset();
    iapacc_attach();
    tx_n = 0;
}

void iaptest_rx(const unsigned char *payload, int len)
{
    unsigned char frame[IAPTEST_MAX_TXLEN];
    int n = 0, sum, i;

    frame[n++] = 0xFF;
    frame[n++] = 0x55;

    if (len <= 0xFC) {
        frame[n++] = (unsigned char)len;
        sum = len;
    } else {
        frame[n++] = 0x00;
        frame[n++] = (len >> 8) & 0xFF;
        frame[n++] = len & 0xFF;
        sum = ((len >> 8) & 0xFF) + (len & 0xFF);
    }

    for (i = 0; i < len; i++) {
        frame[n++] = payload[i];
        sum += payload[i];
    }
    frame[n++] = (unsigned char)(0x100 - (sum & 0xFF));

    iapacc_note_sent(payload, len);

    for (i = 0; i < n; i++)
        iap_getc(frame[i]);

    /* A packet sent while a raised button is still going out is
     * re-queued by iap_handlepkt(), not handled -- so the case that
     * sent it is asserting against a packet the firmware never saw.
     * That has produced two silent false passes here already, one in a
     * harness helper and one in a button-repeat case that read as eight
     * repeats and delivered four. Count them; the runner fails any case
     * that leaves the count moving. */
    if (iap_repeatbtn)
        iaptest_deferred_rx++;

    iap_handlepkt();
}

void iaptest_enter_idps(void)
{
    /* A fresh IDPS is a fresh session: MFi 2.6.1.1 (p.111) has the
     * counter re-initialised "every time it is connected", and the
     * device restarts its own at 1. Reset the model too, or a case that
     * identifies more than once sees the second session's IDs as reuse
     * of the first's. */
    iapacc_reset();

    /* StartIDPS, transaction 0x0001. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);

    /* SetFIDTokenValues, transaction 0x0002, carrying one IdentifyToken
     * (FIDType 0x00, FIDSubtype 0x00) that declares the lingoes a
     * full-featured accessory asks for: General, Simple Remote, Display
     * Remote, Extended Interface and Digital Audio. General lingo
     * command 0x05 is refused unless 0x04 is declared here.
     *
     * Token layout after its length byte: type, subtype, lingo count,
     * the lingo bytes, then a 4-byte options word and a 4-byte device
     * id, so the length is 3 + 5 + 8 = 16.
     */
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);

    /* EndIDPS, transaction 0x0003, status 0x00 = Continue. */
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);

    if (!device.auth.idps)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: IDPS bring-up did not set device.auth.idps");
    if (!(device.lingoes & (1u << 0x04)))
        iaptest_fail(__FILE__, __LINE__,
                     "harness: IdentifyToken did not negotiate lingo 0x04 "
                     "(device.lingoes = 0x%08X)", device.lingoes);
    tx_n = 0;
}

void iaptest_detach_model_for_raw_probes(void)
{
    iapacc_detach();
}

void iaptest_button_sample(int times)
{
    for (int i = 0; i < times; i++)
        remote_control_rx();
}

void iaptest_force_authenticated(void)
{
    /* MFi 2.4.2 treats the accessory as authenticated from the moment the
     * Apple device acks its authentication info, which is the transition
     * into AUST_CERTDONE (see DEVICE_AUTHENTICATED in iap-core.h).
     *
     * Raise only. This used to assign AUST_CERTDONE outright, which is
     * a downgrade after IdentifyDeviceLingoes -- that leaves AUST_AUTH,
     * two states further on. Both satisfy DEVICE_AUTHENTICATED so the
     * name stayed true, but DEVICE_AUTH_RUNNING is
     * "state != AUST_NONE && state != AUST_AUTH", so the helper turned
     * a finished handshake back into one in progress. That silently
     * blocked the capability sweep in one case and the certificate
     * machine in another, and cost a debugging cycle each time. A
     * helper called force_authenticated has no business lowering
     * anything. */
    if (device.auth.state < AUST_CERTDONE)
        device.auth.state = AUST_CERTDONE;
}

/* An authenticated accessory in Extended Interface mode, which is what
 * most Extended Interface cases actually want.
 *
 * Open-coded, this is four steps and three of them have a trap. The
 * lingo has to be declared or iap-lingo0.c refuses the mode. The mode
 * has to be entered explicitly, since iap_reset_device() clears it --
 * cases that skipped this used to pass on the mode a previous case left
 * set. And entering it raises BUTTON_RC_PLAY while playing, which makes
 * iap_handlepkt() defer the next packet, so the button has to go out
 * before the case sends anything. Each of those cost a debugging cycle
 * when a case got it wrong. */
void iaptest_session_extended(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    IAPTEST_RX(0x00, 0x05, 0x00, 0xC0);

    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* And say so if it did not take. Deleting the line above left four
     * of this helper's sixteen callers green, two of them cases that
     * assert a command "is refused" -- which a mode that was never
     * entered refuses just as convincingly, and for the wrong reason.
     * Every other bring-up helper checks its own postcondition; this
     * was the one that did not. */
    if (interface_state != IST_EXTENDED)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: the session is not in Extended Interface "
                     "mode (interface_state = %d), so anything the case "
                     "sends on lingo 4 is refused for the mode rather "
                     "than for what it is testing", (int)interface_state);

    iaptest_tx_clear();
}

void iaptest_identify_legacy(uint32_t lingo_mask)
{
    /* Let any raised button go out first. iap_handlepkt() re-queues
     * every packet while iap_repeatbtn is set, so a case that
     * re-identifies after sending anything that raises a button -- a
     * PlayControl, entering Extended Interface mode while playing --
     * had its identify silently dropped. device.lingoes then still held
     * the previous identification, which is not zero, so the check
     * below saw nothing wrong. */
    for (int i = 0; i < 4 && iap_repeatbtn; i++)
        remote_control_rx();
    if (iap_repeatbtn)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: the button deferral is still armed "
                     "(iap_repeatbtn = %d), so this identify would be "
                     "re-queued rather than handled", iap_repeatbtn);

    /* IdentifyDeviceLingoes (0x00/0x13): lingo bitmask, options, deviceid.
     * MFi 2.6.1.4 exempts this command from transaction IDs. */
    unsigned char p[14] = { 0x00, 0x13 };
    p[2] = (lingo_mask >> 24) & 0xFF;
    p[3] = (lingo_mask >> 16) & 0xFF;
    p[4] = (lingo_mask >> 8) & 0xFF;
    p[5] = lingo_mask & 0xFF;
    iaptest_rx(p, sizeof(p));

    if (device.auth.idps)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: legacy identify unexpectedly enabled IDPS");

    /* Unsupported lingoes are filtered out of the mask, which two cases
     * test on purpose -- but a mask without the General lingo is
     * refused whole and negotiates nothing at all. A case built on that
     * finds every command rejected and reads it as the gate it meant to
     * test. Say so here rather than let it pass for the wrong reason. */
    if (lingo_mask && !device.lingoes)
        iaptest_fail(__FILE__, __LINE__,
                     "harness: IdentifyDeviceLingoes(0x%08X) negotiated "
                     "nothing -- the General lingo (bit 0) has to be in "
                     "the mask", lingo_mask);
    tx_n = 0;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

int iaptest_failures;
int iaptest_deferred_rx;
int iaptest_asserts;

/* Count a verification that did not go through CHECK.
 *
 * iaptest_fail() bumps iaptest_failures, never iaptest_checks, so a
 * helper that reports only through it verifies plenty and counts
 * nothing -- and the zero-assertion guard, which reads iaptest_checks,
 * cannot see the case at all. test_malformed.c's
 * check_output_wellformed() examines every packet a sweep emits and
 * reported exclusively that way: making it return immediately left the
 * check counts byte-identical and the suite green. So did the accessory
 * model, whose violation() is the same shape.
 *
 * Every place that judges something and can fail calls this. */
void iaptest_checked(int n)
{
    iaptest_checks += n;
}
int iaptest_checks;
const char *iaptest_current = "?";

void iaptest_hexdump(const char *label, const unsigned char *b, int len)
{
    fprintf(stderr, "      %-8s", label);
    for (int i = 0; i < len; i++)
        fprintf(stderr, " %02X", b[i]);
    fprintf(stderr, "\n");
}

void iaptest_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    const char *base = strrchr(file, '/');
    iaptest_failures++;
    fprintf(stderr, "  FAIL [%s] %s:%d\n        ",
            iaptest_current, base ? base + 1 : file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void iaptest_expect_payload(const char *file, int line, int idx,
                            const unsigned char *want, int wantlen)
{
    /* An explicit assertion, so the "case asserted nothing" guard can
     * see it. It reports through iaptest_fail() rather than CHECK, and
     * twelve cases verify with nothing else -- they were caught the
     * moment the guard stopped counting the automatic per-frame
     * checks. */
    iaptest_asserts++;
    iaptest_checks++;

    iaptest_checks++;

    const struct iaptest_pkt *p = iaptest_tx(idx);
    if (!p) {
        iaptest_fail(file, line,
                     "expected a packet at index %d, only %d were sent",
                     idx, tx_n);
        return;
    }
    if (!p->checksum_ok) {
        iaptest_fail(file, line, "packet %d has a bad checksum", idx);
        iaptest_hexdump("raw", p->raw, p->rawlen);
        return;
    }
    if (!p->length_form_ok) {
        iaptest_fail(file, line,
                     "packet %d uses an out-of-range length form "
                     "(payload %d, MFi 2.5.2 allows 0x02-0xFC short / "
                     "0xFD-0xFFFA long)", idx, p->paylen);
        iaptest_hexdump("raw", p->raw, p->rawlen);
        return;
    }
    if (p->paylen != wantlen || memcmp(p->payload, want, wantlen) != 0) {
        iaptest_fail(file, line, "packet %d payload mismatch", idx);
        iaptest_hexdump("want", want, wantlen);
        iaptest_hexdump("got", p->payload, p->paylen);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

/* A case that calls exit() takes the process with it: the loop never
 * finishes, no summary is printed, and the shell sees status 0 -- so a
 * run that executed 24 of 245 cases reported success. Nothing in the
 * output said so either, because the summary is the only line that
 * states how many ran.
 *
 * atexit() is the only hook that survives an exit() from inside a case.
 */
static bool iaptest_run_completed;

static void iaptest_check_completed(void)
{
    if (!iaptest_run_completed) {
        printf("\n\033[31mthe run did not finish\033[0m -- a case exited "
               "the process, so most of the suite never ran and the "
               "summary above is missing\n");
        /* _Exit() skips the stdio flush and exit() from inside an
         * atexit handler is undefined, so flush by hand first --
         * without this the diagnostic and everything the run had
         * printed were both lost. */
        fflush(stdout);
        _Exit(1);
    }
}

int main(int argc, char **argv)
{
    atexit(iaptest_check_completed);

    const char *filter = (argc > 1) ? argv[1] : NULL;
    int run = 0, failed_cases = 0;

    printf("iAP protocol conformance tests\n");
    printf("  target %s, MFi Accessory Firmware Specification R46\n\n",
           IAP_TEST_TARGET_NAME);

    /* Every constant the harness copies out of the firmware can drift
     * from it, silently, and no mutation sweep can see it: the mutant
     * and the original read the same wrong number. rb_stubs.c's
     * fm_region_data had drifted -- Europe's channel step was 50 kHz
     * against the real table's 100 kHz -- and every region test was
     * reasoning about a device that does not exist.
     *
     * IAP_TEST_VOLUME_MIN/MAX are the other copy, and they are
     * checkable: firmware/sound.c is compiled for real, so sound_min()
     * and sound_max() are the values the code under test uses. Say so
     * loudly rather than let a codec change quietly invalidate every
     * volume case. */
    if (IAP_TEST_VOLUME_MIN != sound_min(SOUND_VOLUME)
        || IAP_TEST_VOLUME_MAX != sound_max(SOUND_VOLUME)) {
        printf("  harness: IAP_TEST_VOLUME_MIN/MAX are %d..%d and the "
               "codec's range is %d..%d -- iap_test.h has drifted from "
               "the AUDIOHW_SETTING it copies\n",
               IAP_TEST_VOLUME_MIN, IAP_TEST_VOLUME_MAX,
               sound_min(SOUND_VOLUME), sound_max(SOUND_VOLUME));
        return 1;
    }

    for (int i = 0; i < iaptest_case_count; i++) {
        const struct iaptest_case *tc = &iaptest_cases[i];
        if (filter && !strstr(tc->name, filter))
            continue;

        int before = iaptest_failures;
        int checks_before = iaptest_asserts;
        int deferred_before = iaptest_deferred_rx;
        iaptest_current = tc->name;
        run++;

        iaptest_init();
        tc->fn();

        /* A case that asserts nothing is indistinguishable from one
         * that returned early, and reports ok either way. Two cases
         * were found contributing zero checks by an audit rather than
         * by the suite, and a harness change that quietly stopped seven
         * sweeps from being judged passed every run -- "the tests pass"
         * cannot see an assertion that is not made. */
        if (iaptest_asserts == checks_before) {
            iaptest_fail(__FILE__, __LINE__,
                         "case ran and asserted nothing");
            failed_cases++;
            printf("  \033[31mFAIL\033[0m  %s (no checks)\n", tc->name);
            continue;
        }

        /* Same shape of blindness as the check above, one layer down: a
         * packet re-queued behind a raised button never reaches the
         * handler, so whatever the case asserts next it is not
         * asserting about that packet. Drain with
         * iaptest_button_sample() before sending. */
        if (iaptest_deferred_rx != deferred_before)
            iaptest_fail(__FILE__, __LINE__,
                         "%d packet(s) were re-queued behind a raised "
                         "button instead of handled, so the case is "
                         "asserting against packets the firmware never "
                         "saw", iaptest_deferred_rx - deferred_before);

        if (iaptest_failures > before) {
            failed_cases++;
            printf("  \033[31mFAIL\033[0m  %s\n", tc->name);
        } else {
            printf("  \033[32mok\033[0m    %s\n", tc->name);
        }
    }

    iaptest_run_completed = true;

    printf("\n%d case%s run, %d check%s, %d failure%s\n",
           run, run == 1 ? "" : "s",
           iaptest_checks, iaptest_checks == 1 ? "" : "s",
           iaptest_failures, iaptest_failures == 1 ? "" : "s");

    return failed_cases ? 1 : 0;
}
