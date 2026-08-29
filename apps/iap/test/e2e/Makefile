# The iAP protocol layer talking through the real USB HID transport.
#
# Both are compiled unmodified, the same sources the firmware builds.
# Only the USB stack is replaced, by a recorder, so a test can speak in
# HID reports and check what comes back as HID reports -- which is what
# an accessory on the dock connector actually sees.
#
# Separate from the protocol suite because -Dstatic= is needed to reach
# usb_iap_hid.c's internals, and separate from the transport suite
# because that one stubs the protocol layer this one runs for real.

ROOT   := ../../../..

# Both targets' configs. The transport is shared code and the 5G will
# build it as soon as USB_ENABLE_IAP_HID is defined for ipodvideo;
# running only the 6G config meant the other was assumed.
TARGET ?= ipod6g
ifeq ($(TARGET),ipodvideo)
TARGET_DEF  := -DIPOD_VIDEO
TARGET_INCS := -I$(ROOT)/firmware/target/arm/pp \
	       -I$(ROOT)/firmware/target/arm/ipod
# See the header: PP maps USB_DEVBSS_ATTR onto an .ibss section
# attribute that Mach-O will not take.
TARGET_PRELUDE := -include ../stubs/host_pp_prelude.h
else
TARGET_DEF  := -DIPOD_6G
TARGET_INCS := -I$(ROOT)/firmware/target/arm/s5l8702/ipod6g \
	       -I$(ROOT)/firmware/target/arm/s5l8702
endif

BUILD  := $(ROOT)/build-hw-$(TARGET)
CC     ?= cc

CFLAGS := -std=gnu99 -g -O0 -fno-builtin -Wall -Wundef -MMD -MP \
	  -Wno-unused-parameter -Wno-sign-compare \
	  $(TARGET_DEF) -DMEMORYSIZE=64 -D_FORTIFY_SOURCE=0 \
	  -I. -I.. -I../stubs \
	  -I$(BUILD) \
	  -I$(ROOT)/apps -I$(ROOT)/apps/gui -I$(ROOT)/apps/iap \
	  -I$(ROOT)/apps/recorder \
	  -I$(ROOT)/firmware/export -I$(ROOT)/firmware/include \
	  -I$(ROOT)/firmware/common -I$(ROOT)/firmware/drivers \
	  -I$(ROOT)/firmware/kernel/include \
	  $(TARGET_INCS) \
	  -I$(ROOT)/firmware/target/arm \
	  -I$(ROOT)/lib/rbcodec -I$(ROOT)/lib/rbcodec/metadata \
	  -I$(ROOT)/lib/rbcodec/dsp -I$(ROOT)/lib/skin_parser \
	  -I$(ROOT)/lib/tlsf/src \
	  -include ../stubs/host_prelude.h $(TARGET_PRELUDE)

# -Dstatic= only for the transport, whose iap_hid_tx() and
# iap_hid_process_rx() are file-static. Applying it to firmware/sound.c
# too would externalise the per-setting conversion functions its macros
# generate, and they have no out-of-line definitions.
HID_SRC  := $(ROOT)/firmware/usbstack/usb_iap_hid.c

SRCS := e2e_test.c e2e_stubs.c ../rb_stubs.c \
	$(ROOT)/firmware/sound.c \
	$(ROOT)/apps/iap/iap-core.c \
	$(ROOT)/apps/iap/iap-artwork.c \
	$(ROOT)/apps/iap/iap-lingo0.c \
	$(ROOT)/apps/iap/iap-lingo1.c \
	$(ROOT)/apps/iap/iap-lingo2.c \
	$(ROOT)/apps/iap/iap-lingo3.c \
	$(ROOT)/apps/iap/iap-lingo4.c

# CONFIG_TUNER is ipodvideo-only, so the RF Tuner lingo and its driver
# are part of that build and not the 6G's.
ifeq ($(TARGET),ipodvideo)
SRCS += $(ROOT)/apps/iap/iap-lingo7.c
# Built separately with -Dstatic=, as the protocol suite does: rb_stubs.c
# resets this driver's file-local state between cases, and without the
# flag those names are not linkable. Scoped to this one file because it
# externalises file-local names and doing that to firmware/sound.c
# collides with the generated converters.
TUNER_OBJ := ipod_remote_tuner-$(TARGET).o
endif

BIN := e2e_test-$(TARGET)
HID_OBJ := usb_iap_hid-$(TARGET).o

.PHONY: all test test-both clean
all: $(BIN)

test: $(BIN)
	./$(BIN)

# Both configs. A failure in either fails the run.
test-both:
	$(MAKE) TARGET=ipod6g test
	$(MAKE) TARGET=ipodvideo test

$(HID_OBJ): $(HID_SRC) e2e.h
	$(CC) $(CFLAGS) -Dstatic= -c $(HID_SRC) -o $@

$(BIN): $(SRCS) $(HID_OBJ) $(TUNER_OBJ) e2e.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(HID_OBJ) $(TUNER_OBJ)

ipod_remote_tuner-$(TARGET).o: $(ROOT)/firmware/drivers/tuner/ipod_remote_tuner.c
	$(CC) $(CFLAGS) -Dstatic= -c $< -o $@

clean:
	rm -rf e2e_test e2e_test-* *.dSYM *.o *.d
