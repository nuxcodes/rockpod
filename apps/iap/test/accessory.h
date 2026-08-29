/***************************************************************************
 * A model of a conformant accessory.
 *
 * The per-case assertions elsewhere check the bytes a particular command
 * produces. They cannot catch a rule that is violated somewhere nobody
 * wrote a case for, and every transaction-ID defect found in this code
 * so far was of exactly that shape.
 *
 * This models the other end of the link instead. It keeps the state a
 * real accessory keeps -- whether transaction IDs are enabled, its own
 * counter, which of its commands are still outstanding, what the device's
 * counter last was -- and checks every packet the device sends against
 * the rules in MFi R46 section 2.6. Attach it once and every test gets
 * that validation, including the malformed sweeps and the bursts.
 *
 * The rules encoded here come from the specification only. Third-party
 * implementations are not used: the closest one shares an author with a
 * Rockbox fork, so agreement with it would be circular.
 ****************************************************************************/
#ifndef IAP_ACCESSORY_H
#define IAP_ACCESSORY_H

#include <stdbool.h>

/* Attach or detach the model. Detached, packets are still captured but
 * not judged, which some cases want while they set up. */
void iapacc_attach(void);

/* Answer the device's questions as well as judging them. Off by default,
 * because a case that drives an exchange by hand does not want the model
 * replying underneath it. Turn it on for the integration flows, where the
 * point is that the whole conversation completes. */
void iapacc_autorespond(bool on);

/* Deliver the replies queued while the device was transmitting. Returns
 * how many went out. Call after each step of a flow. */
int iapacc_pump(void);

/* How many replies the responder has produced. A case that turns
 * autorespond on and never reaches a question the responder answers is
 * not testing what it claims to; assert on this. */
int iapacc_responses_sent(void);
void iapacc_detach(void);

/* Forget everything, as if the accessory had been unplugged. */
void iapacc_reset(void);

/* Send a command as the accessory would.
 *
 * `cmd` is the lingo byte, the command id (one byte, or two for the
 * Extended Interface lingo) and the parameters. The model inserts a
 * transaction ID in the right place when its own state says one is
 * required, allocates it from its counter, and records the command as
 * outstanding so a later response can be matched to it.
 *
 * This is what a test should use in preference to iaptest_rx() when it
 * cares about transaction IDs, because iaptest_rx() sends the bytes
 * verbatim and the model then has no idea what was asked.
 */
void iapacc_send(const unsigned char *cmd, int len);

#define IAPACC_SEND(...) do { \
        static const unsigned char _c[] = { __VA_ARGS__ }; \
        iapacc_send(_c, (int)sizeof(_c)); \
    } while (0)

/* Drive the identification the model understands, so its transaction-ID
 * state matches the device's. */
void iapacc_identify_idps(const unsigned char *lingoes, int n);
void iapacc_identify_legacy(unsigned long lingo_mask);

/* True once the model believes transaction IDs are in force. A test can
 * assert this to catch the device tearing them down behind its back. */
bool iapacc_transactions_enabled(void);

/* Violations seen since the last reset, and a one-line summary of the
 * first. */
int iapacc_violations(void);
const char *iapacc_first_violation(void);

/* Called by the capture hook for every packet the device transmits.
 * Tests do not call this directly. */
void iapacc_observe(const unsigned char *payload, int paylen);

/* How many device replies the model has judged since the last reset,
 * and whether it is judging at all. Zero in a case that means to be
 * judged is a harness defect running the tests cannot reveal. */
int  iapacc_judged(void);
bool iapacc_is_attached(void);

/* Called by iaptest_rx() for every packet fed in verbatim, so the model
 * can follow a test that builds its own bytes rather than going through
 * iapacc_send(). Tests do not call this directly. */
void iapacc_note_sent(const unsigned char *payload, int paylen);

#endif /* IAP_ACCESSORY_H */
