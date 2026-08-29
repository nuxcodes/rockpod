/***************************************************************************
 * Host tests for iAP album artwork transfer.
 ****************************************************************************/

#include "iap_test.h"
#include "iap-artwork.h"
#include "iap-core.h"

static uint16_t read_u16(const unsigned char *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static unsigned char expected_image_byte(size_t offset)
{
    uint16_t pixel = 0x1000 + (offset >> 1);

    return offset & 1 ? pixel >> 8 : pixel & 0xff;
}

static uint32_t artwork_event(void)
{
    uint32_t transfer_id;

    CHECK_EQ_INT(rbstub_calls.last_event, IAP_EV_ARTWORK,
                 "artwork transfer event");
    transfer_id = (uint32_t)rbstub_calls.last_event_data;
    rbstub_calls.last_event = 0;
    rbstub_calls.last_event_data = 0;
    return transfer_id;
}

static void drain_artwork(void)
{
    int packets = 0;

    while (rbstub_calls.last_event == IAP_EV_ARTWORK) {
        iap_artwork_send_next(artwork_event());
        if (++packets > 128) {
            CHECK(false, "artwork transfer did not finish");
            break;
        }
    }
}

static void check_format(const struct iaptest_pkt *p, int data_offset)
{
    CHECK(p != NULL, "no artwork format reply");
    if (!p || p->paylen < data_offset + 7)
        return;

    CHECK_EQ_INT(read_u16(&p->payload[data_offset]), IAP_ARTWORK_FORMAT_ID,
                 "artwork format ID");
    CHECK_EQ_INT(p->payload[data_offset + 2], IAP_ARTWORK_PIXEL_FORMAT,
                 "artwork pixel format");
    CHECK_EQ_INT(read_u16(&p->payload[data_offset + 3]), IAP_ARTWORK_WIDTH,
                 "artwork width");
    CHECK_EQ_INT(read_u16(&p->payload[data_offset + 5]), IAP_ARTWORK_HEIGHT,
                 "artwork height");
}

void test_artwork_formats_match_both_lingoes(void)
{
    const struct iaptest_pkt *p;

    iaptest_session_extended();

    IAPTEST_RX(0x03, 0x16, 0x21, 0x22);
    p = iaptest_tx(0);
    CHECK(p && p->paylen == 11,
          "Display Remote returned the wrong format list");
    if (p) {
        CHECK_EQ_INT(p->payload[2], 0x21, "Display Remote transaction high");
        CHECK_EQ_INT(p->payload[3], 0x22, "Display Remote transaction low");
        check_format(p, 4);
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0E, 0x23, 0x24);
    p = iaptest_tx(0);
    CHECK(p && p->paylen == 12,
          "Extended Interface returned the wrong format list");
    if (p) {
        CHECK_EQ_INT(p->payload[3], 0x23,
                     "Extended Interface transaction high");
        CHECK_EQ_INT(p->payload[4], 0x24, "Extended Interface transaction low");
        check_format(p, 5);
    }
}

void test_artwork_metadata_and_times_match_availability(void)
{
    const struct iaptest_pkt *p;

    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);

    IAPTEST_RX(0x03, 0x0C, 0x2A, 0x2B, 0x11);
    p = iaptest_tx(0);
    CHECK(p && p->paylen >= 9, "Display Remote capability reply length");
    if (p && p->paylen >= 9) {
        uint32_t caps = (uint32_t)p->payload[5] << 24
                      | (uint32_t)p->payload[6] << 16
                      | (uint32_t)p->payload[7] << 8
                      | p->payload[8];
        CHECK(caps & BIT_N(2), "Display Remote capability omits artwork");
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0C, 0x2C, 0x2D, 0x00,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x00);
    p = iaptest_tx(0);
    CHECK(p && p->paylen >= 10, "Extended Interface capability reply length");
    if (p && p->paylen >= 10) {
        uint32_t caps = (uint32_t)p->payload[6] << 24
                      | (uint32_t)p->payload[7] << 16
                      | (uint32_t)p->payload[8] << 8
                      | p->payload[9];
        CHECK(caps & BIT_N(2), "Extended Interface capability omits artwork");
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x12, 0x30, 0x31, 0x08,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x00);
    p = iaptest_tx(0);
    CHECK(p && p->paylen == 9, "Display Remote artwork count length");
    if (p && p->paylen >= 9) {
        CHECK_EQ_INT(read_u16(&p->payload[5]), IAP_ARTWORK_FORMAT_ID,
                     "Display Remote artwork count format");
        CHECK_EQ_INT(read_u16(&p->payload[7]), 1,
                     "Display Remote artwork count");
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x0C, 0x32, 0x33, 0x07,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x00);
    p = iaptest_tx(0);
    CHECK(p && p->paylen == 10, "Extended Interface artwork count length");
    if (p && p->paylen >= 10) {
        CHECK_EQ_INT(read_u16(&p->payload[6]), IAP_ARTWORK_FORMAT_ID,
                     "Extended Interface artwork count format");
        CHECK_EQ_INT(read_u16(&p->payload[8]), 1,
                     "Extended Interface artwork count");
    }

    iaptest_tx_clear();
    IAPTEST_RX(0x03, 0x1F, 0x34, 0x35,
               0x00, 0x00, 0x00, 0x02, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x01);
    EXPECT_PAYLOAD(0, 0x03, 0x20, 0x34, 0x35, 0x00, 0x00, 0x00, 0x00);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x2A, 0x36, 0x37,
               0x00, 0x00, 0x00, 0x02, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x01);
    EXPECT_PAYLOAD(0, 0x04, 0x00, 0x2B, 0x36, 0x37,
                   0x00, 0x00, 0x00, 0x00);

    iaptest_tx_clear();
    rbstub_set_artwork(false);
    IAPTEST_RX(0x03, 0x12, 0x38, 0x39, 0x08,
               0x00, 0x00, 0x00, 0x02, 0x00, 0x00);
    EXPECT_PAYLOAD(0, 0x03, 0x13, 0x38, 0x39, 0x08);
}

static void check_artwork_data(unsigned char lingo, unsigned char tid_hi,
                               unsigned char tid_lo, size_t payload_limit)
{
    size_t image_offset = 0;
    int count = iaptest_tx_count();

    CHECK(count > 1, "artwork was not split across packets");
    for (int i = 0; i < count; i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        int index_offset = lingo == 0x03 ? 4 : 5;
        int data_offset = i == 0
                        ? (lingo == 0x03 ? 23 : 24)
                        : (lingo == 0x03 ? 6 : 7);

        CHECK(p != NULL, "missing artwork packet %d", i);
        if (!p)
            continue;
        CHECK(p->paylen <= (int)payload_limit,
              "artwork packet %d exceeds payload limit", i);
        CHECK_EQ_INT(p->payload[0], lingo, "artwork response lingo");
        if (lingo == 0x03) {
            CHECK_EQ_INT(p->payload[1], 0x19, "Display Remote artwork command");
            CHECK_EQ_INT(p->payload[2], tid_hi,
                         "Display Remote transaction high");
            CHECK_EQ_INT(p->payload[3], tid_lo,
                         "Display Remote transaction low");
        } else {
            CHECK_EQ_INT(read_u16(&p->payload[1]), 0x0011,
                         "Extended Interface artwork command");
            CHECK_EQ_INT(p->payload[3], tid_hi,
                         "Extended Interface transaction high");
            CHECK_EQ_INT(p->payload[4], tid_lo,
                         "Extended Interface transaction low");
        }
        CHECK_EQ_INT(read_u16(&p->payload[index_offset]), i,
                     "artwork packet index");

        if (i == 0) {
            int d = index_offset + 2;
            CHECK_EQ_INT(p->payload[d], IAP_ARTWORK_PIXEL_FORMAT,
                         "artwork descriptor pixel format");
            CHECK_EQ_INT(read_u16(&p->payload[d + 1]), IAP_ARTWORK_WIDTH,
                         "artwork descriptor width");
            CHECK_EQ_INT(read_u16(&p->payload[d + 3]), IAP_ARTWORK_HEIGHT,
                         "artwork descriptor height");
            CHECK_EQ_INT(read_u16(&p->payload[d + 9]), IAP_ARTWORK_WIDTH - 1,
                         "artwork inset right");
            CHECK_EQ_INT(read_u16(&p->payload[d + 11]), IAP_ARTWORK_HEIGHT - 1,
                         "artwork inset bottom");
            CHECK_EQ_INT((uint32_t)p->payload[d + 13] << 24
                         | (uint32_t)p->payload[d + 14] << 16
                         | (uint32_t)p->payload[d + 15] << 8
                         | p->payload[d + 16], IAP_ARTWORK_WIDTH * 2,
                         "artwork row size");
        }

        for (int j = data_offset; j < p->paylen; j++, image_offset++)
            CHECK_EQ_INT(p->payload[j], expected_image_byte(image_offset),
                         "RGB565 little-endian image byte");
    }

    CHECK_EQ_INT(image_offset,
                 IAP_ARTWORK_WIDTH * IAP_ARTWORK_HEIGHT * 2,
                 "transferred artwork byte count");
}

void test_artwork_display_remote_data_is_multipart_rgb565le(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    device.acc_max_payload = 128;
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x03, 0x18, 0x40, 0x41,
               0x00, 0x00, 0x00, 0x02, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);

    CHECK_EQ_INT(rbstub_calls.artwork_decodes, 1,
                 "Display Remote artwork decode count");
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "artwork request sent data before its queue event");
    iap_artwork_send_next(artwork_event());
    CHECK_EQ_INT(iaptest_tx_count(), 1,
                 "artwork event sent more than one packet");
    drain_artwork();
    check_artwork_data(0x03, 0x40, 0x41, 128);
}

void test_artwork_extended_data_is_multipart_rgb565le(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x04, 0x00, 0x10, 0x42, 0x43,
               0x00, 0x00, 0x00, 0x01, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);

    CHECK_EQ_INT(rbstub_calls.artwork_decodes, 1,
                 "Extended Interface artwork decode count");
    drain_artwork();
    check_artwork_data(0x04, 0x42, 0x43, TX_BUFLEN);
}

void test_artwork_cancel_stops_the_matching_transfer(void)
{
    uint32_t transfer_id;
    int sent;

    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    device.acc_max_payload = 128;
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x03, 0x18, 0x40, 0x41,
               0x00, 0x00, 0x00, 0x02, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    transfer_id = artwork_event();
    iap_artwork_send_next(transfer_id);
    sent = iaptest_tx_count();

    IAPTEST_RX(0x00, 0x50, 0x60, 0x61,
               0x03, 0x00, 0x18, 0x40, 0x41);
    EXPECT_PAYLOAD(sent, 0x00, 0x02, 0x60, 0x61, 0x00, 0x50);
    sent = iaptest_tx_count();
    iap_artwork_send_next(transfer_id);
    CHECK_EQ_INT(iaptest_tx_count(), sent,
                 "cancelled artwork transfer sent another packet");

    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    device.acc_max_payload = 128;
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x04, 0x00, 0x10, 0x42, 0x43,
               0x00, 0x00, 0x00, 0x01, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    transfer_id = artwork_event();
    iap_artwork_send_next(transfer_id);
    sent = iaptest_tx_count();

    IAPTEST_RX(0x00, 0x50, 0x64, 0x65,
               0x04, 0x00, 0x10, 0x42, 0x43);
    EXPECT_PAYLOAD(sent, 0x00, 0x02, 0x64, 0x65, 0x00, 0x50);
    sent = iaptest_tx_count();
    iap_artwork_send_next(transfer_id);
    CHECK_EQ_INT(iaptest_tx_count(), sent,
                 "cancelled extended artwork sent another packet");
}

void test_artwork_cancel_rejects_the_wrong_transaction(void)
{
    uint32_t transfer_id;
    int sent;

    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    device.acc_max_payload = 128;
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x04, 0x00, 0x10, 0x42, 0x43,
               0x00, 0x00, 0x00, 0x01, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    transfer_id = artwork_event();

    IAPTEST_RX(0x00, 0x50, 0x62, 0x63,
               0x04, 0x00, 0x10, 0x42, 0x44);
    EXPECT_PAYLOAD(0, 0x00, 0x02, 0x62, 0x63, 0x02, 0x50);

    sent = iaptest_tx_count();
    iap_artwork_send_next(transfer_id);
    CHECK_EQ_INT(iaptest_tx_count(), sent + 1,
                 "wrong transaction cancelled artwork");
    drain_artwork();
}

void test_artwork_reset_invalidates_queued_transfer(void)
{
    uint32_t transfer_id;

    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x03, 0x18, 0x44, 0x45,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    transfer_id = artwork_event();
    iap_reset_device(&device);
    iap_artwork_send_next(transfer_id);
    CHECK_EQ_INT(iaptest_tx_count(), 0,
                 "reset session sent queued artwork");
}

void test_artwork_packets_keep_the_request_transaction_mode(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(4, 0);
    rbstub_set_artwork(true);
    device.acc_max_payload = 128;
    iaptest_detach_model_for_raw_probes();

    IAPTEST_RX(0x03, 0x18, 0x46, 0x47,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    device.auth.idps = false;
    device.auth.idps_started = false;
    drain_artwork();
    check_artwork_data(0x03, 0x46, 0x47, 128);
}

static void check_ack(unsigned int command, unsigned char status)
{
    const struct iaptest_pkt *p = iaptest_tx(0);

    CHECK(p != NULL, "missing artwork error acknowledgement");
    if (!p || p->paylen < 8)
        return;
    CHECK_EQ_INT(read_u16(&p->payload[1]), 0x0001, "ack command");
    CHECK_EQ_INT(p->payload[5], status, "ack status");
    CHECK_EQ_INT(read_u16(&p->payload[6]), command, "acknowledged command");
}

void test_artwork_invalid_requests_are_refused(void)
{
    iaptest_session_extended();
    rbstub_set_playlist(2, 0);
    rbstub_set_artwork(true);

    IAPTEST_RX(0x04, 0x00, 0x10, 0x50, 0x51,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x05,
               0x00, 0x00, 0x00, 0x00);
    check_ack(0x0010, IAP_ACK_BAD_PARAM);

    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x10, 0x52, 0x53,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x01);
    check_ack(0x0010, IAP_ACK_BAD_PARAM);

    iaptest_tx_clear();
    rbstub_fail_artwork_decode(true);
    IAPTEST_RX(0x04, 0x00, 0x10, 0x54, 0x55,
               0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
               0x00, 0x00, 0x00, 0x00);
    check_ack(0x0010, IAP_ACK_CMD_FAILED);
}
