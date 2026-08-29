#include "iap_test.h"

#include <string.h>

#include "audio.h"
#include "cuesheet.h"
#include "iap-core.h"

static struct cuesheet test_cue;

static void set_chapters(unsigned long elapsed)
{
    struct mp3entry *id3 = rbstub_id3();

    memset(&test_cue, 0, sizeof(test_cue));
    test_cue.track_count = 3;
    test_cue.tracks[0].offset = 0;
    test_cue.tracks[1].offset = 60000;
    test_cue.tracks[2].offset = 150000;
    strcpy(test_cue.tracks[0].title, "Opening");
    strcpy(test_cue.tracks[1].title, "Middle");
    strcpy(test_cue.tracks[2].title, "Finale");
    id3->cuesheet = &test_cue;
    id3->length = 240000;
    id3->elapsed = elapsed;
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);
}

static const struct iaptest_pkt *find_l4_notification(unsigned char type)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);

        if (p && p->paylen >= 6 && p->payload[0] == 0x04
            && p->payload[1] == 0x00 && p->payload[2] == 0x27
            && p->payload[5] == type)
            return p;
    }

    return NULL;
}

static const struct iaptest_pkt *find_l3_notification(unsigned char type)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);

        if (p && p->paylen >= 5 && p->payload[0] == 0x03
            && p->payload[1] == 0x09 && p->payload[4] == type)
            return p;
    }

    return NULL;
}

void test_chapters_extended_queries(void)
{
    const struct iaptest_pkt *p;

    iaptest_session_extended();
    set_chapters(65000);

    IAPTEST_RX(0x04, 0x00, 0x02, 0x10, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x03, 0x10, 0x01,
                   0x00, 0x00, 0x00, 0x01,
                   0x00, 0x00, 0x00, 0x03);

    iaptest_tx_clear();
    rbstub_id3()->elapsed = 60000;
    IAPTEST_RX(0x04, 0x00, 0x02, 0x10, 0x02);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x03, 0x10, 0x02,
                   0x00, 0x00, 0x00, 0x01,
                   0x00, 0x00, 0x00, 0x03);

    iaptest_tx_clear();
    rbstub_id3()->elapsed = 65000;
    IAPTEST_RX(0x04, 0x00, 0x05, 0x10, 0x03,
               0x00, 0x00, 0x00, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x06, 0x10, 0x03,
                   0x00, 0x01, 0x5F, 0x90,
                   0x00, 0x00, 0x13, 0x88);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x07, 0x10, 0x04,
               0x00, 0x00, 0x00, 0x02);
    p = iaptest_tx(0);
    CHECK(p && p->paylen == 12, "chapter name reply length");
    if (p && p->paylen == 12) {
        CHECK(p->payload[0] == 0x04 && p->payload[1] == 0x00
              && p->payload[2] == 0x08,
              "chapter name reply command");
        CHECK(strcmp((const char *)&p->payload[5], "Finale") == 0,
              "chapter name reply value");
    }
}

void test_chapters_extended_set_and_navigation(void)
{
    iaptest_session_extended();
    set_chapters(65000);

    IAPTEST_RX(0x04, 0x00, 0x04, 0x11, 0x01,
               0x00, 0x00, 0x00, 0x02);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x11, 0x01,
                   IAP_ACK_OK, 0x00, 0x04);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 150000,
                 "SetCurrentPlayingTrackChapter target");

    rbstub_id3()->elapsed = 65000;
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x11, 0x02, 0x08);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 150000,
                 "PlayControl Next chapter target");
    CHECK_EQ_INT(rbstub_calls.skip, 0,
                 "PlayControl Next stays in the chaptered track");

    rbstub_id3()->elapsed = 63000;
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x11, 0x03, 0x09);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 60000,
                 "Previous restarts a chapter after two seconds");

    rbstub_id3()->elapsed = 61000;
    rbstub_reset_calls();
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x29, 0x11, 0x04, 0x09);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 0,
                 "Previous enters the prior chapter before two seconds");
}

void test_chapters_display_remote(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    rbstub_set_playlist(5, 2);
    set_chapters(65000);

    IAPTEST_RX(0x03, 0x0C, 0x12, 0x01, 0x02);
    EXPECT_PAYLOAD(0, 0x03, 0x0D, 0x12, 0x01, 0x02,
                   0x00, 0x00, 0x00, 0x02,
                   0x00, 0x03, 0x00, 0x01);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0C, 0x12, 0x02, 0x11);
    EXPECT_PAYLOAD(0, 0x03, 0x0D, 0x12, 0x02, 0x11,
                   0x00, 0x00, 0x00, 0x02);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x12, 0x12, 0x03, 0x00,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x00);
    EXPECT_PAYLOAD(0, 0x03, 0x13, 0x12, 0x03, 0x00,
                   0x00, 0x00, 0x00, 0x02,
                   0x00, 0x03, 0xA9, 0x80,
                   0x00, 0x03);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x12, 0x12, 0x04, 0x01,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x01);
    EXPECT_PAYLOAD(0, 0x03, 0x13, 0x12, 0x04, 0x01,
                   0x00, 0x00, 0xEA, 0x60,
                   'M', 'i', 'd', 'd', 'l', 'e', 0x00);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x12, 0x05, 0x02, 0x00, 0x02);
    EXPECT_PAYLOAD(0, 0x03, 0x00, 0x12, 0x05,
                   IAP_ACK_OK, 0x0E);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 150000,
                 "SetiPodStateInfo chapter target");
}

void test_chapters_simple_remote_buttons(void)
{
    iaptest_identify_legacy(BIT_N(0) | BIT_N(2));
    iaptest_force_authenticated();
    set_chapters(65000);

    IAPTEST_RX(0x02, 0x00, 0x00, 0x08);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 150000,
                 "Simple Remote Next Chapter target");

    rbstub_id3()->elapsed = 65000;
    IAPTEST_RX(0x02, 0x00, 0x00, 0x08);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 1,
                 "a held chapter button acts once");

    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iap_periodic();
    rbstub_id3()->elapsed = 61000;
    IAPTEST_RX(0x02, 0x00, 0x00, 0x10);
    CHECK_EQ_INT(rbstub_calls.last_ff_rewind, 0,
                 "Simple Remote Previous Chapter target");
}

void test_chapters_simple_remote_requires_multiple_chapters(void)
{
    iaptest_identify_legacy(BIT_N(0) | BIT_N(2));
    iaptest_force_authenticated();
    set_chapters(65000);
    test_cue.track_count = 1;

    IAPTEST_RX(0x02, 0x00, 0x00, 0x10);
    CHECK_EQ_INT(rbstub_calls.ff_rewind, 0,
                 "Previous Chapter changed a one-chapter track");
}

void test_chapters_notifications(void)
{
    const struct iaptest_pkt *p;

    iaptest_session_extended();
    rbstub_set_playlist(5, 2);
    set_chapters(65000);

    IAPTEST_RX(0x04, 0x00, 0x26, 0x13, 0x01,
               0x00, 0x00, 0x00, 0xE0);
    iaptest_tx_clear();
    rbstub_id3()->elapsed = 151500;
    for (int i = 0; i < 5; i++)
        iap_periodic();

    p = find_l4_notification(0x05);
    CHECK(p && p->paylen == 10, "Extended chapter-index notification");
    if (p && p->paylen == 10)
        CHECK_EQ_INT((p->payload[8] << 8) | p->payload[9], 2,
                     "Extended chapter-index value");

    p = find_l4_notification(0x08);
    CHECK(p && p->paylen == 10, "Extended chapter-ms notification");
    if (p && p->paylen == 10)
        CHECK_EQ_INT((p->payload[8] << 8) | p->payload[9], 1500,
                     "Extended chapter-ms value");

    p = find_l4_notification(0x09);
    CHECK(p && p->paylen == 10, "Extended chapter-seconds notification");
    if (p && p->paylen == 10)
        CHECK_EQ_INT(p->payload[9], 1, "Extended chapter-seconds value");

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x08, 0x13, 0x02, 0x00, 0x00, 0x00, 0x04);
    iaptest_tx_clear();
    rbstub_id3()->elapsed = 65000;
    for (int i = 0; i < 5; i++)
        iap_periodic();

    p = find_l3_notification(0x02);
    CHECK(p && p->paylen == 13, "Display Remote chapter notification");
    if (p && p->paylen == 13) {
        CHECK_EQ_INT((p->payload[9] << 8) | p->payload[10], 3,
                     "Display Remote chapter count");
        CHECK_EQ_INT((p->payload[11] << 8) | p->payload[12], 1,
                     "Display Remote chapter index");
    }
}

void test_chapters_absent_and_invalid_are_reported(void)
{
    iaptest_session_extended();
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    IAPTEST_RX(0x04, 0x00, 0x02, 0x14, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x03, 0x14, 0x01,
                   0xFF, 0xFF, 0xFF, 0xFF,
                   0x00, 0x00, 0x00, 0x00);

    set_chapters(65000);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x04, 0x14, 0x02,
               0x00, 0x00, 0x00, 0x03);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x14, 0x02,
                   IAP_ACK_BAD_PARAM, 0x00, 0x04);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x05, 0x14, 0x03,
               0x00, 0x00, 0x00, 0x00);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x14, 0x03,
                   IAP_ACK_BAD_PARAM, 0x00, 0x05);
}

void test_audiobook_speed_accepts_only_normal(void)
{
    iaptest_session_extended();

    IAPTEST_RX(0x04, 0x00, 0x09, 0x15, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x0A, 0x15, 0x01, 0x00);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0B, 0x15, 0x02, 0x00);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x15, 0x02,
                   IAP_ACK_OK, 0x00, 0x0B);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0B, 0x15, 0x03, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x15, 0x03,
                   IAP_ACK_CMD_FAILED, 0x00, 0x0B);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0B, 0x15, 0x04, 0x7F);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x01, 0x15, 0x04,
                   IAP_ACK_BAD_PARAM, 0x00, 0x0B);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x15, 0x05, 0x0E, 0x00, 0x01);
    EXPECT_PAYLOAD(0, 0x03, 0x00, 0x15, 0x05,
                   IAP_ACK_OK, 0x0E);

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x0E, 0x15, 0x06, 0x0E, 0xFF, 0x00);
    EXPECT_PAYLOAD(0, 0x03, 0x00, 0x15, 0x06,
                   IAP_ACK_CMD_FAILED, 0x0E);
}
