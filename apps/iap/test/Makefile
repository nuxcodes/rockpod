# Host-side iAP protocol conformance tests.
#
#   cd apps/iap/test && make && ./iap_test
#
# The iAP sources under apps/iap are compiled unmodified for the host.
# Everything else Rockbox provides is stubbed in rb_stubs.c, so these
# tests assert on the exact bytes the protocol layer puts on the wire.

ROOT    := ../../..

CC      ?= cc

# Two targets, because they do not compile the same protocol sources.
#
#   make            ipod6g   -- the reference target. CS42L55 volume range,
#                               USB_ENABLE_IAP_HID, USB_ENABLE_AUDIO, and
#                               no CONFIG_TUNER.
#   make TARGET=ipodvideo    -- PP502x, WM8758 volume range, and
#                               CONFIG_TUNER, which is the only way
#                               iap-lingo7.c (RF Tuner) gets compiled at
#                               all. Three tuner fixes in this series were
#                               invisible to the suite until this existed.
#
#   make both       runs each in turn.
#
TARGET ?= ipod6g

ifeq ($(TARGET),ipod6g)
  TARGET_DEFS := -DIPOD_6G
  TARGET_INC  := \
	-I$(ROOT)/firmware/target/arm/s5l8702/ipod6g \
	-I$(ROOT)/firmware/target/arm/s5l8702 \
	-I$(ROOT)/firmware/target/arm
  # The header the include path must resolve to, and the one it must not.
  WANT_BUTTON_HDR := s5l8702/ipod6g/button-target.h
  DENY_BUTTON_HDR := target/arm/ipod/button-target.h
else ifeq ($(TARGET),ipodvideo)
  TARGET_DEFS := -DIPOD_VIDEO
  # Mirrors TARGET_INC in build-hw-ipodvideo/Makefile.
  TARGET_INC  := \
	-I$(ROOT)/firmware/target/arm/ipod/video \
	-I$(ROOT)/firmware/target/arm/ipod \
	-I$(ROOT)/firmware/target/arm/pp \
	-I$(ROOT)/firmware/target/arm
  WANT_BUTTON_HDR := target/arm/ipod/button-target.h
  DENY_BUTTON_HDR := s5l8702/ipod6g/button-target.h
else
  $(error unknown TARGET '$(TARGET)', expected ipod6g or ipodvideo)
endif

BUILD   := $(ROOT)/build-hw-$(TARGET)

# MEMORYSIZE comes from the build directory's Makefile (both targets use
# 64), not from autoconf.h, so -Wundef sees it missing in a dozen headers.
TARGET_DEFS += -DMEMORYSIZE=64 -D_FORTIFY_SOURCE=0

INCLUDES := \
	-Istubs \
	-I$(BUILD) \
	-I$(ROOT)/apps \
	-I$(ROOT)/apps/gui \
	-I$(ROOT)/apps/iap \
	-I$(ROOT)/apps/recorder \
	-I$(ROOT)/firmware/export \
	-I$(ROOT)/firmware/include \
	-I$(ROOT)/firmware/common \
	-I$(ROOT)/firmware/drivers \
	-I$(ROOT)/firmware/kernel/include \
	$(TARGET_INC) \
	-I$(ROOT)/lib/rbcodec \
	-I$(ROOT)/lib/rbcodec/metadata \
	-I$(ROOT)/lib/rbcodec/dsp \
	-I$(ROOT)/lib/skin_parser \
	-I$(ROOT)/lib/tlsf/src \
	-I.

# -Wundef matters more than it looks: cases.def switches the RF Tuner
# cases on #if CONFIG_TUNER, and without config.h in scope that is a
# silent 0. Nine cases vanished from the ipodvideo run and the totals
# looked plausible. Rockbox builds with -Wundef for the same reason.
CFLAGS  := -std=gnu99 -g -O0 -fno-builtin -Wall -Wundef -MMD -MP \
	   -Wno-unused-parameter -Wno-sign-compare \
	   $(TARGET_DEFS) $(INCLUDES) \
	   -include stubs/host_prelude.h

# Protocol sources under test, compiled unmodified.
# firmware/sound.c is compiled for real, not stubbed. sound_min() and
# sound_max() are what the volume conversion scales against, and a stub
# returning the test's own constants made the codec-range case a
# tautology: an audit changed cs42l55.h to the wrong codec's range and
# every volume test still passed.
IAP_SRCS := \
	$(ROOT)/firmware/sound.c \
	$(ROOT)/apps/iap/iap-core.c \
	$(ROOT)/apps/iap/iap-artwork.c \
	$(ROOT)/apps/iap/iap-lingo0.c \
	$(ROOT)/apps/iap/iap-lingo1.c \
	$(ROOT)/apps/iap/iap-lingo2.c \
	$(ROOT)/apps/iap/iap-lingo3.c \
	$(ROOT)/apps/iap/iap-lingo4.c

# iap-lingo7.c is guarded by CONFIG_TUNER, so it only exists on ipodvideo.
# Listing it unconditionally would fail to link on ipod6g, where every
# function in it compiles away.
ifeq ($(TARGET),ipodvideo)
IAP_SRCS += $(ROOT)/apps/iap/iap-lingo7.c
# The tuner driver is compiled for real too: it holds the sixth volume
# conversion site, which reported silence at maximum volume until this
# series fixed it, and no stub would have caught that.
IAP_SRCS += $(ROOT)/firmware/drivers/tuner/ipod_remote_tuner.c
TEST_SRCS_EXTRA := test_tuner.c
endif

# Harness and cases.
TEST_SRCS := \
	iap_test.c \
	rb_stubs.c \
	accessory.c \
	cases.c \
	test_transaction.c \
	test_volume.c \
	test_buttons.c \
	test_playcontrol.c \
	test_framing.c \
	test_malformed.c \
	test_longtags.c \
	test_session.c \
	test_modes.c \
	test_integration.c \
	test_auth_gate.c \
	test_lengths.c \
	test_chapters.c \
	test_artwork.c \
	$(TEST_SRCS_EXTRA)

OBJDIR  := obj-$(TARGET)
IAP_OBJS  := $(patsubst $(ROOT)/apps/iap/%.c,$(OBJDIR)/iap_%.o,\
               $(patsubst $(ROOT)/firmware/%.c,$(OBJDIR)/iap_%.o,$(IAP_SRCS)))
TEST_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(TEST_SRCS))

BIN  := iap_test-$(TARGET)
DUMP := dump_legacy-$(TARGET)

.PHONY: all both test test-both hid e2e uart dump ubsan check-includes clean
all: check-includes $(BIN)

# ipod6g resolves button-target.h from firmware/target/arm/s5l8702/ipod6g.
# firmware/target/arm/ipod is the PP502x tree that ipodvideo uses, and the
# two files are byte-identical apart from an $$Id$$ line, so nothing in C
# can tell them apart. Pointing the include path at the wrong one made the
# BUTTON_REMOTE cases pass against the iPod Video header while the
# Classic's stayed broken. Check the path the preprocessor actually took.
check-includes:
	@dep=$$($(CC) $(CFLAGS) -M $(ROOT)/apps/iap/iap-lingo2.c 2>/dev/null); echo "$$dep" | grep -q '$(WANT_BUTTON_HDR)' || { echo "iap/test: $(TARGET) button-target.h did not resolve to $(WANT_BUTTON_HDR)" >&2; echo "$$dep" | tr ' ' '\n' | grep button-target >&2; exit 1; }; echo "$$dep" | grep -q '$(DENY_BUTTON_HDR)' && { echo "iap/test: $(TARGET) has the wrong button-target.h on the include path ($(DENY_BUTTON_HDR))" >&2; exit 1; }; true

test: check-includes $(BIN)
	./$(BIN)

# The USB HID transport's transmit path, a separate binary because it
# needs different stubs for the same names. See hid/README-ish header.
hid:
	$(MAKE) -C hid test-both

# The protocol layer talking through the real transport, in HID reports
# rather than through a capture hook. Separate again because it needs
# -Dstatic= on usb_iap_hid.c only.
e2e:
	$(MAKE) -C e2e test-both

# The PP dock UART -- the iPod Video's iAP transport, which the HID
# suites do not touch because that target does not build the HID one.
uart:
	$(MAKE) -C uart test

# Both targets, in turn. A failure in either fails the run.
both:
	$(MAKE) TARGET=ipod6g
	$(MAKE) TARGET=ipodvideo

test-both:
	$(MAKE) TARGET=ipod6g test
	$(MAKE) TARGET=ipodvideo test
	$(MAKE) -C hid test-both
	$(MAKE) -C e2e test-both
	$(MAKE) -C uart test

# Undefined-behaviour build. Catches the signed-overflow and bad-shift
# class that the byte assertions cannot see. AddressSanitizer and lldb
# are both blocked on this machine by the endpoint security agent, so
# this is the sanitizer that is actually available.
ubsan:
	$(MAKE) clean
	$(MAKE) CC="$(CC) -fsanitize=undefined -fno-sanitize-recover=all"
	./$(BIN)

# Golden-output tool: prints the legacy accessory wire format so two
# revisions can be diffed. See README.md.
dump: check-includes $(DUMP)

$(DUMP): $(IAP_OBJS) $(OBJDIR)/iap_test_nomain.o $(OBJDIR)/rb_stubs.o \
        $(OBJDIR)/accessory.o $(OBJDIR)/dump_legacy.o
	$(CC) -o $@ $^

# The harness carries the test runner's main(); compile a copy without it.
$(OBJDIR)/iap_test_nomain.o: iap_test.c | $(OBJDIR)
	$(CC) $(CFLAGS) -Dmain=iaptest_unused_main -c $< -o $@

$(BIN): $(IAP_OBJS) $(TEST_OBJS)
	$(CC) -o $@ $^

$(OBJDIR)/iap_sound.o: $(ROOT)/firmware/sound.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/iap_%.o: $(ROOT)/apps/iap/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# -Dstatic= here so a case can read tuner_frequency and check that a
# payload was decoded from the right offset. Asserting only radio_tuned
# could not: rmt_tuner_freq() sets it whatever the bytes say, so a
# frequency taken from the transaction ID looked exactly like a good
# one. Scoped to this driver -- it externalises file-local names, and
# on firmware/sound.c that collides with the generated converters.
$(OBJDIR)/iap_drivers/tuner/%.o: $(ROOT)/firmware/drivers/tuner/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Dstatic= -c $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Rebuild when a Rockbox header changes, not just a .c file.
-include $(IAP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

clean:
	$(MAKE) -C hid clean
	$(MAKE) -C e2e clean
	# uart was missing, so a clean left its binary behind and the next
	# measurement was of a stale one. That cost a real fix, which was
	# reverted because the resulting crash looked like the fix's fault.
	$(MAKE) -C uart clean
	rm -rf obj-ipod6g obj-ipodvideo obj \
	       iap_test-ipod6g iap_test-ipodvideo iap_test \
	       dump_legacy-ipod6g dump_legacy-ipodvideo dump_legacy
