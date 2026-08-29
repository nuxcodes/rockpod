/***************************************************************************
 * iAP conformance tests: the authentication gate, every command that has one
 *
 * MFi Table 2-7 (p.105) marks each lingo's commands as requiring
 * authentication or not, and the handlers implement that with CHECKAUTH.
 * Twenty of those macros were load-bearing in name only: deleting any one
 * of them individually left all four binaries green, because no case ever
 * sent one of these commands from an unauthenticated session.
 *
 * This drives every CHECKAUTH site there is. The list is generated from
 * the sources -- if a handler gains a gate, add its command here, and if
 * one is removed on purpose the entry has to go with it.
 ****************************************************************************/
#include <string.h>
#include "iap_test.h"
#include "iap-core.h"

struct gated {
    unsigned char lingo;
    unsigned char cmd;
    const char   *name;
    /* True for the two commands that are themselves acknowledgements.
     * Their handlers are CHECKAUTH followed by nothing, so once the
     * gate opens the right answer is silence -- acknowledging an
     * acknowledgement is a packet the accessory is not waiting for.
     * Before the gate opens CHECKAUTH still answers them, which is
     * inconsistent but is what every other command does and what the
     * refusal case pins. */
    bool          acks_nothing;
};

/* Every CHECKAUTH in apps/iap/iap-lingo0.c and iap-lingo3.c. */
static const struct gated gated_cmds[] = {
    { 0x00, 0x1A, "GetiPodAuthenticationInfo"      },
    { 0x00, 0x1C, "AckiPodAuthenticationInfo",      true },
    { 0x00, 0x1D, "GetiPodAuthenticationSignature" },
    { 0x00, 0x1F, "AckiPodAuthenticationStatus",    true },
    { 0x00, 0x29, "GetiPodPreferences"      },
    /* Added when the mutation sweep found its CHECKAUTH uncovered --
     * the command was new and the table was not updated with it. */
    { 0x00, 0x35, "GetUIMode"               },
    { 0x00, 0x2B, "SetiPodPreferences"      },
    { 0x00, 0x37, "SetUIMode"               },

    { 0x03, 0x08, "SetRemoteEventNotification" },
    { 0x03, 0x0A, "GetRemoteEventStatus"    },
    { 0x03, 0x0C, "GetiPodStateInfo"        },
    { 0x03, 0x0E, "SetiPodStateInfo"        },
    { 0x03, 0x0F, "GetPlayStatus"           },
    { 0x03, 0x11, "GetCurrentPlayingTrackIndex" },
    { 0x03, 0x12, "GetIndexedPlayingTrackTitle" },
    { 0x03, 0x14, "GetIndexedPlayingTrackArtistName" },
    { 0x03, 0x16, "GetIndexedPlayingTrackAlbumName" },
    { 0x03, 0x18, "SetPlayStatusChangeNotification" },
    { 0x03, 0x1A, "GetPowerBatteryState"    },
    { 0x03, 0x1C, "GetSoundCheckState"      },
    { 0x03, 0x1E, "SetSoundCheckState"      },
    { 0x03, 0x1F, "GetTrackLengthAndPos"    },
};

/* Extended Interface, whose acknowledgement has a two-byte command ID
 * and so does not fit the table above. Table 5-1 (p.401) marks
 * ResetDBSelectionHierarchy as requiring authentication; it is the only
 * command in that range this firmware implements. */
static void check_l4_gated(unsigned char lo, const char *name)
{
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    device.auth.state = AUST_INIT;
    IAPTEST_RX(0x00, 0x05);                 /* Extended Interface mode */
    iaptest_tx_clear();

    unsigned char pkt[8] = { 0x04, 0x00, lo, 0x01, 0, 0, 0, 0 };
    iaptest_rx(pkt, 4);

    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(r != NULL, "%s was answered with silence before authentication",
          name);
    if (!r || r->paylen < 6)
        return;
    CHECK(r->payload[0] == 0x04 && r->payload[1] == 0x00
          && r->payload[2] == 0x01,
          "%s replied 0x%02X%02X%02X, not an Extended Interface ack",
          name, r->payload[0], r->payload[1], r->payload[2]);
    if (r->payload[2] != 0x01)
        return;
    CHECK(r->payload[3] == IAP_ACK_NO_AUTHEN,
          "%s answered status 0x%02X before authentication; Table 5-1 "
          "(p.401) marks it as requiring it", name, r->payload[3]);
}

void test_auth_gate_extended_interface_database_hierarchy(void)
{
    check_l4_gated(0x3B, "ResetDBSelectionHierarchy");

    /* Authenticated, it goes through. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05);
    iaptest_tx_clear();
    IAPTEST_RX(0x04, 0x00, 0x3B, 0x01);
    {
        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "no reply once authenticated");
        if (r && r->paylen >= 6)
            CHECK(r->payload[3] != IAP_ACK_NO_AUTHEN,
                  "ResetDBSelectionHierarchy still answers Not "
                  "Authenticated after authentication completed");
    }
}

/* The Apple-device acknowledgement for a lingo: General uses its own
 * iPodAck 0x00/0x02, Display Remote uses 0x03/0x00. Both put the status
 * in the first payload byte after the command. */
static bool ack_status(const struct iaptest_pkt *p, unsigned char lingo,
                       unsigned char *status)
{
    if (!p || p->paylen < 3)
        return false;
    if (lingo == 0x00 && p->payload[0] == 0x00 && p->payload[1] == 0x02) {
        *status = p->payload[2];
        return true;
    }
    if (lingo == 0x03 && p->payload[0] == 0x03 && p->payload[1] == 0x00) {
        *status = p->payload[2];
        return true;
    }
    return false;
}

/* MFi Table 3-6 (p.125) status 0x07 is "Not authenticated". A gated
 * command sent before authentication completes must come back with it
 * -- not with success, and not with silence. */
void test_auth_gate_refuses_every_gated_command(void)
{
    for (unsigned i = 0; i < sizeof(gated_cmds)/sizeof(gated_cmds[0]); i++) {
        const struct gated *g = &gated_cmds[i];

        /* A session that has negotiated the lingoes and started, but
         * not finished, authentication. This is the state an accessory
         * is in between IdentifyDeviceLingoes and the last challenge
         * response, and it is reachable with nothing but valid
         * packets. */
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03)
                                | (1u << 0x04));
        device.auth.state = AUST_INIT;
        iaptest_tx_clear();

        /* Generously long so a length check cannot be what refuses it;
         * the payload is zeroes, which every one of these reads as a
         * valid parameter or ignores. */
        unsigned char pkt[24];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = g->lingo;
        pkt[1] = g->cmd;
        iaptest_rx(pkt, sizeof(pkt));

        const struct iaptest_pkt *r = iaptest_tx(0);
        unsigned char st = 0;
        CHECK(r != NULL,
              "%s (lingo 0x%02X command 0x%02X) was answered with "
              "silence before authentication",
              g->name, g->lingo, g->cmd);
        if (r == NULL)
            continue;
        bool is_ack = ack_status(r, g->lingo, &st);
        CHECK(is_ack,
              "%s replied with lingo 0x%02X command 0x%02X, which is "
              "not this lingo's acknowledgement",
              g->name, r->payload[0], r->payload[1]);
        if (!is_ack)
            continue;
        CHECK(st == IAP_ACK_NO_AUTHEN,
              "%s (lingo 0x%02X command 0x%02X) answered status 0x%02X "
              "before authentication; MFi Table 2-7 (p.105) requires it "
              "and Table 3-6 (p.125) status 0x07 says so",
              g->name, g->lingo, g->cmd, st);
    }
}

/* The other half: once authenticated, none of these still answers Not
 * Authenticated. Without this the case above would pass against a
 * handler that refused everything for ever. */
/* The same commands, seen across the transition rather than after it.
 *
 * This used to identify and then call iaptest_force_authenticated(),
 * which does nothing here: IdentifyDeviceLingoes with the options word
 * and device id both zero already sets AUST_AUTH (iap-lingo0.c:762), so
 * the session was authenticated before the helper was called and the
 * case never watched a gate open. Neutering the helper left it
 * byte-identical. And its only assertion sat inside "if (r && ...)", so
 * a command answered with silence passed.
 *
 * Both halves now run against the same command in the same case: refuse
 * while the handshake is unfinished, accept once it is not, with an
 * answer required either way. */
void test_auth_gate_opens_once_authenticated(void)
{
    for (unsigned i = 0; i < sizeof(gated_cmds)/sizeof(gated_cmds[0]); i++) {
        const struct gated *g = &gated_cmds[i];

        unsigned char pkt[24];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = g->lingo;
        pkt[1] = g->cmd;

        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03)
                                | (1u << 0x04));

        /* Mid-handshake: identified, not yet authenticated. */
        device.auth.state = AUST_INIT;
        iaptest_tx_clear();
        iaptest_rx(pkt, sizeof(pkt));
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            CHECK(r != NULL,
                  "%s (lingo 0x%02X command 0x%02X) was answered with "
                  "silence before authentication",
                  g->name, g->lingo, g->cmd);
            if (r && ack_status(r, g->lingo, &st))
                CHECK_EQ_INT(st, IAP_ACK_NO_AUTHEN, g->name);
        }

        /* The same command, the same session, authentication done. */
        device.auth.state = AUST_CERTDONE;
        iaptest_tx_clear();
        iaptest_rx(pkt, sizeof(pkt));
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            if (g->acks_nothing) {
                CHECK(r == NULL,
                      "%s (lingo 0x%02X command 0x%02X) is an "
                      "acknowledgement and must draw no reply once the "
                      "gate is open", g->name, g->lingo, g->cmd);
                continue;
            }
            CHECK(r != NULL,
                  "%s (lingo 0x%02X command 0x%02X) was answered with "
                  "silence after authentication completed",
                  g->name, g->lingo, g->cmd);
            if (r && ack_status(r, g->lingo, &st))
                CHECK(st != IAP_ACK_NO_AUTHEN,
                      "%s (lingo 0x%02X command 0x%02X) still answers Not "
                      "Authenticated after authentication completed",
                      g->name, g->lingo, g->cmd);
        }
    }
}

/* The Microphone lingo has a gate too, and it cannot answer.
 *
 * MFi C.5 (p.533): "the Apple device sends commands to the accessory
 * and the accessory responds with data or AccessoryAck commands", and
 * Table C-12 (p.534) lists one acknowledgement, 0x04 AccessoryAck,
 * Origin: Accessory. So cmd_ack() in iap-lingo1.c is a no-op and the
 * table above cannot reach it -- there is no status byte to read. What
 * an unauthenticated accessory observes instead is that its command
 * drove nothing.
 *
 * The same shape as test_tuner_requires_authentication, and for the
 * same reason: two of the twenty-two gates refuse in silence. */
void test_auth_gate_microphone_refuses_in_silence(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    device.auth.state = AUST_INIT;
    iaptest_tx_clear();

    /* RetAccessoryCaps with both low capability bits set. Authenticated,
     * iap-lingo1.c answers this with SetAccessoryCtrl (0x01/0x0B) to
     * turn on stereo line-in. */
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);

    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2)
            continue;
        CHECK(p->payload[0] != 0x01,
              "an unauthenticated accessory drove Microphone command "
              "0x%02X", p->payload[1]);
    }

    /* Authenticated, the identical packet is honoured -- so the case is
     * watching the gate and not a dead handler. */
    iaptest_force_authenticated();
    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);

    bool saw = false;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01
            && p->payload[1] == 0x0B)
            saw = true;
    }
    CHECK(saw, "an authenticated RetAccessoryCaps drove no SetAccessoryCtrl, "
               "so the case proves nothing about the gate");
}

/* The Microphone lingo's other two guards, both silent for the same
 * reason: it has no Apple-device acknowledgement, so everything it
 * refuses it refuses by doing nothing.
 *
 * What makes them observable is that a RetAccessoryCaps advertising
 * stereo line-in drives SetAccessoryCtrl (MFi C.5.9, p.539) when it is
 * accepted. Its absence is the refusal. */
void test_auth_gate_microphone_length_and_lingo(void)
{
    /* Table C-12 (p.534) gives RetAccessoryCaps four capability bytes,
     * so six with the lingo and command. Send a valid six first, so the
     * five-byte send that follows finds a capability byte with the
     * stereo bits set sitting in the receive buffer -- without the
     * length check the handler reads it and acts on it. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);

    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00);
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01)
            CHECK(p->payload[1] != 0x0B,
                  "a five-byte RetAccessoryCaps drove SetAccessoryCtrl; "
                  "the capability byte came from the previous packet");
    }

    /* And the lingo has to have been declared. MFi 2.3.3 (p.97). */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2)
            CHECK(p->payload[0] != 0x01,
                  "Microphone command 0x%02X went out to an accessory "
                  "that never declared the lingo", p->payload[1]);
    }

    /* iap_record() is the other side of the same gate: the device
     * originates iPodModeChange (C.5.4, p.536) only to an accessory
     * that declared the lingo. */
    iaptest_tx_clear();
    CHECK(iap_record(true) == false,
          "iap_record() reported recording started on an accessory that "
          "never declared the Microphone lingo");
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2)
            CHECK(p->payload[0] != 0x01,
                  "iPodModeChange went out on an undeclared lingo");
    }

    /* Declared, it works -- so the case is watching the gate. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();
    iaptest_tx_clear();
    CHECK(iap_record(true) == true,
          "iap_record() refused an accessory that did declare the lingo");
}

/* The Microphone lingo's own prologue guard.
 *
 * Its mutation survived the sweep, and a review round put that down to
 * the lingo not being compiled for these targets. It is: HAVE_LINE_REC
 * is derived in firmware/export/config_caps.h from REC_SRC_CAPS rather
 * than set in the target config, both targets have SRC_CAP_LINEIN, and
 * iap_handlepkt_mode1 is in both linked binaries and dispatched from
 * iap-core.c. So the guard is live and was simply untested.
 *
 * MFi C.5 (p.533) gives this lingo no Apple-device acknowledgement, so
 * what a refusal looks like is silence -- the same shape as the tuner
 * and the same reason.
 *
 * What this pins is case 0x08's own length check. The prologue
 * CHECKLEN(4) turns out to be subsumed by it: every command re-checks
 * in parameter units before reading, so a three-byte packet is refused
 * either way and that mutation stays alive. Marked as equivalent in the
 * handler. */
void test_auth_gate_microphone_prologue_length(void)
{
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x01));
    iaptest_force_authenticated();

    /* A valid RetAccessoryCaps first: it proves the path works, and it
     * leaves a capability word with the stereo bits set in the receive
     * buffer, so an unguarded handler reading past a short packet finds
     * something it will act on. */
    iaptest_tx_clear();
    IAPTEST_RX(0x01, 0x08, 0x00, 0x00, 0x00, 0x03);

    int drove = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01
            && p->payload[1] == 0x0B)
            drove++;
    }
    CHECK(drove == 1,
          "a valid RetAccessoryCaps drove SetAccessoryCtrl %d times, so "
          "the short packet below would prove nothing", drove);

    /* Three bytes: one short of the four this lingo's commands need. */
    iaptest_tx_clear();
    {
        unsigned char p[3] = { 0x01, 0x08, 0x00 };
        iaptest_rx(p, sizeof(p));
    }

    drove = 0;
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 2 && p->payload[0] == 0x01
            && p->payload[1] == 0x0B)
            drove++;
    }
    CHECK(drove == 0,
          "a three-byte Microphone command drove SetAccessoryCtrl %d "
          "times; the capability byte came from past the end of it",
          drove);
}

/* The Simple Remote lingo has three gates of its own.
 *
 * Commands 0x02, 0x03 and 0x04 each write the test out --
 * "if (!DEVICE_AUTHENTICATED) { cmd_ack(cmd, IAP_ACK_NO_AUTHEN); }" --
 * rather than using the CHECKAUTH macro the table above sweeps. So none
 * of the three was covered, and each could be turned off with the whole
 * suite green. The mutation sweep could not see them either until it
 * learned the open-coded spelling.
 *
 * ContextButtonStatus (0x00) is deliberately not here: MFi 4.2.7
 * (p.226) says it is never answered, and Table 2-7 exempts it from
 * authentication on UART so a remote powered before Rockbox still
 * works. */
void test_auth_gate_simple_remote_commands_are_gated(void)
{
    static const struct { unsigned char cmd; const char *name; } l2[] = {
        { 0x02, "GetAccessoryInfo (0x02)"  },
        { 0x03, "RetAccessoryInfo (0x03)"  },
        { 0x04, "AudioButtonStatus (0x04)" },
    };

    for (unsigned i = 0; i < sizeof(l2)/sizeof(l2[0]); i++) {
        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02));

        /* Mid-handshake: identified, not yet authenticated. */
        device.auth.state = AUST_INIT;
        iaptest_tx_clear();
        {
            unsigned char p[8] = { 0x02, l2[i].cmd, 0x00, 0x30,
                                   0x00, 0x00, 0x00, 0x00 };
            iaptest_rx(p, sizeof(p));
        }

        const struct iaptest_pkt *r = iaptest_tx(0);
        CHECK(r != NULL, "%s drew no reply before authentication",
              l2[i].name);
        if (r && r->paylen >= 4) {
            CHECK(r->payload[0] == 0x02 && r->payload[1] == 0x01,
                  "%s: the refusal must be a Simple Remote ack",
                  l2[i].name);
            CHECK_EQ_INT(r->payload[r->paylen - 2], IAP_ACK_NO_AUTHEN,
                         l2[i].name);
        }

        /* And it opens. 0x04 is the one with a body; the other two are
         * refused as Command Failed once past the gate, which is still
         * a different answer from Not Authenticated. */
        device.auth.state = AUST_CERTDONE;
        iaptest_button_sample(4);
        iaptest_tx_clear();
        {
            unsigned char p[8] = { 0x02, l2[i].cmd, 0x00, 0x31,
                                   0x00, 0x00, 0x00, 0x00 };
            iaptest_rx(p, sizeof(p));
        }
        {
            const struct iaptest_pkt *q = iaptest_tx(0);
            if (q && q->paylen >= 4 && q->payload[0] == 0x02
                && q->payload[1] == 0x01)
                CHECK(q->payload[q->paylen - 2] != IAP_ACK_NO_AUTHEN,
                      "%s still answers Not Authenticated after the "
                      "handshake completed", l2[i].name);
        }
    }
}
