# iAP protocol conformance tests

Host-side tests for the accessory protocol in `apps/iap`. They compile the
real protocol sources unmodified, feed synthetic accessory packets in, and
assert on the exact bytes that come back out.

```sh
cd apps/iap/test
make test-both         # run both targets plus HID, end-to-end, and UART tests
./iap_test-ipod6g volume
```

Exit status is non-zero if any case fails.

## Why this exists

Most of what the accessory code gets wrong is wrong at the byte level: a
missing transaction ID shifting every field by two, a length field in the
wrong form, a volume scale that overflows. None of that is visible from
reading the source with confidence, and the hardware to check it against is
mostly unavailable. These tests make the wire format arguable from evidence.

Expectations come from the **MFi Accessory Firmware Specification R46**
(2012-09-12), cross-checked against the [oandrew/ipod](https://github.com/oandrew/ipod)
Go implementation. Every file header cites the governing section, and
individual cases cite the table they encode. When a case fails it prints the
expected and actual bytes side by side.

## How it is wired

```
apps/iap/*.c            compiled unmodified for the host
  ^
  | iap_transport_send  reassigned to capture every framed packet
  v
iap_test.c              framing, RX injection, IDPS bring-up, assertions
rb_stubs.c              the rest of Rockbox, as controllable fakes
stubs/                  shadow headers for the platform layer only
```

`stubs/` deliberately shadows nothing protocol-relevant. `system-target.h`
and `asm/thread.h` stand in for the S5L8702 platform, and `host_prelude.h`
bridges a few freestanding-libc assumptions. Everything else -- `metadata.h`,
`playlist.h`, `settings.h`, `button-target.h` -- is the real Rockbox header,
so struct layouts and button codes are the ones the firmware uses.

The target is `ipod6g`, which selects the CS42L55 volume range. Both targets
share this code; where behaviour differs by codec the tests derive the range
from `sound_min`/`sound_max` rather than hardcoding it.

Two concessions to there being no scheduler:

- `iaptest_init()` calls `iap_malloc()` directly. On hardware the first sync
  byte makes `iap_getc()` post `IAP_EV_MALLOC` and the iAP thread answers it.
- `iaptest_button_sample()` stands in for the 100 Hz button tick, which is
  the only thing that drains `iap_repeatbtn`. `iap_handlepkt()` defers every
  packet while that counter is set.

## The accessory model

`accessory.c` models the other end of the link: whether transaction IDs
are in force, its own counter, which of its commands are still
outstanding, what the device's counter last was. It is attached by
default, and `iaptest_rx()` tells it about every packet fed in, so it
judges **every** packet the device sends in **every** test against MFi
section 2.6 -- not only the ones somebody wrote an assertion for.

That is the point. Every transaction-ID defect found in this code so far
was in a command nobody had written a case for. Run the suite against the
v5.5 sources and the model reports seven distinct classes on its own,
naming the wrong ID and citing the rule:

```
lingo 0x00 command 0x08 answered with transaction ID 0x524F, which
matches no command the accessory sent; MFi 2.6.1.1 obliges the
accessory to discard it
```

0x524F is `'R','O'` from ROCKBOX: the reply had no transaction ID, so the
accessory read the first two bytes of the name as one.

Rules encoded there come from the specification alone. Third-party
implementations are deliberately not used as authority: the closest one,
mojyack/libiap, shares an author with a Rockbox fork, so agreement with
it would be circular.

A case that deliberately breaks the accessory's own contract must call
`iapacc_detach()` and say why. The malformed sweeps do, because they send
truncated StartIDPS and IdentifyDeviceLingoes, which are exactly the
commands that drive the model's state machine; after one of those the
model and the firmware legitimately disagree and it starts rejecting
correct replies. The burst cases do too, because they drive `iap_getc()`
directly and the model never sees what was asked.

Use `iapacc_send()` rather than `iaptest_rx()` when a case cares about
transaction IDs: it allocates one from the accessory's counter and
records the command so the answer can be matched.

## Writing a case

Add the function, then list it in `cases.def`. The runner calls
`iaptest_init()` before each one, so every case starts from a freshly
attached accessory.

```c
void test_something(void)
{
    iaptest_enter_idps();                 /* or iaptest_identify_legacy() */

    IAPTEST_RX(0x00, 0x07, 0xBE, 0xEF);   /* RequestiPodName */

    EXPECT_PAYLOAD(0, 0x00, 0x08, 0xBE, 0xEF,
                   'R','O','C','K','B','O','X', 0x00);
}
```

`EXPECT_PAYLOAD` checks the checksum and the length form as well as the
bytes, so those do not need separate assertions.

Cover both accessory generations when a change touches shared code.
`iaptest_enter_idps()` completes IDPS with an IdentifyToken declaring
lingoes 0x00, 0x02, 0x03, 0x04 and 0x0A; `iaptest_identify_legacy()` uses
`IdentifyDeviceLingoes` and never enables transaction IDs. Several bugs here
were fixed in a way that would have broken the legacy path, and
`test_transid_absent_for_legacy_accessory` is what catches that.

## A case that passes before and after proves nothing

When adding a case for a bug, check it fails against the unfixed source:

```sh
cp ../iap-lingo0.c /tmp/fixed.c
git show HEAD:apps/iap/iap-lingo0.c > ../iap-lingo0.c
make clean && make && ./iap_test        # expect failures
cp /tmp/fixed.c ../iap-lingo0.c
make clean && make && ./iap_test        # expect none
```

Use a file copy rather than `git stash`; a stray `cd` makes the stash form
easy to point at the wrong repository. And clean between builds: swapping a
source under make leaves objects that only differ by timestamp, which
produces a mix of old and new code and a run that means nothing.

## Checking you did not break the accessories you cannot test

Most accessories never enable transaction IDs, and none of them are
available to test against. `golden-diff.sh` drives a legacy accessory
through a broad set of commands on two revisions and diffs every byte:

```sh
./golden-diff.sh v5.5
```

An empty diff means the legacy wire format is unchanged. A non-empty one
should contain only differences you can name and justify. Run it before
committing anything that touches a shared code path. It handles the source
swap and the cleaning itself, because doing that by hand has gone wrong
more than once.

`dump_legacy.c` is the tool underneath it; extend that when a fix touches a
command the dump does not currently exercise.

## Not covered

Four gaps, in the order they should worry you.

**Nothing here has run on hardware.** Every claim in this suite is host
simulation plus a source trace. Not one packet has crossed a real dock
connector. Treat a green run as "the wire format is arguable from
evidence", which is what the section above promises, and not as "this
works".

**Concurrency is structurally invisible.** The harness has one thread and
no interrupts, so a race between the iAP thread, the UART receive
interrupt, the 100 Hz tick and the USB thread cannot be expressed here.
Both races found so far were found by reading. `apps/iap` has 35
file-scope variables written from two or more execution contexts; the
detach-from-a-tick pattern accounts for most of them and is safe only
because every write in `iap_reset_device()` is a single word. Nothing
enforces that.

**`firmware/drivers/tuner/ipod_remote_tuner.c` has no mutation sites.**
`mutate.py` lists it in `SOURCES` and matches nothing in it: every rule
is keyed on an `apps/iap` idiom. The 5G tuner path is the least verified
code in scope.

**Authentication coverage is protocol-only.** The suite exercises both
versions, transaction IDs, phases, and timeouts with synthetic certificate
and signature data, but the firmware still performs no cryptographic
certificate or signature verification.

The button driver, the backlight stack and `apps/action.c` are out of
scope, so the accessory carve-out from the first-keypress backlight
filter rests on a source trace.

## Decisions on record

Things a review round will want to change, that were considered and
deliberately left. Each is argued at the cited line; this list exists so
the argument is not had a third time.

**Spec-literate but product-wrong.** Advertising the Absolute Volume
capability bit; blanket authentication gating on Extended Interface;
refusing a tokenless `EndIDPS`, or granting only the General lingo to an
accessory that completes IDPS without an `IdentifyToken`; refusing
unsupported `iPodPreferenceToken` preference classes (real accessories
disconnect); answering `IDPSStatus` 7 for the line-out/video
preference-vs-capability inconsistency. Each breaks working accessories
for no observable gain.

**Not our obligation.** Every R46 command carries an `Origin:` line, and
this device is the Apple device. "The accessory may send
`IdentifyDeviceLingoes` but not `StartIDPS` after IDPS status 6" is the
accessory's rule. A whole class of false findings came from adopting one.

**Simple Remote keeps the button path** while Extended Interface's
`PlayControl` was converted to Playback Engine calls. On the Apple Radio
Remote the same `BUTTON_RC_RIGHT` is next track in the WPS and next
station in the radio screen; `PlayControl` carries no such ambiguity
(MFi 5.1.37, p.428). Tried, measured against
`test_integration_apple_radio_remote`, reverted — see `iap-lingo2.c`.

**`audio_skip()` takes an MFi-space delta**, not a Rockbox one.
`apps/playlist.c`'s `get_next_index()` under `REPEAT_OFF` computes
`rotate_index(playlist->index) + steps`, and `rotate_index()` is
literally `iap_track_to_mfi()`. Two rounds disagreed; the code settles
it. Under `REPEAT_ALL` the two deltas are congruent modulo the playlist
length.

**Equivalent mutants are documented, not deleted.** Where a guard cannot
change an observable outcome the source says so and why — `iap-core.c`'s
`periodic_last_mute`, `iap-lingo4.c`'s `dbrecordcount` and
`last_selected_playlist`, the range check inside `iap_get_trackinfo()`.
A survivor in a mutation sweep with no comment beside it is a gap; one
with a comment is a decision.

**Three transport bounds in `usb_iap_hid.c` survive by design.** The
receive floor exists to stop `data[1]` being read when `len` is 1, and
that difference is a read one byte past the caller's buffer — not
observable without a canary or a sanitiser.
