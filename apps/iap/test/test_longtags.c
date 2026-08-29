/***************************************************************************
 * Long metadata.
 *
 * Track tags come off disk and are attacker-supplied in the sense that
 * matters here: a file with a 300-character title is ordinary, and the
 * accessory asks for it by name. iap_send_tx() calls panicf() when the
 * TX buffer overflows (apps/iap/iap-core.c), which halts the device, so
 * the failure mode for getting this wrong is a crash rather than a
 * truncated string.
 *
 * strlcpy() returns strlen(src), not the number of bytes copied, which
 * is the specific way this goes wrong: using the return value as a
 * packet length sends more than was written.
 *
 * TX_BUFLEN is 512 (firmware/export/iap.h).
 ****************************************************************************/

#include "iap_test.h"

#include <string.h>
#include <stdio.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"
#include "cuesheet.h"
#include "metadata.h"

/* Longer than TX_BUFLEN, so nothing can quietly succeed by fitting. */
static char huge[1200];

static void set_huge_tags(void)
{
    struct mp3entry *id3 = rbstub_id3();

    memset(huge, 'W', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    id3->title  = huge;
    id3->artist = huge;
    id3->album  = huge;
    id3->genre_string    = huge;
    id3->composer        = huge;
    id3->albumartist     = huge;
}

/* The tightest bound that is still correct.
 *
 * These used to be built in a local data[70] with the string clamped to
 * 63 characters, which bounded the reply at 70 bytes -- and truncated
 * every longer tag. MFi 5.1.30 (p.422) forbids that: "The track title
 * string is not limited to 252 characters; it may be sent in small or
 * large packet format, depending on the string length." The same note
 * appears under 5.1.32 and 5.1.34 for artist and album.
 *
 * They are built in the TX buffer now, so the only bound is TX_BUFLEN,
 * which iap_tx_strlcpy() enforces itself. The truncation the old bound
 * described is checked for separately, by
 * test_long_tags_are_not_truncated below. */
#define IAPTEST_MAX_TAG_REPLY TX_BUFLEN

/* Display Remote builds its replies straight into the TX buffer with
 * IAP_TX_PUT_STRLCPY, which truncates against TX_BUFLEN rather than a
 * local array, so those legitimately run longer. */
#define IAPTEST_MAX_L3_REPLY  TX_BUFLEN

static void check_bounded_to(const char *what, int max);
static void check_bounded(const char *what)
{ check_bounded_to(what, IAPTEST_MAX_L3_REPLY); }

/* Every reply must fit the buffer and be internally consistent. */
static void check_bounded_to(const char *what, int max)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);

        CHECK(p->rawlen <= IAPTEST_MAX_TXLEN,
              "%s produced a %d byte frame", what, p->rawlen);
        CHECK(p->checksum_ok, "%s produced a bad checksum", what);
        CHECK(p->length_form_ok,
              "%s used an illegal length form for a %d byte payload",
              what, p->paylen);
        /* The payload has to be inside TX_BUFLEN or iap_send_tx()
         * would have panicked on the way out. */
        CHECK(p->paylen <= TX_BUFLEN,
              "%s payload is %d bytes, past TX_BUFLEN (%d)",
              what, p->paylen, TX_BUFLEN);

        CHECK(p->paylen <= max,
              "%s payload is %d bytes, past the %d this reply can hold",
              what, p->paylen, max);
    }
    iaptest_tx_clear();
}

/* ------------------------------------------------------------------ */

void test_long_tags_extended_interface(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x70);
    iaptest_tx_clear();

    set_huge_tags();
    rbstub_set_playlist(4, 0);

    /* GetIndexedPlayingTrackTitle / ArtistName / AlbumName. Each takes a
     * 4-byte track index. */
    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x71, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("GetIndexedPlayingTrackTitle", IAPTEST_MAX_TAG_REPLY);

    IAPTEST_RX(0x04, 0x00, 0x22, 0x00, 0x72, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("GetIndexedPlayingTrackArtistName", IAPTEST_MAX_TAG_REPLY);

    IAPTEST_RX(0x04, 0x00, 0x24, 0x00, 0x73, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("GetIndexedPlayingTrackAlbumName", IAPTEST_MAX_TAG_REPLY);

    /* GetIndexedPlayingTrackInfo, which packs several tags at once. */
    IAPTEST_RX(0x04, 0x00, 0x0C, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("GetIndexedPlayingTrackInfo", IAPTEST_MAX_TAG_REPLY);
}

void test_long_tags_display_remote(void)
{
    static struct cuesheet cue;

    iaptest_enter_idps();
    iaptest_force_authenticated();

    set_huge_tags();
    rbstub_set_playlist(4, 0);
    memset(&cue, 0, sizeof(cue));
    cue.track_count = 1;
    memset(cue.tracks[0].title, 'C', sizeof(cue.tracks[0].title) - 1);
    rbstub_id3()->cuesheet = &cue;

    /* GetIndexedPlayingTrackInfo, every info type the handler knows.
     * The payload must satisfy CHECKLEN(0x09 + doff): lingo, command,
     * two transaction bytes, the info type, a 4-byte track index and a
     * 2-byte chapter index. A 7-byte version failed the length check and
     * every one of these replies was a Bad Parameter ack, so the case
     * exercised no tag handling at all while contributing 52 checks. */
    /* MFi Table 5-15 defines track information types 0x00 to 0x07
     * and the handler implements through 0x08; anything above that is
     * correctly rejected, so sweeping further would only assert on
     * rejections. */
    for (unsigned char type = 0x00; type <= 0x08; type++) {
        char what[64];
        unsigned char p[11] = { 0x03, 0x12, 0x00, 0x80,
                                type,
                                0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00 };
        snprintf(what, sizeof(what),
                 "GetIndexedPlayingTrackInfo type 0x%02X", type);
        iaptest_rx(p, sizeof(p));

        /* A Bad Parameter ack means the request never reached the tag
         * code, so the case would be asserting nothing. */
        const struct iaptest_pkt *r = iaptest_tx(0);
        if (r && r->paylen >= 2 && r->payload[0] == 0x03
            && r->payload[1] == 0x00)
            CHECK(false, "%s was rejected, so no tag handling ran", what);

        check_bounded(what);
    }
}

void test_long_tags_legacy_accessory(void)
{
    iaptest_identify_legacy(0x0000001D);
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05);
    iaptest_tx_clear();

    set_huge_tags();
    rbstub_set_playlist(4, 0);

    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("legacy GetIndexedPlayingTrackTitle", IAPTEST_MAX_TAG_REPLY);

    IAPTEST_RX(0x04, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00);
    check_bounded_to("legacy GetIndexedPlayingTrackArtistName", IAPTEST_MAX_TAG_REPLY);
}

/* A tag exactly at the boundary the clamp guards. */
void test_tags_at_the_clamp_boundary(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x90);
    iaptest_tx_clear();

    struct mp3entry *id3 = rbstub_id3();
    rbstub_set_playlist(4, 0);

    for (int n = 60; n <= 70; n++) {
        char buf[80], what[64];
        memset(buf, 'X', n);
        buf[n] = '\0';
        id3->title = buf;

        snprintf(what, sizeof(what), "title of %d characters", n);
        IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x91, 0x00, 0x00, 0x00, 0x00);
        check_bounded_to(what, IAPTEST_MAX_TAG_REPLY);
    }
    id3->title = (char *)"Test Title";
}

/* ------------------------------------------------------------------ */
/* Missing tags                                                        */
/* ------------------------------------------------------------------ */

/* An untagged file is ordinary. Rockbox leaves the id3 string pointers
 * NULL for a missing tag, and get_metadata_ex() wipes the whole entry
 * and returns false on any parse or open failure -- a return
 * iap_get_trackinfo() does not check. Five Display Remote sites passed
 * those pointers straight to strlcpy(), so one well-formed
 * GetIndexedPlayingTrackInfo halted the player.
 *
 * The harness makes panicf() and a segfault both fatal, so a regression
 * here kills the run rather than reporting. */
void test_missing_tags_do_not_crash(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(4, 0);

    struct mp3entry *id3 = rbstub_id3();
    id3->title       = NULL;
    id3->artist      = NULL;
    id3->album       = NULL;
    id3->genre_string = NULL;
    id3->composer    = NULL;
    id3->albumartist = NULL;
    id3->comment     = NULL;

    /* Every Display Remote info type, which is where the unguarded
     * sites are. Type 0x04 is the genre that crashed. */
    for (unsigned char type = 0x00; type <= 0x08; type++) {
        char what[64];
        unsigned char p[11] = { 0x03, 0x12, 0x00, 0x40,
                                type,
                                0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00 };
        snprintf(what, sizeof(what),
                 "untagged file, Display Remote info type 0x%02X", type);
        iaptest_tx_clear();
        iaptest_rx(p, sizeof(p));
        check_bounded_to(what, IAPTEST_MAX_L3_REPLY);
    }

    /* And the Extended Interface title/artist/album commands. */
    IAPTEST_RX(0x00, 0x05, 0x00, 0x41);
    iaptest_tx_clear();
    for (unsigned char cmd = 0x20; cmd <= 0x24; cmd += 2) {
        char what[64];
        unsigned char p[9] = { 0x04, 0x00, cmd, 0x00, 0x42,
                               0x00, 0x00, 0x00, 0x00 };
        snprintf(what, sizeof(what),
                 "untagged file, Extended Interface 0x%02X", cmd);
        iaptest_tx_clear();
        iaptest_rx(p, sizeof(p));
        check_bounded_to(what, IAPTEST_MAX_TAG_REPLY);
    }

    CHECK(rbstub_calls.panics == 0, "the firmware panicked on an untagged file");

    id3->title  = (char *)"Test Title";
    id3->artist = (char *)"Test Artist";
    id3->album  = (char *)"Test Album";
}

/* MFi 5.1.30 (p.422): "The track title string is not limited to 252
 * characters; it may be sent in small or large packet format, depending
 * on the string length." 5.1.32 (p.423) and 5.1.34 (p.424) say the same
 * for artist and album.
 *
 * The Extended Interface replies clamped every tag to 63 characters, so
 * a car head unit showed the first 63 and nothing else -- while the
 * Display Remote lingo, answering about the same track, sent the whole
 * string. The bound above only checked the reply was not too big; it
 * could not see that it was too small. */
void test_long_tags_are_not_truncated(void)
{
    static char title[300], artist[300], album[300];
    int i;

    /* Three different long strings, so a handler that answers with the
     * wrong one is visible. */
    for (i = 0; i < 260; i++) {
        title[i]  = (char)('a' + (i % 26));
        artist[i] = (char)('A' + (i % 26));
        album[i]  = (char)('0' + (i % 10));
    }
    title[260] = artist[260] = album[260] = '\0';

    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x60);
    rbstub_set_playlist(4, 0);
    rbstub_id3()->title  = title;
    rbstub_id3()->artist = artist;
    rbstub_id3()->album  = album;

    static const struct {
        unsigned short cmd, reply; const char *what; char *want;
    } tc[] = {
        { 0x0020, 0x0021, "GetIndexedPlayingTrackTitle",      title  },
        { 0x0022, 0x0023, "GetIndexedPlayingTrackArtistName", artist },
        { 0x0024, 0x0025, "GetIndexedPlayingTrackAlbumName",  album  },
    };

    for (unsigned k = 0; k < sizeof(tc)/sizeof(tc[0]); k++) {
        iaptest_tx_clear();
        unsigned char p[9] = { 0x04,
                               (unsigned char)(tc[k].cmd >> 8),
                               (unsigned char)tc[k].cmd,
                               0x00, (unsigned char)(0x61 + k),
                               0x00, 0x00, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s got no reply", tc[k].what);
        if (!r || r->paylen < 6)
            continue;

        CHECK_EQ_INT((r->payload[1] << 8) | r->payload[2], tc[k].reply,
                     "the reply command");
        /* lingo, command(2), transaction id(2), then the string. */
        CHECK_EQ_INT(r->paylen, 5 + 260 + 1,
                     "the whole tag plus its NUL should be on the wire");
        CHECK(memcmp(r->payload + 5, tc[k].want, 260) == 0,
              "%s came back altered, truncated, or is the wrong tag",
              tc[k].what);
    }
}

/* MFi p.150 bounds the complete payload and requires string truncation. */
void test_long_tags_honour_accessory_payload_limit(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x70);
    iaptest_tx_clear();

    set_huge_tags();
    rbstub_set_playlist(4, 0);
    device.acc_max_payload = 128;

    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x71,
               0x00, 0x00, 0x00, 0x00);
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no limited Extended Interface metadata reply");
    if (r) {
        CHECK_EQ_INT(r->paylen, 128,
                     "the accessory's 128-byte payload limit");
        CHECK(r->payload[127] == '\0',
              "the limited Extended Interface string is not terminated");
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x12, 0x00, 0x72, 0x03,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    r = iaptest_tx(0);
    CHECK(r != NULL, "no limited Display Remote metadata reply");
    if (r) {
        CHECK_EQ_INT(r->paylen, 128,
                     "the Display Remote payload exceeded the declared limit");
        CHECK(r->payload[127] == '\0',
              "the limited Display Remote string is not terminated");
    }
}

void test_long_tags_honour_idps_payload_limit(void)
{
    iaptest_init();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x02,
               0x10, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x03, 0x04, 0x0A,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00,
               0x05, 0x00, 0x02, 0x09, 0x00, 0x80);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "the IDPS maximum-payload token was not stored");
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "EndIDPS discarded the maximum-payload token");
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x70);
    iaptest_tx_clear();

    set_huge_tags();
    rbstub_set_playlist(4, 0);
    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x71,
               0x00, 0x00, 0x00, 0x00);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "no metadata reply after IDPS");
    if (r) {
        CHECK_EQ_INT(r->paylen, 128,
                     "the IDPS maximum-payload token was not enforced");
        CHECK(r->payload[127] == '\0',
              "the IDPS-limited metadata string was not terminated");
    }
}

static void send_idps_payload_token(unsigned char tid, unsigned char len,
                                    uint16_t value)
{
    unsigned char p[] = { 0x00, 0x39, 0x00, tid, 0x01,
                          len, 0x00, 0x02, 0x09,
                          value >> 8, value, 0x00 };

    iaptest_rx(p, 6 + len);
}

static void expect_idps_payload_ack(unsigned char tid, unsigned char status)
{
    const struct iaptest_pkt *p = iaptest_tx(0);

    CHECK(p && p->paylen == 10,
          "the maximum-payload token acknowledgement length");
    if (p && p->paylen == 10)
        CHECK(p->payload[0] == 0x00 && p->payload[1] == 0x3A
              && p->payload[2] == 0x00 && p->payload[3] == tid
              && p->payload[4] == 0x01 && p->payload[5] == 0x04
              && p->payload[6] == 0x00 && p->payload[7] == 0x02
              && p->payload[8] == status && p->payload[9] == 0x09,
              "the maximum-payload token acknowledgement fields");
}

void test_long_tags_idps_payload_token_validation(void)
{
    iaptest_init();
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);

    iaptest_tx_clear();
    send_idps_payload_token(0x02, 5, 128);
    expect_idps_payload_ack(0x02, 0);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "the minimum maximum-payload token value");

    iaptest_tx_clear();
    send_idps_payload_token(0x03, 5, 127);
    expect_idps_payload_ack(0x03, 2);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "an invalid low payload value replaced the valid value");

    iaptest_tx_clear();
    send_idps_payload_token(0x04, 5, 0xFFFB);
    expect_idps_payload_ack(0x04, 2);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "an invalid high payload value replaced the valid value");

    iaptest_tx_clear();
    send_idps_payload_token(0x05, 4, 128);
    expect_idps_payload_ack(0x05, 2);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "a short payload token replaced the valid value");

    iaptest_tx_clear();
    send_idps_payload_token(0x06, 6, 128);
    expect_idps_payload_ack(0x06, 2);
    CHECK_EQ_INT(device.acc_max_payload, 128,
                 "a long payload token replaced the valid value");

    iaptest_tx_clear();
    send_idps_payload_token(0x07, 5, 256);
    expect_idps_payload_ack(0x07, 0);
    CHECK_EQ_INT(device.acc_max_payload, 256,
                 "the last valid payload token did not replace the first");

    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x08, 0x01);
    CHECK_EQ_INT(device.acc_max_payload, 0,
                 "EndIDPS Reset retained the abandoned payload limit");
}

void test_long_tags_payload_limit_preserves_utf8(void)
{
    static char title[125];

    iaptest_enter_idps();
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05, 0x00, 0x70);
    iaptest_tx_clear();

    memset(title, 'A', 121);
    title[121] = (char)0xE2;
    title[122] = (char)0x82;
    title[123] = (char)0xAC;
    title[124] = '\0';
    rbstub_set_playlist(4, 0);
    rbstub_id3()->title = title;
    device.acc_max_payload = 128;

    IAPTEST_RX(0x04, 0x00, 0x20, 0x00, 0x71,
               0x00, 0x00, 0x00, 0x00);
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r && r->paylen == 127,
          "the UTF-8 payload did not stop before the partial character");
    if (r && r->paylen == 127) {
        CHECK_EQ_INT(r->payload[125], 'A',
                     "the last complete UTF-8 prefix byte");
        CHECK_EQ_INT(r->payload[126], 0,
                     "the UTF-8 prefix terminator");
    }
}
