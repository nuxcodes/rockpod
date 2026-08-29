/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Alan Korr & Nick Robinson
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "iap-core.h"
#include "iap-artwork.h"
#include "iap-lingo.h"
#include "kernel.h"
#include "system.h"
#include "tuner.h"
#if CONFIG_TUNER
#include "ipod_remote_tuner.h"
#endif

/*
 * This macro is meant to be used inside an IAP mode message handler.
 * It is passed the expected minimum length of the message buffer.
 * If the buffer does not have the required lenght an ACK
 * packet with a Bad Parameter error is generated.
 */
#define CHECKLEN(x) do { \
        if (len < (x)) { \
            cmd_ack(cmd, IAP_ACK_BAD_PARAM); \
            return; \
        }} while(0)

/* Check for authenticated state, and return an ACK Not
 * Authenticated on failure.
 */
#define CHECKAUTH do { \
        if (!DEVICE_AUTHENTICATED) { \
            cmd_ack(cmd, IAP_ACK_NO_AUTHEN); \
            return; \
        }} while(0)

/* Transaction ID of the command currently being handled, captured at
 * the top of iap_handlepkt_mode0(). An iPodAck must echo it: MFi spec
 * 2.6.1.2 says "Support for transaction IDs must be disabled upon
 * receipt of a General lingo iPodAck command without a transaction ID.
 * Such commands have a payload length value (byte 2) of either 0x04 or
 * 0x08" -- which is exactly what these two helpers used to emit. One
 * such ack, from any CHECKAUTH, CHECKLEN or unimplemented command, made
 * the accessory stop sending transaction IDs while device.auth.idps
 * stayed set, after which every packet was parsed two bytes off.
 */
static bool l0_has_tid;
/* True when the accessory owes us a transaction ID on this command, so
 * a reply built without one would be malformed rather than legacy. */
static bool l0_tid_expected;
static uint8_t l0_tid_hi, l0_tid_lo;

/* Whether THIS packet carried a transaction ID, which is what every
 * payload offset in this file must key off.
 *
 * device.auth.idps is not that: it is only set at EndIDPS, while MFi
 * 2.6 (p.110) and the IMPORTANT note on p.95 make transaction IDs
 * mandatory from StartIDPS. Deriving an offset from it read every
 * parameter two bytes early for the four commands p.95 permits inside
 * the IDPS window -- GetiPodOptionsForLingo among them, which 2.3.3
 * (p.97) makes the normal capability-discovery path.
 */
#define L0_DOFF() (l0_has_tid ? 2 : 0)

/* The offset the packet is REQUIRED to carry, which is what a length
 * check must demand. L0_DOFF() reports what it actually has, and
 * l0_has_tid needs len >= 4 to be set -- so for a three-byte packet
 * under IDPS the two disagree and a CHECKLEN built on L0_DOFF()
 * collapses to the legacy requirement and lets the packet through.
 * SetUIMode then read the transaction ID's high byte as the mode and
 * changed it. */
#define L0_MINLEN(n) ((n) + (l0_tid_expected ? 2 : 0))

#define L0_TX_TRANSID() do { \
        if (l0_has_tid) { \
            IAP_TX_PUT(l0_tid_hi); \
            IAP_TX_PUT(l0_tid_lo); \
        } else if (l0_tid_expected) { \
            /* MFi 2.6.1.2 (p.111): the accessory turns transaction IDs \
             * off again on "receipt of a General lingo iPodAck command \
             * without a transaction ID ... payload length value of \
             * either 0x04 or 0x08". A bare four-byte ack is exactly \
             * that, so emitting one for a truncated packet would \
             * silently tear transaction IDs down on the accessory while \
             * we carried on adding the two-byte offset to everything we \
             * parsed. Send a zero ID instead: the accessory matches it \
             * against nothing and ignores the reply, which is a far \
             * better failure than a desynchronised link. */ \
            IAP_TX_PUT(0x00); \
            IAP_TX_PUT(0x00); \
        }} while(0)

static void cmd_ack(const unsigned char cmd, const unsigned char status)
{
    /* Command 0x00 is answered like any other.
     *
     * The guard here was "if (cmd != 0)", which silenced the only
     * command an accessory can send that it has no business sending:
     * RequestIdentify is Origin: Apple device (3.3.1, p.124), so
     * receiving one is a direction violation and the default arm's Bad
     * Parameter is the right answer. Suppressed, it was silence -- the
     * same defect the Digital Audio lingo's missing default arm had,
     * and the accessory has nothing to match a silence against. */
    {
        IAP_TX_INIT(0x00, 0x02);
        L0_TX_TRANSID();
        IAP_TX_PUT(status);
        IAP_TX_PUT(cmd);

        iap_send_tx();
    }
}

#define cmd_ok(cmd) cmd_ack((cmd), IAP_ACK_OK)

static void cmd_pending(const unsigned char cmd, const uint32_t msdelay)
{
    IAP_TX_INIT(0x00, 0x02);
    L0_TX_TRANSID();
    IAP_TX_PUT(0x06);
    IAP_TX_PUT(cmd);
    IAP_TX_PUT_U32(msdelay);

    iap_send_tx();
}

void iap_handlepkt_mode0(const unsigned int len, const unsigned char *buf)
{
    unsigned int cmd = buf[1];

    /* We expect at least two bytes in the buffer, one for the
     * lingo, one for the command
     */
    /* Equivalent mutant: the framer never delivers a payload shorter
     * than this. MFi 2.5.2 (p.110) puts the smallest packet payload at
     * 0x02 and iap_getc() enforces it, so len is always at least two
     * and this can never fire. Kept because it states the handler's
     * precondition where a reader looks for it, and recorded so the
     * mutation sweep's survivor list stays fully accounted for. */
    CHECKLEN(2);

    /* Capture the transaction ID so every reply, including the acks
     * emitted by CHECKAUTH/CHECKLEN and by unimplemented commands, can
     * echo it. RequestIdentify (0x00), Identify (0x01) and
     * IdentifyDeviceLingoes (0x13) are the three commands the spec
     * exempts (2.6.1.4), so they never carry one.
     */
    l0_has_tid = false;
    l0_tid_hi = 0;
    l0_tid_lo = 0;
    /* 0x01 and 0x13 are exempt from transaction IDs -- 2.6.1.4 (p.112)
     * -- and 0x00 was listed with them because the same paragraph names
     * RequestIdentify. That exemption is for the accessory *sending*
     * one, and 3.3.1 (p.124) makes it Origin: Apple device, so it never
     * does. Now that receiving one is answered rather than silently
     * dropped, the answer has to carry an ID like any other: without
     * this the reply went out as a bare four-byte General iPodAck,
     * which is precisely the teardown signal L0_TX_TRANSID() above
     * exists to avoid emitting. */
    l0_tid_expected = DEVICE_TRANSID_ACTIVE
                      && cmd != 0x01 && cmd != 0x13;
    if (l0_tid_expected && len >= 4)
    {
        l0_has_tid = true;
        l0_tid_hi = buf[2];
        l0_tid_lo = buf[3];
    }

    /* MFi 2.6.1.2 (p.111): "Support for transaction IDs must be
     * disabled before sending an IdentifyDeviceLingoes command", and
     * 2.6.1.4 (p.112) says the same of the other two exempt commands --
     * "after sending one of these commands the accessory must disable
     * transaction ID support ... until it sends a StartIDPS command."
     *
     * So by the time one of them arrives the accessory has already
     * stopped using them, whether or not the command turns out to be
     * well formed. The state was cleared by iap_reset_device() at the
     * end of the accepted path only, so a rejection -- a short packet,
     * or any of the three parameter errors in 0x13 -- left
     * idps_started set. From there the device demanded transaction IDs
     * from an accessory that had just stopped sending them, and every
     * packet in both directions was parsed two bytes off until replug.
     */
    /* Not 0x00.
     *
     * MFi p.111: "Support for transaction IDs must be disabled upon
     * receipt of a RequestIdentify command, but enabled again before
     * the accessory sends a subsequent StartIDPS command." That is an
     * instruction to the accessory -- 3.3.1 (p.124) gives
     * RequestIdentify "Origin: Apple device", so this side sends it and
     * never receives it. Applying the accessory's rule to ourselves
     * meant a stray or malformed 0x00 tore down our own transaction-ID
     * state mid-session, and every packet after it was parsed two bytes
     * off.
     *
     * 0x01 and 0x13 stay: both are Origin: Accessory, and p.111's other
     * bullets put the teardown on exactly those two. */
    if (cmd == 0x01 || cmd == 0x13)
    {
        device.auth.idps = false;
        device.auth.idps_started = false;
    }

    switch (cmd) {
        /* RequestIdentify (0x00)
         *
         * Sent from the iPod to the device
         */

        /* Identify (0x01)
         * This command is deprecated.
         *
         * It is used by a device to inform the iPod of the devices
         * presence and of the lingo the device supports.
         *
         * Also, it is used to negotiate power for RF transmitters
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x01
         * 0x02: Lingo supported by the device
         *
         * Some RF transmitters use an extended version of this
         * command:
         *
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x01
         * 0x02: Lingo supported by the device, always 0x05 (RF Transmitter)
         * 0x03: Reserved, always 0x00
         * 0x04: Number of valid bits in the following fields
         * 0x05-N: Datafields holding the number of bits specified in 0x04
         *
         * Returns: (none)
         *
         * TODO:
         * BeginHighPower/EndHighPower should be send in the periodic handler,
         * depending on the current play status
         */
        case 0x01:
        {
            /* This is sufficient even for Lingo 0x05, as we are
             * not actually reading from the extended bits for now.
             *
             * Ahead of the read, not after it. The value was fetched
             * first and the check written below it, so a two-byte
             * Identify read the lingo byte from whatever the previous
             * packet left in the buffer. The check did still reject the
             * packet, so nothing acted on it -- but every other handler
             * in this file checks first, and case 0x13 twenty lines up
             * carries a comment about the same mistake. */
            CHECKLEN(3);

            unsigned char lingo = buf[2];

            /* Issuing this command exits any extended interface states
             * and resets authentication
             */
            iap_reset_device(&device);

            /* Legacy Identify has no authentication — grant access so
             * CHECKAUTH does not block lingo commands.
             */
            device.auth.state = AUST_AUTH;

            switch (lingo) {
                case 0x04:
                {
                    /* A single lingo device negotiating the
                     * extended interface lingo. This causes an interface
                     * state change.
                     */
                    iap_interface_state_change(IST_EXTENDED);
                    break;
                }

                case 0x05:
                {
                    /* FM transmitter sends this: */
                    /* FF 55 06 00 01 05 00 02 01 F1 (mode switch) */
                    sleep(HZ/3);
                    /* Accessory Power. BeginHighPower used to go out
                     * here unconditionally; C.8 (p.547) ties the lingo
                     * to playback, and iap_periodic() sends 0x02 and
                     * 0x03 as the play state moves. */
                    iap_high_power_arm();
                    break;
                }
            }

            if (lingo < 32) {
                /* All devices that Identify get access to Lingoes 0x00 and 0x02 */
                device.lingoes = BIT_N(0x00) | BIT_N(0x02);

                device.lingoes |= BIT_N(lingo);

                /* Devices that Identify with Lingo 0x04 also gain access
                 * to Lingo 0x03
                 */
                if (lingo == 0x04)
                    device.lingoes |= BIT_N(0x03);
            } else {
                device.lingoes = 0;
            }
            break;
        }

        /* ACK (0x02)
         *
         * Sent from the iPod to the device
         */

        /* RequestRemoteUIMode (0x03)
         *
         * Request the current Extended Interface Mode state
         * This command may be used only if the accessory requests Lingo 0x04
         * during its identification process.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x03
         *
         * Returns on success:
         * ReturnRemoteUIMode
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x04
         * 0x02: Current Extended Interface Mode (zero: false, non-zero: true)
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x03:
        {
            if (!DEVICE_LINGO_SUPPORTED(0x04)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            IAP_TX_INIT(0x00, 0x04);
            L0_TX_TRANSID();
            if (interface_state == IST_EXTENDED)
                IAP_TX_PUT(0x01);
            else
                IAP_TX_PUT(0x00);

            iap_send_tx();
            break;
        }

        /* ReturnRemoteUIMode (0x04)
         *
         * Sent from the iPod to the device
         */

        /* EnterRemoteUIMode (0x05)
         *
         * Request Extended Interface Mode
         * This command may be used only if the accessory requests Lingo 0x04
         * during its identification process.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x05
         *
         * Returns on success:
         * IAP_ACK_PENDING
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x05:
        {
            if (!DEVICE_LINGO_SUPPORTED(0x04)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            cmd_pending(cmd, 1000);
            iap_interface_state_change(IST_EXTENDED);
            cmd_ok(cmd);
            break;
        }

        /* ExitRemoteUIMode (0x06)
         *
         * Leave Extended Interface Mode
         * This command may be used only if the accessory requests Lingo 0x04
         * during its identification process.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x06
         *
         * Returns on success:
         * IAP_ACK_PENDING
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x06:
        {
            if (!DEVICE_LINGO_SUPPORTED(0x04)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            cmd_pending(cmd, 1000);
            iap_interface_state_change(IST_STANDARD);
            cmd_ok(cmd);
            break;
        }

        /* RequestiPodName (0x07)
         *
         * Retrieves the name of the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x07
         *
         * Returns:
         * ReturniPodName
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x08
         * 0x02-0xNN: iPod name as NULL-terminated UTF8 string
         */
        case 0x07:
        {
            IAP_TX_INIT(0x00, 0x08);
            L0_TX_TRANSID();
            IAP_TX_PUT_STRING("ROCKBOX");

            iap_send_tx();
            break;
        }

        /* ReturniPodName (0x08)
         *
         * Sent from the iPod to the device
         */

        /* RequestiPodSoftwareVersion (0x09)
         *
         * Returns the major, minor and revision numbers of the iPod
         * software version. This not any Lingo protocol version.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x09
         *
         * Returns:
         * ReturniPodSoftwareVersion
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0A
         * 0x02: iPod major software version
         * 0x03: iPod minor software version
         * 0x04: iPod revision software version
         */
        case 0x09:
        {
            IAP_TX_INIT(0x00, 0x0A);
            L0_TX_TRANSID();
            IAP_TX_PUT(IAP_IPOD_FIRMWARE_MAJOR);
            IAP_TX_PUT(IAP_IPOD_FIRMWARE_MINOR);
            IAP_TX_PUT(IAP_IPOD_FIRMWARE_REV);

            iap_send_tx();
            break;
        }

        /* ReturniPodSoftwareVersion (0x0A)
         *
         * Sent from the iPod to the device
         */

        /* RequestiPodSerialNum (0x0B)
         *
         * Returns the iPod serial number
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0B
         *
         * Returns:
         * ReturniPodSerialNumber
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0C
         * 0x02-0xNN: Serial number as NULL-terminated UTF8 string
         */
        case 0x0B:
        {
            IAP_TX_INIT(0x00, 0x0C);
            L0_TX_TRANSID();
            IAP_TX_PUT_STRING("0123456789");

            iap_send_tx();
            break;
        }

        /* ReturniPodSerialNum (0x0C)
         *
         * Sent from the iPod to the device
         */

        /* RequestiPodModelNum (0x0D)
         *
         * Returns the model number as a 32bit unsigned integer and
         * as a string.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0D
         *
         * Returns:
         * ReturniPodModelNum
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0E
         * 0x02-0x05: Model number as 32bit integer
         * 0x06-0xNN: Model number as NULL-terminated UTF8 string
         */
        case 0x0D:
        {
            IAP_TX_INIT(0x00, 0x0E);
            L0_TX_TRANSID();
            IAP_TX_PUT_U32(IAP_IPOD_MODEL);
            IAP_TX_PUT_STRING(IAP_IPOD_VARIANT);

            iap_send_tx();
            break;
        }

        /* ReturniPodSerialNum (0x0E)
         *
         * Sent from the iPod to the device
         */

        /* RequestLingoProtocolVersion (0x0F)
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x0F
         * 0x02: Lingo for which to request version information
         *
         * Returns on success:
         * ReturnLingoProtocolVersion
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x10
         * 0x02: Lingo for which version information is returned
         * 0x03: Major protocol version for the given lingo
         * 0x04: Minor protocol version for the given lingo
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x0F:
        {
            /* Under IDPS a 2-byte transaction ID sits between the
             * command and the payload, so the lingo being asked about
             * is at buf[4], not buf[2]. Reading buf[2] returned the
             * transaction ID's high byte -- zero for the first 255
             * commands of a session -- so every query was answered as
             * though it had asked about the General lingo, and an
             * accessory could never discover any other lingo's version.
             */
            /* L0_MINLEN keys off l0_tid_expected, not l0_has_tid.
             * l0_has_tid needs len >= 4, so a three-byte packet under
             * IDPS collapsed this to the legacy requirement and passed:
             * the lingo was then read from where the transaction ID
             * belongs and the reply carried a fabricated 0x0000. */
            unsigned int off = L0_MINLEN(2);
            unsigned char lingo;

            CHECKLEN(off + 1);
            lingo = buf[off];

            /* Supported lingos and versions are read from the lingo_versions
             * array
             */
            /* LINGO_SUPPORTED(), LINGO_MAJOR() and LINGO_MINOR() all
             * index lingo_versions[(x) & 0x1f], so a raw packet byte
             * aliases: 0x20 answered as the General lingo's row, and
             * 224 lingo IDs that do not exist were reported supported
             * at a version borrowed from a real one. iap_handlepkt()
             * has no case for any of them, so the accessory's follow-up
             * commands vanished with no reply.
             *
             * case 0x01 already guards the same input class with
             * "if (lingo < 32)", and 7ee80330fa fixed it for the IDPS
             * IdentifyToken. This was the last raw byte reaching the
             * mask. */
            if (lingo < 32 && LINGO_SUPPORTED(lingo)) {
                IAP_TX_INIT(0x00, 0x10);
                L0_TX_TRANSID();
                IAP_TX_PUT(lingo);
                IAP_TX_PUT(LINGO_MAJOR(lingo));
                IAP_TX_PUT(LINGO_MINOR(lingo));

                iap_send_tx();
            } else {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            }
            break;
        }

        /* ReturnLingoProtocolVersion (0x10)
         *
         * Sent from the iPod to the device
         */

        /* IdentifyDeviceLingoes (0x13);
         *
         * Used by a device to inform the iPod of the devices
         * presence and of the lingoes the device supports.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x13
         * 0x02-0x05: Device lingoes spoken
         * 0x06-0x09: Device options
         * 0x0A-0x0D: Device ID. Only important for authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_CMD_FAILED
         */
        case 0x13:
        {
            unsigned char i;

            /* Checked before the reads, not after. iap_getc() accepts a
             * payload as short as 0x02 (MFi 2.5.2, p.110), so
             * "FF 55 02 00 13 EB" reached here and the three get_u32
             * below read twelve bytes past the packet -- and past the
             * allocation, for a packet that fills iap_rxlen exactly.
             * The values were discarded by the check underneath them,
             * which is why it never showed. */
            CHECKLEN(14);

            uint32_t lingoes = get_u32(&buf[2]);
            uint32_t options = get_u32(&buf[6]);
            uint32_t deviceid = get_u32(&buf[0x0A]);

            /* Leaving Extended Interface mode is iap_reset_device()'s
             * job, below, and only on the paths that get that far.
             *
             * This call was here, ahead of the three rejection paths,
             * so a command the device went on to refuse with
             * CMD_FAILED had already dropped the mode -- and
             * iap_interface_state_change() raises
             * REMOTE_BUTTON(BUTTON_RC_PLAY) on an EXTENDED to STANDARD
             * transition while playing, so it paused the music too.
             * device.lingoes still had bit 4, so nothing told the
             * accessory it had to re-enter a mode it never asked to
             * leave.
             *
             * iap_reset_device() assigns interface_state directly for
             * exactly this reason; see the comment there. */

            /*
             * Actions by remote listed            Apple Firmware   Rockbox Firmware
             * Apple remote on Radio pause/play  - Mutes            Mutes
             *                       vol up/down - Vol Up/Dn        Vol Up/Dn
             *                       FF/FR       - Station Up/Dn    Station Up/Dn
             *                  iPod Pause/Play  - Mutes            Mutes
             *                       Vol up/down - Vol Up/Dn        Vol Up/Dn
             *                       FF/FR       - Station Up/Dn    Station Up/Dn
             *                 Remote pause/play - Pause/Play       Pause/Play
             *                       vol up/down - Vol Up/Dn        Vol Up/Dn
             *                       FF/FR       - Next/Prev Track  Next/Prev Track
             *                  iPod Pause/Play  - Pause/Play       Pause/Play
             *                       Vol up/down - Vol Up/Dn        Vol Up/Dn
             *                       FF/FR       - Next/Prev Track  Next/Prev Track
             *
             * The following bytes are returned by the accessories listed
             * FF 55 0E 00 13 00 00 00 3D 00 00 00 04 00 00 00 00 9E robi DAB Radio Remote
             * FF 55 0E 00 13 00 00 00 35 00 00 00 04 00 00 00 00 A6 (??) FM Transmitter
             * FF 55 0E 00 13 00 00 00 8D 00 00 00 0E 00 00 00 03 41 Apple Radio Remote
             *
             * Bytes 9-12 = Options         11111100 0000 00 00
             *                              54321098 7654 32 10
             * 00000004 = 00000000 00000000 00000000 0000 01 00 Bits 2
             * 00000004 = 00000000 00000000 00000000 0000 01 00 Bits 2
             * 0000000E = 00000000 00000000 00000000 0000 01 10 Bits 12
             *
             * Bit 0: Authentication 00 = No Authentication 
	     *                       01 = Defer Auth until required (V1)
             * Bit 1:                10 = Authenticate Immediately (V2) 
	     *                       11 = Reserved
             * Bit 2: Power Requirements 00 = Low Power Only 10 = Reserved
             * Bit 3:                    01 = Int High Power 11 = Reserved
             *
             * Bytes 13-16 = Device ID
             * 00000000
             * 00000000
             * 00000003
             *
             * Bytes 5-8 = lingoes spoken   11111100 00000000
             *                              54321098 76543210
             * 0000003D = 00000000 00000000 00000000 00111101 Bits 2345
             * 00000035 = 00000000 00000000 00000000 00110101 Bits 245
             * 0000008D = 00000000 00000000 00000000 10001101 Bits 237
             *
             *
             * Bit 0: Must be set by all devices. See above
             * Bit 1: Microphone Lingo
             * Bit 2: Simple Remote
             * Bit 3: Display Remote
             * Bit 4: Extended Remote
             * Bit 5: RF Transmitter lingo
             */

            /* Strip unsupported lingoes from the requested set instead
             * of rejecting the entire identification.  Accessories
             * (e.g. Onkyo docks) may request lingoes that Apple firmware
             * supports but Rockbox does not (Microphone, USB Host, etc.).
             * Masking them out lets the accessory work with the subset
             * Rockbox can handle.
             */
            for(i=0; i<32; i++) {
                if ((lingoes & BIT_N(i)) && !LINGO_SUPPORTED(i)) {
                    lingoes &= ~BIT_N(i);
                }
            }

            /* Bit 0 _must_ be set by the device */
            if (!(lingoes & 1)) {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }

            /* Specifying a deviceid without requesting authentication is
             * an error
             */
            if (deviceid && !(options & 0x03)) {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }

            /* Specifying authentication without a deviceid is an error */
            if (!deviceid && (options & 0x03)) {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }

            iap_reset_device(&device);
            device.lingoes = lingoes;

            /* Devices using IdentifyDeviceLingoes get power off notifications */
            device.do_power_notify = true;

            /* If a new authentication is requested, start the auth
             * process.
             * The periodic handler will take care of sending out the
             * GetDevAuthenticationInfo packet
             *
             * If no authentication is requested, schedule the start of
             * GetAccessoryInfo
             */
            if (deviceid && (options & 0x03) && !DEVICE_AUTH_RUNNING) {
                device.auth.state = AUST_INIT;
                /* iap_periodic() is what sends GetDevAuthenticationInfo,
                 * and IAP_EV_MSG_RCVD does not call it -- so the
                 * handshake used to start on the next tick rather than
                 * on this packet. Knock, the way every other deferred
                 * send in this tree does. */
                iap_wake();
            } else {
                /* No authentication requested — grant access immediately.
                 * Without this, CHECKAUTH blocks all authenticated lingo
                 * commands (e.g. SetRemoteEventNotification) and the
                 * accessory retries forever.
                 */
                device.auth.state = AUST_AUTH;
                device.accinfo = ACCST_INIT;
            }

            cmd_ok(cmd);

            /* Bit 0: Must be set by all devices. See above*/
            /* Bit 1: Microphone Lingo */
            /* Bit 2: Simple Remote */
            /* Bit 3: Display Remote */
            /* Bit 4: Extended Remote */
            /* Bit 5: RF Transmitter lingo */
            if (lingoes & (1 << 5))
            {
                /* FM transmitter sends this: */
                /* FF 55 0E 00 13 00 00 00 35 00 00 00 04 00 00 00 00 A6 (??)*/
                /* 0x00000035 = 00000000 00000000 00000000 00110101 */
                /* 1<<5                                      1      */
                /* GetAccessoryInfo */
                IAP_TX_INIT(0x00, 0x27);
                IAP_TX_PUT(0x00);

                iap_send_tx();

                /* Accessory Power, as above. */
                iap_high_power_arm();
            }
            /* Bit 6: USB Host Control */
            /* Bit 7: RF Tuner lingo */
#if CONFIG_TUNER
            if (lingoes & (1 << 7))
            {
                /* ipod fm radio remote sends this: */
                /* FF 55 0E 00 13 00 00 00 8D 00 00 00 0E 00 00 00 03 */
                /* 0x0000008D = 00000000 00000000 00000000 00011101   */
                /* 1<<7                                               */
                radio_present = 1;
                /* And ask what it can do. MFi 4.7.7 (p.293) makes
                 * RetTunerCaps a response to GetTunerCaps, so without
                 * the request the capability-driven bring-up in
                 * iap-lingo7.c never runs. Flagged rather than sent:
                 * lingo 7 requires authentication and there is none
                 * yet. */
                device.tuner_caps_pending = true;
            }
#endif
            /* Bit 8: Accessory Equalizer Lingo */
            /* Bit 9: Reserved */
            /* Bit 10: Digial Audio Lingo */
            /* Bit 11: Reserved */
            /* Bit 12: Storage Lingo */
            /* Bit 13: Reserved */
            /* .................*/
            /* Bit 31: Reserved */
            break;
        }

        /* GetDevAuthenticationInfo (0x14)
         *
         * Sent from the iPod to the device
         */

        /* RetDevAuthenticationInfo (0x15)
         *
         * Send certificate information from the device to the iPod.
         * The certificate may come in multiple parts and has
         * to be reassembled.
         *
         * In IDPS mode, a 2-byte transaction ID precedes the data:
         * 0x02-0x03: Transaction ID (if present)
         *
         * Data fields (at offset 'off'):
         * off+0: Authentication major version
         * off+1: Authentication minor version
         * off+2: Certificate current section index
         * off+3: Certificate maximum section index
         * off+4+: Certificate data
         *
         * Returns on success:
         * IAP_ACK_OK for intermediate sections
         * AckDevAuthenticationInfo for the last section
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAMETER
         * AckDevAuthenticationInfo for version mismatches
         *
         */
        case 0x15:
        {
            CHECKLEN(4);

            /* In IDPS mode, a 2-byte transID precedes the auth data.
             * All responses must echo it. */
            unsigned int off = 2;
            uint8_t tid_hi = 0, tid_lo = 0;
            if (DEVICE_TRANSID_ACTIVE) {
                off = 4;
                tid_hi = buf[2];
                tid_lo = buf[3];
                CHECKLEN(6);
                if ((((uint16_t)tid_hi << 8) | tid_lo)
                    != device.auth.ipod_tid)
                    break;
                device.auth.tid_hi = tid_hi;
                device.auth.tid_lo = tid_lo;
            }

            if (device.auth.state != AUST_CERTREQ
                && device.auth.state != AUST_CERTBEG) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            device.auth.version = (buf[off] << 8) | buf[off + 1];

            /* We only support authentication versions 1.0 and 2.0 */
            if ((device.auth.version != 0x100) && (device.auth.version != 0x200)) {
                /* iap_reset_auth() clears auth.idps, but the reply below
                 * still has to carry the transaction ID, and the session
                 * is still an IDPS one -- dropping the flag here would
                 * shift every later packet's payload by two bytes. */
                bool was_idps = device.auth.idps;
                bool was_started = device.auth.idps_started;
                iap_reset_auth(&(device.auth));
                device.auth.idps = was_idps;
                /* idps_started has to survive too. Inside the IDPS
                 * window idps is still false and idps_started is the
                 * only thing saying transaction IDs are in force, so
                 * losing it here dropped the ID from this very reply
                 * and from everything after it. */
                device.auth.idps_started = was_started;

                IAP_TX_INIT(0x00, 0x16);
                if (DEVICE_TRANSID_ACTIVE) {
                    IAP_TX_PUT(tid_hi); IAP_TX_PUT(tid_lo);
                }
                IAP_TX_PUT(0x08);

                iap_send_tx();
                break;
            }
            if (device.auth.version == 0x100) {
                device.auth.state = AUST_CERTALLRECEIVED;
            } else {
                /* Version 2.00 requires cert data */
                CHECKLEN(off + 5);
                switch (device.auth.state)
                {
                    case AUST_CERTREQ:
                    {
                        device.auth.max_section = buf[off + 3];
                        device.auth.state = AUST_CERTBEG;

                        /* Intentional fall-through */
                    }
                    case AUST_CERTBEG:
                    {
                        if (buf[off + 2] != device.auth.next_section) {
                            IAP_TX_INIT(0x00, 0x02);
                            if (DEVICE_TRANSID_ACTIVE) {
                                IAP_TX_PUT(tid_hi); IAP_TX_PUT(tid_lo);
                            }
                            IAP_TX_PUT(IAP_ACK_BAD_PARAM);
                            IAP_TX_PUT(cmd);
                            iap_send_tx();
                            break;
                        }

                        if (device.auth.next_section == device.auth.max_section) {
                            device.auth.state = AUST_CERTALLRECEIVED;
                        } else {
                            device.auth.next_section++;
                            IAP_TX_INIT(0x00, 0x02);
                            if (DEVICE_TRANSID_ACTIVE) {
                                IAP_TX_PUT(tid_hi); IAP_TX_PUT(tid_lo);
                            }
                            IAP_TX_PUT(IAP_ACK_OK);
                            IAP_TX_PUT(cmd);
                            iap_send_tx();
                        }
                        break;
                    }
                    default:
                    {
                        IAP_TX_INIT(0x00, 0x02);
                        if (DEVICE_TRANSID_ACTIVE) {
                            IAP_TX_PUT(tid_hi); IAP_TX_PUT(tid_lo);
                        }
                        IAP_TX_PUT(IAP_ACK_BAD_PARAM);
                        IAP_TX_PUT(cmd);
                        iap_send_tx();
                        break;
                    }
                }
            }
            if (device.auth.state == AUST_CERTALLRECEIVED) {
                /* All certificate data received. ACK OK.
                 * Periodic handler sends GetDevAuthenticationSignature
                 * (0x17) on the next tick via AUST_CERTDONE.
                 */
                IAP_TX_INIT(0x00, 0x16);
                if (DEVICE_TRANSID_ACTIVE) {
                    IAP_TX_PUT(tid_hi); IAP_TX_PUT(tid_lo);
                }
                IAP_TX_PUT(0x00);

                iap_send_tx();

                /* MFi spec Table 2-8 step 4: send GetAccessoryInfo
                 * between AckAccessoryAuthenticationInfo and
                 * GetAccessoryAuthenticationSignature (non-IDPS only). */
                if (!device.auth.idps)
                {
                    IAP_TX_INIT(0x00, 0x27);
                    IAP_TX_PUT(0x00);
                    iap_send_tx();
                }

                device.auth.deadline = 0;
                device.auth.state = AUST_CERTDONE;
            }
            break;
        }

        /* AckDevAuthenticationInfo (0x16)
         *
         * Sent from the iPod to the device
         */

        /* GetDevAuthenticationSignature (0x17)
         *
         * Sent from the iPod to the device
         */

        /* RetDevAuthenticationSignature (0x18)
         *
         * Return a calculated signature based on the device certificate
         * and the challenge sent with GetDevAuthenticationSignature
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x17
         * 0x02-0xNN: Certificate data
         *
         * Returns on success:
         * AckDevAuthenticationStatus
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x19
         * 0x02: Status (0x00: OK)
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         *
         * TODO:
         * There is a timeout of 75 seconds between GetDevAuthenticationSignature
         * and RetDevAuthenticationSignature for Auth 2.0. This is currently not
         * checked.
         */
        case 0x18:
        {
            CHECKLEN(L0_MINLEN(2));

            /* In IDPS mode, transID at buf[2..3]. Save it for the ACK. */
            /* len >= 4, not > 4: the emit below is gated on
             * DEVICE_TRANSID_ACTIVE alone, so a four-byte
             * "00 18 TT TT" was answered with a fabricated 0x0000
             * where TT TT was sitting at buf[2..3]. MFi 2.6.1.1
             * (p.111) has the accessory discard a response matching no
             * command it sent, so it waited for an
             * AckDevAuthenticationStatus that never came.
             * L0_TX_TRANSID() gets this right everywhere else in the
             * file. */
            uint8_t tid_hi_18 = 0, tid_lo_18 = 0;
            if (DEVICE_TRANSID_ACTIVE && len >= 4) {
                tid_hi_18 = buf[2];
                tid_lo_18 = buf[3];
                if ((((uint16_t)tid_hi_18 << 8) | tid_lo_18)
                    != device.auth.ipod_tid)
                    break;
            }

            if (device.auth.state != AUST_CHASENT) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            /* We don't verify the signature — just ACK success */
            IAP_TX_INIT(0x00, 0x19);
            if (DEVICE_TRANSID_ACTIVE) {
                IAP_TX_PUT(tid_hi_18); IAP_TX_PUT(tid_lo_18);
            }
            IAP_TX_PUT(0x00);

            iap_send_tx();
            device.auth.state = AUST_AUTH;
            if (DEVICE_LINGO_SUPPORTED(0x05))
                iap_high_power_arm();
            /* GetAccessoryInfo (0x27) is for accessories that
             * identified with IdentifyDeviceLingoes: MFi Table 2-8
             * marks steps 4 and 5 "non-IDPS only", and 3.3.32 scopes
             * the command to those accessories. An IDPS accessory has
             * already supplied all of it in AccessoryInfoToken.
             */
            if (!device.auth.idps && device.accinfo == ACCST_NONE)
                device.accinfo = ACCST_INIT;

            /* After auth, initiate digital audio via periodic handler.
             * Do NOT send anything here -- tx_buf is shared and 0x19
             * hasn't finished DMA yet. */
            if (DEVICE_LINGO_SUPPORTED(0x0A))
                device.audio_init_pending = true;

            /* A deferred volume push used to be armed here. It went out
             * as 0x03/0x0D, a response command with no unsolicited
             * form, to every accessory whether it had subscribed or
             * not. device.volume_reported in iap_periodic() covers the
             * real case: the level is pushed once, on 0x03/0x09, when
             * an accessory enables the event. */

            break;
        }

        /* AckDevAuthenticationStatus (0x19)
         *
         * Sent from the iPod to the device
         */

        /* GetiPodAuthenticationInfo (0x1A)
         *
         * Obtain authentication information from the iPod.
         * This cannot be implemented without posessing an Apple signed
         * certificate and the corresponding private key.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x1A
         *
         * This command requires authentication
         *
         * Returns:
         * IAP_ACK_CMD_FAILED
         */
        case 0x1A:
        {
            CHECKAUTH;

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* RetiPodAuthenticationInfo (0x1B)
         *
         * Sent from the iPod to the device
         */

        /* AckiPodAuthenticationInfo (0x1C)
         *
         * Confirm authentication information from the iPod.
         * This cannot be implemented without posessing an Apple signed
         * certificate and the corresponding private key.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x1C
         * 0x02: Authentication state (0x00: OK)
         *
         * This command requires authentication
         *
         * Returns: (none)
         */
        case 0x1C:
        {
            CHECKAUTH;

            break;
        }

        /* GetiPodAuthenticationSignature (0x1D)
         *
         * Send challenge information to the iPod.
         * This cannot be implemented without posessing an Apple signed
         * certificate and the corresponding private key.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x1D
         * 0x02-0x15: Challenge
         *
         * This command requires authentication
         *
         * Returns:
         * IAP_ACK_CMD_FAILED
         */
        case 0x1D:
        {
            CHECKAUTH;

            cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* RetiPodAuthenticationSignature (0x1E)
         *
         * Sent from the iPod to the device
         */

        /* AckiPodAuthenticationStatus (0x1F)
         *
         * Confirm chellenge information from the iPod.
         * This cannot be implemented without posessing an Apple signed
         * certificate and the corresponding private key.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x1C
         * 0x02: Challenge state (0x00: OK)
         *
         * This command requires authentication
         *
         * Returns: (none)
         */
        case 0x1F:
        {
            CHECKAUTH;

            break;
        }

        /* NotifyiPodStateChange (0x23)
         *
         * Sent from the iPod to the device
         */

        /* GetIpodOptions (0x24)
         *
         * Request supported features of the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x24
         *
         * Retuns:
         * RetiPodOptions
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x25
         * 0x02-0x09: Options as a bitfield
         */
        case 0x24:
        {
            /* There are only two features that can be communicated via this
             * function, video support and the ability to control line-out usage.
             * Rockbox supports neither
             */
            IAP_TX_INIT(0x00, 0x25);
            L0_TX_TRANSID();
            /* MFi Table 3-41 (p.144): "Bit 1: the Apple device
             * supports using SetiPodPreferences to control line-out
             * usage / Bit 0: the Apple device supports video output".
             *
             * Both words were zero, which contradicted this device's
             * own answers one command over: 2533311cec set the General
             * lingo's line-out bit in RetiPodOptionsForLingo because
             * SetiPodPreferences really does accept class 0x03. An
             * accessory that only implements 0x24 concluded it must not
             * register the line-out preference, and the note to Table
             * 4-59 (p.256) makes that a precondition for volume-change
             * notifications.
             *
             * Video output stays clear; that one is true. */
            IAP_TX_PUT_U32(0x00);
            IAP_TX_PUT_U32(0x02);

            iap_send_tx();
            break;
        }

        /* RetiPodOptions (0x25)
         *
         * Sent from the iPod to the device
         */

        /* GetAccessoryInfo (0x27)
         *
         * Sent from the iPod to the device
         */

        /* RetAccessoryInfo (0x28)
         *
         * Send information about the device
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x28
         * 0x02: Accessory info type
         * 0x03-0xNN: Accessory information (depends on 0x02)
         *
         * Returns: (none)
         *
         * TODO: Actually do something with the information received here.
         * Some devices actually expect us to request the data they
         * offer, so completely ignoring this does not work, either.
         */
        case 0x28:
        {
            /* The Accessory Info Type is payload offset 0, which under
             * IDPS follows the 2-byte transaction ID. Reading buf[2]
             * picked up that ID's high byte instead, so the type-0
             * branch was always taken and device.capabilities was
             * filled with transID and type bytes -- after which the
             * capability sweep stalled and the idle tick never slowed.
             */
            unsigned int off = L0_DOFF();

            CHECKLEN(L0_MINLEN(3));

            switch (buf[0x02 + off])
            {
                /* Info capabilities */
                case 0x00:
                {
                    CHECKLEN(7 + off);

                    device.capabilities = get_u32(&buf[0x03 + off]);
                    /* Type 0x00 was already queried, that's where this 
		     * information comes from 
		     */
                    /* OR, as iap-core.c does. Assigning discarded the
                     * record of everything the sweep had already asked
                     * for, so a second RetAccessoryInfo(0x00) -- which
                     * is reachable, this file sends GetAccessoryInfo
                     * directly and the ACCST_INIT path sends it again
                     * -- made iap_periodic() re-ask the lot. */
                    device.capabilities_queried |= 0x01;
                    device.capabilities &= ~0x01;
                    break;
                }

                /* Accessory incoming maximum payload size, Table 3-46
                 * (p.147) and packet Table 3-53 (p.151): two bytes,
                 * big-endian. The capability sweep asks for this
                 * whenever Table 3-48 (p.148) bit 9 is set, and the
                 * answer used to fall into the default below and be
                 * thrown away -- so the device asked the question and
                 * ignored it, then sent replies bounded only by its own
                 * 512-byte buffer.
                 *
                 * p.150 bounds the declared value at 128..65529;
                 * anything outside that is not a size this device will
                 * honour, and 0 leaves iap_tx_strlcpy() on the
                 * TX_BUFLEN limit. */
                case 0x09:
                {
                    if (len >= (unsigned int)(0x05 + off)) {
                        uint16_t n = (buf[0x03 + off] << 8) | buf[0x04 + off];
                        if (n >= 128 && n <= 0xfffa)
                            device.acc_max_payload = n;
                    }
                    break;
                }

                /* For now, ignore all other information */
                default:
                {
                    break;
                }
            }

            /* If there are any unqueried capabilities left, do so */
            if (device.capabilities)
                device.accinfo = ACCST_DATA;

            break;
        }

        /* GetiPodPreferences (0x29)
         *
         * Retrieve information about the current state of the
         * iPod.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x29
         * 0x02: Information class requested
         *
         * This command requires authentication
         *
         * Returns on success:
         * RetiPodPreferences
         *
         * Packet format (offset in data[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x2A
         * 0x02: Information class provided
         * 0x03: Information
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         */
        case 0x29:
        {
            /* Under IDPS the preference class follows the 2-byte
             * transaction ID, so a bare buf[2] reads that ID's high
             * byte instead. Class 0x03 is the line-out preference the
             * spec names as a precondition for volume notifications
             * (note to Table 4-59, p.256), so misreading it costs the
             * accessory its volume control. */
            unsigned int off = L0_DOFF();

            CHECKLEN(L0_MINLEN(3));
            /* MFi Table 2-7 footnote 2 (p.105): "Preference commands
             * (0x29-0x2B) require authentication on all Apple devices
             * except the 5G iPod; however, getting or setting the
             * line-out preference class (0x03) does not require
             * authentication."
             *
             * The gate was unconditional, so an accessory that had
             * identified but not yet finished authenticating was
             * refused the line-out preference -- which the note to
             * Table 4-59 (p.256) makes a precondition for volume-change
             * notifications. */
            if (buf[0x02 + off] != 0x03)
                CHECKAUTH;

            IAP_TX_INIT(0x00, 0x2A);
            L0_TX_TRANSID();
            /* The only information really supported is 0x03, Line-out usage.
             * All others are video related
             */
            if (buf[0x02 + off] == 0x03) {
                IAP_TX_PUT(0x03);
                IAP_TX_PUT(0x01);   /* Line-out enabled */

                iap_send_tx();
            } else {
                /* Return preference=0 for unsupported classes —
                 * accessories may query before setting */
                IAP_TX_PUT(buf[0x02 + off]);
                IAP_TX_PUT(0x00);

                iap_send_tx();
            }

            break;
        }

        /* RetiPodPreference (0x2A)
         *
         * Sent from the iPod to the device
         */

        /* SetiPodPreferences (0x2B)
         *
         * Set preferences on the iPod
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x29
         * 0x02: Prefecence class requested
         * 0x03: Preference setting
         * 0x04: Restore on exit
         *
         * This command requires authentication
         *
         * Returns on success:
         * IAP_ACK_OK
         *
         * Returns on failure:
         * IAP_ACK_BAD_PARAM
         * IAP_ACK_CMD_FAILED
         */
        case 0x2B:
        {
            /* Same IDPS offset as GetiPodPreferences above. */
            unsigned int off = L0_DOFF();

            CHECKLEN(L0_MINLEN(5));
            /* MFi Table 2-7 footnote 2 (p.105): "Preference commands
             * (0x29-0x2B) require authentication on all Apple devices
             * except the 5G iPod; however, getting or setting the
             * line-out preference class (0x03) does not require
             * authentication."
             *
             * The gate was unconditional, so an accessory that had
             * identified but not yet finished authenticating was
             * refused the line-out preference -- which the note to
             * Table 4-59 (p.256) makes a precondition for volume-change
             * notifications. */
            if (buf[0x02 + off] != 0x03)
                CHECKAUTH;

            /* The only information really supported is 0x03, Line-out usage.
             * All others are video related
             */
            if (buf[0x02 + off] == 0x03) {
                /* If line-out disabled is requested, reply with IAP_ACK_CMD_FAILED,
                 * otherwise with IAP_ACK_CMD_OK
                 */
                if (buf[0x03 + off] == 0x00) {
                    cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                } else {
                    cmd_ok(cmd);
                }
            } else {
                /* Accept unsupported preference classes (video, etc.) —
                 * accessories disconnect if preferences are rejected.
                 *
                 * Deliberate, and reviewed again: a review objected
                 * that answering Success for a video class claims
                 * something the option bits and the capability word
                 * both deny. True, and still the right trade -- a real
                 * dock that sets the video format on connect and gets
                 * refused drops the link, and the alternative buys
                 * nothing because nothing here is stored either way.
                 *
                 * That is also why SetiPodPreferences has no Restore on
                 * Exit handling although 3.3.36 (p.157) defines the
                 * field: "If the Restore on Exit field is set to 0x01
                 * the Apple device restores the original setting for
                 * this preference when the accessory is disconnected."
                 * No preference this command accepts is ever changed --
                 * line-out is always on and the rest are ignored -- so
                 * there is no original to put back. If a preference
                 * ever becomes real, the restore has to come with it. */
                cmd_ok(cmd);
            }

            break;
        }

        /* SetUIMode (0x37)
         *
         * Sets the UI mode of the iPod.
         * UIMode 0x00 = Standard mode
         * UIMode 0x01 = Extended Interface mode
         * UIMode 0x02 = iPod Out mode
         *
         * In IDPS mode, a 2-byte transID precedes the payload.
         */
        /* GetUIMode (0x35)
         *
         * MFi 3.3.37 (p.158): "The accessory sends this command to the
         * attached Apple device to determine its current user interface
         * mode. The Apple device replies by sending a RetUIMode
         * command." 3.3.38 and Table 3-61 (p.159) give RetUIMode one
         * parameter, and Table 3-62 the values: "0x00 Standard Apple
         * device operating mode, 0x01 Extended Interface mode, 0x02
         * iPod Out fullscreen mode, 0x03 iPod Out action-safe mode".
         *
         * SetUIMode below was implemented and this was not, so an
         * accessory could put the device into a mode and not read it
         * back. Both are protocol 1.09, which this device advertises,
         * and Table 3-132 defines no option bit for either -- so there
         * was no way for an accessory to discover the asymmetry short
         * of getting a Bad Parameter for a command the version says is
         * there. iPod Out is not implemented, so only 0x00 and 0x01 are
         * ever reported.
         */
        case 0x35:
        {
            CHECKAUTH;

            IAP_TX_INIT(0x00, 0x36);
            L0_TX_TRANSID();
            IAP_TX_PUT(interface_state == IST_EXTENDED ? 0x01 : 0x00);
            iap_send_tx();
            break;
        }

        case 0x37:
        {
            CHECKAUTH;
            CHECKLEN(L0_MINLEN(3));
            int off = L0_DOFF();
            unsigned char mode = buf[2 + off];
            unsigned char status;
            if (mode == 0x01) {
                /* "Extended Interface mode is available only if the
                 * accessory identifies itself successfully for the
                 * Extended Interface lingo (Lingo 0x04)" (MFi spec
                 * 3.3.39). Refusing with Bad Parameter is also what
                 * lets an accessory fall back to the deprecated
                 * EnterExtendedInterfaceMode.
                 */
                if (!DEVICE_LINGO_SUPPORTED(0x04)) {
                    status = IAP_ACK_BAD_PARAM;
                } else {
                    iap_interface_state_change(IST_EXTENDED);
                    status = IAP_ACK_OK;
                }
            } else if (mode == 0x00) {
                iap_interface_state_change(IST_STANDARD);
                status = IAP_ACK_OK;
            } else {
                status = IAP_ACK_BAD_PARAM;
            }
            /* Hand-rolled and gated on device.auth.idps, this emitted a
             * four-byte General iPodAck inside the IDPS window, which
             * MFi 2.6.1.2 (p.111) makes an instruction to the accessory
             * to stop using transaction IDs. cmd_ack() goes through
             * L0_TX_TRANSID(), which keys off the packet. */
            cmd_ack(cmd, status);
            break;
        }

        /* StartIDPS (0x38)
         *
         * Newer accessories use IDPS instead of IdentifyDeviceLingoes.
         * All IDPS commands include 2-byte transaction IDs after the
         * command byte. Responses MUST echo the same transaction ID.
         *
         * Packet format:
         * 0x00: Lingo ID, always 0x00
         * 0x01: Command, always 0x38
         * 0x02-0x03: Transaction ID
         *
         * Returns: iPodAck with transID, status=OK
         */
        case 0x38:
        {
            CHECKLEN(4);
            uint8_t tid_hi = buf[2];
            uint8_t tid_lo = buf[3];
#ifdef LOGF_ENABLE
            logf("iap: StartIDPS tid=%02x%02x", tid_hi, tid_lo);
#endif
            /* MFi p.96: "If the accessory sends StartIDPS again, while
             * the Apple device is in the IDPS process, the Apple device
             * restarts the IDPS process", and "If an accessory has
             * already completed IDPS successfully, inadvertently
             * sending a StartIDPS or IdentifyDeviceLingoes command
             * resets its authentication and identification states."
             *
             * This only set the flag and acked, so a second StartIDPS
             * carried the first attempt's IdentifyToken forward:
             * whatever lingoes, options and device ID it had declared
             * survived into a session that was supposed to begin
             * again.
             *
             * Clearing the token was not enough either. The sentence
             * quoted above says authentication and identification, and
             * both survived: DEVICE_AUTHENTICATED, the negotiated
             * lingoes, Extended Interface mode and every notification
             * mask. p.95 has a power glitch as a reason to restart, so
             * this is reachable without an accessory misbehaving --
             * and inside the three-second IDPS budget the device went
             * on serving gated commands and sending unsolicited
             * notifications for a session that no longer existed.
             *
             * iap_reset_device() is what case 0x13 uses for the same
             * sentence. idps_started is re-set after it because
             * transaction IDs are live from this command, including
             * for the ack below. */
            iap_reset_device(&device);
            device.auth.idps_started = true;

            /* iPodAck with transaction ID: format per Table 3-5 */
            IAP_TX_INIT(0x00, 0x02);
            IAP_TX_PUT(tid_hi);
            IAP_TX_PUT(tid_lo);
            IAP_TX_PUT(IAP_ACK_OK);
            IAP_TX_PUT(cmd);
            iap_send_tx();
            break;
        }

        /* SetFIDTokenValues (0x39)
         *
         * Accessory sends FID tokens describing its capabilities.
         * Respond with RetFIDTokenValueACKs (0x3A) accepting all tokens.
         *
         * Packet format:
         * 0x00: Lingo ID, always 0x00
         * 0x01: Command, always 0x39
         * 0x02-0x03: Transaction ID
         * 0x04: Number of FID token values
         * 0x05+: FID token data: [length(1)][type(1)][subtype(1)][data(length-2)]
         *        length includes type+subtype but not itself
         */
        case 0x39:
        {
            CHECKLEN(5);

            /* MFi 3.3.41 (p.160): the Apple device accepts this only
             * inside the IDPS process -- "If the accessory sends this
             * command while the Apple device is not in the IDPS
             * process, the Apple device responds with an iPodAck
             * command that passes a nonzero status" (3.3.43, p.173),
             * and 3.3.41 (p.160) says the same for SetFIDTokenValues.
             *
             * Neither checked. An EndIDPS with no StartIDPS before it
             * was accepted outright: it set device.auth.idps, which
             * makes DEVICE_TRANSID_ACTIVE true for every lingo, so
             * every later packet from an accessory that never enabled
             * transaction IDs was parsed two bytes off. */
            if (!device.auth.idps_started) {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }
            uint8_t tid_hi = buf[2];
            uint8_t tid_lo = buf[3];
            int num_tokens = buf[4];
            int offset = 5; /* start of first FID token in buf */
            int i;

#ifdef LOGF_ENABLE
            logf("iap: SetFIDTokenValues tid=%02x%02x n=%d",
                 tid_hi, tid_lo, num_tokens);
#endif

            /* Walk the tokens once before answering any of them.
             *
             * Each token can consume as little as one input byte, when
             * its length byte is zero, but every token always produces
             * four or five bytes of acknowledgement. A 134-byte command
             * declaring 255 zero-length tokens therefore built a
             * 513-byte reply and hit panicf("IAP: TX buffer overflow"),
             * which halts the player. Any accessory could send it.
             *
             * MFi Table 3-65 (p.160) already prescribes the answer: "If
             * the number of token-value fields the Apple device parses
             * from the command doesn't match this value, the Apple
             * device returns a nonzero iPodAck and accepts no
             * token-value fields." A malformed stream cannot produce a
             * matching count, so validating here covers the crash and
             * conforms at the same time.
             *
             * Table 3-66 (p.161) puts FIDType and FIDSubtype after the
             * length byte, which does not count itself, so the smallest
             * structurally valid token has a length of 2.
             */
            {
                int scan = offset;
                int parsed = 0;

                while (parsed < num_tokens && scan + 3 <= (int)len)
                {
                    uint8_t tlen = buf[scan];

                    if (tlen < 2 || scan + 1 + tlen > (int)len)
                        break;
                    if (buf[scan + 1] == 0x00 && tlen < 3
                        && (buf[scan + 2] == 0x02
                            || buf[scan + 2] == 0x03
                            || buf[scan + 2] == 0x04))
                        break;
                    scan += 1 + tlen;
                    parsed++;
                }

                /* Largest acknowledgement entry is five bytes, plus the
                 * lingo, command, transaction ID and count ahead of
                 * them. */
                if (parsed != num_tokens || scan != (int)len
                    || (4 + 2 + 5 * num_tokens) > TX_BUFLEN)
                {
                    cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                    break;
                }
            }

            /* RetFIDTokenValueACKs (0x3A) with transID */
            IAP_TX_INIT(0x00, 0x3A);
            IAP_TX_PUT(tid_hi);
            IAP_TX_PUT(tid_lo);
            IAP_TX_PUT(num_tokens);

            for (i = 0; i < num_tokens && offset + 3 <= (int)len; i++)
            {
                uint8_t fid_len = buf[offset];         /* token payload length */
                uint8_t fid_type = buf[offset + 1];
                uint8_t fid_subtype = buf[offset + 2];
                uint8_t ack_status = 0;

                /* Parse IdentifyToken (FIDType=0x00, FIDSubtype=0x00) */
                if (fid_type == 0x00 && fid_subtype == 0x00 &&
                    fid_len >= 3 && offset + 1 + fid_len <= (int)len)
                {
                    uint8_t num_lingoes = buf[offset + 3];
                    uint32_t lingoes = 0;
                    int j;
                    for (j = 0; j < num_lingoes &&
                         (offset + 4 + j) < (offset + 1 + fid_len); j++)
                    {
                        uint8_t lingo = buf[offset + 4 + j];
                        if (lingo < 32)
                            lingoes |= BIT_N(lingo);
                    }
                    /* Filtered the same way IdentifyDeviceLingoes
                     * filters, and for the same reason. That path
                     * masks out lingoes this firmware has no handler
                     * for; this one stored whatever the accessory
                     * listed, so DEVICE_LINGO_SUPPORTED() answered yes
                     * for a lingo iap_handlepkt() has no case for and
                     * the accessory's commands vanished with no reply
                     * at all. Two identify paths, two answers about
                     * the same lingo. */
                    for (j = 0; j < 32; j++) {
                        if ((lingoes & BIT_N(j)) && !LINGO_SUPPORTED(j))
                            lingoes &= ~BIT_N(j);
                    }

                    device.idps_lingoes = lingoes;

                    /* Extract options (4 bytes) after lingo bytes */
                    int opt_off = offset + 4 + num_lingoes;
                    if (opt_off + 4 <= offset + 1 + fid_len)
                        device.idps_options = get_u32(&buf[opt_off]);

                    /* Extract deviceID (4 bytes) after options */
                    int did_off = opt_off + 4;
                    if (did_off + 4 <= offset + 1 + fid_len)
                        device.idps_deviceid = get_u32(&buf[did_off]);
                }

                if (fid_type == 0x00 && fid_subtype == 0x02
                    && fid_len >= 3 && buf[offset + 3] == 0x09)
                {
                    if (fid_len == 5) {
                        uint16_t n = get_u16(&buf[offset + 4]);

                        if (n >= 128 && n <= 0xfffa)
                            device.acc_max_payload = n;
                        else
                            ack_status = 2;
                    } else {
                        ack_status = 2;
                    }
                }

                /* ACK entry. Most tokens use [0x03][type][subtype]
                 * [status], but AccessoryInfoToken, iPodPreferenceToken
                 * and EAProtocolToken use a five-byte form that echoes
                 * accInfoType / iPodPrefClass / protocolIndex -- each of
                 * which sits in the byte right after FIDSubtype.
                 * Without the echo an accessory cannot tell which of
                 * several AccessoryInfoTokens was accepted.
                 */
                if (fid_type == 0x00 &&
                    (fid_subtype == 0x02 || fid_subtype == 0x03 ||
                     fid_subtype == 0x04) &&
                    fid_len >= 3)
                {
                    IAP_TX_PUT(0x04);
                    IAP_TX_PUT(fid_type);
                    IAP_TX_PUT(fid_subtype);
                    IAP_TX_PUT(ack_status);
                    IAP_TX_PUT(buf[offset + 3]); /* echo field */
                }
                else
                {
                    IAP_TX_PUT(0x03); /* length: type+subtype+status */
                    IAP_TX_PUT(fid_type);
                    IAP_TX_PUT(fid_subtype);
                    IAP_TX_PUT(0x00); /* status: accepted */
                }

                /* skip: length_byte(1) + payload(fid_len) */
                offset += 1 + fid_len;
            }

            iap_send_tx();
            break;
        }

        /* EndIDPS (0x3B)
         *
         * End of IDPS session. Response is IDPSStatus (0x3C) with transID.
         * Then start authentication.
         *
         * Packet format:
         * 0x00: Lingo ID, always 0x00
         * 0x01: Command, always 0x3B
         * 0x02-0x03: Transaction ID
         * 0x04: AccEndIDPSStatus (0x00=Continue, 0x01=Reset, 0x02=Abandon)
         */
        case 0x3B:
        {
            CHECKLEN(5);

            /* MFi 3.3.43 (p.173): the Apple device accepts this only
             * inside the IDPS process -- "If the accessory sends this
             * command while the Apple device is not in the IDPS
             * process, the Apple device responds with an iPodAck
             * command that passes a nonzero status" (3.3.43, p.173),
             * and 3.3.41 (p.160) says the same for SetFIDTokenValues.
             *
             * Neither checked. An EndIDPS with no StartIDPS before it
             * was accepted outright: it set device.auth.idps, which
             * makes DEVICE_TRANSID_ACTIVE true for every lingo, so
             * every later packet from an accessory that never enabled
             * transaction IDs was parsed two bytes off. */
            if (!device.auth.idps_started) {
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
                break;
            }
            uint8_t tid_hi = buf[2];
            uint8_t tid_lo = buf[3];
            uint8_t idps_status = buf[4];
#ifdef LOGF_ENABLE
            logf("iap: EndIDPS tid=%02x%02x status=%d",
                 tid_hi, tid_lo, idps_status);
#endif

            if (idps_status == 0x00) /* AccEndIDPSStatusContinue */
            {
                /* IDPSStatus (0x3C) with transID = OK */
                IAP_TX_INIT(0x00, 0x3C);
                IAP_TX_PUT(tid_hi);
                IAP_TX_PUT(tid_lo);
                IAP_TX_PUT(0x00); /* IDPSStatusOK */
                iap_send_tx();

                /* Set up device and start auth.
                 * Do NOT send GetDevAuthenticationInfo (0x14) here because
                 * tx_buf is shared and usb_drv_send_nonblocking hasn't
                 * finished sending IDPSStatus (0x3C) yet.
                 * The periodic handler will send 0x14 on the next tick.
                 */
                /* Save IDPS-parsed fields before reset clears them */
                uint32_t saved_lingoes = device.idps_lingoes;
                uint32_t saved_options = device.idps_options;
                uint32_t saved_deviceid = device.idps_deviceid;
                uint16_t saved_max_payload = device.acc_max_payload;

                iap_reset_device(&device);
                if (saved_lingoes)
                    device.lingoes = saved_lingoes;
                else {
                    device.lingoes = BIT_N(0x00) | BIT_N(0x03);
                    if (LINGO_SUPPORTED(0x0A))
                        device.lingoes |= BIT_N(0x0A);
                }
                device.idps_lingoes = saved_lingoes;
                device.idps_options = saved_options;
                device.idps_deviceid = saved_deviceid;
                device.acc_max_payload = saved_max_payload;

                if (device.lingoes & BIT_N(0x05))
                    iap_high_power_arm();

#if CONFIG_TUNER
                /* The tuner state the lingoes imply, derived here
                 * rather than where the token was parsed.
                 *
                 * radio_present was set during the parse and then wiped
                 * by the iap_reset_device() above, which the restore
                 * block did not put back -- so an accessory that
                 * declared the RF Tuner lingo through IDPS got lingo
                 * 0x07 service while the tuner driver answered
                 * RADIO_PRESENT = 0 and the radio screen never
                 * appeared. And tuner_caps_pending was only ever set on
                 * the IdentifyDeviceLingoes path, so an IDPS accessory
                 * was never asked what it could do either.
                 *
                 * Both follow from device.lingoes, so they belong after
                 * the restore that decides it, in one place. */
                if (device.lingoes & BIT_N(0x07)) {
                    radio_present = 1;
                    device.tuner_caps_pending = true;
                }
#endif
                device.do_power_notify = true;
                device.auth.idps = true;
                device.auth.state = AUST_INIT;
                iap_wake();
            }
            else if (idps_status == 0x01) /* Reset */
            {
                /* MFi p.173: "The accessory asks to reset all IDPS
                 * information it has sent to the Apple device." This
                 * answered and discarded nothing, so the lingoes,
                 * options and device ID from the attempt being
                 * abandoned survived into the next one. */
                device.idps_lingoes = 0;
                device.idps_options = 0;
                device.idps_deviceid = 0;
                device.acc_max_payload = 0;

                IAP_TX_INIT(0x00, 0x3C);
                IAP_TX_PUT(tid_hi);
                IAP_TX_PUT(tid_lo);
                IAP_TX_PUT(0x04); /* IDPSStatusTimeLimitNotExceeded */
                iap_send_tx();
            }
            else if (idps_status == 0x02) /* Abandon */
            {
                IAP_TX_INIT(0x00, 0x3C);
                IAP_TX_PUT(tid_hi);
                IAP_TX_PUT(tid_lo);
                IAP_TX_PUT(0x06); /* IDPSStatusWillNotAccept */
                iap_send_tx();
                /* Table 3-99 (p.175) pairs status 6 with
                 * accEndIDPSStatus 2 alone, and it means "the accessory
                 * may send IdentifyDeviceLingoes but not StartIDPS". */
                device.auth.idps_started = false;
            }
            else if (idps_status == 0x03) /* Restarting on another transport */
            {
                /* Table 3-97 (p.173): "The accessory has finished with
                 * IDPS on the current transport and is restarting IDPS
                 * on another transport. The accessory must cease
                 * traffic on the old transport immediately and restart
                 * IDPS on the new transport within 2 seconds."
                 *
                 * This used to fall into the Abandon arm and answer 6,
                 * which forbids the very StartIDPS the accessory is
                 * obliged to send within two seconds. Table 3-99
                 * (p.175) defines no IDPSStatus for accEndIDPSStatus 3
                 * at all, and the accessory stops listening here
                 * anyway, so say nothing -- and let go of the IDPS
                 * state so its StartIDPS on the new transport is
                 * accepted. */
                device.auth.idps_started = false;
                device.auth.idps = false;
            }
            else /* Reserved */
            {
                /* MFi 3.3.43 (p.173): "If the Apple device is in the
                 * IDPS process and the accessory sends this command
                 * with an unsupported accEndIDPSStatus value, the Apple
                 * device remains in the IDPS process." So acknowledge
                 * the bad parameter and change nothing. */
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            }
            break;
        }

        /* RequestTransportMaxPayloadSize (0x11)
         *
         * Accessory requests the maximum allowable payload size per
         * packet using the current iAP transport.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x11
         * 0x02-0x03: Transaction ID
         *
         * Returns:
         * ReturnTransportMaxPayloadSize (0x12)
         */
        case 0x11:
        {
            /* Read a transaction ID only if the packet carries one.
             * This assumed one unconditionally, so a legacy accessory
             * failed CHECKLEN(4) on the correct two-byte form and got
             * Bad Parameter, then a four-byte form was answered with
             * the ID bytes still in place -- a four-byte payload where
             * RetTransportMaxPayloadSize has two. A conformant legacy
             * accessory could never learn the payload size and sized
             * its frames from its own default instead. Same bug 0x4B
             * had; 0x11 was missed. */
            CHECKLEN(L0_MINLEN(2));

            IAP_TX_INIT(0x00, 0x12);
            L0_TX_TRANSID();
            /* Max payload size as a big-endian uint16. MFi 3.3.14
             * (p.132) says an accessory assumes 506 if the device
             * answers Bad Parameter. We report 0x00FF for the USB HID
             * full-speed transport. */
            IAP_TX_PUT(0x00);
            IAP_TX_PUT(0xFF);
            iap_send_tx();
            break;
        }

        /* GetiPodOptionsForLingo (0x4B)
         *
         * Accessory queries per-lingo option bits for a given lingo.
         *
         * Packet format (offset in buf[]: Description)
         * 0x00: Lingo ID: General Lingo, always 0x00
         * 0x01: Command, always 0x4B
         * 0x02-0x03: Transaction ID, only under IDPS
         * 0x02+off: Lingo to query options for
         *
         * MFi Table 3-130 (p.191) gives the payload as a single LingoID
         * byte; the transaction ID appears only once IDPS has enabled
         * it (2.6.1.4). Requiring five bytes unconditionally rejected
         * every legacy accessory with a nonzero iPodAck -- which
         * 3.3.55 defines as "that lingo is not listed in Table 3-132 or
         * is not supported by the Apple device on the port being used",
         * so the accessory concluded the lingo was unavailable.
         *
         * Returns:
         * RetiPodOptionsForLingo (0x4C)
         */
        case 0x4B:
        {
            unsigned int off = L0_DOFF();

            CHECKLEN(L0_MINLEN(3));
            uint8_t lingo = buf[0x02 + off];

            /* Table 3-132 (p.194) Display Remote bit 01, Absolute Volume,
             * is left clear. That decision stands and is on the
             * rejected list; the reason recorded here did not. It said
             * GetiPodStateInfo "has no 0x10 arm at all", which stopped
             * being true when iap-lingo3.c gained one -- Set, Get and
             * the 0x10 event are all implemented now. What keeps the
             * bit clear is a product judgement, not an absence. */
            uint32_t opt_lo = 0;

            /* A lingo this device does not have gets a refusal, not an
             * answer.
             *
             * MFi 3.3.55 (p.191): "If the accessory requests options
             * for any other lingo and the Apple device returns a
             * nonzero iPodAck, that lingo is not listed in Table 3-132
             * (page 192) or is not supported by the Apple device on the
             * port being used; no RetiPodOptionsForLingo command will
             * be returned."
             *
             * Every byte drew a RetiPodOptionsForLingo naming it with
             * an empty option word, so "0x0C, no options" and "there is
             * no 0x0C here" were the same answer -- and an accessory
             * walking the lingoes to see what it can use was told all
             * 256 exist. The sibling RequestLingoProtocolVersion does
             * this test; this one did not. */
            /* lingo < 32 as well as LINGO_SUPPORTED(), because that
             * macro indexes lingo_versions[] with "& 0x1f" -- so 0x20
             * aliases the General lingo, 0x24 aliases Extended
             * Interface, and 48 of the 256 possible bytes answered as
             * though they named a lingo this device has. The sibling
             * at RequestLingoProtocolVersion tests the range for the
             * same reason. */
            if (lingo >= 32 || !LINGO_SUPPORTED(lingo)) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            if (lingo == 0x00) {
                /* Table 3-132 (p.192) bit 00 is "Line out usage", and we
                 * do support it: GetiPodPreferences answers class 0x03
                 * with line out enabled. Table 3-56's note (p.152) says
                 * "Class ID 0x03 is available only if the Apple device
                 * supports line-out usage", so answering zero here
                 * contradicted the rest of the implementation. */
                opt_lo = BIT_N(0);
            } else if (lingo == 0x02) {
                /* Table 3-132 (p.194): Simple Remote bit 00 is
                 * "Context-specific controls" and bit 01 is "Audio
                 * media controls". iap-lingo2.c implements both --
                 * ContextButtonStatus (0x00) and AudioButtonStatus
                 * (0x04) -- and this answered zero for the lingo.
                 *
                 * MFi 2.3.3 (p.97) says why that matters: the accessory
                 * calls GetiPodOptionsForLingo "so that the accessory
                 * does not try to declare and use features that the
                 * device cannot handle". One reading all-zero here
                 * concludes the transport buttons are unsupported and
                 * never sends either command. */
                opt_lo = BIT_N(0) | BIT_N(1);
            } else if (lingo == 0x03) {
                opt_lo = BIT_N(0);      /* UI Volume control */
            }

            IAP_TX_INIT(0x00, 0x4C);
            L0_TX_TRANSID();
            IAP_TX_PUT(lingo);
            IAP_TX_PUT_U32(0x00);
            IAP_TX_PUT_U32(opt_lo);
            iap_send_tx();
            break;
        }

        /* SetEventNotification (0x49)
         *
         * Accessory registers for asynchronous event notifications.
         * Payload is a 64-bit big-endian notification bitmask (MFi
         * spec Table 3-111). "The Apple device acknowledges this
         * command with a General Lingo iPodAck command reporting
         * Status OK (0x00)" (spec 3.3.53), so ACK OK; we do not
         * generate any of the notifications yet.
         *
         * No CHECKAUTH: per MFi spec Table 2-7 the General lingo
         * range 0x46-0x4C requires no authentication on UART.
         */
        case 0x49:
        {
            /* Equivalent mutant: the arm below refuses this command
             * whatever its length, with the same Bad Parameter this
             * check would send, so removing it changes nothing
             * observable. Kept so the shape matches every other command
             * in the file and so the length is stated where a reader
             * looks for it, and recorded so the sweep's survivor list
             * stays fully accounted for. */
            CHECKLEN(L0_MINLEN(2) + 8);
            /* Bad Parameter, deliberately.
             *
             * MFi 1.4.1 (p.48) says what each answer promises: "If the
             * Apple device responds with a successful iPodAck command,
             * it means that it supports Flow Control notifications and
             * the accessory should be prepared to handle them", while a
             * Bad Parameter "means that it does not support
             * notifications".
             *
             * This firmware never sends iPodNotification (0x00/0x4A) --
             * there is no IAP_TX_INIT for it anywhere -- so acking
             * Success told the accessory to expect notifications that
             * cannot arrive. Flow Control is how an accessory learns the
             * receive buffer is full, and iap_getc() does silently drop
             * frames on overflow, so the false claim is the harmful
             * direction.
             *
             * It still goes through cmd_ack(), which carries the
             * transaction ID, so this can never be the four-byte form
             * MFi 2.6.1.2 makes a teardown. */
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }

        case 0x50:
        {
            unsigned int off = L0_DOFF();
            unsigned char lingo;
            uint16_t command;

            CHECKAUTH;
            CHECKLEN(L0_MINLEN(7));
            if (!l0_has_tid) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            lingo = buf[2 + off];
            command = get_u16(&buf[3 + off]);
            if (!((lingo == 0x03 && command == 0x0018)
                  || (lingo == 0x04 && command == 0x0010))) {
                cmd_ack(cmd, IAP_ACK_BAD_PARAM);
                break;
            }

            if (iap_artwork_cancel(lingo, command,
                                   buf[5 + off], buf[6 + off]))
                cmd_ok(cmd);
            else
                cmd_ack(cmd, IAP_ACK_CMD_FAILED);
            break;
        }

        /* The default response is IAP_ACK_BAD_PARAM */
        default:
        {
#ifdef LOGF_ENABLE
            logf("iap: Unsupported Mode00 Command");
#endif
            cmd_ack(cmd, IAP_ACK_BAD_PARAM);
            break;
        }
    }
}
