/***************************************************************************
 * The PP dock UART path -- the iPod Video's only iAP transport
 *
 * firmware/target/arm/pp/uart-pp.c feeds iap_getc() on every PP iPod,
 * and on the Video it is the whole story: USB_ENABLE_IAP_HID is not
 * defined for that target, so usb_iap_hid.c is not even compiled. Every
 * other suite here drives the HID transport. This one had no coverage
 * at all, which made the Video the one player whose entire accessory
 * path was assumed rather than tested.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "uart_stubs.h"

static int failures, checks;
static const char *current = "?";

#define CHECK(cond, ...) do {                                           \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "  FAIL [%s] %s:%d\n        ",              \
                    current, __FILE__, __LINE__);                       \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b, ...) do {                                        \
        long _a = (long)(a), _b = (long)(b);                            \
        checks++;                                                       \
        if (_a != _b) {                                                 \
            failures++;                                                 \
            fprintf(stderr, "  FAIL [%s] %s:%d\n        ",              \
                    current, __FILE__, __LINE__);                       \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, ": got %ld, want %ld\n", _a, _b);           \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */

/* The poll reads the accessory line every time it runs. If it stopped,
 * a detach could never be noticed.
 *
 * It is called from iap_periodic(), on the iAP thread. It was a
 * tick_add_task() first, and that panics the device within 400 ms of
 * boot: tick tasks run from the timer IRQ, and adc_read() here takes
 * i2c_lock() -> mutex_lock() whenever its cache has expired, which
 * asserts thread context. Nothing in this harness or the compiler can
 * see that -- the stub adc_read() is just a variable -- so the case
 * names it here instead. */
static void test_tick_reads_the_accessory_line(void)
{
    uartstub_reset();
    iap_uart_test_set_accessory_present(false);

    for (int i = 0; i < 3; i++)
        iap_accessory_poll();

    CHECK_EQ(uartstub.adc_reads, 3,
             "the poll read the accessory line %d times in 3 calls",
             uartstub.adc_reads);
}

/* MFi 4.3.11 (p.255): "on accessory detach, event notification is reset
 * to the default disabled state."
 *
 * iap_reset_state() had two callers and neither reached a dock detach
 * on this target: firmware/usb.c on USB extract, and
 * firmware/drivers/button.c on a headphone unplug, which is compiled
 * only for the 4G, mini and colour iPods -- they have their remote on
 * the jack, where here iAP is on the dock connector. So the session
 * survived, and the next accessory inherited the previous one's
 * authentication, negotiated lingoes, notification masks and
 * transaction-ID counter. */
static void test_detach_resets_the_session(void)
{
    uartstub_reset();
    uartstub.accessory_adc = 0;         /* attached */
    iap_uart_test_set_accessory_present(true);

    /* Steady state, attached: nothing happens. */
    for (int i = 0; i < 5; i++)
        iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 0,
             "the session was reset while the accessory stayed attached");

    /* Gone. ipod_remote_tuner.c reads the same line and treats >= 10 as
     * the accessory having left. */
    uartstub.accessory_adc = 50;
    iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 1,
             "a dock detach did not reset the iAP session");

    /* Still gone: the edge, not the level. A reset every tick would
     * fight any accessory that is slow to re-enumerate. */
    for (int i = 0; i < 5; i++)
        iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 1,
             "the session kept being reset while nothing was attached");
}

/* A replacement accessory must get the same treatment, or only the
 * first swap of a session is clean. */
static void test_second_detach_resets_again(void)
{
    uartstub_reset();
    uartstub.accessory_adc = 0;
    iap_uart_test_set_accessory_present(false);
    iap_accessory_poll();                     /* notices it arrive */

    uartstub.accessory_adc = 50;        /* first one leaves */
    iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 1, "first detach");

    uartstub.accessory_adc = 0;         /* a replacement arrives */
    iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 1,
             "attaching an accessory reset the session it just started");

    uartstub.accessory_adc = 50;        /* and leaves */
    iap_accessory_poll();
    CHECK_EQ(uartstub.resets, 2,
             "the second detach did not reset the session, so a third "
             "accessory would inherit the second one's state");
}

/* An accessory already gone when the player boots must not look like a
 * detach. serial_setup() seeds the flag from the line for that reason. */
static void test_boot_with_nothing_attached_is_not_a_detach(void)
{
    uartstub_reset();
    uartstub.accessory_adc = 50;        /* nothing there at boot */
    iap_uart_test_set_accessory_present(false);

    for (int i = 0; i < 5; i++)
        iap_accessory_poll();

    CHECK_EQ(uartstub.resets, 0,
             "booting with no accessory attached was treated as a detach");
}

/* ------------------------------------------------------------------ */

/* Everything the line delivers has to reach the protocol framer. This
 * is the whole receive path on the iPod Video: SERIAL_ISR() drains the
 * FIFO while the ready bit is set and hands each byte to iap_getc(). */
static void test_bytes_reach_the_framer(void)
{
    static const unsigned char pkt[] = { 0xFF, 0x55, 0x02, 0x00, 0x07, 0xF7 };

    uartstub_reset();
    iap_uart_test_set_autobaud(0);
    uartstub_feed(pkt, sizeof(pkt));
    SERIAL_ISR(0);

    CHECK_EQ(uartstub.nbytes, (int)sizeof(pkt),
             "the ISR delivered %d of %zu bytes", uartstub.nbytes,
             sizeof(pkt));
    for (unsigned i = 0; i < sizeof(pkt) && i < (unsigned)uartstub.nbytes; i++)
        CHECK_EQ(uartstub.bytes[i], pkt[i], "byte %u", i);
}

/* Autobaud mode 1. A sync byte arriving at the wrong bitrate is read as
 * a specific wrong value, and which value says what the real rate is:
 * 0xFC means the line is running at 19200, 0xE0 at 9600. The byte is
 * then rewritten to 0xFF before it goes on, because it *was* a sync
 * byte -- the framer must see one or it discards the packet that
 * follows. */
static void test_autobaud_mode1_maps_and_rewrites(void)
{
    static const struct { unsigned char raw; const char *what; } t[] = {
        { 0xFC, "0xFC (19200)" },
        { 0xE0, "0xE0 (9600)" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        unsigned char pkt[2] = { t[i].raw, 0x55 };

        uartstub_reset();
        iap_uart_test_set_autobaud(1);
        uartstub_feed(pkt, sizeof(pkt));
        SERIAL_ISR(0);

        CHECK_EQ(uartstub.nbytes, 2, "%s: bytes delivered", t[i].what);
        if (uartstub.nbytes >= 1)
            CHECK_EQ(uartstub.bytes[0], 0xFF,
                     "%s was passed through as-is; the framer needs a "
                     "sync byte and that is what it was", t[i].what);
    }
}

/* Autobaud mode 2 reads the same line at a different rate, so the same
 * wrong values mean different things: 0xFE is 57600, 0xFC is 38400,
 * 0xE0 is 19200. Each is still rewritten to a sync byte. */
static void test_autobaud_mode2_maps_and_rewrites(void)
{
    static const struct { unsigned char raw; const char *what; } t[] = {
        { 0xFE, "0xFE (57600)" },
        { 0xFC, "0xFC (38400)" },
        { 0xE0, "0xE0 (19200)" },
    };

    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        unsigned char pkt[2] = { t[i].raw, 0x55 };

        uartstub_reset();
        iap_uart_test_set_autobaud(2);
        uartstub_feed(pkt, sizeof(pkt));
        SERIAL_ISR(0);

        CHECK_EQ(uartstub.nbytes, 2, "%s: bytes delivered", t[i].what);
        if (uartstub.nbytes >= 1)
            CHECK_EQ(uartstub.bytes[0], 0xFF,
                     "%s was not rewritten to a sync byte", t[i].what);
    }
}

/* A real sync byte in either mode passes through untouched -- the rate
 * is already right, so there is nothing to correct. */
static void test_autobaud_leaves_a_good_sync_alone(void)
{
    for (int mode = 1; mode <= 2; mode++) {
        unsigned char pkt[2] = { 0xFF, 0x55 };

        uartstub_reset();
        iap_uart_test_set_autobaud(mode);
        uartstub_feed(pkt, sizeof(pkt));
        SERIAL_ISR(0);

        CHECK_EQ(uartstub.nbytes, 2, "mode %d: bytes delivered", mode);
        if (uartstub.nbytes >= 2) {
            CHECK_EQ(uartstub.bytes[0], 0xFF, "mode %d: sync byte", mode);
            CHECK_EQ(uartstub.bytes[1], 0x55, "mode %d: start byte", mode);
        }
    }
}

static void test_autobaud_locks_after_sof(void)
{
    static const unsigned char sync = 0xFF;
    static const unsigned char sof = 0x55;

    uartstub_reset();
    iap_uart_test_set_autobaud(2);

    uartstub_feed(&sync, 1);
    SERIAL_ISR(0);
    CHECK_EQ(iap_uart_test_get_autobaud(), 2,
             "autobaud locked before receiving the start-of-frame byte");

    uartstub_feed(&sof, 1);
    SERIAL_ISR(0);
    CHECK_EQ(iap_uart_test_get_autobaud(), 0,
             "autobaud did not lock after a valid sync/start pair");
}

static void test_autobaud_miss_count_survives_interrupts(void)
{
    static const unsigned char bad_sync = 0x12;

    uartstub_reset();
    iap_uart_test_set_autobaud(2);

    for (int i = 0; i < 5; i++)
    {
        uartstub_feed(&bad_sync, 1);
        SERIAL_ISR(0);
        CHECK_EQ(iap_uart_test_get_autobaud(), 2,
                 "autobaud changed modes after %d misses", i + 1);
    }

    uartstub_feed(&bad_sync, 1);
    SERIAL_ISR(0);
    CHECK_EQ(iap_uart_test_get_autobaud(), 1,
             "the sixth miss did not switch autobaud detection modes");
}

static void start_partial_frame(bool automatic)
{
    static const unsigned char prefix[] = { 0xFF, 0x55, 0x03, 0x00 };

    if (automatic)
        serial_bitrate(0);
    else
        serial_bitrate(19200);

    uartstub_feed(prefix, sizeof(prefix));
    SERIAL_ISR(0);
}

static void test_corrupt_bytes_are_discarded(void)
{
    static const struct {
        unsigned char error;
        const char *name;
    } cases[] = {
        { UARTSTUB_LSR_PARITY, "parity" },
        { UARTSTUB_LSR_FRAMING, "framing" },
        { UARTSTUB_LSR_BREAK, "break" },
    };
    static const unsigned char bytes[] = { 0x42, 0xFF, 0x55 };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        unsigned char errors[] = { cases[i].error, 0, 0 };

        uartstub_reset();
        start_partial_frame(false);
        int before = uartstub.nbytes;
        uartstub_feed_errors(bytes, errors, sizeof(bytes));
        SERIAL_ISR(0);

        CHECK_EQ(uartstub.flushes, 1, "%s error did not flush the frame",
                 cases[i].name);
        CHECK_EQ(uartstub.nbytes, before + 2,
                 "%s-corrupted byte reached the framer", cases[i].name);
        CHECK_EQ(uartstub.bytes[before], 0xFF,
                 "%s-corrupted byte was not discarded", cases[i].name);
        CHECK_EQ(iap_uart_test_get_autobaud(), 0,
                 "%s error rearmed a fixed bitrate", cases[i].name);
    }
}

static void test_overrun_preserves_current_byte(void)
{
    static const unsigned char bytes[] = { 0x42, 0xFF, 0x55 };
    static const unsigned char errors[] = { UARTSTUB_LSR_OVERRUN, 0, 0 };

    uartstub_reset();
    start_partial_frame(true);
    int before = uartstub.nbytes;
    uartstub_feed_errors(bytes, errors, sizeof(bytes));
    SERIAL_ISR(0);

    CHECK_EQ(uartstub.flushes, 1, "overrun did not flush the partial frame");
    CHECK_EQ(uartstub.nbytes, before + 3,
             "overrun-only current byte was discarded");
    CHECK_EQ(uartstub.bytes[before], 0x42,
             "overrun-only current byte changed");
    CHECK_EQ(iap_uart_test_get_autobaud(), 0,
             "overrun rearmed autobaud");
    CHECK(iap_uart_test_get_auto_bitrate(),
          "automatic bitrate intent was lost after locking");
}

static void test_only_framing_rearms_automatic_bitrate(void)
{
    static const struct {
        unsigned char error;
        const char *name;
    } no_rearm[] = {
        { UARTSTUB_LSR_PARITY, "parity" },
        { UARTSTUB_LSR_BREAK, "break" },
    };
    static const unsigned char byte = 0x42;

    for (unsigned i = 0; i < sizeof(no_rearm) / sizeof(no_rearm[0]); i++)
    {
        uartstub_reset();
        start_partial_frame(true);
        uartstub_feed_errors(&byte, &no_rearm[i].error, 1);
        SERIAL_ISR(0);

        CHECK_EQ(iap_uart_test_get_autobaud(), 0,
                 "%s error rearmed autobaud", no_rearm[i].name);
        CHECK_EQ(uartstub.flushes, 1, "%s error did not flush the frame",
                 no_rearm[i].name);
    }

    static const unsigned char bytes[] = { 0x42, 0x43 };
    static const unsigned char errors[] = { UARTSTUB_LSR_FRAMING, 0 };

    uartstub_reset();
    start_partial_frame(true);
    int before = uartstub.nbytes;
    uartstub_feed_errors(bytes, errors, sizeof(bytes));
    SERIAL_ISR(0);

    CHECK_EQ(uartstub.flushes, 1, "framing error did not flush the frame");
    CHECK_EQ(uartstub.nbytes, before,
             "old-rate bytes reached the framer after a framing error");
    CHECK_EQ(uartstub.fed, (int)sizeof(bytes),
             "old-rate bytes remained in the receive FIFO");
    CHECK_EQ(iap_uart_test_get_autobaud(), 2,
             "framing error did not rearm automatic bitrate detection");
    CHECK(iap_uart_test_get_auto_bitrate(),
          "framing recovery changed automatic bitrate intent");
}

static void test_fifo_error_summary_is_not_current_byte_error(void)
{
    static const unsigned char byte = 0x42;
    static const unsigned char error = UARTSTUB_LSR_FIFOERR;

    uartstub_reset();
    start_partial_frame(false);
    int before = uartstub.nbytes;
    uartstub_feed_errors(&byte, &error, 1);
    SERIAL_ISR(0);

    CHECK_EQ(uartstub.flushes, 0,
             "FIFO error summary was treated as a current-byte error");
    CHECK_EQ(uartstub.nbytes, before + 1,
             "FIFO error summary discarded the current byte");
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    { "uart_bytes_reach_the_framer",     test_bytes_reach_the_framer },
    { "uart_autobaud_mode1",             test_autobaud_mode1_maps_and_rewrites },
    { "uart_autobaud_mode2",             test_autobaud_mode2_maps_and_rewrites },
    { "uart_autobaud_good_sync",         test_autobaud_leaves_a_good_sync_alone },
    { "uart_autobaud_locks_after_sof",   test_autobaud_locks_after_sof },
    { "uart_autobaud_miss_count",
      test_autobaud_miss_count_survives_interrupts },
    { "uart_corrupt_bytes_discarded",    test_corrupt_bytes_are_discarded },
    { "uart_overrun_preserves_byte",     test_overrun_preserves_current_byte },
    { "uart_only_framing_rearms_auto",
      test_only_framing_rearms_automatic_bitrate },
    { "uart_fifo_summary_not_current",
      test_fifo_error_summary_is_not_current_byte_error },
    { "uart_poll_reads_the_line",        test_tick_reads_the_accessory_line },
    { "uart_detach_resets_the_session",  test_detach_resets_the_session },
    { "uart_second_detach_resets_again", test_second_detach_resets_again },
    { "uart_boot_detached_is_not_a_detach", test_boot_with_nothing_attached_is_not_a_detach },
};

int main(void)
{
    printf("iAP over the PP dock UART: the iPod Video's transport\n");

    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        current = cases[i].name;
        int before = failures;
        cases[i].fn();
        printf("  %s  %s\n", failures == before ? "ok  " : "FAIL", current);
    }

    printf("\n%zu cases run, %d checks, %d failure%s\n",
           sizeof(cases)/sizeof(cases[0]), checks, failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
