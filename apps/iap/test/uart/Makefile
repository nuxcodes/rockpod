# Host build of the PP dock UART path.
#
# firmware/target/arm/pp/uart-pp.c is compiled unmodified. Its
# fixed-address register macros are redirected at an array by
# stubs/uart_prelude.h, and everything under it -- adc_read(),
# iap_getc(), iap_reset_state(), tick_add_task() -- is recorded.
#
# The cases drive the tick directly rather than through serial_setup().
# That function pokes 0x7000008C and 0x70000018 as raw addresses -- not
# through any register macro, so there is nothing to redirect -- and on
# arm64 macOS __PAGEZERO covers the low 4 GB, so those pages cannot be
# mapped. The tick is the part with logic in it; the pin routing is not.
#
# The headers are shadowed from stubs/ rather than taken from the tree.
# On an arm64 host the real config.h sends rbendian.h down its ARM path
# and the helpers end up defined twice, and thread.h then wants
# asm/thread.h. The hid suite shadows for the same reason.
#
# ipodvideo only: this driver is the iAP transport on the PP iPods, and
# the 6G uses firmware/target/arm/s5l8702/ipod6g/serial-6g.c instead.

ROOT   := ../../../..
TARGET := ipodvideo
CC     ?= cc

# -MMD -MP and the -include below: this Makefile listed two headers as
# prerequisites and there are ten in stubs/, so editing any of the other
# eight left a stale binary behind. Worse, uart_stubs.h defines the
# struct both objects share -- adding a field to it and rebuilding
# produced two translation units disagreeing about its size, which
# segfaults rather than failing, and cost a real fix that was reverted
# because the crash looked like the fix's fault.
CFLAGS := -std=gnu99 -g -O0 -Wall -Wno-unused-parameter -MMD -MP \
          -DIAP_UART_TEST -DIPOD_VIDEO -DMEMORYSIZE=64 -D_FORTIFY_SOURCE=0 \
          -Istubs -I. \
          -include stubs/uart_prelude.h

BIN  := uart_test
DRIVER := $(ROOT)/firmware/target/arm/pp/uart-pp.c
SRCS := uart_test.c uart_stubs.c $(DRIVER)

.PHONY: all test check-no-tick clean
all: $(BIN)

# A source check, because no host harness can reproduce what it guards.
# iap_accessory_poll() calls adc_read(), which takes i2c_lock() ->
# mutex_lock() once its 400 ms cache expires; ASSERT_CPU_MODE in
# firmware/kernel/mutex.c is a real panicf on ARM classic, and tick
# tasks run from the timer IRQ. Registering the poll as a tick task
# therefore panics the device within 400 ms of boot -- which is how it
# shipped once. The stub adc_read() here is a variable, so the harness
# cannot feel the difference; this can.
#
# The 6G does register a tick for the same job and is correct, because
# pmu_accessory_present() returns a cached int and touches no bus. The
# two look identical in source and are not.
check-no-tick:
	@if grep -v '^[[:space:]]*[*/]' $(DRIVER) | grep -q 'tick_add_task('; then 	    echo "uart-pp.c registers a tick task."; 	    echo "adc_read() cannot be called from one: it takes a mutex, and"; 	    echo "the assert in mutex_lock() is a panic on ARM classic."; 	    exit 1; 	fi


# A second source check, for the same reason: the harness has no
# interrupts, so it cannot feel this either.
#
# pp5020.h aliases SER0_RBR, SER0_THR and SER0_DLL to 0x70006000. Which
# one a read returns is decided by the divisor-latch bit in LCR, and
# set_bitrate() sets that bit, writes the divisor and clears it again.
# The Rx ISR's drain loop is "while (LSR & 1) temp = *RBR_THR_DLL" -- a
# read that with the latch enabled returns the divisor and does not pop
# the Rx FIFO. Data Ready stays asserted, the loop never exits, and
# nothing clears the latch again. The device hangs.
#
# An accessory plugged in before boot is already transmitting (MFi
# 2.2.1, p.88), and both iap_setup() and iap_reset_state() on USB
# extract reach serial_bitrate() from thread context, so the window is
# reachable with nothing misbehaving. The 6G has a separate UBRDIV
# register and no window at all.
check-dlab-masked:
	@awk '/^static void set_bitrate/,/^}/' $(DRIVER) | grep -v '^[[:space:]]*[*/]' \
	    | grep -q 'disable_irq_save' || { \
	    echo "set_bitrate() writes the divisor latch with interrupts on."; \
	    echo "SER0_RBR and SER0_DLL are the same address: an Rx interrupt"; \
	    echo "inside that window reads the divisor instead of the FIFO,"; \
	    echo "Data Ready never clears and the ISR spins for ever."; \
	    exit 1; \
	}
	@awk '/^static void set_bitrate/,/^}/' $(DRIVER) | grep -v '^[[:space:]]*[*/]' \
	    | grep -q 'restore_irq' || { \
	    echo "set_bitrate() disables interrupts and does not restore them."; \
	    exit 1; \
	}

test: $(BIN) check-no-tick check-dlab-masked
	./$(BIN)

OBJS := obj/uart_test.o obj/uart_stubs.o obj/uart-pp.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/uart-pp.o: $(DRIVER) | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj:
	mkdir -p obj

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BIN) obj *.dSYM *.d
