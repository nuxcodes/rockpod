/***************************************************************************
 * Legacy wire-format dump.
 *
 * Drives a non-IDPS accessory through a broad set of commands and prints
 * every byte transmitted. Running this against two revisions and diffing
 * the output shows whether anything changed for accessories that never
 * enable transaction IDs -- which is most of them, and all the ones this
 * project cannot test on hardware.
 *
 * Built as a separate binary so it can be pointed at older sources:
 *
 *     make dump && ./dump_legacy > /tmp/after.txt
 *
 * This file deliberately asserts nothing. It is a golden-output tool.
 ****************************************************************************/

#include "iap_test.h"
#include "accessory.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "iap.h"
#include "iap-core.h"
#include "appevents.h"

extern void iap_periodic(void);

/* iap_test.c is linked with its runner renamed away, but that runner
 * still references the case table. Satisfy it with an empty one. */
const struct iaptest_case iaptest_cases[] = { { "", 0 } };
const int iaptest_case_count = 0;

static void show(const char *what)
{
    printf("%-46s", what);
    if (iaptest_tx_count() == 0) {
        printf(" (silence)\n");
        return;
    }
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (i)
            printf("%-46s", "");
        for (int b = 0; b < p->rawlen; b++)
            printf(" %02X", p->raw[b]);
        printf("%s\n", p->checksum_ok ? "" : "   [BAD CHECKSUM]");
    }
    iaptest_tx_clear();
}

#define STEP(label, ...) do { \
        static const unsigned char _p[] = { __VA_ARGS__ }; \
        iaptest_rx(_p, (int)sizeof(_p)); \
        show(label); \
    } while (0)

int main(void)
{
    /* This records what the device says; it does not judge it. With the
     * model attached, dumping an older revision printed conformance
     * failures into the middle of the byte log. */
    iapacc_detach();

    printf("# Legacy (non-IDPS) accessory wire format\n");
    printf("# Every line: the command sent, then the framed bytes returned.\n\n");

    /* ---------------------------------------------------------------- */
    printf("## General lingo, identified with IdentifyDeviceLingoes\n");
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy(0x0000001D);   /* 0x00, 0x02, 0x03, 0x04 */

    STEP("RequestExtendedInterfaceMode 0x00/0x03", 0x00, 0x03);
    STEP("RequestiPodName 0x00/0x07",              0x00, 0x07);
    STEP("RequestiPodSoftwareVersion 0x00/0x09",   0x00, 0x09);
    STEP("RequestiPodSerialNum 0x00/0x0B",         0x00, 0x0B);
    STEP("RequestiPodModelNum 0x00/0x0D",          0x00, 0x0D);
    STEP("RequestLingoProtocolVersion 0x00/0x0F",  0x00, 0x0F, 0x03);
    STEP("GetiPodOptions 0x00/0x24",               0x00, 0x24);
    STEP("GetiPodOptionsForLingo 0x00/0x4B lingo3",0x00, 0x4B, 0x03);
    STEP("GetiPodOptionsForLingo 0x00/0x4B lingo4",0x00, 0x4B, 0x04);
    STEP("GetiPodPreferences 0x00/0x29 class 3",   0x00, 0x29, 0x03);
    STEP("SetiPodPreferences 0x00/0x2B on",        0x00, 0x2B, 0x03, 0x01, 0x00);
    STEP("SetiPodPreferences 0x00/0x2B off",       0x00, 0x2B, 0x03, 0x00, 0x00);
    STEP("Unimplemented 0x00/0x77",                0x00, 0x77);

    /* ---------------------------------------------------------------- */
    printf("\n## Display Remote lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy(0x0000001D);
    iaptest_force_authenticated();

    STEP("GetCurrentEQProfileIndex 0x03/0x01",     0x03, 0x01);
    STEP("GetNumEQProfiles 0x03/0x04",             0x03, 0x04);
    STEP("SetRemoteEventNotification 0x03/0x08",   0x03, 0x08, 0x00, 0x00, 0x80, 0x1F);
    STEP("GetRemoteEventStatus 0x03/0x0A",         0x03, 0x0A);
    STEP("GetiPodStateInfo 0x03/0x0C type 0x00",   0x03, 0x0C, 0x00);
    STEP("GetiPodStateInfo 0x03/0x0C type 0x01",   0x03, 0x0C, 0x01);
    STEP("GetiPodStateInfo 0x03/0x0C type 0x04",   0x03, 0x0C, 0x04);
    STEP("GetiPodStateInfo 0x03/0x0C type 0x05",   0x03, 0x0C, 0x05);
    STEP("GetiPodStateInfo 0x03/0x0C type 0x10",   0x03, 0x0C, 0x10);
    STEP("GetPlayStatus 0x03/0x0F",                0x03, 0x0F);
    STEP("GetPowerBatteryState 0x03/0x1A",         0x03, 0x1A);
    STEP("GetSoundCheckState 0x03/0x1C",           0x03, 0x1C);

    /* Volume reporting across the range. */
    printf("\n## Volume reported at each end of the codec range\n");
    for (int v = -60; v <= 12; v += 6) {
        char label[64];
        rbstub_set_volume(v);
        snprintf(label, sizeof(label), "GetiPodStateInfo type 0x04 at %+d dB", v);
        static const unsigned char q[] = { 0x03, 0x0C, 0x04 };
        iaptest_rx(q, sizeof(q));
        show(label);
    }

    /* ---------------------------------------------------------------- */
    printf("\n## Simple Remote lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy(0x0000001D);

    STEP("ContextButtonStatus play 0x02/0x00",     0x02, 0x00, 0x00, 0x01);
    iaptest_button_sample(4);
    STEP("ContextButtonStatus release 0x02/0x00",  0x02, 0x00, 0x00, 0x00);
    iaptest_button_sample(4);

    /* ---------------------------------------------------------------- */
    printf("\n## Extended Interface lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy(0x0000001D);
    iaptest_force_authenticated();

    STEP("EnterRemoteUIMode 0x00/0x05",            0x00, 0x05);
    STEP("GetProtocolVersion 0x04/0x0012",         0x04, 0x00, 0x12);
    STEP("GetiPodName 0x04/0x0014",                0x04, 0x00, 0x14);
    STEP("GetPlayStatus 0x04/0x001C",              0x04, 0x00, 0x1C);
    STEP("GetCurrentPlayingTrackIndex 0x04/0x001E",0x04, 0x00, 0x1E);
    STEP("GetNumPlayingTracks 0x04/0x0035",        0x04, 0x00, 0x35);
    STEP("GetShuffle 0x04/0x002C",                 0x04, 0x00, 0x2C);
    STEP("GetRepeat 0x04/0x002F",                  0x04, 0x00, 0x2F);
    STEP("SetPlayStatusChangeNotification enable", 0x04, 0x00, 0x26, 0x01);
    STEP("PlayControl toggle 0x04/0x0029",         0x04, 0x00, 0x29, 0x01);
    iaptest_button_sample(4);

    /* ---------------------------------------------------------------- */
    printf("\n## Unsolicited notifications, legacy accessory\n");
    iaptest_init();
    iapacc_detach();
    iaptest_identify_legacy(0x0000001D);
    iaptest_force_authenticated();
    {
        static const unsigned char ui[] = { 0x00, 0x05 };
        iaptest_rx(ui, sizeof(ui));
        static const unsigned char en[] = { 0x04, 0x00, 0x26, 0x01 };
        iaptest_rx(en, sizeof(en));
        static const unsigned char rn[] = { 0x03, 0x08, 0x00, 0x00, 0x80, 0x1F };
        iaptest_rx(rn, sizeof(rn));
        iaptest_tx_clear();

        rbstub_set_playlist(10, 3);
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        show("track change notification");

        for (int i = 0; i < 6; i++)
            iap_periodic();
        show("six periodic ticks");
    }

    /* ---------------------------------------------------------------- */
    printf("\n\n# IDPS accessory wire format\n");
    printf("# Every reply must carry a 2-byte transaction ID after the\n");
    printf("# command ID (MFi 2.6.1.4). Extended Interface uses a 2-byte\n");
    printf("# command ID, so its transaction sits one byte later.\n\n");

    printf("## General lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_enter_idps();

    STEP("RequestiPodName tid=BEEF",               0x00, 0x07, 0xBE, 0xEF);
    STEP("RequestiPodSoftwareVersion tid=0123",    0x00, 0x09, 0x01, 0x23);
    STEP("RequestiPodSerialNum tid=0007",          0x00, 0x0B, 0x00, 0x07);
    STEP("RequestiPodModelNum tid=0008",           0x00, 0x0D, 0x00, 0x08);
    STEP("RequestLingoProtocolVersion tid=0009",   0x00, 0x0F, 0x00, 0x09, 0x03);
    STEP("GetiPodOptions tid=0010",                0x00, 0x24, 0x00, 0x10);
    STEP("GetiPodOptionsForLingo l3 tid=0011",     0x00, 0x4B, 0x00, 0x11, 0x03);
    STEP("GetiPodPreferences class3 tid=0012",     0x00, 0x29, 0x00, 0x12, 0x03);
    STEP("SetiPodPreferences on tid=0013",         0x00, 0x2B, 0x00, 0x13, 0x03, 0x01, 0x00);
    STEP("SetiPodPreferences off tid=0014",        0x00, 0x2B, 0x00, 0x14, 0x03, 0x00, 0x00);
    STEP("Unimplemented 0x77 tid=0015",            0x00, 0x77, 0x00, 0x15);
    STEP("Unimplemented 0x77, truncated",          0x00, 0x77, 0xAA);

    printf("\n## Display Remote lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_enter_idps();
    iaptest_force_authenticated();

    STEP("GetNumEQProfiles tid=0020",              0x03, 0x04, 0x00, 0x20);
    STEP("SetRemoteEventNotification tid=0021",    0x03, 0x08, 0x00, 0x21, 0x00, 0x00, 0x80, 0x1F);
    STEP("GetRemoteEventStatus tid=0022",          0x03, 0x0A, 0x00, 0x22);
    STEP("GetiPodStateInfo type 0x04 tid=0023",    0x03, 0x0C, 0x00, 0x23, 0x04);
    STEP("GetiPodStateInfo type 0x10 tid=0024",    0x03, 0x0C, 0x00, 0x24, 0x10);
    STEP("GetPlayStatus tid=0025",                 0x03, 0x0F, 0x00, 0x25);
    STEP("GetPowerBatteryState tid=0026",          0x03, 0x1A, 0x00, 0x26);

    printf("\n## Extended Interface lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_enter_idps();
    iaptest_force_authenticated();

    STEP("EnterRemoteUIMode tid=0030",             0x00, 0x05, 0x00, 0x30);
    STEP("GetProtocolVersion tid=0031",            0x04, 0x00, 0x12, 0x00, 0x31);
    STEP("GetiPodName tid=0032",                   0x04, 0x00, 0x14, 0x00, 0x32);
    STEP("GetPlayStatus tid=0033",                 0x04, 0x00, 0x1C, 0x00, 0x33);
    STEP("GetNumPlayingTracks tid=0034",           0x04, 0x00, 0x35, 0x00, 0x34);
    STEP("GetShuffle tid=0035",                    0x04, 0x00, 0x2C, 0x00, 0x35);
    STEP("GetRepeat tid=0036",                     0x04, 0x00, 0x2F, 0x00, 0x36);
    STEP("SetPlayStatusChangeNotify tid=0037",     0x04, 0x00, 0x26, 0x00, 0x37, 0x01);
    STEP("PlayControl toggle tid=0038",            0x04, 0x00, 0x29, 0x00, 0x38, 0x01);
    iaptest_button_sample(4);

    printf("\n## Digital Audio lingo\n");
    iaptest_init();
    iapacc_detach();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    STEP("RetAccSampleRateCaps tid=0040",          0x0A, 0x03, 0x00, 0x40,
                                                   0x00, 0x00, 0xAC, 0x44);

    printf("\n## Unsolicited notifications, IDPS accessory\n");
    iaptest_init();
    iapacc_detach();
    iaptest_enter_idps();
    iaptest_force_authenticated();
    {
        static const unsigned char ui[] = { 0x00, 0x05, 0x00, 0x50 };
        iaptest_rx(ui, sizeof(ui));
        static const unsigned char en[] = { 0x04, 0x00, 0x26, 0x00, 0x51, 0x01 };
        iaptest_rx(en, sizeof(en));
        static const unsigned char rn[] = { 0x03, 0x08, 0x00, 0x52,
                                            0x00, 0x00, 0x80, 0x1F };
        iaptest_rx(rn, sizeof(rn));
        iaptest_tx_clear();

        rbstub_set_playlist(10, 3);
        rbstub_fire_event(PLAYBACK_EVENT_TRACK_CHANGE, NULL);
        show("track change notification");

        for (int i = 0; i < 6; i++)
            iap_periodic();
        show("six periodic ticks");
    }

    printf("\n# end\n");
    return 0;
}
