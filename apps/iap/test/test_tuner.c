/***************************************************************************
 * RF Tuner lingo (0x07), ipodvideo only.
 *
 * apps/iap/iap-lingo7.c is guarded by CONFIG_TUNER, which is defined for
 * ipodvideo and commented out for the Classic, so none of this compiles
 * on the reference target. These cases only exist in the ipodvideo build
 * of the harness -- see TARGET in the Makefile.
 *
 * The accessory here is the Apple Radio Remote, the one piece of hardware
 * this lingo was written for. It is a legacy accessory: it identifies
 * with IdentifyDeviceLingoes and never runs IDPS.
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include "config.h"
#include "sound.h"
#include "settings.h"
#include "iap-core.h"
#include "tuner.h"
#include "ipod_remote_tuner.h"

/* File-local in the driver; the test build compiles it with -Dstatic=
 * so a case can see what a payload actually decoded to. */
extern int tuner_frequency;

/* Bring the device up as a legacy accessory that has declared the RF
 * Tuner lingo. iap_handlepkt_mode7() refuses everything unless
 * DEVICE_LINGO_SUPPORTED(0x07) holds, so without this every case below
 * would assert against a Bad Parameter ack and prove nothing. */
static void tuner_bringup(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x07));
    iaptest_force_authenticated();
    iaptest_tx_clear();
}

/* Find the first captured packet with this lingo and command. */
static const struct iaptest_pkt *find_tx(unsigned char lingo,
                                         unsigned char cmd)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == lingo
            && p->payload[1] == cmd)
            return p;
    }
    return NULL;
}

/* Send RetTunerCaps (0x07/0x02) with the given 32-bit capability word.
 * The packet is lingo, command, four capability bytes big-endian, then
 * two reserved bytes: iap-lingo7.c reads inbuffer[2] as bits 31:24
 * through inbuffer[5] as bits 7:0. */
static void send_ret_tuner_caps(uint32_t caps)
{
    unsigned char p[8] = {
        0x07, 0x02,
        (unsigned char)(caps >> 24), (unsigned char)(caps >> 16),
        (unsigned char)(caps >> 8),  (unsigned char)caps,
        0x00, 0x00
    };
    iaptest_rx(p, sizeof(p));
}

/* Capability bits, from the comment block at iap-lingo7.c:180 and the
 * branches that read them. All four live in the low byte. */
#define CAP_AM        (1u << 0)
#define CAP_FM_EU_US  (1u << 1)
#define CAP_FM_JAPAN  (1u << 2)
#define CAP_FM_WIDE   (1u << 3)

/* MFi Table 4-126 (p.297), verbatim:
 *   0x00  AM band worldwide (520-1710 KHz)
 *   0x01  Europe/US FM band (87.5-108.0 MHz)
 *   0x02  Japan FM band (76.0-90.0 MHz)
 *   0x03  FM wide band (76.0-108.0 MHz)
 * SetTunerBand (0x07/0x08) carries one of these IDs.
 *
 * Every branch used to echo the capability bit that selected it, so
 * Europe/US asked for Japan, Japan and wide asked for values the table
 * does not define, and AM asked for Europe/US.
 */
static void check_band_for_caps(uint32_t caps, unsigned char want_band,
                                const char *what)
{
    iaptest_tx_clear();
    send_ret_tuner_caps(caps);

    const struct iaptest_pkt *p = find_tx(0x07, 0x08);
    if (!p) {
        CHECK(false, "%s: no SetTunerBand sent for caps 0x%08X",
              what, caps);
        return;
    }
    CHECK_EQ_INT(p->paylen, 3, "SetTunerBand payload length");
    if (p->paylen >= 3) {
        CHECK_EQ_INT(p->payload[2], want_band, what);
        CHECK(p->payload[2] <= 0x03,
              "%s: band 0x%02X is not one of the four IDs in MFi "
              "Table 4-126", what, p->payload[2]);
    }
}

void test_tuner_band_ids_match_the_spec_table(void)
{
    tuner_bringup();

    check_band_for_caps(CAP_FM_EU_US, 0x01, "Europe/US FM band ID");
    check_band_for_caps(CAP_FM_WIDE,  0x03, "FM wide band ID");
    check_band_for_caps(CAP_FM_JAPAN, 0x02, "Japan FM band ID");
    check_band_for_caps(CAP_AM,       0x00, "AM band ID");
}

/* The branches are an if/else chain, so a remote advertising several
 * bands gets the first one in source order. Check the precedence is
 * stable and still yields a defined ID rather than an OR of the bits. */
void test_tuner_band_precedence_when_several_advertised(void)
{
    tuner_bringup();

    check_band_for_caps(CAP_AM | CAP_FM_EU_US | CAP_FM_JAPAN | CAP_FM_WIDE,
                        0x01, "band ID when the remote advertises all four");
    check_band_for_caps(CAP_AM | CAP_FM_JAPAN,
                        0x02, "band ID for AM plus Japan FM");
}

/* A remote advertising no band at all must not be told to tune one. */
void test_tuner_no_band_advertised_sends_no_band(void)
{
    tuner_bringup();

    iaptest_tx_clear();
    send_ret_tuner_caps(0);

    CHECK(find_tx(0x07, 0x08) == NULL,
          "SetTunerBand sent although the remote advertised no band");
}

/* MFi Table 4-134 gives SetTunerMode a 3-byte payload. rmt_tuner_sleep()
 * built a 3-byte data4[] and then passed sizeof(data3), which is 6, so
 * three bytes of whatever followed data4 on the stack went out on the
 * wire behind a length field that claimed them. The packet's length
 * disagreed with the command it named, and its tail was uninitialised.
 *
 * radio_present gates the branch: rmt_tuner_sleep() only sends the
 * wake-up sequence once the tuner has been detected. */
void test_tuner_setmode_payload_is_three_bytes(void)
{
    tuner_bringup();

    radio_present = 1;
    iaptest_tx_clear();

    ipod_rmt_tuner_set(RADIO_SLEEP, 0);

    const struct iaptest_pkt *p = find_tx(0x07, 0x0E);
    CHECK(p != NULL, "no SetTunerMode sent on wake-up");
    if (!p)
        return;

    CHECK_EQ_INT(p->paylen, 3,
                 "SetTunerMode payload length (MFi Table 4-134)");
    CHECK(p->checksum_ok, "SetTunerMode checksum");

    /* The three bytes are the lingo, the command and the mode. Anything
     * longer is the over-read: the extra bytes are stack residue, so
     * assert on the length rather than on a value that varies. */
    if (p->paylen >= 3) {
        CHECK_EQ_INT(p->payload[0], 0x07, "SetTunerMode lingo");
        CHECK_EQ_INT(p->payload[1], 0x0E, "SetTunerMode command");
    }
}

/* Every packet the tuner driver emits on wake-up has to be
 * self-consistent, not just the one with the known bug. */
void test_tuner_wakeup_packets_are_all_wellformed(void)
{
    tuner_bringup();

    radio_present = 1;
    iaptest_tx_clear();

    ipod_rmt_tuner_set(RADIO_SLEEP, 0);

    int n = iaptest_tx_count();
    CHECK(n > 0, "wake-up sent nothing");

    for (int i = 0; i < n; i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        CHECK(p->checksum_ok, "wake-up packet %d has a bad checksum", i);
        CHECK(p->length_form_ok,
              "wake-up packet %d uses an out-of-range length form", i);
        CHECK(p->paylen >= 2,
              "wake-up packet %d is shorter than a lingo and command", i);
    }

    /* Well-formed is not the same as present. This case checked that
     * every packet sent was sound and never that any particular one was
     * sent, so deleting a command from the sequence went unnoticed --
     * which is how the RDS enable came to be dropped in a2a0843187 and
     * restored in 8be41a454c.
     *
     * The sequence, in order: tuner power on, RDS on, gain boost, tuner
     * mode, volume. */
    static const struct {
        unsigned char lingo, cmd; const char *what;
    } expected[] = {
        { 0x07, 0x05, "tuner power on" },
        { 0x07, 0x20, "RDS on (SetRdsNotifyMask)" },
        { 0x07, 0x24, "gain boost" },
        { 0x07, 0x08, "SetTunerBand" },
        { 0x07, 0x0E, "SetTunerMode" },
        { 0x03, 0x09, "the volume notification" },
    };
    for (unsigned e = 0; e < sizeof(expected)/sizeof(expected[0]); e++) {
        CHECK(find_tx(expected[e].lingo, expected[e].cmd) != NULL,
              "the wake-up sequence is missing %s (%02X/%02X)",
              expected[e].what, expected[e].lingo, expected[e].cmd);
    }
    CHECK_EQ_INT(n, 6, "the wake-up sequence should be six packets");

    /* And SetTunerMode has to carry the user's settings, not the
     * accessory's defaults. Table 4-135 (p.301): bits 1:0 are the
     * channel resolution, bit 4 forces mono, bit 6 selects 50 us
     * deemphasis. It used to send a literal 0x00 -- 200 kHz, stereo
     * allowed, 75 us -- while invalidating the driver's shadows in the
     * same breath, and apps/radio/radio.c:222-228 re-applies region and
     * force-mono only when the radio was fully off. So resuming from
     * pause left every non-US region on the wrong grid and the wrong
     * deemphasis, with Force Mono silently off. */
    global_settings.fm_region = REGION_EUROPE;      /* 100 kHz, 50 us */
    global_settings.fm_force_mono = true;
    iaptest_tx_clear();
    tuner_set(RADIO_SLEEP, 0);

    const struct iaptest_pkt *m = find_tx(0x07, 0x0E);
    CHECK(m != NULL, "no SetTunerMode in the wake-up sequence");
    if (m && m->paylen >= 3) {
        CHECK_EQ_INT(m->payload[2] & 0x03, 0x01,
                     "SetTunerMode did not carry Europe's 100 kHz "
                     "channel resolution");
        CHECK_EQ_INT(m->payload[2] & 0x40, 0x40,
                     "SetTunerMode did not carry Europe's 50 us "
                     "deemphasis");
        CHECK_EQ_INT(m->payload[2] & 0x10, 0x10,
                     "SetTunerMode did not carry the user's Force Mono "
                     "setting");
    }
}

/* The tuner driver holds the sixth volume conversion site. It computed
 * (volume + 58) * 4, an open-coded scale for a -58..+6 dB range: on the
 * iPod Video's WM8758 the real range is -90..+6, so the expression
 * underflowed below -58 dB and, at maximum volume, produced
 * (6 + 58) * 4 = 256, which truncates to 0 in a byte -- silence at full
 * volume. */
void test_tuner_volume_uses_the_shared_scale(void)
{
    /* A volume limit below the maximum, so the UI and absolute scales
     * actually differ. rb_stubs.c defaults volume_limit to sound_max,
     * where iap_volume_to_ui_byte() and iap_volume_to_byte() return the
     * same byte for every input -- with that default the case cannot
     * tell which one the driver used, and the wrong one survived. */
    global_settings.volume_limit = (sound_min(SOUND_VOLUME)
                                    + sound_max(SOUND_VOLUME)) / 2;

    tuner_bringup();

    radio_present = 1;

    /* Maximum volume must be maximum on the wire, not zero. */
    rbstub_set_volume(sound_max(SOUND_VOLUME));
    iaptest_tx_clear();
    ipod_rmt_tuner_set(RADIO_SLEEP, 0);

    const struct iaptest_pkt *p = find_tx(0x03, 0x09);
    CHECK(p != NULL, "no volume packet in the wake-up sequence");
    if (p && p->paylen >= 5) {
        CHECK_EQ_INT(p->payload[4], 255,
                     "tuner volume byte at maximum volume");
    }

    /* And minimum volume must be zero, not a wrapped value. */
    rbstub_set_volume(sound_min(SOUND_VOLUME));
    iaptest_tx_clear();
    ipod_rmt_tuner_set(RADIO_SLEEP, 0);

    p = find_tx(0x03, 0x09);
    if (p && p->paylen >= 5) {
        CHECK_EQ_INT(p->payload[4], 0,
                     "tuner volume byte at minimum volume");
    }

    /* The whole range has to be monotonic, which the old expression was
     * not once it wrapped. */
    int prev = -1;
    for (int v = sound_min(SOUND_VOLUME); v <= sound_max(SOUND_VOLUME); v++) {
        rbstub_set_volume(v);
        iaptest_tx_clear();
        ipod_rmt_tuner_set(RADIO_SLEEP, 0);
        p = find_tx(0x03, 0x09);
        if (!p || p->paylen < 5)
            continue;
        int got = p->payload[4];
        if (got < prev) {
            CHECK(false, "tuner volume is not monotonic: %d dB gave %d "
                  "after %d", v, got, prev);
            break;
        }
        prev = got;
    }
    CHECK(prev == 255, "the volume sweep never reached 255, ended at %d",
          prev);
}

/* An accessory that never declared lingo 0x07 must not reach the tuner
 * code at all. */
void test_tuner_rejects_undeclared_lingo(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    iaptest_tx_clear();

    send_ret_tuner_caps(CAP_FM_EU_US);

    CHECK(find_tx(0x07, 0x08) == NULL,
          "SetTunerBand sent although lingo 0x07 was never declared");
    CHECK(find_tx(0x07, 0x0E) == NULL,
          "SetTunerMode sent although lingo 0x07 was never declared");
}

/* iap_handlepkt_mode7() reads inbuffer[4] and inbuffer[5] in the
 * RetTunerCaps branch, but the only length check in the whole file is
 * CHECKLEN(2) at the top. A truncated RetTunerCaps therefore reads past
 * the packet, and whatever the buffer happens to hold decides which
 * band the remote is told to tune.
 *
 * The harness buffer is not poisoned, so this cannot prove the read is
 * out of bounds; what it can prove is the observable consequence, which
 * is that a packet too short to carry capabilities still produces a
 * SetTunerBand. */
void test_tuner_truncated_caps_is_rejected(void)
{
    tuner_bringup();
    iaptest_detach_model_for_raw_probes();

    /* RetTunerCaps with only two of its four capability bytes. */
    iaptest_tx_clear();
    IAPTEST_RX(0x07, 0x02, 0x00, 0x00);

    CHECK(find_tx(0x07, 0x08) == NULL,
          "a truncated RetTunerCaps produced a SetTunerBand, so the "
          "band came from bytes past the end of the packet");
    CHECK(find_tx(0x07, 0x05) == NULL,
          "a truncated RetTunerCaps produced a tuner power command");

    /* And the shortest packet that reaches the dispatcher at all. */
    iaptest_tx_clear();
    IAPTEST_RX(0x07, 0x02);

    CHECK(find_tx(0x07, 0x08) == NULL,
          "a two-byte RetTunerCaps produced a SetTunerBand");
}

/* MFi 2.6.1.2 (p.111) has the accessory enable transaction IDs before it
 * sends StartIDPS and keep them enabled "for all iAP commands", so an
 * accessory that negotiates lingo 0x07 through IDPS puts two ID bytes
 * between the command and its payload. iap-lingo7.c had no doff -- alone
 * among the lingoes -- so it read the ID itself as capability bits.
 *
 * The reply side is the other rule, 2.6.1.1 (p.111): the device echoes an
 * accessory's ID only when responding to it, and uses its own counter for
 * commands it initiates. Every transmission in this file is a Dev-to-Acc
 * command (Table 4-111, p.288, marks SetTunerBand "Dev to Acc"), and none
 * of them is the response to RetTunerCaps -- RetTunerCaps is itself the
 * accessory's response to GetTunerCaps. So they carry the device's own
 * counter, not an echo.
 *
 * The IdentifyToken in iaptest_enter_idps() does not declare 0x07, so
 * declare it here on top of the negotiated set. */
void test_tuner_honours_transaction_ids(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.lingoes |= (1u << 0x07);
    iaptest_tx_clear();

    /* RetTunerCaps with a transaction ID of 0x0201, chosen so that a
     * handler ignoring the ID reads 0x02 as the top capability byte --
     * whose low bits are the RDS and RSSI flags -- and puts the real
     * capability word two bytes late. */
    IAPTEST_RX(0x07, 0x02, 0x02, 0x01,
               0x00, 0x00, 0x00, CAP_FM_EU_US,
               0x00, 0x00);

    const struct iaptest_pkt *p = find_tx(0x07, 0x08);
    CHECK(p != NULL,
          "no SetTunerBand for a RetTunerCaps carrying a transaction ID");
    if (!p)
        return;

    if (p->paylen < 5) {
        CHECK(false,
              "SetTunerBand is %d bytes, too short to carry a transaction "
              "ID: lingo 7 emits none", p->paylen);
        return;
    }
    CHECK_EQ_INT(p->paylen, 5, "SetTunerBand length with a transaction ID");
    CHECK_EQ_INT(p->payload[4], 0x01, "Europe/US FM band ID with IDPS");

    /* The accessory declared only the FM Europe/US band, so the RDS
     * branch must not have fired: that it did was the visible symptom of
     * reading the ID as capabilities. */
    CHECK(find_tx(0x07, 0x18) == NULL,
          "the RDS notification command fired although the accessory "
          "advertised no RDS capability -- the transaction ID was read "
          "as a capability byte");

    /* MFi 2.6.1.1: the counter advances per command sent, so the three
     * commands of this exchange carry three distinct, ascending IDs. */
    int n = iaptest_tx_count(), seen = 0, prev = -1;
    for (int i = 0; i < n; i++) {
        const struct iaptest_pkt *q = iaptest_tx(i);
        if (!q || q->paylen < 4 || q->payload[0] != 0x07)
            continue;
        int id = (q->payload[2] << 8) | q->payload[3];
        CHECK(id > prev,
              "transaction ID 0x%04X does not advance on the previous "
              "0x%04X (command 0x%02X)", id, prev, q->payload[1]);
        prev = id;
        seen++;
    }
    CHECK(seen >= 2, "expected at least two stamped commands, saw %d", seen);
}

/* MFi 4.7.1 (p.287): "All RF tuner lingo commands require
 * authentication." The lingo bit and radio_present are both set by the
 * IdentifyDeviceLingoes handler, which runs before any handshake that
 * identify may have started, so the unauthenticated window is
 * reachable with nothing but well-formed packets.
 *
 * The refusal is silent. Table 4-111 (pp.288-290) gives this lingo one
 * acknowledgement, 0x00 AccessoryAck, "Acc to Dev" -- Origin:
 * Accessory. There is nothing the device can send to say no, exactly
 * as in the Microphone lingo, so cmd_ack() here is a no-op and what
 * the accessory observes is that its command drove nothing. */
void test_tuner_requires_authentication(void)
{
    /* Declare the lingo but stop short of authenticating. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x07));
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();

    /* RetTunerCaps advertising every capability. Authenticated, this
     * drives SetTunerCtrl and SetTunerBand off the back of it. */
    send_ret_tuner_caps(0xFFFFFFFF);

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2)
            continue;
        CHECK(p->payload[0] != 0x07,
              "an unauthenticated accessory drove RF Tuner command 0x%02X",
              p->payload[1]);
    }

    /* GetRdsData's reply is the other half: it reaches the RDS text
     * that ends up on the radio screen. */
    iaptest_tx_clear();
    {
        unsigned char rds[8] = { 0x07, 0x1D, 0x1E, 0x00,
                                 'H', 'A', 'C', 'K' };
        iaptest_rx(rds, sizeof(rds));
    }
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2)
            continue;
        CHECK(p->payload[0] != 0x07,
              "an unauthenticated RetRdsData drove RF Tuner command 0x%02X",
              p->payload[1]);
    }

    /* Once authenticated the same packet is honoured, so the gate is
     * refusing the state and not the command. */
    iaptest_force_authenticated();
    iaptest_tx_clear();
    send_ret_tuner_caps(0xFFFFFFFF);
    CHECK(find_tx(0x07, 0x05) != NULL || find_tx(0x07, 0x08) != NULL,
          "an authenticated RetTunerCaps drove no tuner command, so the "
          "case proves nothing about the gate");
}

/* The two tuner payloads the accessory originates are read straight
 * out of the packet with no length of their own. Table 4-111 (p.289)
 * gives RetTunerFreq and TunerSeekDone a data length of 7;
 * rmt_tuner_freq() reads buf[2..6] and casts its len argument to void.
 * RetRdsData is "0xNN" bytes (p.290) and rmt_tuner_rds_data() takes its
 * payload from buf+4, so a packet shorter than that underflows. */
void test_tuner_short_payloads_are_refused(void)
{
    tuner_bringup();

    /* A bare RetTunerFreq. Unchecked, this took the frequency and the
     * RSSI from five bytes of RX-buffer tail and declared the radio
     * tuned on them. */
    {
        unsigned char p[2] = { 0x07, 0x0A };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(ipod_rmt_tuner_get(RADIO_TUNED), 0,
                 "a two-byte RetTunerFreq tuned the radio from whatever "
                 "followed the packet");

    /* TunerSeekDone lands in the same reader and has the same length. */
    {
        unsigned char p[6] = { 0x07, 0x13, 0x00, 0x01, 0x63, 0x14 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(ipod_rmt_tuner_get(RADIO_TUNED), 0,
                 "a six-byte TunerSeekDone was accepted; Table 4-111 gives "
                 "it a data length of 7");

    /* A full one is still honoured, so the bound is not simply refusing
     * everything. */
    {
        unsigned char p[7] = { 0x07, 0x0A, 0x00, 0x01, 0x63, 0x14, 0x40 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(ipod_rmt_tuner_get(RADIO_TUNED), 1,
                 "a seven-byte RetTunerFreq did not tune the radio");
    CHECK_EQ_INT(tuner_frequency, 90900 * 1000,
                 "a full RetTunerFreq decoded the wrong frequency");

    /* RadioText below the payload start. len-4 is unsigned, so this
     * used to ask rds_push_info() for 0xFFFFFFFF bytes; it clamps
     * against the destination only, so 64 bytes past the packet went
     * to the radio screen. */
    rbstub_rds.n = 0;
    {
        unsigned char p[3] = { 0x07, 0x1D, 0x04 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(rbstub_rds.n, 0,
                 "a three-byte RetRdsData pushed RDS text read from past "
                 "the end of the packet");

    /* Straight at the driver as well. It is exported, it owns the
     * len-4 subtraction, and going through the lingo cannot separate
     * its guard from a length check at the call site -- with both
     * present, removing either one is invisible. */
    rbstub_rds.n = 0;
    {
        unsigned char raw[3] = { 0x07, 0x1D, 0x04 };
        rmt_tuner_rds_data(sizeof(raw), raw);
    }
    CHECK_EQ_INT(rbstub_rds.n, 0,
                 "rmt_tuner_rds_data() underflowed len-4 on a three-byte "
                 "buffer and pushed what followed it");

    /* A station name shorter than eight characters is pushed at the
     * length the packet actually carries. */
    rbstub_rds.n = 0;
    {
        unsigned char p[6] = { 0x07, 0x1D, 0x1E, 0x00, 'B', 'C' };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(rbstub_rds.n, 1, "a short station name pushed nothing");
    if (rbstub_rds.n == 1)
        CHECK_EQ_INT(rbstub_rds.push[0].size, 2,
                     "the station name was pushed at its fixed eight bytes "
                     "from a packet carrying two, so six came from past it");
}

/* MFi 2.6.1.4 (p.112): "When transaction IDs are enabled, all iAP
 * commands must use them ... the developer must add them after the
 * Command ID field in every command packet". 1fa4c9f171 added the
 * offset to this handler but applied it to RetTunerCaps only; the
 * payloads handed to the driver still indexed from the packet start,
 * so under IDPS the frequency came out of the transaction ID. */
void test_tuner_payloads_honour_transaction_ids(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();
    device.lingoes |= (1u << 0x07);
    iaptest_tx_clear();

    /* 0x016314 kHz with an RSSI of 0x40, behind a transaction ID. */
    {
        unsigned char p[9] = { 0x07, 0x0A, 0x12, 0x34,
                               0x00, 0x01, 0x63, 0x14, 0x40 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(ipod_rmt_tuner_get(RADIO_TUNED), 1,
                 "a RetTunerFreq under IDPS did not tune the radio");
    /* radio_tuned alone proves nothing here -- rmt_tuner_freq() sets it
     * whatever the bytes decode to. 0x016314 kHz is 90900, so the
     * frequency is 90.9 MHz; read from the packet start instead it
     * would come out of 0x12 0x34 0x00 0x01. */
    CHECK_EQ_INT(tuner_frequency, 90900 * 1000,
                 "the frequency was decoded from the wrong offset, so it "
                 "came out of the transaction ID");

    /* And the RDS type byte is where the offset puts it, not two bytes
     * earlier -- at 0x12 it matched neither 0x1E nor 0x04, so RDS went
     * silently dead for any accessory that ran IDPS. */
    rbstub_rds.n = 0;
    {
        unsigned char p[10] = { 0x07, 0x1D, 0x12, 0x34,
                                0x1E, 0x00, 'W', 'X', 'Y', 'Z' };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(rbstub_rds.n, 1,
                 "RetRdsData under IDPS pushed nothing; the data type was "
                 "read out of the transaction ID");
    if (rbstub_rds.n == 1) {
        CHECK_EQ_INT(rbstub_rds.push[0].size, 4, "station name length");
        CHECK(memcmp(rbstub_rds.push[0].data, "WXYZ", 4) == 0,
              "the station name came from the wrong offset");
    }
}

/* The parameter byte is a cache of what the tuner holds, and every
 * setting that touches it writes the whole byte back. So a setting that
 * changes the cache without sending it corrupts every later write.
 *
 * set_mono() cleared bit 4 outside its own guard, on every call --
 * including calls that change nothing and send nothing. apps/radio/radio.c
 * sets the region and then force-mono on each entry to the radio
 * screen, so by the third entry the byte on the wire had force-mono off
 * while the user's setting still said on. */
void test_tuner_force_mono_survives_a_repeated_set(void)
{
    tuner_bringup();

    /* Turn it on, then ask for it again -- the second ask is a no-op
     * and must leave the cached byte alone. */
    ipod_rmt_tuner_set(RADIO_FORCE_MONO, 1);
    ipod_rmt_tuner_set(RADIO_FORCE_MONO, 1);

    /* Now the region writes the parameter byte -- which is exactly
     * what apps/radio/radio.c does on entry to the radio screen,
     * before it sets force-mono again. Whatever goes out has to still
     * have bit 4 set. */
    iaptest_tx_clear();
    ipod_rmt_tuner_set(RADIO_REGION, global_settings.fm_region);

    const struct iaptest_pkt *p = find_tx(0x07, 0x0E);
    CHECK(p != NULL, "no SetTunerMode after a de-emphasis change");
    if (p && p->paylen >= 3)
        CHECK(p->payload[2] & 0x10,
              "the parameter byte went out as 0x%02X with force-mono "
              "clear, although it was set twice and never turned off",
              p->payload[2]);
}

/* MFi 4.3.12 (p.257) gives RemoteEventNotification "Origin: Apple
 * device" -- it is a report. Announcing that playback stopped must not
 * also stop things.
 *
 * The play-status block used to call audio_pause() and
 * tuner_set(RADIO_MUTE, 1) after sending its packet. Opening the FM
 * radio screen calls audio_stop(), which lands there as a status
 * change: the accessory was sent "Stopped", and the tuner_set() then
 * made ipod_remote_tuner.c send its own "Paused" for the same event and
 * put the tuner hardware to sleep -- the radio the user had just
 * opened. */
void test_tuner_play_status_notification_does_not_stop_the_radio(void)
{
    tuner_bringup();
    /* tuner_bringup() declares lingoes 0x00, 0x02 and 0x07; the
     * subscription below is a Display Remote command. */
    device.lingoes |= (1u << 0x03);
    radio_present = 1;
    rbstub_set_audio_status(AUDIO_STATUS_PLAY);

    /* Subscribe to play status. Legacy session, so no transaction ID
     * -- the mask starts at byte 2. */
    IAPTEST_RX(0x03, 0x08, 0x00, 0x00, 0x00, 0x08);
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* The radio screen stops playback. */
    rbstub_set_audio_status(0);
    iaptest_tx_clear();
    rbstub_reset_calls();
    for (int t = 0; t < 4; t++)
        iap_periodic();

    /* Exactly one play-status event, and it says Stopped. */
    int events = 0, paused = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        /* Legacy session: no transaction ID, so the event type is at
         * byte 2 and its state at byte 3. */
        if (!p || p->paylen < 4 || p->payload[0] != 0x03
            || p->payload[1] != 0x09 || p->payload[2] != 0x03)
            continue;
        events++;
        if (p->payload[3] == 0x02)
            paused++;
    }
    CHECK(events == 1,
          "%d play-status events went out for one change; the second is "
          "the tuner driver answering the first", events);
    CHECK_EQ_INT(paused, 0,
                 "a Paused event was sent for a stop");

    /* And the tuner was not put to sleep. */
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2 || p->payload[0] != 0x07)
            continue;
        CHECK(p->payload[1] != 0x05,
              "SetTunerCtrl went out from a play-status notification; "
              "that powers down the radio the user just opened");
    }

    CHECK_EQ_INT(rbstub_calls.pause, 0,
                 "the notification paused playback it was only meant to "
                 "report on");
}

/* MFi 4.2.7 (p.226): the accessory "must send the button status packet
 * repeatedly at intervals between 30 and 100 ms, while one or more
 * buttons are pressed". So a held Play/Pause arrives as 10 to 33
 * packets a second.
 *
 * Each one used to flip the radio mute and call tuner_set(), which
 * ipod_remote_tuner.c answers with its own play-status notification and
 * an rmt_tuner_sleep() -- ten to thirty alternating Playing/Paused
 * packets a second, with the net mute state decided by whether the
 * count was odd or even. Shuffle and Repeat in the same handler were
 * already debounced; this was not. */
void test_tuner_play_pause_mutes_once_per_press(void)
{
    tuner_bringup();
    radio_present = 1;

    /* One press, repeated as the spec requires.
     *
     * Two samples between repeats, not one. iap-lingo2.c sets
     * iap_repeatbtn to 2 on a state change and remote_control_rx()
     * takes one off, so at one sample per repeat the deferral in
     * iap_handlepkt() swallowed every other packet and the case
     * measured four repeats where it reads eight. */
    iaptest_tx_clear();
    for (int r = 0; r < 8; r++) {
        IAPTEST_RX(0x02, 0x00, 0x01);
        iaptest_button_sample(2);
    }

    int toggles = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x07
            && p->payload[1] == 0x05)
            toggles++;
    }
    CHECK(toggles == 1,
          "a single held Play/Pause drove the tuner %d times; the "
          "accessory repeats its status every 30 to 100 ms and each "
          "repeat was taken as a new press", toggles);

    /* Release, then press again: that is a second toggle.
     *
     * The release raises no button but it is still a state change, so
     * it re-arms iap_repeatbtn. iap_periodic() does not take that down
     * -- remote_control_rx() does -- so the press below was re-queued
     * rather than handled, and the second toggle it counts was really
     * the tail of the first. */
    IAPTEST_RX(0x02, 0x00, 0x00);
    iaptest_button_sample(2);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x01);

    toggles = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x07
            && p->payload[1] == 0x05)
            toggles++;
    }
    CHECK(toggles == 1,
          "a second press after a release reached the tuner %d times",
          toggles);

    /* The discrete Play and Pause buttons are byte 1 bits 0 and 1
     * (Table 4-14, p.227) and repeat at the same 30 to 100 ms. They sit
     * one screen below the contextual Play/Pause and were left
     * unlatched when it was fixed -- one function, two rules. */
    static const struct { unsigned char bit; const char *name; } discrete[] = {
        { 0x01, "Play/Resume" },
        { 0x02, "Pause" },
    };

    for (unsigned d = 0; d < sizeof(discrete)/sizeof(discrete[0]); d++) {
        iaptest_init();
        tuner_bringup();
        radio_present = 1;

        iaptest_tx_clear();
        /* Sampled between repeats, as the 100 Hz button tick does on
         * hardware. Without it iap_handlepkt() defers every packet
         * after the first -- a pending button re-queues the next one --
         * so the harness silently drops the repeats and cannot tell a
         * latched arm from an unlatched one.
         *
         * Two samples, not one: iap-lingo2.c arms iap_repeatbtn at 2
         * and remote_control_rx() takes one off, so at one per repeat
         * this still dropped every other packet and the loop delivered
         * four of the eight it reads. */
        unsigned char pkt[4] = { 0x02, 0x00, 0x00, discrete[d].bit };
        for (int r = 0; r < 8; r++) {
            iaptest_rx(pkt, sizeof(pkt));
            iaptest_button_sample(2);
        }

        int n = 0;
        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (p && p->paylen >= 2 && p->payload[0] == 0x07
                && p->payload[1] == 0x05)
                n++;
        }
        CHECK(n <= 1,
              "a held %s drove the tuner %d times; the accessory repeats "
              "its status every 30 to 100 ms and each repeat reset the "
              "tuner's cached state", discrete[d].name, n);
    }
}

/* Unmuting must not undo what tuning just did.
 *
 * radio_start() (apps/radio/radio.c) drives, in this order:
 * RADIO_SLEEP 0, RADIO_REGION, RADIO_FORCE_MONO, RADIO_FREQUENCY, then
 * RADIO_MUTE 0. Unmute used to call rmt_tuner_sleep(0), the cold
 * wake-up -- which clears old_region, mono_mode, tuner_frequency and
 * radio_tuned and re-sends SetTunerMode 0x00, the 200 kHz / stereo /
 * 75 us default. So the region's spacing and deemphasis and Force Mono
 * were applied and immediately reverted, and radio_tuned was cleared
 * after the tune.
 *
 * radio_tuned gates the whole body of RADIO_SCAN_FREQUENCY, so seek
 * sent no TunerSeekStart and preset scanning found nothing.
 *
 * MFi 4.7.10 (p.295) is why power alone is the right thing for a mute
 * to do: "RF tuner state information, such as tuner frequency, band,
 * and so on, must be preserved by the accessory across tuner on and off
 * cycles", and "When the tuner power is turned off, it must disable its
 * audio output". */
void test_tuner_unmute_keeps_the_tuned_state(void)
{
    tuner_bringup();
    send_ret_tuner_caps(CAP_AM | CAP_FM_EU_US | CAP_FM_JAPAN | CAP_FM_WIDE);

    /* The order radio_start() uses. */
    tuner_set(RADIO_SLEEP, 0);
    tuner_set(RADIO_REGION, 0);
    tuner_set(RADIO_FORCE_MONO, 1);
    tuner_set(RADIO_FREQUENCY, 90900000);

    /* The accessory reports what it tuned to -- RetTunerFreq, Table
     * 4-111 (p.289), 90900 kHz and a signal level. */
    IAPTEST_RX(0x07, 0x0A, 0x00, 0x01, 0x63, 0x14, 0x40);
    CHECK(tuner_get(RADIO_TUNED),
          "the accessory's RetTunerFreq did not mark the radio tuned, "
          "so the unmute below would prove nothing");

    /* Unmute, last in the sequence. */
    iaptest_tx_clear();
    tuner_set(RADIO_MUTE, 0);

    CHECK(tuner_get(RADIO_TUNED),
          "unmuting cleared radio_tuned, which gates every arm of "
          "RADIO_SCAN_FREQUENCY -- so seek and preset scanning stop "
          "working the moment the radio starts");
    /* The cached frequency is not readable through tuner_get(), so ask
     * for the same frequency again: rmt_tuner_set_freq() only puts a
     * SetTunerFreq on the wire when the value differs from its cache.
     * A re-tune here means the cache was cleared. */
    {
        int before = iaptest_tx_count();
        tuner_set(RADIO_FREQUENCY, 90900000);
        bool retuned = false;
        for (int i = before; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *q = iaptest_tx(i);
            if (q && q->paylen >= 2 && q->payload[0] == 0x07
                && q->payload[1] == 0x0B)
                retuned = true;
        }
        CHECK(!retuned,
              "unmuting forgot the tuned frequency, so the next tune to "
              "the same station went out again as a fresh SetTunerFreq");
    }
    CHECK(find_tx(0x07, 0x0E) == NULL,
          "unmuting re-sent SetTunerMode, reverting the spacing and "
          "deemphasis the region had just set");
    CHECK(find_tx(0x07, 0x08) == NULL,
          "unmuting re-sent SetTunerBand");

    /* It must still turn the tuner on, which is what actually unmutes:
     * 4.7.10 again, "When the tuner power is turned off, it must
     * disable its audio output." */
    const struct iaptest_pkt *p = find_tx(0x07, 0x05);
    CHECK(p != NULL && p->paylen >= 3 && p->payload[2] == 0x01,
          "unmuting did not turn the tuner back on");
}

/* A seek that found nothing is not a station.
 *
 * MFi 4.7.24 (p.305), RetTunerFreq after a seek: "If no channel was
 * found, a tuner frequency value of 0xFFFFFFFF must be reported."
 *
 * It was taken for a frequency. 0xFFFFFFFF * 1000 wraps to 4294966296
 * and lands in an int as -1000, which is below every region's
 * freq_min -- so a failed seek left the radio reporting a negative
 * frequency with radio_tuned set, and the UI showed a station that was
 * not there. */
void test_tuner_failed_seek_is_not_a_station(void)
{
    tuner_bringup();
    send_ret_tuner_caps(CAP_FM_EU_US);

    tuner_set(RADIO_SLEEP, 0);
    tuner_set(RADIO_FREQUENCY, 90900000);

    /* A real one first, so "not tuned" below cannot be the initial
     * state passing for a result. */
    IAPTEST_RX(0x07, 0x0A, 0x00, 0x01, 0x63, 0x14, 0x40);
    CHECK(tuner_get(RADIO_TUNED),
          "a real RetTunerFreq did not mark the radio tuned");

    /* And then the sentinel. */
    IAPTEST_RX(0x07, 0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0x00);
    CHECK(!tuner_get(RADIO_TUNED),
          "a seek that found no channel left the radio marked tuned");

    /* And the cached frequency is untouched, so the next tune to the
     * station that was playing before the seek is still a no-op. The
     * signal level is cleared too, but nothing reads it through
     * tuner_get() -- RADIO_STEREO is hardcoded true in this driver. */
    {
        int before = iaptest_tx_count();
        tuner_set(RADIO_FREQUENCY, 90900000);
        bool retuned = false;
        for (int i = before; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *q = iaptest_tx(i);
            if (q && q->paylen >= 2 && q->payload[0] == 0x07
                && q->payload[1] == 0x0B)
                retuned = true;
        }
        CHECK(!retuned,
              "the sentinel overwrote the cached frequency");
    }
}

/* Mute toggle, Table 4-14 (p.227) byte 1 bit 2.
 *
 * Undecoded, so the button did nothing. Only the radio has a mute to
 * toggle: Rockbox has no mute for playback, and turning the volume down
 * to nothing is not one, because there is nowhere to put the level it
 * replaced. Latched like Play/Resume and Pause beside it, because the
 * accessory repeats its status every 30 to 100 ms. */
void test_tuner_mute_toggle_toggles_once_per_press(void)
{
    tuner_bringup();
    radio_present = 1;

    iaptest_tx_clear();
    for (int r = 0; r < 6; r++) {
        IAPTEST_RX(0x02, 0x00, 0x00, 0x04);
        iaptest_button_sample(2);
    }

    int toggles = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 3 && p->payload[0] == 0x07
            && p->payload[1] == 0x05)
            toggles++;
    }
    CHECK(toggles == 1,
          "a held Mute toggle drove the tuner %d times", toggles);

    /* Release, press again: the second press toggles back. */
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(2);
    for (int t = 0; t < 6; t++)
        iap_periodic();

    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x04);
    const struct iaptest_pkt *p = find_tx(0x07, 0x05);
    CHECK(p != NULL && p->paylen >= 3,
          "a second Mute press did not reach the tuner");
    if (p && p->paylen >= 3)
        CHECK_EQ_INT(p->payload[2], 0x01,
                     "the second press must unmute, not mute again");
}

/* The device asks the tuner what it can do.
 *
 * MFi 4.7.7 (p.293), RetTunerCaps: "This command is sent by an RF tuner
 * accessory in response to a GetTunerCaps command sent by an Apple
 * device." Purely a response -- and nothing sent the request, so
 * iap_handlepkt_mode7()'s case 0x02 could not run on hardware at all.
 * That case is the whole capability-driven bring-up: it powers the
 * tuner on with a status-notify mask the accessory said it supports,
 * sets the mode, and picks a band from the bands the accessory
 * advertises. Every other case in this file feeds it RetTunerCaps by
 * hand, so the suite exercised a path a real accessory would never take
 * -- fifteen cases resting on a packet that could not arrive. */
void test_tuner_capabilities_are_requested(void)
{
    iaptest_init();
    iapacc_attach();

    /* Identify with the RF Tuner lingo, unauthenticated. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x07));
    device.auth.state = AUST_INIT;

    /* Nothing yet: lingo 7 needs authentication and there is none. */
    iaptest_tx_clear();
    iap_periodic();
    CHECK(find_tx(0x07, 0x01) == NULL,
          "GetTunerCaps went out before the accessory was "
          "authenticated, and every lingo 7 command requires it");

    /* Once authenticated, it goes. */
    iaptest_force_authenticated();
    iaptest_tx_clear();
    iap_periodic();
    {
        const struct iaptest_pkt *p = find_tx(0x07, 0x01);
        CHECK(p != NULL,
              "the device never asked the tuner for its capabilities, "
              "so the whole of iap-lingo7.c case 0x02 is unreachable");
        if (p)
            CHECK_EQ_INT(p->paylen, 2,
                         "GetTunerCaps takes no parameters (Table 4-115, "
                         "p.292)");
    }

    /* And only once. */
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK(find_tx(0x07, 0x01) == NULL,
          "GetTunerCaps was sent again with nothing to prompt it");

    /* An accessory without the lingo is never asked. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    for (int t = 0; t < 4; t++)
        iap_periodic();
    CHECK(find_tx(0x07, 0x01) == NULL,
          "GetTunerCaps went to an accessory without the RF Tuner "
          "lingo");
}

/* An accessory that declares the RF Tuner lingo through IDPS gets a
 * radio too.
 *
 * radio_present was set while the IdentifyToken was being parsed and
 * then wiped by the iap_reset_device() that EndIDPS runs, which the
 * restore block did not put back -- so the accessory got lingo 0x07
 * service while the tuner driver answered RADIO_PRESENT = 0 and the
 * radio screen never appeared. And tuner_caps_pending was only ever set
 * on the IdentifyDeviceLingoes path, so an IDPS accessory was never
 * asked what it could do either: the whole capability bring-up in
 * iap-lingo7.c case 0x02 stayed unreachable for it. */
void test_tuner_idps_accessory_gets_a_radio(void)
{
    iaptest_init();
    radio_present = 0;

    /* StartIDPS, an IdentifyToken declaring General, Simple Remote and
     * RF Tuner, then EndIDPS. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x0E, 0x00, 0x00,
               0x03, 0x00, 0x02, 0x07,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);

    CHECK(device.lingoes & (1u << 0x07),
          "the IdentifyToken did not negotiate the RF Tuner lingo, so "
          "nothing below is being tested (lingoes = 0x%08X)",
          device.lingoes);
    CHECK(radio_present,
          "an accessory that declared the RF Tuner lingo through IDPS "
          "was left with RADIO_PRESENT = 0, so the radio screen never "
          "appears");

    /* And it is asked for its capabilities, once authenticated. */
    iaptest_force_authenticated();
    iaptest_tx_clear();
    iap_periodic();
    CHECK(find_tx(0x07, 0x01) != NULL,
          "an IDPS accessory was never asked for its tuner "
          "capabilities");
}

/* The tuner driver's commands carry transaction IDs under IDPS.
 *
 * MFi p.95: "After an Apple device has successfully acknowledged an
 * accessory's StartIDPS command, all subsequent iAP command packets
 * must include transaction IDs, regardless of lingo."
 *
 * ipod_remote_tuner.c sends fourteen commands through iap_send_pkt(),
 * which copied the caller's bytes through untouched -- while
 * iap-lingo7.c builds the very same commands with
 * IAP_TX_PUT_IPOD_TRANSID(), so 07 05 could go out both ways in one
 * session. It stayed hidden while radio_present was only set on the
 * legacy identify path; once an IDPS accessory could have a radio,
 * opening the FM screen put a dozen malformed frames on the wire. */
void test_tuner_driver_commands_carry_transaction_ids(void)
{
    iaptest_init();
    iapacc_attach();

    /* An IDPS accessory with the RF Tuner lingo. */
    IAPTEST_RX(0x00, 0x38, 0x00, 0x01);
    IAPTEST_RX(0x00, 0x39, 0x00, 0x02, 0x01,
               0x0E, 0x00, 0x00,
               0x03, 0x00, 0x02, 0x07,
               0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00);
    IAPTEST_RX(0x00, 0x3B, 0x00, 0x03, 0x00);
    iaptest_force_authenticated();
    CHECK(DEVICE_TRANSID_ACTIVE, "the session must be using IDPS");
    CHECK(radio_present, "the accessory must have a radio");

    /* Everything the driver sends when the radio screen opens. */
    iaptest_tx_clear();
    tuner_set(RADIO_SLEEP, 0);
    tuner_set(RADIO_REGION, 0);
    tuner_set(RADIO_FREQUENCY, 90900000);
    tuner_set(RADIO_MUTE, 0);

    int seen = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2)
            continue;
        if (p->payload[0] != 0x07 && p->payload[0] != 0x03)
            continue;

        seen++;
        /* Lingo, one-byte command, then the two-byte ID. A driver
         * packet without one is two bytes short of every equivalent
         * iap-lingo7.c builds. */
        CHECK(p->paylen >= 4,
              "the driver sent a %d byte lingo 0x%02X command with no "
              "room for a transaction ID", p->paylen, p->payload[0]);
    }
    CHECK(seen > 0,
          "the driver sent nothing, so this case is not testing it");

    /* The accessory model judges the IDs of everything above; it flags
     * a device-originated command that carries none. */
    CHECK(iapacc_judged() > 0, "the model judged nothing");
    CHECK(iapacc_violations() == 0,
          "the accessory model rejected a driver packet: %s",
          iapacc_first_violation());
}

/* The tuner is not powered on just because it can be.
 *
 * MFi Table 4-123 (p.296) bit 0: "Turn RF tuner current draw from the
 * Apple device on (1) or off (0). When RF tuner current draw is turned
 * off, the accessory should rest in the lowest power state that still
 * allows iAP commands to be received and processed."
 *
 * The capability handler sent SetTunerCtrl with that bit set as soon as
 * the accessory said it supported power control -- at authentication.
 * So plugging in a Radio Remote and never opening the radio left it
 * drawing Intermittent High Power (4.7.2, p.288) for the whole session,
 * with the only power-off on the FM-screen exit path.
 *
 * Power belongs to the radio screen. Reachable only since the device
 * started asking for capabilities at all, which is this campaign's own
 * doing. */
void test_tuner_is_not_powered_on_at_authentication(void)
{
    tuner_bringup();
    radio_present = 1;

    iaptest_tx_clear();
    /* Bit 08 is "supports tuner power on/off" -- the byte the old code
     * keyed on. Without it in the word the handler never reached its
     * SetTunerCtrl at all, so a case using only the band bits proved
     * nothing about the power. */
    send_ret_tuner_caps(CAP_AM | CAP_FM_EU_US | CAP_FM_JAPAN | CAP_FM_WIDE
                        | (1u << 8));

    /* The band and mode are set from the capabilities -- 4.7.10 (p.295)
     * has the accessory keep them "across tuner on and off cycles", so
     * that is safe with the tuner off. */
    CHECK(find_tx(0x07, 0x08) != NULL,
          "the capability reply did not select a band, so this case is "
          "not exercising the handler");

    /* But nothing turns the current draw on. */
    {
        const struct iaptest_pkt *p = find_tx(0x07, 0x05);
        CHECK(p == NULL,
              "the tuner was powered on at authentication, before the "
              "user asked for the radio");
    }

    /* Opening the radio does. */
    iaptest_tx_clear();
    tuner_set(RADIO_SLEEP, 0);
    {
        const struct iaptest_pkt *p = find_tx(0x07, 0x05);
        CHECK(p != NULL && p->paylen >= 3,
              "opening the radio did not power the tuner");
        if (p && p->paylen >= 3)
            CHECK_EQ_INT(p->payload[p->paylen - 1], 0x01,
                         "the radio screen must turn the current draw "
                         "on");
    }

    /* And leaving it turns it off again. */
    iaptest_tx_clear();
    tuner_set(RADIO_SLEEP, 1);
    {
        const struct iaptest_pkt *p = find_tx(0x07, 0x05);
        CHECK(p != NULL && p->paylen >= 3,
              "leaving the radio did not power the tuner down");
        if (p && p->paylen >= 3)
            CHECK_EQ_INT(p->payload[p->paylen - 1], 0x00,
                         "leaving the radio must turn the current draw "
                         "off");
    }
}

/* iap_reset_lingo2()'s clear of remote_mute is load-bearing, and until
 * the mutation sweep learned to delete reset-path clears nothing said
 * so: the whole class was invisible to it.
 *
 * remote_mute is the Simple Remote Mute button's latch (Table 4-14
 * p.227, byte 1 bit 2), and the button is a toggle -- it sends no state,
 * only "flip it". rmt_tuner_mute() answers a mute by powering the tuner
 * down (ipod_remote_tuner.c:222; the Apple tuner has no mute of its
 * own), so a detach leaves the tuner off and the next accessory starts
 * from unmuted. If the latch survives, its first Mute press flips a
 * true that no longer describes anything and unmutes an already-unmuted
 * radio: the user presses Mute, hears no change, and has to press it
 * again. */
void test_tuner_mute_does_not_outlive_the_accessory(void)
{
    tuner_bringup();
    radio_present = 1;

    /* Mute, and let the button go up again. */
    IAPTEST_RX(0x02, 0x00, 0x00, 0x04);
    iaptest_button_sample(2);
    {
        const struct iaptest_pkt *p = find_tx(0x03, 0x09);
        CHECK(p != NULL && p->paylen >= 4 && p->payload[3] == 0x02,
              "the Mute button unmuted rather than muted on a fresh "
              "session -- the latch came in true from whatever ran "
              "before this case, which is the same leak the second half "
              "of this case tests across a detach");
    }
    IAPTEST_RX(0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(2);

    /* It goes away; another Radio Remote arrives. */
    iap_reset_state(IF_IAP_MP(0));
    tuner_bringup();
    radio_present = 1;

    /* One Mute press on the new accessory. It must mute. */
    iaptest_tx_clear();
    IAPTEST_RX(0x02, 0x00, 0x00, 0x04);
    iaptest_button_sample(2);

    const struct iaptest_pkt *p = find_tx(0x03, 0x09);
    CHECK(p != NULL,
          "the replacement accessory's Mute press drove the tuner not "
          "at all");
    if (p && p->paylen >= 4)
        CHECK_EQ_INT(p->payload[3], 0x02,
                     "the replacement accessory's first Mute press "
                     "unmuted -- it inherited the previous accessory's "
                     "mute latch, and the radio was not muted");
}
