#!/usr/bin/env python3
"""Ask every load-bearing line in the iAP layer whether anything notices
when it stops working.

A test suite cannot report its own coverage: it can only say that what
it checks still holds. This walks the production sources, disables one
construct at a time, rebuilds, runs all four binaries, and lists the
mutations that nobody caught. Each survivor is either a gap in the tests
or a line that does nothing.

It works on a copy of the tree under /tmp, so it is safe to run while
something else is editing the worktree -- and so a crash cannot leave a
mutation behind, which has happened.

    ./mutate.py                 every site
    ./mutate.py --list          show the sites, change nothing
    ./mutate.py --filter volume only sites whose description matches
"""

import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))

SOURCES = [
    "apps/iap/iap-core.c",
    "apps/iap/iap-lingo0.c",
    "apps/iap/iap-lingo1.c",
    "apps/iap/iap-lingo2.c",
    "apps/iap/iap-lingo3.c",
    "apps/iap/iap-lingo4.c",
    "apps/iap/iap-lingo7.c",
    "firmware/usbstack/usb_iap_hid.c",
    "firmware/drivers/tuner/ipod_remote_tuner.c",
]

# The calls that make playback happen, as opposed to the guards that
# decide whether it should. Named here so both the rule and the coverage
# audit work from one list.
PLAYBACK = (r'audio_pause|audio_resume|audio_stop|audio_skip|audio_next'
            r'|audio_prev|audio_ff_rewind|audio_flush_and_reload_tracks'
            r'|playlist_sort|playlist_randomise|playlist_start|setvol')

# (regex, replacement, what the mutation means[, function-name regex])
#
# A fourth element scopes the rule to the bodies of functions whose name
# it matches. Without it a rule that deletes "x = false;" would fire on
# every assignment in the tree; with it, the same rule asks a precise
# question of the reset path alone.
RULES = [
    # The subscription test itself, however the condition around it is
    # written. Anchored on "if (" these matched 9 of 23 Display Remote
    # gates and none at all of the nine Extended Interface ones, which
    # are "&& (...)" continuations -- so the whole play-status
    # notification family had no coverage while the audit reported
    # every guard covered. BIT_N takes hex at some sites and decimal at
    # others, which cost three more.
    (r'device\.notifications & BIT_N\((0x[0-9A-Fa-f]+|\d+)\)',
     r'0', "notification bit {0} never sent"),
    (r'device\.pb_notifications & BIT_N\((0x[0-9A-Fa-f]+|\d+)\)',
     r'0', "play-status bit {0} never sent"),
    (r'\bCHECKLEN\((\d+) \+ doff\);',
     r'CHECKLEN(0);', "length check {0}+doff removed"),
    # The same check, spelled in parameter units. A lingo converted to
    # L*_NEED() would otherwise drop silently out of the sweep.
    (r'\bL(\d)_NEED\((\d+)\);',
     r'CHECKLEN(0);', "lingo {0} length check for {1} parameters removed"),
    (r'\bCHECKLEN\(L0_MINLEN\((\d+)\)\);',
     r'CHECKLEN(0);', "length check L0_MINLEN({0}) removed"),
    # Checks written against the local payload offset rather than doff,
    # and one composed of both forms. Five guards sat outside every rule
    # in these three spellings; the coverage audit named them once it
    # was made to print lines instead of counts.
    (r'\bCHECKLEN\(off \+ (\d+)\);',
     r'CHECKLEN(0);', "length check off+{0} removed"),
    (r'\bCHECKLEN\((\d+) \+ off\);',
     r'CHECKLEN(0);', "length check {0}+off removed"),
    (r'\bCHECKLEN\(L0_MINLEN\((\d+)\) \+ (\d+)\);',
     r'CHECKLEN(0);', "length check L0_MINLEN({0})+{1} removed"),
    (r'\bCHECKAUTH;',
     r'((void)0);', "authentication check removed"),
    # The open-coded form. CHECKAUTH is a macro three lingoes use; the
    # rest write the test out, and the rule only knew the macro -- so
    # the three Simple Remote gates could each be turned off with the
    # whole suite green. Answering "yes, authenticated" is what removing
    # the gate means.
    (r'!DEVICE_AUTHENTICATED',
     r'0', "open-coded authentication check removed"),
    # Mid-handshake test, guarding the certificate machine.
    (r'!DEVICE_AUTH_RUNNING',
     r'0', "authentication-running check removed"),
    # The transaction-ID predicate. 26 sites decide packet layout on it
    # and no rule reached any of them; "doff = 2" only covered the ones
    # written that way.
    (r'\bDEVICE_TRANSID_ACTIVE\b',
     r'0', "transaction IDs treated as absent"),
    # The "is this parameter present" tests, the optional-field
    # counterpart of L*_NEED.
    (r'\bL(\d)_HAVE\((\d+)\)',
     r'1', "lingo {0} optional-parameter test for {1} forced true"),
    # The absolute form, with no doff term: the prologue checks that
    # bound a handler before it knows its offset. No rule covered these
    # and the coverage audit below is what said so.
    (r'\bCHECKLEN\((\d+)\);',
     r'CHECKLEN(0);', "absolute length check for {0} bytes removed"),
    # The token itself, with nothing said about the condition around it.
    # Answering "yes" disables both polarities at once: a "!X" refusal
    # stops refusing and an "if (X)" gate stops gating. Keyed on the
    # written conditions instead, this rule missed the digital audio
    # gate (an authentication test on the same line), lingo 2's (a
    # command test), both of lingo 1's, and iap-core's, which spans two
    # lines. The coverage audit below is what said so, each time.
    (r'\bDEVICE_LINGO_SUPPORTED\((0x[0-9A-Fa-f]+)\)',
     r'1', "lingo {0} treated as always negotiated"),
    (r'\bdoff = 2;',
     r'doff = 0;', "transaction-ID offset forced to zero"),

    # Everything above disables a guard. These two disable an action,
    # which is the half of the layer the sweep could not see: round ten
    # found zero sites in iap_reset_device(), zero in iap_reset_state()
    # and zero among lingo 4's playback decisions, so a test could
    # assert an accessory's command was acknowledged without anything
    # asking whether it did what it said.
    #
    # A detach owes a clear for every piece of session state. Deleting
    # one asks whether any case notices that state surviving into the
    # next accessory -- which is how radio_present, device.pb_seeking
    # and the button latches were each found still set after a detach.
    (r'^[ \t]+(?:device->|device\.|auth->)?(\w+)\s*='
     r'\s*(?:false|0|0x00|NULL|BUTTON_NONE|ACCST_NONE|AUST_NONE'
     r'|IST_STANDARD|REPEAT_OFF)\s*;[ \t]*\n',
     r'', "reset no longer clears {0}",
     r'iap_reset_(?:device|state|auth|lingo\d)'),

    # And the calls that make playback happen. "while (0)" rather than
    # deleting the statement: it evaluates nothing, still mentions every
    # argument so no variable goes unused, and -- unlike "if (0)" --
    # cannot steal an else from the if above it, which two sites here
    # have.
    (r'\b(' + PLAYBACK + r')\s*\([^;]*\);',
     r'while (0) \g<0>', "call to {0}() never made"),

    # The two transport drivers, which had no sites at all: every rule
    # above is keyed on an apps/iap idiom -- CHECKLEN, L*_NEED,
    # DEVICE_*, the notification masks -- and neither file uses any of
    # them. What they do have is bounds, and a bound that stops holding
    # is how a transport corrupts a frame.
    #
    # "if (0 &&" rather than deleting the condition: it keeps the body
    # reachable to the compiler and cannot change which branch an else
    # belongs to.
    (r'if \((iap_len|len|length|size|count|n) ([<>]=?) ',
     r'if (0 && \g<1> \g<2> ', "bound on {0} disabled",
     r'iap_hid_process_rx|usb_iap_hid_send|iap_hid_tx'
     r'|rmt_tuner_set_freq|rmt_tuner_scan|reply_timeout'),
]

def uncommented(text):
    """The same text with every /* ... */ blanked to spaces.

    Offsets are preserved, so a match found here splices correctly into
    the original. Matching has to happen on this rather than on the
    source, because re.finditer does not overlap: a comment that named a
    function swallowed the real call underneath it, so
    "audio_skip() wants Rockbox\'s. */" and the audio_pause() on the next
    line were one match, and the audit then reported that audio_pause()
    as a call no rule reached. Filtering such matches out afterwards
    does not help -- the span is consumed either way. Two real sites
    were hidden like this, and fourteen rule "sites" were prose.
    """
    out = list(text)
    for m in re.finditer(r'/\*.*?\*/', text, re.S):
        for i in range(m.start(), m.end()):
            if out[i] != "\n":
                out[i] = " "
    return "".join(out)


def bodies(text, name_pat):
    """Spans of the bodies of functions whose name matches, by brace
    depth from the signature's opening brace."""
    sig = re.compile(r'^(?:static\s+)?\w[\w \t*]*\b(?:' + name_pat +
                     r')\s*\([^;{]*\)\s*\{', re.M)
    for m in sig.finditer(text):
        i = text.index("{", m.end() - 1)
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield i, j


def sites(text):
    scan = uncommented(text)
    for rule in RULES:
        pat, repl, desc = rule[0], rule[1], rule[2]
        scope = rule[3] if len(rule) > 3 else None
        spans = list(bodies(scan, scope)) if scope else None
        for m in re.finditer(pat, scan, re.M):
            if spans is not None and not any(a <= m.start() < b
                                             for a, b in spans):
                continue
            yield m.start(), m.end(), m.group(0), \
                  re.sub(pat, repl, m.group(0), flags=re.M), \
                  desc.format(*m.groups()) if m.groups() else desc

# Every construct the rules above are meant to cover, counted crudely
# and independently of them.
#
# A rule is a regex over source text, so it reports "N sites" with
# confidence and no way to say what it did not match. Three length
# checks sat outside the rules for months because they were spelled
# 0x09 rather than 9 -- the sweep counted 90 sites, said nothing, and
# those three had never been tested. This compares what the rules found
# against what is actually in the tree, so the next spelling that slips
# past is reported rather than silently skipped.
AUDIT = [
    (r'\bCHECKAUTH\b\s*;',            "auth"),
    (r'!DEVICE_AUTHENTICATED',        "auth"),
    (r'!DEVICE_AUTH_RUNNING',         "auth"),
    (r'\bDEVICE_TRANSID_ACTIVE\b',    "transid"),
    (r'\bL\d_HAVE\s*\(',              "optional"),
    (r'notifications & BIT_N\s*\(',   "notification"),
    (r'\bCHECKLEN\s*\(',              "length"),
    (r'\bL\d_NEED\s*\(',              "length"),
    (r'\bDEVICE_LINGO_SUPPORTED\s*\(', "negotiation"),
    # The action half. No audit entry for the reset-path clears: that
    # rule is scoped to four functions on purpose, and a raw token count
    # of "x = false;" over the whole tree would report every assignment
    # in it as an untested guard.
    (r'\b(?:' + PLAYBACK + r')\s*\(', "playback"),
]

# Which AUDIT tag each rule's description belongs to.
def audit_coverage(found):
    """Report guards present in the sources that no rule matched.

    Line by line, not by count. A count says six guards in this file are
    untested and gives no way to find them, which is the same shape of
    problem one level up -- the audit existed and the guards it was
    counting still went untested for weeks. Print the line and the text
    so the gap is a work item rather than a number.
    """
    print("\nrule coverage against a raw token count:")
    gaps = []
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        text = open(path).read()
        lines = text.split("\n")

        # Lines a rule already reaches.
        covered = set()
        for (r, line, _, _, _, _, _) in found:
            if r == rel:
                covered.add(line)

        for pat, cat in AUDIT:
            for m in re.finditer(pat, text):
                line = text.count("\n", 0, m.start()) + 1
                src = lines[line - 1]
                # The macro's own definition is not a guard, and
                # neither is a comment that names it -- seven of the
                # thirteen this first reported were prose about the
                # checks rather than checks.
                t = src.lstrip()
                if t.startswith("#") or t.startswith("*") \
                   or t.startswith("/*") or t.startswith("//"):
                    continue
                if line in covered:
                    continue
                gaps.append((rel, line, cat, src.strip()))

    if not gaps:
        print("  every guard the audit knows how to look for is covered")
        return 0

    for rel, line, cat, src in gaps:
        print(f"  {rel}:{line}  {cat} guard no rule matches:  {src}")
    print(f"  {len(gaps)} guard(s) the sweep never tests")
    return len(gaps)


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, shell=True,
                          capture_output=True, text=True)

# Why a build or run step failed. "The baseline does not build" with
# nothing after it is as unactionable as the coverage audit's bare
# counts were; a mutation that breaks the build is expected and gets
# counted, but a baseline that does not is a stop-everything, so say
# what happened.
_why_on = [True]

def _why(what, output):
    if not _why_on[0]:
        return
    print(f"  {what}")
    tail = [l for l in output.strip().split("\n") if l.strip()][-12:]
    for l in tail:
        print(f"    | {l}")


def failures(tree):
    """Total failures across all four binaries, or None if a build broke."""
    t = os.path.join(tree, "apps/iap/test")
    total = 0
    # All six binaries. The transport suites gained a per-target suffix
    # when they started building against both configs; leaving the old
    # names here made the baseline "fail to build" rather than run.
    for cmd, exe in (
            ("make TARGET=ipod6g",             "./iap_test-ipod6g"),
            ("make TARGET=ipodvideo",          "./iap_test-ipodvideo"),
            ("make -C hid TARGET=ipod6g",      "hid/hid_test-ipod6g"),
            ("make -C hid TARGET=ipodvideo",   "hid/hid_test-ipodvideo"),
            ("make -C e2e TARGET=ipod6g",      "e2e/e2e_test-ipod6g"),
            ("make -C e2e TARGET=ipodvideo",   "e2e/e2e_test-ipodvideo"),
            # make test-both runs this and the sweep did not, so a
            # mutation that only the dock UART suite catches counted as
            # a survivor.
            ("make -C uart",                   "uart/uart_test")):
        r = run(cmd, t)
        if r.returncode != 0:
            _why(f"{cmd} exited {r.returncode}", r.stdout)
            return None
        r = run(exe, t)
        m = re.search(r'(\d+) failures?', r.stdout)
        if not m:
            _why(f"{exe} printed no failure count "
                 f"(exit {r.returncode})", r.stdout)
            return None
        total += int(m.group(1))
    return total

def main():
    argv = sys.argv[1:]
    listing = "--list" in argv
    filt = None
    if "--filter" in argv:
        filt = argv[argv.index("--filter") + 1]

    # Two lists. The audit compares the rules against the whole tree, so
    # it has to see every site; the sweep runs what the filter asked
    # for. Handing the filtered list to audit_coverage() made it report
    # every guard in every other file as one no rule matches -- 217 of
    # them for a single-file filter, which is the documented workflow.
    # A count with no way to find the things it counts is exactly the
    # failure the audit was written to replace.
    every, found = [], []
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        text = open(path).read()
        for a, b, old, new, desc in sites(text):
            line = text.count("\n", 0, a) + 1
            site = (rel, line, a, b, old, new, desc)
            every.append(site)
            if filt and filt not in desc and filt not in rel:
                continue
            found.append(site)

    if filt:
        print(f"{len(found)} mutation sites matching {filt!r}, "
              f"of {len(every)} in the tree")
    else:
        print(f"{len(found)} mutation sites")

    # Per-file, and loudly for a file with none.
    #
    # audit_coverage() below compares the rules against a raw token
    # count, and a file containing none of those tokens produces no
    # gaps -- so it printed "every guard is covered" while
    # usb_iap_hid.c and ipod_remote_tuner.c, both named in SOURCES, had
    # zero sites between them. A sweep cannot report completeness over
    # files it never touches.
    per = {}
    for rel, *_ in every:
        per[rel] = per.get(rel, 0) + 1
    print("\nsites per source:")
    empty = []
    for rel in SOURCES:
        n = per.get(rel, 0)
        print(f"  {n:4d}  {rel}")
        if n == 0 and os.path.exists(os.path.join(ROOT, rel)):
            empty.append(rel)
    if empty:
        print("\n  NOT SWEPT -- these are in SOURCES and no rule "
              "matches anything in them:")
        for rel in empty:
            print(f"    {rel}")

    audit_coverage(every)
    if listing:
        for rel, line, _, _, old, _, desc in found:
            print(f"  {rel}:{line}  {desc}")
        return 0

    tree = tempfile.mkdtemp(prefix="iapmut-")
    print(f"working in {tree}")
    # A copy, so nothing here can touch the real tree.
    run(f"cp -R '{ROOT}/' '{tree}/'", "/")
    run("make clean", os.path.join(tree, "apps/iap/test"))

    base = failures(tree)
    if base is None:
        print("  the baseline does not build; nothing to compare against")
        return 1
    if base != 0:
        print(f"  the baseline already has {base} failures; fix those first")
        return 1
    print("  baseline is clean\n")

    _why_on[0] = False          # past the baseline, breakage is a result
    survivors, killed, broke = [], 0, 0
    for i, (rel, line, a, b, old, new, desc) in enumerate(found, 1):
        p = os.path.join(tree, rel)
        orig = open(p).read()
        open(p, "w").write(orig[:a] + new + orig[b:])
        run("make clean", os.path.join(tree, "apps/iap/test"))
        n = failures(tree)
        open(p, "w").write(orig)

        if n is None:
            broke += 1
            mark = "build"
        elif n > 0:
            killed += 1
            mark = f"{n:4d}"
        else:
            survivors.append((rel, line, desc))
            mark = "LIVE"
        print(f"  [{i:3d}/{len(found)}] {mark}  {rel}:{line}  {desc}")

    print(f"\n{killed} caught, {len(survivors)} survived, {broke} did not build")
    if survivors:
        print("\nSurvivors -- each is a gap in the tests or a line that does nothing:")
        for rel, line, desc in survivors:
            print(f"  {rel}:{line}  {desc}")
    shutil.rmtree(tree, ignore_errors=True)
    return 1 if survivors else 0

if __name__ == "__main__":
    sys.exit(main())
