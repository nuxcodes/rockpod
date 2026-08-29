/***************************************************************************
 * iAP conformance tests: the length checks, driven so they are load-bearing
 *
 * Every handler indexes buf[] through a doff offset that is 2 under IDPS
 * and 0 otherwise, guarded by a CHECKLEN that has to be adjusted to
 * match. Getting one wrong reads past the packet. Thirty-seven of those
 * checks could be deleted one at a time with all four binaries staying
 * green.
 *
 * They were dark for a specific reason. Sending a short packet and
 * asserting a refusal proves nothing, because the bytes past the end are
 * whatever the receive buffer happens to hold -- zeroes, or the 0xA5 the
 * malformed sweep primes -- and those are invalid parameter values for
 * these commands. The handler refuses at the category or index check
 * instead of at the length check, and the assertion cannot tell the two
 * apart.
 *
 * So each case here sends the command twice: once complete and valid,
 * and then again one byte shorter. The first send leaves its own bytes
 * in the receive buffer, so a handler with no length check reads a
 * perfectly valid parameter off the end of the second one and answers
 * Success. The only thing that can refuse it is the length check.
 *
 * Both halves are asserted. If the full-length send is ever refused the
 * case says so, because a command that fails for its parameters would
 * make the short-packet half pass for the wrong reason.
 ****************************************************************************/
#include <string.h>
#include "iap_test.h"
#include "button.h"
#include "iap-core.h"

struct lencase {
    const char   *name;
    unsigned char pkt[16];
    int           len;      /* the complete, valid length */
};

/* Extended Interface. The command ID is two bytes (MFi 5.1, p.400), so a
 * CHECKLEN(n + doff) leaves n - 3 parameter bytes. */
static const struct lencase l4_cases[] = {
    { "SelectDBRecord",
      { 0x04, 0x00, 0x17, 0x02, 0x00, 0x00, 0x00, 0x00 }, 8 },
    { "GetNumberCategorizedDBRecords",
      { 0x04, 0x00, 0x18, 0x02 }, 4 },
    { "RetrieveCategorizedDatabaseRecords",
      { 0x04, 0x00, 0x1A, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01 }, 12 },
    { "GetIndexedPlayingTrackAlbumName",
      { 0x04, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00 }, 7 },
    { "SetPlayStatusChangeNotification",
      { 0x04, 0x00, 0x26, 0x00 }, 4 },
    { "PlayCurrentSelection",
      { 0x04, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00 }, 7 },
    { "PlayControl",
      { 0x04, 0x00, 0x29, 0x01 }, 4 },
    { "SetShuffle",
      { 0x04, 0x00, 0x2E, 0x00 }, 4 },
    { "SetRepeat",
      { 0x04, 0x00, 0x31, 0x00 }, 4 },
    { "SetCurrentPlayingTrack",
      { 0x04, 0x00, 0x37, 0x00, 0x00, 0x00, 0x00 }, 7 },
    { "SelectSortDBRecord",
      { 0x04, 0x00, 0x38, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 }, 9 },
    { "ResetDBSelectionHierarchy",
      { 0x04, 0x00, 0x3B, 0x01 }, 4 },
};

/* The Extended Interface acknowledgement is 0x04 0x0001: status byte
 * then the two-byte command being acknowledged (Table 5-3, p.403). */
static bool l4_status(const struct iaptest_pkt *p, unsigned char *st)
{
    if (!p || p->paylen < 6)
        return false;
    if (p->payload[0] != 0x04 || p->payload[1] != 0x00
        || p->payload[2] != 0x01)
        return false;
    *st = p->payload[3];
    return true;
}

/* A populated queue, so the parameters in the table are genuinely
 * valid. On an empty playlist every index is out of range and the
 * full-length send is refused for its parameters, which would leave the
 * short-packet half proving nothing. */
static void l4_bringup(void)
{
    /* The runner calls iaptest_init() before each case; a loop that
     * drives many commands inside one case has to do it per iteration,
     * or the second command runs against the first one's leftovers. */
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    iaptest_force_authenticated();
    /* Extended Interface mode. Without it every command below is
     * refused whatever its length, which would make the short-packet
     * half meaningless. */
    IAPTEST_RX(0x00, 0x05);
    rbstub_set_playlist(20, 3);
}

void test_lengths_extended_interface_rejects_one_byte_short(void)
{
    for (unsigned i = 0; i < sizeof(l4_cases)/sizeof(l4_cases[0]); i++) {
        const struct lencase *c = &l4_cases[i];

        l4_bringup();

        /* Complete and valid. This both proves the parameters are
         * acceptable and leaves them in the receive buffer. */
        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            if (r && l4_status(r, &st))
                CHECK(st != IAP_ACK_BAD_PARAM,
                      "%s was refused at its full length of %d with a "
                      "parameter error, so the short-packet half below "
                      "would pass for the wrong reason",
                      c->name, c->len);
        }

        /* Let any remote button the command raised go out. While
         * iap_repeatbtn is set, iap_handlepkt() re-queues the next
         * packet instead of handling it (iap-core.c), so a PlayControl
         * would otherwise swallow the send that follows it. */
        for (int t = 0; t < 4; t++)
            iap_periodic();

        /* One byte short. The byte the handler would read off the end
         * is the one the send above left there, so it is valid --
         * nothing but the length check can refuse this. */
        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len - 1);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            CHECK(r != NULL,
                  "%s one byte short was answered with silence", c->name);
            if (!r)
                continue;
            bool ok = l4_status(r, &st);
            CHECK(ok, "%s one byte short was not answered with an "
                      "Extended Interface acknowledgement", c->name);
            if (!ok)
                continue;
            CHECK(st == IAP_ACK_BAD_PARAM,
                  "%s at %d bytes, one short of %d, answered status "
                  "0x%02X -- the missing byte was read from the previous "
                  "packet's residue",
                  c->name, c->len - 1, c->len, st);
        }
    }
}

/* The same, under IDPS. MFi 2.6.1.4 (p.112): "every packet length byte
 * must be increased by 2", so each check must widen by the same two
 * bytes the transaction ID adds. A CHECKLEN that forgot the doff term
 * lets a packet two bytes short of its parameters through. */
void test_lengths_extended_interface_rejects_short_under_idps(void)
{
    for (unsigned i = 0; i < sizeof(l4_cases)/sizeof(l4_cases[0]); i++) {
        const struct lencase *c = &l4_cases[i];

        iaptest_init();
        iaptest_enter_idps();
        iaptest_force_authenticated();
        device.lingoes |= (1u << 0x04);
        IAPTEST_RX(0x00, 0x05, 0x00, 0xC0);
        rbstub_set_playlist(20, 3);

        /* lingo, command id, transaction id, then the parameters. */
        unsigned char pkt[18];
        pkt[0] = c->pkt[0];
        pkt[1] = c->pkt[1];
        pkt[2] = c->pkt[2];
        pkt[3] = 0x00;
        pkt[4] = (unsigned char)(0x40 + i);
        memcpy(&pkt[5], &c->pkt[3], c->len - 3);
        int full = c->len + 2;

        iaptest_tx_clear();
        iaptest_rx(pkt, full);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            if (r && r->paylen >= 8 && r->payload[0] == 0x04
                && r->payload[1] == 0x00 && r->payload[2] == 0x01)
            {
                st = r->payload[5];
                CHECK(st != IAP_ACK_BAD_PARAM,
                      "%s under IDPS was refused at its full length of %d",
                      c->name, full);
            }
        }

        for (int t = 0; t < 4; t++)
            iap_periodic();
        iaptest_tx_clear();
        iaptest_rx(pkt, full - 1);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            CHECK(r != NULL,
                  "%s one byte short under IDPS was answered with silence",
                  c->name);
            if (r && r->paylen >= 8 && r->payload[0] == 0x04
                && r->payload[1] == 0x00 && r->payload[2] == 0x01)
            {
                CHECK(r->payload[5] == IAP_ACK_BAD_PARAM,
                      "%s at %d bytes under IDPS, one short of %d, "
                      "answered status 0x%02X",
                      c->name, full - 1, full, r->payload[5]);
            }
        }
    }
}

/* MFi 2.3.3 (p.97) has the accessory declare the lingoes it will use.
 *
 * Two guards implement that for Extended Interface, and only one of
 * them can ever fire. iap-lingo0.c refuses to enter the mode without
 * the lingo, and iap_handlepkt_mode4() then refuses every command
 * outside the mode -- so DEVICE_LINGO_SUPPORTED(0x04) in the handler
 * is unreachable, and deleting it changes nothing observable. That is
 * an equivalent mutant, not a coverage gap: it stays because it is the
 * right place for the check if a future path ever sets the mode
 * without the lingo, and the guard doing the real work
 * (iap-lingo0.c, entering the mode) fails 52 checks when removed.
 *
 * What this case pins down is the behaviour an accessory sees: every
 * Extended Interface command refused when it never declared the
 * lingo. */
void test_lengths_extended_interface_needs_the_lingo(void)
{
    for (unsigned i = 0; i < sizeof(l4_cases)/sizeof(l4_cases[0]); i++) {
        const struct lencase *c = &l4_cases[i];

        /* Everything except lingo 0x04. */
        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
        iaptest_force_authenticated();
        /* Ask for Extended Interface mode anyway. It is refused --
         * iap-lingo0.c will not enter the mode for an accessory that
         * did not declare the lingo, and that refusal is what this
         * case actually pins down. */
        IAPTEST_RX(0x00, 0x05);
        rbstub_set_playlist(20, 3);
        iaptest_tx_clear();

        iaptest_rx(c->pkt, c->len);

        const struct iaptest_pkt *r = iaptest_tx(0);
        unsigned char st = 0;
        CHECK(r != NULL,
              "%s was answered with silence on a lingo the accessory "
              "never declared", c->name);
        if (!r)
            continue;
        bool ok = l4_status(r, &st);
        CHECK(ok, "%s on an undeclared lingo was not answered with an "
                  "Extended Interface acknowledgement", c->name);
        if (ok)
            CHECK(st == IAP_ACK_BAD_PARAM,
                  "%s answered status 0x%02X on a lingo the accessory "
                  "never declared", c->name, st);
    }
}

/* Display Remote. One-byte command ID, so a CHECKLEN(n + doff) leaves
 * n - 2 parameter bytes. */
static const struct lencase l3_cases[] = {
    { "GetiPodStateInfo",
      { 0x03, 0x0C, 0x00 }, 3 },
    /* SetiPodStateInfo's own L3_NEED(1) is not here, because on length
     * alone it is invisible: it guarantees the information-type byte
     * exists, but every type then demands more than three bytes, so a
     * three-byte packet is refused by the type's own check either way.
     * What it does decide is the order -- it runs ahead of CHECKAUTH,
     * so a packet too short to name a type is refused for being short
     * rather than for being unauthenticated. That is what
     * test_lengths_stateinfo_length_is_checked_before_auth drives. The
     * per-type checks are driven by
     * test_lengths_stateinfo_rejects_one_byte_short below. */
    { "SetRemoteEventNotification",
      { 0x03, 0x08, 0x00, 0x00, 0x00, 0x00 }, 6 },
    { "GetIndexedPlayingTrackInfo",
      { 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }, 7 },
    { "SetPlayStatusChangeNotification",
      { 0x03, 0x06, 0x00, 0x00, 0x00, 0x00 }, 6 },
    { "SetPlayStatusChangeNotification4",
      { 0x03, 0x11, 0x00, 0x00, 0x00, 0x00 }, 6 },
    { "SetSoundCheckState",
      { 0x03, 0x1E, 0x00, 0x00 }, 4 },

    /* Both of these were invisible to the sweep until their length
     * checks were converted out of the hex spelling -- mutate.py's rule
     * only matched the decimal one, so nothing had ever asked whether
     * they were load-bearing. Neither was. */
    { "GetIndexedPlayingTrackInfo",
      { 0x03, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 9 },
    { "GetTrackArtworkTimes",
      { 0x03, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04,
        0x00, 0x00, 0x00, 0x01 }, 12 },
};

/* SetiPodStateInfo carries its own length per information type
 * (Table 4-74, pp.266-269), each with its own CHECKLEN inside the
 * command. The packet is lingo, command, type, then the payload. */
static const struct lencase l3_stateinfo[] = {
    { "SetiPodStateInfo(0x00)", { 0x03, 0x0E, 0x00, 0,0,0,0 }, 7 },
    { "SetiPodStateInfo(0x01)", { 0x03, 0x0E, 0x01, 0,0,0,0 }, 7 },
    { "SetiPodStateInfo(0x03)", { 0x03, 0x0E, 0x03, 0 },       4 },
    { "SetiPodStateInfo(0x04)", { 0x03, 0x0E, 0x04, 0,0 },     5 },
    { "SetiPodStateInfo(0x06)", { 0x03, 0x0E, 0x06, 0,0,0,0,0 },8 },
    { "SetiPodStateInfo(0x07)", { 0x03, 0x0E, 0x07, 0,0 },     5 },
    { "SetiPodStateInfo(0x08)", { 0x03, 0x0E, 0x08, 0,0 },     5 },
    { "SetiPodStateInfo(0x09)", { 0x03, 0x0E, 0x09, 0,0,0,0,0,0 }, 9 },
    { "SetiPodStateInfo(0x0A)", { 0x03, 0x0E, 0x0A, 0,0,0,0 }, 7 },
    { "SetiPodStateInfo(0x0B)", { 0x03, 0x0E, 0x0B, 0,0 },     5 },
    { "SetiPodStateInfo(0x0D)", { 0x03, 0x0E, 0x0D, 0,0 },     5 },
    /* Two data bytes -- speed, then bRestoreOnExit -- per Table 4-74
     * (p.269). This row said one, matching a stale "Data length: 1"
     * comment in iap-lingo3.c rather than the table, and the two agreed
     * with each other for as long as neither was checked. */
    { "SetiPodStateInfo(0x0E)", { 0x03, 0x0E, 0x0E, 0,0 },     5 },
    { "SetiPodStateInfo(0x0F)", { 0x03, 0x0E, 0x0F, 0,0 },     5 },
    { "SetiPodStateInfo(0x10)", { 0x03, 0x0E, 0x10, 0,0,0,0 }, 7 },
};

/* The Display Remote acknowledgement is 0x03 0x00: status then the
 * command being acknowledged (Table 4-49, p.250). */
static bool l3_status(const struct iaptest_pkt *p, unsigned char *st)
{
    if (!p || p->paylen < 4)
        return false;
    if (p->payload[0] != 0x03 || p->payload[1] != 0x00)
        return false;
    *st = p->payload[2];
    return true;
}

static void l3_run(const struct lencase *tbl, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const struct lencase *c = &tbl[i];

        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03));
        iaptest_force_authenticated();
        rbstub_set_playlist(20, 3);

        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            if (r && l3_status(r, &st))
                CHECK(st != IAP_ACK_BAD_PARAM,
                      "%s was refused at its full length of %d with a "
                      "parameter error, so the short-packet half below "
                      "would pass for the wrong reason", c->name, c->len);
        }

        for (int t = 0; t < 4; t++)
            iap_periodic();

        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len - 1);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            CHECK(r != NULL, "%s one byte short was answered with silence",
                  c->name);
            if (!r)
                continue;
            bool ok = l3_status(r, &st);
            CHECK(ok, "%s one byte short was not answered with a Display "
                      "Remote acknowledgement", c->name);
            if (ok)
                CHECK(st == IAP_ACK_BAD_PARAM,
                      "%s at %d bytes, one short of %d, answered status "
                      "0x%02X -- the missing byte was read from the "
                      "previous packet's residue",
                      c->name, c->len - 1, c->len, st);
        }
    }
}

void test_lengths_display_remote_rejects_one_byte_short(void)
{
    l3_run(l3_cases, sizeof(l3_cases)/sizeof(l3_cases[0]));
}

void test_lengths_stateinfo_rejects_one_byte_short(void)
{
    l3_run(l3_stateinfo, sizeof(l3_stateinfo)/sizeof(l3_stateinfo[0]));
}

/* General lingo. One-byte command ID like Display Remote, but the
 * length floor is written L0_MINLEN(n), which folds the transaction-ID
 * offset in for itself. */
static const struct lencase l0_cases[] = {
    /* RetAccessoryInfo. Information type 0x01 falls to the default
     * branch, which is deliberately silent -- so with the length check
     * the truncated packet gets a Bad Parameter ack, and without it the
     * handler reads the type off the residue and says nothing at all.
     * Type 0x00 would not do: it has its own CHECKLEN(7 + off), which
     * refuses the short packet either way. */
    { "RetAccessoryInfo",       { 0x00, 0x28, 0x01 },             3 },
    /* Four of these five were named after the wrong command. The
     * packets were always right, so the checks they drive are the ones
     * meant -- but 0x29 is GetiPodPreferences (MFi 3.3.34, p.151), not
     * SetEventNotification; 0x2B is SetiPodPreferences (3.3.36, p.157),
     * not GetSupportedEventNotification; 0x37 is SetUIMode (3.3.39,
     * p.159), not SetAvailableCurrent; and 0x4B is
     * GetiPodOptionsForLingo (3.3.55, p.191), not whatever "GetUIMode2"
     * was meant to be. The same mislabelling was found in
     * test_auth_gate.c's table, and a wrong name in a table is what a
     * reader takes for coverage they have. */
    { "GetiPodPreferences",     { 0x00, 0x29, 0x00 },             3 },
    { "SetiPodPreferences",     { 0x00, 0x2B, 0x00, 0x00, 0x00 }, 5 },
    { "SetUIMode",              { 0x00, 0x37, 0x00 },             3 },
    { "GetiPodOptionsForLingo", { 0x00, 0x4B, 0x00 },             3 },
};

/* The General iPodAck is 0x00 0x02: status then the acknowledged
 * command (Table 3-6, p.125). */
static bool l0_status(const struct iaptest_pkt *p, unsigned char *st)
{
    if (!p || p->paylen < 4)
        return false;
    if (p->payload[0] != 0x00 || p->payload[1] != 0x02)
        return false;
    *st = p->payload[2];
    return true;
}

void test_lengths_general_lingo_rejects_one_byte_short(void)
{
    for (unsigned i = 0; i < sizeof(l0_cases)/sizeof(l0_cases[0]); i++) {
        const struct lencase *c = &l0_cases[i];

        iaptest_init();
        iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03)
                                | (1u << 0x04));
        iaptest_force_authenticated();
        rbstub_set_playlist(20, 3);

        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            if (r && l0_status(r, &st))
                CHECK(st != IAP_ACK_BAD_PARAM,
                      "%s was refused at its full length of %d with a "
                      "parameter error, so the short-packet half below "
                      "would pass for the wrong reason", c->name, c->len);
        }

        for (int t = 0; t < 4; t++)
            iap_periodic();

        iaptest_tx_clear();
        iaptest_rx(c->pkt, c->len - 1);
        {
            const struct iaptest_pkt *r = iaptest_tx(0);
            unsigned char st = 0;
            CHECK(r != NULL, "%s one byte short was answered with silence",
                  c->name);
            if (!r)
                continue;
            bool ok = l0_status(r, &st);
            CHECK(ok, "%s one byte short was not answered with a General "
                      "iPodAck", c->name);
            if (ok)
                CHECK(st == IAP_ACK_BAD_PARAM,
                      "%s at %d bytes, one short of %d, answered status "
                      "0x%02X -- the missing byte was read from the "
                      "previous packet's residue",
                      c->name, c->len - 1, c->len, st);
        }
    }
}

/* GetTransportMaxPayloadSize (0x11) has a length floor of two, which
 * the framer already guarantees -- MFi 2.5.2 (p.110) puts the smallest
 * payload at 0x02 -- so on a legacy session the check can never fire.
 * Under IDPS the floor becomes four, and a packet carrying only part of
 * its transaction ID is a real packet the framer will deliver.
 *
 * That is the whole point of L0_MINLEN: the same command has two
 * lengths depending on whether transaction IDs are in use, and 3ab3ac09
 * fixed 0x4B for it while missing 0x11.
 *
 * The reply is RetTransportMaxPayloadSize (0x12), not an ack, so what
 * is asserted is that a short packet does not produce one. */
void test_lengths_transport_payload_size_under_idps(void)
{
    iaptest_init();
    iaptest_enter_idps();
    iaptest_force_authenticated();

    /* Complete: command plus its two transaction-ID bytes. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x11, 0x00, 0x51);
    {
        bool saw = false;
        for (int i = 0; i < iaptest_tx_count(); i++) {
            const struct iaptest_pkt *p = iaptest_tx(i);
            if (p && p->paylen >= 2 && p->payload[0] == 0x00
                && p->payload[1] == 0x12)
                saw = true;
        }
        CHECK(saw, "a complete GetTransportMaxPayloadSize under IDPS was "
                   "not answered with RetTransportMaxPayloadSize");
    }

    /* One byte short: half a transaction ID. Echoing an ID assembled
     * from one packet byte and one byte of the last packet's residue
     * is exactly what MFi 2.6.1.1 (p.111) has the accessory discard. */
    iaptest_tx_clear();
    IAPTEST_RX(0x00, 0x11, 0x00);
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (!p || p->paylen < 2)
            continue;
        CHECK(!(p->payload[0] == 0x00 && p->payload[1] == 0x12),
              "a GetTransportMaxPayloadSize one byte short of its "
              "transaction ID was answered anyway, with an ID half read "
              "from the previous packet");
    }
}

/* The absolute length checks -- the prologue guards that bound a
 * handler before it knows its transaction-ID offset.
 *
 * No mutation rule matched these until the sweep began auditing its own
 * coverage, so nothing had ever asked whether they held. Each needs its
 * own primer: the bytes past a short packet have to be *valid* for the
 * handler being probed, or it refuses them for a parameter reason and
 * the length check cannot be told apart from its absence. One shared
 * primer served none of them -- it made the button case work and broke
 * the identify case, and vice versa.
 */
static void probe_prologue(const unsigned char *primer, int plen,
                           const unsigned char *pkt, int len,
                           const char *name, bool expect_zero_cmd)
{
    iaptest_init();
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03)
                            | (1u << 0x04));
    iaptest_force_authenticated();
    IAPTEST_RX(0x00, 0x05);                 /* Extended Interface mode */
    iaptest_button_sample(4);

    iaptest_rx(primer, plen);
    iaptest_button_sample(4);
    iaptest_tx_clear();
    rbstub_reset_calls();

    iaptest_rx(pkt, len);

    CHECK(rbstub_calls.skip == 0, "%s moved playback", name);

    const struct iaptest_pkt *r = iaptest_tx(0);
    if (r && r->paylen >= 4 && r->payload[0] == 0x00
        && r->payload[1] == 0x02)
        CHECK(r->payload[2] != 0x00, "%s was acked Success", name);

    if (expect_zero_cmd && r && r->paylen >= 6 && r->payload[0] == 0x04
        && r->payload[1] == 0x00 && r->payload[2] == 0x01)
        /* 0x0000 is the only honest answer for a packet too short to
         * say what command it was; anything else means the two-byte ID
         * was assembled from a byte the packet did not carry. */
        CHECK(r->payload[4] == 0x00 && r->payload[5] == 0x00,
              "%s was refused naming command 0x%02X%02X, read from past "
              "the end of the packet", name, r->payload[4], r->payload[5]);
}

void test_lengths_prologue_guards(void)
{
    /* Extended Interface: the residue is a real command ID, so an
     * unguarded handler assembles 0x0029 and acts on it. */
    {
        unsigned char primer[6] = { 0x04, 0x00, 0x29, 0x01, 0x00, 0x00 };
        unsigned char pkt[2]    = { 0x04, 0x00 };
        probe_prologue(primer, sizeof(primer), pkt, sizeof(pkt),
                       "Extended Interface, 2 bytes", true);
    }

    /* Simple Remote's prologue guards are not probed here, but they are
     * covered -- see test_buttons_legacy_short_packet_is_not_read_past
     * and its IDPS pair.
     *
     * They do not fit this helper. The residue a short packet reads is
     * the previous packet's button state, so the primer has to leave a
     * button set at that byte -- and a button set is a button held,
     * which iap_repeatbtn then makes the handler defer rather than
     * process. Releasing it means sending all-zero state at the same
     * byte, which is the residue. Same byte, two values, no ordering
     * satisfies both.
     *
     * The cases in test_buttons.c get out of it by driving
     * AudioButtonStatus (0x04) instead of ContextButtonStatus (0x00):
     * it runs the same three prologue guards, shares the same decode,
     * has no length check of its own, and -- unlike 0x00, which MFi
     * 4.2.7 (p.226) says is never answered -- leaves an ack behind, so
     * the button state does not have to be the observable.
     */

    /* IdentifyDeviceLingoes: the residue is a valid lingo mask with a
     * device ID and no authentication requested, which an unguarded
     * handler refuses with CMD_FAILED rather than BAD_PARAM. */
    {
        unsigned char primer[14] = { 0x00, 0x13,
                                     0x00, 0x00, 0x00, 0x1D,
                                     0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00 };
        unsigned char pkt[4]     = { 0x00, 0x13, 0x00, 0x00 };
        probe_prologue(primer, sizeof(primer), pkt, sizeof(pkt),
                       "IdentifyDeviceLingoes, 4 bytes", false);
    }
}

/* Find a General iPodAck, tolerating the IDPS transaction ID. Table 3-6
 * (p.125) is status then the acknowledged command; under IDPS the ID
 * comes first, so the pair sits at the end either way. */
static bool l0_refused(unsigned char cmd, unsigned char status)
{
    for (int i = 0; i < iaptest_tx_count(); i++) {
        const struct iaptest_pkt *p = iaptest_tx(i);
        if (p && p->paylen >= 4 && p->payload[0] == 0x00
            && p->payload[1] == 0x02
            && p->payload[p->paylen - 2] == status
            && p->payload[p->paylen - 1] == cmd)
            return true;
    }
    return false;
}

/* Identify (0x01) resets the device and grants access before it does
 * anything else, so it is the one General command where completing a
 * truncated packet costs the whole session. */
void test_lengths_short_identify_does_not_reset_the_session(void)
{
    iaptest_enter_idps();
    iaptest_force_authenticated();

    CHECK(device.lingoes & (1u << 0x04),
          "the harness must have negotiated Extended Interface, or the "
          "assertion below would hold for the wrong reason");
    uint32_t before = device.lingoes;

    /* Two bytes: no lingo byte at all. Identify's own reset is the
     * observable -- it drops every negotiated lingo and re-grants a
     * bare pair, so a handler that runs it on a packet this short
     * silently disconnects a working accessory. */
    {
        unsigned char p[2] = { 0x00, 0x01 };
        iaptest_rx(p, sizeof(p));
    }

    CHECK_EQ_INT(device.lingoes, before,
                 "a two-byte Identify renegotiated the session's "
                 "lingoes from a byte the packet did not carry");
    CHECK(DEVICE_AUTHENTICATED,
          "a two-byte Identify reset the authentication state");
}

/* DevAuthenticationInfo (0x15) reads a two-byte version out of the
 * packet and acts on it. Its length checks are the only thing standing
 * between a truncated one and a version assembled from the packet
 * before it -- and a version that reads as supported moves the
 * authentication state machine on a packet that carried nothing. */
void test_lengths_short_auth_info_is_refused(void)
{
    /* Legacy first: the version sits at buf[2..3]. */
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));
    device.auth.state = AUST_CERTREQ;    /* DEVICE_AUTH_RUNNING */

    /* A complete one, so the bytes past the short packet below are a
     * supported version rather than zeroes. */
    IAPTEST_RX(0x00, 0x15, 0x01, 0x00, 0x00, 0x01, 0x00);

    device.auth.state = AUST_CERTREQ;
    iaptest_tx_clear();

    /* Three bytes: half a version. */
    {
        unsigned char p[3] = { 0x00, 0x15, 0x01 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK(l0_refused(0x15, IAP_ACK_BAD_PARAM),
          "a three-byte DevAuthenticationInfo was not refused, so its "
          "version came from the packet before it");
    CHECK_EQ_INT(device.auth.state, AUST_CERTREQ,
                 "a three-byte DevAuthenticationInfo moved the "
                 "authentication state machine");

    /* Under IDPS the transaction ID pushes the version to buf[4..5],
     * and a second check covers it. */
    iaptest_enter_idps();
    device.auth.state = AUST_CERTREQ;
    IAPTEST_RX(0x00, 0x15, 0x00, 0x21, 0x01, 0x00, 0x00, 0x01, 0x00);

    device.auth.state = AUST_CERTREQ;
    iaptest_tx_clear();

    /* Five bytes: the transaction ID is whole, the version is not. */
    {
        unsigned char p[5] = { 0x00, 0x15, 0x00, 0x22, 0x01 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK(l0_refused(0x15, IAP_ACK_BAD_PARAM),
          "a five-byte DevAuthenticationInfo was not refused under IDPS");
    CHECK_EQ_INT(device.auth.state, AUST_CERTREQ,
                 "a five-byte DevAuthenticationInfo moved the "
                 "authentication state machine");
}

/* SetiPodStateInfo checks that the information-type byte is there
 * before it checks authentication. Reversing them answers a malformed
 * packet with Not Authenticated, which is both the wrong reason and a
 * statement about session state to a peer that has not earned one. */
void test_lengths_stateinfo_length_is_checked_before_auth(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x03));
    device.auth.state = AUST_NONE;      /* not authenticated */
    CHECK(!DEVICE_AUTHENTICATED, "the case needs an unauthenticated peer");

    iaptest_tx_clear();
    {
        unsigned char p[2] = { 0x03, 0x0E };
        iaptest_rx(p, sizeof(p));
    }

    unsigned char st = 0xAA;
    const struct iaptest_pkt *r = iaptest_tx(0);
    CHECK(l3_status(r, &st),
          "a two-byte SetiPodStateInfo drew no Display Remote ack");
    CHECK_EQ_INT(st, IAP_ACK_BAD_PARAM,
                 "a packet with no information-type byte was refused "
                 "for the wrong reason");
}

/* The version 2.00 branch of DevAuthenticationInfo reads a section
 * index and a section count and walks the certificate state machine on
 * them. Its own length check is separate from the one covered above:
 * that one guards the version field, this one guards what follows it. */
void test_lengths_short_auth_cert_section_is_refused(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x04));

    /* A complete one first: version 0x0200, section 0 of 0, one byte of
     * certificate. It walks the machine to AUST_CERTALLRECEIVED, which
     * is what the short packet below must not be able to do. */
    device.auth.state = AUST_CERTREQ;
    device.auth.next_section = 0;
    IAPTEST_RX(0x00, 0x15, 0x02, 0x00, 0x00, 0x00, 0xAA);
    /* The last section takes it to AUST_CERTALLRECEIVED and the reply
     * carries it on to AUST_CERTDONE, so assert the machine moved on
     * rather than the exact state it stopped at. */
    CHECK(device.auth.state >= AUST_CERTALLRECEIVED,
          "the full-length certificate section did not complete (state "
          "%d), so the short one below would prove nothing",
          (int)device.auth.state);

    device.auth.state = AUST_CERTREQ;
    device.auth.next_section = 0;
    iaptest_tx_clear();

    /* Six bytes: the section index and count are there, the
     * certificate data they describe is not. */
    {
        unsigned char p[6] = { 0x00, 0x15, 0x02, 0x00, 0x00, 0x00 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(device.auth.state, AUST_CERTREQ,
                 "a certificate section with no certificate in it "
                 "advanced the authentication state machine");
    CHECK(l0_refused(0x15, IAP_ACK_BAD_PARAM),
          "the short certificate section was not refused");
}

/* RetAccessoryInfo information type 0x00 carries a four-byte capability
 * word that drives the whole capability sweep. Its own length check is
 * why the general RetAccessoryInfo case above has to use type 0x01. */
void test_lengths_short_accessory_caps_is_refused(void)
{
    iaptest_identify_legacy((1u << 0x00) | (1u << 0x02) | (1u << 0x03)
                            | (1u << 0x04));
    iaptest_force_authenticated();

    /* A complete one, with a capability word whose bytes are all
     * distinct so the short packet below cannot read the same value off
     * the residue by accident. */
    IAPTEST_RX(0x00, 0x28, 0x00, 0x11, 0x22, 0x33, 0x44);
    CHECK_EQ_INT(device.capabilities, 0x11223344,
                 "the full-length RetAccessoryInfo did not record its "
                 "capability word, so the short one below would prove "
                 "nothing");

    iaptest_tx_clear();

    /* Six bytes: three of the four capability bytes. An unguarded read
     * takes the fourth from the packet before it. */
    {
        unsigned char p[6] = { 0x00, 0x28, 0x00, 0x55, 0x66, 0x77 };
        iaptest_rx(p, sizeof(p));
    }
    CHECK_EQ_INT(device.capabilities, 0x11223344,
                 "a six-byte RetAccessoryInfo rewrote the capability "
                 "word, so its last byte came from past the end");
    CHECK(l0_refused(0x28, IAP_ACK_BAD_PARAM),
          "the six-byte RetAccessoryInfo was not refused");
}
