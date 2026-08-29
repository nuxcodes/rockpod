# Host build of the USB HID transport's transmit path.
#
# firmware/usbstack/usb_iap_hid.c is compiled unmodified, with the USB
# stack replaced by recording stubs, so the assertions are about the
# exact bytes it puts on the wire. -Dstatic= exposes iap_hid_tx() and
# iap_hid_active, which are file-static in the real build.

ROOT   := ../../../..
CC     ?= cc

# The transport is shared code, so it has to be exercised against both
# targets' configs. The 6G builds it today; the 5G will as soon as
# USB_ENABLE_IAP_HID is defined for it, and nothing here is
# SoC-specific -- no endpoint numbers, no DesignWare, 64 bytes is the
# full-speed HID maximum on both. Running one config only meant the
# other was assumed rather than checked.
TARGET ?= ipod6g
ifeq ($(TARGET),ipodvideo)
TARGET_DEF := -DIPOD_VIDEO
else
TARGET_DEF := -DIPOD_6G
endif
BIN := hid_test-$(TARGET)

CFLAGS := -std=gnu99 -g -O0 -Wall -Wno-unused-parameter \
          -Dstatic= $(TARGET_DEF) -DMEMORYSIZE=64 -D_FORTIFY_SOURCE=0 \
          -Istubs -I. \
          -I$(ROOT)/build-hw-$(TARGET) \
          -I$(ROOT)/firmware/export \
          -I$(ROOT)/firmware/include \
          -I$(ROOT)/firmware/kernel/include \
          -I$(ROOT)/apps \
          -include stubs/hid_prelude.h

SRCS := hid_test.c hid_stubs.c $(ROOT)/firmware/usbstack/usb_iap_hid.c

.PHONY: all test test-both clean
all: $(BIN)

test: $(BIN)
	./$(BIN)

# Both configs. A failure in either fails the run.
test-both:
	$(MAKE) TARGET=ipod6g test
	$(MAKE) TARGET=ipodvideo test

$(BIN): $(SRCS) hid_stubs.h stubs/hid_prelude.h
	$(CC) $(CFLAGS) -o $@ hid_test.c hid_stubs.c \
	      $(ROOT)/firmware/usbstack/usb_iap_hid.c

clean:
	rm -rf hid_test hid_test-* *.dSYM
